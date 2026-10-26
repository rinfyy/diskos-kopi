/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Pip - a virtual pet for the round screen (an original creature). Runs as `mq_ui --pet` (games/platform.c).
 *
 * Pip hatches from an egg, then grows: baby (first day), child (to day three), then one of three adults depending on
 * how well it was looked after (Sunny, Dreamy or Wild). Time passes for real, also while the app is closed: food and
 * joy run down, messes appear, and a neglected Pip gets sick. It never dies (relaxed rules): it gets sad, and
 * medicine and care bring it back. Feed, Play, Clean and Sleep sit along the bottom; when Pip is sick, Play becomes
 * Medicine. Tap Pip to cheer it up. Hold anywhere ~1 s to leave. State: /usr/data/pip/state.conf. */
#include "platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STATE_DIR  "/usr/data/pip"
#define STATE_FILE STATE_DIR "/state.conf"
#define HATCH_S    (10 * 60)
#define BABY_AGE   (24 * 3600)
#define CHILD_AGE  (3 * 24 * 3600)
#define STEP_S     600                       /* the simulation's step: ten minutes */
#define MAX_CATCHUP (14 * 24 * 3600)

enum { EGG, BABY, CHILD, ADULT };
typedef struct {
    long long born, last;                    /* wall-clock seconds */
    float food, joy;                         /* 0..100 */
    int mess, sick, asleep, form;            /* mess: 0..4 piles; form: adult kind 0 Sunny 1 Dreamy 2 Wild */
    long long mess_clock, neglect;           /* seconds awake since the last mess; seconds neglected */
    int care_good, care_all;                 /* hourly care samples */
    long long care_clock;
} pet_t;
static pet_t P;
static const char *g_state = STATE_FILE;
static char g_msg[24]; static double g_msg_until, g_anim_until; static int g_anim;   /* 1 eat, 2 play, 3 clean, 4 meds, 5 pat */
static double g_hint_until;
static long long g_now_wall;                 /* previews pin the clock */
static double g_fixed_now;                   /* ... and the animation clock */
static double clock_now(void){ return g_fixed_now ? g_fixed_now : gp_now(); }

static long long wall(void){ return g_now_wall ? g_now_wall : (long long)time(NULL); }
static int stage(void){
    long long age = wall() - P.born;
    return age < HATCH_S ? EGG : age < BABY_AGE ? BABY : age < CHILD_AGE ? CHILD : ADULT;
}
static int night(long long t){ time_t tt = (time_t)t; struct tm lt; localtime_r(&tt, &lt); return lt.tm_hour >= 22 || lt.tm_hour < 7; }
static float clampf(float v){ return v < 0 ? 0 : v > 100 ? 100 : v; }

