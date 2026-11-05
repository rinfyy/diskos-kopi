/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* sleepsheet.c - the Sleep timer, one tap away (kopi.27).
 *
 * A sheet over whatever is on screen: what the timer is doing now ("Pausing in 23 min"), then Off, 15, 30, 45, 60,
 * 90 and 120 minutes and "End of song" as pills (the one running lit). Tap one and the sheet closes; tap outside to
 * close it as it was. Opened from the Sleep tile in Quick Settings (which used to open nothing outside the Zune
 * theme), from Shortcuts, and from Settings > Battery & power. In each theme's look: NTDisk's pills, iDisc's glass,
 * Null's outlines. Colours are theme roles only. */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "idisc.h"
#include "ipc.h"
#include "config.h"
#include "simple_theme.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *g_root, *g_status;
static lv_timer_t *g_tm;
static const int MINS[7] = { 0, 15, 30, 45, 60, 90, 120 };
static void close_sheet(void){
    if(g_tm){ lv_timer_delete(g_tm); g_tm = NULL; }
    if(g_root){ lv_obj_delete_async(g_root); g_root = NULL; g_status = NULL; }
}
static void status_paint(void){
    if(!g_status) return;
    int secs = 0, st = ui_sleep_state(&secs); char b[64];
    if(st == 1) snprintf(b, sizeof b, "Pausing in %d min", (secs + 59) / 60);
    else if(st == 2) snprintf(b, sizeof b, "Pausing at the end of this song");
    else snprintf(b, sizeof b, "Off");
    lv_label_set_text(g_status, b);
}
static void tick_cb(lv_timer_t *t){ (void)t; status_paint(); }
static void pick_cb(lv_event_t *e){
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    char msg[48];
    if(k == 7){                                                           /* the end of the song playing */
        track_state_t st; ipc_get_state(&st);
        if(!st.have_track || st.duration_ms <= 0){ ui_toast("Nothing playing"); return; }
        ui_set_sleep_timer(0); ui_set_sleep_eoc(st.duration_ms - 1200, st.path, 1);
        snprintf(msg, sizeof msg, "Pausing at the end of this song");
    } else {
        ui_set_sleep_timer(MINS[k]); simple_cfg_set_int("sleep_idx", 0);
        if(MINS[k]) snprintf(msg, sizeof msg, "Sleep in %d min", MINS[k]); else snprintf(msg, sizeof msg, "Sleep timer off");
    }
    ui_toast(msg);
    close_sheet();
}
static void outside_cb(lv_event_t *e){ if(lv_event_get_target(e) == g_root) close_sheet(); }
static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, lv_color_t c, const char *s){
    lv_obj_t *l = lv_label_create(p); lv_label_set_text(l, s); lv_obj_set_style_text_font(l, f, 0); lv_obj_set_style_text_color(l, c, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE); kit_keep(l); return l;
}
void sleep_sheet_open(void){
    close_sheet();
    int secs = 0, state = ui_sleep_state(&secs), cur = -1;
    if(state == 2) cur = 7;
    else if(state == 1){ int m = (secs + 59) / 60, best = 1; for(int k = 1; k < 7; k++) if(MINS[k] >= m){ best = k; break; } cur = best; }
    else cur = 0;
    g_root = lv_obj_create(lv_layer_top()); lv_obj_remove_style_all(g_root); lv_obj_set_size(g_root, 360, 360); kit_keep(g_root);
    lv_obj_set_style_bg_color(g_root, TC(SCRIM), 0); lv_obj_set_style_bg_opa(g_root, 150, 0);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_root, outside_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *card = lv_obj_create(g_root); lv_obj_remove_style_all(card); kit_keep(card);
    lv_obj_set_size(card, 292, 232); lv_obj_center(card); lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(card, 44, 0); lv_obj_set_style_bg_color(card, TC(SURFACE), 0); lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, th_null() ? 2 : 1, 0); lv_obj_set_style_border_color(card, th_null() ? TC(ACCENT_PRIMARY) : TC(BORDER), 0);
    if(th_idisc()) idisc_glass(card, 44);
    lv_obj_t *t = label(card, TF(HEADER), TC(TEXT_PRIMARY), th_nt_caps() ? "SLEEP TIMER" : "Sleep timer");
    if(th_nt()) nt_title_style(t);
    lv_obj_align(t, LV_ALIGN_TOP_MID, th_nt_caps() ? 6 : 0, 18);
    g_status = label(card, TF(UI_14), TC(TEXT_SECONDARY), ""); lv_obj_align(g_status, LV_ALIGN_TOP_MID, 0, 44);
    static const char *const NAMES[8] = { "Off", "15 min", "30 min", "45 min", "1 hour", "90 min", "2 hours", "End of song" };
    for(int k = 0; k < 8; k++){
        int col = k % 2, row = k / 2;
        lv_obj_t *b = lv_button_create(card); lv_obj_remove_style_all(b); kit_keep(b);
        lv_obj_set_size(b, 124, 36); lv_obj_set_pos(b, 18 + col * 132, 70 + row * 40);
        lv_obj_set_style_radius(b, 18, 0);
        int on = k == cur;
        if(th_idisc() && !on) idisc_glass(b, 18);
        else { lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
               lv_obj_set_style_bg_color(b, on ? (th_nt_caps() ? TC(TEXT_PRIMARY) : TC(ACCENT_PRIMARY)) : TC(SURFACE_RAISED), 0); }
        if(th_null() && !on){ lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(b, 1, 0); lv_obj_set_style_border_color(b, TC(BORDER_STRONG), 0); }
        lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED); lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        lv_color_t fg = on ? (th_nt_caps() ? TC(CANVAS) : TC(ON_ACCENT)) : TC(TEXT_PRIMARY);
        lv_obj_t *l = label(b, TF(UI_14), fg, NAMES[k]); lv_obj_center(l);
    }
    status_paint();
    g_tm = lv_timer_create(tick_cb, 1000, NULL);
}
int sleep_sheet_visible(void){ return g_root != NULL; }
