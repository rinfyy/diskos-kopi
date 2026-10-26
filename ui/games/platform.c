/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* games/platform.c - see platform.h. */
#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HOLD_S 0.85

uint32_t g_frame[GW * GH];
uint32_t G_BG = 0x000000, G_TEXT = 0xF3E9DF, G_DIM = 0xA8968A, G_ACC = 0xB89AD6;
int g_dirty = 1, g_best;
volatile int g_quit;
static int rotation;
static uint32_t rng = 0x2545F491u;
static char state_dir[64], state_path[96];

double gp_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
uint32_t gp_rand(void){ rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* ---- drawing --------------------------------------------------------------------------------------------------- */
uint32_t gp_mix(uint32_t a, uint32_t b, int t){
    if(t <= 0) return a;
    if(t >= 255) return b;
    int r = ((a >> 16 & 255) * (255 - t) + (b >> 16 & 255) * t) / 255;
    int g = ((a >> 8 & 255) * (255 - t) + (b >> 8 & 255) * t) / 255;
    int l = ((a & 255) * (255 - t) + (b & 255) * t) / 255;
    return (uint32_t)(r << 16 | g << 8 | l);
}
void gp_put(int x, int y, uint32_t c, int a){
    if(x < 0 || y < 0 || x >= GW || y >= GH || a <= 0) return;
    uint32_t *p = &g_frame[y * GW + x]; *p = a >= 255 ? c : gp_mix(*p, c, a);
}
void gp_clear(uint32_t c){ for(int i = 0; i < GW * GH; i++) g_frame[i] = c; }
void gp_rect(int x, int y, int w, int h, uint32_t c, int radius, int a){
    if(radius * 2 > w) radius = w / 2;
    if(radius * 2 > h) radius = h / 2;
    for(int j = 0; j < h; j++){
        int py = y + j; if(py < 0 || py >= GH) continue;
        for(int i = 0; i < w; i++){
            int dx = i < radius ? radius - i : i >= w - radius ? i - (w - radius - 1) : 0;
            int dy = j < radius ? radius - j : j >= h - radius ? j - (h - radius - 1) : 0;
            if(dx && dy){ float d = sqrtf((float)(dx * dx + dy * dy)) - radius; if(d > 0.5f) continue;
                          gp_put(x + i, py, c, d > -0.5f ? (int)(a * (0.5f - d)) : a); }
            else gp_put(x + i, py, c, a);
        }
    }
}
void gp_disc(float cx, float cy, float r, uint32_t c, int a){
    for(int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) for(int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++){
        float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) - r;
        if(d < 0.5f) gp_put(x, y, c, d > -0.5f ? (int)(a * (0.5f - d)) : a);
    }
}
void gp_ring(float cx, float cy, float r, float width, uint32_t c, int a){
    float o = r + width / 2 + 1;
    for(int y = (int)(cy - o); y <= (int)(cy + o); y++) for(int x = (int)(cx - o); x <= (int)(cx + o); x++){
        float d = fabsf(sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) - r) - width / 2;
        if(d < 0.5f) gp_put(x, y, c, d > -0.5f ? (int)(a * (0.5f - d)) : a);
    }
}

/* 5 x 7 pixel font: A-Z, 0-9 and a little punctuation, one byte per row (bit 4 = left column) - the same face as Snake */
static const unsigned char FONT[][7] = {
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},
    {31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,2,18,12},
    {17,18,20,24,20,18,17},{16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,17,25,21,19,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},{17,17,10,4,4,4,4},
    {31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},{2,6,10,18,31,2,2},
    {31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,2,12},
    {0,0,0,0,0,0,0},{0,4,4,0,4,4,0},{0,0,0,0,0,12,12},{1,2,2,4,8,8,16},{4,4,4,4,4,0,4},{0,0,0,31,0,0,0},{4,4,8,0,0,0,0},{0,0,0,0,12,4,8}
};
static int glyph(char c){
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return c - 'a';
    if(c >= '0' && c <= '9') return 26 + c - '0';
    return c == ':' ? 37 : c == '.' ? 38 : c == '/' ? 39 : c == '!' ? 40 : c == '-' ? 41 : c == '\'' ? 42 : c == ',' ? 43 : 36;
}
int gp_text_w(const char *s, int scale){ int n = (int)strlen(s); return n ? n * 6 * scale - scale : 0; }
void gp_text_at(int x, int cy, const char *s, int scale, uint32_t c){
    for(; *s; s++, x += 6 * scale){
        const unsigned char *g = FONT[glyph(*s)];
        for(int r = 0; r < 7; r++) for(int b = 0; b < 5; b++) if(g[r] >> (4 - b) & 1)
            for(int j = 0; j < scale; j++) for(int i = 0; i < scale; i++) gp_put(x + b * scale + i, cy + r * scale + j, c, 255);
    }
}
void gp_text(int cy, const char *s, int scale, uint32_t c){ gp_text_at((GW - gp_text_w(s, scale)) / 2, cy, s, scale, c); }
void gp_button(int cy, const char *label, int filled){
    int w = gp_text_w(label, 2) + 48, h = 40, x = (GW - w) / 2;
    if(filled){ gp_rect(x, cy, w, h, G_ACC, h / 2, 255); gp_text(cy + 13, label, 2, G_BG); }
    else { gp_rect(x, cy, w, h, G_DIM, h / 2, 255); gp_rect(x + 2, cy + 2, w - 4, h - 4, G_BG, h / 2 - 2, 255); gp_text(cy + 13, label, 2, G_TEXT); }
}
int gp_in_button(int x, int y, int cy){ return x >= 100 && x <= 260 && y >= cy - 6 && y <= cy + 46; }

