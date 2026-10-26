/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Echo - a memory game for the round screen. Four quarter-pads light up in a sequence; tap them back in the same
 * order. Each round adds one more. Runs as `mq_ui --echo` (games/platform.c).
 *
 *   Title: PLAY / EXIT (or hold to leave).  Playing: watch, then repeat; hold ~1 s for the title.
 *   Game over: AGAIN; hold for the title. The best score is kept in /usr/data/echo/best.conf. */
#include "platform.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define R0 62.0f                 /* the pads' inner and outer radius, and the gap between them */
#define R1 170.0f
#define GAP 12.0f
#define MAXSEQ 256

enum { TITLE, WAIT, SHOW, INPUT, GOOD, OVER };
static int mode = TITLE, seq[MAXSEQ], len, show_i, show_on, input_i, lit = -1, wrong = -1, score, newbest, pressed = -1;
static double t_next, lit_until;
static const uint32_t PAD[4] = { 0xE8735A, 0xE9B949, 0x4CC2A7, 0x9A7BE0 };   /* coral, amber, teal, violet */
static unsigned char g_id[GW * GH], g_cov[GW * GH];                        /* the board: pad index + 1, coverage */

/* which pad (0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left) a point is on, and how much of the pixel */
static int pad_at(float dx, float dy, float r0, float r1, float gap, int *cov){
    float d = sqrtf(dx * dx + dy * dy);
    float c = fminf(fminf(d - r0, r1 - d), fminf(fabsf(dx), fabsf(dy)) - gap / 2) + 0.5f;
    if(c <= 0){ *cov = 0; return -1; }
    *cov = c >= 1 ? 255 : (int)(255 * c);
    return dy < 0 ? (dx < 0 ? 0 : 1) : (dx < 0 ? 3 : 2);
}
static void build_board(void){
    for(int y = 0; y < GH; y++) for(int x = 0; x < GW; x++){
        int cov, p = pad_at(x + 0.5f - 180, y + 0.5f - 180, R0, R1, GAP, &cov);
        g_id[y * GW + x] = (unsigned char)(p + 1); g_cov[y * GW + x] = (unsigned char)cov;
    }
}
static uint32_t pad_colour(int p, int on){
    return on ? gp_mix(PAD[p], 0xFFFFFF, 50) : gp_mix(PAD[p], G_BG, 165);
}
static void draw_board(int fade){
    for(int i = 0; i < GW * GH; i++){
        int p = g_id[i] - 1;
        if(p < 0){ g_frame[i] = G_BG; continue; }
        uint32_t c = pad_colour(p, p == lit || p == wrong);
        if(fade) c = gp_mix(c, G_BG, 215);
        g_frame[i] = gp_mix(G_BG, c, g_cov[i]);
    }
}

static void lose(void){
    mode = OVER; g_dirty = 1;
    if(score > g_best){ g_best = score; newbest = 1; gp_save_best(); } else newbest = 0;
}
static void start(double now){
    len = 1; seq[0] = (int)(gp_rand() % 4); score = 0; newbest = 0; wrong = -1; lit = -1;
    mode = WAIT; t_next = now + 0.7; g_dirty = 1;
}
static double step_on(void){ double s = 0.56 - 0.02 * len; return s < 0.24 ? 0.24 : s; }

static double tick(double now){
    if(lit_until > 0 && now >= lit_until && (mode == INPUT || mode == GOOD)){ lit = -1; lit_until = 0; g_dirty = 1; }
    switch(mode){
    case WAIT:
        if(now >= t_next){ mode = SHOW; show_i = 0; show_on = 1; lit = seq[0]; t_next = now + step_on(); g_dirty = 1; }
        return t_next;
    case SHOW:
        if(now < t_next) return t_next;
        if(show_on){ lit = -1; show_on = 0; t_next = now + 0.16; }
        else if(++show_i < len){ lit = seq[show_i]; show_on = 1; t_next = now + step_on(); }
        else { mode = INPUT; input_i = 0; lit = -1; t_next = 0; }
        g_dirty = 1;
        return mode == SHOW ? t_next : 0;
    case GOOD:
        if(now >= t_next){
            if(len < MAXSEQ) seq[len++] = (int)(gp_rand() % 4);
            mode = WAIT; t_next = now + 0.25; g_dirty = 1; lit = -1; lit_until = 0;
        }
        return lit_until > 0 && lit_until < t_next ? lit_until : t_next;
    case INPUT: return lit_until > 0 ? lit_until : 0;
    default: return 0;
    }
}

