/*
 * lv_port_disp.cc — LVGL display driver for L4Re / fb-drv
 *
 * Connects to fb-drv via the "fb" Goos cap, maps the framebuffer,
 * and registers it as an LVGL full-screen draw buffer.
 * ramfb is auto-refresh, so the flush callback just calls refresh + flush_ready.
 */

#include <l4/re/util/video/goos_fb>
#include <l4/re/video/view>
#include <cstdio>

extern "C" {
#include "lvgl/lvgl.h"
}

#include "lv_port_disp.h"

static L4Re::Util::Video::Goos_fb s_goos_fb;
static unsigned s_width;
static unsigned s_height;

static void flush_cb(lv_display_t *disp,
                     const lv_area_t * /*area*/,
                     uint8_t * /*px_map*/)
{
    s_goos_fb.refresh(0, 0, s_width, s_height);
    lv_display_flush_ready(disp);
}

void lv_port_disp_init()
{
    long err = s_goos_fb.init("fb");
    if (err < 0)
    {
        printf("[lv_port] 'fb' cap not found (%ld), trying 'vesa'\n", err);
        err = s_goos_fb.init("vesa");
    }
    if (err < 0)
    {
        printf("[lv_port] ERROR: no display cap available (%ld)\n", err);
        return;
    }

    L4Re::Video::View::Info vi;
    if (s_goos_fb.view_info(&vi) < 0)
    {
        printf("[lv_port] ERROR: cannot get view info\n");
        return;
    }

    s_width  = vi.width;
    s_height = vi.height;
    unsigned bpp = vi.pixel_info.bytes_per_pixel();

    void *fb = s_goos_fb.attach_buffer();
    if (!fb)
    {
        printf("[lv_port] ERROR: cannot map framebuffer\n");
        return;
    }

    printf("[lv_port] FB %ux%u %ubpp at %p\n", s_width, s_height, bpp * 8, fb);

    lv_display_t *disp = lv_display_create(s_width, s_height);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);

    // Use physical framebuffer directly as the LVGL draw buffer (zero-copy)
    lv_display_set_buffers(disp, fb, nullptr,
                           s_width * s_height * bpp,
                           LV_DISPLAY_RENDER_MODE_FULL);

    lv_display_set_flush_cb(disp, flush_cb);
}
