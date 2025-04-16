#include <cassert>
#include <chrono>               // for std::chrono::...
#include <iostream>             // for std::cout
#include <libserialport.h>      // for sp_*() from Libserialport
#include <thread>               // for std::this_thread::sleep_for()
#include <windows.h>            // for HANDLE

#include "option.hpp"           // for option::..., l_error()
#include "serial_win.hpp"



// Initialize m_port and set option::DEVICE_PATH if it is currently undefined.
serial::serial()
: m_port(nullptr)
{
    if ( option::DEVICE_PATH ) {
        sp_get_port_by_name(option::DEVICE_PATH, &m_port);
        return;
    }

    sp_port** ports;
    sp_list_ports(&ports);
    sp_port** pport = ports;

    for ( ; *pport ; ++pport ) {
        sp_port* const port = *pport;
        const char* prod = sp_get_port_usb_product(port);
        if ( prod == nullptr || __builtin_strstr(prod, DROP_ALT) == nullptr )
            continue;

        if ( m_port ) {
            sp_free_port(m_port);
            m_port = nullptr;
            break;
        }

        sp_copy_port(port, &m_port);
        option::DEVICE_PATH = sp_get_port_name(m_port);
    }

    if ( m_port == nullptr ) {
        if ( *pport )
            l_error() << "multiple devices found for \"" << DROP_ALT << "\"\n";
        else
            l_error() << "no device found for \"" << DROP_ALT << "\"\n";
    }

    sp_free_port_list(ports);
}

serial::~serial()
{
    if ( m_port ) {
        close();
        sp_free_port(m_port);
    }
}

void serial::close()
{
    // Do not call sp_free_port() to enable m_port reopened.
    assert( m_port );
    sp_close(m_port);
}

int serial::input_available()
{
    int len = sp_input_waiting(m_port);
    if ( len < 0 )
        close();
    return len;
}

// status_t serial::wait_for_input()
// {
//     DWORD eventMask;
//     if ( WaitCommEvent(fd(), &eventMask, nullptr) && (eventMask & EV_RXCHAR) )
//         return LUA_OK;
//     close();
//     return LUA_ERRIO;
// }

HANDLE serial::fd() const
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    (void)sp_get_port_handle(m_port, &handle);
    return handle;
}

status_t serial::open()
{
    if ( m_port == nullptr )
        return LUA_ERRFATAL;

    // If it's already open, reuse it.
    if ( fd() != INVALID_HANDLE_VALUE )
        return LUA_OK;

    // Open the port, attempting repeatedly if NO_RECONNECT is false.
    while ( sp_open(m_port, SP_MODE_READ_WRITE) != SP_OK ) {
        if ( unlikely(option::NO_RECONNECT) ) {
            char* msg = sp_last_error_message();
            l_error() << msg << ": " << option::DEVICE_PATH << '\n';
            sp_free_error_message(msg);
            return LUA_ERRFATAL;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_PERIOD_MS));
    }

    // Resolve the issue where DTR is not set when Libserialport opens a serial port for
    // the first time. This issue does not occur if the port was previously used by
    // another terminal.
    sp_set_dtr(m_port, SP_DTR_ON);

    // Send a ping to the remote Lua REPL and await a response.
    const ping ping(option::LOG_MASK);
    const bool display_logs = option::LOG_MASK & ~0x01;
    const bool display_welcome = option::LOG_MASK & 0x01;
    if ( sp_blocking_write(m_port, &ping, sizeof(ping), 0) == sizeof(ping) ) {
        if ( receive_status(display_logs, RESPONSE_TIMEOUT_MS) == LUA_OK ) {
            if ( display_welcome )
                std::cout << "Connected to " << option::DEVICE_PATH << '\n';
            return LUA_OK;
        }
    }

    l_error() << "no Lua running on " << option::DEVICE_PATH << '\n';
    close();
    return LUA_ERRFATAL;
}

// Not used but this function can check if the internal buffer associated with a serial
// port is full, before reading from the port.
// #include <windows.h>
// static bool is_input_buffer_full(HANDLE handle)
// {
//     DWORD errors;
//     COMSTAT comStat;
//     assert( ClearCommError(handle, &errors, &comStat) );
//     return (errors & CE_RXOVER) != 0;
// }

// Simulate Canonical input mode (line-buffered input mode) on Windows.
int serial::read_line(int timeout_ms)
{
    if ( m_size_out > 0 ) {
        m_size_in -= m_size_out;
        __builtin_memmove(m_buffer, m_buffer + m_size_out, m_size_in);
        m_size_out = 0;
    }

    using namespace std::chrono;
    steady_clock::time_point since = steady_clock::now();

    do {
        for ( ; m_size_out < m_size_in && m_buffer[m_size_out] != '\n' ; ++m_size_out );
        if ( m_size_out < m_size_in )  // Found a complete line.
            return ++m_size_out;  // Include '\n'.

        if ( m_size_out == sizeof(m_buffer) )  // Buffer is full.
            break;

        int len;
        if ( timeout_ms != 0 ) {
            if ( timeout_ms > 0 ) {
                steady_clock::time_point now = steady_clock::now();
                auto elapsed_ms = duration_cast<milliseconds>(now - since).count();
                if ( timeout_ms <= elapsed_ms )
                    break;
                timeout_ms -= elapsed_ms;  // timeout_ms > 0!
                since = now;
            }

            len = sp_blocking_read_next(
                m_port, m_buffer + m_size_in, sizeof(m_buffer) - m_size_in,
                timeout_ms > 0 ? timeout_ms : 0);
        }
        else {
            len = sp_nonblocking_read(
                m_port, m_buffer + m_size_in, sizeof(m_buffer) - m_size_in);
        }

        if ( len < 0 ) {  // Read error!
            close();
            return len;
        }
        m_size_in += len;

        if ( len == 0 )  // Timeout! This can happen only when timeout_ms >= 0.
            break;
    } while ( timeout_ms != 0 );

    // Flush the buffer.
    return m_size_out = m_size_in;
}

lua_Writer serial::writer()
{
    return [](lua_State*, const void* pdata, size_t sz, void* arg) {
        serial* that = static_cast<serial*>(arg);
        if ( sp_blocking_write(that->m_port, pdata, sz, 0) == int(sz) )
            return LUA_OK;

        char* msg = sp_last_error_message();
        l_error() << msg << ": " << option::DEVICE_PATH << '\n';
        sp_free_error_message(msg);
        that->close();
        return LUA_ERRIO;
    };
}
