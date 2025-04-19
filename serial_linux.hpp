#pragma once

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

    // Return the operating system handle for the serial port, which can also indicate
    // whether the port is open (>= 0) or not (= -1).
    int fd() const { return m_fd; }

private:
    // Determine the serial port, either as specified by the option or by automatically
    // scanning all available ports.
    serial();

    int read_line(int timeout_ms) override;

    // Identify the USB serial device(s) where the iProduct field includes "Drop ALT".
    static constexpr char DROP_ALT[] = "Drop_ALT";

    static constexpr int RECONNECT_PERIOD_MS = 100;
    static constexpr int RESPONSE_TIMEOUT_MS = 500;

    char* m_realpath = nullptr;

    int m_fd;
};
