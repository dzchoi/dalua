// Todo: Support Windows or Mingw.

#include <assert.h>
#include <unistd.h>             // for STDIN_FILENO, isatty()

#include "dalua.h"
#include "lua.h"
#include "lauxlib.h"
// #include "lualib.h"



// Program name for this application.
const char* const PROGNAME = "dalua";

// Options configured for dalua.
const char* DEVICE_PATH = NULL;
bool NO_RECONNECT = false;
uint8_t LOG_MASK = 0x01;  // Disable all thread logs but show the REPL welcome message.

// Globals shared with the prasers.
static lua_State* L;
static bool interactive = true;
static status_t status = LUA_OK;



// A hand-crafted, tail-recursive command-line parser rather than using getopt().
// Note that each parser function returns 0 upon successful parsing or the failing option
// character otherwise.

char parse_opt_arg(char opt, const char* arg)
{
    assert( arg != NULL );

    LOG_MASK &= ~0x01;  // Do not show the REPL welcome message.
    interactive = false;  // No interactive mode unless specified explicitly.

    switch ( opt ) {
    case 'e':
        status = do_string(L, arg);
        break;

    case 'f':
        status = do_file(L, arg);
        break;

    default:
        assert( false );  // This should have been handled in parse_chars().
    }

    return 0;
}

char parse_chars(const char* arg, const char* const* next_args)
{
    char parse_args(const char* const* args);
    assert( arg != NULL );

    char opt = *arg;
    switch ( opt ) {
    case 0:
        return parse_args(next_args);

    case 'h':
        interactive = false;
        printf(
            "Usage: %s [<tty-device>] [<options>]\n"
            "\n"
            "Options:\n"
            "  -d\t\t\tstream log messages\n"
            "  -e '<lua_code>'\texecute the inline code remotely\n"
            "  -f <script-file>\texecute the script file remotely\n"
            "  -h\t\t\tdisplay this help message\n"
            "  -n\t\t\tdo not reconnect\n"
            "  - (at the end)\teventually execute REPL remotely\n",
            PROGNAME );
        return 0;

    case 'd':
        LOG_MASK = 0xff;
        return parse_chars(++arg, next_args);

    case 'n':
        NO_RECONNECT = true;
        return parse_chars(++arg, next_args);

    case 'e':
    case 'f':
        if ( *++arg == 0 ) {
            arg = *next_args++;
            if ( arg == NULL )
                break;  // Option lacking its associated option argument
        }

        opt = parse_opt_arg(opt, arg);
        if ( opt == 0 && status == LUA_OK )
            return parse_args(next_args);
        // Else: option error or status != LUA_OK from parse_opt_arg()
    }

    return opt;  // Unknown option character.
}

char parse_args(const char* const* args)
{
    if ( *args == NULL )
        return 0;

    const char* arg = *args++;
    char opt = *arg++;
    if ( opt == '-' ) {
        if ( *arg )
            return parse_chars(arg, args);
        if ( *args == NULL ) {
            interactive = true;  // Overide the previous interactive.
            opt = 0;
        }
        // Else: "-" is not the final argument.
    }
    // Else: Non-option argument is allowed only in argv[1].

    return opt;
}

void parse_command(const char* const* argv)
{
    // Only one device path is allowed as the first argument.
    if ( *argv && **argv != '-' )
        DEVICE_PATH = *argv++;

    char opt = parse_args((const char* const*)argv);
    if ( opt ) {
        status = LUA_ERROPT;
        l_message("invalid option near '%c'. run -h for help", opt);
    }
}



status_t main(int, char** argv)
{
    L = luaL_newstate();
    if ( L == NULL ) {
        l_message("cannot create Lua environment: not enough memory");
        return LUA_ERRMEM;
    }

    // We don't need any standard libraries, not even the base library (lbaselib).
    // luaL_openlibs(L);  // Open all standard libraries.

    // Note that we do not validate LUA_VERSION_NUM and LUAL_NUMSIZES against the Lua
    // interpreter on the remote device. These details are included as part of the
    // bytecode header, and the device will verify them remotely for each bytecode it
    // receives.

    parse_command((const char* const*)(argv + 1));  // Skip argv[0]

    if ( status == LUA_OK && interactive )
        // If stdin is not `tty` (i.e. redirecting from a file), process it as a file.
        status = isatty(STDIN_FILENO) ? do_repl(L) : do_file(L, NULL);

    lua_close(L);
    return status;
}
