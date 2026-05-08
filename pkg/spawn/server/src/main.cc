/*
 * spawnd — TuringOS program loader server
 *
 * Receives spawn/wait/kill requests from native_shell (and any other client)
 * via IPC, loads ELF binaries, and manages child task lifecycles.
 *
 * Startup order: after ext4fs and syslogd, before native_shell.
 */

#include <cstdio>

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/br_manager>
#include <l4/re/util/object_registry>
#include <l4/sys/cxx/ipc_epiface>
#include <l4/re/l4aux.h>

#include "spawnd.h"

/* l4re_aux must be a global; Spawn_model_base::local_kip_ds() references it
 * (same requirement as ned and native_shell). */
l4re_aux_t const *l4re_aux = nullptr;

static L4Re::Util::Registry_server<L4Re::Util::Br_manager_hooks> server;
static Spawnd spawnd_obj;

int main(int argc, char const *const *argv)
{
    /* ---- Parse AUX vector to initialise l4re_aux ---- */
    auto auxp = reinterpret_cast<char const *const *>(&argv[argc] + 1);
    while (*auxp) ++auxp;   /* skip envp entries */
    ++auxp;                  /* skip envp terminator */

    auto *sentinel = reinterpret_cast<char const *>(0xf0); /* L4_AUX_TYPE_KINFO */
    while (*auxp) {
        if (*auxp == sentinel)
            l4re_aux = reinterpret_cast<l4re_aux_t const *>(auxp[1]);
        auxp += 2;
    }

    printf("[spawnd] starting\n");

    auto svr_cap = server.registry()->register_obj(&spawnd_obj, "svr");
    if (!svr_cap.is_valid()) {
        printf("[spawnd] ERROR: 'svr' capability not found — exiting\n");
        return 1;
    }

    printf("[spawnd] ready\n");
    server.loop();
    return 0;
}
