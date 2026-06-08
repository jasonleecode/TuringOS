/*
 * exectest — demonstrates spawnd exec (replace-behind-handle) semantics.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Spawned via `run exectest`.  Prints a marker, then calls Spawn_svr::exec to
 * replace its own program image with rom/hello — under the SAME spawnd handle.
 * If exec works, this process becomes hello (which prints its own output and
 * exits), and the shell's wait on the handle returns hello's exit code.
 * exec only returns on failure.
 */

#include <l4/re/env>
#include <spawn_ipc.h>

#include <cstdio>
#include <cstdlib>

int main(int /*argc*/, char ** /*argv*/)
{
    const char *h_str  = getenv("SPAWND_HANDLE");
    unsigned    handle = h_str ? (unsigned)atoi(h_str) : 0;

    printf("exectest: before exec (handle=%u)\n", handle);
    fflush(stdout);

    auto spawnd = L4Re::Env::env()->get_cap<Spawn_svr>("spawnd");
    if (!spawnd.is_valid()) {
        printf("exectest: no 'spawnd' cap — cannot exec\n");
        return 1;
    }
    if (!h_str) {
        printf("exectest: no SPAWND_HANDLE — cannot exec\n");
        return 1;
    }

    /* Replace ourselves with rom/hello.  argv packed NUL-separated as spawn(). */
    static const char new_path[] = "rom/hello";
    static const char new_args[] = "hello";
    long r = spawnd->exec(handle,
                          L4::Ipc::Array<char const>(sizeof(new_path), new_path),
                          L4::Ipc::Array<char const>(sizeof(new_args), new_args));

    /* On success exec does not return — this task is destroyed. */
    printf("exectest: exec failed (%ld)\n", r);
    return 1;
}
