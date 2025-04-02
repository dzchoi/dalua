#include <assert.h>             // for assert()
#include <dirent.h>             // for opendir(), readdir(), closedir()
#include <errno.h>              // for errno
#include <fcntl.h>              // for open(), O_RDWR, O_NOCTTY
#include <poll.h>               // for poll()
#include <readline/readline.h>  // for Readline library
#include <readline/history.h>   // for add_history()
#include <stdio.h>              // for fprintf(), printf(), putc()
#include <string.h>             // for strerror()
#include <sys/file.h>           // for flock()
#include <termios.h>            // for tcgetattr(), tcsetattr()
#include <time.h>               // for clock()
#include <unistd.h>             // for usleep(), read(), write(), close(), STDIN_FILENO

// Override the lua_writestringerror() definition in the lauxlib.h below.
#define lua_writestringerror(format, ...) \
    fprintf(stderr, format, __VA_ARGS__)

#include "dalua.h"
#include "lua.h"
#include "lauxlib.h"
// #include "lualib.h"

#define likely(x) __builtin_expect((x), true)
#define unlikely(x) __builtin_expect((x), false)

// Compile-time strlen(s) for a literal string s.
#define LEN(s)  (sizeof(s) - 1)



// fds[0].fd will be set to the FD of the serial port.
// #define serial_fd  (fds[0].fd)

static struct pollfd fds[] = {
    { -1, POLLIN, 0 },
    { STDIN_FILENO, POLLIN, 0 }
};

// This buffer temporarily holds input from the serial port for display purposes.
// Overflowing input is handled seamlessly, only requiring the display routine to run
// more frequently. Note that the internal buffer size for serial ports on Linux is
// typically 4 KB, which retains older input data briefly.
#define MAX_SERIAL_INPUT 128  // Must be >= 4.
static char serial_buffer[MAX_SERIAL_INPUT];



// Resolve DEVICE_PATH if not provided via the command line.
static bool resolve_device_path(void)
{
    if ( DEVICE_PATH != NULL )
        return true;

    static const char DEVICE_DIR[] = "/dev/serial/by-id/";

    DIR* pdir = opendir(DEVICE_DIR);
    if ( pdir == NULL ) {
        l_message("%s: %s", strerror(errno), DEVICE_DIR);
        return false;
    }

    struct dirent* pdirent = NULL;
    static char device_path[sizeof(pdirent->d_name) + LEN(DEVICE_DIR)];
    __builtin_strcpy(device_path, DEVICE_DIR);

    bool found = false;
    while ( (pdirent = readdir(pdir)) != NULL ) {
        if ( __builtin_strstr(pdirent->d_name, DROP_ALT) == NULL )
            continue;

        if ( found ) {
            found = false;
            l_message("multiple devices found for \"%s\"", DROP_ALT);
            break;
        }

        found = true;
        __builtin_strcat(device_path, pdirent->d_name);
    }

    if ( found )
        DEVICE_PATH = device_path;
    else if ( pdirent == NULL )
        l_message("no device found for \"%s\"", DROP_ALT);

    closedir(pdir);
    return found;
}

// Try to receive the Lua status remotely, which indicates the remote Lua REPL is ready.
// Return the status on success, or LUA_ERRIO on a timeout or a serial port error (e.g.
// serial connection lost). Note that when LUA_ERRIO is returned, close_serial() must be
// called explicitly. Using -1 for timeout_ms will result in waiting indefinitely.
static status_t receive_status(bool verbose, int timeout_ms)
{
    status_t status = LUA_ERRIO;
    static const clock_t CLOCKS_PER_MS = CLOCKS_PER_SEC / 1000;
    clock_t since = clock();

    for (;;) {
        if ( timeout_ms >= 0 ) {
            clock_t now = clock();
            int elapsed_ms = (now - since) / CLOCKS_PER_MS;
            if ( timeout_ms <= elapsed_ms )
                break;  // status == LUA_ERRIO
            timeout_ms -= elapsed_ms;  // timeout_ms > 0!
            since = now;
        }

        // Wait for an input from the serial port.
        // Todo: Replace poll() with select() for Mingw compatibility.
        int result = poll(fds, 1, timeout_ms);
        assert( result >= 0 );

        // Return false for a timeout or a serial port error.
        if ( result == 0 || (fds[0].revents & POLLERR) )
            break;  // status == LUA_ERRIO

        // In canonical mode, read() reads a string up to the ending '\n'.
        int len = read(fds[0].fd, serial_buffer, sizeof(serial_buffer));
        assert( len >= 0 );

        // Got the 4 byte response "[}?\n". Return the status at "?".
        if ( (size_t)len == sizeof(PING(0))
          && serial_buffer[0] == (char)PING(0)              // '['
          && serial_buffer[1] == (char)(PING(0) >> 8)       // '}'
          && serial_buffer[3] == (char)(PING(0) >> 24) ) {  // '\n'
            status = (status_t)(serial_buffer[2] - '0');
            break;  // Do not display the response.
        }

        // Display any other messages from the serial port while waiting.
        if ( verbose )
            lua_writestring(serial_buffer, len);
    }

    return status;
}

