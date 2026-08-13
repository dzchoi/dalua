#include <cassert>
#include <cstdlib>              // for free()
#include <iostream>             // for std::cout, std::cerr
#include <readline/readline.h>  // for Readline library
#include <readline/history.h>   // for add_history()
#include <unistd.h>             // for STDIN_FILENO
#ifdef _WIN32
#include <conio.h>              // for _kbhit()
#include "serial_win.hpp"       // for serial::obj()
#else
#include <poll.h>               // for poll()
#include "serial_linux.hpp"     // for serial::obj()
#endif

#include "darepl.hpp"
#include "lua.hpp"              // for lua_*(), luaL_*()
#include "option.hpp"           // for l_error()



namespace lua {

static status_t read_status = LUA_OK;

#ifdef _WIN32
// Show or hide the console cursor, issuing a Win32 call only when the state actually
// changes.
static void show_cursor(bool show)
{
    static bool shown = true;
    if ( show == shown )
        return;

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO ci;
    if ( GetConsoleCursorInfo(h, &ci) ) {  // Skipped when output is not a console.
        ci.bVisible = show;
        SetConsoleCursorInfo(h, &ci);
        shown = show;
    }
}

// Callback that readline() invokes periodically while it waits for input (installed as
// rl_event_hook). It displays any serial output that has arrived and always returns 0
// (readline() ignores the value); on a lost connection it sets rl_done = 1 to make
// readline() stop reading the line.
//
// readline() owns all console reads here; this hook never touches the console buffer
// itself. _kbhit() is only used to detect a pending keystroke so the hook yields back
// to readline() to consume it. Reading the console directly instead (e.g.
// ReadConsoleInput) would race with Readline's own getc and leave keystrokes such as
// Enter stranded.
int check_serial()
{
    bool disconnected = false;
    while ( true ) {
        // Do not consume serial output while Readline has text to preserve or a pending
        // keystroke to process.
        if ( rl_end != 0 || _kbhit() > 0 )
            break;

        // read_line(0) drains any bytes already retained in m_buffer and any bytes in
        // the driver queue, but never waits for a partial line.
        int len = serial::obj().read_line(0);
        if ( len < 0 ) {
            disconnected = true;
            break;
        }

        // Yield to readline() when there is no complete line to show.
        if ( len == 0 )
            break;

        // About to repaint serial output: hide the cursor. The cursor belongs at the
        // prompt (idle or while the user types), but during repaints Win32 would
        // otherwise flicker it between the prompt and column 0 (the Linux TTY coalesces
        // such updates, so it never flickers there).
        show_cursor(false);
        std::cout << '\r';  // Erase the prompt.
        serial::obj().print_line(len);
        rl_on_new_line();
        rl_redisplay();  // Show the prompt again.
    }

    show_cursor(true);
    if ( disconnected ) {
        rl_replace_line("serial connection lost\n", 0);
        read_status = LUA_ERRIO;
        rl_done = 1;  // Exit readline().
    }
    return 0;
}

// Read a line from the console using readline() while concurrently showing any output
// arriving on the serial port (serviced by check_serial above). Push the line, or an
// error message on EOF or a lost connection.
// ( -- line | )
static status_t push_line(lua_State* L, bool firstline)
{
    // Enable the asynchronous serial check while readline() blocks for input.
    [[maybe_unused]] static bool _ = (rl_event_hook = check_serial);

    read_status = LUA_OK;
    char* line = readline(firstline ? repl::LUA_PROMPT : repl::LUA_PROMPT2);
    if ( likely(line != nullptr) ) {
        if ( read_status == LUA_OK )
            lua_pushstring(L, line);
        else
            l_error() << line;  // check_serial() left the error in the line buffer.
    }
    else {  // Hit EOF.
        rl_clear_visible_line();  // Erase the prompt.
        l_error() << "Bye!\n";
        read_status = LUA_ERRFATAL;
    }

    free(line);
    return read_status;
}
#else
// Either read_error or read_line can be non-NULL, but not both.
static const char* read_error = nullptr;
static const char* read_line = nullptr;

static void _cb_linehandler(char* line)
{
    if ( line == nullptr ) {  // Hit EOF.
        // Note that Readline interprets ^D as EOF only when it is at the start of a
        // line; otherwise, it removes the character at the cursor position.
        read_error = "Bye!\n";
        read_status = LUA_ERRFATAL;
    }

    // The read_line variable is set only when a non-blank line is entered (rl_end
    // indicates the length of the current input line stored in Readline's buffer). This
    // ensures that blank lines remain within the pushline() function's while-loop,
    // prompting the reading of the next line.
    else if ( rl_end )
        read_line = line;
}

static status_t push_line(lua_State* L, bool firstline)
{
    // We do not use the Lua global variable "_PROMPT" to show the prompt, as the
    // interpreter runs remotely.
    rl_callback_handler_install(
        (firstline ? repl::LUA_PROMPT : repl::LUA_PROMPT2), _cb_linehandler);

    pollfd fds[] = {
        { serial::obj().fd(), POLLIN, 0 },
        { STDIN_FILENO, POLLIN, 0 }
    };

    read_error = nullptr;
    read_line = nullptr;
    read_status = LUA_OK;

    do {
        // Wait for an input either from the serial port or from stdin.
        int result = poll(fds, 2, -1);  // -1 waits indefinitely.
        assert( result > 0 );

        if ( unlikely(fds[0].revents & POLLERR) ) {
            read_error = "serial connection lost\n";
            read_status = LUA_ERRIO;
            serial::obj().close();
            break;
        }

        // Display input from the serial port when not actively typing.
        if ( (fds[0].revents & POLLIN) && rl_end == 0 ) {
            std::cout << '\r';  // Erase the prompt.
            if ( unlikely(!serial::obj().print_line()) ) {
                read_error = "serial connection lost\n";
                read_status = LUA_ERRIO;
                break;
            }
            rl_on_new_line();
            rl_redisplay();  // Show the prompt again.
        }

        // Read input from stdin and feed it to Readline.
        else if ( fds[1].revents & POLLIN ) {
            // This will read a character from stdin, updating Readline's buffer and
            // triggering the line handler on newline.
            rl_callback_read_char();
        }
    } while ( read_error == nullptr && read_line == nullptr );

    if ( read_line )
        lua_pushstring(L, read_line);

    // Erase the prompt and free the memory for read_line.
    rl_callback_handler_remove();

    if ( read_error )
        l_error() << read_error;

    return read_status;
}
#endif

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
    constexpr char EOFMARK[] = "<eof>";

