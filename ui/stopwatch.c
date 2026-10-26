/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* stopwatch.c - Stopwatch (SCR_STOPWATCH), from Apps.
 *
 * Big minutes:seconds.hundredths in the middle, a thin hand of light running round the rim once a minute, the laps
 * underneath (newest first, the best one in the accent colour) and two buttons: Lap / Reset on the left, Start / Stop
 * on the right. It counts from CLOCK_MONOTONIC, so it keeps running while you go back to your music, and it only
 * redraws while it is on screen. Theme roles only. */
#include "screens.h"
#include "theme.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SW_MAXLAP 99
static lv_obj_t *g_root, *g_time, *g_hund, *g_rim, *g_laps, *g_left, *g_left_l, *g_right, *g_right_l, *g_hint;
static lv_timer_t *g_tm;
static int g_running;
static int64_t g_start_ms, g_acc_ms;                 /* g_acc_ms: time banked before the current run */
static int64_t g_lap_at[SW_MAXLAP + 1];              /* total time at each lap press */
static int g_nlap;

static int64_t mono_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static int64_t elapsed(void){ return g_acc_ms + (g_running ? mono_ms() - g_start_ms : 0); }
static void fmt(int64_t ms, char *main, int mn, char *hs, int hn){
    if(ms < 0) ms = 0;
    int64_t cs = ms / 10, s = cs / 100, m = s / 60, h = m / 60;
    if(h > 0) snprintf(main, (size_t)mn, "%d:%02d:%02d", (int)h, (int)(m % 60), (int)(s % 60));
    else      snprintf(main, (size_t)mn, "%02d:%02d", (int)m, (int)(s % 60));
    if(hs) snprintf(hs, (size_t)hn, ".%02d", (int)(cs % 100));
}
static void paint_time(void){
    char m[16], h[8]; int64_t t = elapsed();
    fmt(t, m, sizeof m, h, sizeof h);
    if(strcmp(lv_label_get_text(g_time), m)) lv_label_set_text(g_time, m);
    lv_label_set_text(g_hund, h);
    int deg = (int)((t % 60000) * 360 / 60000);
    lv_arc_set_angles(g_rim, 0, (lv_value_precise_t)(deg ? deg : (t ? 360 : 0)));
}
static void paint_buttons(void){
    lv_label_set_text(g_right_l, g_running ? "Stop" : (elapsed() ? "Go on" : "Start"));
    lv_label_set_text(g_left_l, g_running ? "Lap" : "Reset");
    lv_obj_set_style_opa(g_left, (g_running || elapsed()) ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_bg_color(g_right, g_running ? TC(SURFACE_RAISED) : TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_text_color(g_right_l, g_running ? TC(TEXT_PRIMARY) : TC(ON_ACCENT), 0);
}
static void paint_laps(void){
    lv_obj_clean(g_laps);
    if(g_hint){ if(g_nlap) lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(g_hint, LV_OBJ_FLAG_HIDDEN); }
    int best = -1; int64_t bestv = 0;
    for(int i = 0; i < g_nlap; i++){ int64_t v = g_lap_at[i] - (i ? g_lap_at[i - 1] : 0); if(best < 0 || v < bestv){ best = i; bestv = v; } }
    for(int i = g_nlap - 1; i >= 0; i--){
        int64_t v = g_lap_at[i] - (i ? g_lap_at[i - 1] : 0);
        char m[16], h[8], line[48]; fmt(v, m, sizeof m, h, sizeof h);
        snprintf(line, sizeof line, "Lap %d  %s%s", i + 1, m, h);
        lv_obj_t *l = lv_label_create(g_laps); lv_label_set_text(l, line);
        lv_obj_set_width(l, 236); lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(l, TF(UI_16), 0);
        lv_obj_set_style_text_color(l, (i == best && g_nlap > 1) ? TC(ACCENT_PRIMARY) : TC(TEXT_SECONDARY), 0);
    }
}
static void tick_cb(lv_timer_t *t){
    (void)t;
    if(screen_current() != SCR_STOPWATCH){ lv_timer_pause(g_tm); return; }   /* off screen: no work, time still counts */
    paint_time();
}
static void right_cb(lv_event_t *e){
    (void)e;
    if(g_running){ g_acc_ms += mono_ms() - g_start_ms; g_running = 0; }
    else { g_start_ms = mono_ms(); g_running = 1; lv_timer_resume(g_tm); }
    paint_time(); paint_buttons();
}
static void left_cb(lv_event_t *e){
    (void)e;
    if(g_running){
        if(g_nlap < SW_MAXLAP) g_lap_at[g_nlap++] = elapsed(); else ui_toast("That's 99 laps");
    } else { g_acc_ms = 0; g_nlap = 0; }
    paint_time(); paint_buttons(); paint_laps();
}
static lv_obj_t *round_button(lv_obj_t *root, int x, lv_obj_t **lbl, lv_event_cb_t cb){
    lv_obj_t *b = lv_obj_create(root); lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 96, 50); lv_obj_set_pos(b, x, 268);
    lv_obj_set_style_radius(b, 25, 0); lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    *lbl = lv_label_create(b); lv_obj_set_style_text_font(*lbl, TF(UI_18), 0);
    lv_obj_set_style_text_color(*lbl, TC(TEXT_PRIMARY), 0); lv_obj_center(*lbl);
    return b;
}
static void back_cb(lv_event_t *e){ (void)e; screen_back(); }
void stopwatch_create(lv_obj_t *root){
    g_root = root;
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0); lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    g_rim = lv_arc_create(root);                                           /* the seconds hand round the rim */
    lv_obj_remove_style(g_rim, NULL, LV_PART_KNOB); lv_obj_remove_flag(g_rim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g_rim, 346, 346); lv_obj_center(g_rim);
    lv_arc_set_bg_angles(g_rim, 0, 360); lv_arc_set_rotation(g_rim, 270);
    lv_obj_set_style_arc_width(g_rim, 3, LV_PART_MAIN); lv_obj_set_style_arc_color(g_rim, TC(CONTROL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(g_rim, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_rim, 3, LV_PART_INDICATOR); lv_obj_set_style_arc_color(g_rim, TC(ACCENT_PRIMARY), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_rim, true, LV_PART_INDICATOR);
    lv_arc_set_angles(g_rim, 0, 0);
    ui_header_cb(root, "Stopwatch", back_cb);
    g_time = lv_label_create(root); lv_label_set_text(g_time, "00:00");
    lv_obj_set_style_text_font(g_time, TF(UI_48), 0); lv_obj_set_style_text_color(g_time, TC(TEXT_PRIMARY), 0);
    lv_obj_align(g_time, LV_ALIGN_TOP_MID, -18, 96);
    g_hund = lv_label_create(root); lv_label_set_text(g_hund, ".00");
    lv_obj_set_style_text_font(g_hund, TF(UI_28), 0); lv_obj_set_style_text_color(g_hund, TC(TEXT_SECONDARY), 0);
    lv_obj_align_to(g_hund, g_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -5);
    g_laps = lv_obj_create(root); lv_obj_remove_style_all(g_laps);         /* the laps, newest first, scrolling */
    lv_obj_set_size(g_laps, 240, 96); lv_obj_align(g_laps, LV_ALIGN_TOP_MID, 0, 162);
    lv_obj_set_flex_flow(g_laps, LV_FLEX_FLOW_COLUMN); lv_obj_set_flex_align(g_laps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_laps, 4, 0); lv_obj_set_scroll_dir(g_laps, LV_DIR_VER); lv_obj_set_scrollbar_mode(g_laps, LV_SCROLLBAR_MODE_OFF);
    g_hint = lv_label_create(root); lv_label_set_text(g_hint, "Lap marks a split while it runs");
    lv_obj_set_style_text_font(g_hint, TF(UI_14), 0); lv_obj_set_style_text_color(g_hint, TC(TEXT_MUTED), 0);
    lv_obj_align(g_hint, LV_ALIGN_TOP_MID, 0, 200);
    g_left = round_button(root, 76, &g_left_l, left_cb);
    g_right = round_button(root, 188, &g_right_l, right_cb);
    g_tm = lv_timer_create(tick_cb, 50, NULL); lv_timer_pause(g_tm);
    paint_time(); paint_buttons(); paint_laps();
}
void stopwatch_open(void){
    screen_show(SCR_STOPWATCH);
    paint_time(); paint_buttons();
    if(g_running) lv_timer_resume(g_tm);
}
/* host renders */
void stopwatch_test_state(int64_t total_ms, int running, int laps){
    g_acc_ms = total_ms; g_running = 0; g_nlap = 0;
    for(int i = 0; i < laps && i < SW_MAXLAP; i++) g_lap_at[g_nlap++] = total_ms * (i + 1) / (laps + 1) + (i == 1 ? -1200 : 0);
    if(running){ g_running = 1; g_start_ms = mono_ms(); }
    paint_time(); paint_buttons(); paint_laps();
}
