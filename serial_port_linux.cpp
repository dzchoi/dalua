#include <cassert>
#include <chrono>               // for std::chrono::...
#include <cstdlib>              // for realpath(), free()
#include <cstring>              // for strerror()
#include <dirent.h>             // for opendir(), readdir(), closedir()
#include <fcntl.h>              // for open(), O_RDWR, O_NOCTTY
#include <poll.h>               // for poll()
#include <sys/file.h>           // for flock()
#include <termios.h>            // for tcgetattr(), tcsetattr()
#include <thread>               // for std::this_thread::sleep_for()
#include <unistd.h>             // for read(), write(), close(), STDIN_FILENO

#include "option.hpp"           // for option::..., l_error()
#include "serial_port.hpp"



// Resolve option::DEVICE_PATH if it is currently undefined.
serial_port::serial_port()
: m_fd(-1)
{
    if ( option::DEVICE_PATH )
        return;

    constexpr char DEVICE_DIR[] = "/dev/serial/by-id/";

    DIR* pdir = opendir(DEVICE_DIR);
    if ( pdir == nullptr ) {
        l_error() << strerror(errno) << ": " << option::DEVICE_PATH << '\n';
        return;
    }

    dirent* pdirent;
    static char device_path[sizeof(pdirent->d_name) + __builtin_strlen(DEVICE_DIR)];
    __builtin_strcpy(device_path, DEVICE_DIR);

    bool found = false;
    while ( (pdirent = readdir(pdir)) != nullptr ) {
        if ( __builtin_strstr(pdirent->d_name, DROP_ALT) == nullptr )
            continue;

        if ( found ) {
            found = false;
            option::DEVICE_PATH = nullptr;
            break;
        }

        found = true;
        __builtin_strcat(device_path, pdirent->d_name);
        option::DEVICE_PATH = device_path;
    }

    if ( !found ) {
        if ( pdirent )
            l_error() << "multiple devices found for \"" << DROP_ALT << "\"\n";
        else
            l_error() << "no device found for \"" << DROP_ALT << "\"\n";
    }

    closedir(pdir);
}

serial_port::~serial_port()
{
    close();
}

void serial_port::close()
{
    if ( m_fd != -1 ) {
        ::close(m_fd);
        m_fd = -1;
    }
}

status_t serial_port::open()
{
    // If it's already open, reuse it.
    if ( m_fd != -1 )
        return LUA_OK;

    if ( option::DEVICE_PATH == nullptr )
        return LUA_ERRFATAL;

    // Open the port, attempting repeatedly if NO_RECONNECT is false.
    while ( (m_fd = ::open(option::DEVICE_PATH, O_RDWR | O_NOCTTY)) == -1 ) {
        if ( unlikely(option::NO_RECONNECT) ) {
            l_error() << strerror(errno) << ": " << option::DEVICE_PATH << '\n';
            return LUA_ERRFATAL;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_PERIOD_MS));
        // usleep(option::RECONNECT_PERIOD_MS * 1000);  // to microseconds
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

            // Send a ping to the remote Lua REPL and await a response.
            const ping ping(option::LOG_MASK);
            const bool display_logs = option::LOG_MASK & ~0x01;
            const bool display_welcome = option::LOG_MASK & 0x01;
            if ( ::write(m_fd, &ping, sizeof(ping)) == sizeof(ping) ) {
                if ( receive_status(display_logs, RESPONSE_TIMEOUT_MS) == LUA_OK ) {
                    if ( display_welcome ) {
                        // Display the resolved path if DEVICE_PATH is a symbolic link.
                        char* resolved_path = realpath(option::DEVICE_PATH, nullptr);
                        std::cout << "Connected to " << resolved_path << '\n';
                        free(resolved_path);
                    }
                    return LUA_OK;
                }
            }

            // Restore the terminal attributes if it is not our serial port.
            tcsetattr(m_fd, TCSANOW, &orig_ios);
            l_error() << "no Lua running on " << option::DEVICE_PATH << '\n';
        } else
            l_error() << strerror(errno) << ": " << option::DEVICE_PATH << '\n';
    } else
        l_error() << "locked by another process: " << option::DEVICE_PATH << '\n';

    close();
    return LUA_ERRFATAL;
}

int serial_port::read_line(int timeout_ms)
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

status_t serial_port::receive_status(bool verbose, int timeout_ms)
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

bool serial_port::print_line()
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

status_t serial_port::_writer(lua_State*, const void* pdata, size_t sz, void* arg)
{
    serial_port* that = static_cast<serial_port*>(arg);
    if ( ::write(that->m_fd, pdata, sz) == ssize_t(sz) )
        return LUA_OK;

    l_error() << strerror(errno) << ": " << option::DEVICE_PATH << '\n';
    that->close();
    return LUA_ERRIO;
}
