#include "dev_temp.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

ssize_t Temp_device_file::readv(const struct iovec *iov, int iovcnt) noexcept
{
  int temp_c100 = 0;
  if (_sensor.read_temp_c100(&temp_c100) != L4_EOK)
    return -EIO;

  bool neg   = temp_c100 < 0;
  int  abs_v = neg ? -temp_c100 : temp_c100;

  char buf[24];
  int len = snprintf(buf, sizeof(buf), "%s%d.%02d\n",
                     neg ? "-" : "+", abs_v / 100, abs_v % 100);

  ssize_t copied = 0;
  for (int i = 0; i < iovcnt && copied < len; ++i) {
    size_t n = iov[i].iov_len < (size_t)(len - copied)
               ? iov[i].iov_len : (size_t)(len - copied);
    memcpy(iov[i].iov_base, buf + copied, n);
    copied += (ssize_t)n;
  }
  return copied;
}
