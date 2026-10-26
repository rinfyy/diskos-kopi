/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* chess.c - Chess (SCR_CHESS), from Apps: you against the Disc.
 *
 * The board fills the square inside the round screen; Undo sits on the left rim and New game on the right, the
 * game's state under the board. Tap a piece to pick it up (its moves show as dots, captures as rings), tap a square to
 * move. The bot thinks on a worker thread (chess_engine.c) at one of four levels - Easy, Medium, Hard, Expert - chosen
 * with your colour on the New game page. The game stays as it is while you go back to your music. Theme roles only:
 * the board's two greys are mixes of the fixed media white and black, the accent marks the move just made. */
#include "screens.h"
#include "theme.h"
#include "config.h"
#include "chess_engine.h"
#include "theme_kit.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(font_chess_26)
#define SQ_PX  28
#define B_X    68
#define B_Y    72
static const char *const LEVELS[4] = { "Easy", "Medium", "Hard", "Expert" };
static const char *const SIDES[3] = { "White", "Black", "Random" };

static lv_obj_t *g_root, *g_sq[64], *g_fill[64], *g_line[64], *g_hint[64], *g_status, *g_undo, *g_new;
static lv_obj_t *g_setup, *g_lvl_lbl, *g_side_lbl, *g_promo;
static lv_timer_t *g_tm;
static ce_pos_t g_pos;
static ce_undo_t g_hist[CE_HIST]; static uint64_t g_keys[CE_HIST]; static int g_nh;
static ce_move_t g_moves[CE_MAXMOVES]; static int g_nmoves;
static int g_me = CE_WHITE, g_level = 1, g_sel = -1, g_over, g_started, g_last_from = -1, g_last_to = -1;
static char g_last_txt[24];
static int g_promo_from = -1, g_promo_to = -1;
/* the bot's worker */
static int g_thinking, g_game_id; static uint32_t g_think_t0;
typedef struct { ce_pos_t pos; int level, game; unsigned seed; } job_t;
static pthread_mutex_t g_res_mx = PTHREAD_MUTEX_INITIALIZER;
static struct { int ready, game, ok; ce_move_t m; } g_res;               /* the latest answer, tagged with its game */

static int disp_to_sq(int dr, int dc){ return g_me == CE_WHITE ? (7 - dr) * 8 + dc : dr * 8 + (7 - dc); }
static lv_color_t c_light(void){ return theme_color_mix(TC(FIXED_MEDIA_WHITE), TC(FIXED_MEDIA_BLACK), 208); }
static lv_color_t c_dark(void){ return theme_color_mix(TC(FIXED_MEDIA_WHITE), TC(FIXED_MEDIA_BLACK), 138); }

