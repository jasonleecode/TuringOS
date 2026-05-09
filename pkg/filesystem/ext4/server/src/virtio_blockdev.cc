/*
 * VirtIO-backed lwext4 block device for TuringOS
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Wraps L4virtio::Driver::Block_device to provide the ext4_blockdev_iface
 * callbacks expected by lwext4.  I/O is fully synchronous (process_request
 * blocks until the device completes each request).
 *
 * The L4virtio::Device capability is pre-allocated by Ned (virt-blk.cfg)
 * via the block driver's factory interface before this task starts, so no
 * factory create is needed here.
 */

#include "virtio_blockdev.h"

#include <l4/l4virtio/client/virtio-block>
#include <l4/l4virtio/l4virtio>
#include <l4/re/error_helper>

extern "C" {
#include <ext4_errno.h>      // EOK, EIO, EAGAIN
#include <ext4_blockdev.h>   // lwext4 – via PRIVATE_INCDIR
}

#include <string.h>
#include <stdio.h>

namespace {

// Shared I/O buffer between this driver and the VirtIO block device.
// Must be large enough for the biggest single transfer lwext4 will request.
// 128 sectors = 64 KB covers the default block-cache transfer size.
static const l4_size_t IO_SIZE = 128 * 512;   // 64 KB

static L4virtio::Driver::Block_device            s_dev;
static void                                     *s_iobuf     = nullptr;
static L4virtio::Ptr<void>                       s_iodev_addr;

// ----- lwext4 callbacks -----

static int vio_open(struct ext4_blockdev * /*bdev*/)
{
    return EOK;
}

static int vio_close(struct ext4_blockdev * /*bdev*/)
{
    return EOK;
}

static int vio_bread(struct ext4_blockdev * /*bdev*/, void *buf,
                     uint64_t blk_id, uint32_t blk_cnt)
{
    static const uint32_t BATCH = IO_SIZE / 512;  // max sectors per request
    char *dst = reinterpret_cast<char *>(buf);

    while (blk_cnt > 0)
    {
        uint32_t  n     = (blk_cnt > BATCH) ? BATCH : blk_cnt;
        l4_size_t bytes = (l4_size_t)n * 512;

        auto h = s_dev.start_request(blk_id, L4VIRTIO_BLOCK_T_IN, nullptr);
        if (!h.valid())
            return EAGAIN;

        if (s_dev.add_block(h, s_iodev_addr, (l4_uint32_t)bytes) < 0)
        {
            s_dev.free_request(h);
            return EIO;
        }

        int r = s_dev.process_request(h);
        if (r < 0)
            return EIO;

        memcpy(dst, s_iobuf, bytes);
        dst    += bytes;
        blk_id += n;
        blk_cnt -= n;
    }
    return EOK;
}

static int vio_bwrite(struct ext4_blockdev * /*bdev*/, const void *buf,
                      uint64_t blk_id, uint32_t blk_cnt)
{
    static const uint32_t BATCH = IO_SIZE / 512;
    const char *src = reinterpret_cast<const char *>(buf);

    while (blk_cnt > 0)
    {
        uint32_t  n     = (blk_cnt > BATCH) ? BATCH : blk_cnt;
        l4_size_t bytes = (l4_size_t)n * 512;

        memcpy(s_iobuf, src, bytes);

        auto h = s_dev.start_request(blk_id, L4VIRTIO_BLOCK_T_OUT, nullptr);
        if (!h.valid())
            return EAGAIN;

        if (s_dev.add_block(h, s_iodev_addr, (l4_uint32_t)bytes) < 0)
        {
            s_dev.free_request(h);
            return EIO;
        }

        int r = s_dev.process_request(h);
        if (r < 0)
            return EIO;

        src    += bytes;
        blk_id += n;
        blk_cnt -= n;
    }
    return EOK;
}

// ----- Block device descriptor (runtime-initialised) -----

static uint8_t                  s_bbuf[512];
static struct ext4_blockdev_iface s_iface;
static struct ext4_blockdev       s_blockdev;

} // namespace

int virtio_blockdev_init(L4::Cap<L4virtio::Device> vio_cap)
{
    // Perform VirtIO handshake and share I/O memory.
    s_dev.setup_device(vio_cap, IO_SIZE, &s_iobuf, s_iodev_addr);

    l4_uint64_t sectors = s_dev.device_config().capacity;
    printf("[ext4] VirtIO block: %llu sectors (%llu MiB)\n",
           (unsigned long long)sectors,
           (unsigned long long)(sectors >> 11));

    // Initialise lwext4 structures.
    memset(&s_iface, 0, sizeof(s_iface));
    s_iface.open     = vio_open;
    s_iface.bread    = vio_bread;
    s_iface.bwrite   = vio_bwrite;
    s_iface.close    = vio_close;
    s_iface.ph_bsize = 512;
    s_iface.ph_bcnt  = sectors;
    s_iface.ph_bbuf  = s_bbuf;

    memset(&s_blockdev, 0, sizeof(s_blockdev));
    s_blockdev.bdif        = &s_iface;
    s_blockdev.part_offset = 0;
    s_blockdev.part_size   = sectors * 512;

    return 0;
}

struct ext4_blockdev *virtio_blockdev_get(void)
{
    return &s_blockdev;
}
