#pragma once

#include "lua.hpp"              // for status_t, lua_State
#include "serial_port.hpp"      // for serial_port



namespace lua {

class repl {
public:
    static serial_port& serial() {
        static serial_port _serial;  // Lazy initialization when required.
        return _serial;
    }

    // Execute the given string remotely.
    // ( -- )
    static status_t do_string(lua_State* L, const char* s);

    // Execute the given file remotely.
    // If filename is NULL, it reads from the stdin. The first line in the file is
    // ignored if it starts with '#'.
    // ( -- )
    static status_t do_file(lua_State* L, const char* filename);

    // Execute Lua REPL remotely.
    // ( -- )
    static status_t do_repl(lua_State* L);

    // Note that non-printing parts of the prompt should be wrapped with '\x1' and '\x2'.
    static constexpr char LUA_PROMPT[]  = "\x1\e[0m\x2" "> ";
    static constexpr char LUA_PROMPT2[] = "\x1\e[0m\x2" "+ ";

private:
    constexpr repl() =delete;  // Ensure a static class

    // If the status is LUA_OK, execute the chunk on the stack remotely and return the
    // remote status. Otherwise, display the error message on the stack.
    // ( chunk | error | -- )
    static status_t do_chunk(lua_State* L, status_t status);
};

}
