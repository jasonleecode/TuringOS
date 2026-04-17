#include "dev_radio.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

ssize_t Radio_device_file::readv(const struct iovec *iov, int iovcnt) noexcept
{
  if (!_inited)
    return -ENXIO;

  Tef6686hn::Quality q = {};
  l4_uint32_t freq_khz = 0;

  _radio.get_quality(&q);
  _radio.get_frequency(&freq_khz);

  char buf[96];
  int len = snprintf(buf, sizeof(buf),
                     "{\"freq\":%u,\"rssi\":%.1f,\"snr\":%.1f,\"mp\":%d}\n",
                     freq_khz,
                     q.rssi / 10.0,
                     q.snr  / 10.0,
                     (int)q.multipath);

  ssize_t copied = 0;
  for (int i = 0; i < iovcnt && copied < len; ++i) {
    size_t n = iov[i].iov_len < (size_t)(len - copied)
               ? iov[i].iov_len : (size_t)(len - copied);
    memcpy(iov[i].iov_base, buf + copied, n);
    copied += (ssize_t)n;
  }
  return copied;
}

/* Collect iovec chain into a flat null-terminated string. */
static int collect_cmd(const struct iovec *iov, int iovcnt,
                       char *out, int max)
{
  int total = 0;
  for (int i = 0; i < iovcnt && total < max - 1; ++i) {
    int n = (int)iov[i].iov_len;
    if (total + n > max - 1)
      n = max - 1 - total;
    memcpy(out + total, iov[i].iov_base, (size_t)n);
    total += n;
  }
  /* Strip trailing newline/whitespace */
  while (total > 0 && (out[total - 1] == '\n' || out[total - 1] == '\r' ||
                       out[total - 1] == ' '))
    --total;
  out[total] = '\0';
  return total;
}

ssize_t Radio_device_file::writev(const struct iovec *iov, int iovcnt) noexcept
{
  char cmd[64];
  int len = collect_cmd(iov, iovcnt, cmd, (int)sizeof(cmd));
  if (len <= 0)
    return -EINVAL;

  /* "init" */
  if (strcmp(cmd, "init") == 0) {
    long r = _radio.init();
    if (r < 0)
      return -EIO;
    _inited = true;
    return len;
  }

  if (!_inited)
    return -ENXIO;

  /* "tune [FM|MW|LW] <freq_khz>" */
  if (strncmp(cmd, "tune ", 5) == 0) {
    char band_str[8] = {};
    unsigned freq_khz = 0;
    if (sscanf(cmd + 5, "%7s %u", band_str, &freq_khz) != 2)
      return -EINVAL;

    Tef6686hn::Band band;
    if      (strcmp(band_str, "FM") == 0) band = Tef6686hn::Band::FM;
    else if (strcmp(band_str, "MW") == 0) band = Tef6686hn::Band::MW;
    else if (strcmp(band_str, "LW") == 0) band = Tef6686hn::Band::LW;
    else return -EINVAL;

    if (_radio.tune(band, freq_khz) < 0)
      return -EIO;
    return len;
  }

  /* "seek up" / "seek down" */
  if (strncmp(cmd, "seek ", 5) == 0) {
    Tef6686hn::Seek_dir dir;
    if      (strcmp(cmd + 5, "up")   == 0) dir = Tef6686hn::Seek_dir::Up;
    else if (strcmp(cmd + 5, "down") == 0) dir = Tef6686hn::Seek_dir::Down;
    else return -EINVAL;

    if (_radio.seek(dir) < 0)
      return -EIO;
    return len;
  }

  /* "mute" / "unmute" */
  if (strcmp(cmd, "mute")   == 0) { _radio.mute(true);  return len; }
  if (strcmp(cmd, "unmute") == 0) { _radio.mute(false); return len; }

  /* "vol <level_db10>" */
  if (strncmp(cmd, "vol ", 4) == 0) {
    unsigned level = (unsigned)atoi(cmd + 4);
    if (_radio.set_output_level((l4_uint16_t)level) < 0)
      return -EIO;
    return len;
  }

  return -EINVAL;
}
