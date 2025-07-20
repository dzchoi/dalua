#include <errno.h>              // for errno
#include <stddef.h>             // for offsetof(), size_t
#include <stdint.h>             // for uint32_t, uint16_t
#include <stdlib.h>             // for malloc(), free()
#include <string.h>             // for strerror()
#include <sys/stat.h>           // for stat(), ...
#include <time.h>               // for time_t
#include <unistd.h>             // for write(), STDOUT_FILENO

#include "checksum/fletcher32.h"  // for fletcher32()
#include "dacomp.h"
#include "lua.h"
#include "lauxlib.h"
// #include "lualib.h"



// Program name for this application.
const char* const PROGNAME = "daluac";

#define RIOTBOOT_HDR_LEN    1024            // for Cortex-M
static const uint32_t RIOTBOOT_MAGIC = 0x544f4952;  // "RIOT"

// #define RIOTBOOT_LEN        (16 *1024)      // for Cortex-M
// #define SLOT1_OFFSET        (RIOTBOOT_LEN + SLOT0_LEN)
// static const uint32_t SLOT1_IMAGE_OFFSET = SLOT1_OFFSET + RIOTBOOT_HDR_LEN;

// Note: In real firmware, riotboot_hdr_t.start_addr is used to mark the code's
// starting address. However, for Lua bytecode, we can repurpose it to denote the
// bytecode size, without losing the validity of the slot header.

typedef struct {
    uint32_t magic_number;      // Header magic number (always "RIOT")
    uint32_t version;           // Integer representing the partition version
    uint32_t start_addr;        // Address after the allocated space for the header
    uint32_t chksum;            // Checksum of riotboot_hdr
} riotboot_hdr_t;

// Output the 1KB RIOT slot header to stdout.
// ( -- )
status_t dump_hdr(lua_State*, uint32_t mtime, uint32_t codesize)
{
    static uint8_t hdr_buf[RIOTBOOT_HDR_LEN];

    riotboot_hdr_t* const hdr = (riotboot_hdr_t*)hdr_buf;

    // Generate image header
    hdr->magic_number = RIOTBOOT_MAGIC;
    hdr->version = mtime;        // Latest script modification time as the version.
    hdr->start_addr = codesize;  // Size of the bytecode.

    // Calculate header checksum
    hdr->chksum = fletcher32(
        (uint16_t*)hdr, offsetof(riotboot_hdr_t, chksum) / sizeof(uint16_t));

    if ( write(STDOUT_FILENO, hdr_buf, sizeof(hdr_buf)) != (ssize_t)sizeof(hdr_buf) ) {
        l_error("%s: stdout", strerror(errno));
        return LUA_ERRFILE;
    }

    return LUA_OK;
}

status_t main(int argc, char* argv[])
{
    if ( argc < 2 ) {
        printf("Usage: %s <script-file>...\n"
             "\tCompiles the script files (to stdout).\n", PROGNAME);
        return LUA_ERROPT;
    }

    time_t latest_mtime = 0;  // January 1, 1970, 00:00:00 UTC
    for ( int i = 1 ; i < argc ; i++ ) {
        struct stat st;
        if ( stat(argv[i], &st) != 0 ) {
            l_error("%s: %s", strerror(errno), argv[i]);
            return LUA_ERRFILE;
        }

        if ( st.st_mtime > latest_mtime )
            latest_mtime = st.st_mtime;
    }

    lua_State* L = luaL_newstate();
    if ( L == NULL ) {
        l_error("cannot create Lua environment: not enough memory");
        return LUA_ERRMEM;
    }

    // Allocate an array for storing the compiled bytecode.
    static const size_t INITIAL_CAPACITY = 32;  // Allocate 32 bytes initially.
    array_t array = {
        .memory = malloc(INITIAL_CAPACITY),
        .size = 0,
        .capacity = INITIAL_CAPACITY
    };
    if ( !array.memory ) {
        l_error("cannot create buffer: not enough memory");
        return LUA_ERRMEM;
    }

    status_t status = compile_files(L, (const char**)(argv + 1), &array);
    if ( status == LUA_OK ) {
        status = dump_hdr(L, latest_mtime, array.size);
        if ( status == LUA_OK ) {
            if ( write(STDOUT_FILENO, array.memory, array.size)
                  != (ssize_t)array.size ) {
                l_error("%s: stdout", strerror(errno));
                status = LUA_ERRFILE;
            }
        }
    }

    free(array.memory);
    lua_close(L);
    return status;
}
