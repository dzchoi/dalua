#include <chrono>               // for std::chrono::...
#include <iostream>             // for std::cout

#include "serial_common.hpp"



status_t serial_common::receive_status(bool verbose, int timeout_ms)
{
    using namespace std::chrono;
    steady_clock::time_point since = steady_clock::now();

    do {
        if ( timeout_ms > 0 ) {
            steady_clock::time_point now = steady_clock::now();
            auto elapsed_ms = duration_cast<milliseconds>(now - since).count();
            if ( timeout_ms <= elapsed_ms )
                break;
            timeout_ms -= elapsed_ms;  // timeout_ms > 0!
            since = now;
        }

        int len = read_line(timeout_ms);
        if ( len <= 0 )
            break;  // A timeout or a serial port error

        if ( len == sizeof(ping) ) {
            status_t status = reinterpret_cast<const ping*>(m_buffer)->status();
            if ( status != LUA_NOSTATUS )
                return status;
        }

        // Display any other messages from the serial port while waiting.
        if ( verbose )
            std::cout.write(m_buffer, len);
    } while ( timeout_ms != 0 );

    return LUA_ERRIO;
}

bool serial_common::print_line()
{
    int len;
    do {
        len = read_line(-1);
        if ( len <= 0 )
            return false;
        std::cout.write(m_buffer, len);
    } while ( m_buffer[len - 1] != '\n' );
    return true;
}