    if ( status == LUA_ERRSYNTAX ) {
        size_t msg_l;
        const char* msg = lua_tolstring(L, -1, &msg_l);
        if ( msg_l >= __builtin_strlen(EOFMARK)
          && __builtin_strcmp(msg + msg_l - __builtin_strlen(EOFMARK), EOFMARK) == 0 ) {
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
        status = push_line(L, false);
        if ( status != LUA_OK )
            return status;

        // ( line1 line2 -- line )
        lua_pushliteral(L, "\n");  // Add newline
        lua_insert(L, -2);         // between the two lines.
        lua_concat(L, 3);          // Join them.
    }
}



// We execute lua_* functions here in unprotected mode, since we are only using stack
// manipulation functions, luaL_loadbuffer() and lua_dump(), which basically do not
// throw an exception other than not enough memory.

// ( chunk | error | -- )
status_t repl::do_chunk(lua_State* L, status_t status)
{
    if ( status == LUA_OK ) {
        // If lua_dump() fails, it returns the non-zero status from _writer() and
        // leaves the chunk on the stack unchanged.
        status = lua_dump(L, serial::obj().writer(), &serial::obj(), 0);  // 0 == no strip
        lua_pop(L, 1);  // Pop the compiled chunk.
    }

    if ( status == LUA_OK ) {
        status = serial::obj().receive_status(true, -1);
        // For statuses remotely received, the stack does not contain an error message,
        // as any error would have already been displayed remotely. So, we only pass the
        // received status here.
        if ( status < LUA_OK ) {  // i.e. status == LUA_ERRIO
            serial::obj().close();
            l_error() << "no response from serial port\n";
        }
    }

    else if ( status == LUA_ERRSYNTAX ) {
        // lua_writestringerror() is used instead of l_message() to avoid including the
        // program name to the error message, since the syntax error message already
        // contains the chunk name. Prepend "> " instead to distinguish it from remote
        // errors.
        std::cerr << LUA_PROMPT << lua_tostring(L, -1) << '\n';
        lua_pop(L, 1);  // Pop the error message.
    }

    // Negative error status does not have an associated error message on the stack.
    else if ( status > LUA_OK ) {
        l_error() << lua_tostring(L, -1) << '\n';
        lua_pop(L, 1);  // Pop the error message.
    }

    // Sanity check
    assert( lua_gettop(L) == 0 );
    return status;
}

// ( -- )
status_t repl::do_string(lua_State* L, const char* s)
{
    status_t status = serial::obj().open();
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
status_t repl::do_file(lua_State* L, const char* filename)
{
    status_t status = serial::obj().open();
    if ( status == LUA_OK )
        status = luaL_loadfile(L, filename);

    return do_chunk(L, status);
}

// ( -- )
status_t repl::do_repl(lua_State* L)
{
    status_t status;
    do {
        status = serial::obj().open();
        if ( status == LUA_OK ) {
            assert( lua_gettop(L) == 0 );
            status = push_line(L, true);
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

    return LUA_OK;
}

}
