#include "components.hpp"

namespace ui::c {

static constexpr lv_coord_t kW = 410;
static constexpr lv_coord_t kPad = 12;

static void bubble(lv_obj_t* o) {
  lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static void clickable(lv_obj_t* o) {
  lv_obj_add_flag(
      o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE |
                                    LV_OBJ_FLAG_EVENT_BUBBLE));
}

lv_obj_t* header(lv_obj_t* scr, const char* title, bool back_btn) {
  const Theme& t = theme();
  lv_obj_t* h = lv_obj_create(scr);
  bubble(h);
  lv_obj_set_size(h, kW - 2 * kPad, t.tap_min);
  lv_obj_set_pos(h, kPad, 8);
  lv_obj_set_style_bg_opa(h, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(h, 0, 0);
  lv_obj_set_style_pad_all(h, 0, 0);
  lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);

  if (back_btn) {
    lv_obj_t* b = lv_button_create(h);
    clickable(b);
    lv_obj_set_size(b, 76, 44);
    lv_obj_set_pos(b, 0, 6);
    lv_obj_set_style_radius(b, t.radius_sm, 0);
    lv_obj_set_style_bg_color(b, t.surface2, 0);
    lv_obj_t* l = lv_label_create(b);
    bubble(l);
    lv_label_set_text(l, "戻る");
    lv_obj_set_style_text_font(l, t.font_body, 0);
    lv_obj_set_style_text_color(l, t.text, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(
        b, [](lv_event_t*) { ui::emit(watch::ActionType::Back); },
        LV_EVENT_CLICKED, nullptr);
  }

  lv_obj_t* tl = lv_label_create(h);
  bubble(tl);
  lv_label_set_text(tl, title);
  lv_obj_set_style_text_font(tl, t.font_title, 0);
  lv_obj_set_style_text_color(tl, t.text, 0);
  if (back_btn) {
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 88, 0);
  } else {
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 4, 0);
  }
  return h;
}

lv_obj_t* content(lv_obj_t* scr) {
  const Theme& t = theme();
  lv_obj_t* c = lv_obj_create(scr);
  bubble(c);
  lv_obj_set_size(c, kW - 2 * kPad, 502 - 8 - t.tap_min - 16);
  lv_obj_set_pos(c, kPad, 8 + t.tap_min + 8);
  lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_pad_all(c, 0, 0);
  lv_obj_set_style_pad_row(c, t.space, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_scroll_dir(c, LV_DIR_VER);
  lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

static lv_obj_t* mk_button(lv_obj_t* parent, const char* text,
                           lv_event_cb_t cb, void* ud, lv_color_t bg,
                           lv_color_t fg) {
  const Theme& t = theme();
  lv_obj_t* b = lv_button_create(parent);
  clickable(b);
  lv_obj_set_size(b, kW - 2 * kPad - 16, t.tap_min);
  lv_obj_set_style_radius(b, t.radius_sm, 0);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = lv_label_create(b);
  bubble(l);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, t.font_body, 0);
  lv_obj_set_style_text_color(l, fg, 0);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}

lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                 void* ud) {
  const Theme& t = theme();
  return mk_button(parent, text, cb, ud, t.surface2, t.text);
}

lv_obj_t* button_primary(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                         void* ud) {
  const Theme& t = theme();
  return mk_button(parent, text, cb, ud, t.primary, t.on_primary);
}

lv_obj_t* button_danger(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                        void* ud) {
  const Theme& t = theme();
  return mk_button(parent, text, cb, ud, t.danger, t.on_primary);
}

lv_obj_t* list_row(lv_obj_t* parent, const char* text, const char* sub,
                   lv_event_cb_t cb, void* ud) {
  const Theme& t = theme();
  lv_obj_t* r = lv_obj_create(parent);
  clickable(r);
  lv_obj_set_size(r, kW - 2 * kPad - 16, t.tap_min);
  lv_obj_set_style_radius(r, t.radius_sm, 0);
  lv_obj_set_style_bg_color(r, t.surface, 0);
  lv_obj_set_style_border_width(r, 0, 0);
  lv_obj_set_style_pad_left(r, 14, 0);
  lv_obj_set_style_pad_right(r, 14, 0);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* l = lv_label_create(r);
  bubble(l);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, t.font_body, 0);
  lv_obj_set_style_text_color(l, t.text, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_width(l, sub ? 200 : kW - 2 * kPad - 44);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

  if (sub) {
    lv_obj_t* s = lv_label_create(r);
    bubble(s);
    lv_label_set_text(s, sub);
    lv_obj_set_style_text_font(s, t.font_body, 0);
    lv_obj_set_style_text_color(s, t.text_dim, 0);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s, 150);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s, LV_ALIGN_RIGHT_MID, 0, 0);
  }
  if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
  return r;
}

lv_obj_t* card(lv_obj_t* parent) {
  const Theme& t = theme();
  lv_obj_t* c = lv_obj_create(parent);
  bubble(c);
  lv_obj_set_size(c, kW - 2 * kPad - 16, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(c, t.radius_lg, 0);
  lv_obj_set_style_bg_color(c, t.surface, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_pad_all(c, 12, 0);
  lv_obj_set_style_pad_row(c, t.space, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_START);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

lv_obj_t* slider_row(lv_obj_t* parent, const char* label, int32_t min,
                     int32_t max, int32_t value, lv_event_cb_t cb, void* ud) {
  const Theme& t = theme();
  lv_obj_t* r = card(parent);
  lv_obj_t* l = lv_label_create(r);
  bubble(l);
  lv_label_set_text(l, label);
  lv_obj_set_style_text_font(l, t.font_body, 0);
  lv_obj_set_style_text_color(l, t.text, 0);

  lv_obj_t* s = lv_slider_create(r);
  clickable(s);
  lv_obj_set_size(s, LV_PCT(100), 28);
  lv_slider_set_range(s, min, max);
  lv_slider_set_value(s, value, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(s, t.surface2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, t.primary, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, t.primary, LV_PART_KNOB);
  lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
  if (cb) lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, ud);
  return r;
}

lv_obj_t* slider_of(lv_obj_t* row) { return lv_obj_get_child(row, 1); }

lv_obj_t* line(lv_obj_t* parent, const char* text) {
  const Theme& t = theme();
  lv_obj_t* l = lv_label_create(parent);
  bubble(l);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, t.font_body, 0);
  lv_obj_set_style_text_color(l, t.text_dim, 0);
  return l;
}

lv_obj_t* status_dot(lv_obj_t* parent, lv_color_t col) {
  lv_obj_t* d = lv_obj_create(parent);
  bubble(d);
  lv_obj_set_size(d, 14, 14);
  lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(d, col, 0);
  lv_obj_set_style_border_width(d, 0, 0);
  lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
  return d;
}

void set_dot(lv_obj_t* dot, lv_color_t col) {
  lv_obj_set_style_bg_color(dot, col, 0);
}

}  // namespace ui::c
