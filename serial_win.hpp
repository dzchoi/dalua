#pragma once

#include <windows.h>            // for HANDLE
#include "serial_common.hpp"



class serial: public serial_common {
public:
    static serial& obj() {  // singleton instance
        static serial obj;  // Lazy initialization when required.
        return obj;
    }

    ~serial();

    status_t open() override;

    void close() override;

    lua_Writer writer() override;

    // Return the number of bytes waiting in the input buffer, or a negative value if
    // the port has been lost.
    int input_available();

private:
    // Determine the serial port, either as specified by the option or by automatically
    // scanning all available ports.
    serial();

    int read_line(int timeout_ms) override;

    // Configure the freshly opened port: 8N1 with DTR/RTS asserted. Return true on
    // success.
    bool configure();

    // Read up to bufsize bytes into buf, blocking for input per the timeout if necessary.
    // The timeout behavior matches read_line(): timeout_ms < 0 waits indefinitely, 0 is
    // non-blocking, > 0 waits up to the given milliseconds. Return the number of bytes
    // read (>= 0, where 0 means the timeout elapsed) or -1 on error.
    int read_some(char* buf, size_t bufsize, int timeout_ms);

    // Write all sz bytes to the port. Return true on success.
    bool write_all(const void* data, size_t sz);

    // Identify the USB serial device where the iProduct field (reported as the USB bus
    // device description) includes "Drop ALT".
    static constexpr char DROP_ALT[] = "Drop ALT";

    static constexpr int RECONNECT_PERIOD_MS = 100;
    static constexpr int RESPONSE_TIMEOUT_MS = 500;

    HANDLE m_handle = INVALID_HANDLE_VALUE;

    unsigned m_size_out = 0;
    unsigned m_size_in = 0;
};
