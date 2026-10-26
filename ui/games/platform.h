/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* games/platform.h - what the small built-in games share (Echo, Glide): a 360 x 360 XRGB frame copied to /dev/fb0
 * with Settings' screen rotation, the touch panel as press / release / hold events, the current theme's colours
 * (from /tmp/diskos-app-env, written by mq_ui just before a launch), a 5 x 7 pixel font, a few anti-aliased shapes
 * and a best score kept in /usr/data/<game>/best.conf. No LVGL; each game runs as `mq_ui --<game>`, a child process
 * that owns the screen until it exits, like Boomshine and Snake. */
#ifndef GAMES_PLATFORM_H
#define GAMES_PLATFORM_H
#include <stdint.h>

#define GW 360
#define GH 360

extern uint32_t g_frame[GW * GH];
extern uint32_t G_BG, G_TEXT, G_DIM, G_ACC;          /* theme colours (defaults if no environment file) */

typedef struct {
    const char *name;                                /* "echo": log tag and /usr/data/echo/best.conf */
    double (*tick)(double now);                      /* advance; returns the time the next tick is wanted (0 = idle) */
    void (*render)(void);                            /* draw g_frame */
    void (*down)(int x, int y);                      /* finger down */
    void (*up)(int x, int y, int moved);             /* finger lifted (not after a hold) */
    void (*hold)(void);                              /* ~0.85 s still press */
} game_t;

extern int g_dirty;                                  /* set to redraw */
extern volatile int g_quit;                          /* set to leave */
extern int g_best;                                   /* loaded before the game starts */

double   gp_now(void);
uint32_t gp_rand(void);
uint32_t gp_mix(uint32_t a, uint32_t b, int t);      /* t of 255 toward b */
void     gp_put(int x, int y, uint32_t c, int a);
void     gp_clear(uint32_t c);
void     gp_rect(int x, int y, int w, int h, uint32_t c, int radius, int a);
void     gp_disc(float cx, float cy, float r, uint32_t c, int a);
void     gp_ring(float cx, float cy, float r, float width, uint32_t c, int a);
int      gp_text_w(const char *s, int scale);
void     gp_text(int cy, const char *s, int scale, uint32_t c);          /* centred on x */
void     gp_text_at(int x, int cy, const char *s, int scale, uint32_t c);
void     gp_button(int cy, const char *label, int filled);               /* 40 px tall pill, centred */
int      gp_in_button(int x, int y, int cy);
void     gp_save_best(void);
int      gp_main(const game_t *g, int argc, char **argv,
                 int (*render_preview)(const char *which));                /* --render <which> <out.ppm> */
#endif
