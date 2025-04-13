#pragma once

#include <cstdint>              // for uint8_t
#include <iostream>             // for std::ostream, std::cerr
#include <unordered_map>        // for std::unordered_map<>

#include "lua.hpp"              // for lua_State, status_t



// A hand-crafted, tail-recursive command-line parser rather than using getopt().
class option {
public:
    using opt_f_map = std::unordered_map<char, status_t (*)(lua_State*)>;
    using optarg_f_map = std::unordered_map<char, status_t (*)(lua_State*, const char*)>;

    option(lua_State* L, opt_f_map opt_f, optarg_f_map optarg_f);

    // Parse the command line arguments and result in the following option values.
    status_t parse_argv(const char* const* argv);

    // Options extracted by parse_argv().
    static const char* PROGNAME;  // Captured from argv[0] ("dalua")
    static const char* DEVICE_PATH;  // E.g. "COM7" or "/dev/ttyACMx"
    static bool NO_RECONNECT;
    static uint8_t LOG_MASK;
    static bool interactive;

private:
    lua_State* m_L;

    // The option character currently being processed.
    char m_opt_c;

    // Handlers for options without arguments.
    opt_f_map m_opt_f;

    // Handlers for options requiring arguments.
    optarg_f_map m_optarg_f;

    status_t parse_args(const char* const* args);

    status_t parse_chars(const char* arg, const char* const* next_args);
};

inline std::ostream& l_error() {
    std::cerr << option::PROGNAME << ": ";
    return std::cerr;
}
