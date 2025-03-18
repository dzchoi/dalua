#include <assert.h>             // for assert()
#include <dirent.h>             // for readdir()
#include <errno.h>              // for errno
#include <fcntl.h>              // for open(), O_RDWR, O_NOCTTY
#include <poll.h>               // for poll()
#include <readline/readline.h>  // for readline()
#include <readline/history.h>   // for add_history()
#include <stdbool.h>            // for bool
#include <stdlib.h>             // for free()
#include <stdint.h>             // for uint16_t
#include <string.h>             // for strerror(), strncmp(), strcmp()
#include <termios.h>            // for tcsetattr() and tcgetattr()
#include <unistd.h>             // for write(), STDOUT_FILENO

// Override the lua_writestringerror() definition in the lauxlib.h below.
#define lua_writestringerror(format, ...) \
    fprintf(stderr, format, __VA_ARGS__)

#include "dalua.h"
#include "lprefix.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"



static const char DEVICE_DIR[] = "/dev/";
static const char TTYACM[] = "ttyACM";

static const char LUA_PROMPT[] = "> ";
static const char LUA_PROMPT2[] = "+ ";

// Compile-time strlen(s) when s points to a literal string.
#define LEN(s)  (sizeof(s)/sizeof((s)[0]) - 1)



// fds[0].fd will be set to the FD of the serial port.
#define serial_fd  (fds[0].fd)

static struct pollfd fds[] = {
    { -1, POLLIN, 0 },
    { STDIN_FILENO, POLLIN, 0 }
};

#define MAX_SERIAL_INPUT 1024
static char serial_buffer[MAX_SERIAL_INPUT];

// Try to receive Lua status remotely, which indicates the remote Lua REPL is ready.
// Return the received status if successful, or LUA_ERRIO if it times out or if there
// is an error in reading from the serial port (e.g. serial connection lost).
// Using -1 for timeout_ms will wait indefinitely.
static status_t receive_status(bool verbose, int timeout_ms)
{
    for (;;) {
        // Wait for an input from the serial port.
        int result = poll(fds, 1, timeout_ms);
        assert( result >= 0 );

        // Return false if it times out or if there is an error in the serial port.
        if ( result == 0 || (fds[0].revents & POLLERR) )
            return LUA_ERRIO;

        // As in cananical mode, read() receives a complete line that ends with '\n'.
        result = read(fds[0].fd, serial_buffer, sizeof(serial_buffer));
        assert( result >= 0 );

        // Got the 4 byte response "[}?\n". Extract the status at "?".
        if ( result == 4 && *(uint16_t*)serial_buffer == '[' + ('}' << 8) )
            return serial_buffer[2] - '0';

        // Print out any incoming messages from the serial port while waiting.
        if ( verbose )
            lua_writestring(serial_buffer, result);
    }
}

static struct termios orig_serial_io_state;

status_t setup_serial(bool verbose)
// It stores the file descriptor of the serial port in fds[0].fd, if the connection is
// successful.
{
    DIR* pdir = opendir(DEVICE_DIR);
    if ( pdir == NULL ) {
        l_message("%s: %s", strerror(errno), DEVICE_DIR);
        return LUA_ERRIO;
    }

    struct dirent* pdirent = NULL;
    char device_path[sizeof(pdirent->d_name)/sizeof(char) + LEN(DEVICE_DIR)];
    __builtin_strcpy(device_path, DEVICE_DIR);

    while ( (pdirent = readdir(pdir)) != NULL ) {
        if ( strncmp(pdirent->d_name, TTYACM, LEN(TTYACM)) != 0 )
            continue;

        __builtin_strcpy(device_path + LEN(DEVICE_DIR), pdirent->d_name);
        fds[0].fd = open(device_path, O_RDWR | O_NOCTTY);

        if ( tcgetattr(fds[0].fd, &orig_serial_io_state) == 0 ) {
            // Set up the serial port for canonical input mode with all ECHOs
            // disabled, which should come before the first read(). In canonical
            // input mode, input is made available line by line. An input line is
            // available when one of the line delimiters is typed (NL, EOL, EOL2; or
            // EOF at the start of line).
            // See Canonical Input Processing:
            //  https://tldp.org/HOWTO/Serial-Programming-HOWTO/x115.html
            //  https://manual.cs50.io/3/cfmakeraw

            struct termios ios = orig_serial_io_state;
            // ios.c_cflag = B1152000 | CS8 | CLOCAL | CREAD;
            // ios.c_iflag &= ~(IXON | IXOFF | IXANY);  // Disable software flow control.
            ios.c_iflag = 0;  // Disable software flow control and spcial characters.
            ios.c_oflag = 0;  // raw output
            ios.c_lflag = ICANON;  // Enable canonical input mode and disable all ECHOs.
            tcsetattr(fds[0].fd, TCSANOW, &ios);

            if ( receive_status(verbose, 100) == LUA_OK ) {  // 100 ms
                if ( verbose )
                    printf("via %s\n", device_path);
                break;
            }

            // Restore the attributes if it is not our serial port.
            tcsetattr(fds[0].fd, TCSANOW, &orig_serial_io_state);
            close(fds[0].fd);
            fds[0].fd = -1;
        }
    }
    closedir(pdir);

    if ( pdirent == NULL ) {
        l_message("serial port not found");
        return LUA_ERRIO;
    }
    return LUA_OK;
}



