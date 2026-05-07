/*
 * lv_port_disp.cc — LVGL display driver for L4Re / fb-drv
 *
 * Uses partial rendering mode with a full-screen-sized off-screen buffer.
 * LVGL renders only the bounding box of dirty widgets into the buffer (one
 * render pass since the buffer is never too small), then flush_cb copies
 * just that rectangle row-by-row to the physical FB.  This avoids both
 * mid-render blank flashes and the expensive full-screen memcpy that
 * FULL mode incurs on every slider drag or button press.
 */

#include <l4/re/util/video/goos_fb>
#include <l4/re/video/view>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "lvgl/lvgl.h"
}

#include "lv_port_disp.h"

static L4Re::Util::Video::Goos_fb s_goos_fb;
static unsigned s_width;
static unsigned s_height;
static unsigned s_bpp;
static void    *s_fb;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint8_t *src = px_map;
    for (int32_t y = area->y1; y <= area->y2; ++y) {
        uint8_t *dst = (uint8_t *)s_fb
                       + ((unsigned)y * s_width + (unsigned)area->x1) * s_bpp;
        memcpy(dst, src, (unsigned)w * s_bpp);
        src += (unsigned)w * s_bpp;
    }
    s_goos_fb.refresh(area->x1, area->y1, w, h);
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
    s_bpp    = vi.pixel_info.bytes_per_pixel();

    {
        auto buf = s_goos_fb.buffer();
        l4_size_t buf_sz = (l4_size_t)buf->size();
        printf("[lv_port] FB DS cap=0x%lx size=%zu\n", buf.cap(), buf_sz);
        if (buf_sz > 0) {
            void *addr = nullptr;
            int r = L4Re::Env::env()->rm()->attach(
                &addr, buf_sz,
                L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW,
                L4::Ipc::make_cap_rw(buf), 0, L4_PAGESHIFT);
            printf("[lv_port] rm->attach ret=%d addr=%p\n", r, addr);
            if (r >= 0) s_fb = addr;
        }
    }
    if (!s_fb)
    {
        printf("[lv_port] ERROR: cannot map framebuffer\n");
        return;
    }

    printf("[lv_port] FB %ux%u %ubpp at %p\n", s_width, s_height, s_bpp * 8, s_fb);

    // Full-screen off-screen buffer: any dirty bounding box fits in one render pass
    void *draw_buf = malloc(s_width * s_height * s_bpp);
    if (!draw_buf)
    {
        printf("[lv_port] ERROR: cannot allocate draw buffer\n");
        return;
    }

    lv_display_t *disp = lv_display_create(s_width, s_height);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, draw_buf, nullptr,
                           s_width * s_height * s_bpp,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
}

bool lv_port_disp_is_ready() { return s_fb != nullptr; }