static inline void close_serial(void)
{
    close(fds[0].fd);
    fds[0].fd = -1;
}

// Establish a serial connection through the appropriate serial port, returning the
// connection status (LUA_OK or LUA_ERRFATAL). If LOG_MASK is set, log messages from
// threads matching the mask are displayed while establishing the connection. On success,
// fds[0].fd stores the file descriptor of the serial port; otherwise, it is set to -1.
static status_t setup_serial(void)
{
    if ( fds[0].fd != -1 )
        return LUA_OK;

    if ( !resolve_device_path() )  // Resolve DEVICE_PATH.
        return LUA_ERRFATAL;

    while ( (fds[0].fd = open(DEVICE_PATH, O_RDWR | O_NOCTTY)) == -1 ) {
        if ( unlikely(NO_RECONNECT) ) {
            l_message("%s: %s", strerror(errno), DEVICE_PATH);
            return LUA_ERRFATAL;
        }
        usleep(RECONNECT_PERIOD_MS * 1000);  // to microseconds
    }

    if ( flock(fds[0].fd, LOCK_EX | LOCK_NB) == 0 ) {
        struct termios orig_ios;
        if ( tcgetattr(fds[0].fd, &orig_ios) == 0 ) {
            // Configure the serial port for canonical input mode (i.e. line-buffered
            // input mode) with all ECHOs disabled, which is to be done before the
            // initial read(). In this mode, input is provided line by line, where a line
            // becomes available upon receiving a line delimiter (NL, EOL, EOL2, or EOF
            // if received at the start of the line).
            // See Canonical Input Processing:
            // - https://tldp.org/HOWTO/Serial-Programming-HOWTO/x115.html
            // - https://manual.cs50.io/3/cfmakeraw

            struct termios ios = orig_ios;
            // ios.c_iflag &= ~(IXON | IXOFF | IXANY);  // Disable software flow control.
            ios.c_iflag = 0;       // Disable all input processing; e.g. XON/XOFF/ICRNL.
            ios.c_oflag = 0;       // Disable all output processing
            ios.c_lflag = ICANON;  // Enable canonical mode and disable all ECHOs.
            tcsetattr(fds[0].fd, TCSANOW, &ios);

            const uint32_t ping = PING(LOG_MASK);
            const bool display_logs = LOG_MASK & ~0x01;
            const bool display_welcome = LOG_MASK & 0x01;
            if ( write(fds[0].fd, &ping, sizeof(ping)) == sizeof(ping) ) {
                if ( receive_status(display_logs, RESPONSE_TIMEOUT_MS) == LUA_OK ) {
                    if ( display_welcome )
                        printf("Connected to %s\n", DEVICE_PATH);
                    return LUA_OK;
                }
            }

            // Restore the terminal attributes if it is not our serial port.
            tcsetattr(fds[0].fd, TCSANOW, &orig_ios);
            l_message("no Lua running on %s", DEVICE_PATH);
        } else
            l_message("%s: %s", strerror(errno), DEVICE_PATH);
    } else
        l_message("locked by another process: %s", DEVICE_PATH);

    close_serial();
    return LUA_ERRFATAL;
}



// Either read_error or read_line can be non-NULL, but not both.
static const char* read_error = NULL;
static const char* read_line = NULL;
static status_t pushline_status = LUA_OK;

static void _cb_linehandler(char* line)
{
    if ( line == NULL ) {  // Indicates EOF.
        // Note that Readline interprets ^D as EOF only when it is at the start of a
        // line; otherwise, it removes the character at the cursor position.
        read_error = "Bye!";
        pushline_status = LUA_ERRFATAL;
    }

    // The read_line variable is set only when a non-blank line is entered (rl_end
    // indicates the length of the current input line stored in Readline's buffer). This
    // ensures that blank lines remain within the pushline() function's while-loop,
    // prompting the reading of the next line.
    else if ( rl_end )
        read_line = line;
}

