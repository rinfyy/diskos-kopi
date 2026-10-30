/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* nt_panels.c - NTDisk's Quick Settings and volume panel (the layouts of Null's, in NTDisk's look).
 *
 * Quick Settings slides down over the screen you were on: the time (dot matrix) and date, eight pill tiles in two
 * columns (icon + short name; an active tile turns white - black in Light - with a red dot), the cover with
 * previous / play-pause / next, the battery on the left rim and brightness on the right, and a handle to close it
 * (or swipe up). Tap a tile for its action, hold it for its settings, as everywhere else.
 *
 * Volume: a card across the middle - a speaker, the level as a big dot-matrix number, a row of dots to drag or tap,
 * and - / + buttons for single steps. Tap outside or wait 2.5 s and it slides away. Theme roles only. */
#include "simple_theme.h"
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "ipc.h"
#include "quicksettings.h"
#include "idisc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static lv_obj_t *plain(lv_obj_t *p){
    lv_obj_t *o = lv_obj_create(p); lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}
static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, lv_color_t c, const char *s){
    lv_obj_t *l = lv_label_create(p); lv_label_set_text(l, s ? s : "");
    lv_obj_set_style_text_font(l, f, 0); lv_obj_set_style_text_color(l, c, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}
static void keep_all(lv_obj_t *o){ kit_keep(o); for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++) keep_all(lv_obj_get_child(o, (int32_t)i)); }
static void set_text(lv_obj_t *l, const char *s){ if(l && strcmp(lv_label_get_text(l), s ? s : "")) lv_label_set_text(l, s ? s : ""); }