/* ---- settings and score ---------------------------------------------------------------------------------------- */
static float luma(uint32_t c){ return 0.2126f * (c >> 16 & 255) + 0.7152f * (c >> 8 & 255) + 0.0722f * (c & 255); }
static void load_settings(const char *name){
    FILE *f = fopen("/tmp/diskos-app-env", "r");
    if(f){
        char line[96]; unsigned v;
        while(fgets(line, sizeof line, f)){
            if(sscanf(line, "canvas=%6x", &v) == 1) G_BG = v;
            else if(sscanf(line, "text=%6x", &v) == 1) G_TEXT = v;
            else if(sscanf(line, "text_muted=%6x", &v) == 1) G_DIM = v;
            else if(sscanf(line, "accent=%6x", &v) == 1) G_ACC = v;
        }
        fclose(f);
        if(fabsf(luma(G_ACC) - luma(G_BG)) < 40) G_ACC = G_TEXT;   /* an accent that vanishes on the canvas */
    }
    f = fopen("/usr/data/diskos.conf", "r");
    if(f){ char line[256]; int v; while(fgets(line, sizeof line, f)) if(sscanf(line, "screen_rot=%d", &v) == 1 && v >= 0 && v < 4) rotation = v; fclose(f); }
    snprintf(state_dir, sizeof state_dir, "/usr/data/%s", name);
    snprintf(state_path, sizeof state_path, "%s/best.conf", state_dir);
    f = fopen(state_path, "r");
    if(f){ char line[64]; int v; while(fgets(line, sizeof line, f)) if(sscanf(line, "best=%d", &v) == 1 && v >= 0 && v < 1000000) g_best = v; fclose(f); }
    rng ^= (uint32_t)(gp_now() * 1000.0);
}
void gp_save_best(void){
    if(!state_path[0]) return;
    mkdir(state_dir, 0755);
    char tmp[128]; snprintf(tmp, sizeof tmp, "%s.tmp", state_path);
    FILE *f = fopen(tmp, "w"); if(!f) return;
    int ok = fprintf(f, "best=%d\n", g_best) > 0;
    if(fflush(f) != 0 || fsync(fileno(f)) != 0) ok = 0;
    if(fclose(f) != 0) ok = 0;
    if(ok) rename(tmp, state_path); else unlink(tmp);
}

/* ---- touch ----------------------------------------------------------------------------------------------------- */
static void from_raw(int rx, int ry, int *x, int *y){        /* the same mapping as Boomshine */
    switch(rotation){ case 1: *x = GW - 1 - ry; *y = rx; break; case 2: *x = rx; *y = ry; break;
                      case 3: *x = ry; *y = GW - 1 - rx; break; default: *x = GW - 1 - rx; *y = GH - 1 - ry; break; }
}
typedef struct { int fd, mt, xmin, xmax, ymin, ymax, slot, rawx, rawy, down, was, x0, y0, x, y, moved, used, dropped; double t0; } touch_t;
static int scale_axis(int v, int lo, int hi){ if(hi <= lo){ lo = 0; hi = 359; } if(v < lo) v = lo; if(v > hi) v = hi; return (int)(((int64_t)v - lo) * 359 / ((int64_t)hi - lo)); }
static int open_touch(touch_t *t){
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
        return 0;
    }
    return -1;
}
static void read_touch(touch_t *t, const game_t *g){
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
        if(t->down && !t->was){
            t->x0 = t->x; t->y0 = t->y; t->moved = 0; t->used = 0; t->t0 = gp_now();
            if(g->down) g->down(t->x0, t->y0);
        }
        if(t->down && (abs(t->x - t->x0) > 14 || abs(t->y - t->y0) > 14)) t->moved = 1;
        if(!t->down && t->was && !t->used){ t->used = 1; if(g->up) g->up(t->x0, t->y0, t->moved); }
        t->was = t->down;
    }
}

