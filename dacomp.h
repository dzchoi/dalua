#pragma once

#include <limits.h>             // for INT_MIN
#include <stddef.h>             // for size_t
#include <stdio.h>              // for fprintf(), stderr

#include "lua.h"                // for lua_State



// Represents the status code indicating both the Lua REPL state and the serial port
// condition.
typedef int status_t;  // E.g. LUA_OK, LUA_ERRSYNTAX, etc

// Additional Lua statuses. They do not have an associated error message on the stck.
static const status_t LUA_ERROPT   = -1;  // Command-line argument error
static const status_t LUA_ERRIO    = -2;  // IO error (e.g. serial connection lost)
static const status_t LUA_ERRFATAL = -3;  // Non-recoverable IO error (e.g. EOF in stdin)
static const status_t LUA_NOSTATUS = INT_MIN;

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

// Print an error message in printf() fashion, implicitly adding the program name at
// the beginning and a newline at the end. The format doesn't need to be a literal
// string if it is the only argument.
#define l_error(format, ...) \
    _l_error_ ## __VA_OPT__(1) (format __VA_OPT__(,) __VA_ARGS__)

#define _l_error_(s)  fprintf(stderr, "%s: %s\n", PROGNAME, (s))
#define _l_error_1(format, ...) \
    fprintf(stderr, "%s: " format "\n", PROGNAME, __VA_ARGS__)



// Program name for this application, "daluac".
extern const char* const PROGNAME;

// Dynamic array that can enlarge as more data is written.
typedef struct {
    void* memory;
    size_t size;
    size_t capacity;
} array_t;

// Compile the given files and output the bytecode to stdout.
// If filename is NULL, it reads from the stdin. The first line in the file is ignored
// if it starts with '#'.
// ( -- )
status_t compile_files(lua_State* L, const char* filenames[], array_t* parray);
