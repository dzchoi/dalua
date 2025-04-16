#pragma once

#include <cstdint>              // for int8_t, uint32_t

#include "lua.hpp"              // for status_t, lua_Writer



class serial_common {
public:
    virtual ~serial_common() =default;

    // Open the serial port determined by serial().
    virtual status_t open() =0;

    // Close the serial port, enabling it to be reopened later if needed. This method is
    // automatically invoked upon an error when accessing the port.
    virtual void close() =0;

    // Receive a response from the remote Lua REPL and return the status code in it, or
    // LUA_ERRIO in the case of a serial port error or a timeout. Timeout behavior is as
    // follows:
    //  - timeout_ms > 0: Wait up to the specified timeout in milliseconds.
    //  - timeout_ms = 0: Perform a non-blocking read (no waiting).
    //  - timeout_ms < 0: Wait indefinitely.
    status_t receive_status(bool verbose, int timeout_ms);

    // Read a complete line of input from the serial port and output it to stdout.
    bool print_line();

    // A lua_Writer function used by lua_dump.
    virtual lua_Writer writer() =0;

protected:
    // This buffer temporarily stores input from the serial port. Overflowing input is
    // handled seamlessly, only requiring the handler to run more frequently. Note that
    // the OS buffer for a serial port is typically 4 KB, which retains unread input data.
    static constexpr size_t MAX_SERIAL_INPUT = 128;
    char m_buffer[MAX_SERIAL_INPUT];

    // Read a line from the serial port into m_buffer[], stopping at either a newline
    // character or a timeout, and return the number of characters read. The timeout
    // behavior is the same as in receive_status().
    virtual int read_line(int timeout_ms) =0;
};



class ping {
public:
    constexpr ping(status_t status):
        hd0('['), hd1('}'), st(status + '0'), nl('\n') {}

    operator uint32_t() const { return value; }

    // Retrieve the status embedded in the ping, or return -1 if the ping is invalid.
    status_t status() const {
        if ( hd0 == '[' && hd1 == '}' && nl == '\n' )
            return st - '0';
        return LUA_NOSTATUS;
    }

private:
    union {
        uint32_t value;
        struct {
            const char hd0;   // '['
            const char hd1;   // '}'
            const int8_t st;  // 1 byte of status
            const char nl;    // '\n'
        } __attribute__((packed));
    };
};
