#include <unistd.h>             // for STDIN_FILENO, isatty()

#include "darepl.hpp"           // for lua::repl::do_*()
#include "lua.hpp"              // for luaL_*()
#include "opt_parser.hpp"       // for opt_parser
#include "option.hpp"           // for option, l_error()



int main(int, char* argv[])
{
    lua_State* L = luaL_newstate();
    assert((L != nullptr) && "cannot create Lua environment: not enough memory");

    // We don't need any standard libraries, not even the base library (lbaselib).
    // luaL_openlibs(L);  // Open all standard libraries.

    // Note that we do not validate LUA_VERSION_NUM and LUAL_NUMSIZES against the Lua
    // interpreter on the remote device. These details are included as part of the
    // bytecode header, and the device will verify them remotely for each bytecode it
    // receives.

    status_t status = opt_parser<option_t>(L,
    {
        // Standalone "-" appearing as the last argument.
        { 0, [](lua_State*, int, option_t& option) -> status_t {
            option.interactive = true;  // Overide the previous interactive.
            return LUA_OK;
        }},

        { 'd', [](lua_State*, int, option_t& option) -> status_t {
            option.LOG_MASK = 0xff;
            return LUA_OK;
        }},

        { 'n', [](lua_State*, int, option_t& option) -> status_t {
            option.NO_RECONNECT = true;
            return LUA_OK;
        }},

        { 'h', [](lua_State*, int, option_t& option) -> status_t {
            option.interactive = false;
            std::cout <<
                "Usage: " << option.PROGNAME << " [<tty-device>] [<options>]\n"
                "\n"
                "Options:\n"
                "  -d\t\t\tstream log messages (debug mode)\n"
                "  -e '<lua_code>'\texecute the inline code remotely\n"
                "  -f <script-file>\texecute the script file remotely\n"
                "  -h\t\t\tdisplay this help message\n"
                "  -n\t\t\tdo not reconnect\n"
                "  - (at the end)\teventually execute REPL remotely\n";
            // Though this is a simple request for a help screen, it's treated as an error
            // not being able to execute Lua REPL.
            return LUA_ERRFATAL;
        }},
    },
    {
        { 0, [](lua_State*, int optind, const char* arg, option_t& option) -> status_t {
#ifdef _WIN32
            constexpr char SEPARATOR = '\\';
#else
            constexpr char SEPARATOR = '/';
#endif
            if ( optind == 0 ) {
                const char* progname = __builtin_strrchr(arg, SEPARATOR);
                option.PROGNAME = progname ? progname + 1 : arg;
                return LUA_OK;
            }
            else if ( optind == 1 ) {
                // Only one device path is allowed as the first argument.
                option.DEVICE_PATH = arg;
                return LUA_OK;
            }

            return LUA_ERROPT;
        }},

        { 'e', [](lua_State* L, int, const char* arg, option_t& option) -> status_t {
            // It is assured that arg != nullptr.
            option.interactive = false;  // No interactive mode unless noted explicitly.
            option.LOG_MASK &= ~0x01;    // Do not show the REPL welcome message.
            return lua::repl::do_string(L, arg);
        }},

        { 'f', [](lua_State* L, int, const char* arg, option_t& option) -> status_t {
            option.interactive = false;
            option.LOG_MASK &= ~0x01;
            return lua::repl::do_file(L, arg);
        }},
    },
    option  // `option_t option` from option.hpp
    ).parse(argv);

    if ( status == LUA_OK && option.interactive ) {
        // If stdin is not `tty` (i.e. redirecting from a file), process it as a file.
        status = isatty(STDIN_FILENO)
            ? lua::repl::do_repl(L) : lua::repl::do_file(L, nullptr);
    }

    lua_close(L);
    return status;  // `$?` can convert any negative values to 127 (on Windows).
}
