/* chess_engine_test.c - perft counts (move generation incl. castling, en passant, promotion), game-end rules and a
 * bot sanity check. Build: gcc -O2 -I.. chess_engine_test.c ../chess_engine.c */
#include "chess_engine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef NDEBUG
#error "this test needs assert"
#endif
static void fen(ce_pos_t *p, const char *f){
    ce_init(p); memset(p->sq, 0, sizeof p->sq);
    int r = 7, c = 0;
    for(; *f && *f != ' '; f++){
        if(*f == '/'){ r--; c = 0; continue; }
        if(*f >= '1' && *f <= '8'){ c += *f - '0'; continue; }
        const char *P = "pnbrqk"; char lo = (char)(*f | 32); int t = (int)(strchr(P, lo) - P) + 1;
        p->sq[r * 8 + c++] = (int8_t)(*f == lo ? -t : t);
    }
    f++; p->side = *f == 'w' ? CE_WHITE : CE_BLACK; f += 2; p->castle = 0;
    for(; *f && *f != ' '; f++){ if(*f == 'K') p->castle |= 1; if(*f == 'Q') p->castle |= 2; if(*f == 'k') p->castle |= 4; if(*f == 'q') p->castle |= 8; }
    f++; p->ep = *f == '-' ? -1 : (f[0] - 'a') + (f[1] - '1') * 8;
    p->half = 0;
}
static long perft(ce_pos_t *p, int d){
    ce_move_t m[CE_MAXMOVES]; int n = ce_legal_moves(p, m);
    if(d == 1) return n;
    long t = 0;
    for(int i = 0; i < n; i++){ ce_undo_t u; ce_make(p, m[i], &u); t += perft(p, d - 1); ce_unmake(p, &u); }
    return t;
}
int main(void){
    ce_pos_t p; ce_init(&p);
    assert(perft(&p, 1) == 20); assert(perft(&p, 2) == 400); assert(perft(&p, 3) == 8902); assert(perft(&p, 4) == 197281);
    fen(&p, "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");      /* Kiwipete */
    assert(perft(&p, 1) == 48); assert(perft(&p, 2) == 2039); assert(perft(&p, 3) == 97862);
    fen(&p, "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");                                 /* position 3: ep pins */
    assert(perft(&p, 1) == 14); assert(perft(&p, 2) == 191); assert(perft(&p, 3) == 2812); assert(perft(&p, 4) == 43238);
    fen(&p, "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1");          /* position 4: promotions */
    assert(perft(&p, 1) == 6); assert(perft(&p, 2) == 264); assert(perft(&p, 3) == 9467);
    /* rules */
    fen(&p, "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"); assert(ce_status(&p, NULL, 0) == CE_STALEMATE);
    fen(&p, "7k/6Q1/6K1/8/8/8/8/8 b - - 0 1"); assert(ce_status(&p, NULL, 0) == CE_MATE);
    fen(&p, "8/8/4k3/8/8/2N5/4K3/8 w - - 0 1"); assert(ce_status(&p, NULL, 0) == CE_MATERIAL);
    /* the bot: takes a free queen, finds mate in one, every level answers */
    fen(&p, "4k3/8/8/3q4/4P3/8/8/4K3 w - - 0 1");
    ce_move_t b; for(int lv = 1; lv < 4; lv++){ assert(ce_think(&p, lv, 7, &b)); assert(b.from == 28 && b.to == 35); }
    fen(&p, "6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1");
    for(int lv = 1; lv < 4; lv++){ assert(ce_think(&p, lv, 3, &b)); char t[16]; ce_move_text(&p, b, t, sizeof t); assert(!strcmp(t, "Ra8#")); }
    ce_init(&p); clock_t c0 = clock(); assert(ce_think(&p, 3, 1, &b)); double ms = (clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
    char t[16]; ce_move_text(&p, b, t, sizeof t);
    printf("PASS chess: perft (start, Kiwipete, positions 3 and 4), mate/stalemate/material, bot takes the queen and mates; Expert opening %s in %.0f ms\n", t, ms);
    return 0;
}