static void paint(void){
    static const char *const FILL[7] = { "", "\xE2\x99\x9F", "\xE2\x99\x9E", "\xE2\x99\x9D", "\xE2\x99\x9C", "\xE2\x99\x9B", "\xE2\x99\x9A" };   /* U+265F.. solid */
    static const char *const LINE[7] = { "", "\xE2\x99\x99", "\xE2\x99\x98", "\xE2\x99\x97", "\xE2\x99\x96", "\xE2\x99\x95", "\xE2\x99\x94" };   /* U+2659.. outline */
    int check_k = -1;
    if(ce_in_check(&g_pos, g_pos.side)) for(int s = 0; s < 64; s++) if(g_pos.sq[s] == g_pos.side * CE_K) check_k = s;
    for(int d = 0; d < 64; d++){                                           /* d: the square on screen, s: on the board */
        int s = disp_to_sq(d >> 3, d & 7);
        int pc = g_pos.sq[s], t = pc < 0 ? -pc : pc, light = ((s >> 3) + (s & 7)) & 1;
        lv_color_t bg = light ? c_light() : c_dark();
        if(s == g_last_from || s == g_last_to) bg = theme_color_mix(TC(ACCENT_PRIMARY), bg, 90);
        if(s == g_sel) bg = theme_color_mix(TC(ACCENT_PRIMARY), bg, 170);
        lv_obj_set_style_bg_color(g_sq[d], bg, 0);
        lv_obj_set_style_border_width(g_sq[d], s == check_k ? 3 : 0, 0);
        lv_label_set_text(g_fill[d], FILL[t]); lv_label_set_text(g_line[d], LINE[t]);
        lv_obj_set_style_text_color(g_fill[d], pc > 0 ? TC(FIXED_MEDIA_WHITE) : TC(FIXED_MEDIA_BLACK), 0);
        int target = 0, cap = 0;
        if(g_sel >= 0) for(int i = 0; i < g_nmoves; i++) if(g_moves[i].from == g_sel && g_moves[i].to == s){ target = 1; cap = (g_moves[i].flags & 1) != 0; }
        if(target){
            lv_obj_remove_flag(g_hint[d], LV_OBJ_FLAG_HIDDEN);
            if(cap){ lv_obj_set_size(g_hint[d], SQ_PX - 2, SQ_PX - 2); lv_obj_set_style_bg_opa(g_hint[d], 0, 0); lv_obj_set_style_border_width(g_hint[d], 3, 0); }
            else { lv_obj_set_size(g_hint[d], 10, 10); lv_obj_set_style_bg_opa(g_hint[d], 200, 0); lv_obj_set_style_border_width(g_hint[d], 0, 0); }
            lv_obj_center(g_hint[d]);
        } else lv_obj_add_flag(g_hint[d], LV_OBJ_FLAG_HIDDEN);
    }
}
static void status(void){
    char b[96]; const char *who = g_pos.side == g_me ? "Your move" : "Thinking...";
    int st = g_over;
    if(st == CE_MATE) snprintf(b, sizeof b, "Checkmate - %s", g_pos.side == g_me ? "the bot wins" : "you win!");
    else if(st == CE_STALEMATE) snprintf(b, sizeof b, "Stalemate - a draw");
    else if(st == CE_FIFTY) snprintf(b, sizeof b, "Fifty moves - a draw");
    else if(st == CE_MATERIAL) snprintf(b, sizeof b, "No mate possible - a draw");
    else if(st == CE_REPEAT) snprintf(b, sizeof b, "Threefold repetition - a draw");
    else if(g_last_txt[0]) snprintf(b, sizeof b, "%s  \xC2\xB7  %s%s", g_last_txt, who, ce_in_check(&g_pos, g_pos.side) ? " (check)" : "");
    else snprintf(b, sizeof b, "%s - %s", LEVELS[g_level], who);
    lv_label_set_text(g_status, b);
    lv_obj_set_style_text_color(g_status, st ? TC(ACCENT_PRIMARY) : TC(TEXT_SECONDARY), 0);
    lv_obj_set_style_opa(g_undo, (g_nh && !g_thinking) ? LV_OPA_COVER : LV_OPA_40, 0);
}
static void refresh_moves(void){ g_nmoves = g_pos.side == g_me && !g_over ? ce_legal_moves(&g_pos, g_moves) : 0; }