// Read a line from stdin using the Readline library. If successful, push the line onto
// the Lua stack; otherwise, push an error message (e.g., when EOF is encountered). The
// returned line excludes the trailing newline, leaving only the plain text.
// ( -- line | )
static status_t pushline(lua_State* L, bool firstline)
{
    // We do not use the Lua global variable "_PROMPT" to show the prompt, as the
    // interpreter runs remotely.
    rl_callback_handler_install((firstline ? LUA_PROMPT : LUA_PROMPT2), _cb_linehandler);

    read_error = NULL;
    read_line = NULL;
    pushline_status = LUA_OK;
    bool is_reading_serial = false;

    while ( read_error == NULL && read_line == NULL ) {
        // Wait for an input either from the serial port or from stdin.
        int result = poll(fds, 2, -1);  // -1 to wait indefinitely.
        assert( result >= 0 );

        if ( fds[0].revents & POLLERR ) {
            read_error = "serial connection lost";
            pushline_status = LUA_ERRIO;
            break;
        }

        // Display input from the serial port when not actively typing.
        if ( (fds[0].revents & POLLIN)
          && (rl_end == 0 || unlikely(is_reading_serial)) ) {
            if ( !is_reading_serial )
                // Make the following lua_writestring() overwrite the prompt.
                putc('\r', stdout);

            int len = read(fds[0].fd, serial_buffer, sizeof(serial_buffer));
            assert( len >= 0 );
            lua_writestring(serial_buffer, len);
            // In canonical mode, the last chunk of the serial message is expected to end
            // with '\n'
            is_reading_serial = (serial_buffer[len - 1] != '\n');

            if ( likely(!is_reading_serial) ) {
                rl_on_new_line();
                rl_redisplay();  // Show the prompt again.
            }
        }

        // Read input from stdin and feed it to Readline.
        else if ( fds[1].revents & POLLIN ) {
            assert( !is_reading_serial );  // It should not happen in canonical mode.
            // This will read a character from stdin, updating Readline's buffer and
            // triggering the line handler on newline.
            rl_callback_read_char();
        }
    }

    if ( read_line )
        lua_pushstring(L, read_line);

    // Erase the prompt and free the memory for read_line.
    rl_callback_handler_remove();

    if ( read_error ) {
        l_message(read_error);
        close_serial();
    }

    return pushline_status;
}

// Try to compile a line on top of the stack as "return <line>;". Push the the compiled
// chunk if successful, or a compile-error message.
// ( line -- line chunk | line error )
static status_t compile_expression(lua_State* L, const char* chunkname)
{
    const char* line = lua_tostring(L, -1);  // Original line.
    const char* s = lua_pushfstring(L, "return %s;", line);

    status_t status = luaL_loadbuffer(L, s, __builtin_strlen(s), chunkname);
    lua_remove(L, -2);  // Remove the modified line.
    if ( status == LUA_OK ) {
        if ( line[0] != '\0' )  // Non-empty?
            add_history(line);  // Keep it in the Readline history.
    }

    return status;
}

// Check if the given status indicates syntax error and the error message at the top
// of the stack ends with the "<eof>" mark for incomplete statements.
// ( x -- | x )
static bool incomplete(lua_State* L, status_t status)
{
    static const char EOFMARK[] = "<eof>";

    if ( status == LUA_ERRSYNTAX ) {
        size_t msg_l;
        const char* msg = lua_tolstring(L, -1, &msg_l);
        if ( msg_l >= LEN(EOFMARK)
          && __builtin_strcmp(msg + msg_l - LEN(EOFMARK), EOFMARK) == 0 ) {
            lua_pop(L, 1);
            return true;
        }
    }
    return false;
}

