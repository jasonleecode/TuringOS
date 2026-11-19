#include "icons.h"
#include "minesweeper.h"
#include <l4/lvgl/port.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>

extern "C" {
extern const uint8_t _binary_meadow_rgb565_start[];
extern const lv_font_t desktop_font;
}

namespace {
constexpr int bar_height = 36;
constexpr unsigned ink = 0x173552, white = 0xffffff;
struct App { const char *name; Icon icon; int w = 330, h = 219; };
const App computer{"我的电脑", Icon::computer};
const App calculator{"计算器", Icon::calculator, 255, 254};
const App mines_app{"扫雷", Icon::minesweeper, 280, 328};
const App display{"显示属性", Icon::display};
const App clock_app{"日期和时间", Icon::calendar};

lv_obj_t *desktop, *wallpaper, *bar, *menu_layer, *start, *task, *task_label;
lv_obj_t *window, *time_label, *date_label, *result;
const App *current;
int width, height;

lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h)
{
    auto obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    return obj;
}

void fill(lv_obj_t *obj, unsigned color)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

void gradient(lv_obj_t *obj, unsigned top, unsigned bottom)
{
    fill(obj, top);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
}

void edge(lv_obj_t *obj, unsigned color, int radius = 0)
{
    lv_obj_set_style_border_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, radius, 0);
}

lv_obj_t *label(lv_obj_t *parent, const char *str, unsigned color = ink,
                const lv_font_t *font = &desktop_font)
{
    auto obj = lv_label_create(parent);
    lv_label_set_text(obj, str);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, lv_event_cb_t click,
                 void *data = nullptr)
{
    auto obj = box(parent, x, y, w, h);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, click, LV_EVENT_CLICKED, data);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x8cb9ed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_PRESSED);
    return obj;
}