static void *think_worker(void *arg){                                    /* owns its job; an old game's answer is dropped */
    job_t *j = arg; ce_move_t m = { 0 }; int ok = ce_think(&j->pos, j->level, j->seed, &m);
    pthread_mutex_lock(&g_res_mx);
    if(!g_res.ready || j->game > g_res.game){ g_res.ready = 1; g_res.game = j->game; g_res.ok = ok; g_res.m = m; }   /* never over a newer one */
    pthread_mutex_unlock(&g_res_mx);
    free(j); return NULL;
}
static void bot_start(void){
    if(g_over || g_pos.side == g_me || g_thinking) return;
    job_t *j = malloc(sizeof *j); if(!j){ ui_toast("Not enough memory"); return; }
    j->pos = g_pos; j->level = g_level; j->game = ++g_game_id; j->seed = (unsigned)time(NULL) ^ (unsigned)(g_nh * 2654435761u);
    g_thinking = 1; g_think_t0 = lv_tick_get();
    pthread_t th;
    if(pthread_create(&th, NULL, think_worker, j)) think_worker(j);         /* no thread: think here */
    else pthread_detach(th);
    lv_timer_resume(g_tm);
}
static void play(ce_move_t m){
    if(g_nh >= CE_HIST){ ui_toast("That's a very long game"); return; }
    ce_move_text(&g_pos, m, g_last_txt, sizeof g_last_txt);
    g_keys[g_nh] = g_pos.key; ce_make(&g_pos, m, &g_hist[g_nh]); g_nh++;
    g_last_from = m.from; g_last_to = m.to; g_sel = -1;
    g_over = ce_status(&g_pos, g_keys, g_nh);
    refresh_moves(); paint(); status();
    if(!g_over) bot_start();
}
static void tick_cb(lv_timer_t *t){
    (void)t;
    if(!g_thinking){ lv_timer_pause(g_tm); return; }
    if(lv_tick_elaps(g_think_t0) < 450) return;                            /* a moment to see it "think" */
    pthread_mutex_lock(&g_res_mx);
    int ready = g_res.ready, mine = ready && g_res.game == g_game_id, ok = g_res.ok; ce_move_t m = g_res.m;
    if(ready) g_res.ready = 0;
    pthread_mutex_unlock(&g_res_mx);
    if(!mine) return;                                                       /* nothing yet, or an old game's answer */
    g_thinking = 0; lv_timer_pause(g_tm);
    if(!ok){ status(); return; }
    play(m);
}

