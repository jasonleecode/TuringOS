/*
 * ext4fs server implementation
 */

#ifndef EXT4_SERVER_H
#define EXT4_SERVER_H

#include <l4/re/dataspace>
#include <l4/re/rm>

class Ext4_server {
public:
    Ext4_server();
    ~Ext4_server();

    int run();
    int mount(const char *path);
    int umount(const char *path);

private:
    L4::Cap<L4Re::Dataspace> _fs_cap;
    L4::Cap<L4Re::Rm> _rm;
    bool _running;
};

#endif // EXT4_SERVER_H