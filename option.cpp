#include <cassert>

#include "option.hpp"



const char* option::PROGNAME = nullptr;
const char* option::DEVICE_PATH = nullptr;
bool option::NO_RECONNECT = false;
uint8_t option::LOG_MASK = 0x01;  // Disable all thread logs except the REPL welcome message.
bool option::interactive = true;



option::option(lua_State* L, opt_f_map opt_f, optarg_f_map optarg_f)
: m_L(L), m_opt_c(0), m_opt_f(std::move(opt_f)), m_optarg_f(std::move(optarg_f))
{}  // No body.

#ifdef _WIN32
static constexpr char SEPARATOR = '\\';
#else
static constexpr char SEPARATOR = '/';
#endif

status_t option::parse_argv(const char* const* argv)
{
    const char* progname = __builtin_strrchr(*argv, SEPARATOR);
    PROGNAME = progname ? progname + 1 : *argv;

    // Only one device path is allowed as the first argument.
    if ( *++argv && **argv != '-' )
        DEVICE_PATH = *argv++;

    status_t status = parse_args(argv);
    if ( status == LUA_ERROPT )
        l_error() << "invalid option near '" << m_opt_c << "'. run -h for help\n";

    return status;
}

status_t option::parse_args(const char* const* args)
{
    if ( *args == nullptr )
        return LUA_OK;

    const char* arg = *args++;
    m_opt_c = *arg++;
    if ( m_opt_c == '-' ) {
        if ( *arg )
            return parse_chars(arg, args);

        if ( *args == nullptr ) {
            auto it = m_opt_f.find(0);
            assert( it != m_opt_f.end() );
            return it->second(m_L);
        }
        // Else: Standalone "-" must appear as the last argument.
    }
    // Else: Non-option argument is allowed only at argv[1].

    return LUA_ERROPT;
}

status_t option::parse_chars(const char* arg, const char* const* next_args)
{
    assert( arg != nullptr );
    m_opt_c = *arg;
    if ( m_opt_c == 0 )
        return parse_args(next_args);

    // Process options lacking an associated argument (e.g. -d).
    if ( auto it = m_opt_f.find(m_opt_c); it != m_opt_f.end() ) {
        status_t status = it->second(m_L);
        return status == LUA_OK ? parse_chars(++arg, next_args) : status;
    }

    // Process options accompanied by an argument (e.g. -e).
    if ( auto it = m_optarg_f.find(m_opt_c); it != m_optarg_f.end() ) {
        if ( *++arg == 0 ) {
            arg = *next_args++;
            if ( arg == nullptr )
                return LUA_ERROPT;  // Option lacking its associated option argument.
        }

        status_t status = it->second(m_L, arg);
        return status == LUA_OK ? parse_args(next_args) : status;
    }

    return LUA_ERROPT;  // Unknown option character.
}
