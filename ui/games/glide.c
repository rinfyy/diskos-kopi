/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Glide - a one-tap game for the round screen. Tap to lift a glowing orb through the gaps between pillars; the
 * round edge of the screen is the wall. Runs as `mq_ui --glide` (games/platform.c).
 *
 *   Title: PLAY / EXIT (or hold to leave).  Ready: tap to start.  Playing: tap anywhere to rise; hold ~1 s for the
 *   title.  Game over: AGAIN; hold for the title. The best score is kept in /usr/data/glide/best.conf. */
#include "platform.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define ORB_X 128.0f
#define ORB_R 10.0f
#define RIM 174.0f               /* the orb dies when it leaves this circle */
#define PW 46                    /* pillar width */
#define GAPH 120                 /* gap height */
#define SPACING 196              /* distance between pillars */
#define NP 4
#define GRAV 980.0f
#define FLAP -300.0f
#define FRAME (1.0 / 30.0)

enum { TITLE, READY, PLAY, OVER };
static int mode = TITLE, score, newbest;
static float oy = 180, vy, speed;
static float px[NP], gy[NP]; static int passed[NP];     /* pillar x (left edge), gap centre y, counted */
static double t_last, t_ready, t_over;
static uint32_t g_bg[GW * GH];                            /* the backdrop, drawn once (needs the theme colours) */
static int g_bg_ok;

