// Device file base class for TuringOS devfs.
//
// Subclass this and override readv / writev / ioctl as needed.
// Be_file_stream provides default -EINVAL returns; only override what
// your device actually supports.
#pragma once

#include <l4/l4re_vfs/backend>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

namespace Devfs {

class Device_file : public L4Re::Vfs::Be_file_stream
{
public:
  // Default fstat: character device, mode 0666.
  int fstat(struct stat64 *buf) const noexcept override
  {
    memset(buf, 0, sizeof(*buf));
    buf->st_mode  = S_IFCHR | 0666;
    buf->st_nlink = 1;
    return 0;
  }

  int get_status_flags() const noexcept override { return O_RDWR; }

  virtual ~Device_file() noexcept {}
};

} // namespace Devfs
