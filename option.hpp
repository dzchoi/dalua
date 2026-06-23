#pragma once

#include <cstdint>              // for uint8_t
#include <iostream>             // for std::ostream, std::cerr



struct option_t {
    // Options extracted by opt_parser.
    const char* PROGNAME    = nullptr;  // Captured from argv[0] ("dalua")
    const char* DEVICE_PATH = nullptr;  // E.g. "COM7" or "/dev/ttyACMx"
    bool NO_RECONNECT       = false;
    uint8_t LOG_MASK        = 0x01;  // Disable all thread logs except the REPL welcome message.
    bool interactive        = true;
};

// Shared global instance (C++17 inline)
inline option_t option;

inline std::ostream& l_error() {
    std::cerr << option.PROGNAME << ": ";
    return std::cerr;
}
