#include <l4/lvgl/port.h>
#include "lvgl/demos/widgets/lv_demo_widgets.h"

int main()
{
    return lv_port_run("lvgl-demo", lv_demo_widgets);
}