// Try to compile the line on top of the stack as a statement, possibly combining more
// lines from stdin to make it a complete statement. Push the the compiled chunk if
// successful, or a compile-error message.
// ( line -- line chunk | line error | line )
static status_t compile_more_lines(lua_State* L)
{
    for (;;) {  // Repeat until gets a complete statement.
        size_t line_l;
        const char* line = lua_tolstring(L, -1, &line_l);

        // Try to compile the (combined) line as a statement.
        status_t status = luaL_loadbuffer(L, line, line_l, "=stdin");

        // If it compiled successfully, return ( -- line chunk ) with status = LUA_OK.
        // Otherwise, if it fails and no further lines are needed, return
        // ( -- line error ) with the compile error as the status.
        if ( !incomplete(L, status) ) {
            add_history(line);  // Keep it in the Readline history.
            return status;
        }

        // Read an additional line. If it fails, return ( -- line ).
        status = pushline(L, false);
        if ( status != LUA_OK )
            return status;

        // ( line1 line2 -- line )
        lua_pushliteral(L, "\n");  // Add newline
        lua_insert(L, -2);         // between the two lines.
        lua_concat(L, 3);          // Join them.
    }
}

static status_t _writer(lua_State*, const void* pdata, size_t sz, void*)
{
    if ( write(fds[0].fd, pdata, sz) == (ssize_t)sz )
        return LUA_OK;

    l_message("%s: %s", strerror(errno), DEVICE_PATH);
    close_serial();
    return LUA_ERRIO;
}

// If the status is LUA_OK, execute the chunk on the stack remotely and return the
// remote status. Otherwise, display the error message on the stack.
// ( chunk | error | -- )
static status_t do_chunk(lua_State* L, status_t status)
{
    if ( status == LUA_OK ) {
        // If lua_dump() fails, it returns the non-zero status from _writer() and
        // leaves the chunk on the stack unchanged.
        status = lua_dump(L, _writer, NULL, 0);  // 0 == strip disabled
        lua_pop(L, 1);  // Pop the compiled chunk.
    }

    if ( status == LUA_OK ) {
        status = receive_status(true, -1);
        // For statuses remotely received, the stack does not contain an error message,
        // as any error would have already been displayed remotely. So, we only pass the
        // received status here.
        if ( status < LUA_OK ) {  // i.e. status == LUA_ERRIO
            l_message("serial connection lost");
            close_serial();
        }
    }

    else if ( status == LUA_ERRSYNTAX ) {
        // lua_writestringerror() is used instead of l_message() to avoid including the
        // program name to the error message, since the syntax error message already
        // contains the chunk name. Prepend "> " instead to distinguish it from remote
        // errors.
        lua_writestringerror("%s%s\n", LUA_PROMPT, lua_tostring(L, -1));
        lua_pop(L, 1);  // Pop the error message.
    }

    // Negative error status does not have an associated error message on the stack.
    else if ( status > LUA_OK ) {
        l_message(lua_tostring(L, -1));
        lua_pop(L, 1);  // Pop the error message.
    }

    // Sanity check
    assert( lua_gettop(L) == 0 );
    return status;
}



// We execute lua_* functions here in unprotected mode, since we are only using stack
// manipulation functions, luaL_loadbuffer() and lua_dump(), which basically do not
// throw an exception other than not enough memory.

// ( -- )
status_t do_string(lua_State* L, const char* s)
{
    status_t status = setup_serial();
    if ( status == LUA_OK ) {
        lua_pushstring(L, s);
        status = compile_expression(L, "=inline");
        if ( status != LUA_OK ) {
            lua_pop(L, 1);  // Pop the error message.
            status = luaL_loadbuffer(L, s, __builtin_strlen(s), "=inline");
        }
        lua_remove(L, -2);  // Remove the line.
    }

    return do_chunk(L, status);
}

// ( -- )
status_t do_file(lua_State* L, const char* filename)
{
    status_t status = setup_serial();
    if ( status == LUA_OK )
        status = luaL_loadfile(L, filename);

    return do_chunk(L, status);
}

// ( -- )
status_t do_repl(lua_State* L)
{
    status_t status;
    do {
        status = setup_serial();
        if ( status == LUA_OK ) {
            assert( lua_gettop(L) == 0 );
            status = pushline(L, true);
            if ( status == LUA_OK ) {
                status = compile_expression(L, "=stdin");  // Try as "return ...".
                if ( status != LUA_OK ) {
                    lua_pop(L, 1);  // Pop the error message.
                    // Try as command, possibly reading in more lines from stdin.
                    status = compile_more_lines(L);
                }
                lua_remove(L, 1);  // Remove the line from the stack.
            }
        }

        status = do_chunk(L, status);
    } while ( status != LUA_ERRFATAL );

    return status;
}
