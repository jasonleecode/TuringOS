/*
 * ext4 block device implementation for L4Re
 */

#ifndef EXT4_BLOCKDEV_H
#define EXT4_BLOCKDEV_H

#include <ext4_blockdev.h>

int ext4_blockdev_init(void);
int ext4_blockdev_cleanup(void);

#endif // EXT4_BLOCKDEV_H