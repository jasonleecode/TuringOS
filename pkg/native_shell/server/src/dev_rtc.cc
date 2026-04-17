#include "dev_rtc.h"

#include <l4/re/env>
#include <l4/rtc/rtc.h>

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

ssize_t Rtc_device_file::readv(const struct iovec *iov, int iovcnt) noexcept
{
  struct timespec real;
  clock_gettime(CLOCK_REALTIME, &real);

  char buf[40];
  int len;
  if (real.tv_sec > 946684800L) { // after year 2000 → RTC valid
    struct tm *tm = gmtime(&real.tv_sec);
    len = strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC\n", tm);
  } else {
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    len = snprintf(buf, sizeof(buf), "(no RTC) uptime %llds\n",
                   (long long)mono.tv_sec);
  }

  ssize_t copied = 0;
  for (int i = 0; i < iovcnt && copied < len; ++i) {
    size_t n = iov[i].iov_len < (size_t)(len - copied)
               ? iov[i].iov_len : (size_t)(len - copied);
    memcpy(iov[i].iov_base, buf + copied, n);
    copied += (ssize_t)n;
  }
  return copied;
}

/* Parse "YYYY-MM-DD HH:MM:SS" from the iovec chain into a flat buffer. */
static int collect_write(const struct iovec *iov, int iovcnt,
                         char *out, int max)
{
  int total = 0;
  for (int i = 0; i < iovcnt && total < max - 1; ++i) {
    int n = (int)iov[i].iov_len;
    if (total + n > max - 1)
      n = max - 1 - total;
    memcpy(out + total, iov[i].iov_base, n);
    total += n;
  }
  out[total] = '\0';
  return total;
}

/* Portable UTC mktime (no timezone). */
static time_t utc_mktime(int y, int mo, int d, int h, int mi, int s)
{
  static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  bool is_leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  long days = (y - 1970) * 365L
    + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400
    + mdays[mo - 1] + (mo > 2 && is_leap ? 1 : 0) + d - 1;
  return (time_t)(days * 86400L + h * 3600 + mi * 60 + s);
}

ssize_t Rtc_device_file::writev(const struct iovec *iov, int iovcnt) noexcept
{
  char line[32];
  int len = collect_write(iov, iovcnt, line, (int)sizeof(line));
  if (len <= 0)
    return -EINVAL;

  int y, mo, d, h, mi, s;
  if (sscanf(line, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6)
    return -EINVAL;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 ||
      h  < 0 || h  > 23 || mi < 0 || mi > 59 || s < 0 || s > 59)
    return -EINVAL;

  l4_cap_idx_t rtc_cap = l4re_env_get_cap("rtc");
  if (l4_is_invalid_cap(rtc_cap))
    return -ENODEV;

  time_t t = utc_mktime(y, mo, d, h, mi, s);

  struct timespec mono;
  clock_gettime(CLOCK_MONOTONIC, &mono);
  l4_uint64_t uptime_ns = (l4_uint64_t)mono.tv_sec  * 1000000000ULL
                        + (l4_uint64_t)mono.tv_nsec;
  l4_uint64_t new_off   = (l4_uint64_t)t * 1000000000ULL - uptime_ns;

  if (l4rtc_set_offset_to_realtime(rtc_cap, new_off) != 0)
    return -EIO;

  return len;
}
