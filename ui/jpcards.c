/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* jpcards.c - Flashcards (SCR_JPCARDS), from Apps: Japanese vocabulary and kanji, JLPT N5 and N4.
 *
 * The deck page chooses what to learn - Vocabulary N5, N4 or both, Kanji N5, N4 or both - which side shows first
 * (Japanese, or English to recall the Japanese), and the order (shuffled or as listed). A card sits in the middle of
 * the screen; tap it to turn it over: the reading and the meaning, or for a kanji its on (katakana) and kun (hiragana)
 * readings. "Got it" takes the card out of this round; "Again" puts it back a few cards later. When every card is
 * done it says so and offers another round. The round is kept while you go back to your music.
 * Data: jpcards_data.c (generated; JLPT lists by Jonathan Waller via open-anki-jlpt-decks, kanji from KANJIDIC).
 * Japanese text: Noto Sans JP subsets (font_jp_64 / 30 / 18), Latin falls back to the theme's own font. Theme roles
 * only. */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "jpcards.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(font_jp_64) LV_FONT_DECLARE(font_jp_30) LV_FONT_DECLARE(font_jp_18)
enum { D_V5, D_V4, D_V54, D_K5, D_K4, D_K54, D_COUNT };
static const char *const DECKS[D_COUNT] = { "Vocabulary N5", "Vocabulary N4", "Vocabulary N5+N4", "Kanji N5", "Kanji N4", "Kanji N5+N4" };
static const char *const SHORT[D_COUNT] = { "N5 VOCABULARY", "N4 VOCABULARY", "N5+N4 VOCABULARY", "N5 KANJI", "N4 KANJI", "N5+N4 KANJI" };
static const char *const FRONTS[2] = { "Japanese", "English" };
static const char *const ORDERS[2] = { "Shuffled", "In order" };

static lv_obj_t *g_root, *g_card, *g_big, *g_line1, *g_line2, *g_line3, *g_info, *g_hint, *g_again, *g_got, *g_again_l, *g_got_l;
static lv_obj_t *g_setup, *g_deck_l, *g_front_l, *g_order_l;
static lv_font_t g_f64, g_f30, g_f18;                  /* the Japanese fonts, with the theme's font for Latin behind */
static const jp_card_t **g_cards; static int g_ncards;
static int *g_queue, g_nq, g_known, g_deck, g_eng, g_turned, g_started, g_done;

