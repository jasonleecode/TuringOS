#include <l4/lvgl/port.h>
#include "lvgl/lvgl.h"

static void create_ui()
{
    auto screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x16324f), 0);
    auto label = lv_label_create(screen);
    lv_label_set_text(label, "Hello from TuringOS");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
}

int main()
{
    return lv_port_run("sdk-lvgl", create_ui);
}