/* a small dot-matrix number (5 x 7 digits, round dots), drawn in one object */
static const uint8_t DIG[10][7] = {
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}, {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
};
typedef struct { char s[12]; int cell; lv_color_t c; } dnum_t;
static int dnum_cols(const char *s){ int x = 0; for(; *s; s++){ if(*s >= '0' && *s <= '9'){ if(x) x++; x += 5; } else if(*s == ':'){ x += 2; } } return x; }
static void dnum_draw(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e); dnum_t *d = lv_obj_get_user_data(o); lv_layer_t *layer = lv_event_get_layer(e);
    if(!d || !layer) return;
    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r); r.radius = LV_RADIUS_CIRCLE; r.bg_color = d->c; r.bg_opa = LV_OPA_COVER;
    int x = 0, dot = d->cell - 1 > 2 ? d->cell - 1 : 2;
    for(const char *p = d->s; *p; p++){
        if(*p >= '0' && *p <= '9'){
            if(x) x++;
            for(int row = 0; row < 7; row++) for(int col = 0; col < 5; col++) if(DIG[*p - '0'][row] & (0x10 >> col)){
                lv_area_t c = { a.x1 + (x + col) * d->cell, a.y1 + row * d->cell, 0, 0 }; c.x2 = c.x1 + dot - 1; c.y2 = c.y1 + dot - 1;
                lv_draw_rect(layer, &r, &c);
            }
            x += 5;
        } else if(*p == ':'){
            x++;
            for(int row = 2; row <= 4; row += 2){ lv_area_t c = { a.x1 + x * d->cell, a.y1 + row * d->cell, 0, 0 }; c.x2 = c.x1 + dot - 1; c.y2 = c.y1 + dot - 1; lv_draw_rect(layer, &r, &c); }
            x++;
        }
    }
}
static void dnum_free(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static lv_obj_t *dnum_create(lv_obj_t *p, int cell, lv_color_t c){
    lv_obj_t *o = plain(p); dnum_t *d = calloc(1, sizeof *d);
    if(d){ d->cell = cell; d->c = c; }
    lv_obj_set_user_data(o, d);
    lv_obj_add_event_cb(o, dnum_draw, LV_EVENT_DRAW_MAIN, NULL); lv_obj_add_event_cb(o, dnum_free, LV_EVENT_DELETE, NULL);
    return o;
}
static void dnum_set(lv_obj_t *o, const char *s){
    dnum_t *d = o ? lv_obj_get_user_data(o) : NULL; if(!d || !s || !strcmp(d->s, s)) return;
    snprintf(d->s, sizeof d->s, "%s", s);
    lv_obj_set_size(o, dnum_cols(d->s) * d->cell, 7 * d->cell); lv_obj_invalidate(o);
}

/* ---- Quick Settings ------------------------------------------------------------------------------------------------ */
static const char *const SHORT[] = { "Wi-Fi", "BT", "Library", "Rescan", "Modes", "Settings", "Equalizer", "Search", "Queue",
                                     "Up next", "Shortcut", "Weather", "Lyrics", "Info", "Immerse", "Folders", "Faves",
                                     "Mode", "Gain", "Shuffle", "Sleep" };
static lv_obj_t *q_top, *q_tsep, *q_tbat, *q_tbat_fill, *q_tpct;
static lv_obj_t *q_root, *q_panel, *q_clock, *q_date, *q_tile[8], *q_icon[8], *q_name[8], *q_dot[8];
static lv_obj_t *q_cover, *q_art, *q_pp, *q_pp_l, *q_bright, *q_batt, *q_batt_fill, *q_batt_txt, *q_handle;
static int q_ids[8], q_n, q_open;
static lv_timer_t *q_tm;

static void tile_cb(lv_event_t *e){
    int k = (int)(intptr_t)lv_event_get_user_data(e); if(k < 0 || k >= q_n) return;
    lv_event_code_t c = lv_event_get_code(e);
    if(c == LV_EVENT_LONG_PRESSED) quicksettings_null_action(q_ids[k], 1);
    else if(c == LV_EVENT_CLICKED && !screen_press_was_long()){ quicksettings_null_action(q_ids[k], 0); nt_qs_refresh(); }
}
static void transport_cb(lv_event_t *e){
    const char *cmd = lv_event_get_user_data(e);
    ui_transport_command(cmd);
    nt_qs_refresh();
}
static void bright_cb(lv_event_t *e){
    int v = lv_slider_get_value(lv_event_get_target(e));
    if(lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) ui_backlight(v); else ui_set_brightness(v);
}
static void close_cb(lv_event_t *e){ (void)e; screen_back(); }
static void q_tick(lv_timer_t *t){ (void)t; if(screen_current() == SCR_QUICK) nt_qs_refresh(); else lv_timer_pause(q_tm); }
static lv_obj_t *round_btn(lv_obj_t *p, int x, int y, int d, const char *glyph, const char *cmd){
    lv_obj_t *b = plain(p); lv_obj_set_pos(b, x, y); lv_obj_set_size(b, d, d);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_bg_color(b, TC(SURFACE), 0); lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE); lv_obj_set_ext_click_area(b, 6);
    lv_obj_add_event_cb(b, transport_cb, LV_EVENT_CLICKED, (void *)cmd);
    lv_obj_t *l = label(b, TF(UI_16), TC(TEXT_PRIMARY), glyph); lv_obj_center(l);
    return b;
}
static const lv_font_t *icon_font(void){                                 /* LVGL's symbols, then the extra icons */
    static lv_font_t f, g; extern const lv_font_t font_weather16;
    if(!f.get_glyph_dsc){ g = *TF(ICON_20); g.fallback = &font_weather16; f = *theme_font_original(18); f.fallback = &g; }
    return &f;
}
static void qs_construct(void){
    if(q_panel) return;
    q_panel = plain(q_root); lv_obj_set_size(q_panel, 360, 360); lv_obj_set_pos(q_panel, 0, -360);
    lv_obj_set_style_bg_color(q_panel, TC(CANVAS), 0); lv_obj_set_style_bg_opa(q_panel, LV_OPA_COVER, 0);
    lv_obj_add_flag(q_panel, LV_OBJ_FLAG_CLICKABLE);                      /* taps never fall through */
    q_clock = dnum_create(q_panel, 4, TC(TEXT_PRIMARY)); lv_obj_align(q_clock, LV_ALIGN_TOP_MID, 0, 18); dnum_set(q_clock, "00:00");
    /* the top bar under the time: date · a small battery and its percentage */
    q_top = plain(q_panel); lv_obj_set_size(q_top, LV_SIZE_CONTENT, 16); lv_obj_align(q_top, LV_ALIGN_TOP_MID, 0, 49);
    lv_obj_set_flex_flow(q_top, LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(q_top, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(q_top, 6, 0);
    q_date = label(q_top, TF(UI_12), TC(TEXT_MUTED), "");
    q_tsep = plain(q_top); lv_obj_set_size(q_tsep, 3, 3); lv_obj_set_style_radius(q_tsep, 2, 0);
    lv_obj_set_style_bg_color(q_tsep, TC(ACCENT_PRIMARY), 0); lv_obj_set_style_bg_opa(q_tsep, LV_OPA_COVER, 0);
    q_tbat = plain(q_top); lv_obj_set_size(q_tbat, 20, 10); lv_obj_set_style_radius(q_tbat, 3, 0);
    lv_obj_set_style_border_width(q_tbat, 1, 0); lv_obj_set_style_border_color(q_tbat, TC(TEXT_MUTED), 0); lv_obj_set_style_pad_all(q_tbat, 2, 0);
    q_tbat_fill = plain(q_tbat); lv_obj_set_size(q_tbat_fill, 14, 4); lv_obj_set_style_radius(q_tbat_fill, 1, 0);
    lv_obj_set_style_bg_color(q_tbat_fill, TC(TEXT_PRIMARY), 0); lv_obj_set_style_bg_opa(q_tbat_fill, LV_OPA_COVER, 0); lv_obj_align(q_tbat_fill, LV_ALIGN_LEFT_MID, 0, 0);
    q_tpct = label(q_top, TF(UI_12), TC(TEXT_PRIMARY), "");
    lv_obj_add_flag(q_tsep, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(q_tbat, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(q_tpct, LV_OBJ_FLAG_HIDDEN);
    for(int k = 0; k < 8; k++){
        int col = k % 2, row = k / 2;
        lv_obj_t *t = q_tile[k] = plain(q_panel); lv_obj_set_size(t, 112, 44); lv_obj_set_pos(t, 62 + col * 124, 72 + row * 50);
        lv_obj_set_style_radius(t, 22, 0); lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k); lv_obj_add_event_cb(t, tile_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)k);
        q_icon[k] = label(t, icon_font(), TC(TEXT_PRIMARY), ""); lv_obj_set_width(q_icon[k], 28);
        lv_obj_set_style_text_align(q_icon[k], LV_TEXT_ALIGN_CENTER, 0); lv_obj_align(q_icon[k], LV_ALIGN_LEFT_MID, 10, 0);
        q_name[k] = label(t, TF(UI_12), TC(TEXT_PRIMARY), ""); lv_obj_align(q_name[k], LV_ALIGN_LEFT_MID, 42, 0);
        q_dot[k] = plain(t); lv_obj_set_size(q_dot[k], 6, 6); lv_obj_align(q_dot[k], LV_ALIGN_TOP_RIGHT, -12, 9);
        lv_obj_set_style_radius(q_dot[k], LV_RADIUS_CIRCLE, 0); lv_obj_set_style_bg_color(q_dot[k], TC(ACCENT_PRIMARY), 0); lv_obj_set_style_bg_opa(q_dot[k], LV_OPA_COVER, 0);
    }
    round_btn(q_panel, 96, 278, 40, LV_SYMBOL_PREV, "0201000C0002");
    round_btn(q_panel, 224, 278, 40, LV_SYMBOL_NEXT, "0201000C0001");
    q_cover = plain(q_panel); lv_obj_set_size(q_cover, 64, 64); lv_obj_set_pos(q_cover, 148, 266);
    lv_obj_set_style_radius(q_cover, 14, 0); lv_obj_set_style_clip_corner(q_cover, true, 0);
    lv_obj_set_style_bg_color(q_cover, TC(SURFACE), 0); lv_obj_set_style_bg_opa(q_cover, LV_OPA_COVER, 0);
    q_art = lv_image_create(q_cover); lv_obj_remove_flag(q_art, LV_OBJ_FLAG_CLICKABLE);
    q_pp = plain(q_cover); lv_obj_set_size(q_pp, 64, 64); lv_obj_add_flag(q_pp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(q_pp, TC(SCRIM), 0); lv_obj_set_style_bg_opa(q_pp, 90, 0); lv_obj_set_style_bg_opa(q_pp, 160, LV_STATE_PRESSED);
    lv_obj_add_event_cb(q_pp, transport_cb, LV_EVENT_CLICKED, (void *)"0201000C0000");
    q_pp_l = label(q_pp, TF(UI_20), TC(FIXED_MEDIA_WHITE), LV_SYMBOL_PLAY); lv_obj_center(q_pp_l);
    /* battery (left rim) and brightness (right rim) */
    q_batt = plain(q_panel); lv_obj_set_size(q_batt, 8, 140); lv_obj_set_pos(q_batt, 32, 106);
    lv_obj_set_style_radius(q_batt, 4, 0); lv_obj_set_style_bg_color(q_batt, TC(CONTROL_TRACK), 0); lv_obj_set_style_bg_opa(q_batt, LV_OPA_COVER, 0);
    q_batt_fill = plain(q_batt); lv_obj_set_width(q_batt_fill, 8); lv_obj_align(q_batt_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(q_batt_fill, 4, 0); lv_obj_set_style_bg_color(q_batt_fill, TC(TEXT_PRIMARY), 0); lv_obj_set_style_bg_opa(q_batt_fill, LV_OPA_COVER, 0);
    q_batt_txt = label(q_panel, TF(UI_12), TC(TEXT_MUTED), ""); lv_obj_set_width(q_batt_txt, 44);
    lv_obj_set_style_text_align(q_batt_txt, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_pos(q_batt_txt, 14, 252);
    q_bright = lv_slider_create(q_panel); lv_obj_remove_style_all(q_bright);
    lv_obj_set_size(q_bright, 8, 140); lv_obj_set_pos(q_bright, 320, 106); lv_obj_set_ext_click_area(q_bright, 16);
    lv_slider_set_range(q_bright, 4, 40); lv_slider_set_value(q_bright, ui_get_brightness(), LV_ANIM_OFF);
    lv_obj_set_style_radius(q_bright, 4, LV_PART_MAIN); lv_obj_set_style_bg_color(q_bright, TC(CONTROL_TRACK), LV_PART_MAIN); lv_obj_set_style_bg_opa(q_bright, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(q_bright, 4, LV_PART_INDICATOR); lv_obj_set_style_bg_color(q_bright, TC(TEXT_PRIMARY), LV_PART_INDICATOR); lv_obj_set_style_bg_opa(q_bright, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(q_bright, LV_RADIUS_CIRCLE, LV_PART_KNOB); lv_obj_set_style_bg_color(q_bright, TC(ACCENT_PRIMARY), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(q_bright, LV_OPA_COVER, LV_PART_KNOB); lv_obj_set_style_pad_all(q_bright, 3, LV_PART_KNOB);
    lv_obj_add_event_cb(q_bright, bright_cb, LV_EVENT_VALUE_CHANGED, NULL); lv_obj_add_event_cb(q_bright, bright_cb, LV_EVENT_RELEASED, NULL);
    extern const lv_font_t font_weather16;
    lv_obj_t *sun = label(q_panel, &font_weather16, TC(TEXT_MUTED), "\xEF\x86\x85"); lv_obj_set_pos(sun, 316, 254);
    /* the handle: tap (or swipe up) to close */
    q_handle = plain(q_panel); lv_obj_set_size(q_handle, 80, 26); lv_obj_align(q_handle, LV_ALIGN_TOP_MID, 0, 330);
    lv_obj_add_flag(q_handle, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(q_handle, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bar = plain(q_handle); lv_obj_set_size(bar, 36, 4); lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_radius(bar, 2, 0); lv_obj_set_style_bg_color(bar, TC(TEXT_MUTED), 0); lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    keep_all(q_panel);
}
void nt_qs_create(lv_obj_t *root){
    if(th_idisc()){ idisc_qs_create(root); return; }                    /* iDisc: its Control Center */
    q_root = root; q_panel = NULL;
    lv_obj_set_style_bg_opa(root, 0, 0);                                   /* the screen below shows while it slides */
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    qs_construct();
    if(!q_tm){ q_tm = lv_timer_create(q_tick, 1000, NULL); lv_timer_pause(q_tm); }
    nt_qs_build();
}
void nt_qs_build(void){
    if(th_idisc()){ idisc_qs_build(); return; }
    if(!q_root) return;
    qs_construct();
    int items[QS_MAX_ITEMS]; q_n = quicksettings_items_load(items); if(q_n > 8) q_n = 8;
    for(int k = 0; k < 8; k++){
        q_ids[k] = k < q_n ? items[k] : -1;
        if(k >= q_n){ lv_obj_add_flag(q_tile[k], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(q_tile[k], LV_OBJ_FLAG_HIDDEN);
        int id = q_ids[k];
        set_text(q_icon[k], quicksettings_item_glyph(id));
        set_text(q_name[k], id >= 0 && id < (int)(sizeof SHORT / sizeof *SHORT) ? SHORT[id] : "Item");
    }
    nt_qs_refresh();
}
void nt_qs_refresh(void){
    if(th_idisc()){ idisc_qs_refresh(); return; }
    if(!q_panel) return;
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char b[32]; int h24 = cfg_get_int("time_24h", 1);
    int hh = h24 ? tm.tm_hour : (tm.tm_hour % 12 ? tm.tm_hour % 12 : 12);
    snprintf(b, sizeof b, "%02d:%02d", hh, tm.tm_min); dnum_set(q_clock, b);
    strftime(b, sizeof b, "%a %d %b", &tm); for(char *p = b; *p; p++) if(*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    set_text(q_date, b);
    for(int k = 0; k < q_n; k++){
        int on = quicksettings_item_active(q_ids[k]);
        lv_color_t bg = on ? TC(TEXT_PRIMARY) : TC(SURFACE), fg = on ? TC(CANVAS) : TC(TEXT_PRIMARY);
        if(!lv_color_eq(lv_obj_get_style_bg_color(q_tile[k], 0), bg)){
            lv_obj_set_style_bg_color(q_tile[k], bg, 0);
            lv_obj_set_style_text_color(q_icon[k], fg, 0); lv_obj_set_style_text_color(q_name[k], fg, 0);
        }
        if(on) lv_obj_remove_flag(q_dot[k], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(q_dot[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(q_tile[k], on ? TC(PRIMARY_PRESSED) : TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    }
    set_text(q_pp_l, ui_is_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    if(q_bright && !lv_obj_has_state(q_bright, LV_STATE_PRESSED)) lv_slider_set_value(q_bright, ui_get_brightness(), LV_ANIM_OFF);
}
void nt_qs_art(const void *cover){
    if(th_idisc()){ idisc_qs_art(cover); return; }
    if(!q_art) return;
    lv_image_set_src(q_art, NULL);
    if(cover){
        const lv_image_dsc_t *d = cover; lv_image_set_src(q_art, cover);
        if(d->header.w) lv_image_set_scale(q_art, (uint32_t)(64 * 256 / d->header.w) + 1);
        lv_obj_center(q_art); lv_obj_remove_flag(q_art, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(q_art, LV_OBJ_FLAG_HIDDEN);
}
void nt_qs_battery(int pct, int charging){
    if(th_idisc()){ idisc_qs_battery(pct, charging); return; }
    if(!q_batt_fill || pct < 0) return;
    if(pct > 100) pct = 100;
    lv_obj_set_height(q_batt_fill, 140 * pct / 100 < 8 ? 8 : 140 * pct / 100);
    lv_obj_set_style_bg_color(q_batt_fill, charging ? TC(STATUS_SUCCESS) : pct <= 15 ? TC(ACCENT_PRIMARY) : TC(TEXT_PRIMARY), 0);
    char b[16]; snprintf(b, sizeof b, "%s%d%%", charging ? LV_SYMBOL_CHARGE : "", pct); set_text(q_batt_txt, b);
    if(q_tbat){                                                            /* the top bar */
        lv_obj_remove_flag(q_tsep, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(q_tbat, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(q_tpct, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(q_tbat_fill, 14 * pct / 100 < 2 ? 2 : 14 * pct / 100);
        lv_obj_set_style_bg_color(q_tbat_fill, charging ? TC(STATUS_SUCCESS) : pct <= 15 ? TC(ACCENT_PRIMARY) : TC(TEXT_PRIMARY), 0);
        if(charging) lv_obj_set_style_text_font(q_tpct, theme_font_original(12), 0);
        snprintf(b, sizeof b, "%s%d%%", charging ? LV_SYMBOL_CHARGE " " : "", pct); set_text(q_tpct, b);
    }
}
static void q_pos(void *o, int32_t v){ lv_obj_set_y(o, v); }
static void q_hidden(lv_anim_t *a){ (void)a; if(!q_open && q_root) lv_obj_add_flag(q_root, LV_OBJ_FLAG_HIDDEN); }
void nt_qs_slide(int open){
    if(th_idisc()){ idisc_qs_slide(open); return; }
    if(!q_panel) return;
    q_open = !!open;
    if(open){ nt_qs_refresh(); lv_timer_resume(q_tm); }
    lv_anim_delete(q_panel, q_pos);
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, q_panel); lv_anim_set_exec_cb(&a, q_pos);
    lv_anim_set_values(&a, lv_obj_get_y(q_panel), open ? 0 : -360);
    lv_anim_set_duration(&a, screen_get_anim() ? 220 : 1); lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, q_hidden); lv_anim_start(&a);
}

/* ---- volume -------------------------------------------------------------------------------------------------------- */
#define V_DOTS 24
static lv_obj_t *v_root, *v_card, *v_num, *v_meter, *v_icon;
static lv_timer_t *v_tm;
static int v_val, v_pending, v_closing; static uint32_t v_sent;
static int vmax(void){ int m = simple_cfg_get_int("max_vol", VOL_MAX); if(m < 1) m = 1; if(m > VOL_MAX) m = VOL_MAX; return m; }
static void v_send(int v){
    if(lv_tick_elaps(v_sent) >= 120){ ui_set_volume(v); v_sent = lv_tick_get(); v_pending = 0; } else v_pending = 1;
}
static void v_paint(void){
    char b[8]; snprintf(b, sizeof b, "%d", v_val); dnum_set(v_num, b);
    lv_obj_align(v_num, LV_ALIGN_TOP_MID, 0, 18);
    set_text(v_icon, v_val == 0 ? LV_SYMBOL_MUTE : v_val < vmax() / 2 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
    lv_obj_invalidate(v_meter);
}
static void meter_draw(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e); lv_layer_t *layer = lv_event_get_layer(e); if(!layer) return;
    lv_area_t a; lv_obj_get_coords(o, &a);
    int lit = v_val <= 0 ? 0 : (v_val * V_DOTS + vmax() - 1) / vmax(); if(lit > V_DOTS) lit = V_DOTS;
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r); r.radius = LV_RADIUS_CIRCLE; r.bg_opa = LV_OPA_COVER;
    int w = lv_area_get_width(&a), step = w / V_DOTS, cy = (a.y1 + a.y2) / 2;
    for(int i = 0; i < V_DOTS; i++){
        int big = i == lit - 1, d = big ? 10 : 6, cx = a.x1 + step * i + step / 2;
        r.bg_color = i < lit ? (big ? TC(ACCENT_PRIMARY) : TC(TEXT_PRIMARY)) : TC(CONTROL_TRACK);
        lv_area_t c = { cx - d / 2, cy - d / 2, cx - d / 2 + d - 1, cy - d / 2 + d - 1 };
        lv_draw_rect(layer, &r, &c);
    }
}
static void v_close(void);
static void meter_cb(lv_event_t *e){
    lv_event_code_t c = lv_event_get_code(e); lv_indev_t *in = lv_indev_active(); if(!in) return;
    lv_point_t p; lv_indev_get_point(in, &p);
    lv_area_t a; lv_obj_get_coords(v_meter, &a);
    int w = lv_area_get_width(&a), v = (int)((long)(p.x - a.x1) * vmax() / (w > 1 ? w : 1));
    if(v < 0) v = 0;
    if(v > vmax()) v = vmax();
    if(c == LV_EVENT_PRESSED || c == LV_EVENT_PRESSING){ if(v != v_val){ v_val = v; v_paint(); v_send(v); } }
    else if((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && v_pending){ ui_set_volume(v_val); v_sent = lv_tick_get(); v_pending = 0; }
    lv_timer_reset(v_tm);
}
static void step_cb(lv_event_t *e){
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int v = v_val + d; if(v < 0) v = 0; if(v > vmax()) v = vmax();
    if(v != v_val){ v_val = v; v_paint(); ui_set_volume(v); v_sent = lv_tick_get(); }
    lv_timer_reset(v_tm);
}
static void outside_cb(lv_event_t *e){ if(lv_event_get_target(e) == v_root) v_close(); }
static void v_opa(void *o, int32_t v){ lv_obj_set_style_opa(o, (lv_opa_t)v, 0); lv_obj_set_y(v_card, 128 + (255 - v) * 12 / 255); }
static void v_hidden(lv_anim_t *a){ (void)a; v_closing = 0; lv_obj_add_flag(v_root, LV_OBJ_FLAG_HIDDEN); }
static void v_close(void){
    if(!v_root || v_closing || lv_obj_has_flag(v_root, LV_OBJ_FLAG_HIDDEN)) return;
    lv_timer_pause(v_tm); v_closing = 1;
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, v_root); lv_anim_set_exec_cb(&a, v_opa);
    lv_anim_set_values(&a, 255, 0); lv_anim_set_duration(&a, screen_get_anim() ? 140 : 1);
    lv_anim_set_completed_cb(&a, v_hidden); lv_anim_start(&a);
}
static void v_timeout(lv_timer_t *t){
    if(v_meter && lv_obj_has_state(v_meter, LV_STATE_PRESSED)){ lv_timer_reset(t); return; }
    v_close();
}
static lv_obj_t *step_btn(lv_obj_t *p, int x, const char *g, int d){
    lv_obj_t *b = plain(p); lv_obj_set_size(b, 44, 44); lv_obj_set_pos(b, x, 34);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_bg_color(b, TC(SURFACE_RAISED), 0); lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE); lv_obj_set_ext_click_area(b, 8);
    lv_obj_add_event_cb(b, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)d);
    lv_obj_t *l = label(b, TF(UI_24), TC(TEXT_PRIMARY), g); lv_obj_center(l);
    return b;
}
void nt_volume_show(int vol){
    if(th_idisc()){ idisc_volume_show(vol); return; }
    if(!v_root){
        v_root = plain(lv_layer_top()); lv_obj_set_size(v_root, 360, 360);
        lv_obj_set_style_bg_color(v_root, TC(SCRIM), 0); lv_obj_set_style_bg_opa(v_root, 90, 0);
        lv_obj_add_flag(v_root, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(v_root, outside_cb, LV_EVENT_CLICKED, NULL);
        v_card = plain(v_root); lv_obj_set_size(v_card, 316, 112); lv_obj_set_pos(v_card, 22, 128);
        lv_obj_set_style_radius(v_card, 56, 0); lv_obj_set_style_bg_color(v_card, TC(SURFACE), 0); lv_obj_set_style_bg_opa(v_card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(v_card, 1, 0); lv_obj_set_style_border_color(v_card, TC(BORDER), 0);
        lv_obj_add_flag(v_card, LV_OBJ_FLAG_CLICKABLE);                    /* a tap on the card keeps it open */
        v_icon = label(v_card, TF(UI_14), TC(TEXT_MUTED), LV_SYMBOL_VOLUME_MAX); lv_obj_align(v_icon, LV_ALIGN_TOP_MID, -58, 22);
        v_num = dnum_create(v_card, 5, TC(TEXT_PRIMARY));
        lv_obj_t *cap = label(v_card, TF(UI_12), TC(TEXT_MUTED), "VOL"); lv_obj_align(cap, LV_ALIGN_TOP_MID, 58, 22);
        step_btn(v_card, 16, "-", -1); step_btn(v_card, 256, "+", +1);
        v_meter = plain(v_card); lv_obj_set_size(v_meter, 192, 22); lv_obj_align(v_meter, LV_ALIGN_TOP_MID, 0, 72);
        lv_obj_add_flag(v_meter, LV_OBJ_FLAG_CLICKABLE); lv_obj_set_ext_click_area(v_meter, 14);
        lv_obj_add_event_cb(v_meter, meter_draw, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_add_event_cb(v_meter, meter_cb, LV_EVENT_PRESSED, NULL); lv_obj_add_event_cb(v_meter, meter_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(v_meter, meter_cb, LV_EVENT_RELEASED, NULL); lv_obj_add_event_cb(v_meter, meter_cb, LV_EVENT_PRESS_LOST, NULL);
        v_tm = lv_timer_create(v_timeout, 2500, NULL); lv_timer_pause(v_tm);
        lv_obj_add_flag(v_root, LV_OBJ_FLAG_HIDDEN);
        keep_all(v_root);
    }
    if(vol < 0) vol = 0;
    if(vol > VOL_MAX) vol = VOL_MAX;
    if(!lv_obj_has_state(v_meter, LV_STATE_PRESSED)){ v_val = vol; v_paint(); }
    int hidden = lv_obj_has_flag(v_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(v_root, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(v_root);
    if(hidden || v_closing){
        lv_anim_delete(v_root, NULL); v_closing = 0;
        lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, v_root); lv_anim_set_exec_cb(&a, v_opa);
        lv_anim_set_values(&a, hidden ? 0 : lv_obj_get_style_opa(v_root, 0), 255); lv_anim_set_duration(&a, screen_get_anim() ? 160 : 1);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out); lv_anim_start(&a);
    }
    lv_timer_reset(v_tm); lv_timer_resume(v_tm);
}
int nt_volume_visible(void){ if(th_idisc()) return idisc_volume_visible(); return v_root && !lv_obj_has_flag(v_root, LV_OBJ_FLAG_HIDDEN); }
lv_obj_t *nt_volume_root(void){ return th_idisc() ? idisc_volume_root() : v_root; }

/* ---- the NTDisk kit: one look for every screen ------------------------------------------------------------------
 * Titles: small capitals, spaced, quiet grey, with the red dot before them (Song Info, Working mode, Quick Settings
 * and the menu panel already look like this). Icons in rows: white, not red (red is for what is on / current).
 * Chevrons quiet. Buttons keep Disco's fine ring. */
static void nt_dot_cb(lv_event_t *e){
    lv_obj_t *l = lv_event_get_target(e); lv_layer_t *layer = lv_event_get_layer(e); if(!layer) return;
    const char *t = lv_label_get_text(l); if(!t || !t[0]) return;
    lv_area_t a; lv_obj_get_coords(l, &a);
    lv_point_t sz; lv_text_get_size(&sz, t, lv_obj_get_style_text_font(l, 0), lv_obj_get_style_text_letter_space(l, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = lv_area_get_width(&a), x;
    if(sz.x >= w || lv_obj_get_style_text_align(l, 0) == LV_TEXT_ALIGN_LEFT) x = a.x1 - 13;
    else x = a.x1 + (w - sz.x) / 2 - 13;
    int cy = a.y1 + lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)) / 2;
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r); r.radius = LV_RADIUS_CIRCLE; r.bg_color = TC(ACCENT_PRIMARY); r.bg_opa = LV_OPA_COVER;
    lv_area_t d = { x, cy - 3, x + 5, cy + 2 }; lv_draw_rect(layer, &r, &d);
}
static void nt_dot_ext(lv_event_t *e){ lv_event_set_ext_draw_size(e, 16); }
void nt_title_style(lv_obj_t *l){
    if(!l || !th_nt()) return;
    if(th_idisc()){ idisc_title_style(l); return; }                      /* iDisc: bold, no dot */
    lv_obj_set_style_text_font(l, ui_font_cjk(14), 0);
    lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_remove_event_cb(l, nt_dot_cb); lv_obj_remove_event_cb(l, nt_dot_ext);   /* once, however often it's styled */
    lv_obj_add_event_cb(l, nt_dot_cb, LV_EVENT_DRAW_POST, NULL);
    lv_obj_add_event_cb(l, nt_dot_ext, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(l);
}
static lv_obj_t *nt_header(lv_obj_t *root, const char *title, lv_event_cb_t back_cb){
    lv_obj_t *t = theme_kit_default.header(root, title, back_cb);
    lv_obj_t *back = lv_obj_get_child(root, lv_obj_get_index(t) - 1);
    if(back){                                                              /* a quiet chevron, nearer the title */
        lv_obj_set_pos(back, 60, 22);
        lv_obj_t *ic = lv_obj_get_child(back, 0);
        if(ic){ lv_obj_set_style_text_font(ic, TF(UI_16), 0); lv_obj_set_style_text_color(ic, TC(TEXT_MUTED), 0); }
    }
    lv_obj_set_pos(t, 104, 34); lv_obj_set_size(t, 152, 22);
    nt_title_style(t);
    return t;
}
static void nt_title(lv_obj_t *l, const char *text){ lv_label_set_text(l, text); nt_title_style(l); }
static int is_icon(const char *s){ return s && (unsigned char)s[0] == 0xEF && strlen(s) <= 4; }
static void nt_style(lv_obj_t *o, kit_role_t role){
    extern const theme_kit_t theme_kit_ring;
    if(theme_kit_ring.style) theme_kit_ring.style(o, role);               /* Disco's fine button rings */
    if(role == KIT_ROW && lv_obj_get_style_bg_opa(o, 0) < LV_OPA_10 && lv_obj_get_width(o) >= 160){   /* every list row a pill */
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(o, TC(SURFACE), 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, TC(SURFACE_PRESSED), LV_STATE_PRESSED); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        return;
    }
    if(!lv_obj_check_type(o, &lv_label_class)) return;
    const char *s = lv_label_get_text(o);
    if(role == KIT_TITLE){ nt_title_style(o); return; }
    if(role == KIT_ROW_CHEVRON){ lv_obj_set_style_text_color(o, TC(TEXT_MUTED), 0); return; }
    if((role == KIT_ROW_TEXT || role == KIT_ROW_SUB || role == KIT_TEXT) && is_icon(s)
       && lv_color_eq(lv_obj_get_style_text_color(o, 0), TC(ACCENT_PRIMARY)))
        lv_obj_set_style_text_color(o, TC(TEXT_PRIMARY), 0);           /* icons white; red stays for "on" */
}
const theme_kit_t theme_kit_nt = { .id = "ntdisk", .header = nt_header, .style = nt_style, .title = nt_title };
