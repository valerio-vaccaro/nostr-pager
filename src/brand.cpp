#include "brand.h"
namespace {
void draw(lv_event_t *event) {
    auto *object = lv_event_get_target(event);
    auto *context = lv_event_get_draw_ctx(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    const int size = lv_obj_get_width(object);
    auto point = [&](int x, int y) {
        return lv_point_t{lv_coord_t(area.x1 + x * size / 64), lv_coord_t(area.y1 + y * size / 64)};
    };
    auto rectangle = [&](int x1, int y1, int x2, int y2, int radius, uint32_t color, int border = 0) {
        auto a = point(x1, y1), b = point(x2, y2);
        lv_area_t bounds{a.x, a.y, b.x, b.y};
        lv_draw_rect_dsc_t style;
        lv_draw_rect_dsc_init(&style);
        style.radius = radius * size / 64;
        style.bg_color = lv_color_hex(color);
        style.border_color = lv_color_hex(0x68e0c4);
        style.border_width = border ? LV_MAX(1, size / 32) : 0;
        lv_draw_rect(context, &style, &bounds);
    };
    rectangle(0, 0, 63, 63, 18, 0x19372f);
    rectangle(12, 15, 51, 49, 8, 0x0d191a, 1);
    rectangle(19, 23, 44, 36, 3, 0x68e0c4);
    rectangle(19, 42, 28, 43, 1, 0x68e0c4);
    rectangle(34, 42, 43, 43, 1, 0x68e0c4);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(0xb7a1ff);
    line.width = LV_MAX(2, size / 20);
    line.round_start = line.round_end = 1;
    auto a = point(43, 9), b = point(51, 9), c = point(56, 14);
    lv_draw_line(context, &line, &a, &b);
    lv_draw_line(context, &line, &b, &c);
    a = point(24, 28); b = point(39, 28);
    line.color = lv_color_hex(0x19372f);
    line.width = LV_MAX(1, size / 32);
    lv_draw_line(context, &line, &a, &b);
    a = point(24, 32); b = point(33, 32);
    lv_draw_line(context, &line, &a, &b);
}
}
lv_obj_t *brand::logo(lv_obj_t *parent, int x, int y, int size) {
    auto *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, size, size);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(object, draw, LV_EVENT_DRAW_MAIN, nullptr);
    return object;
}
