#pragma once

#include <windows.h>            // for HANDLE
#include "serial_common.hpp"



struct sp_port;

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

    // Return the number of bytes available to read on success, a negative error code
    // otherwise.
    int input_available();

    // Return the operating system handle for the serial port, which can also indicate
    // whether the port is open (!= INVALID_HANDLE_VALUE) or not (= INVALID_HANDLE_VALUE).
    HANDLE fd() const;

private:
    // Determine the serial port, either as specified by the option or by automatically
    // scanning all available ports.
    serial();

    int read_line(int timeout_ms) override;

    // Identify the USB serial device(s) where the iProduct field includes "Drop ALT".
    static constexpr char DROP_ALT[] = "Drop ALT";

    static constexpr int RECONNECT_PERIOD_MS = 100;
    static constexpr int RESPONSE_TIMEOUT_MS = 500;

    sp_port* m_port;

    unsigned m_size_out = 0;
    unsigned m_size_in = 0;
};
