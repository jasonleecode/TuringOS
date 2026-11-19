#include "icons.h"
#include <cstdint>

namespace {
// Small vector icons share a 32 × 32 coordinate system at every UI scale.
struct Painter {
    lv_layer_t *layer;
    lv_area_t bounds;
    int size;

    int x(int v) const { return bounds.x1 + v * size / 32; }
    int y(int v) const { return bounds.y1 + v * size / 32; }

    void rect(int left, int top, int right, int bottom, unsigned color) const
    {
        lv_area_t area = {x(left), y(top), x(right) - 1, y(bottom) - 1};
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(color);
        d.bg_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &d, &area);
    }

    void tri(int ax, int ay, int bx, int by, int cx, int cy, unsigned color) const
    {
        lv_draw_triangle_dsc_t d;
        lv_draw_triangle_dsc_init(&d);
        d.bg_color = lv_color_hex(color);
        d.bg_opa = LV_OPA_COVER;
        d.p[0] = {x(ax), y(ay)};
        d.p[1] = {x(bx), y(by)};
        d.p[2] = {x(cx), y(cy)};
        lv_draw_triangle(layer, &d);
    }
};

void draw(lv_event_t *e)
{
    auto obj = lv_event_get_target_obj(e);
    Painter p = {lv_event_get_layer(e), {}, lv_obj_get_width(obj)};
    lv_obj_get_coords(obj, &p.bounds);
    switch (static_cast<Icon>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)))) {
    case Icon::computer:
        p.rect(3, 3, 29, 23, 0x3c5368); p.rect(3, 2, 28, 21, 0xe2ebeb);
        p.rect(5, 4, 26, 19, 0x426781); p.rect(6, 5, 25, 18, 0x3485c8);
        p.tri(6, 5, 24, 5, 6, 15, 0x81d8f3);
        p.rect(13, 22, 19, 25, 0x9aa7b1);
        p.rect(4, 25, 27, 30, 0x546d82); p.rect(3, 24, 26, 28, 0xdbe4e9);
        p.rect(18, 25, 23, 26, 0x567185); break;
    case Icon::calculator:
        p.rect(5, 2, 28, 31, 0x45566c); p.rect(4, 1, 26, 29, 0xc3cfdb);
        p.rect(6, 3, 24, 12, 0x677a85); p.rect(7, 4, 23, 11, 0xe1f1c1);
        p.rect(18, 6, 21, 9, 0x475d3b);
        for (int y = 14; y < 26; y += 5)
            for (int x = 7; x <= 20; x += 6)
                p.rect(x, y, x + 4, y + 3, x == 19 ? 0xf18a4a : 0x47749f);
        break;
    case Icon::display:
        p.rect(2, 4, 30, 27, 0x80572e); p.rect(2, 3, 29, 25, 0xffdc84);
        p.rect(5, 6, 26, 22, 0x6eacec); p.rect(19, 8, 23, 11, 0xffee96);
        p.tri(5, 21, 12, 11, 24, 21, 0x428b5e);
        p.tri(12, 21, 20, 14, 26, 21, 0x7cb34b); break;
    case Icon::calendar:
        p.rect(3, 4, 29, 30, 0x586e85); p.rect(2, 3, 27, 28, 0xfffcf0);
        p.rect(2, 3, 27, 10, 0xe46b50); p.rect(7, 1, 10, 6, 0x607789);
        p.rect(20, 1, 23, 6, 0x607789);
        for (int y = 13; y <= 22; y += 5)
            for (int x = 6; x <= 22; x += 5) p.rect(x, y, x + 3, y + 3, 0x6489ae);
        break;
    case Icon::flag: {
        unsigned colors[] = {0xf87839, 0x9cd832, 0x59b5ef, 0xffda42};
        for (int i = 0; i < 4; ++i) {
            int x = 4 + (i % 2) * 13 - (i / 2) * 2;
            int y = 5 + (i / 2) * 12 - (i % 2) * 2;
            p.tri(x, y + 2, x + 12, y, x + 10, y + 10, colors[i]);
            p.tri(x, y + 2, x + 10, y + 10, x - 2, y + 12, colors[i]);
        }
        break;
    }
    case Icon::user:
        p.rect(1, 1, 31, 31, 0xffffff); p.rect(3, 3, 29, 29, 0x7cbae7);
        p.rect(11, 7, 22, 18, 0xfbd096); p.rect(11, 6, 22, 10, 0x92623d);
        p.tri(4, 28, 15, 16, 27, 28, 0x315b9a); break;
    case Icon::exit:
        p.rect(2, 3, 30, 29, 0xb3472b); p.rect(3, 3, 28, 27, 0xee8654);
        p.rect(7, 13, 19, 17, 0xffffff); p.tri(17, 8, 25, 15, 17, 22, 0xffffff); break;
    case Icon::cursor:
        p.tri(0, 0, 0, 27, 19, 18, 0x15243a);
        p.tri(4, 13, 11, 31, 16, 28, 0x15243a);
        p.tri(2, 4, 2, 23, 15, 17, 0xffffff);
        p.tri(6, 14, 11, 28, 13, 26, 0xffffff); break;
    }
}
}

lv_obj_t *desktop_icon(lv_obj_t *parent, Icon icon, int size)
{
    auto obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, size, size);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj, draw, LV_EVENT_DRAW_MAIN,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(icon)));
    return obj;
}
