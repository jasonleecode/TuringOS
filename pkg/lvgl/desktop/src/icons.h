#pragma once

#include "lvgl/lvgl.h"

enum class Icon { computer, calculator, display, calendar, minesweeper, flag, user, exit, cursor };
lv_obj_t *desktop_icon(lv_obj_t *parent, Icon icon, int size);
