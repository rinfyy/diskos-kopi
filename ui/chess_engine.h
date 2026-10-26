/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* chess_engine - the rules and a small alpha-beta player for the Chess app. Plain C: no LVGL, no globals beyond the
 * position the caller passes, so the bot can think on a worker thread while the UI keeps its own copy. */
#ifndef CHESS_ENGINE_H
#define CHESS_ENGINE_H
#include <stdint.h>

enum { CE_EMPTY = 0, CE_P = 1, CE_N, CE_B, CE_R, CE_Q, CE_K };   /* white > 0, black < 0 */
enum { CE_WHITE = 1, CE_BLACK = -1 };
#define CE_MAXMOVES 256
#define CE_HIST     600

typedef struct { uint8_t from, to; int8_t promo, flags; } ce_move_t;   /* flags: 1 capture, 2 en passant, 4 castle, 8 double push */
typedef struct {
    int8_t sq[64];               /* a1 = 0 .. h8 = 63 */
    int side;                    /* CE_WHITE / CE_BLACK to move */
    int castle;                  /* 1 white O-O, 2 white O-O-O, 4 black O-O, 8 black O-O-O */
    int ep;                      /* en-passant target square or -1 */
    int half, fullmove;          /* fifty-move counter, move number */
    uint64_t key;                /* position hash (repetitions) */
} ce_pos_t;
typedef struct { ce_move_t m; int8_t captured; int castle, ep, half; uint64_t key; } ce_undo_t;

enum { CE_PLAYING = 0, CE_MATE, CE_STALEMATE, CE_FIFTY, CE_MATERIAL, CE_REPEAT };

void ce_init(ce_pos_t *p);                                      /* the starting position */
int  ce_legal_moves(ce_pos_t *p, ce_move_t *out);              /* all legal moves for the side to move */
void ce_make(ce_pos_t *p, ce_move_t m, ce_undo_t *u);
void ce_unmake(ce_pos_t *p, const ce_undo_t *u);
int  ce_in_check(const ce_pos_t *p, int side);
/* the game's state; hist: the keys of earlier positions (for threefold repetition), nh of them */
int  ce_status(ce_pos_t *p, const uint64_t *hist, int nh);
/* the bot: level 0 Easy .. 3 Expert. seed varies its choice among near-equal moves. Returns 0 when there's no move. */
int  ce_think(const ce_pos_t *p, int level, unsigned seed, ce_move_t *best);
void ce_move_text(const ce_pos_t *before, ce_move_t m, char *out, int n);   /* "Nf3", "exd5", "O-O", "e8=Q+" */
#endif
