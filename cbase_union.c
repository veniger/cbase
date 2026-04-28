#include "cbase.h"

/* Pin the cbase/user info-code partition at compile time. cbase reserves
 * codes below 0x1000; user code (e.g. explorers) starts at 0x1000. The
 * sentinel CB_INFO__LAST is the last enumerator in cb_info_t, so this
 * static assert fires the moment a new cbase code would cross the line. */
#define CB_INFO__USER_BASE 0x1000
_Static_assert(CB_INFO__LAST < CB_INFO__USER_BASE,
    "cbase info codes must fit below CB_INFO__USER_BASE so the user (explorers) namespace stays clean");

#include "cbase_arena.c"
#include "cbase_threading.c"
#include "cbase_network.c"
#include "cbase_fixed.c"
#include "cbase_random.c"
#include "cbase_bytes.c"
#include "cbase_time.c"
#include "cbase_config.c"
#include "cbase_log.c"
#include "cbase_hash.c"
#include "cbase_netsim.c"
#include "cbase_transport.c"