/* advance the pet by `secs` of real time */
static void live(long long from, long long secs){
    for(long long t = 0; t < secs; t += STEP_S){
        long long d = secs - t < STEP_S ? secs - t : STEP_S, now = from + t;
        float h = d / 3600.0f;
        if(now - P.born < HATCH_S) continue;                             /* an egg needs nothing */
        if(P.asleep){ P.food -= 2.0f * h; if(!night(now)) P.joy -= 1.0f * h; }
        else {
            P.food -= 6.0f * h; P.joy -= 4.0f * h + P.mess * 2.0f * h;
            if(night(now)) P.joy -= 2.0f * h;                             /* tired: it wants to sleep */
            P.mess_clock += d;
            if(P.mess_clock >= 3 * 3600){ P.mess_clock = 0; if(P.mess < 4) P.mess++; }
        }
        if(P.asleep && !night(now)){ time_t tt = (time_t)now; struct tm lt; localtime_r(&tt, &lt); if(lt.tm_hour >= 8) P.asleep = 0; }   /* morning */
        P.food = clampf(P.food); P.joy = clampf(P.joy);
        if(P.food < 1 || P.mess >= 3) P.neglect += d; else P.neglect = 0;
        if(P.neglect >= 6 * 3600) P.sick = 1;
        P.care_clock += d;
        if(P.care_clock >= 3600){ P.care_clock = 0; P.care_all++; if(P.food > 40 && P.joy > 40 && P.mess < 2 && !P.sick) P.care_good++; }
        /* growing up: the adult form is decided by the care so far */
        if(now - P.born < CHILD_AGE && now + d - P.born >= CHILD_AGE){
            float r = P.care_all ? (float)P.care_good / P.care_all : 1.0f;
            P.form = r >= 0.75f ? 0 : r >= 0.45f ? 1 : 2;
        }
    }
}
static void save(void){
    mkdir(STATE_DIR, 0755);
    char tmp[96]; snprintf(tmp, sizeof tmp, "%s.tmp", g_state);
    FILE *f = fopen(tmp, "w"); if(!f) return;
    int ok = fprintf(f, "version=1\nborn=%lld\nlast=%lld\nfood=%.1f\njoy=%.1f\nmess=%d\nsick=%d\nasleep=%d\nform=%d\n"
                        "mess_clock=%lld\nneglect=%lld\ncare_good=%d\ncare_all=%d\ncare_clock=%lld\nbest=%d\n",
                     P.born, P.last, P.food, P.joy, P.mess, P.sick, P.asleep, P.form, P.mess_clock, P.neglect,
                     P.care_good, P.care_all, P.care_clock, g_best) > 0;
    if(fflush(f) != 0 || fsync(fileno(f)) != 0) ok = 0;
    if(fclose(f) != 0) ok = 0;
    if(ok) rename(tmp, g_state); else unlink(tmp);
}
static void hatch_new(void){
    memset(&P, 0, sizeof P);
    P.born = P.last = wall(); P.food = 80; P.joy = 80;
}
static void load(void){
    FILE *f = fopen(g_state, "r");
    if(!f){ hatch_new(); return; }
    hatch_new();
    char line[96];
    while(fgets(line, sizeof line, f)){
        sscanf(line, "born=%lld", &P.born); sscanf(line, "last=%lld", &P.last);
        sscanf(line, "food=%f", &P.food); sscanf(line, "joy=%f", &P.joy); sscanf(line, "mess=%d", &P.mess);
        sscanf(line, "sick=%d", &P.sick); sscanf(line, "asleep=%d", &P.asleep); sscanf(line, "form=%d", &P.form);
        sscanf(line, "mess_clock=%lld", &P.mess_clock); sscanf(line, "neglect=%lld", &P.neglect);
        sscanf(line, "care_good=%d", &P.care_good); sscanf(line, "care_all=%d", &P.care_all); sscanf(line, "care_clock=%lld", &P.care_clock);
    }
    fclose(f);
    if(P.form < 0 || P.form > 2) P.form = 0;
    if(P.mess < 0 || P.mess > 4) P.mess = 0;
    P.food = clampf(P.food); P.joy = clampf(P.joy);
    long long now = wall();
    if(P.born > now || P.born <= 0) P.born = now;                       /* a clock that went backwards */
    if(P.last > now || P.last < P.born) P.last = now;
    long long gap = now - P.last; if(gap > MAX_CATCHUP) gap = MAX_CATCHUP;
    live(now - gap, gap);
    P.last = now;
}
static void sync_time(void){ long long now = wall(); if(now > P.last){ live(P.last, now - P.last); P.last = now; } }

/* ---- pictures ---------------------------------------------------------------------------------------------------- */
/* 16 x 16 bodies: '#' body, 's' shade, 'w' shine, '.' empty. Eyes and mouth are drawn on top from E[] */
static const char *const EGG_S[16] = {
    "................","......####......",".....##ww##.....","....##w#####....","....#####s##....","...##s#######...",
    "...##########...","...#######s##...","...##s########..","...###########..","...##########...","....###s####....",
    ".....#######....","......#####.....","................","................" };
static const char *const BABY_S[16] = {
    "................","................","................","................",".....######.....","....##w#####....",
    "...##w#######...","...##########...","..############..","..############..","..############..","..############..",
    "...##########...","....########....","................","................" };
static const char *const CHILD_S[16] = {
    "................","...#........#...","...##......##...","...###....###...","...##########...","..###w########..",
    "..##w#########..","..############..","..############..","..############..","..############..","..############..",
    "...##########...","...###....###...","................","................" };
static const char *const SUNNY_S[16] = {
    ".....ss.........","......ss........",".......#........",".....######.....","...###w######...","..##w#########..",
    ".##############.",".##############.",".##############.",".##############.",".##############.","..############..",
    "...##########...","...##......##...","..###......###..","................" };
static const char *const DREAMY_S[16] = {
    "................",".##..........##.",".###........###.","..###......###..","...##########...","#.##w#########.#",
    "#.#w##########.#","##############.#","#.############.#","..############..","..############..","..############..",
    "...##########...","....##....##....","................","................" };
static const char *const WILD_S[16] = {
    "..#....#...#....","..##..###.###...","..###########...","..############..",".##w###########.",".#w############.",
    "###############.","###############.",".##############.",".##############.",".##############.","..############..",
    "...##########...","..###......###..","................","................" };
