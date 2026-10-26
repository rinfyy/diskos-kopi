/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Snake for the Snowsky Disc's round screen. Built into mq_ui and run as `mq_ui --snake`: a child process that owns
 * the framebuffer and the touch panel until it exits (the same contract as Boomshine), so the UI's own Vol-Up escape
 * still applies.
 *
 *   Title:   tap PLAY to start; tap EXIT (or hold anywhere) to leave.
 *   Playing: swipe, or tap on the side of the head you want to go. Hold ~1 s to go back to the title.
 *   Over:    tap to play again; hold to go back to the title.
 *
 * The board is a 20 x 20 grid of 16 px cells clipped to a circle, so the snake dies on the round rim. Colours come
 * from /tmp/diskos-app-env (written by mq_ui just before launch: the current theme's canvas, text and accent), so the
 * game matches whatever theme and palette is active. The best score is kept in /usr/data/snake/best.conf.
 *
 * No LVGL: it draws into a 360 x 360 XRGB buffer and copies it to /dev/fb0, honouring Settings' screen rotation
 * (screen_rot in diskos.conf), exactly like Boomshine. `--render <screen> <out.ppm>` draws one frame for previews. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define W 360
#define H 360
#define CELL 16
#define GRID 20
#define OFF ((W - GRID * CELL) / 2)            /* 20 px margin round the grid */
#define RIM 158.0f                             /* a cell is playable if its centre is inside this radius */
#define HOLD_S 0.85

static uint32_t frame[W * H];
static uint32_t C_BG = 0x000000, C_TEXT = 0xF3E9DF, C_DIM = 0xA8968A, C_ACC = 0xB89AD6, C_FOOD = 0xF3E9DF;
static int rotation;
static volatile sig_atomic_t quitting;

/* ---- game state ------------------------------------------------------------------------------------------------ */
enum { TITLE, PLAY, OVER };
static int mode = TITLE, dirty = 1, best, score, newbest;
static unsigned char inside[GRID][GRID];       /* playable cells */
static int sx[GRID * GRID], sy[GRID * GRID], len, dir, nextdir, fx, fy;   /* snake (head at index 0), food */
static double steptime;                        /* seconds per move */
static uint32_t rng = 0x9E3779B9u;
static char state_path[96] = "/usr/data/snake/best.conf";

static double seconds(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static uint32_t rnd(void){ rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static const int DX[4] = { 1, 0, -1, 0 }, DY[4] = { 0, 1, 0, -1 };   /* right, down, left, up */

static void build_board(void){
    for(int y = 0; y < GRID; y++) for(int x = 0; x < GRID; x++){
        float cx = OFF + x * CELL + CELL / 2.0f - W / 2.0f, cy = OFF + y * CELL + CELL / 2.0f - H / 2.0f;
        inside[y][x] = cx * cx + cy * cy <= RIM * RIM;
    }
}
static int on_snake(int x, int y, int upto){ for(int i = 0; i < upto; i++) if(sx[i] == x && sy[i] == y) return 1; return 0; }
static void place_food(void){
    int free_cells = 0;
    for(int y = 0; y < GRID; y++) for(int x = 0; x < GRID; x++) if(inside[y][x] && !on_snake(x, y, len)) free_cells++;
    if(!free_cells){ fx = fy = -1; return; }
    int k = (int)(rnd() % (uint32_t)free_cells);
    for(int y = 0; y < GRID; y++) for(int x = 0; x < GRID; x++) if(inside[y][x] && !on_snake(x, y, len) && k-- == 0){ fx = x; fy = y; return; }
}
static void save_best(void){
    mkdir("/usr/data/snake", 0755);
    char tmp[128]; snprintf(tmp, sizeof tmp, "%s.tmp", state_path);
    FILE *f = fopen(tmp, "w"); if(!f) return;
    int ok = fprintf(f, "best=%d\n", best) > 0;
    if(fflush(f) != 0 || fsync(fileno(f)) != 0) ok = 0;
    if(fclose(f) != 0) ok = 0;
    if(ok) rename(tmp, state_path); else unlink(tmp);
}
static void start(void){
    len = 4; dir = nextdir = 0; score = 0; newbest = 0; steptime = 1.0 / 7.0;
    for(int i = 0; i < len; i++){ sx[i] = GRID / 2 - 2 - i; sy[i] = GRID / 2; }
    rng ^= (uint32_t)(seconds() * 1000.0);
    place_food(); mode = PLAY; dirty = 1;
}
static void game_over(void){
    mode = OVER; dirty = 1;
    if(score > best){ best = score; newbest = 1; save_best(); }
}
static void step(void){
    dir = nextdir;
    int nx = sx[0] + DX[dir], ny = sy[0] + DY[dir];
    int grow = nx == fx && ny == fy;
    if(nx < 0 || ny < 0 || nx >= GRID || ny >= GRID || !inside[ny][nx] || on_snake(nx, ny, grow ? len : len - 1)){ game_over(); return; }
    if(grow && len < GRID * GRID) len++;
    for(int i = len - 1; i > 0; i--){ sx[i] = sx[i - 1]; sy[i] = sy[i - 1]; }
    sx[0] = nx; sy[0] = ny;
    if(grow){
        score++;
        steptime = 1.0 / (7.0 + score * 0.35 < 15.0 ? 7.0 + score * 0.35 : 15.0);
        place_food();
        if(fx < 0) game_over();                  /* the whole board is snake: you win, really */
    }
    dirty = 1;
}
static void turn(int d){ if(d >= 0 && d < 4 && d != (dir + 2) % 4) nextdir = d; }

/* ---- drawing --------------------------------------------------------------------------------------------------- */
static uint32_t mix(uint32_t a, uint32_t b, int t){        /* t of 255 toward b */
    int r = ((a >> 16 & 255) * (255 - t) + (b >> 16 & 255) * t) / 255;
    int g = ((a >> 8 & 255) * (255 - t) + (b >> 8 & 255) * t) / 255;
    int l = ((a & 255) * (255 - t) + (b & 255) * t) / 255;
    return (uint32_t)(r << 16 | g << 8 | l);
}
static void put(int x, int y, uint32_t c, int a){
    if(x < 0 || y < 0 || x >= W || y >= H || a <= 0) return;
    uint32_t *p = &frame[y * W + x]; *p = a >= 255 ? c : mix(*p, c, a);
}
static void rect(int x, int y, int w, int h, uint32_t c, int radius){
    for(int j = 0; j < h; j++) for(int i = 0; i < w; i++){
        int dx = i < radius ? radius - i : i >= w - radius ? i - (w - radius - 1) : 0;
        int dy = j < radius ? radius - j : j >= h - radius ? j - (h - radius - 1) : 0;
        if(dx && dy){ float d = sqrtf((float)(dx * dx + dy * dy)) - radius; if(d > 0.5f) continue;
                      put(x + i, y + j, c, d > -0.5f ? (int)(255 * (0.5f - d)) : 255); }
        else put(x + i, y + j, c, 255);
    }
}
static void disc(float cx, float cy, float r, uint32_t c, int a){
    for(int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) for(int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++){
        float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) - r;
        if(d < 0.5f) put(x, y, c, d > -0.5f ? (int)(a * (0.5f - d)) : a);
    }
}
static void ring(float r, float width, uint32_t c, int a){
    for(int y = 0; y < H; y++) for(int x = 0; x < W; x++){
        float d = fabsf(sqrtf((x + 0.5f - 180) * (x + 0.5f - 180) + (y + 0.5f - 180) * (y + 0.5f - 180)) - r) - width / 2;
        if(d < 0.5f) put(x, y, c, d > -0.5f ? (int)(a * (0.5f - d)) : a);
    }
}

/* 5 x 7 pixel font: A-Z, 0-9 and a little punctuation, one byte per row (bit 4 = left column). */
static const unsigned char FONT[][7] = {
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},
    {31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,2,18,12},
    {17,18,20,24,20,18,17},{16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,17,25,21,19,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},{17,17,10,4,4,4,4},
    {31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},{2,6,10,18,31,2,2},
    {31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,2,12},
    {0,0,0,0,0,0,0},{0,4,4,0,4,4,0},{0,0,0,0,0,12,12},{1,2,2,4,8,8,16},{4,4,4,4,4,0,4}
};
static int glyph(char c){
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return c - 'a';
    if(c >= '0' && c <= '9') return 26 + c - '0';
    return c == ':' ? 37 : c == '.' ? 38 : c == '/' ? 39 : c == '!' ? 40 : 36;
}
static int text_w(const char *s, int scale){ int n = (int)strlen(s); return n ? n * 6 * scale - scale : 0; }
static void text(int cy, const char *s, int scale, uint32_t c){       /* centred on x, cy = top */
    int x = (W - text_w(s, scale)) / 2;
    for(; *s; s++, x += 6 * scale){
        const unsigned char *g = FONT[glyph(*s)];
        for(int r = 0; r < 7; r++) for(int b = 0; b < 5; b++) if(g[r] >> (4 - b) & 1)
            for(int j = 0; j < scale; j++) for(int i = 0; i < scale; i++) put(x + b * scale + i, cy + r * scale + j, c, 255);
    }
}
static void button(int cy, const char *label, int filled){
    int w = text_w(label, 2) + 48, h = 40, x = (W - w) / 2;
    if(filled){ rect(x, cy, w, h, C_ACC, h / 2); text(cy + 13, label, 2, C_BG); }
    else { rect(x, cy, w, h, C_DIM, h / 2); rect(x + 2, cy + 2, w - 4, h - 4, C_BG, h / 2 - 2); text(cy + 13, label, 2, C_TEXT); }
}
static void draw_board(int faded){
    for(int y = 0; y < GRID; y++) for(int x = 0; x < GRID; x++) if(inside[y][x])     /* a faint dot grid */
        disc(OFF + x * CELL + CELL / 2.0f, OFF + y * CELL + CELL / 2.0f, 1.0f, C_DIM, faded ? 30 : 55);
    ring(RIM + 8, 1.5f, C_DIM, faded ? 50 : 90);
    if(fx >= 0) disc(OFF + fx * CELL + CELL / 2.0f, OFF + fy * CELL + CELL / 2.0f, 5.0f, C_FOOD, faded ? 90 : 255);
    for(int i = len - 1; i >= 0; i--){
        int t = len > 1 ? i * 150 / (len - 1) : 0;                  /* the tail fades toward the canvas */
        uint32_t c = i == 0 ? C_TEXT : mix(C_ACC, C_BG, t);
        if(faded) c = mix(c, C_BG, 215);
        int x = OFF + sx[i] * CELL + 2, y = OFF + sy[i] * CELL + 2, w = CELL - 4, h = CELL - 4;
        if(i > 0){                                                  /* join to the segment ahead: one continuous body */
            int px = OFF + sx[i - 1] * CELL + 2, py = OFF + sy[i - 1] * CELL + 2;
            int x0 = x < px ? x : px, y0 = y < py ? y : py;
            rect(x0, y0, (x > px ? x : px) + w - x0, (y > py ? y : py) + h - y0, c, 4);
        } else rect(x - 1, y - 1, w + 2, h + 2, c, 5);              /* the head, a touch larger */
    }
}
static void render(void){
    for(int i = 0; i < W * H; i++) frame[i] = C_BG;
    char buf[48];
    if(mode == TITLE){
        static const signed char S[][2] = { {5,9},{6,9},{7,9},{8,9},{8,10},{8,11},{9,11},{10,11},{11,11},{11,10} };
        for(unsigned i = 0; i < sizeof S / sizeof S[0]; i++){
            int k = (int)(sizeof S / sizeof S[0]) - 1 - (int)i;
            uint32_t c = k == 0 ? C_TEXT : mix(C_ACC, C_BG, k * 14);
            int x = OFF + S[i][0] * CELL + 26, y = 42 + (S[i][1] - 9) * CELL, w = CELL - 4;
            if(i + 1 < sizeof S / sizeof S[0]){ int nx = OFF + S[i + 1][0] * CELL + 26, ny = 42 + (S[i + 1][1] - 9) * CELL;
                int x0 = x < nx ? x : nx, y0 = y < ny ? y : ny; rect(x0, y0, (x > nx ? x : nx) + w - x0, (y > ny ? y : ny) + w - y0, c, 4); }
            else rect(x - 1, y - 1, w + 2, w + 2, c, 5);
        }
        disc(OFF + 14 * CELL + 8 + 24, 40 + 8, 5, C_FOOD, 255);
        text(112, "SNAKE", 6, C_TEXT);
        snprintf(buf, sizeof buf, "BEST %d", best); text(170, buf, 2, C_DIM);
        button(204, "PLAY", 1);
        button(258, "EXIT", 0);
        text(316, "SWIPE OR TAP TO TURN", 1, C_DIM);
    } else if(mode == PLAY){
        draw_board(0);
        snprintf(buf, sizeof buf, "%d", score); text(2, buf, 2, C_TEXT);
    } else {
        draw_board(1);
        text(92, "GAME OVER", 4, C_TEXT);
        snprintf(buf, sizeof buf, "%d", score); text(142, buf, 6, C_ACC);
        snprintf(buf, sizeof buf, newbest ? "NEW BEST!" : "BEST %d", best); text(198, buf, 2, newbest ? C_TEXT : C_DIM);
        button(236, "AGAIN", 1);
        text(292, "HOLD FOR MENU", 1, C_DIM);
    }
}

/* ---- input ----------------------------------------------------------------------------------------------------- */
static int in_button(int x, int y, int cy){ return x >= 100 && x <= 260 && y >= cy - 6 && y <= cy + 46; }
static void tap(int x, int y){
    dirty = 1;
    if(mode == TITLE){ if(in_button(x, y, 204)) start(); else if(in_button(x, y, 258)) quitting = 1; return; }
    if(mode == OVER){ start(); return; }
    int hx = OFF + sx[0] * CELL + CELL / 2, hy = OFF + sy[0] * CELL + CELL / 2, dx = x - hx, dy = y - hy;
    if(dir == 0 || dir == 2) turn(dy > 0 ? 1 : 3);              /* moving sideways: tap above/below the head */
    else turn(dx > 0 ? 0 : 2);                                  /* moving up/down: tap left/right of it */
}
static void swipe(int dx, int dy){
    if(mode != PLAY) return;
    if(abs(dx) > abs(dy)) turn(dx > 0 ? 0 : 2); else turn(dy > 0 ? 1 : 3);
}
static void hold(void){
    if(mode == TITLE) quitting = 1;
    else { mode = TITLE; dirty = 1; }
}

static void from_raw(int rx, int ry, int *x, int *y){        /* the same mapping as Boomshine */
    switch(rotation){ case 1: *x = W - 1 - ry; *y = rx; break; case 2: *x = rx; *y = ry; break;
                      case 3: *x = ry; *y = W - 1 - rx; break; default: *x = W - 1 - rx; *y = H - 1 - ry; break; }
}
typedef struct { int fd, mt, xmin, xmax, ymin, ymax, slot, rawx, rawy, down, was, x0, y0, x, y, moved, used, dropped; double t0; } Touch;
static int scale_axis(int v, int lo, int hi){ if(hi <= lo){ lo = 0; hi = 359; } if(v < lo) v = lo; if(v > hi) v = hi; return (int)(((int64_t)v - lo) * 359 / ((int64_t)hi - lo)); }
static int open_touch(Touch *t){
    for(int i = 0; i < 32; i++){
        char path[40]; snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if(fd < 0) continue;
        struct input_absinfo ax, ay; int mt = 1;
        if(ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) < 0 || ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) < 0){
            mt = 0;
            if(ioctl(fd, EVIOCGABS(ABS_X), &ax) < 0 || ioctl(fd, EVIOCGABS(ABS_Y), &ay) < 0){ close(fd); continue; }
        }
        if(ax.maximum <= ax.minimum || ay.maximum <= ay.minimum){ close(fd); continue; }
        t->fd = fd; t->mt = mt; t->xmin = ax.minimum; t->xmax = ax.maximum; t->ymin = ay.minimum; t->ymax = ay.maximum;
        fprintf(stderr, "snake: touch=%s %s\n", path, mt ? "MT" : "ABS");
        return 0;
    }
    return -1;
}
static void read_touch(Touch *t){
    struct input_event ev[32]; ssize_t n;
    while((n = read(t->fd, ev, sizeof ev)) > 0) for(size_t i = 0; i < (size_t)n / sizeof ev[0]; i++){
        struct input_event *e = &ev[i];
        if(e->type == EV_SYN && e->code == SYN_DROPPED){ t->dropped = 1; t->used = 1; continue; }
        if(e->type == EV_ABS && e->code == ABS_MT_SLOT){ t->slot = e->value; continue; }
        if(e->type == EV_ABS && t->slot == 0){
            if(e->code == (t->mt ? ABS_MT_POSITION_X : ABS_X)) t->rawx = e->value;
            else if(e->code == (t->mt ? ABS_MT_POSITION_Y : ABS_Y)) t->rawy = e->value;
            else if(e->code == ABS_MT_TRACKING_ID) t->down = e->value >= 0;
            continue;
        }
        if(e->type == EV_KEY && e->code == BTN_TOUCH && !t->mt){ t->down = e->value != 0; continue; }
        if(e->type != EV_SYN || e->code != SYN_REPORT) continue;
        if(t->dropped){ if(!t->down){ t->dropped = 0; t->was = 0; } continue; }   /* a lost packet never acts */
        from_raw(scale_axis(t->rawx, t->xmin, t->xmax), scale_axis(t->rawy, t->ymin, t->ymax), &t->x, &t->y);
        if(t->down && !t->was){ t->x0 = t->x; t->y0 = t->y; t->moved = 0; t->used = 0; t->t0 = seconds(); }
        if(t->down && !t->used){
            int dx = t->x - t->x0, dy = t->y - t->y0;
            if(abs(dx) > 14 || abs(dy) > 14) t->moved = 1;
            if(mode == PLAY && (abs(dx) > 26 || abs(dy) > 26)){ swipe(dx, dy); t->used = 1; }   /* turn mid-swipe: snappier */
        }
        if(!t->down && t->was && !t->used){
            if(t->moved) swipe(t->x - t->x0, t->y - t->y0); else tap(t->x0, t->y0);
            t->used = 1;
        }
        t->was = t->down;
    }
}

