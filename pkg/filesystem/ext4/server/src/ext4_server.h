/*
 * ext4fs server implementation
 */

#ifndef EXT4_SERVER_H
#define EXT4_SERVER_H

#include <l4/re/dataspace>
#include <l4/re/rm>
#include <l4/re/util/cap_alloc>
#include <l4/sys/thread>
#include <ext4.h>

class Ext4_server
{
public:
    Ext4_server();
    ~Ext4_server();

    int run();
    int mount(const char *path);
    int umount(const char *path);

private:
    // Block device interface for ext4
    struct ext4_block_device
    {
        void *buf;           // Buffer for I/O
        size_t buf_size;     // Buffer size (512 bytes per sector)
        int (*read)(void *buf, l_loff_t pos);
        int (*write)(const void *buf, l_loff_t pos);
        unsigned long nsect; // Number of sectors
        bool busy;
    };

    /* Filesystem state */
    ext4_file _rootfd;          // Root directory file descriptor
    static ext4_block_device _blockdev;  // Block device structure
    bool _mounted;              // Whether filesystem is mounted
};

// Block device I/O functions for VirtIO
int ext4block_read(void *buf, l_loff_t pos);
int ext4block_write(const void *buf, l_loff_t pos);

#endif // EXT4_SERVER_H