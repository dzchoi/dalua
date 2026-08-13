#include <chrono>               // for std::chrono::...
#include <cstring>              // for strlen(), strcpy()
#include <cwchar>               // for wcsstr()
#include <iostream>             // for std::cout
#include <string>               // for std::string
#include <thread>               // for std::this_thread::sleep_for()

#define INITGUID                // instantiate GUID_DEVCLASS_PORTS and DEVPKEY_* below
#ifndef _WIN32_WINNT            // SetupDiGetDevicePropertyW requires Vista (0x0600) or later
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>            // for HANDLE, CreateFile(), ReadFile(), ...
#include <initguid.h>           // for DEFINE_GUID / DEFINE_DEVPROPKEY storage
#include <devguid.h>            // for GUID_DEVCLASS_PORTS
#include <devpkey.h>            // for DEVPKEY_Device_BusReportedDeviceDesc
#include <setupapi.h>           // for SetupDi*()

#include "option.hpp"           // for option, l_error()
#include "serial_win.hpp"



// Return the message for a Win32 error code.
static std::string win_error(DWORD err)
{
    char* msg = nullptr;
    // FORMAT_MESSAGE_MAX_WIDTH_MASK collapses the message's line breaks into spaces and
    // drops the trailing newline, so no manual trimming is needed.
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
        nullptr, err, 0, reinterpret_cast<LPSTR>(&msg), 0, nullptr);

    std::string result = msg ? msg : "unknown error";
    if ( msg )
        LocalFree(msg);
    return result;
}