static int is_kanji(void){ return g_deck >= D_K5; }
static void build_deck(void){
    free(g_cards); g_cards = NULL; g_ncards = 0;
    const jp_card_t *a = NULL, *b = NULL; int na = 0, nb = 0;
    switch(g_deck){
    case D_V5: a = JP_VOCAB_N5; na = JP_VOCAB_N5_N; break;
    case D_V4: a = JP_VOCAB_N4; na = JP_VOCAB_N4_N; break;
    case D_V54: a = JP_VOCAB_N5; na = JP_VOCAB_N5_N; b = JP_VOCAB_N4; nb = JP_VOCAB_N4_N; break;
    case D_K5: a = JP_KANJI_N5; na = JP_KANJI_N5_N; break;
    case D_K4: a = JP_KANJI_N4; na = JP_KANJI_N4_N; break;
    default: a = JP_KANJI_N5; na = JP_KANJI_N5_N; b = JP_KANJI_N4; nb = JP_KANJI_N4_N; break;
    }
    g_cards = malloc(sizeof *g_cards * (size_t)(na + nb)); if(!g_cards) return;
    for(int i = 0; i < na; i++) g_cards[g_ncards++] = &a[i];
    for(int i = 0; i < nb; i++) g_cards[g_ncards++] = &b[i];
}
static void new_round(void){
    free(g_queue); g_queue = malloc(sizeof *g_queue * (size_t)(g_ncards ? g_ncards : 1)); g_nq = 0; g_known = 0; g_done = 0; g_turned = 0;
    if(!g_queue) return;
    for(int i = 0; i < g_ncards; i++) g_queue[g_nq++] = i;
    if(!cfg_get_int("jp_order", 0)){                                       /* shuffled */
        unsigned s = (unsigned)time(NULL) * 2654435761u;
        for(int i = g_nq - 1; i > 0; i--){ s = s * 1103515245u + 12345u; int j = (int)((s >> 8) % (unsigned)(i + 1)); int t = g_queue[i]; g_queue[i] = g_queue[j]; g_queue[j] = t; }
    }
}
static void set_text(lv_obj_t *l, const char *t, const lv_font_t *f){
    lv_label_set_text(l, t ? t : ""); if(f) lv_obj_set_style_text_font(l, f, 0);
    if(t && *t) lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
}
static const lv_font_t *word_font(const char *w){                          /* a short word large, a long one smaller */
    int n = 0; for(const unsigned char *p = (const unsigned char *)w; *p; p++) if((*p & 0xC0) != 0x80) n++;
    return n <= 7 ? &g_f30 : &g_f18;
}
static void paint(void){
    char b[96];
    if(!g_started){ return; }
    if(g_done || !g_nq){
        snprintf(b, sizeof b, "%s  \xC2\xB7  ALL DONE", SHORT[g_deck]); lv_label_set_text(g_info, b);
        set_text(g_big, "\xE3\x81\x8A\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x84", &g_f30);   /* おしまい */
        snprintf(b, sizeof b, "%d cards, all known", g_ncards); set_text(g_line1, "Round complete", TF(UI_18)); set_text(g_line2, b, TF(UI_14)); set_text(g_line3, "", NULL);
        lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_again_l, "Decks"); lv_label_set_text(g_got_l, "Again");
        return;
    }
    lv_label_set_text(g_again_l, "Again"); lv_label_set_text(g_got_l, "Got it");
    snprintf(b, sizeof b, "%s  \xC2\xB7  %d / %d", SHORT[g_deck], g_known + 1 > g_ncards ? g_ncards : g_known + 1, g_ncards); lv_label_set_text(g_info, b);
    const jp_card_t *c = g_cards[g_queue[0]];
    int show_jp = g_eng ? g_turned : 1, show_back = g_eng ? 1 : g_turned;
    if(g_eng && !g_turned){                                                /* English first: just the meaning */
        set_text(g_big, c->meaning, TF(UI_22)); set_text(g_line1, "", NULL); set_text(g_line2, "", NULL); set_text(g_line3, "", NULL);
    } else if(!g_turned){                                                  /* Japanese first: just the word / kanji */
        set_text(g_big, c->front, is_kanji() ? &g_f64 : word_font(c->front)); set_text(g_line1, "", NULL); set_text(g_line2, "", NULL); set_text(g_line3, "", NULL);
    } else if(is_kanji()){                                                 /* the back of a kanji */
        set_text(g_big, c->front, &g_f64);
        char on[96], kun[96]; snprintf(on, sizeof on, "ON  %s", c->a && *c->a ? c->a : "-"); snprintf(kun, sizeof kun, "KUN  %s", c->b && *c->b ? c->b : "-");
        set_text(g_line1, on, &g_f18); set_text(g_line2, kun, &g_f18); set_text(g_line3, c->meaning, TF(UI_16));
    } else {                                                               /* the back of a word */
        set_text(g_big, c->front, word_font(c->front));
        set_text(g_line1, c->a, &g_f18); set_text(g_line2, "", NULL); set_text(g_line3, c->meaning, TF(UI_16));
    }
    (void)show_jp; (void)show_back;
    if(g_turned) lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(g_again, LV_OPA_COVER, 0);
}
static void card_cb(lv_event_t *e){ (void)e; if(!g_started || g_done || !g_nq) return; g_turned = !g_turned; paint(); }
static void again_cb(lv_event_t *e){
    (void)e;
    if(g_done || !g_nq){ lv_obj_remove_flag(g_setup, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(g_setup); return; }   /* "Decks" */
    int c = g_queue[0], to = g_nq < 6 ? g_nq - 1 : 5;                     /* back in a few cards' time */
    memmove(g_queue, g_queue + 1, sizeof *g_queue * (size_t)to); g_queue[to] = c;
    g_turned = 0; paint();
}
static void got_cb(lv_event_t *e){
    (void)e;
    if(g_done || !g_nq){ new_round(); paint(); return; }                  /* "Again": another round */
    memmove(g_queue, g_queue + 1, sizeof *g_queue * (size_t)(g_nq - 1)); g_nq--; g_known++;
    if(!g_nq) g_done = 1;
    g_turned = 0; paint();
}

/* ---- the deck page ---- */
static void setup_paint(void){
    lv_label_set_text(g_deck_l, DECKS[cfg_get_int("jp_deck", 0) % D_COUNT]);
    lv_label_set_text(g_front_l, FRONTS[cfg_get_int("jp_front", 0) & 1]);
    lv_label_set_text(g_order_l, ORDERS[cfg_get_int("jp_order", 0) & 1]);
}
static void deck_cb(lv_event_t *e){ int d = (int)(intptr_t)lv_event_get_user_data(e); cfg_set_int("jp_deck", (cfg_get_int("jp_deck", 0) + d + D_COUNT) % D_COUNT); setup_paint(); }
static void front_cb(lv_event_t *e){ (void)e; cfg_set_int("jp_front", !cfg_get_int("jp_front", 0)); setup_paint(); }
static void order_cb(lv_event_t *e){ (void)e; cfg_set_int("jp_order", !cfg_get_int("jp_order", 0)); setup_paint(); }
static void start_cb(lv_event_t *e){
    (void)e;
    g_deck = cfg_get_int("jp_deck", 0) % D_COUNT; g_eng = cfg_get_int("jp_front", 0) & 1;
    build_deck(); new_round(); g_started = 1;
    lv_obj_add_flag(g_setup, LV_OBJ_FLAG_HIDDEN); paint();
}
static void setup_back_cb(lv_event_t *e){ (void)e; if(g_started) lv_obj_add_flag(g_setup, LV_OBJ_FLAG_HIDDEN); else screen_back(); }
static void cards_back_cb(lv_event_t *e){ (void)e; setup_paint(); lv_obj_remove_flag(g_setup, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(g_setup); }

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
static lv_obj_t *centred(lv_obj_t *p, int w){
    lv_obj_t *l = lv_label_create(p); lv_obj_set_width(l, w); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0); lv_label_set_text(l, "");
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}
void jpcards_create(lv_obj_t *root){
    g_root = root; g_started = 0;
    g_f64 = font_jp_64; g_f64.fallback = TF(UI_40);
    g_f30 = font_jp_30; g_f30.fallback = TF(UI_24);
    g_f18 = font_jp_18; g_f18.fallback = TF(UI_16);
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0); lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    ui_header_cb(root, "Japanese", cards_back_cb);
    g_info = caption(root, "", 64); lv_obj_set_style_text_letter_space(g_info, 2, 0);
    g_card = lv_obj_create(root); lv_obj_remove_style_all(g_card); lv_obj_set_size(g_card, 268, 172); lv_obj_align(g_card, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_radius(g_card, 24, 0); lv_obj_set_style_bg_opa(g_card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_card, TC(SURFACE), 0); lv_obj_set_style_bg_color(g_card, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(g_card, 1, 0); lv_obj_set_style_border_color(g_card, TC(BORDER), 0);
    lv_obj_add_flag(g_card, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(g_card, LV_OBJ_FLAG_SCROLLABLE); kit_keep(g_card);
    lv_obj_add_event_cb(g_card, card_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(g_card, LV_FLEX_FLOW_COLUMN);                     /* the lines stack, centred, never overlap */
    lv_obj_set_flex_align(g_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_card, 4, 0); lv_obj_set_style_pad_ver(g_card, 8, 0);
    g_big = centred(g_card, 244); g_line1 = centred(g_card, 244); g_line2 = centred(g_card, 244); g_line3 = centred(g_card, 244);
    lv_label_set_long_mode(g_line1, LV_LABEL_LONG_DOT); lv_label_set_long_mode(g_line2, LV_LABEL_LONG_DOT);   /* readings: one line each */
    lv_obj_set_style_text_color(g_line1, TC(TEXT_SECONDARY), 0); lv_obj_set_style_text_color(g_line2, TC(TEXT_SECONDARY), 0);
    lv_obj_set_style_text_color(g_line3, TC(TEXT_PRIMARY), 0);
    g_hint = caption(root, "Tap the card to turn it", 262);
    g_again = pill(root, 104, 44, again_cb, NULL, "Again", TF(UI_18), &g_again_l); lv_obj_align(g_again, LV_ALIGN_TOP_MID, -58, 284);
    g_got = pill(root, 104, 44, got_cb, NULL, "Got it", TF(UI_18), &g_got_l); lv_obj_align(g_got, LV_ALIGN_TOP_MID, 58, 284);
    lv_obj_set_style_bg_color(g_got, TC(ACCENT_PRIMARY), 0); lv_obj_set_style_text_color(g_got_l, TC(ON_ACCENT), 0);
    /* the deck page, over the cards until a round starts */
    g_setup = lv_obj_create(root); lv_obj_remove_style_all(g_setup); lv_obj_set_size(g_setup, 360, 360);
    lv_obj_set_style_bg_color(g_setup, TC(CANVAS), 0); lv_obj_set_style_bg_opa(g_setup, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_setup, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(g_setup, LV_OBJ_FLAG_SCROLLABLE);
    ui_header_cb(g_setup, "Flashcards", setup_back_cb);
    caption(g_setup, "Deck", 72);
    lv_obj_t *l = pill(g_setup, 44, 44, deck_cb, (void *)(intptr_t)-1, LV_SYMBOL_LEFT, TF(UI_16), NULL); lv_obj_align(l, LV_ALIGN_TOP_MID, -112, 90);
    lv_obj_t *r = pill(g_setup, 44, 44, deck_cb, (void *)(intptr_t)1, LV_SYMBOL_RIGHT, TF(UI_16), NULL); lv_obj_align(r, LV_ALIGN_TOP_MID, 112, 90);
    g_deck_l = lv_label_create(g_setup); lv_obj_set_style_text_font(g_deck_l, TF(UI_18), 0); lv_obj_set_style_text_color(g_deck_l, TC(TEXT_PRIMARY), 0);
    lv_obj_set_width(g_deck_l, 170); lv_obj_set_style_text_align(g_deck_l, LV_TEXT_ALIGN_CENTER, 0); lv_label_set_long_mode(g_deck_l, LV_LABEL_LONG_WRAP);
    lv_obj_align(g_deck_l, LV_ALIGN_TOP_MID, 0, 102);
    caption(g_setup, "First side", 150);
    lv_obj_t *fr = pill(g_setup, 200, 40, front_cb, NULL, "", TF(UI_16), &g_front_l); lv_obj_align(fr, LV_ALIGN_TOP_MID, 0, 168);
    caption(g_setup, "Order", 216);
    lv_obj_t *od = pill(g_setup, 200, 40, order_cb, NULL, "", TF(UI_16), &g_order_l); lv_obj_align(od, LV_ALIGN_TOP_MID, 0, 234);
    lv_obj_t *gl, *go = pill(g_setup, 140, 46, start_cb, NULL, "Start", TF(UI_20), &gl); lv_obj_align(go, LV_ALIGN_TOP_MID, 0, 290);
    lv_obj_set_style_bg_color(go, TC(ACCENT_PRIMARY), 0); lv_obj_set_style_text_color(gl, TC(ON_ACCENT), 0);
    setup_paint();
}
void jpcards_open(void){
    screen_show(SCR_JPCARDS);
    if(!g_started){ setup_paint(); lv_obj_remove_flag(g_setup, LV_OBJ_FLAG_HIDDEN); }
}
/* host renders: "setup", or "deck,front,turned,skip" e.g. "3,0,1,0" (Kanji N5, Japanese first, turned over) */
void jpcards_test(const char *spec){
    if(!spec) return;
    if(!strcmp(spec, "setup")){ cards_back_cb(NULL); return; }
    int d = 0, f = 0, t = 0, k = 0; sscanf(spec, "%d,%d,%d,%d", &d, &f, &t, &k);
    cfg_set_int("jp_deck", d); cfg_set_int("jp_front", f); cfg_set_int("jp_order", 1);
    start_cb(NULL);
    for(int i = 0; i < k; i++) got_cb(NULL);
    if(t) card_cb(NULL);
}
