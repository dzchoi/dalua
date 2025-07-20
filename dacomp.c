#include <assert.h>
#include <stdlib.h>             // for realloc()

// Override the lua_writestringerror() definition in the lauxlib.h below.
#define lua_writestringerror(format, ...) \
    fprintf(stderr, format, __VA_ARGS__)

#include "dacomp.h"
#include "lua.h"
#include "lauxlib.h"
#include "lobject.h"            // for getproto()
#include "lstate.h"             // for lua_State
#include "lundump.h"            // for Proto
// #include "lualib.h"


static const char RETURN[] = "return";
static const char FUNCTION[] = "(function()end)(";

// This reader generates a template Lua statement:
//   "return"
//   "(function()end)("
//   "(function()end)("
//   ...
//   "(function()end)("
//   "..." "))...)"
// The number of "(function()end)(" lines is specified by the initial value of `*arg`.
static const char* _reader(lua_State*, void* arg, size_t* psize)
{
    static int depth;
    const int step = (*(int*)arg)++;

    // EOF
    if ( unlikely(step == 1) ) {
        *(int*)arg = 1;
        *psize = 0;
        return NULL;
    }

    // "return"
    if ( step > 1 ) {
        depth = step;
        *(int*)arg = -step;
        *psize = __builtin_strlen(RETURN);
        return RETURN;
    }

    // "(function()end)("
    if ( step < 0 ) {
        *psize = __builtin_strlen(FUNCTION);
        return FUNCTION;
    }

    // "..." "))...)"  (step == 0)
    static char buffer[64] = "...";
    int total = 3;  // 3 == __builtin_strlen("...")
    assert( (size_t)(total + depth) <= sizeof(buffer) );
    for ( int i = 0 ; i < depth ; i++ )
        buffer[total++] = ')';

    *psize = total;
    return buffer;
}

#define toproto(L, i) getproto(L->top + (i))

// ( chunk... -- combined-chunk )
static void combine(lua_State* L)
{
    int n = lua_gettop(L);
    assert( n >= 1 );
    if ( n == 1 )
        return;

    int arg = n;
    status_t status = lua_load(L, _reader, &arg, NULL, "t");
    assert( status == LUA_OK );  // Template should compile successfully.
    // ( -- chunk... template-chunk )

    Proto* f = toproto(L, -1);
    assert( n == f->sizep );
    lua_insert(L, 1);
    // ( -- template-chunk chunk... )

    for ( int i = 0 ; i < n ; i++ ) {
        // Replace each "function()end" with the actual chunk on the stack.
        f->p[i] = toproto(L, -1);
        // Redirect first upvalue (_ENV) to reference outer scope instead of local stack.
        if ( f->p[i]->sizeupvalues > 0 )
            f->p[i]->upvalues[0].instack = 0;
        lua_pop(L, 1);
    }
    // No map to source code line numbers.
    f->sizelineinfo = 0;
    // ( -- combined-chunk )
}

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
status_t compile_files(lua_State* L, const char* filenames[], array_t* parray)
{
    status_t status = LUA_OK;

    // Sequentially compile each Lua file and push its chunk onto the stack.
    for ( const char* filename = *filenames ;
      status == LUA_OK && filename != NULL ; filename = *++filenames ) {
        if ( __builtin_strcmp(filename, "-") == 0 )
            filename = NULL;
        status = luaL_loadfile(L, filename);
    }

    if ( status == LUA_OK ) {
        // Combine the compiled chunks into one big chained chunk.
        combine(L);

        // If strip is enabled (1), run-time error messages will not include the script
        // line, only showing "?:-1:".
        status = lua_dump(L, _writer, parray, 0);  // 0 == strip disabled
        lua_pop(L, 1);  // Pop the compiled chunk.
    }

    if ( status == LUA_ERRSYNTAX ) {
        // Do not prepend the program name to the error message, since the syntax
        // error message already contains the chunk name.
        lua_writestringerror("%s\n", lua_tostring(L, -1));
        lua_pop(L, 1);  // Pop the error message.
    }

    // Negative error status does not have an associated error message on the stack.
    else if ( status > LUA_OK ) {
        l_error(lua_tostring(L, -1));
        lua_pop(L, 1);  // Pop the error message.
    }

    // Sanity check
    assert( lua_gettop(L) == 0 );
    return status;
}