/* ---- promotion: pick the piece ---- */
static void promo_cb(lv_event_t *e){
    int pr = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_add_flag(g_promo, LV_OBJ_FLAG_HIDDEN);
    for(int i = 0; i < g_nmoves; i++)
        if(g_moves[i].from == g_promo_from && g_moves[i].to == g_promo_to && g_moves[i].promo == pr){ play(g_moves[i]); return; }
}
static void sq_cb(lv_event_t *e){
    int d = (int)(intptr_t)lv_event_get_user_data(e), s = disp_to_sq(d >> 3, d & 7);
    if(g_over || g_thinking || g_pos.side != g_me || !lv_obj_has_flag(g_promo, LV_OBJ_FLAG_HIDDEN)) return;
    int pc = g_pos.sq[s];
    if(g_sel >= 0){
        int promo = 0; ce_move_t mv = { 0 }; int ok = 0;
        for(int i = 0; i < g_nmoves; i++) if(g_moves[i].from == g_sel && g_moves[i].to == s){ ok = 1; mv = g_moves[i]; if(mv.promo) promo = 1; }
        if(ok && promo){ g_promo_from = g_sel; g_promo_to = s; lv_obj_remove_flag(g_promo, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(g_promo); return; }
        if(ok){ play(mv); return; }
    }
    int has = 0; for(int i = 0; i < g_nmoves; i++) if(g_moves[i].from == s) has = 1;
    g_sel = (pc && (pc > 0 ? CE_WHITE : CE_BLACK) == g_me && has && s != g_sel) ? s : -1;
    paint();
}
static void undo_cb(lv_event_t *e){
    (void)e;
    if(g_thinking || !g_nh){ if(!g_nh) ui_toast("Nothing to undo"); return; }
    do { g_nh--; ce_unmake(&g_pos, &g_hist[g_nh]); } while(g_nh && g_pos.side != g_me);   /* back to your move */
    if(g_pos.side != g_me){ g_game_id++; }                                 /* (the bot moved first: it plays again) */
    g_over = 0; g_sel = -1; g_last_txt[0] = 0;
    if(g_nh){ g_last_from = g_hist[g_nh - 1].m.from; g_last_to = g_hist[g_nh - 1].m.to; } else g_last_from = g_last_to = -1;
    refresh_moves(); paint(); status();
    if(g_pos.side != g_me) bot_start();
}

/* ---- New game page ---- */
static void setup_paint(void){
    lv_label_set_text(g_lvl_lbl, LEVELS[g_level]);
    lv_label_set_text(g_side_lbl, SIDES[cfg_get_int("chess_side", 0) % 3]);
}
static void lvl_cb(lv_event_t *e){ int d = (int)(intptr_t)lv_event_get_user_data(e); g_level = (g_level + d + 4) % 4; cfg_set_int("chess_level", g_level); setup_paint(); }
static void side_cb(lv_event_t *e){ int d = (int)(intptr_t)lv_event_get_user_data(e); cfg_set_int("chess_side", (cfg_get_int("chess_side", 0) + d + 3) % 3); setup_paint(); }
static void start_cb(lv_event_t *e){
    (void)e;
    int side = cfg_get_int("chess_side", 0) % 3;
    g_me = side == 0 ? CE_WHITE : side == 1 ? CE_BLACK : ((time(NULL) & 1) ? CE_WHITE : CE_BLACK);
    g_game_id++; g_thinking = 0; ce_init(&g_pos); g_nh = 0; g_over = 0; g_sel = -1; g_last_from = g_last_to = -1; g_last_txt[0] = 0; g_started = 1;
    lv_obj_add_flag(g_setup, LV_OBJ_FLAG_HIDDEN);
    refresh_moves(); paint(); status();
    bot_start();                                                           /* you're Black: the bot opens */
}
static void new_cb(lv_event_t *e){ (void)e; setup_paint(); lv_obj_remove_flag(g_setup, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(g_setup); }
static void setup_back_cb(lv_event_t *e){ (void)e; if(g_started) lv_obj_add_flag(g_setup, LV_OBJ_FLAG_HIDDEN); else screen_back(); }
static void back_cb(lv_event_t *e){ (void)e; screen_back(); }

static lv_obj_t *pill(lv_obj_t *p, int w, int h, lv_event_cb_t cb, void *ud, const char *txt, const lv_font_t *f, lv_obj_t **lbl){
    lv_obj_t *b = lv_obj_create(p); lv_obj_remove_style_all(b); lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0); lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE), 0); lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_ext_click_area(b, 6);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, txt); lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0); lv_obj_center(l);
    if(lbl) *lbl = l;
    return b;
}
static lv_obj_t *caption(lv_obj_t *p, const char *t, int y){
    lv_obj_t *l = lv_label_create(p); lv_label_set_text(l, t); lv_obj_set_style_text_font(l, TF(UI_12), 0);
    lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0); lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y); return l;
}
static void chooser(lv_obj_t *p, int y, lv_obj_t **lbl, lv_event_cb_t cb){
    lv_obj_t *l = pill(p, 44, 44, cb, (void *)(intptr_t)-1, LV_SYMBOL_LEFT, TF(UI_16), NULL); lv_obj_align(l, LV_ALIGN_TOP_MID, -94, y);
    lv_obj_t *r = pill(p, 44, 44, cb, (void *)(intptr_t)1, LV_SYMBOL_RIGHT, TF(UI_16), NULL); lv_obj_align(r, LV_ALIGN_TOP_MID, 94, y);
    *lbl = lv_label_create(p); lv_obj_set_style_text_font(*lbl, TF(UI_22), 0); lv_obj_set_style_text_color(*lbl, TC(TEXT_PRIMARY), 0);
    lv_obj_set_width(*lbl, 130); lv_obj_set_style_text_align(*lbl, LV_TEXT_ALIGN_CENTER, 0); lv_obj_align(*lbl, LV_ALIGN_TOP_MID, 0, y + 10);
}

