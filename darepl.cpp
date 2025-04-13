#include <cassert>
#include <cstdlib>              // for free()
#include <iostream>             // for std::cout, std::cerr
#ifndef _WIN32
#include <poll.h>               // for poll()
#endif
#include <readline/readline.h>  // for Readline library
#include <readline/history.h>   // for add_history()
#include <unistd.h>             // for STDIN_FILENO

#include "darepl.hpp"
#include "lua.hpp"              // for lua_*(), luaL_*()
#include "option.hpp"           // for l_error()
#include "serial_port.hpp"      // for serial().*()



namespace lua {

#ifdef _WIN32
static status_t read_status = LUA_OK;

static int _check_serial()
{
    if ( rl_end != 0 )
        return 0;  // Continue readline().

    int len = repl::serial().input_available();
    if ( len == 0 )
        return 0;

    if ( len > 0 ) {
        std::cout << '\r';  // Erase the prompt.
        if ( repl::serial().print_line() ) {
            rl_on_new_line();
            rl_redisplay();  // Show the prompt again.
            return 0;
        }
    }

    rl_replace_line("serial connection lost\n", 0);
    read_status = LUA_ERRIO;
    // repl::serial().close();  // Already closed by serial().print_line().
    rl_done = 1;  // Exit readline().
    return 0;
}

// Read a line from stdin using the Readline library and push it onto the Lua stack.
// In case of a reading failure (e.g., encountering EOF), push an error message instead.
// The pushed line excludes the trailing newline, leaving only the plain text.
// ( -- line | )
static status_t push_line(lua_State* L, bool firstline)
{
    // Enable asynchronous check for the serial port while running readline().
    [[maybe_unused]] static bool _ = (rl_event_hook = _check_serial);

    read_status = LUA_OK;
    char* line = readline(firstline ? repl::LUA_PROMPT : repl::LUA_PROMPT2);
    if ( likely(line != nullptr) ) {
        if ( read_status == LUA_OK )
            lua_pushstring(L, line);
        else
            l_error() << line;
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
static status_t read_status = LUA_OK;

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
        { repl::serial().fd(), POLLIN, 0 },
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
            repl::serial().close();
            break;
        }

        // Display input from the serial port when not actively typing.
        if ( (fds[0].revents & POLLIN) && rl_end == 0 ) {
            std::cout << '\r';  // Erase the prompt.
            if ( unlikely(!repl::serial().print_line()) ) {
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
        status = lua_dump(L, serial_port::_writer, &serial(), 0);  // 0 == strip disabled
        lua_pop(L, 1);  // Pop the compiled chunk.
    }

    if ( status == LUA_OK ) {
        status = serial().receive_status(true, -1);
        // For statuses remotely received, the stack does not contain an error message,
        // as any error would have already been displayed remotely. So, we only pass the
        // received status here.
        if ( status < LUA_OK )  // i.e. status == LUA_ERRIO
            l_error() << "no response from serial port\n";
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
    status_t status = serial().open();
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
    status_t status = serial().open();
    if ( status == LUA_OK )
        status = luaL_loadfile(L, filename);

    return do_chunk(L, status);
}

// ( -- )
status_t repl::do_repl(lua_State* L)
{
    status_t status;
    do {
        status = serial().open();
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