static void render(void){
    char buf[40];
    if(mode == TITLE){
        gp_clear(G_BG);
        for(int y = 30; y < 112; y++) for(int x = 139; x < 221; x++){                 /* a small board as the logo */
            int cov, p = pad_at(x + 0.5f - 180, y + 0.5f - 71, 16, 40, 5, &cov);
            if(p >= 0) gp_put(x, y, pad_colour(p, p == 1), cov);
        }
        gp_text(124, "ECHO", 6, G_TEXT);
        snprintf(buf, sizeof buf, "BEST %d", g_best); gp_text(178, buf, 2, G_DIM);
        gp_button(208, "PLAY", 1);
        gp_button(260, "EXIT", 0);
        gp_text(316, "WATCH. THEN REPEAT.", 1, G_DIM);
        return;
    }
    draw_board(mode == OVER);
    if(mode == OVER){
        gp_text(96, "GAME OVER", 4, G_TEXT);
        snprintf(buf, sizeof buf, "%d", score); gp_text(144, buf, 6, G_ACC);
        snprintf(buf, sizeof buf, newbest ? "NEW BEST!" : "BEST %d", g_best); gp_text(198, buf, 2, newbest ? G_TEXT : G_DIM);
        gp_button(234, "AGAIN", 1);
        gp_text(290, "HOLD FOR MENU", 1, G_DIM);
        return;
    }
    snprintf(buf, sizeof buf, "%d", score); gp_text(150, buf, 5, G_TEXT);
    const char *st = mode == INPUT ? "YOUR TURN" : mode == GOOD ? "NICE!" : "WATCH";
    gp_text(198, st, 2, mode == INPUT ? G_ACC : G_DIM);
}

static int pad_hit(int x, int y){                        /* a little more generous than what is drawn */
    int cov; return pad_at(x + 0.5f - 180, y + 0.5f - 180, R0 - 8, R1 + 12, 0, &cov);
}
static void down(int x, int y){
    if(mode != INPUT) return;
    pressed = pad_hit(x, y);
    if(pressed >= 0){ lit = pressed; lit_until = 0; g_dirty = 1; }
}
static void up(int x, int y, int moved){
    double now = gp_now();
    if(mode == TITLE){ if(gp_in_button(x, y, 208)) start(now); else if(gp_in_button(x, y, 260)) g_quit = 1; return; }
    if(mode == OVER){ if(gp_in_button(x, y, 234) || !moved) start(now); return; }
    if(mode != INPUT || pressed < 0) return;
    int p = pressed; pressed = -1;
    if(p != seq[input_i]){ wrong = p; lit = seq[input_i]; lose(); return; }   /* show which one it was */
    lit_until = now + 0.14; g_dirty = 1;
    if(++input_i == len){ score = len; mode = GOOD; t_next = now + 0.7; }
}
static void hold(void){ if(mode == TITLE) g_quit = 1; else { mode = TITLE; lit = wrong = -1; g_dirty = 1; } }

static int preview(const char *which){
    build_board();
    if(!strcmp(which, "title")) return 0;
    len = 7; score = 6; for(int i = 0; i < len; i++) seq[i] = i % 4;
    if(!strcmp(which, "show")){ mode = SHOW; lit = 2; return 0; }
    if(!strcmp(which, "play")){ mode = INPUT; lit = 0; return 0; }
    if(!strcmp(which, "over")){ mode = OVER; wrong = 3; lit = 1; return 0; }
    return -1;
}
static const game_t GAME = { "echo", tick, render, down, up, hold };
int echo_main(int argc, char **argv){
    build_board();
    return gp_main(&GAME, argc, argv, preview);
}
#ifdef GAMES_STANDALONE
int main(int argc, char **argv){ return echo_main(argc, argv); }
#endif
