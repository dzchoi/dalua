#pragma once

#include <stdbool.h>            // for bool
#include <stddef.h>             // for size_t
#include <stdint.h>             // for uint32_t, ...
#include <stdio.h>              // for fprintf(), stderr

#include "lua.h"                // for lua_State



// Program name for this application, "dalua" or "daluac".
extern const char* const PROGNAME;

// Options configured for dalua.
extern const char* DEVICE_PATH;
extern bool NO_RECONNECT;
extern uint8_t LOG_MASK;

// Identify the devices in `lsusb` where the iProduct field includes "Drop ALT".
static const char DROP_ALT[] = "Drop_ALT";

static const int RECONNECT_PERIOD_MS = 100;
static const int RESPONSE_TIMEOUT_MS = 500;

// Note that non-printing parts of the prompt should be wrapped with '\x1' and '\x2'.
static const char LUA_PROMPT[]  = "\x1\e[0m\x2" "> ";
static const char LUA_PROMPT2[] = "\x1\e[0m\x2" "+ ";



typedef int status_t;  // for LUA_OK, LUA_ERRSYNTAX, etc

// Additional Lua statuses. They do not have an associated error message on the stck.
static const status_t LUA_ERROPT   = -1;  // Command-line argument error
static const status_t LUA_ERRIO    = -2;  // IO error (e.g. serial connection lost)
static const status_t LUA_ERRFATAL = -3;  // Non-recoverable IO error (e.g. EOF in stdin)

static inline uint32_t PING(status_t status)
{
    return '[' | '}' << 8 | ((int8_t)status + '0') << 16 | '\n' << 24;
}

// Print an error message in printf() fashion, implicitly adding the program name at
// the beginning and a newline at the end. The format doesn't need to be a literal
// string if it is the only argument.
#define l_message(format, ...) \
    _l_message_ ## __VA_OPT__(1) (format __VA_OPT__(,) __VA_ARGS__)

#define _l_message_(s)  fprintf(stderr, "%s: %s\n", PROGNAME, (s))
#define _l_message_1(format, ...) \
    fprintf(stderr, "%s: " format "\n", PROGNAME, __VA_ARGS__)



// Execute the given string remotely.
// ( -- )
status_t do_string(lua_State* L, const char* s);

// Execute the given file remotely.
// If filename is NULL, it reads from the stdin. The first line in the file is ignored
// if it starts with '#'.
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
// If filename is NULL, it reads from the stdin. The first line in the file is ignored
// if it starts with '#'.
// ( -- )
status_t compile_file(lua_State* L, const char* filename, array_t* parray);
