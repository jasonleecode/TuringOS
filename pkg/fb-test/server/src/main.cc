/*
 * Framebuffer test for QEMU ramfb + L4Re.
 *
 * Accesses the "vesa" Goos capability exposed by MOE (populated from QEMU
 * ramfb via bootstrap's setup_ramfb()). Draws a diagnostic test pattern and
 * reports display info to the serial console.
 *
 * Usage: launch in ned cfg with the "vesa" capability passed in, or let
 * Env_ns query it from the global namespace automatically.
 */

#include <l4/re/env>
#include <l4/util/util.h>
#include <l4/re/util/video/goos_fb>
#include <l4/re/video/goos>
#include <l4/re/video/view>
#include <l4/re/error_helper>
#include <l4/sys/err.h>
#include <cstdio>
#include <cstring>

static void draw_pattern(unsigned char *fb,
                         unsigned width, unsigned height, unsigned bpp,
                         unsigned bytes_per_line)
{
  // Stripe 1/4: red
  // Stripe 2/4: green
  // Stripe 3/4: blue
  // Stripe 4/4: white gradient
  unsigned stripe = height / 4;

  for (unsigned y = 0; y < height; ++y)
    {
      for (unsigned x = 0; x < width; ++x)
        {
          unsigned char *px = fb + y * bytes_per_line + x * bpp;
          unsigned char r = 0, g = 0, b = 0;

          if (y < stripe)               // red stripe
            r = 255;
          else if (y < stripe * 2)      // green stripe
            g = 255;
          else if (y < stripe * 3)      // blue stripe
            b = 255;
          else                          // white gradient left→right
            r = g = b = (unsigned char)(x * 255 / (width - 1));

          // Draw a white border (4px)
          if (x < 4 || x >= width - 4 || y < 4 || y >= height - 4)
            r = g = b = 255;

          if (bpp == 4)
            {
              px[0] = b;   // XRGB8888: B
              px[1] = g;   // G
              px[2] = r;   // R
              px[3] = 0;   // X
            }
          else if (bpp == 3)
            {
              px[0] = b;
              px[1] = g;
              px[2] = r;
            }
          else if (bpp == 2)
            {
              // RGB565
              unsigned short c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
              px[0] = c & 0xFF;
              px[1] = c >> 8;
            }
        }
    }
}

int main()
{
  printf("[fb-test] Starting framebuffer test\n");

  L4Re::Util::Video::Goos_fb gfb;

  // "vesa" is registered by MOE from bootstrap's ramfb VBE info
  long err = gfb.init("vesa");
  if (err < 0)
    {
      printf("[fb-test] ERROR: cannot get 'vesa' capability: %s (%ld)\n"
             "          Make sure QEMU is started with -device ramfb\n",
             l4sys_errtostr(err), err);
      return 1;
    }

  L4Re::Video::View::Info vi;
  if (gfb.view_info(&vi) < 0)
    {
      printf("[fb-test] ERROR: cannot get view info\n");
      return 1;
    }

  unsigned width   = vi.width;
  unsigned height  = vi.height;
  unsigned bpp     = vi.pixel_info.bytes_per_pixel();
  unsigned bpl     = vi.bytes_per_line;

  printf("[fb-test] Framebuffer: %ux%u, %u bpp, %u bytes/line\n",
         width, height, bpp * 8, bpl);

  void *fb_addr = gfb.attach_buffer();
  if (!fb_addr)
    {
      printf("[fb-test] ERROR: cannot attach framebuffer buffer\n");
      return 1;
    }

  printf("[fb-test] Framebuffer mapped at %p, drawing test pattern...\n",
         fb_addr);

  draw_pattern(reinterpret_cast<unsigned char *>(fb_addr),
               width, height, bpp, bpl);

  // Trigger refresh (needed if auto-refresh is not set)
  gfb.refresh(0, 0, width, height);

  printf("[fb-test] Done. Pattern visible in the display window.\n"
         "          Connect VNC viewer to localhost:5900 if using --gpu --vnc\n");

  // Keep running so the display stays visible
  l4_sleep_forever();

  return 0;
}