/* ---- display --------------------------------------------------------------------------------------------------- */
typedef struct { int fd; unsigned char *map; struct fb_var_screeninfo v; struct fb_fix_screeninfo f; size_t offset; } display_t;
static int open_display(display_t *d){
    memset(d, 0, sizeof *d); d->fd = open("/dev/fb0", O_RDWR | O_CLOEXEC); if(d->fd < 0) return -1;
    if(ioctl(d->fd, FBIOGET_VSCREENINFO, &d->v) < 0 || ioctl(d->fd, FBIOGET_FSCREENINFO, &d->f) < 0) goto fail;
    if(d->v.xres != GW || d->v.yres != GH || d->v.bits_per_pixel != 32 || d->f.line_length < GW * 4 ||
       d->v.red.length != 8 || d->v.green.length != 8 || d->v.blue.length != 8){ errno = ENOTSUP; goto fail; }
    d->offset = (size_t)d->v.yoffset * d->f.line_length + (size_t)d->v.xoffset * 4;   /* the visible page */
    if(d->offset + (GH - 1) * (size_t)d->f.line_length + GW * 4 > d->f.smem_len){ errno = EINVAL; goto fail; }
    d->map = mmap(NULL, d->f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if(d->map == MAP_FAILED) goto fail;
    return 0;
fail: close(d->fd); d->fd = -1; return -1;
}
static void present(display_t *d){
    int first, stepx, rowstep;
    switch(rotation){ case 1: first = GW - 1; stepx = GW; rowstep = -1; break; case 2: first = 0; stepx = 1; rowstep = GW; break;
                      case 3: first = (GH - 1) * GW; stepx = -GW; rowstep = 1; break; default: first = GW * GH - 1; stepx = -1; rowstep = -GW; break; }
    int native = d->v.red.offset == 16 && d->v.green.offset == 8 && d->v.blue.offset == 0;
    for(int y = 0; y < GH; y++, first += rowstep){
        uint32_t *dst = (uint32_t *)(d->map + d->offset + (size_t)y * d->f.line_length);
        if(native && stepx == 1){ memcpy(dst, g_frame + first, GW * 4); continue; }
        for(int x = 0, i = first; x < GW; x++, i += stepx){
            uint32_t c = g_frame[i];
            dst[x] = native ? c : ((c >> 16 & 255) << d->v.red.offset) | ((c >> 8 & 255) << d->v.green.offset) | ((c & 255) << d->v.blue.offset);
        }
    }
}
static int write_ppm(const char *path){
    FILE *f = fopen(path, "wb"); if(!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", GW, GH);
    for(int i = 0; i < GW * GH; i++){ unsigned char p[3] = { g_frame[i] >> 16, g_frame[i] >> 8, g_frame[i] }; fwrite(p, 1, 3, f); }
    return fclose(f);
}
static void on_signal(int s){ (void)s; g_quit = 1; }

int gp_main(const game_t *g, int argc, char **argv, int (*render_preview)(const char *)){
    if(argc == 4 && !strcmp(argv[1], "--render")){
        load_settings(g->name); g_best = 42;
        if(render_preview(argv[2]) != 0) return 2;
        g->render();
        return write_ppm(argv[3]) ? 1 : 0;
    }
    load_settings(g->name);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGHUP, on_signal);
    display_t d; if(open_display(&d) < 0){ fprintf(stderr, "%s: display: %s\n", g->name, strerror(errno)); return 1; }
    touch_t t; memset(&t, 0, sizeof t);
    if(open_touch(&t) < 0){ fprintf(stderr, "%s: no touch panel\n", g->name); munmap(d.map, d.f.smem_len); close(d.fd); return 1; }
    double next = 0;
    while(!g_quit){
        double now = gp_now(), deadline = next > 0 ? next : now + 0.25;
        if(t.down && !t.used && !t.moved && t.t0 + HOLD_S < deadline) deadline = t.t0 + HOLD_S;
        if(g_dirty && deadline > now + 0.001) deadline = now;
        int ms = (int)ceil((deadline - now) * 1000); if(ms < 0) ms = 0;
        struct pollfd p = { .fd = t.fd, .events = POLLIN };
        int r = poll(&p, 1, ms);
        if(r < 0 && errno != EINTR) break;
        if(r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if(r > 0 && (p.revents & POLLIN)) read_touch(&t, g);
        now = gp_now();
        if(t.down && !t.used && !t.moved && now - t.t0 >= HOLD_S){ t.used = 1; if(g->hold) g->hold(); }
        if(next == 0 || now >= next) next = g->tick(now);
        if(g_dirty){ g->render(); present(&d); g_dirty = 0; }
    }
    close(t.fd); munmap(d.map, d.f.smem_len); close(d.fd);
    return 0;
}
