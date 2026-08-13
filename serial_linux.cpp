#include <cassert>
#include <chrono>               // for std::chrono::milliseconds
#include <cstdlib>              // for realpath(), free()
#include <cstring>              // for strerror()
#include <dirent.h>             // for opendir(), readdir(), closedir()
#include <fcntl.h>              // for open(), O_RDWR, O_NOCTTY
#include <poll.h>               // for poll()
#include <sys/file.h>           // for flock()
#include <termios.h>            // for tcgetattr(), tcsetattr()
#include <thread>               // for std::this_thread::sleep_for()
#include <unistd.h>             // for read(), write(), close(), STDIN_FILENO

#include "option.hpp"           // for option, l_error()
#include "serial_linux.hpp"



// Resolve option.DEVICE_PATH if it is currently undefined.
serial::serial()
: m_fd(-1)
{
    if ( option.DEVICE_PATH )
        return;

    constexpr char DEVICE_DIR[] = "/dev/serial/by-id/";

    DIR* pdir = opendir(DEVICE_DIR);
    if ( pdir == nullptr ) {
        l_error() << strerror(errno) << ": " << DEVICE_DIR << '\n';
        return;
    }

    dirent* pdirent;
    char device_path[sizeof(pdirent->d_name) + __builtin_strlen(DEVICE_DIR)];
    __builtin_strcpy(device_path, DEVICE_DIR);

    bool found = false;
    while ( (pdirent = readdir(pdir)) != nullptr ) {
        if ( __builtin_strstr(pdirent->d_name, DROP_ALT) == nullptr )
            continue;

        if ( found ) {
            found = false;
            free(m_realpath);
            m_realpath = nullptr;
            option.DEVICE_PATH = nullptr;
            break;
        }

        found = true;
        __builtin_strcat(device_path, pdirent->d_name);
        m_realpath = realpath(device_path, nullptr);
        assert( m_realpath );
        option.DEVICE_PATH = m_realpath;
    }

    if ( !found ) {
        if ( pdirent )
            l_error() << "multiple devices found for \"" << DROP_ALT << "\"\n";
        else
            l_error() << "no device found for \"" << DROP_ALT << "\"\n";
    }

    closedir(pdir);
}

serial::~serial()
{
    close();
    free(m_realpath);
}

void serial::close()
{
    if ( m_fd != -1 ) {
        ::close(m_fd);
        m_fd = -1;
    }
}

status_t serial::open()
{
    // If it's already open, reuse it.
    if ( m_fd != -1 )
        return LUA_OK;

    if ( option.DEVICE_PATH == nullptr )
        return LUA_ERRFATAL;

    // Open the port, attempting repeatedly if NO_RECONNECT is false.
    while ( (m_fd = ::open(option.DEVICE_PATH, O_RDWR | O_NOCTTY)) == -1 ) {
        if ( unlikely(option.NO_RECONNECT) ) {
            l_error() << strerror(errno) << ": " << option.DEVICE_PATH << '\n';
            return LUA_ERRFATAL;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_PERIOD_MS));
        // usleep(option.RECONNECT_PERIOD_MS * 1000);  // to microseconds
    }

    if ( flock(m_fd, LOCK_EX | LOCK_NB) == 0 ) {
        termios orig_ios;
        if ( tcgetattr(m_fd, &orig_ios) == 0 ) {
            // Configure the serial port for canonical input mode (i.e. line-buffered
            // input mode) with all ECHOs disabled, which is to be done before the
            // initial read(). In this mode, input is provided line by line, where a line
            // becomes available upon receiving a line delimiter (NL, EOL, EOL2, or EOF
            // if received at the start of the line).
            // See Canonical Input Processing:
            // - https://tldp.org/HOWTO/Serial-Programming-HOWTO/x115.html
            // - https://manual.cs50.io/3/cfmakeraw

            termios ios = orig_ios;
            // ios.c_iflag &= ~(IXON | IXOFF | IXANY);  // Disable software flow control.
            ios.c_iflag = 0;       // Disable all input processing; e.g. XON/XOFF/ICRNL.
            ios.c_oflag = 0;       // Disable all output processing
            ios.c_lflag = ICANON;  // Enable canonical mode and disable all ECHOs.
            tcsetattr(m_fd, TCSANOW, &ios);

            // Linux opens a serial port with ECHO enabled by default, and there is no
            // kernel-level mechanism to bundle open() and tcsetattr() atomically. This
            // means any data received between those calls may be echoed back to the
            // device. While tcflush() helps clear the output buffer, the device should
            // avoid transmitting until we send a ping below to signal readiness.
            tcflush(m_fd, TCOFLUSH);

            // Send a ping to the remote Lua REPL and await a response.
            const ping ping(option.LOG_MASK);
            const bool display_logs = option.LOG_MASK & ~0x01;
            const bool display_welcome = option.LOG_MASK & 0x01;
            while ( ::write(m_fd, &ping, sizeof(ping)) == sizeof(ping) ) {
                status_t status = receive_status(display_logs, RESPONSE_TIMEOUT_MS);
                if ( status == LUA_OK ) {
                    if ( display_welcome )
                        std::cout << APP_VERSION << ": connected to "
                                  << option.DEVICE_PATH << '\n';
                    return LUA_OK;
                }
                if ( status != LUA_YIELD )
                    break;

                // DFU mode confirms that this is our device but the REPL is not ready
                // ready yet. Keep the established port open and try again shortly.
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(RESPONSE_TIMEOUT_MS));
            }

            // Restore the terminal attributes if it is not our serial port.
            tcsetattr(m_fd, TCSANOW, &orig_ios);
            l_error() << "no Lua running on " << option.DEVICE_PATH << '\n';
        } else
            l_error() << strerror(errno) << ": " << option.DEVICE_PATH << '\n';
    } else
        l_error() << "locked by another process: " << option.DEVICE_PATH << '\n';

    close();
    return LUA_ERRFATAL;
}

int serial::read_line(int timeout_ms)
{
    int result;

    if ( timeout_ms < 0 )
        // Optimization: poll() can be skipped.
        result = ::read(m_fd, m_buffer, sizeof(m_buffer));
    else {
        // Use poll() to wait for an input from the serial port.
        pollfd fds = { m_fd, POLLIN, 0 };
        result = poll(&fds, 1, timeout_ms);
        assert( result >= 0 );

        if ( result > 0 ) {
            // In canonical mode, read() reads a string up to the ending '\n'.
            result = likely(fds.revents & POLLIN)
                ? ::read(m_fd, m_buffer, sizeof(m_buffer)) : -1;
        }
    }

    if ( result < 0 )
        close();

    return result;
}

lua_Writer serial::writer()
{
    return [](lua_State*, const void* pdata, size_t sz, void* arg) {
        serial* that = static_cast<serial*>(arg);
        if ( ::write(that->m_fd, pdata, sz) == ssize_t(sz) )
            return LUA_OK;

        l_error() << strerror(errno) << ": " << option.DEVICE_PATH << '\n';
        that->close();
        return LUA_ERRIO;
    };
}
