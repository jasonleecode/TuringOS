// A bounded, volatile volume for the shared /tmp namespace.
#include <cstdlib>
#include <cstring>
extern "C" {
#include <ext4_blockdev.h>
#include <ext4_errno.h>
}

namespace {
constexpr unsigned sectors = 4096;
unsigned char *bytes;
int noop(ext4_blockdev *) { return EOK; }
int read_blocks(ext4_blockdev *, void *dst, uint64_t first, uint32_t count)
{
  if (first > sectors || count > sectors - first) return EIO;
  memcpy(dst, bytes + first * 512, count * 512);
  return EOK;
}
int write_blocks(ext4_blockdev *, const void *src, uint64_t first, uint32_t count)
{
  if (first > sectors || count > sectors - first) return EIO;
  memcpy(bytes + first * 512, src, count * 512);
  return EOK;
}
}
ext4_blockdev *ram_blockdev()
{
  static ext4_blockdev_iface iface{};
  static ext4_blockdev dev{};
  static unsigned char scratch[512];
  bytes = static_cast<unsigned char *>(calloc(sectors, 512));
  if (!bytes) return nullptr;
  iface.open = iface.close = noop;
  iface.bread = read_blocks;
  iface.bwrite = write_blocks;
  iface.ph_bsize = 512;
  iface.ph_bcnt = sectors;
  iface.ph_bbuf = scratch;
  dev.bdif = &iface;
  dev.part_size = sectors * 512;
  return &dev;
}
