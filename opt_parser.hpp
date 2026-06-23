#pragma once

#include <cassert>
#include <unordered_map>        // for std::unordered_map<>

#include "lua.hpp"              // for lua_State, status_t
#include "option.hpp"           // for option, l_error()



// A hand-crafted, tail-recursive command-line parser rather than using getopt().
template <class Opt>
class opt_parser {
public:
    using opt_f_map = std::unordered_map<char, status_t (*)(lua_State*, int, Opt&)>;
    using optarg_f_map = std::unordered_map<char, status_t (*)(lua_State*, int, const char*, Opt&)>;

    opt_parser(lua_State* L, opt_f_map opt_f, optarg_f_map optarg_f, Opt& option);

    // Parse the command line arguments; fill in m_option.
    status_t parse(const char* const* argv);

private:
    lua_State* const m_L;

    // Handlers for options without arguments. Key 0 handles a standalone "-".
    const opt_f_map m_opt_f;

    // Handlers for options requiring arguments. Key 0 handles argv[0] and bare arguments.
    const optarg_f_map m_optarg_f;

    // The option object filled in by the handlers during parsing.
    Opt& m_option;

    // The argv[] passed to parse().
    const char* const* m_argv = nullptr;

    // Index of the argv word currently being processed.
    int m_optind = 0;

    status_t parse_next();

    status_t parse_flags(const char* arg);
};



template <class Opt>
opt_parser<Opt>::opt_parser(lua_State* L, opt_f_map opt_f, optarg_f_map optarg_f, Opt& option)
: m_L(L), m_opt_f(std::move(opt_f)), m_optarg_f(std::move(optarg_f)), m_option(option)
{}  // No body.

template <class Opt>
status_t opt_parser<Opt>::parse(const char* const* argv)
{
    m_argv = argv;
    m_optind = 0;
    status_t status = LUA_OK;

    if ( auto it = m_optarg_f.find(0); it != m_optarg_f.end() )
        status = it->second(m_L, m_optind, m_argv[0], m_option);

    if ( status == LUA_OK )
        status = parse_next();

    if ( status == LUA_ERROPT )
        l_error() << "invalid option near \"" << m_argv[m_optind] << "\". run -h for help\n";

    return status;
}

template <class Opt>
status_t opt_parser<Opt>::parse_next()
{
    m_optind++;  // Advance to the next argv word.
    status_t status = LUA_ERROPT;

    if ( m_argv[m_optind] == nullptr )
        return LUA_OK;

    if ( *m_argv[m_optind] == '-' ) {
        const char* arg = m_argv[m_optind] + 1;  // Skip the leading '-'.

        // Delegate the current word to parse_flags().
        if ( *arg )
            status = parse_flags(arg);

        // Process a standalone "-" option at the end.
        else if ( m_argv[m_optind + 1] == nullptr ) {
            if ( auto it = m_opt_f.find(0); it != m_opt_f.end() )
                status = it->second(m_L, m_optind, m_option);
        }
        // Else: Standalone "-" must appear as the last argument.
    }

    // Handle non-option (bare) arguments � ones that don't start with '-'.
    else if ( auto it = m_optarg_f.find(0); it != m_optarg_f.end() )
        status = it->second(m_L, m_optind, m_argv[m_optind], m_option);

    // On success, recurse for the next word.
    return status == LUA_OK ? parse_next() : status;
}

template <class Opt>
status_t opt_parser<Opt>::parse_flags(const char* arg)
{
    assert( arg != nullptr );
    const char flag = *arg;
    if ( flag == 0 )
        return LUA_OK;  // End of flag word; parse_next() will advance to the next argv word.

    // Process options lacking an associated argument (e.g. -d).
    if ( auto it = m_opt_f.find(flag); it != m_opt_f.end() ) {
        status_t status = it->second(m_L, m_optind, m_option);
        return status == LUA_OK ? parse_flags(++arg) : status;
    }

    // Process options accompanied by an argument (e.g. -e).
    if ( auto it = m_optarg_f.find(flag); it != m_optarg_f.end() ) {
        if ( *++arg == 0 ) {
            arg = m_argv[m_optind + 1];
            if ( arg == nullptr )
                return LUA_ERROPT;  // Option lacking its associated option argument.
            m_optind++;  // Advance past the flag word to the argument word.
        }

        return it->second(m_L, m_optind, arg, m_option);
    }

    return LUA_ERROPT;  // Unknown option character.
}
