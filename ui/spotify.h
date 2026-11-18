/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Spotify (kopi.35): spotify.c (UI) + spotify_helper.c / spotify/ (the "spot-disc" helper, librespot) */
#ifndef SPOTIFY_H
#define SPOTIFY_H
#include "lvgl/lvgl.h"
void spot_create(lv_obj_t *root);          /* SCR_SPOTIFY */
void spot_refresh(void);
int  spot_back_consumed(void);             /* a list steps back inside the screen */
int  spot_is_on(void);
void spot_set_on(int on);
void spot_boot(void);
void spot_restart(void);
void spot_mode_leaving(void);              /* another working mode: Spotify (on AirPlay) turns off */
int  spot_controls(void);                  /* Music's buttons drive Spotify */
int  spot_playing(void);                   /* keeps the Disc from its idle power-off */
void spot_send(const char *cmd);
int  spot_seek(long ms);
void spot_edit_name(void);
void spot_edit_cid(void);
void spot_sign_out(void);
void spot_settings_load(void);
void spot_test_state(int s);               /* host renders */
void spot_test_list(const char *tsv, const char *title);
extern char g_spot_name_lbl[48], g_spot_cid_lbl[48];
#endif
