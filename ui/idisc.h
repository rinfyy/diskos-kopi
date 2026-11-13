/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* idisc.h - the iDisc theme's own screens (idisc.c). NTDisk's entry points forward here when th_idisc(). */
#ifndef IDISC_H
#define IDISC_H
#include "lvgl/lvgl.h"
void idisc_wallpaper(lv_obj_t *o);            /* the soft wallpaper (Clear glass) or the plain canvas (Tinted) */
void idisc_glass(lv_obj_t *o, int radius);    /* a glass surface over the wallpaper */
void idisc_glass_on(lv_obj_t *o, int radius, const lv_image_dsc_t *src);
int  idisc_reduce_motion(void);                         /* Settings > Display > Reduce motion (kopi.30) */
void idisc_materialize(lv_obj_t *o, int in, void (*done)(lv_obj_t *));   /* glass comes up from clear (or goes), then its content */
void idisc_material(lv_obj_t *o, int m);                /* set it directly, 0..255 (a gesture drives it) */
void idisc_sheet_in(lv_obj_t *root, lv_obj_t *card);    /* a sheet over a scrim: the scrim darkens, the card materializes */
void idisc_sheet_out(lv_obj_t *root, lv_obj_t *card);   /* ...and back; root is deleted at the end (all themes) */   /* ... over another screen-aligned 360 px frost (NULL: none) */
void idisc_solid(lv_obj_t *o);                /* a glass surface made solid (a lit tile): no frost under it */
void idisc_title_style(lv_obj_t *l);
void idisc_home_create(lv_obj_t *root); void idisc_home_now_playing(const char *title, const char *artist, int playing);
void idisc_home_art_changed(void); void idisc_home_lyrics_apply(int on); void idisc_home_clock(const char *t);
void idisc_home_weather(const char *s); int idisc_home_owns_point(int x, int y); void idisc_pictures_apply(void);
void idisc_status(int batt, int charging);
const lv_image_dsc_t *idisc_wash(void);      /* the album wash behind Music (NULL until made) */
void idisc_saver_create(lv_obj_t *root); void idisc_saver_clock(const char *t); void idisc_saver_weather(const char *s);
void idisc_saver_track(const char *title, const char *artist); void idisc_saver_pictures(void);
void idisc_qs_create(lv_obj_t *root); void idisc_qs_build(void); void idisc_qs_refresh(void); void idisc_qs_art(const void *cover);
void idisc_qs_battery(int pct, int charging); void idisc_qs_slide(int open);
void idisc_volume_show(int vol); int idisc_volume_visible(void); lv_obj_t *idisc_volume_root(void);
#endif
