/*
 * VirtIO-backed lwext4 block device for TuringOS
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 */
#pragma once

#ifdef __cplusplus
#include <l4/l4virtio/l4virtio>

/**
 * Set up the lwext4 block device using an already-created L4virtio::Device cap.
 * Ned allocates the cap via block driver factory before starting this task.
 * Must be called once before virtio_blockdev_get().
 *
 * \param vio_cap  L4virtio::Device capability (from the "blk" environment cap).
 * \throws L4::Runtime_error on failure.
 */
int virtio_blockdev_init(L4::Cap<L4virtio::Device> vio_cap);

extern "C" {
#endif

struct ext4_blockdev;

/** Return pointer to the initialised lwext4 block device. */
struct ext4_blockdev *virtio_blockdev_get(void);

#ifdef __cplusplus
}
#endif
