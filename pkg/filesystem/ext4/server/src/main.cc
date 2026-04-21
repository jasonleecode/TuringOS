/*
 * ext4fs server for L4Re/TuringOS
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <l4/re/env>
#include <l4/re/util/cap_alloc>
#include <l4/sys/capability>
#include <l4/sys/thread>

#include "ext4_server.h"
#include "ext4_blockdev.h"

int main(int argc, char const* const* argv)
{
    printf("ext4fs server starting...\n");

    // Initialize L4Re environment
    auto *env = L4Re::Env::env();

    // Initialize ext4 block device
    if (ext4_blockdev_init() != 0) {
        fprintf(stderr, "Failed to initialize ext4 block device\n");
        return 1;
    }

    // Create ext4 server
    Ext4_server server;

    // Register server capability
    auto fs_cap = L4Re::Util::cap_alloc.alloc<L4Re::Dataspace>();
    if (!fs_cap.is_valid()) {
        fprintf(stderr, "Failed to allocate filesystem capability\n");
        return 1;
    }

    // Start server loop
    printf("ext4fs server running\n");
    server.run();

    printf("ext4fs server exiting\n");
    return 0;
}