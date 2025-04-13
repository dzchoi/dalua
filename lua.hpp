#pragma once

#include <climits>              // for INT_MIN

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}



// Represents the status code indicating both the Lua REPL state and the serial port
// condition.
using status_t = int;  // E.g. LUA_OK, LUA_ERRSYNTAX, etc

// Additional Lua statuses. They do not have an associated error message on the stck.
constexpr status_t LUA_ERROPT   = -1;  // Command-line argument error
constexpr status_t LUA_ERRIO    = -2;  // IO error (e.g. serial connection lost)
constexpr status_t LUA_ERRFATAL = -3;  // Non-recoverable IO error (e.g. EOF in stdin)
constexpr status_t LUA_NOSTATUS = INT_MIN;

#define likely(x) __builtin_expect((x), true)
#define unlikely(x) __builtin_expect((x), false)