// Either read_error or read_line can be non-NULL, but not both.
static const char* read_error = NULL;
static char* read_line;

// Note that the readline library treats ^D as EOF only at the beginning of a line,
// otherwise deletes the character under cursor.
static void _cb_linehandler(char* line)
{
    // NULL line indicates EOF.
    if ( line == NULL )
        read_error = "Bye!";

    // If it is a blank line we stay in pushline() and read next line.
    else if ( rl_end )
        read_line = line;
}

// Read a line from stdin using the readline library. Push the line onto the Lua
// stack if successful, or an error message otherwise (e.g. EOF is hit). The line
// returned has the trailing newline removed, so only the text remains.
// ( -- line | )
static status_t pushline(lua_State* L, bool firstline)
{
    // We do not use the Lua global variable "_PROMPT" to show the prompt, as the
    // interpreter runs remotely.
    rl_callback_handler_install((firstline ? LUA_PROMPT : LUA_PROMPT2), _cb_linehandler);

    read_line = NULL;

    // Once read_error is set to true, we keep returning the same error.
    while ( read_error == NULL && read_line == NULL ) {
        // Wait for an input either from the serial port or from stdin.
        int result = poll(fds, 2, -1);  // -1 to wait indefinitely.
        assert( result >= 0 );

        if ( fds[0].revents & POLLERR ) {
            read_error = "serial connection lost";
            break;
        }

        // If a complete line is read from the serial port, show it on stdout.
        if ( rl_end == 0 && (fds[0].revents & POLLIN) ) {
            result = read(fds[0].fd, serial_buffer, sizeof(serial_buffer));
            assert( result >= 0 );

            rl_clear_visible_line();  // Erase the prompt.
            lua_writestring(serial_buffer, result);
            rl_reset_line_state();
            rl_redisplay();           // Show the prompt again.
        }

        if ( fds[1].revents & POLLIN )
            rl_callback_read_char();  // It will call _cb_linehandler() on a newline.
    }

    rl_clear_visible_line();  // Erase the prompt.
    rl_callback_handler_remove();

    if ( read_error ) {
        l_message(read_error);
        return LUA_ERRIO;  // Reading error
    }

    lua_pushstring(L, read_line);
    free(read_line);  // Free the memory allocated for read_line.
    return LUA_OK;
}

// Try to compile a line on top of the stack as "return <line>;". Push the the
// compiled chunk if successful, or a compile-error message.
// ( line -- line chunk | line error )
static status_t compile_expression(lua_State* L, const char* chunkname)
{
    const char* line = lua_tostring(L, -1);  // Original line.
    const char* s = lua_pushfstring(L, "return %s;", line);

    status_t status = luaL_loadbuffer(L, s, __builtin_strlen(s), chunkname);
    lua_remove(L, -2);  // Remove the modified line.
    if ( status == LUA_OK ) {
        if ( line[0] != '\0' )  // Non-empty?
            add_history(line);  // Keep it in the readline history.
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
          && strcmp(msg + msg_l - LEN(EOFMARK), EOFMARK) == 0 ) {
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
        // Otherwise, if it fails with no need for additional lines, return
        // ( -- line error ) with the compile-error as the status.
        if ( !incomplete(L, status) ) {
            add_history(line);  // Keep it in the readline history.
            return status;
        }

        // Read in an additional line. If it fails, return ( -- line ).
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

    l_message("cannot write to the serial port");
    return LUA_ERRIO;
}

// If the status is LUA_OK, execute the chunk on the stack remotely and return the
// remote status. Otherwise, print the error message on the stack.
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
        // For the status received remotely, there is no associated error message on
        // the stack. We only pass the received status.
        status = receive_status(true, -1);
    }
    else if ( status == LUA_ERRSYNTAX ) {
        // Do not prepend the program name to the error message, since the syntax
        // error message already contains the chunk name.
        lua_writestringerror("%s\n", lua_tostring(L, -1));
        lua_pop(L, 1);  // Pop the error message.
    }
    // LUA_ERRIO does not have the associated error message on the stack.
    else if ( status != LUA_ERRIO ) {
        l_message(lua_tostring(L, -1));
        lua_pop(L, 1);  // Pop the error message.
    }

    // Sanity check
    assert( lua_gettop(L) == 0 );
    return status;
}

// ( -- )
status_t do_string(lua_State* L, const char* s)
{
    lua_pushstring(L, s);
    status_t status = compile_expression(L, "=script");
    if ( status != LUA_OK ) {
        lua_pop(L, 1);  // Pop the error message.
        status = luaL_loadbuffer(L, s, __builtin_strlen(s), "=script");
    }

    lua_remove(L, -2);  // Remove the line.
    return do_chunk(L, status);
}

// If filename is NULL, it loads from the stdin.
// ( -- )
status_t do_file(lua_State* L, const char* filename)
{
    return do_chunk(L, luaL_loadfile(L, filename));
}

// ( -- )
status_t do_repl(lua_State* L)
{
    status_t status;
    do {
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
    } while ( do_chunk(L, status) != LUA_ERRIO );

    return LUA_OK;  // Return LUA_OK always.
}
