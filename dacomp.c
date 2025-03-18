#include <assert.h>             // for assert()
#include <stddef.h>             // for size_t
#include <stdlib.h>             // for realloc()

// Override the lua_writestringerror() definition in the lauxlib.h below.
#define lua_writestringerror(format, ...) \
    fprintf(stderr, format, __VA_ARGS__)

#include "dalua.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"



static status_t _writer(lua_State*, const void* pdata, size_t sz, void* arg)
{
    array_t* const parray = (array_t*)arg;

    size_t new_capacity = parray->capacity;
    while ( parray->size + sz > new_capacity )
        new_capacity *= 2;

    if ( parray->capacity != new_capacity ) {
        void* new_memory = realloc(parray->memory, new_capacity);
        if ( !new_memory )
            return LUA_ERRMEM;
        parray->memory = new_memory;
        parray->capacity = new_capacity;
    }

    __builtin_memcpy(parray->memory + parray->size, pdata, sz);
    parray->size += sz;
    return LUA_OK;
}

// ( -- )
status_t compile_file(lua_State* L, const char* filename, array_t* parray)
{
    status_t status = luaL_loadfile(L, filename);
    if ( status == LUA_OK ) {
        // If strip is enabled (1), run-time error messages will not include
        // the script line, only showing "?:-1:".
        status = lua_dump(L, _writer, parray, 0);  // 0 == strip disabled
        lua_pop(L, 1);  // Pop the compiled chunk.
    }

    if ( status == LUA_ERRSYNTAX ) {
        // Do not prepend the program name to the error message, since the syntax
        // error message already contains the chunk name.
        lua_writestringerror("%s\n", lua_tostring(L, -1));
        lua_pop(L, 1);  // Pop the error message.
    }
    // LUA_ERRIO does not have the associated error message on the stack.
    else if ( status != LUA_OK && status != LUA_ERRIO ) {
        l_message(lua_tostring(L, -1));
        lua_pop(L, 1);  // Pop the error message.
    }

    // Sanity check
    assert( lua_gettop(L) == 0 );
    return status;
}