typedef struct { const char *const *px; int ex0, ex1, ey, my; } sprite_t;
static const sprite_t SPR[6] = {
    { EGG_S, 0, 0, 0, 0 }, { BABY_S, 6, 9, 8, 10 }, { CHILD_S, 5, 10, 7, 10 },
    { SUNNY_S, 5, 10, 7, 10 }, { DREAMY_S, 5, 10, 8, 11 }, { WILD_S, 5, 10, 7, 10 } };

static void block(int x, int y, int s, uint32_t c){ for(int j = 0; j < s; j++) for(int i = 0; i < s; i++) gp_put(x + i, y + j, c, 255); }
static void draw_pet(int cx, int cy, int s, int mood){                 /* mood: 0 ok 1 happy 2 sad 3 sleep 4 sick */
    int st = stage(), k = st == ADULT ? 3 + P.form : st;
    const sprite_t *sp = &SPR[k];
    int x0 = cx - 8 * s, y0 = cy - 8 * s;
    uint32_t body = P.sick ? gp_mix(G_ACC, 0x7FB069, 110) : G_ACC, shade = gp_mix(body, G_BG, 90), ink = G_BG;
    for(int y = 0; y < 16; y++) for(int x = 0; x < 16; x++){
        char c = sp->px[y][x];
        if(c == '#') block(x0 + x * s, y0 + y * s, s, body);
        else if(c == 's') block(x0 + x * s, y0 + y * s, s, shade);
        else if(c == 'w') block(x0 + x * s, y0 + y * s, s, gp_mix(body, G_TEXT, 170));
    }
    if(st == EGG) return;
    int ex[2] = { sp->ex0, sp->ex1 }, ey = sp->ey;
    for(int e = 0; e < 2; e++){
        int x = x0 + ex[e] * s, y = y0 + ey * s;
        if(mood == 3){ block(x - s, y + s, s, ink); block(x, y + s, s, ink); block(x + s, y + s, s, ink); }          /* closed */
        else if(mood == 1){ block(x - s, y + s, s, ink); block(x, y, s, ink); block(x + s, y + s, s, ink); }         /* ^ ^ */
        else if(mood == 4){ block(x - s, y - s, s, ink); block(x + s, y + s, s, ink); block(x, y, s, ink); block(x + s, y - s, s, ink); block(x - s, y + s, s, ink); }   /* x x */
        else { block(x, y, s, ink); block(x, y + s, s, ink); if(mood == 2) block(x + (e ? s : -s), y + 2 * s, s, 0x6FB7FF); }   /* tear */
    }
    int mx = x0 + 7 * s, my = y0 + sp->my * s;
    uint32_t cheek = gp_mix(body, 0xFF7A9A, 150);
    block(x0 + (ex[0] - 2) * s, my - s, s, cheek); block(x0 + (ex[1] + 2) * s, my - s, s, cheek);
    if(mood == 1){ block(mx - s, my, s, ink); block(mx, my + s, s, ink); block(mx + s, my + s, s, ink); block(mx + 2 * s, my, s, ink); }
    else if(mood == 2 || mood == 4){ block(mx - s, my + s, s, ink); block(mx, my, s, ink); block(mx + s, my, s, ink); block(mx + 2 * s, my + s, s, ink); }
    else if(mood != 3){ block(mx, my, s, ink); block(mx + s, my, s, ink); }
}
/* 8 x 8 icons */
static const char *const IC_FOOD[8]  = { "...#....", "..#.....", ".##.##..", "#######.", "#######.", "#######.", ".#####..", "..###..." };
static const char *const IC_PLAY[8]  = { "..####..", ".#....#.", "#.#..#.#", "#..##..#", "#..##..#", "#.#..#.#", ".#....#.", "..####.." };
static const char *const IC_CLEAN[8] = { "...#....", "...#....", "..###...", ".#####..", "#######.", "#######.", ".#####..", "..###..." };
static const char *const IC_SLEEP[8] = { "..####..", ".###....", "###.....", "###.....", "###.....", "###.....", ".###....", "..####.." };
static const char *const IC_MEDS[8]  = { "...##...", "...##...", "...##...", "########", "########", "...##...", "...##...", "...##..." };
static const char *const MESS[5]     = { "..#...", ".###..", ".####.", "######", ".####." };
static void icon(const char *const *ic, int cx, int cy, int s, uint32_t c){
    for(int y = 0; y < 8; y++) for(int x = 0; x < 8; x++) if(ic[y][x] == '#') block(cx - 4 * s + x * s, cy - 4 * s + y * s, s, c);
}

