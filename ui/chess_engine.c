/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* chess_engine.c - legal move generation (castling, en passant, promotion), game-end rules (mate, stalemate, fifty
 * moves, bare kings, threefold repetition) and the bot: negamax alpha-beta with a capture search, material plus
 * piece-square tables, and a node budget so even the strongest level answers in a few seconds on the player. */
#include "chess_engine.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define F(s) ((s) & 7)
#define R(s) ((s) >> 3)
#define SQ(f, r) ((r) * 8 + (f))
#define ABS(x) ((x) < 0 ? -(x) : (x))
#define SGN(x) ((x) > 0 ? 1 : -1)

/* ---- hashing (Zobrist) ---- */
static uint64_t Z[13][64], ZSIDE, ZCAST[16], ZEP[8];
static int z_ready;
static uint64_t xs(uint64_t *s){ *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static void z_init(void){
    if(z_ready) return;
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for(int i = 0; i < 13; i++) for(int j = 0; j < 64; j++) Z[i][j] = xs(&s);
    ZSIDE = xs(&s); for(int i = 0; i < 16; i++) ZCAST[i] = xs(&s); for(int i = 0; i < 8; i++) ZEP[i] = xs(&s);
    z_ready = 1;
}
static uint64_t key_of(const ce_pos_t *p){
    uint64_t k = 0;
    for(int s = 0; s < 64; s++) if(p->sq[s]) k ^= Z[p->sq[s] + 6][s];
    if(p->side == CE_BLACK) k ^= ZSIDE;
    k ^= ZCAST[p->castle & 15];
    if(p->ep >= 0) k ^= ZEP[F(p->ep)];
    return k;
}

void ce_init(ce_pos_t *p){
    static const int8_t back[8] = { CE_R, CE_N, CE_B, CE_Q, CE_K, CE_B, CE_N, CE_R };
    z_init(); memset(p, 0, sizeof *p);
    for(int f = 0; f < 8; f++){ p->sq[SQ(f, 0)] = back[f]; p->sq[SQ(f, 1)] = CE_P; p->sq[SQ(f, 6)] = -CE_P; p->sq[SQ(f, 7)] = -back[f]; }
    p->side = CE_WHITE; p->castle = 15; p->ep = -1; p->half = 0; p->fullmove = 1;
    p->key = key_of(p);
}

static const int KN[8][2] = { {1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2} };
static const int KG[8][2] = { {1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1} };
static const int DIAG[4][2] = { {1,1},{1,-1},{-1,1},{-1,-1} };
static const int ORTH[4][2] = { {1,0},{-1,0},{0,1},{0,-1} };

/* is square s attacked by side 'by'? */
static int attacked(const ce_pos_t *p, int s, int by){
    int f = F(s), r = R(s);
    int pr = r - by;                                                       /* a pawn of 'by' attacking s stands here */
    if(pr >= 0 && pr < 8){
        if(f > 0 && p->sq[SQ(f - 1, pr)] == by * CE_P) return 1;
        if(f < 7 && p->sq[SQ(f + 1, pr)] == by * CE_P) return 1;
    }
    for(int i = 0; i < 8; i++){
        int nf = f + KN[i][0], nr = r + KN[i][1];
        if(nf >= 0 && nf < 8 && nr >= 0 && nr < 8 && p->sq[SQ(nf, nr)] == by * CE_N) return 1;
        nf = f + KG[i][0]; nr = r + KG[i][1];
        if(nf >= 0 && nf < 8 && nr >= 0 && nr < 8 && p->sq[SQ(nf, nr)] == by * CE_K) return 1;
    }
    for(int d = 0; d < 4; d++){
        for(int nf = f + DIAG[d][0], nr = r + DIAG[d][1]; nf >= 0 && nf < 8 && nr >= 0 && nr < 8; nf += DIAG[d][0], nr += DIAG[d][1]){
            int q = p->sq[SQ(nf, nr)]; if(!q) continue;
            if(q == by * CE_B || q == by * CE_Q) return 1;
            break;
        }
        for(int nf = f + ORTH[d][0], nr = r + ORTH[d][1]; nf >= 0 && nf < 8 && nr >= 0 && nr < 8; nf += ORTH[d][0], nr += ORTH[d][1]){
            int q = p->sq[SQ(nf, nr)]; if(!q) continue;
            if(q == by * CE_R || q == by * CE_Q) return 1;
            break;
        }
    }
    return 0;
}
static int king_sq(const ce_pos_t *p, int side){ for(int s = 0; s < 64; s++) if(p->sq[s] == side * CE_K) return s; return -1; }
int ce_in_check(const ce_pos_t *p, int side){ int k = king_sq(p, side); return k >= 0 && attacked(p, k, -side); }

static int add(ce_move_t *o, int n, int from, int to, int promo, int flags){
    if(n < CE_MAXMOVES){ o[n].from = (uint8_t)from; o[n].to = (uint8_t)to; o[n].promo = (int8_t)promo; o[n].flags = (int8_t)flags; }
    return n + 1;
}
static int pseudo(const ce_pos_t *p, ce_move_t *o, int captures_only){
    int n = 0, me = p->side;
    for(int s = 0; s < 64; s++){
        int pc = p->sq[s]; if(!pc || SGN(pc) != me) continue;
        int t = ABS(pc), f = F(s), r = R(s);
        if(t == CE_P){
            int dir = me, last = me > 0 ? 7 : 0, start = me > 0 ? 1 : 6, r1 = r + dir;
            if(r1 < 0 || r1 > 7) continue;
            if(!captures_only || r1 == last){
                if(!p->sq[SQ(f, r1)]){
                    if(r1 == last){ for(int pr = CE_Q; pr >= CE_N; pr--) n = add(o, n, s, SQ(f, r1), pr, 0); }
                    else {
                        n = add(o, n, s, SQ(f, r1), 0, 0);
                        if(!captures_only && r == start && !p->sq[SQ(f, r1 + dir)]) n = add(o, n, s, SQ(f, r1 + dir), 0, 8);
                    }
                }
            }
            for(int df = -1; df <= 1; df += 2){
                int nf = f + df; if(nf < 0 || nf > 7) continue;
                int to = SQ(nf, r1), q = p->sq[to];
                if(q && SGN(q) != me){
                    if(r1 == last){ for(int pr = CE_Q; pr >= CE_N; pr--) n = add(o, n, s, to, pr, 1); }
                    else n = add(o, n, s, to, 0, 1);
                } else if(to == p->ep) n = add(o, n, s, to, 0, 1 | 2);
            }
        } else if(t == CE_N || t == CE_K){
            const int (*D)[2] = t == CE_N ? KN : KG;
            for(int i = 0; i < 8; i++){
                int nf = f + D[i][0], nr = r + D[i][1];
                if(nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
                int to = SQ(nf, nr), q = p->sq[to];
                if(q && SGN(q) == me) continue;
                if(captures_only && !q) continue;
                n = add(o, n, s, to, 0, q ? 1 : 0);
            }
            if(t == CE_K && !captures_only){                                 /* castling: squares empty and not attacked */
                int home = me > 0 ? 4 : 60, ks = me > 0 ? 1 : 4, qs = me > 0 ? 2 : 8;
                if(s == home && !attacked(p, s, -me)){
                    if((p->castle & ks) && !p->sq[s + 1] && !p->sq[s + 2] && p->sq[s + 3] == me * CE_R &&
                       !attacked(p, s + 1, -me) && !attacked(p, s + 2, -me)) n = add(o, n, s, s + 2, 0, 4);
                    if((p->castle & qs) && !p->sq[s - 1] && !p->sq[s - 2] && !p->sq[s - 3] && p->sq[s - 4] == me * CE_R &&
                       !attacked(p, s - 1, -me) && !attacked(p, s - 2, -me)) n = add(o, n, s, s - 2, 0, 4);
                }
            }
        } else {
            int dirs = 0; int D[8][2];
            if(t == CE_B || t == CE_Q) for(int i = 0; i < 4; i++){ D[dirs][0] = DIAG[i][0]; D[dirs][1] = DIAG[i][1]; dirs++; }
            if(t == CE_R || t == CE_Q) for(int i = 0; i < 4; i++){ D[dirs][0] = ORTH[i][0]; D[dirs][1] = ORTH[i][1]; dirs++; }
            for(int d = 0; d < dirs; d++)
                for(int nf = f + D[d][0], nr = r + D[d][1]; nf >= 0 && nf < 8 && nr >= 0 && nr < 8; nf += D[d][0], nr += D[d][1]){
                    int to = SQ(nf, nr), q = p->sq[to];
                    if(q && SGN(q) == me) break;
                    if(!captures_only || q) n = add(o, n, s, to, 0, q ? 1 : 0);
                    if(q) break;
                }
        }
    }
    return n < CE_MAXMOVES ? n : CE_MAXMOVES;
}

static const int CASTLE_MASK[64] = {                                      /* rights left after a move touches a square */
    13,15,15,15,12,15,15,14, 15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15,  7,15,15,15, 3,15,15,11 };

void ce_make(ce_pos_t *p, ce_move_t m, ce_undo_t *u){
    u->m = m; u->castle = p->castle; u->ep = p->ep; u->half = p->half; u->key = p->key;
    int pc = p->sq[m.from], me = p->side;
    int cap_sq = (m.flags & 2) ? m.to - 8 * me : m.to;
    u->captured = p->sq[cap_sq];
    p->sq[cap_sq] = 0;
    p->sq[m.to] = m.promo ? (int8_t)(me * m.promo) : (int8_t)pc; p->sq[m.from] = 0;
    if(m.flags & 4){                                                        /* the rook jumps over */
        if(m.to > m.from){ p->sq[m.to - 1] = p->sq[m.to + 1]; p->sq[m.to + 1] = 0; }
        else { p->sq[m.to + 1] = p->sq[m.to - 2]; p->sq[m.to - 2] = 0; }
    }
    p->castle &= CASTLE_MASK[m.from] & CASTLE_MASK[m.to];
    p->ep = (m.flags & 8) ? (m.from + m.to) / 2 : -1;
    p->half = (ABS(pc) == CE_P || u->captured) ? 0 : p->half + 1;
    if(me == CE_BLACK) p->fullmove++;
    p->side = -me;
    p->key = key_of(p);
}
void ce_unmake(ce_pos_t *p, const ce_undo_t *u){
    ce_move_t m = u->m; int me = -p->side;
    int pc = m.promo ? me * CE_P : p->sq[m.to];
    p->sq[m.from] = (int8_t)pc; p->sq[m.to] = 0;
    int cap_sq = (m.flags & 2) ? m.to - 8 * me : m.to;
    p->sq[cap_sq] = u->captured;
    if(m.flags & 4){
        if(m.to > m.from){ p->sq[m.to + 1] = p->sq[m.to - 1]; p->sq[m.to - 1] = 0; }
        else { p->sq[m.to - 2] = p->sq[m.to + 1]; p->sq[m.to + 1] = 0; }
    }
    p->castle = u->castle; p->ep = u->ep; p->half = u->half; p->key = u->key;
    if(me == CE_BLACK) p->fullmove--;
    p->side = me;
}
static int legal_filter(ce_pos_t *p, ce_move_t *o, int n){
    int k = 0;
    for(int i = 0; i < n; i++){
        ce_undo_t u; ce_make(p, o[i], &u);
        int bad = ce_in_check(p, -p->side);
        ce_unmake(p, &u);
        if(!bad) o[k++] = o[i];
    }
    return k;
}
int ce_legal_moves(ce_pos_t *p, ce_move_t *out){ return legal_filter(p, out, pseudo(p, out, 0)); }

static int bare(const ce_pos_t *p){                                       /* not enough to mate: K v K, K+N v K, K+B v K */
    int minors = 0;
    for(int s = 0; s < 64; s++){ int t = ABS(p->sq[s]); if(t == CE_P || t == CE_R || t == CE_Q) return 0; if(t == CE_N || t == CE_B) minors++; }
    return minors <= 1;
}
int ce_status(ce_pos_t *p, const uint64_t *hist, int nh){
    ce_move_t m[CE_MAXMOVES];
    if(!ce_legal_moves(p, m)) return ce_in_check(p, p->side) ? CE_MATE : CE_STALEMATE;
    if(p->half >= 100) return CE_FIFTY;
    if(bare(p)) return CE_MATERIAL;
    int same = 1; for(int i = 0; i < nh; i++) if(hist[i] == p->key) same++;
    return same >= 3 ? CE_REPEAT : CE_PLAYING;
}

/* ---- the bot ---- */
static const int VAL[7] = { 0, 100, 320, 330, 500, 900, 0 };
static const int8_t PST[7][64] = {                                         /* white's view, a1 first; black mirrors */
    {0},
    {  0,  0,  0,  0,  0,  0,  0,  0,   5, 10, 10,-20,-20, 10, 10,  5,   5, -5,-10,  0,  0,-10, -5,  5,   0,  0,  0, 20, 20,  0,  0,  0,
       5,  5, 10, 25, 25, 10,  5,  5,  10, 10, 20, 30, 30, 20, 10, 10,  50, 50, 50, 50, 50, 50, 50, 50,   0,  0,  0,  0,  0,  0,  0,  0 },
    {-50,-40,-30,-30,-30,-30,-40,-50, -40,-20,  0,  5,  5,  0,-20,-40, -30,  5, 10, 15, 15, 10,  5,-30, -30,  0, 15, 20, 20, 15,  0,-30,
     -30,  5, 15, 20, 20, 15,  5,-30, -30,  0, 10, 15, 15, 10,  0,-30, -40,-20,  0,  0,  0,  0,-20,-40, -50,-40,-30,-30,-30,-30,-40,-50 },
    {-20,-10,-10,-10,-10,-10,-10,-20, -10,  5,  0,  0,  0,  0,  5,-10, -10, 10, 10, 10, 10, 10, 10,-10, -10,  0, 10, 10, 10, 10,  0,-10,
     -10,  5,  5, 10, 10,  5,  5,-10, -10,  0,  5, 10, 10,  5,  0,-10, -10,  0,  0,  0,  0,  0,  0,-10, -20,-10,-10,-10,-10,-10,-10,-20 },
    {  0,  0,  0,  5,  5,  0,  0,  0,  -5,  0,  0,  0,  0,  0,  0, -5,  -5,  0,  0,  0,  0,  0,  0, -5,  -5,  0,  0,  0,  0,  0,  0, -5,
      -5,  0,  0,  0,  0,  0,  0, -5,  -5,  0,  0,  0,  0,  0,  0, -5,   5, 10, 10, 10, 10, 10, 10,  5,   0,  0,  0,  0,  0,  0,  0,  0 },
    {-20,-10,-10, -5, -5,-10,-10,-20, -10,  0,  5,  0,  0,  0,  0,-10, -10,  5,  5,  5,  5,  5,  0,-10,   0,  0,  5,  5,  5,  5,  0, -5,
      -5,  0,  5,  5,  5,  5,  0, -5, -10,  0,  5,  5,  5,  5,  0,-10, -10,  0,  0,  0,  0,  0,  0,-10, -20,-10,-10, -5, -5,-10,-10,-20 },
    { 20, 30, 10,  0,  0, 10, 30, 20,  20, 20,  0,  0,  0,  0, 20, 20, -10,-20,-20,-20,-20,-20,-20,-10, -20,-30,-30,-40,-40,-30,-30,-20,
     -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30 },
};
static const int8_t KEND[64] = {                                           /* the king in the endgame: come to the middle */
    -50,-30,-30,-30,-30,-30,-30,-50, -30,-30,  0,  0,  0,  0,-30,-30, -30,-10, 20, 30, 30, 20,-10,-30, -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30, -30,-10, 20, 30, 30, 20,-10,-30, -30,-20,-10,  0,  0,-10,-20,-30, -50,-40,-30,-20,-20,-30,-40,-50 };
static int evaluate(const ce_pos_t *p){                                   /* from the side to move's view */
    int sc = 0, heavy = 0;
    for(int s = 0; s < 64; s++){ int t = ABS(p->sq[s]); if(t == CE_Q) heavy += 2; else if(t == CE_R || t == CE_B || t == CE_N) heavy++; }
    int endgame = heavy <= 6;
    for(int s = 0; s < 64; s++){
        int pc = p->sq[s]; if(!pc) continue;
        int t = ABS(pc), idx = pc > 0 ? s : SQ(F(s), 7 - R(s));
        int v = VAL[t] + ((t == CE_K && endgame) ? KEND[idx] : PST[t][idx]);
        sc += pc > 0 ? v : -v;
    }
    return sc * p->side;
}
typedef struct { long nodes, budget; int stop; } srch_t;
#define MATE 100000
static int order_score(const ce_pos_t *p, ce_move_t m){
    int s = 0;
    if(m.flags & 1){ int v = (m.flags & 2) ? CE_P : ABS(p->sq[m.to]); s = 10 * VAL[v] - VAL[ABS(p->sq[m.from])] + 10000; }
    if(m.promo) s += VAL[m.promo] + 9000;
    return s;
}
static void sort_moves(const ce_pos_t *p, ce_move_t *m, int n, const ce_move_t *first){
    int sc[CE_MAXMOVES];
    for(int i = 0; i < n; i++){ sc[i] = order_score(p, m[i]); if(first && m[i].from == first->from && m[i].to == first->to && m[i].promo == first->promo) sc[i] = 1 << 30; }
    for(int i = 1; i < n; i++){ ce_move_t x = m[i]; int y = sc[i], j = i - 1; while(j >= 0 && sc[j] < y){ m[j + 1] = m[j]; sc[j + 1] = sc[j]; j--; } m[j + 1] = x; sc[j + 1] = y; }
}
static int quiesce(ce_pos_t *p, int alpha, int beta, srch_t *S, int qd){
    if(++S->nodes > S->budget){ S->stop = 1; return 0; }
    int stand = evaluate(p);
    if(stand >= beta) return beta;
    if(stand > alpha) alpha = stand;
    if(qd > 6) return alpha;
    ce_move_t m[CE_MAXMOVES]; int n = pseudo(p, m, 1);
    sort_moves(p, m, n, NULL);
    for(int i = 0; i < n; i++){
        ce_undo_t u; ce_make(p, m[i], &u);
        if(ce_in_check(p, -p->side)){ ce_unmake(p, &u); continue; }
        int v = -quiesce(p, -beta, -alpha, S, qd + 1);
        ce_unmake(p, &u);
        if(S->stop) return 0;
        if(v >= beta) return beta;
        if(v > alpha) alpha = v;
    }
    return alpha;
}
static int negamax(ce_pos_t *p, int depth, int alpha, int beta, int ply, srch_t *S){
    if(depth <= 0) return quiesce(p, alpha, beta, S, 0);
    if(++S->nodes > S->budget){ S->stop = 1; return 0; }
    if(ply && p->half >= 100) return 0;
    ce_move_t m[CE_MAXMOVES]; int n = pseudo(p, m, 0);
    sort_moves(p, m, n, NULL);
    int legal = 0, check = ce_in_check(p, p->side);
    for(int i = 0; i < n; i++){
        ce_undo_t u; ce_make(p, m[i], &u);
        if(ce_in_check(p, -p->side)){ ce_unmake(p, &u); continue; }
        legal++;
        int v = -negamax(p, depth - 1 + (ce_in_check(p, p->side) && ply < 6 ? 1 : 0), -beta, -alpha, ply + 1, S);
        ce_unmake(p, &u);
        if(S->stop) return 0;
        if(v >= beta) return beta;
        if(v > alpha) alpha = v;
    }
    if(!legal) return check ? -MATE + ply : 0;
    return alpha;
}
int ce_think(const ce_pos_t *start, int level, unsigned seed, ce_move_t *best){
    static const int DEPTH[4] = { 1, 2, 4, 6 }, NOISE[4] = { 140, 35, 8, 0 };
    static const long BUDGET[4] = { 20000, 60000, 200000, 600000 };
    if(level < 0) level = 0;
    if(level > 3) level = 3;
    ce_pos_t p = *start;
    ce_move_t m[CE_MAXMOVES]; int n = ce_legal_moves(&p, m);
    if(!n) return 0;
    uint64_t rs = 0x2545F4914F6CDD1Dull ^ (uint64_t)seed * 0x9E3779B97F4A7C15ull; if(!rs) rs = 1;
    if(level == 0 && xs(&rs) % 4 == 0){ *best = m[xs(&rs) % (unsigned)n]; return 1; }   /* Easy: now and then just any move */
    srch_t S = { 0, BUDGET[level], 0 };
    ce_move_t bm = m[0]; int found = 0;
    for(int d = 1; d <= DEPTH[level]; d++){                                /* iterative deepening; a cut-off depth is dropped */
        sort_moves(&p, m, n, found ? &bm : NULL);
        int alpha = -MATE - 1, bi = -1; ce_move_t cur = m[0];
        for(int i = 0; i < n; i++){
            ce_undo_t u; ce_make(&p, m[i], &u);
            int v = -negamax(&p, d - 1, -MATE - 1, -(alpha - NOISE[level]), 1, &S);   /* wide enough for the noise */
            ce_unmake(&p, &u);
            if(S.stop) break;
            if(NOISE[level] && v < MATE - 1000 && v > -MATE + 1000){         /* the same jitter for a move at every depth */
                uint64_t h = (uint64_t)seed * 0x9E3779B97F4A7C15ull ^ (uint64_t)(m[i].from * 4096 + m[i].to * 16 + m[i].promo) * 0xBF58476D1CE4E5B9ull;
                h ^= h >> 31; v += (int)(h % (uint64_t)(2 * NOISE[level] + 1)) - NOISE[level];
            }
            if(v > alpha){ alpha = v; bi = i; cur = m[i]; }
        }
        if(S.stop && d > 1) break;
        if(bi >= 0){ bm = cur; found = 1; }
        if(S.stop) break;
    }
    *best = bm;
    return 1;
}

void ce_move_text(const ce_pos_t *before, ce_move_t m, char *out, int n){
    static const char PC[7] = { 0, 0, 'N', 'B', 'R', 'Q', 'K' };
    ce_pos_t p = *before; int t = ABS(p.sq[m.from]); char b[16]; int k = 0;
    if(m.flags & 4) k = snprintf(b, sizeof b, "%s", m.to > m.from ? "O-O" : "O-O-O");
    else {
        if(t != CE_P){
            b[k++] = PC[t];
            ce_move_t all[CE_MAXMOVES]; int na = ce_legal_moves(&p, all), sf = 0, sr = 0, amb = 0;   /* tell twins apart */
            for(int i = 0; i < na; i++) if(all[i].to == m.to && all[i].from != m.from && ABS(p.sq[all[i].from]) == t){
                amb = 1; if(F(all[i].from) == F(m.from)) sf = 1; if(R(all[i].from) == R(m.from)) sr = 1; }
            if(amb){ if(!sf) b[k++] = (char)('a' + F(m.from)); else if(!sr) b[k++] = (char)('1' + R(m.from)); else { b[k++] = (char)('a' + F(m.from)); b[k++] = (char)('1' + R(m.from)); } }
        } else if(m.flags & 1) b[k++] = (char)('a' + F(m.from));
        if(m.flags & 1) b[k++] = 'x';
        b[k++] = (char)('a' + F(m.to)); b[k++] = (char)('1' + R(m.to));
        if(m.promo){ b[k++] = '='; b[k++] = PC[m.promo]; }
        b[k] = 0;
    }
    ce_undo_t u; ce_make(&p, m, &u);
    if(ce_in_check(&p, p.side)){ ce_move_t r[CE_MAXMOVES]; b[k++] = ce_legal_moves(&p, r) ? '+' : '#'; b[k] = 0; }
    snprintf(out, (size_t)n, "%s", b);
}