// Read the COM port name (e.g. "COM7") for a device from its hardware registry key,
// returning it, or an empty string on failure.
static std::string read_port_name(HDEVINFO devs, SP_DEVINFO_DATA* info)
{
    std::string result;
    HKEY key = SetupDiOpenDevRegKey(devs, info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if ( key != INVALID_HANDLE_VALUE ) {
        // The name ranges from "COM1" to "COM32767", so 16 bytes should be enough to
        // hold it with the null terminator.
        char name[16];
        DWORD type = 0;
        DWORD len = sizeof(name);
        LSTATUS status = RegQueryValueExA(
            key, "PortName", nullptr, &type, reinterpret_cast<LPBYTE>(name), &len);
        RegCloseKey(key);
        if ( status == ERROR_SUCCESS && type == REG_SZ )
            result = name;
    }
    return result;
}

// Scan all present serial ports for a unique USB device whose bus-reported description
// (the USB iProduct string) contains needle. On a unique match, return its COM port name
// (e.g. "COM7"). Otherwise return an empty string, reporting the reason (no match,
// multiple matches, or device enumeration failure) here.
static std::string find_device(const char* needle)
{
    // Promote the ASCII needle to wide (wchar_t) so it can be matched against the wide
    // device description with the standard wcsstr().
    wchar_t wneedle[__builtin_strlen(needle) + 1];
    MultiByteToWideChar(CP_ACP, 0, needle, -1, wneedle, ARRAYSIZE(wneedle));

    HDEVINFO devs = SetupDiGetClassDevsA(
        &GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if ( devs == INVALID_HANDLE_VALUE ) {
        l_error() << win_error(GetLastError()) << ": SetupDiGetClassDevs\n";
        return {};
    }

    std::string port_name;
    bool ambiguous = false;
    SP_DEVINFO_DATA info = {};
    info.cbSize = sizeof(info);
    for ( DWORD i = 0 ; SetupDiEnumDeviceInfo(devs, i, &info) ; ++i ) {
        wchar_t desc[LINE_LEN];
        DEVPROPTYPE type;
        if ( !SetupDiGetDevicePropertyW(
                 devs, &info, &DEVPKEY_Device_BusReportedDeviceDesc, &type,
                 reinterpret_cast<PBYTE>(desc), sizeof(desc), nullptr, 0) )
            continue;
        if ( !wcsstr(desc, wneedle) )
            continue;

        if ( !port_name.empty() ) {  // More than one match.
            ambiguous = true;
            break;
        }
        port_name = read_port_name(devs, &info);
    }

    SetupDiDestroyDeviceInfoList(devs);

    if ( ambiguous ) {
        l_error() << "multiple devices found for \"" << needle << "\"\n";
        port_name.clear();
    }
    else if ( port_name.empty() )
        l_error() << "no device found for \"" << needle << "\"\n";

    return port_name;
}



// Resolve option.DEVICE_PATH if it is currently undefined.
serial::serial()
{
    if ( option.DEVICE_PATH )
        return;

    // The returned name backs option.DEVICE_PATH, so it must outlive this constructor.
    static std::string port_name = find_device(DROP_ALT);
    if ( !port_name.empty() )
        option.DEVICE_PATH = port_name.c_str();
}

serial::~serial()
{
    close();
}

void serial::close()
{
    if ( m_handle != INVALID_HANDLE_VALUE ) {
        CancelIo(m_handle);
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        m_size_in = m_size_out = 0;  // Drop any partial line input from the old session.
    }
}

bool serial::configure()
{
    // 8N1, no flow control. The baud rate is irrelevant for a USB CDC ACM device but is
    // set to a sane value anyway. Zero-init leaves flow control off; we assert DTR/RTS
    // so the remote device recognizes us as ready.
    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if ( !SetCommState(m_handle, &dcb) )
        return false;

    // Discard any stray data lingering in the driver's output buffer, analogous to the
    // output flush (TCOFLUSH) done on Linux before the initial exchange.
    PurgeComm(m_handle, PURGE_TXCLEAR);
    return true;
}

status_t serial::open()
{
    // If it's already open, reuse it.
    if ( m_handle != INVALID_HANDLE_VALUE )
        return LUA_OK;

    if ( option.DEVICE_PATH == nullptr )
        return LUA_ERRFATAL;

    // The "\\.\" prefix is required to open COM10 and above, and is harmless for COM1-9.
    char fullpath[MAX_PATH] = "\\\\.\\";
    __builtin_strncat(
        fullpath, option.DEVICE_PATH, sizeof(fullpath) - __builtin_strlen(fullpath) - 1);

    // Open the port, attempting repeatedly if NO_RECONNECT is false.
    while ( (m_handle = CreateFileA(
                 fullpath, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                 OPEN_EXISTING, 0, nullptr))
            == INVALID_HANDLE_VALUE ) {
        DWORD err = GetLastError();
        if ( err == ERROR_ACCESS_DENIED ) {
            l_error() << "locked by another process: " << option.DEVICE_PATH << '\n';
            return LUA_ERRFATAL;
        }
        if ( unlikely(option.NO_RECONNECT) ) {
            l_error() << win_error(err) << ": " << option.DEVICE_PATH << '\n';
            return LUA_ERRFATAL;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_PERIOD_MS));
    }

    if ( configure() ) {
        // Send a ping to the remote Lua REPL and await a response.
        const ping ping(option.LOG_MASK);
        const bool display_logs = option.LOG_MASK & ~0x01;
        const bool display_welcome = option.LOG_MASK & 0x01;
        while ( write_all(&ping, sizeof(ping)) ) {
            status_t status = receive_status(display_logs, RESPONSE_TIMEOUT_MS);
            if ( status == LUA_OK ) {
                if ( display_welcome )
                    std::cout << APP_VERSION << ": connected to "
                              << option.DEVICE_PATH << '\n';
                return LUA_OK;
            }
            if ( status != LUA_YIELD )
                break;

            // DFU mode confirms that this is our device but the REPL is not ready yet.
            // Keep the established port open and try again shortly.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(RESPONSE_TIMEOUT_MS));
        }
        l_error() << "no Lua running on " << option.DEVICE_PATH << '\n';
    } else
        l_error() << win_error(GetLastError()) << ": " << option.DEVICE_PATH << '\n';

    close();
    return LUA_ERRFATAL;
}

int serial::read_some(char* buf, size_t bufsize, int timeout_ms)
{
    // Program the read timeout for this call. With ReadIntervalTimeout = MAXDWORD, two
    // documented COMMTIMEOUTS special cases drive the behavior: when both total timeouts
    // are zero (timeout_ms == 0) ReadFile returns at once with whatever is already
    // buffered, even nothing (non-blocking); otherwise the totals set below make it
    // return on the first available byte or when the total timeout elapses.
    COMMTIMEOUTS to = {};
    to.ReadIntervalTimeout = MAXDWORD;
    if ( timeout_ms != 0 ) {
        // Return as soon as any byte is available, otherwise block up to the total
        // timeout; for "indefinite" use the largest finite constant (~49 days).
        to.ReadTotalTimeoutMultiplier = MAXDWORD;
        to.ReadTotalTimeoutConstant = timeout_ms < 0 ? MAXDWORD - 1 : DWORD(timeout_ms);
    }
    if ( !SetCommTimeouts(m_handle, &to) )
        return -1;

    DWORD nread = 0;
    if ( !ReadFile(m_handle, buf, DWORD(bufsize), &nread, nullptr) )
        return -1;
    return int(nread);  // 0 means the timeout elapsed with no data.
}

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

    while ( true ) {
        for ( ; m_size_out < m_size_in && m_buffer[m_size_out] != '\n' ; ++m_size_out );
        if ( m_size_out < m_size_in )  // Found a complete line.
            return ++m_size_out;  // Include '\n'.

        if ( m_size_out == sizeof(m_buffer) )  // Buffer is full.
            break;

        if ( timeout_ms > 0 ) {
            steady_clock::time_point now = steady_clock::now();
            auto elapsed_ms = duration_cast<milliseconds>(now - since).count();
            if ( timeout_ms <= elapsed_ms )
                break;
            timeout_ms -= elapsed_ms;  // timeout_ms > 0!
            since = now;
        }

        int len = read_some(
            m_buffer + m_size_in, sizeof(m_buffer) - m_size_in, timeout_ms);
        if ( len < 0 ) {  // Read error!
            close();
            return len;
        }
        m_size_in += len;

        if ( len == 0 )
            // No data: the timeout elapsed, or a non-blocking read found nothing.
            break;
    }

    // On buffer overflow, return the partial data so the buffer can be replenished.
    // On timeout, reset the scan position and return 0 (matching Linux behavior),
    // preserving any partial data in the buffer for the next call.
    if ( m_size_out < sizeof(m_buffer) )
        m_size_out = 0;
    return m_size_out;
}

bool serial::write_all(const void* data, size_t sz)
{
    const char* p = static_cast<const char*>(data);
    size_t remaining = sz;

    while ( remaining > 0 ) {
        DWORD nwritten = 0;
        if ( !WriteFile(m_handle, p, DWORD(remaining), &nwritten, nullptr)
          || nwritten == 0)
            return false;
        p += nwritten;
        remaining -= nwritten;
    }
    return true;
}

lua_Writer serial::writer()
{
    return [](lua_State*, const void* pdata, size_t sz, void* arg) {
        serial* that = static_cast<serial*>(arg);
        if ( that->write_all(pdata, sz) )
            return LUA_OK;

        l_error() << win_error(GetLastError()) << ": " << option.DEVICE_PATH << '\n';
        that->close();
        return LUA_ERRIO;
    };
}