/* ---- the screen -------------------------------------------------------------------------------------------------- */
#define BTN_Y 274
static const int BTN_X[4] = { 88, 149, 211, 272 };
static int mood(void){
    if(stage() == EGG) return 0;
    if(P.asleep) return 3;
    if(P.sick) return 4;
    if(g_anim && clock_now() < g_anim_until) return 1;
    if(P.food < 25 || P.joy < 25 || P.mess >= 3) return 2;
    return P.joy > 70 ? 1 : 0;
}
static void meter(int x, int y, const char *label, float v){
    int w = 64; uint32_t c = v < 25 ? 0xE8735A : G_ACC;
    gp_text_at(x + (w - gp_text_w(label, 1)) / 2, y, label, 1, G_DIM);
    gp_rect(x, y + 11, w, 8, gp_mix(G_DIM, G_BG, 170), 4, 255);
    int f = (int)(w * v / 100.0f); if(f > 0) gp_rect(x, y + 11, f < 8 ? 8 : f, 8, c, 4, 255);
}
static void render(void){
    gp_clear(G_BG);
    double now = g_fixed_now ? g_fixed_now : gp_now();
    int st = stage();
    char buf[40];
    if(st != EGG){
        meter(76, 40, "FOOD", P.food);
        meter(148, 40, "JOY", P.joy);
        meter(220, 40, "CLEAN", 100.0f - P.mess * 25.0f);
    } else gp_text(44, "AN EGG!", 2, G_DIM);
    if(P.asleep){ gp_disc(180, 168, 96, gp_mix(G_BG, 0x000000, 90), 255); }        /* lights out */
    int bob = (int)(3 * sin(now * (g_anim == 2 && now < g_anim_until ? 12.0 : 3.0)));
    if(st == EGG) bob = (int)(3 * sin(now * 7.0)) * ((int)(now * 2) % 3 == 0);      /* a wobble now and then */
    draw_pet(180, 160 + bob, st == EGG ? 6 : 7, mood());
    for(int i = 0; i < P.mess; i++){                                              /* messes beside it */
        int mx = i < 2 ? 92 + i * 20 : 248 + (i - 2) * 20, my = 196;
        for(int y = 0; y < 5; y++) for(int x = 0; x < 6; x++) if(MESS[y][x] == '#') block(mx + x * 3, my + y * 3, 3, gp_mix(0x8B5A2B, G_BG, 40));
    }
    if(P.asleep){ int zz = (int)(now * 1.5) % 3; for(int i = 0; i <= zz; i++) gp_text_at(232 + i * 12, 96 - i * 14, "Z", 2 + (i == 2), G_DIM); }
    if(g_anim && now < g_anim_until){                                             /* what was just given */
        const char *const *ic = g_anim == 1 ? IC_FOOD : g_anim == 2 ? IC_PLAY : g_anim == 3 ? IC_CLEAN : g_anim == 4 ? IC_MEDS : NULL;
        if(ic) icon(ic, 262, 150 - (int)((now - (g_anim_until - 1.4)) * 20), 3, G_TEXT);
    }
    if(now < g_msg_until) gp_text(224, g_msg, 2, G_TEXT);
    else {
        long long age = wall() - P.born;
        static const char *const ST[4] = { "EGG", "BABY", "CHILD", "" };
        static const char *const FORM[3] = { "SUNNY", "DREAMY", "WILD" };
        if(st == EGG) snprintf(buf, sizeof buf, "HATCHING SOON");
        else snprintf(buf, sizeof buf, "PIP  %s  DAY %lld", st == ADULT ? FORM[P.form] : ST[st], age / 86400 + 1);
        gp_text(228, buf, 1, G_DIM);
        if(P.sick) gp_text(240, "FEELING SICK", 1, 0xE8735A);
    }
    const char *const *ics[4] = { IC_FOOD, P.sick ? IC_MEDS : IC_PLAY, IC_CLEAN, IC_SLEEP };
    static const char *const LBL[4] = { "FEED", "PLAY", "CLEAN", "SLEEP" };
    for(int i = 0; i < 4; i++){
        int off = st == EGG;
        gp_disc(BTN_X[i], BTN_Y, 24, off ? gp_mix(G_BG, G_DIM, 30) : gp_mix(G_BG, G_ACC, 60), 255);
        icon(ics[i], BTN_X[i], BTN_Y, 3, off ? gp_mix(G_BG, G_DIM, 90) : G_TEXT);
        const char *l = i == 1 && P.sick ? "MEDS" : i == 3 && P.asleep ? "WAKE" : LBL[i];
        gp_text_at(BTN_X[i] - gp_text_w(l, 1) / 2, BTN_Y + 29, l, 1, off ? gp_mix(G_BG, G_DIM, 90) : G_DIM);
    }
    if(now < g_hint_until) gp_text(18, "HOLD TO EXIT", 1, G_DIM);
}
static void say(const char *m){ snprintf(g_msg, sizeof g_msg, "%s", m); g_msg_until = clock_now() + 1.6; g_dirty = 1; }
static void act(int a){ g_anim = a; g_anim_until = clock_now() + 1.4; }
static void button(int i){
    sync_time();
    if(stage() == EGG){ say("NOT YET"); return; }
    if(P.asleep && i != 3){ say("SHH... ZZZ"); return; }
    switch(i){
    case 0: if(P.food >= 95){ say("I'M FULL"); break; } P.food = clampf(P.food + 30); P.joy = clampf(P.joy + 2); act(1); say("YUM!"); break;
    case 1: if(P.sick){ P.sick = 0; P.neglect = 0; P.joy = clampf(P.joy + 5); act(4); say("ALL BETTER"); break; }
            if(P.food < 10){ say("TOO HUNGRY"); break; }
            P.joy = clampf(P.joy + 20); P.food = clampf(P.food - 5); act(2); say("WHEE!"); break;
    case 2: if(!P.mess){ say("ALREADY CLEAN"); break; } P.mess = 0; P.mess_clock = 0; P.joy = clampf(P.joy + 5); act(3); say("SPARKLY!"); break;
    case 3: P.asleep = !P.asleep; say(P.asleep ? "GOOD NIGHT" : "GOOD MORNING"); break;
    }
    save(); g_dirty = 1;
}
static double tick(double now){
    static double last_save, last_sync;
    if(now - last_sync > 20){ last_sync = now; int was = stage(); sync_time(); if(was == EGG && stage() != EGG) say("HATCHED!"); }
    if(now - last_save > 60){ last_save = now; save(); }
    g_dirty = 1;
    return now + ((g_anim && now < g_anim_until) || stage() == EGG ? 0.08 : 0.25);
}
static void down(int x, int y){ (void)x; (void)y; }
static void up(int x, int y, int moved){
    if(moved) return;
    for(int i = 0; i < 4; i++){ int dx = x - BTN_X[i], dy = y - BTN_Y; if(dx * dx + dy * dy <= 34 * 34){ button(i); return; } }
    if(abs(x - 180) < 70 && abs(y - 160) < 70){                             /* a pat */
        sync_time();
        if(stage() == EGG){ say("TAP TAP"); return; }
        if(P.asleep){ say("ZZZ"); return; }
        P.joy = clampf(P.joy + 2); act(5); say(P.sick ? "NOT FEELING WELL" : "HEHE!"); save();
    }
}
static void hold(void){ save(); g_quit = 1; }

