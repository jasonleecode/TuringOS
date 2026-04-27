/*
 * uart-test — L4::Vcon client test
 *
 * Receives a "vcon" capability from the environment (provided by
 * virtio-serial server via ned config) and exercises it using only
 * the standard L4Re Vcon API:
 *   l4_vcon_write()  →  send bytes to the serial port
 *   l4_vcon_read()   →  receive bytes from the serial port
 *
 * No hardware access.  No virtio details.  Just vcon IPC.
 */

#include <cstdio>
#include <cstring>
#include <l4/re/env>
#include <l4/sys/vcon.h>
#include <l4/util/util.h>

int main()
{
  printf("[uart-test] starting L4::Vcon client test\n");

  /* Get the vcon capability provided by virtio-serial server via ned */
  auto vcon = L4Re::Env::env()->get_cap<L4::Vcon>("vcon");
  if (!vcon.is_valid())
    {
      printf("[uart-test] 'vcon' capability not found — check uart-test.cfg\n");
      return 1;
    }
  printf("[uart-test] got 'vcon' cap\n");

  /* Send banner to the host pty */
  const char *banner =
    "\r\n=== TuringOS uart-test (L4::Vcon client) ===\r\n"
    "Type anything and it will be echoed back.\r\n"
    "Connect: picocom /dev/pts/N  (path printed by QEMU on startup)\r\n\r\n";

  long n = l4_vcon_write(vcon.cap(), banner, (unsigned)strlen(banner));
  if (n < 0)
    printf("[uart-test] write failed: %ld\n", n);
  else
    printf("[uart-test] sent banner (%ld bytes)\n", n);

  /* Echo loop: poll for input and echo it back */
  printf("[uart-test] entering echo loop (polling every 10ms)\n");

  char buf[L4_VCON_READ_SIZE];
  unsigned long rx_total = 0;
  unsigned long tx_total = 0;

  for (;;)
    {
      int r = l4_vcon_read(vcon.cap(), buf, sizeof(buf));
      if (r > 0)
        {
          unsigned bytes = (unsigned)(r & L4_VCON_READ_SIZE_MASK);
          rx_total += bytes;

          /* Echo back via vcon */
          long w = l4_vcon_write(vcon.cap(), buf, bytes);
          if (w > 0)
            tx_total += (unsigned long)w;

          /* Also show on L4Re console */
          printf("[uart-test] rx %u bytes: '", bytes);
          for (unsigned i = 0; i < bytes; ++i)
            {
              char c = buf[i];
              if (c >= 0x20 && c < 0x7f)
                putchar(c);
              else
                printf("\\x%02x", (unsigned char)c);
            }
          printf("'\n");
        }

      l4_sleep(10);
    }

  return 0;
}