void show_menu(bool visible)
{
    if (visible) {
        lv_obj_move_to_index(menu_layer, -1);
        lv_obj_remove_flag(menu_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(start, LV_STATE_CHECKED);
    } else {
        lv_obj_add_flag(menu_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(start, LV_STATE_CHECKED);
    }
}

void tick(lv_timer_t *)
{
    const time_t now = time(nullptr);
    struct tm local;
    if (!localtime_r(&now, &local)) return;
    char text[48];
    strftime(text, sizeof(text), "%H:%M", &local);
    if (strcmp(lv_label_get_text(time_label), text)) lv_label_set_text(time_label, text);
    if (date_label) {
        strftime(text, sizeof(text), "%Y-%m-%d\n%H:%M:%S", &local);
        lv_label_set_text(date_label, text);
    }
}

struct {
    char value[24] = "0";
    double stored;
    char operation;
    bool replace = true;
} calc;

void compute()
{
    double right = atof(calc.value), answer = right;
    switch (calc.operation) {
    case '+': answer = calc.stored + right; break;
    case '-': answer = calc.stored - right; break;
    case '*': answer = calc.stored * right; break;
    case '/':
        answer = std::fpclassify(right) == FP_ZERO
                 ? std::numeric_limits<double>::quiet_NaN() : calc.stored / right;
        break;
    }
    if (std::isfinite(answer)) snprintf(calc.value, sizeof(calc.value), "%.9g", answer);
    else strcpy(calc.value, "Error");
    calc.operation = 0;
    calc.replace = true;
}

void key(lv_event_t *e)
{
    const char c = *static_cast<const char *>(lv_event_get_user_data(e));
    if (c == 'C') {
        calc = {};
    } else if (c == '.' || (c >= '0' && c <= '9')) {
        if (calc.replace) strcpy(calc.value, "0");
        calc.replace = false;
        size_t n = strlen(calc.value);
        if (c == '.' && strchr(calc.value, '.')) return;
        if (c != '.' && !strcmp(calc.value, "0")) n = 0;
        if (n < 11) { calc.value[n] = c; calc.value[n + 1] = 0; }
    } else {
        if (calc.operation && !calc.replace) compute();
        if (c == '=') {
            calc.operation = 0;
        } else if (strcmp(calc.value, "Error")) {
            calc.stored = atof(calc.value);
            calc.operation = c;
        }
        calc.replace = true;
    }
    lv_label_set_text(result, calc.value);
}

void calculator_view(lv_obj_t *body, int w)
{
    auto lcd = box(body, 14, 12, w - 28, 36);
    fill(lcd, 0xf6fae9); edge(lcd, 0x89938b, 2);
    result = label(lcd, calc.value, 0x263922, &lv_font_montserrat_20);
    lv_obj_align(result, LV_ALIGN_RIGHT_MID, -8, 0);
    static const char *const keys[] = {"7","8","9","/", "4","5","6","*",
                                       "1","2","3","-", "0",".","=","+"};
    int kw = (w - 28) / 4;
    for (int i = 0; i < 16; ++i) {
        auto b = button(body, 14 + i % 4 * kw, 57 + i / 4 * 32, kw - 4, 28,
                        key, const_cast<char *>(keys[i]));
        gradient(b, white, i % 4 == 3 ? 0xcaddf3 : 0xe4dfce); edge(b, 0x929596, 3);
        lv_obj_center(label(b, keys[i], i % 4 == 3 ? 0x174d95 : ink, &lv_font_montserrat_16));
    }
    auto reset = button(body, 14, 188, w - 32, 24, key, const_cast<char *>("C"));
    fill(reset, 0xf7f4ec); edge(reset, 0x9a9d99, 3);
    lv_obj_center(label(reset, "清除"));
}

Minesweeper game;
bool flag_mode;
lv_obj_t *cell_labels[Minesweeper::cells], *mine_status, *flag_button;

void draw_board()
{
    static constexpr unsigned numbers[] = {ink, 0x154eaf, 0x19703c, 0xa62520, 0x473085,
                                          0x842f21, 0x126d72, 0x262c35, 0x4c5860};
    bool lost = game.phase() == Minesweeper::Phase::lost;
    if (lost) lv_label_set_text(mine_status, "踩雷了");
    else if (game.phase() == Minesweeper::Phase::won) lv_label_set_text(mine_status, "胜利");
    else lv_label_set_text_fmt(mine_status, "余雷 %d", game.remaining_flags());
    for (int i = 0; i < Minesweeper::cells; ++i) {
        const auto &cell = game.at(i);
        auto text = cell_labels[i];
        auto tile = lv_obj_get_parent(text);
        bool visible = cell.open || (lost && cell.mine);
        unsigned color = ink;
        const char *mark = "";
        char digit[] = "0";
        if (cell.flag) {
            mark = lost && !cell.mine ? "X" : "旗";
            color = 0xa12723;
        } else if (visible && cell.mine) {
            mark = "雷";
        } else if (cell.open && cell.neighbors) {
            digit[0] += cell.neighbors;
            mark = digit;
            color = numbers[cell.neighbors];
        }
        fill(tile, cell.open && cell.mine ? 0xff9280 : visible ? 0xece9d8 : 0xb9c9d8);
        lv_label_set_text(text, mark);
        lv_obj_set_style_text_color(text, lv_color_hex(color), 0);
    }
}

void play_tile(lv_event_t *e)
{
    int i = static_cast<int>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    if (flag_mode) game.toggle_flag(i);
    else game.reveal(i, lv_tick_get());
    draw_board();
}

void new_game(lv_event_t *)
{
    game = {};
    flag_mode = false;
    lv_obj_remove_state(flag_button, LV_STATE_CHECKED);
    draw_board();
}

void mines_view(lv_obj_t *body, int w, int h)
{
    mine_status = label(body, "");
    lv_obj_set_pos(mine_status, 10, 9);
    flag_button = button(body, w - 146, 5, 62, 27, [](lv_event_t *) {
        flag_mode = !flag_mode;
        lv_obj_set_state(flag_button, LV_STATE_CHECKED, flag_mode);
    });
    fill(flag_button, 0xf7f4ec); edge(flag_button, 0x929596, 3);
    lv_obj_set_style_bg_color(flag_button, lv_color_hex(0xffcd80), LV_STATE_CHECKED);
    lv_obj_center(label(flag_button, "插旗"));
    auto reset = button(body, w - 76, 5, 66, 27, new_game);
    fill(reset, 0xf7f4ec); edge(reset, 0x929596, 3);
    lv_obj_center(label(reset, "新一局"));
    int size = std::min(w - 12, h - 43) / Minesweeper::side;
    int left = (w - size * Minesweeper::side) / 2;
    for (int i = 0; i < Minesweeper::cells; ++i) {
        auto tile = button(body, left + i % Minesweeper::side * size, 37 + i / Minesweeper::side * size,
                           size, size, play_tile, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
        edge(tile, 0x83909b);
        cell_labels[i] = label(tile, "");
        lv_obj_center(cell_labels[i]);
    }
    new_game(nullptr);
}

void select_wallpaper(lv_event_t *e)
{
    bool meadow = lv_event_get_user_data(e) != nullptr;
    if (meadow) lv_obj_remove_flag(wallpaper, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(wallpaper, LV_OBJ_FLAG_HIDDEN);
}

void details(lv_obj_t *body, int w)
{
    auto icon = desktop_icon(body, current->icon, 42);
    lv_obj_set_pos(icon, 16, 18);
    lv_obj_set_pos(label(body, current->name), 70, 27);
    fill(box(body, 16, 69, w - 32, 1), 0xb7b4a6);
    if (current == &clock_app) {
        date_label = label(body, "", 0x174a88, &lv_font_montserrat_24);
        lv_obj_set_pos(date_label, 22, 88);
        tick(nullptr);
    } else if (current == &computer) {
        char info[128];
        snprintf(info, sizeof(info), "TuringOS\n\n显示分辨率   %d x %d\n图形系统       LVGL %d.%d",
                 width, height, LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR);
        lv_obj_set_pos(label(body, info), 20, 86);
    } else {
        lv_obj_set_pos(label(body, "桌面背景"), 18, 85);
        const char *names[] = {"绿色山丘", "经典蓝色"};
        for (int i = 0; i < 2; ++i) {
            int bw = (w - 40) / 2;
            auto b = button(body, 16 + i * (bw + 8), 115, bw, 40, select_wallpaper,
                            i == 0 ? body : nullptr);
            gradient(b, white, 0xe2dfcf); edge(b, 0x929392, 3);
            lv_obj_center(label(b, names[i]));
        }
    }
}

void close_window(lv_event_t *)
{
    if (!window) return;
    date_label = nullptr;
    result = nullptr;
    lv_obj_delete(window);
    window = nullptr;
    current = nullptr;
    lv_obj_add_flag(task, LV_OBJ_FLAG_HIDDEN);
}

void drag_window(lv_event_t *)
{
    lv_point_t v;
    lv_indev_get_vect(lv_indev_active(), &v);
    const int max_x = std::max(0, width - lv_obj_get_width(window));
    const int max_y = std::max(0, height - bar_height - lv_obj_get_height(window));
    lv_obj_set_pos(window, std::clamp(lv_obj_get_x(window) + v.x, 0, max_x),
                   std::clamp(lv_obj_get_y(window) + v.y, 0, max_y));
}

void open_app(lv_event_t *e)
{
    const auto app = static_cast<const App *>(lv_event_get_user_data(e));
    show_menu(false);
    if (app == current) {
        lv_obj_remove_flag(window, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    close_window(nullptr);
    current = app;
    int w = std::min(width - 16, app->w);
    int h = std::min(height - bar_height - 8, app->h);
    window = box(desktop, (width - w) / 2, (height - bar_height - h) / 2, w, h);
    fill(window, 0x1555bb); edge(window, 0x073a90, 6);
    auto title = box(window, 2, 1, w - 6, 30);
    gradient(title, 0x4799ff, 0x0751bc);
    lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(title, drag_window, LV_EVENT_PRESSING, nullptr);
    lv_obj_set_pos(desktop_icon(title, app->icon, 21), 5, 4);
    lv_obj_align(label(title, app->name, white), LV_ALIGN_LEFT_MID, 32, 0);
    auto minimize = button(title, w - 62, 3, 23, 23, [](lv_event_t *) {
        lv_obj_add_flag(window, LV_OBJ_FLAG_HIDDEN);
    });
    gradient(minimize, 0x70adff, 0x2e6fd8); edge(minimize, white, 3);
    lv_obj_center(label(minimize, LV_SYMBOL_MINUS, white, LV_FONT_DEFAULT));
    auto close = button(title, w - 35, 3, 23, 23, close_window);
    gradient(close, 0xffb292, 0xd14424); edge(close, white, 3);
    lv_obj_center(label(close, LV_SYMBOL_CLOSE, white, LV_FONT_DEFAULT));
    auto body = box(window, 3, 32, w - 8, h - 37);
    fill(body, 0xece9d8);
    if (app == &calculator) calculator_view(body, w - 8);
    else if (app == &mines_app) mines_view(body, w - 8, h - 37);
    else details(body, w - 8);
    lv_label_set_text(task_label, app->name);
    lv_obj_remove_flag(task, LV_OBJ_FLAG_HIDDEN);
}

void link_row(lv_obj_t *parent, const App &app, int x, int y, int w, int h)
{
    auto row = button(parent, x, y, w, h, open_app, const_cast<App *>(&app));
    lv_obj_set_style_bg_color(row, lv_color_hex(0xb5d6fd), LV_STATE_HOVERED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_HOVERED);
    lv_obj_align(desktop_icon(row, app.icon, 32), LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_align(label(row, app.name), LV_ALIGN_LEFT_MID, 46, 0);
}

void make_menu()
{
    menu_layer = box(desktop, 0, 0, width, height - bar_height);
    lv_obj_add_flag(menu_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(menu_layer, [](lv_event_t *) { show_menu(false); }, LV_EVENT_CLICKED, nullptr);
    int w = std::min(width - 4, 348), h = std::min(height - bar_height, 274);
    int half = (w - 4) / 2;
    auto menu = box(menu_layer, 0, height - bar_height - h, w, h);
    lv_obj_add_flag(menu, LV_OBJ_FLAG_CLICKABLE);
    fill(menu, 0x1a5bbc); edge(menu, 0x154498, 5);
    auto header = box(menu, 1, 1, w - 4, 58);
    gradient(header, 0x1464c8, 0x408dea);
    lv_obj_set_pos(desktop_icon(header, Icon::user, 44), 9, 6);
    lv_obj_align(label(header, "TuringOS", white, &lv_font_montserrat_20), LV_ALIGN_LEFT_MID, 62, 0);
    fill(box(menu, 1, 59, w - 4, 2), 0xf5ae41);
    auto left = box(menu, 1, 61, half, h - 103);
    auto right = box(menu, 1 + half, 61, w - half - 4, h - 103);
    fill(left, white); fill(right, 0xd4e5f9);
    lv_obj_set_pos(label(left, "程序", 0x375d86), 12, 8);
    lv_obj_set_pos(label(right, "系统", 0x375d86), 12, 8);
    int row_h = std::min(46, (h - 103 - 30) / 3);
    link_row(left, calculator, 1, 30, half - 2, row_h);
    link_row(left, clock_app, 1, 30 + row_h, half - 2, row_h);
    link_row(left, mines_app, 1, 30 + 2 * row_h, half - 2, row_h);
    link_row(right, computer, 1, 30, half - 2, row_h);
    link_row(right, display, 1, 30 + row_h, half - 2, row_h);
    auto footer = box(menu, 1, h - 42, w - 4, 39);
    gradient(footer, 0x438ee5, 0x1a64c8);
    auto quit = button(footer, w - 118, 2, 108, 34, [](lv_event_t *) { lv_port_quit(); });
    lv_obj_align(desktop_icon(quit, Icon::exit, 25), LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_align(label(quit, "退出桌面", white), LV_ALIGN_LEFT_MID, 36, 0);
    show_menu(false);
}

void shortcut(const App &app, int y, int x = 7)
{
    auto obj = button(desktop, x, y, 90, 70, open_app, const_cast<App *>(&app));
    lv_obj_align(desktop_icon(obj, app.icon, 39), LV_ALIGN_TOP_MID, 0, 1);
    lv_obj_align(label(obj, app.name, 0x20344a), LV_ALIGN_TOP_MID, 1, 46);
    lv_obj_align(label(obj, app.name, white), LV_ALIGN_TOP_MID, 0, 45);
}

void create_ui()
{
    auto screen = lv_screen_active();
    width = lv_display_get_horizontal_resolution(nullptr);
    height = lv_display_get_vertical_resolution(nullptr);
    desktop = box(screen, 0, 0, width, height - bar_height);
    fill(desktop, 0x356da0);
    static lv_image_dsc_t meadow{};
    meadow.header.magic = LV_IMAGE_HEADER_MAGIC;
    meadow.header.cf = LV_COLOR_FORMAT_RGB565;
    meadow.header.w = 800; meadow.header.h = 600; meadow.header.stride = 1600;
    meadow.data_size = 960000; meadow.data = _binary_meadow_rgb565_start;
    wallpaper = lv_image_create(desktop);
    lv_image_set_src(wallpaper, &meadow);
    // Round the fixed-point scale up to cover the screen at non-native resolutions.
    int scale = std::max((width * LV_SCALE_NONE + 799) / 800,
                         (height * LV_SCALE_NONE + 599) / 600);
    lv_image_set_pivot(wallpaper, 0, 0);
    lv_image_set_scale(wallpaper, scale);
    lv_obj_set_pos(wallpaper, (width - 800 * scale / LV_SCALE_NONE) / 2,
                   (height - 600 * scale / LV_SCALE_NONE) / 2);
    shortcut(computer, 13); shortcut(calculator, 94); shortcut(display, 175);
    if (height - bar_height >= 326) shortcut(mines_app, 256);
    else shortcut(mines_app, 13, 104);

    bar = box(screen, 0, height - bar_height, width, bar_height);
    gradient(bar, 0x367eec, 0x1954c1);
    fill(box(bar, 0, 0, width, 2), 0x79acfc);
    start = button(bar, -3, 0, 105, bar_height, [](lv_event_t *) {
        show_menu(lv_obj_has_flag(menu_layer, LV_OBJ_FLAG_HIDDEN));
    });
    gradient(start, 0x78c961, 0x29822c); edge(start, 0x317229, 8);
    lv_obj_set_style_bg_color(start, lv_color_hex(0x3b8b38), LV_STATE_CHECKED);
    lv_obj_set_pos(desktop_icon(start, Icon::flag, 30), 10, 2);
    lv_obj_align(label(start, "开始", white), LV_ALIGN_LEFT_MID, 48, 0);
    task = button(bar, 108, 4, std::min(152, width - 210), 28, [](lv_event_t *) {
        show_menu(false);
        if (lv_obj_has_flag(window, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(window, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(window, LV_OBJ_FLAG_HIDDEN);
    });
    gradient(task, 0x4388e7, 0x2564bf); edge(task, 0x1756ab, 3);
    task_label = label(task, "", white); lv_obj_center(task_label);
    lv_obj_add_flag(task, LV_OBJ_FLAG_HIDDEN);
    auto tray = button(bar, width - 91, 1, 91, bar_height - 1, open_app, const_cast<App *>(&clock_app));
    gradient(tray, 0x41acf3, 0x1385d4);
    lv_obj_align(desktop_icon(tray, Icon::calendar, 17), LV_ALIGN_LEFT_MID, 8, 0);
    time_label = label(tray, "--:--", white, &lv_font_montserrat_14);
    lv_obj_align(time_label, LV_ALIGN_RIGHT_MID, -10, 0);
    tick(nullptr);
    lv_timer_create(tick, 1000, nullptr);
    make_menu();

    for (auto indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev))
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER)
            lv_indev_set_cursor(indev, desktop_icon(lv_layer_sys(), Icon::cursor, 24));
}
}

int main()
{
    return lv_port_run("lvgl-desktop", create_ui);
}
