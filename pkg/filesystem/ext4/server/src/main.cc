/*
 * ext4fs server for TuringOS – Phase 3b
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Boot order:
 *   io (virt-blk.io) → virtio-block-driver ("svr" cap) → ext4fs ("blk" cap)
 *
 * This server:
 *   1. Connects to virtio-block-driver via the "blk" factory cap.
 *   2. Formats the disk with ext4 if no superblock is present.
 *   3. Mounts the filesystem at "/".
 *   4. Runs a quick write/read self-test (creates /hello.txt).
 *   5. Registers an L4Re::Namespace server on the "svr" capability so other
 *      tasks can open ext4 files via standard POSIX fopen()/fread().
 *   6. Enters the IPC dispatch loop (filesystem stays mounted).
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/l4virtio/l4virtio>
#include <l4/re/util/br_manager>
#include <l4/re/util/object_registry>

extern "C" {
#include <ext4_errno.h>
#include <ext4.h>
#include <ext4_mkfs.h>
}

#include "virtio_blockdev.h"
#include "ext4_ns.h"

static const char *const DEV_NAME    = "virtio-blk0";
static const char *const MOUNT_POINT = "/";

// ----- Helpers -----

static bool has_valid_superblock(struct ext4_blockdev *bd)
{
    struct ext4_mkfs_info info;
    return ext4_mkfs_read_info(bd, &info) == EOK;
}

static int format_disk(struct ext4_blockdev *bd)
{
    printf("[ext4] No superblock found – formatting with ext4\n");

    struct ext4_fs        fs;
    struct ext4_mkfs_info info;
    memset(&info, 0, sizeof(info));
    info.block_size = 1024;   // 1 KB blocks (safe for 100 MiB disk)
    info.journal    = false;  // skip journal for simplicity

    int r = ext4_mkfs(&fs, bd, &info, F_SET_EXT4);
    if (r != EOK)
        printf("[ext4] mkfs failed: %d\n", r);
    else
        printf("[ext4] Format complete\n");
    return r;
}

static int run_io_test()
{
    const char *path = "/hello.txt";
    const char *msg  = "Hello from TuringOS ext4fs!\n";

    // Write
    ext4_file f;
    int r = ext4_fopen(&f, path, "wb");
    if (r != EOK) { printf("[ext4] fopen wb: %d\n", r); return r; }

    size_t written = 0;
    r = ext4_fwrite(&f, msg, strlen(msg), &written);
    ext4_fclose(&f);
    if (r != EOK || written != strlen(msg))
    {
        printf("[ext4] fwrite: r=%d written=%zu\n", r, written);
        return r ? r : -1;
    }

    // Read back and verify
    r = ext4_fopen(&f, path, "rb");
    if (r != EOK) { printf("[ext4] fopen rb: %d\n", r); return r; }

    char buf[64] = {};
    size_t rd = 0;
    r = ext4_fread(&f, buf, sizeof(buf) - 1, &rd);
    ext4_fclose(&f);
    if (r != EOK) { printf("[ext4] fread: %d\n", r); return r; }

    buf[rd] = '\0';
    if (strcmp(buf, msg) != 0)
    {
        printf("[ext4] data mismatch!\n  wrote: '%s'\n  read:  '%s'\n", msg, buf);
        return -1;
    }

    printf("[ext4] I/O test PASSED  (\"%.*s\")\n", (int)(rd - 1), buf);
    return 0;
}

// ----- Server -----

static L4Re::Util::Registry_server<L4Re::Util::Br_manager_hooks> server;
static Ext4_namespace ext4_ns;

// ----- main -----

int main(int /*argc*/, char const *const * /*argv*/)
{
    printf("[ext4] ext4fs server starting\n");

    // Get the pre-allocated VirtIO device cap (created by Ned via block driver factory).
    auto vio_cap = L4Re::Env::env()->get_cap<L4virtio::Device>("blk");
    if (!vio_cap.is_valid())
    {
        printf("[ext4] ERROR: 'blk' capability not found\n");
        return 1;
    }

    // Set up the lwext4 block device using the VirtIO device cap.
    try
    {
        virtio_blockdev_init(vio_cap);
    }
    catch (L4::Runtime_error const &e)
    {
        printf("[ext4] block device init failed: %s\n", e.str());
        return 1;
    }

    struct ext4_blockdev *bd = virtio_blockdev_get();

    // Register the block device with lwext4.
    int r = ext4_device_register(bd, DEV_NAME);
    if (r != EOK)
    {
        printf("[ext4] ext4_device_register: %d\n", r);
        return 1;
    }

    // Auto-format on first boot (no valid superblock).
    if (!has_valid_superblock(bd))
    {
        r = format_disk(bd);
        if (r != EOK)
            return 1;
    }

    // Mount.
    r = ext4_mount(DEV_NAME, MOUNT_POINT, false);
    if (r != EOK)
    {
        printf("[ext4] mount failed: %d\n", r);
        return 1;
    }
    printf("[ext4] Mounted %s at %s\n", DEV_NAME, MOUNT_POINT);

    // Run a quick write/read self-test (creates /hello.txt for clients to read).
    r = run_io_test();
    if (r != 0)
        printf("[ext4] WARNING: I/O self-test failed (%d) – continuing anyway\n", r);

    // Register the Namespace server on the "svr" capability so that
    // Ned can hand it out to client tasks as their "ext4" cap.
    auto svr_cap = server.registry()->register_obj(&ext4_ns, "svr");
    if (!svr_cap.is_valid())
    {
        printf("[ext4] WARNING: 'svr' cap not found – no namespace clients\n");
        // Fall through: still useful as a standalone mount.
    }
    else
    {
        printf("[ext4] Namespace server ready on 'svr' cap\n");
    }

    printf("[ext4] Entering server loop (filesystem stays mounted)\n");
    server.loop();

    // Not reached.
    return 0;
}
