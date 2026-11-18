/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Racer - a top-down traffic racer for the round screen, in the spirit of the old phone car games (kopi.33).
 * Your car stays near the bottom; the road runs down towards you, faster and faster. Steer around the traffic:
 * keep a finger on the left or right half of the screen (slide across to change your mind). The distance is the
 * score; the best is kept in /usr/data/racer/best.conf. Runs as `mq_ui --racer` (games/platform.c).
 *
 *   Title: PLAY / EXIT (or hold to leave).  Ready: tap to start.  Playing: hold left / right to steer; tap the
 *   distance at the top to pause.  Paused: RESUME / MENU.  Crashed: AGAIN, or hold for the title. */
#include "platform.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define ROAD_L 96                /* the road, 3 lanes of 56 px */
#define ROAD_R 264
#define LANE_W 56
#define KERB 10
#define CAR_W 30
#define CAR_H 50
#define PLAYER_Y 262             /* the player's car, top edge */
#define NT 6                     /* traffic cars at most */
#define STEER 300.0f             /* px / s sideways */
#define V0 230.0f                /* road speed at the start, px / s */
#define VMAX 560.0f
#define FRAME (1.0 / 30.0)

enum { TITLE, READY, PLAY, PAUSE, OVER };
static int mode = TITLE, newbest, steer;          /* steer: -1 left, 0, 1 right (the finger's side) */
static float px, speed, dist, scroll, next_gap;   /* player x (centre), road speed, metres, road offset, px to next spawn */
static float tx[NT], ty[NT], tv[NT]; static int tc[NT], ton[NT];   /* traffic: centre x, top y, own speed, colour, alive */
static double t_last, t_ready, t_over, t_crash;
static int finger_down;
static uint32_t g_bg[GW * GH]; static int g_bg_ok;