/* ---- display --------------------------------------------------------------------------------------------------- */
typedef struct { int fd; unsigned char *map; struct fb_var_screeninfo v; struct fb_fix_screeninfo f; size_t offset; } Display;
static int open_display(Display *d){
    memset(d, 0, sizeof *d); d->fd = open("/dev/fb0", O_RDWR | O_CLOEXEC); if(d->fd < 0) return -1;
    if(ioctl(d->fd, FBIOGET_VSCREENINFO, &d->v) < 0 || ioctl(d->fd, FBIOGET_FSCREENINFO, &d->f) < 0) goto fail;
    if(d->v.xres != W || d->v.yres != H || d->v.bits_per_pixel != 32 || d->f.line_length < W * 4 ||
       d->v.red.length != 8 || d->v.green.length != 8 || d->v.blue.length != 8){ errno = ENOTSUP; goto fail; }
    d->offset = (size_t)d->v.yoffset * d->f.line_length + (size_t)d->v.xoffset * 4;   /* the visible page */
    if(d->offset + (H - 1) * (size_t)d->f.line_length + W * 4 > d->f.smem_len){ errno = EINVAL; goto fail; }
    d->map = mmap(NULL, d->f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if(d->map == MAP_FAILED) goto fail;
    return 0;
fail: close(d->fd); d->fd = -1; return -1;
}
static void present(Display *d){
    int first, stepx, rowstep;
    switch(rotation){ case 1: first = W - 1; stepx = W; rowstep = -1; break; case 2: first = 0; stepx = 1; rowstep = W; break;
                      case 3: first = (H - 1) * W; stepx = -W; rowstep = 1; break; default: first = W * H - 1; stepx = -1; rowstep = -W; break; }
    for(int y = 0; y < H; y++, first += rowstep){
        uint32_t *dst = (uint32_t *)(d->map + d->offset + (size_t)y * d->f.line_length);
        for(int x = 0, i = first; x < W; x++, i += stepx){
            uint32_t c = frame[i];
            dst[x] = ((c >> 16 & 255) << d->v.red.offset) | ((c >> 8 & 255) << d->v.green.offset) | ((c & 255) << d->v.blue.offset);
        }
    }
}

/* ---- settings -------------------------------------------------------------------------------------------------- */
static float luma(uint32_t c){ return 0.2126f * (c >> 16 & 255) + 0.7152f * (c >> 8 & 255) + 0.0722f * (c & 255); }
static void load_settings(void){
    FILE *f = fopen("/tmp/diskos-app-env", "r");
    if(f){
        char line[96]; unsigned v; int light = 0;
        while(fgets(line, sizeof line, f)){
            if(sscanf(line, "canvas=%6x", &v) == 1) C_BG = v;
            else if(sscanf(line, "text=%6x", &v) == 1) C_TEXT = v;
            else if(sscanf(line, "text_muted=%6x", &v) == 1) C_DIM = v;
            else if(sscanf(line, "accent=%6x", &v) == 1) C_ACC = v;
            else if(sscanf(line, "variant=%u", &v) == 1) light = v == 1;
        }
        fclose(f);
        (void)light;
        C_FOOD = C_TEXT;
        if(fabsf(luma(C_ACC) - luma(C_BG)) < 40) C_ACC = C_TEXT;   /* an accent that vanishes on the canvas */
    }
    f = fopen("/usr/data/diskos.conf", "r");
    if(f){ char line[256]; int v; while(fgets(line, sizeof line, f)) if(sscanf(line, "screen_rot=%d", &v) == 1 && v >= 0 && v < 4) rotation = v; fclose(f); }
    f = fopen(state_path, "r");
    if(f){ char line[64]; int v; while(fgets(line, sizeof line, f)) if(sscanf(line, "best=%d", &v) == 1 && v >= 0 && v < 100000) best = v; fclose(f); }
}
static int write_ppm(const char *path){
    FILE *f = fopen(path, "wb"); if(!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for(int i = 0; i < W * H; i++){ unsigned char p[3] = { frame[i] >> 16, frame[i] >> 8, frame[i] }; fwrite(p, 1, 3, f); }
    return fclose(f);
}
static void on_signal(int s){ (void)s; quitting = 1; }

int snake_main(int argc, char **argv){
    build_board();
    if(argc == 4 && !strcmp(argv[1], "--render")){                 /* previews: --render title|play|over out.ppm */
        load_settings(); best = 42;
        if(strcmp(argv[2], "title")){
            start();
            static const signed char BODY[][2] = { {13,6},{13,7},{13,8},{13,9},{12,9},{11,9},{10,9},{9,9},{8,9},{8,10},{8,11},
                                                   {8,12},{9,12},{10,12},{11,12},{11,13},{11,14},{10,14},{9,14} };
            len = (int)(sizeof BODY / sizeof BODY[0]); score = len - 4; dir = nextdir = 3;
            for(int i = 0; i < len; i++){ sx[i] = BODY[i][0]; sy[i] = BODY[i][1]; }
            fx = 6; fy = 5;
            if(!strcmp(argv[2], "over")){ mode = OVER; newbest = 0; }
        }
        render(); return write_ppm(argv[3]) ? 1 : 0;
    }
    load_settings();
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGHUP, on_signal);
    Display d; if(open_display(&d) < 0){ perror("snake: display"); return 1; }
    Touch t; memset(&t, 0, sizeof t);
    if(open_touch(&t) < 0){ perror("snake: touch"); munmap(d.map, d.f.smem_len); close(d.fd); return 1; }
    double next = seconds();
    while(!quitting){
        double now = seconds();
        double deadline = mode == PLAY ? next : now + 0.25;
        if(t.down && !t.used && !t.moved && t.t0 + HOLD_S < deadline) deadline = t.t0 + HOLD_S;
        if(dirty) deadline = now;
        int ms = (int)ceil((deadline - now) * 1000); if(ms < 0) ms = 0;
        struct pollfd p = { .fd = t.fd, .events = POLLIN };
        int r = poll(&p, 1, ms);
        if(r < 0 && errno != EINTR) break;
        if(r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if(r > 0 && (p.revents & POLLIN)) read_touch(&t);
        now = seconds();
        if(t.down && !t.used && !t.moved && now - t.t0 >= HOLD_S){ t.used = 1; hold(); }
        if(mode == PLAY){
            if(now >= next){ step(); next = now + steptime; }
        } else next = now + 0.4;                 /* a short breath before the first move */
        if(dirty){ render(); present(&d); dirty = 0; }
    }
    close(t.fd); munmap(d.map, d.f.smem_len); close(d.fd);
    return 0;
}

#ifdef SNAKE_STANDALONE
int main(int argc, char **argv){ return snake_main(argc, argv); }
#endif
