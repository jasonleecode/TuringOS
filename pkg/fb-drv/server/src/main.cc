/*
 * fb-drv: user-space framebuffer driver server for TuringOS
 *
 * Accepts the "vesa" Goos cap (from MOE/bootstrap ramfb) as upstream,
 * and exposes a standard L4Re::Video::Goos IPC interface on the "fb" cap
 * for client applications. Clients use "vesa" name in their environment,
 * which ned maps to fb-drv's "fb" server endpoint.
 *
 * Architecture:
 *   MOE vesa cap → fb-drv (Goos_svr) → clients via "fb" cap
 */

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/br_manager>
#include <l4/re/util/object_registry>
#include <l4/re/util/video/goos_svr>
#include <l4/re/util/video/goos_fb>
#include <l4/re/video/goos>
#include <l4/re/video/view>
#include <l4/sys/cxx/ipc_epiface>
#include <l4/util/util.h>
#include <cstdio>

class Fb_svr :
  public L4Re::Util::Video::Goos_svr,
  public L4::Epiface_t<Fb_svr, L4Re::Video::Goos>
{
  L4Re::Util::Video::Goos_fb _upstream; // client handle to MOE's vesa cap

public:
  int init()
  {
    // Connect to upstream vesa cap (provided by ned via caps table)
    long r = _upstream.init("vesa");
    if (r < 0)
      {
        printf("[fb-drv] ERROR: cannot open 'vesa' cap (%ld)\n", r);
        printf("[fb-drv]        Start QEMU with -device ramfb\n");
        return (int)r;
      }

    // Read screen geometry from upstream Goos
    L4Re::Video::Goos::Info gi;
    r = _upstream.goos()->info(&gi);
    if (r < 0)
      {
        printf("[fb-drv] ERROR: cannot get screen info (%ld)\n", r);
        return (int)r;
      }

    L4Re::Video::View::Info vi;
    r = _upstream.view_info(&vi);
    if (r < 0)
      {
        printf("[fb-drv] ERROR: cannot get view info (%ld)\n", r);
        return (int)r;
      }

    // Map the upstream framebuffer into our address space
    // (needed to verify it works; clients get the ds directly)
    void *fb_addr = _upstream.attach_buffer();
    if (!fb_addr)
      {
        printf("[fb-drv] ERROR: cannot map framebuffer\n");
        return -L4_ENOMEM;
      }

    // Populate Goos_svr fields with upstream info
    _screen_info = gi;
    _view_info   = vi;
    // Share the exact same dataspace with clients (zero-copy)
    _fb_ds       = _upstream.buffer();

    printf("[fb-drv] Upstream framebuffer: %lux%lu, %u bpp, %lu bytes/line\n",
           (unsigned long)gi.width, (unsigned long)gi.height,
           gi.pixel_info.bytes_per_pixel() * 8,
           (unsigned long)vi.bytes_per_line);
    printf("[fb-drv] FB mapped at %p, ds cap %lu\n",
           fb_addr, _fb_ds.cap());

    return 0;
  }

  // ramfb is F_auto_refresh; real HW drivers override this for vsync/DMA flush
  int refresh(int /*x*/, int /*y*/, int /*w*/, int /*h*/) override
  { return 0; }
};

static L4Re::Util::Registry_server<L4Re::Util::Br_manager_hooks> server;

int main()
{
  printf("[fb-drv] Starting\n");

  static Fb_svr fb_svr;

  if (fb_svr.init() < 0)
    return 1;

  // Register on the "fb" capability (server endpoint passed by ned)
  L4Re::chkcap(server.registry()->register_obj(&fb_svr, "fb"),
               "[fb-drv] register_obj failed — 'fb' cap missing in environment");

  printf("[fb-drv] Ready, serving Goos IPC on 'fb'\n");

  server.loop();
  return 0;
}
