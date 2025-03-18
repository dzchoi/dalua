#pragma once

#include <stdbool.h>            // for bool
#include <stdio.h>              // for stderr

#include "lua.h"                // for lua_State



// Program name for this application, i.e. "luada".
extern const char* PROGNAME;

typedef int status_t;  // for LUA_OK, LUA_ERRSYNTAX, etc

// Additional Lua statuses
#define LUA_ERRIO   (-1)  // IO error (e.g. serial connection lost or EOF in stdin)
#define LUA_ERROPT  (-2)  // Command-line argument error

// Compile-time strlen(s) when s points to a literal string.
#define LEN(s)  (sizeof(s)/sizeof((s)[0]) - 1)

// Print an error message in printf() fashion, implicitly adding the program name at
// the beginning and a newline at the end. The format doesn't need to be a literal
// string if it is the only argument.
#define l_message(format, ...) \
    _l_message_ ## __VA_OPT__(1) (format __VA_OPT__(,) __VA_ARGS__)

#define _l_message_(s)  fprintf(stderr, "%s: %s\n", PROGNAME, (s))
#define _l_message_1(format, ...) \
    fprintf(stderr, "%s: " format "\n", PROGNAME, __VA_ARGS__)



// Establish a serial connection to the appropriate "/dev/ttyACMx" device, returning
// the status (LUA_OK or LUA_ERRIO) of the connection. If verbose is true, print out
// any incoming messages from the serial port while waiting.
status_t setup_serial(bool verbose);

// Execute the given string remotely.
// ( -- )
status_t do_string(lua_State* L, const char* s);

// Execute the given file remotely.
// If filename is NULL, it reads from the stdin. The first line in the file is
// ignored if it starts with a #.
// ( -- )
status_t do_file(lua_State* L, const char* filename);

// Execute Lua REPL remotely.
// ( -- )
status_t do_repl(lua_State* L);

// Dynamic array that can enlarge as more data is written.
typedef struct {
    void* memory;
    size_t size;
    size_t capacity;
} array_t;

// Compile the given file and output the bytecode to stdout.
// If filename is NULL, it reads from the stdin. The first line in the file is
// ignored if it starts with a #.
// ( -- )
status_t compile_file(lua_State* L, const char* filename, array_t* parray);