void chess_create(lv_obj_t *root){
    g_root = root;
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0); lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    ui_header_cb(root, "Chess", back_cb);
    lv_obj_t *frame = lv_obj_create(root); lv_obj_remove_style_all(frame);   /* a fine edge round the board */
    lv_obj_set_size(frame, 8 * SQ_PX + 4, 8 * SQ_PX + 4); lv_obj_set_pos(frame, B_X - 2, B_Y - 2);
    lv_obj_set_style_radius(frame, 4, 0); lv_obj_set_style_bg_color(frame, TC(BORDER_STRONG), 0); lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE); kit_keep(frame);
    for(int dr = 0; dr < 8; dr++) for(int dc = 0; dc < 8; dc++){
        lv_obj_t *q = lv_obj_create(root); lv_obj_remove_style_all(q);
        lv_obj_set_size(q, SQ_PX, SQ_PX); lv_obj_set_style_bg_opa(q, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(q, TC(ACCENT_PRIMARY), 0);
        lv_obj_add_flag(q, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(q, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(q, 0, 0); kit_keep(q);                     /* squares stay square in every theme */
        lv_obj_t *h = lv_obj_create(q); lv_obj_remove_style_all(h); lv_obj_set_style_radius(h, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(h, TC(ACCENT_PRIMARY), 0); lv_obj_set_style_border_color(h, TC(ACCENT_PRIMARY), 0); lv_obj_set_style_border_opa(h, 200, 0);
        lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_flag(h, LV_OBJ_FLAG_HIDDEN); kit_keep(h);
        lv_obj_t *f = lv_label_create(q), *l = lv_label_create(q);
        lv_obj_set_style_text_font(f, &font_chess_26, 0); lv_obj_set_style_text_font(l, &font_chess_26, 0);
        lv_obj_set_style_text_color(l, TC(FIXED_MEDIA_BLACK), 0);
        lv_label_set_text(f, ""); lv_label_set_text(l, ""); lv_obj_align(f, LV_ALIGN_CENTER, 0, 1); lv_obj_align(l, LV_ALIGN_CENTER, 0, 1);
        lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        g_sq[dr * 8 + dc] = q; g_fill[dr * 8 + dc] = f; g_line[dr * 8 + dc] = l; g_hint[dr * 8 + dc] = h;
    }
    /* the side buttons: Undo on the left rim, New game on the right */
    lv_obj_t *ul, *nl;
    g_undo = pill(root, 46, 46, undo_cb, NULL, LV_SYMBOL_LEFT, TF(UI_18), &ul); lv_obj_set_pos(g_undo, 12, 152);
    g_new = pill(root, 46, 46, new_cb, NULL, LV_SYMBOL_PLUS, TF(UI_18), &nl); lv_obj_set_pos(g_new, 302, 152);
    lv_obj_t *uc = caption(root, "Undo", 0); lv_obj_align_to(uc, g_undo, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_t *nc = caption(root, "New", 0); lv_obj_align_to(nc, g_new, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    g_status = lv_label_create(root); lv_obj_set_width(g_status, 240); lv_label_set_long_mode(g_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(g_status, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_style_text_font(g_status, TF(UI_14), 0);
    lv_obj_align(g_status, LV_ALIGN_TOP_MID, 0, B_Y + 8 * SQ_PX + 10);
    /* promotion: four pieces over the board */
    g_promo = lv_obj_create(root); lv_obj_remove_style_all(g_promo); lv_obj_set_size(g_promo, 4 * 50 + 24, 84);
    lv_obj_align(g_promo, LV_ALIGN_CENTER, 0, 4); lv_obj_set_style_radius(g_promo, 18, 0);
    lv_obj_set_style_bg_color(g_promo, TC(SURFACE_RAISED), 0); lv_obj_set_style_bg_opa(g_promo, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_promo, 1, 0); lv_obj_set_style_border_color(g_promo, TC(BORDER_STRONG), 0);
    lv_obj_remove_flag(g_promo, LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(g_promo, LV_OBJ_FLAG_CLICKABLE);
    { lv_obj_t *c = caption(g_promo, "Promote to", 6); (void)c; }
    static const char *const PP[4] = { "\xE2\x99\x9B", "\xE2\x99\x9C", "\xE2\x99\x9D", "\xE2\x99\x9E" };
    for(int i = 0; i < 4; i++){
        lv_obj_t *b = pill(g_promo, 46, 46, promo_cb, (void *)(intptr_t)(CE_Q - i), PP[i], &font_chess_26, NULL);
        lv_obj_set_pos(b, 12 + i * 50, 28);
    }
    lv_obj_add_flag(g_promo, LV_OBJ_FLAG_HIDDEN);
    /* the New game page */
    g_setup = lv_obj_create(root); lv_obj_remove_style_all(g_setup); lv_obj_set_size(g_setup, 360, 360);
    lv_obj_set_style_bg_color(g_setup, TC(CANVAS), 0); lv_obj_set_style_bg_opa(g_setup, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_setup, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(g_setup, LV_OBJ_FLAG_SCROLLABLE);
    ui_header_cb(g_setup, "New game", setup_back_cb);
    caption(g_setup, "Bot level", 84); chooser(g_setup, 102, &g_lvl_lbl, lvl_cb);
    caption(g_setup, "You play", 162); chooser(g_setup, 180, &g_side_lbl, side_cb);
    lv_obj_t *sl, *go = pill(g_setup, 150, 48, start_cb, NULL, "Play", TF(UI_20), &sl); lv_obj_align(go, LV_ALIGN_TOP_MID, 0, 252);
    lv_obj_set_style_bg_color(go, TC(ACCENT_PRIMARY), 0); lv_obj_set_style_text_color(sl, TC(ON_ACCENT), 0);
    g_tm = lv_timer_create(tick_cb, 60, NULL); lv_timer_pause(g_tm);
    g_level = cfg_get_int("chess_level", 1) & 3;
    ce_init(&g_pos); refresh_moves(); paint(); status(); setup_paint();
    if(g_started) lv_obj_add_flag(g_setup, LV_OBJ_FLAG_HIDDEN);
    for(int dr = 0; dr < 8; dr++) for(int dc = 0; dc < 8; dc++) lv_obj_set_pos(g_sq[dr * 8 + dc], B_X + dc * SQ_PX, B_Y + dr * SQ_PX);
    for(int i = 0; i < 64; i++) lv_obj_add_event_cb(g_sq[i], sq_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
}
void chess_open(void){
    screen_show(SCR_CHESS);
    if(g_thinking) lv_timer_resume(g_tm);
}

/* host renders: CHESS = "setup" (the New game page), or "lvl,side,moves" e.g. "2,0,e2e4 e7e5 g1f3"; tap = a square
 * to tap afterwards ("g1") to show a selection */
void chess_test(const char *spec, const char *tap){
    if(!spec) return;
    if(!strcmp(spec, "setup")){ new_cb(NULL); return; }
    int lvl = 1, side = 0, k = 0; sscanf(spec, "%d,%d,%n", &lvl, &side, &k);
    g_level = lvl & 3; cfg_set_int("chess_side", side); start_cb(NULL);
    g_thinking = 0; g_game_id++;
    for(const char *m = spec + k; *m && m[1] && m[2] && m[3]; ){
        int f = (m[0] - 'a') + (m[1] - '1') * 8, t = (m[2] - 'a') + (m[3] - '1') * 8, pr = 0; m += 4;
        if(*m == 'q' || *m == 'r' || *m == 'b' || *m == 'n'){ pr = *m == 'q' ? CE_Q : *m == 'r' ? CE_R : *m == 'b' ? CE_B : CE_N; m++; }
        while(*m == ' ' || *m == '_' || *m == '.') m++;
        ce_move_t all[CE_MAXMOVES]; int n = ce_legal_moves(&g_pos, all);
        for(int i = 0; i < n; i++) if(all[i].from == f && all[i].to == t && all[i].promo == pr){
            ce_move_text(&g_pos, all[i], g_last_txt, sizeof g_last_txt); g_keys[g_nh] = g_pos.key; ce_make(&g_pos, all[i], &g_hist[g_nh]); g_nh++;
            g_last_from = f; g_last_to = t; break; }
    }
    g_over = ce_status(&g_pos, g_keys, g_nh); refresh_moves(); g_sel = -1;
    if(tap && tap[0]) g_sel = (tap[0] - 'a') + (tap[1] - '1') * 8;
    paint(); status();
    if(g_pos.side != g_me && !g_over) bot_start();
}
