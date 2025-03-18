// Todo: Support Windows or mingw.

#include <stdbool.h>            // for bool
#include <string.h>             // for strcmp()
#include <unistd.h>             // for STDIN_FILENO, isatty()

#include "dalua.h"
#include "lua.h"
#include "lauxlib.h"
// #include "lualib.h"



const char* PROGNAME = "dalua";

// static status_t setup_serial_once(bool verbose)
// {
//     static status_t status = LUA_ERRIO;
//     if ( status != LUA_OK )
//         status = setup_serial(verbose);
//     return status;
// }

// We execute lua_* functions in unprotected mode, since we are only using some stack
// manipulation functions, luaL_loadbuffer() and lua_dump(), which basically do not
// throw an exception other than not enough memory.

status_t main(int argc, char* argv[])
{
    // Determine the interactive mode early.
    bool interactive = (argc <= 1);
    if ( !interactive && strcmp(argv[argc-1], "-") == 0 ) {
        argc--;
        interactive = true;
    }

    status_t status = setup_serial(interactive);
    if ( status != LUA_OK )
        return status;

    lua_State* L = luaL_newstate();
    if ( L == NULL ) {
        l_message("cannot create state: not enough memory");
        return LUA_ERRMEM;
    }

    // We don't need any standard libraries, not even the base library (lbaselib).
    // luaL_openlibs(L);  // Open all standard libraries.

    // Note that we do not validate LUA_VERSION_NUM and LUAL_NUMSIZES against the Lua
    // interpreter on the remote device. These details are included as part of the
    // bytecode header, and the device will verify them remotely for each bytecode it
    // receives.

    // Simple command-line parser
    for ( int i = 1 ; status == LUA_OK && i < argc ; i++ ) {
        const char* const arg = argv[i];
        if ( arg[0] == '-' )
            switch ( arg[1] ) {
            case 'f':
                if ( arg[2] != '\0' )
                    status = do_file(L, &arg[2]);
                else if ( ++i < argc )
                    status = do_file(L, argv[i]);
                else  // fall through

            default: {  // Handle option errors and print usage.
                    status = LUA_ERROPT;
                    l_message("invalid option: %.2s", arg);
                    printf(
                        "Usage: %s [option|'<lua_code>']... [-]\n"
                        "\n"
                        "  -f <script-file>\texecute the script file remotely\n"
                        "  '<lua_code>'\t\texecute the inline code remotely\n"
                        "  - (at the end)\teventually execute REPL remotely\n",
                        PROGNAME );
                }
                break;
            }

        else
            status = do_string(L, arg);
    }

    if ( status == LUA_OK && interactive )
        // If stdin is not `tty` (i.e. redirecting from a file), process it as a file.
        status = isatty(STDIN_FILENO) ? do_repl(L) : do_file(L, NULL);

    lua_close(L);
    return status;
}