static int preview(const char *which){
    g_state = "/tmp/pip-preview.conf"; unlink(g_state);
    g_now_wall = 1798000000LL; g_fixed_now = 1000.25;                    /* a fixed moment, for repeatable pictures */
    hatch_new();
    if(!strcmp(which, "egg")) return 0;
    long long age = !strcmp(which, "baby") ? 3600 * 5 : !strcmp(which, "child") ? 86400 * 2 : 86400 * 5;
    P.born = g_now_wall - age; P.food = 72; P.joy = 85; P.mess = 1;
    if(!strncmp(which, "adult", 5)) P.form = which[5] ? which[5] - '0' : 0;
    if(!strcmp(which, "sad")){ P.food = 12; P.joy = 20; P.mess = 3; }
    if(!strcmp(which, "sick")){ P.sick = 1; P.food = 5; P.mess = 3; }
    if(!strcmp(which, "sleep")){ P.asleep = 1; }
    if(!strcmp(which, "eat")){ act(1); say("YUM!"); }
    return 0;
}
static const game_t GAME = { "pip", tick, render, down, up, hold };
int pet_main(int argc, char **argv){
    if(argc == 4) return gp_main(&GAME, argc, argv, preview);
    load(); save();
    g_hint_until = gp_now() + 4;
    return gp_main(&GAME, argc, argv, preview);
}
#ifdef GAMES_STANDALONE
int main(int argc, char **argv){ return pet_main(argc, argv); }
#endif
