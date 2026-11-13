/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* powermenu.c - what holding the power key asks (kopi.28).
 *
 * Held for 1.5 s (main.c), the screen lights and this sheet asks: Shut down, Restart, or Cancel. Shut down is the same
 * clean shutdown as Settings > Battery & power > Shut down player (the song's play counts saved, the card synced);
 * Restart the same as Settings > Maintenance > Restart. Cancel, a tap outside the card, or 10 seconds without a tap
 * closes it. In each theme's look: NTDisk's pills, iDisc's glass, Null's outlines; colours are theme roles only. */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "idisc.h"
#include <stdio.h>

static lv_obj_t *g_root, *g_card;
static lv_timer_t *g_tm;
static void close_menu(void){
    if(g_tm){ lv_timer_delete(g_tm); g_tm = NULL; }
    if(g_root){ lv_obj_t *r = g_root; g_root = NULL; idisc_sheet_out(r, g_card); g_card = NULL; }
}
static void timeout_cb(lv_timer_t *t){ (void)t; g_tm = NULL; close_menu(); }
static void pick_cb(lv_event_t *e){
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    close_menu();
    if(k == 0) ui_power_off();
    else if(k == 1) settings_restart_now();
}
static void outside_cb(lv_event_t *e){ if(lv_event_get_target(e) == g_root) close_menu(); }
static void poke_cb(lv_event_t *e){ (void)e; if(g_tm) lv_timer_reset(g_tm); }   /* a finger on it: don't time out */
static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, lv_color_t c, const char *s){
    lv_obj_t *l = lv_label_create(p); lv_label_set_text(l, s); lv_obj_set_style_text_font(l, f, 0); lv_obj_set_style_text_color(l, c, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE); kit_keep(l); return l;
}
void power_menu_open(void){
    close_menu();
    g_root = lv_obj_create(lv_layer_top()); lv_obj_remove_style_all(g_root); lv_obj_set_size(g_root, 360, 360); kit_keep(g_root);
    lv_obj_set_style_bg_color(g_root, TC(SCRIM), 0); lv_obj_set_style_bg_opa(g_root, 170, 0);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_root, outside_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(g_root, poke_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *card = lv_obj_create(g_root); lv_obj_remove_style_all(card); kit_keep(card);
    lv_obj_set_size(card, 250, 250); lv_obj_center(card); lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(card, 48, 0); lv_obj_set_style_bg_color(card, TC(SURFACE), 0); lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, th_null() ? 2 : 1, 0); lv_obj_set_style_border_color(card, th_null() ? TC(ACCENT_PRIMARY) : TC(BORDER), 0);
    lv_obj_add_event_cb(card, poke_cb, LV_EVENT_PRESSED, NULL);
    if(th_idisc()) idisc_glass(card, 48);
    lv_obj_t *t = label(card, TF(HEADER), TC(TEXT_PRIMARY), th_nt_caps() ? "POWER" : "Power");
    if(th_nt()) nt_title_style(t);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);
    static const char *const NAMES[3] = { "Shut down", "Restart", "Cancel" };
    for(int k = 0; k < 3; k++){
        lv_obj_t *b = lv_button_create(card); lv_obj_remove_style_all(b); kit_keep(b);
        lv_obj_set_size(b, 190, 44); lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 64 + k * 54);
        lv_obj_set_style_radius(b, 22, 0);
        int main_one = k == 0;
        if(th_idisc() && !main_one) idisc_glass(b, 22);
        else { lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
               lv_obj_set_style_bg_color(b, main_one ? TC(ACCENT_PRIMARY) : TC(SURFACE_RAISED), 0); }
        if(th_null() && !main_one){ lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(b, 1, 0); lv_obj_set_style_border_color(b, TC(BORDER_STRONG), 0); }
        lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED); lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        lv_color_t fg = main_one ? TC(ON_ACCENT) : TC(TEXT_PRIMARY);
        lv_obj_t *l = label(b, TF(UI_18), fg, NAMES[k]); lv_obj_center(l);
    }
    g_card = card; idisc_sheet_in(g_root, card);
    g_tm = lv_timer_create(timeout_cb, 10000, NULL); lv_timer_set_repeat_count(g_tm, 1);
}
int power_menu_visible(void){ return g_root != NULL; }
