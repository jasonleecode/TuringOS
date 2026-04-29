/*
 * lvgl-demo: LVGL v9 on L4Re with fb-drv display backend
 *
 * Threads:
 *   tick thread  — calls lv_tick_inc(1) every 1 ms
 *   main thread  — runs lv_timer_handler() loop
 */

#include <l4/util/util.h>
#include <pthread.h>
#include <cstdio>

extern "C" {
#include "lvgl/lvgl.h"
#include "lvgl/demos/widgets/lv_demo_widgets.h"
}

#include "lv_port_disp.h"

static void *tick_thread(void *)
{
    while (true)
    {
        lv_tick_inc(1);
        l4_sleep(1);
    }
    return nullptr;
}

int main()
{
    printf("[lvgl-demo] Starting\n");

    lv_init();
    lv_port_disp_init();

    pthread_t tid;
    pthread_create(&tid, nullptr, tick_thread, nullptr);

    lv_demo_widgets();

    printf("[lvgl-demo] UI loop running\n");

    while (true)
    {
        uint32_t delay = lv_timer_handler();
        l4_sleep(delay ? delay : 1);
    }

    return 0;
}