static int lane_x(int lane){ return ROAD_L + LANE_W / 2 + lane * LANE_W; }
static void reset(void){
    px = lane_x(1); speed = V0; dist = 0; scroll = 0; next_gap = 120; newbest = 0; steer = 0;
    for(int i = 0; i < NT; i++) ton[i] = 0;
}
static uint32_t car_colour(int k){
    switch(k % 4){
    case 0:  return gp_mix(G_TEXT, G_BG, 40);
    case 1:  return gp_mix(G_DIM, G_TEXT, 90);
    case 2:  return gp_mix(G_ACC, G_TEXT, 150);
    default: return gp_mix(G_DIM, G_BG, 30);
    }
}
/* the still parts, drawn once: the verge, the road surface */
static void build_backdrop(void){
    uint32_t verge = gp_mix(G_BG, G_DIM, 60), road = gp_mix(G_BG, G_TEXT, 22);
    for(int y = 0; y < GH; y++) for(int x = 0; x < GW; x++)
        g_bg[y * GW + x] = (x >= ROAD_L && x < ROAD_R) ? road : verge;
    for(int k = 0; k < 40; k++){                                       /* a few fixed tufts on the verge */
        int y = (k * 97) % GH, side = k & 1, x = side ? ROAD_R + KERB + 8 + (k * 37) % 70 : ROAD_L - KERB - 10 - (k * 41) % 70;
        for(int dy = 0; dy < 3; dy++) for(int dx = 0; dx < 3; dx++){
            int xx = x + dx, yy = y + dy; if(xx >= 0 && xx < GW && yy >= 0 && yy < GH) g_bg[yy * GW + xx] = gp_mix(verge, G_DIM, 120);
        }
    }
    g_bg_ok = 1;
}
static void spawn(void){
    int lanes[3] = { 0, 1, 2 }, used = 0;
    for(int i = 0; i < NT; i++) if(ton[i] && ty[i] < 40) used |= 1 << ((int)(tx[i] - ROAD_L) / LANE_W);   /* lanes just filled */
    int n = 1 + (dist > 300 && gp_rand() % 3 == 0);                     /* later on, sometimes two at once - never three */
    for(int k = 0; k < 3; k++){ int j = (int)(gp_rand() % 3); int t = lanes[k]; lanes[k] = lanes[j]; lanes[j] = t; }
    for(int k = 0, made = 0; k < 3 && made < n; k++){
        int lane = lanes[k]; if(used & (1 << lane)) continue;
        if(((used | (1 << lane)) & 7) == 7) break;                      /* always leave a way through */
        for(int i = 0; i < NT; i++) if(!ton[i]){
            ton[i] = 1; tx[i] = (float)lane_x(lane); ty[i] = -CAR_H - 10.0f;
            tv[i] = 60.0f + (float)(gp_rand() % 70); tc[i] = (int)(gp_rand() % 4);
            used |= 1 << lane; made++; break;
        }
    }
}
static void lose(double now){
    mode = OVER; t_over = t_crash = now; g_dirty = 1;
    int sc = (int)dist;
    if(sc > g_best){ g_best = sc; newbest = 1; gp_save_best(); }
}
static int crashed(void){
    for(int i = 0; i < NT; i++){
        if(!ton[i]) continue;
        if(fabsf(tx[i] - px) < CAR_W - 6 && ty[i] + CAR_H - 6 > PLAYER_Y + 4 && ty[i] + 6 < PLAYER_Y + CAR_H - 4) return 1;   /* a little forgiving */
    }
    return 0;
}
static double tick(double now){
    float dt = (float)(now - t_last); t_last = now;
    if(dt > 0.1f) dt = 0.1f;
    if(mode == OVER) return now - t_crash < 0.6 ? (g_dirty = 1, now + FRAME) : 0;   /* the crash flash, then rest */
    if(mode != PLAY){ if(mode == READY){ g_dirty = 1; return now + FRAME; } return 0; }
    px += steer * STEER * dt;
    if(px < ROAD_L + CAR_W / 2 + 3) px = ROAD_L + CAR_W / 2 + 3;
    if(px > ROAD_R - CAR_W / 2 - 3) px = ROAD_R - CAR_W / 2 - 3;
    speed += 9.0f * dt; if(speed > VMAX) speed = VMAX;                  /* faster all the time */
    float step = speed * dt;
    scroll = fmodf(scroll + step, 48.0f);
    dist += step / 20.0f;                                               /* 20 px = 1 m */
    next_gap -= step;
    if(next_gap <= 0){ spawn(); next_gap = 150.0f + (float)(gp_rand() % 120) - (speed - V0) * 0.12f; if(next_gap < 95) next_gap = 95; }
    for(int i = 0; i < NT; i++){
        if(!ton[i]) continue;
        ty[i] += (speed - tv[i]) * dt;                                  /* they drive too, slower than you */
        if(ty[i] > GH + 10) ton[i] = 0;
    }
    if(crashed()) lose(now);
    g_dirty = 1;
    return mode == PLAY ? now + FRAME : 0;
}
static void draw_car(int cx, int top, uint32_t body, int you){
    int x = cx - CAR_W / 2;
    uint32_t dark = gp_mix(G_BG, 0xFF000000u, 120), glass = gp_mix(body, G_BG, 150);
    for(int k = 0; k < 4; k++)                                          /* wheels */
        gp_rect(k & 1 ? x + CAR_W - 3 : x - 3, top + (k < 2 ? 8 : CAR_H - 18), 6, 11, dark, 2, 255);
    gp_rect(x, top, CAR_W, CAR_H, body, 9, 255);
    gp_rect(x + 5, top + (you ? 10 : 30), CAR_W - 10, 9, glass, 3, 255);        /* the windscreen faces up for you */
    gp_rect(x + 6, top + (you ? 30 : 12), CAR_W - 12, 7, glass, 3, 200);
    if(you){ gp_rect(x + 3, top + 1, 6, 3, G_TEXT, 1, 220); gp_rect(x + CAR_W - 9, top + 1, 6, 3, G_TEXT, 1, 220); }   /* headlights */
    else   { gp_rect(x + 3, top + CAR_H - 4, 6, 3, G_ACC, 1, 230); gp_rect(x + CAR_W - 9, top + CAR_H - 4, 6, 3, G_ACC, 1, 230); }   /* tail lights */
}
static void draw_road(void){
    if(!g_bg_ok) build_backdrop();
    memcpy(g_frame, g_bg, sizeof g_bg);
    int off = (int)scroll;
    for(int y = -48 + off; y < GH; y += 48){                            /* kerbs: alternating blocks */
        int odd = ((y - off) / 48) & 1;
        uint32_t a = odd ? G_ACC : G_TEXT;
        gp_rect(ROAD_L - KERB, y, KERB, 24, a, 0, 255); gp_rect(ROAD_R, y, KERB, 24, a, 0, 255);
        gp_rect(ROAD_L - KERB, y + 24, KERB, 24, odd ? G_TEXT : G_ACC, 0, 255); gp_rect(ROAD_R, y + 24, KERB, 24, odd ? G_TEXT : G_ACC, 0, 255);
        for(int l = 1; l < 3; l++) gp_rect(ROAD_L + l * LANE_W - 2, y + 8, 4, 22, gp_mix(G_TEXT, G_BG, 70), 1, 255);   /* lane dashes */
    }
}
static void render(void){
    char buf[48];
    if(mode == TITLE){
        gp_clear(G_BG);
        uint32_t road = gp_mix(G_BG, G_TEXT, 22);
        gp_rect(140, 24, 80, 96, road, 10, 255);                        /* a little scene as the logo */
        for(int k = 0; k < 3; k++) gp_rect(178, 30 + k * 32, 4, 18, gp_mix(G_TEXT, G_BG, 70), 1, 255);
        draw_car(160, 60, G_ACC, 1);
        draw_car(200, 30, car_colour(0), 0);
        gp_text(156, "RACER", 6, G_TEXT);
        snprintf(buf, sizeof buf, "BEST %d M", g_best); gp_text(212, buf, 2, G_DIM);
        gp_button(246, "PLAY", 1);
        gp_button(296, "EXIT", 0);
        return;
    }
    draw_road();
    for(int i = 0; i < NT; i++) if(ton[i]) draw_car((int)tx[i], (int)ty[i], car_colour(tc[i]), 0);
    double now = gp_now();
    int flash = mode == OVER && now - t_crash < 0.6 && ((int)((now - t_crash) * 12) & 1);
    draw_car((int)px, PLAYER_Y, flash ? G_TEXT : G_ACC, 1);
    if(mode == OVER || mode == PAUSE){
        for(int i = 0; i < GW * GH; i++) g_frame[i] = gp_mix(g_frame[i], G_BG, 200);
        if(mode == PAUSE){
            gp_text(110, "PAUSED", 4, G_TEXT);
            snprintf(buf, sizeof buf, "%d M", (int)dist); gp_text(156, buf, 3, G_ACC);
            gp_button(214, "RESUME", 1); gp_button(266, "MENU", 0);
            return;
        }
        gp_text(96, "CRASH!", 4, G_TEXT);
        snprintf(buf, sizeof buf, "%d M", (int)dist); gp_text(146, buf, 5, G_ACC);
        snprintf(buf, sizeof buf, newbest ? "NEW BEST!" : "BEST %d M", g_best); gp_text(196, buf, 2, newbest ? G_TEXT : G_DIM);
        gp_button(234, "AGAIN", 1);
        gp_text(290, "HOLD FOR MENU", 1, G_DIM);
        return;
    }
    snprintf(buf, sizeof buf, "%d M", (int)dist);                       /* distance, and the speed under it */
    gp_rect(180 - 44, 14, 88, 30, gp_mix(G_BG, 0xFF000000u, 60), 15, 200);
    gp_text(29, buf, 2, G_TEXT);
    snprintf(buf, sizeof buf, "%d KM/H", (int)(speed * 0.36f)); gp_text(338, buf, 1, G_DIM);
    if(mode == READY){
        gp_rect(180 - 118, 178, 236, 62, gp_mix(G_BG, 0xFF000000u, 60), 16, 225);
        gp_text(196, "HOLD LEFT / RIGHT", 2, G_TEXT);
        gp_text(222, "TO STEER", 2, G_TEXT);
        gp_rect(180 - 66, 152, 132, 22, gp_mix(G_BG, 0xFF000000u, 60), 11, 225);
        gp_text(163, "TAP TO GO", 2, G_ACC);
    }
}
static void side(int x){ steer = x < 180 ? -1 : 1; }
static void down(int x, int y){
    finger_down = 1;
    if(mode == PLAY){
        if(y < 56 && x > 110 && x < 250){ mode = PAUSE; steer = 0; g_dirty = 1; return; }   /* the distance pill: pause */
        side(x);
    }
}
static void drag(int x, int y){ (void)y; if(mode == PLAY && finger_down) side(x); }
static void lift(int x, int y){ (void)x; (void)y; finger_down = 0; steer = 0; }
static void up(int x, int y, int moved){
    double now = gp_now();
    if(mode == TITLE){
        if(gp_in_button(x, y, 246)){ reset(); mode = READY; t_ready = t_last = now; g_dirty = 1; }
        else if(gp_in_button(x, y, 296)) g_quit = 1;
        return;
    }
    if(mode == READY){ mode = PLAY; t_last = now; g_dirty = 1; return; }
    if(mode == PAUSE){
        if(gp_in_button(x, y, 214)){ mode = PLAY; t_last = now; g_dirty = 1; }
        else if(gp_in_button(x, y, 266)){ mode = TITLE; g_dirty = 1; }
        return;
    }
    if(mode == OVER && now - t_over > 0.8 && (gp_in_button(x, y, 234) || !moved)){   /* a beat, so a late steer can't restart */
        reset(); mode = READY; t_ready = t_last = now; g_dirty = 1;
    }
}
static void hold(void){
    if(mode == PLAY) return;                                            /* holding is steering */
    if(mode == TITLE) g_quit = 1; else { mode = TITLE; g_dirty = 1; }
}

static int preview(const char *which){
    build_backdrop(); reset();
    if(!strcmp(which, "title")) return 0;
    dist = 1240; speed = 410; scroll = 17; px = lane_x(1) + 12;
    ton[0] = 1; tx[0] = lane_x(0); ty[0] = 120; tc[0] = 0;
    ton[1] = 1; tx[1] = lane_x(2); ty[1] = 30;  tc[1] = 1;
    ton[2] = 1; tx[2] = lane_x(1); ty[2] = -20; tc[2] = 2;
    if(!strcmp(which, "ready")){ reset(); mode = READY; return 0; }
    if(!strcmp(which, "play")){ mode = PLAY; return 0; }
    if(!strcmp(which, "pause")){ mode = PAUSE; return 0; }
    if(!strcmp(which, "over")){ mode = OVER; t_crash = -10; return 0; }
    return -1;
}
static const game_t GAME = { "racer", tick, render, down, up, hold, drag, lift };
int racer_main(int argc, char **argv){ return gp_main(&GAME, argc, argv, preview); }
#ifdef GAMES_STANDALONE
int main(int argc, char **argv){ return racer_main(argc, argv); }
#endif