static float gap_y(void){ return 120.0f + (float)(gp_rand() % 121); }   /* 120..240: always inside the circle */
static void reset(void){
    oy = 180; vy = 0; score = 0; newbest = 0; speed = 112;
    for(int i = 0; i < NP; i++){ px[i] = GW + 40 + i * SPACING; gy[i] = gap_y(); passed[i] = 0; }
}
static void build_backdrop(void){
    for(int i = 0; i < GW * GH; i++) g_frame[i] = G_BG;
    for(int k = 0; k < 26; k++){                                       /* a few faint specks, fixed */
        float a = k * 2.399963f, r = 30 + (k * 53) % 140;
        gp_disc(180 + r * cosf(a), 180 + r * sinf(a), 1.2f, G_DIM, 70);
    }
    gp_ring(180, 180, RIM + 3, 1.5f, G_DIM, 90);
    memcpy(g_bg, g_frame, sizeof g_bg); g_bg_ok = 1;
}
static void lose(double now){
    mode = OVER; t_over = now; g_dirty = 1;
    if(score > g_best){ g_best = score; newbest = 1; gp_save_best(); }
}
static int hits(void){
    float dx = ORB_X - 180, dy = oy - 180;
    if(sqrtf(dx * dx + dy * dy) > RIM - ORB_R) return 1;
    for(int i = 0; i < NP; i++){
        float l = px[i], r = px[i] + PW;
        if(ORB_X + ORB_R < l || ORB_X - ORB_R > r) continue;
        float top = gy[i] - GAPH / 2.0f, bot = gy[i] + GAPH / 2.0f;
        float cx = ORB_X < l ? l : ORB_X > r ? r : ORB_X;              /* nearest point of each pillar half */
        float ty = oy < top ? oy : top, by = oy > bot ? oy : bot;
        const float hit = ORB_R * ORB_R * 0.8f;                         /* a touch forgiving: a graze survives */
        if((ORB_X - cx) * (ORB_X - cx) + (oy - ty) * (oy - ty) < hit) return 1;
        if((ORB_X - cx) * (ORB_X - cx) + (oy - by) * (oy - by) < hit) return 1;
    }
    return 0;
}
static double tick(double now){
    float dt = (float)(now - t_last); t_last = now;
    if(dt > 0.1f) dt = 0.1f;
    if(mode == READY){ oy = 180 + 8 * sinf((float)(now - t_ready) * 3.2f); g_dirty = 1; return now + FRAME; }
    if(mode != PLAY) return 0;
    vy += GRAV * dt; if(vy > 520) vy = 520;
    oy += vy * dt;
    for(int i = 0; i < NP; i++){
        px[i] -= speed * dt;
        if(px[i] + PW < -10){                                              /* recycle behind the last one */
            float far = px[0]; for(int k = 1; k < NP; k++) if(px[k] > far) far = px[k];
            px[i] = far + SPACING; gy[i] = gap_y(); passed[i] = 0;
        }
        if(!passed[i] && px[i] + PW < ORB_X - ORB_R){ passed[i] = 1; score++; if(speed < 170) speed += 2.5f; }
    }
    if(hits()) lose(now);
    g_dirty = 1;
    return mode == PLAY ? now + FRAME : 0;
}
static void draw_orb(float x, float y, uint32_t c){
    gp_disc(x, y, ORB_R + 7, G_ACC, 40);
    gp_disc(x, y, ORB_R + 3, G_ACC, 90);
    gp_disc(x, y, ORB_R, c, 255);
}
static void draw_pillars(int faded){
    uint32_t c = gp_mix(G_ACC, G_BG, faded ? 200 : 90);
    for(int i = 0; i < NP; i++){
        int x = (int)floorf(px[i]); if(x > GW || x + PW < 0) continue;
        int top = (int)(gy[i] - GAPH / 2.0f), bot = (int)(gy[i] + GAPH / 2.0f);
        gp_rect(x, -20, PW, top + 20, c, 12, 255);
        gp_rect(x, bot, PW, GH + 20 - bot, c, 12, 255);
    }
}
static void render(void){
    char buf[40];
    if(mode == TITLE){
        gp_clear(G_BG);
        uint32_t c = gp_mix(G_ACC, G_BG, 90);
        gp_rect(206, 22, 24, 40, c, 8, 255); gp_rect(206, 92, 24, 40, c, 8, 255);   /* a tiny scene as the logo */
        draw_orb(160, 80, G_TEXT);
        gp_text(146, "GLIDE", 6, G_TEXT);
        snprintf(buf, sizeof buf, "BEST %d", g_best); gp_text(196, buf, 2, G_DIM);
        gp_button(224, "PLAY", 1);
        gp_button(276, "EXIT", 0);
        return;
    }
    if(!g_bg_ok) build_backdrop();
    memcpy(g_frame, g_bg, sizeof g_bg);
    draw_pillars(mode == OVER);
    draw_orb(ORB_X, oy, mode == OVER ? G_DIM : G_TEXT);
    if(mode == OVER){
        for(int i = 0; i < GW * GH; i++) g_frame[i] = gp_mix(g_frame[i], G_BG, 120);
        gp_text(96, "GAME OVER", 4, G_TEXT);
        snprintf(buf, sizeof buf, "%d", score); gp_text(144, buf, 6, G_ACC);
        snprintf(buf, sizeof buf, newbest ? "NEW BEST!" : "BEST %d", g_best); gp_text(198, buf, 2, newbest ? G_TEXT : G_DIM);
        gp_button(234, "AGAIN", 1);
        gp_text(290, "HOLD FOR MENU", 1, G_DIM);
        return;
    }
    snprintf(buf, sizeof buf, "%d", score); gp_text(28, buf, 3, G_TEXT);
    if(mode == READY) gp_text(250, "TAP TO FLY", 2, G_DIM);
}
static void down(int x, int y){
    (void)x; (void)y;
    double now = gp_now();
    if(mode == READY){ mode = PLAY; t_last = now; vy = FLAP; g_dirty = 1; return; }
    if(mode == PLAY){ vy = FLAP; }
}
static void up(int x, int y, int moved){
    double now = gp_now();
    if(mode == TITLE){
        if(gp_in_button(x, y, 224)){ reset(); mode = READY; t_ready = t_last = now; g_dirty = 1; }
        else if(gp_in_button(x, y, 276)) g_quit = 1;
        return;
    }
    if(mode == OVER && now - t_over > 0.5 && (gp_in_button(x, y, 234) || !moved)){   /* a beat, so a late flap can't restart */
        reset(); mode = READY; t_ready = t_last = now; g_dirty = 1;
    }
}
static void hold(void){ if(mode == TITLE) g_quit = 1; else { mode = TITLE; g_dirty = 1; } }

static int preview(const char *which){
    build_backdrop(); reset();
    if(!strcmp(which, "title")) return 0;
    for(int i = 0; i < NP; i++) px[i] = 190 + i * SPACING;
    px[0] = 40; px[1] = 236; gy[0] = 170; gy[1] = 205; oy = 162; score = 12;
    if(!strcmp(which, "ready")){ mode = READY; reset(); return 0; }
    if(!strcmp(which, "play")){ mode = PLAY; return 0; }
    if(!strcmp(which, "over")){ mode = OVER; return 0; }
    return -1;
}
static const game_t GAME = { "glide", tick, render, down, up, hold };
int glide_main(int argc, char **argv){ return gp_main(&GAME, argc, argv, preview); }
#ifdef GAMES_STANDALONE
int main(int argc, char **argv){ return glide_main(argc, argv); }
#endif
