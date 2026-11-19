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
#include <cstring>
#include <initializer_list>

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--caps")) {
        auto env = L4Re::Env::env();
        for (auto name : {"netd", "data", "ext4", "tmp", "authd"})
            printf("%s=%d ",name,env->get_cap<void>(name).is_valid());
        puts("");
        if (argc == 3) {
            FILE *f = fopen(argv[2],"wb");
            if (!f) return 1;
            bool ok = fputs("application data\n",f) >= 0;
            return fclose(f) || !ok;
        }
        return 0;
    }
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
