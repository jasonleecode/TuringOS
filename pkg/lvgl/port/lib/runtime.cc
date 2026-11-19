#include <l4/lvgl/port.h>
#include <l4/klog/klog.h>
#include <l4/re/env.h>
#include <l4/sys/kip.h>
#include <l4/util/util.h>
#include "lvgl/lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_input.h"

static bool running;

static uint32_t now_ms()
{
    return static_cast<uint32_t>(l4_kip_clock(l4re_kip()) / 1000ULL);
}

void lv_port_quit()
{
    running = false;
}

int lv_port_run(const char *name, void (*create_ui)())
{
    klog_init(nullptr);
    klog_info(KLOG_DISP, "%s: starting", name);
    lv_init();
    lv_port_disp_init();
    if (!lv_port_disp_is_ready() || !lv_display_get_default()) {
        klog_err(KLOG_DISP, "%s: no display available", name);
        return 1;
    }
    lv_port_input_init();
    create_ui();
    klog_info(KLOG_DISP, "%s: UI loop running", name);
    klog_flush();

    running = true;
    uint32_t previous = now_ms(), last_flush = previous;
    while (running) {
        uint32_t now = now_ms();
        lv_tick_inc(now - previous);
        previous = now;
        if (now - last_flush >= 10000u) {
            klog_flush();
            last_flush = now;
        }
        uint32_t delay = lv_timer_handler();
        l4_sleep(delay > 16 ? 16 : (delay ? delay : 1));
    }
    lv_port_input_shutdown();
    lv_obj_clean(lv_screen_active());
    lv_obj_clean(lv_layer_sys());
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_refr_now(nullptr);
    return 0;
}
