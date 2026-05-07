/*
 * lvgl-demo: LVGL v9 on L4Re with fb-drv display backend
 */

#include <l4/util/util.h>
#include <l4/sys/kip.h>
#include <l4/re/env.h>
#include <l4/klog/klog.h>
#include <cstdio>

extern "C" {
#include "lvgl/lvgl.h"
#include "lvgl/demos/widgets/lv_demo_widgets.h"
}

#include "lv_port_disp.h"
#include "lv_port_input.h"

static inline uint32_t get_ms()
{
    return (uint32_t)(l4_kip_clock(l4re_kip()) / 1000ULL);
}

int main()
{
    klog_init(NULL);
    klog_info(KLOG_DISP, "lvgl-demo: starting");

    lv_init();
    lv_port_disp_init();
    lv_port_input_init();

    if (!lv_port_disp_is_ready())
    {
        klog_err(KLOG_DISP, "lvgl-demo: no display available — exiting");
        return 1;
    }

    lv_demo_widgets();

    klog_info(KLOG_DISP, "lvgl-demo: UI loop running");
    klog_flush();   /* write startup log to /ext4/syslog.txt */

    uint32_t last_ms = get_ms();
    uint32_t flush_ms = last_ms;
    while (true)
    {
        uint32_t now_ms = get_ms();
        lv_tick_inc(now_ms - last_ms);
        last_ms = now_ms;

        /* Flush log to disk every 10 s */
        if (now_ms - flush_ms >= 10000u)
        {
            klog_flush();
            flush_ms = now_ms;
        }

        uint32_t delay = lv_timer_handler();
        l4_sleep(delay > 16 ? 16 : (delay ? delay : 1));
    }

    return 0;
}
