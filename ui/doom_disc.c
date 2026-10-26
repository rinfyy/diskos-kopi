/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* doom_disc.c - Doom on the Disc: the doomgeneric backend (vendor/doomgeneric, GPL-2.0-or-later), run as
 * `mq_ui --doom`, a child that owns the screen until it exits, like Snake.
 *
 *   - The game's 320 x 200 frame is shown at Doom's real 4:3 shape, 288 x 216, the largest that fits the round
 *     screen (bilinear), with Settings' screen rotation.
 *   - Touch, with several fingers at once: the left half is a joystick (up / down walk, left / right turn; push far
 *     to run), the right half fires (and is Enter / Yes in the menus), USE opens doors and flips switches, and the
 *     row on top has MAP, MENU and the next weapon. Each finger keeps the job of the place it landed.
 *   - The game: your own DOOM.WAD / DOOM2.WAD / DOOM1.WAD (or Plutonia, TNT) from the Doom folder on the SD card,
 *     else Freedoom. Saves (.savegame/) and settings in /usr/data/doom. No sound (the player keeps the audio).
 * Leave with Menu > Quit Game (or hold Vol-Up, as for every app). */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
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
/* Doom's own key codes, after <linux/input.h>: that header has KEY_ENTER (28), KEY_TAB (15) and others with the same
 * names but the scan-code values, and Doom wants ASCII (Enter 13, Tab 9) - the clash made menus ignore Enter. */
#undef KEY_ENTER
#undef KEY_TAB
#undef KEY_BACKSPACE
#undef KEY_PAUSE
#undef KEY_MINUS
#undef KEY_CAPSLOCK
#undef KEY_NUMLOCK
#undef KEY_HOME
#undef KEY_END
#undef KEY_F1
#undef KEY_F2
#undef KEY_F3
#undef KEY_F4
#undef KEY_F5
#undef KEY_F6
#undef KEY_F7
#undef KEY_F8
#undef KEY_F9
#undef KEY_F10
#undef KEY_F11
#undef KEY_F12
#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "games/platform.h"
_Static_assert(KEY_ENTER == 13 && KEY_TAB == 9, "Doom's key codes");

#define IMG_W 288
#define IMG_H 216
#define IMG_X ((GW - IMG_W) / 2)
#define IMG_Y 72
#define KEY_NEXTWPN 0x5d                                  /* ']' - bound to "next weapon" below */
extern int key_nextweapon;

/* ---- screen ------------------------------------------------------------------------------------------------------ */
static int rot, fb = -1; static unsigned char *fbmap; static struct fb_var_screeninfo vi; static struct fb_fix_screeninfo fi; static size_t fboff;
static int render_only;                                    /* host renders: write a picture instead of the screen */
static const char *render_path; static int selftest;
static int open_fb(void){
    fb = open("/dev/fb0", O_RDWR | O_CLOEXEC); if(fb < 0) return -1;
    if(ioctl(fb, FBIOGET_VSCREENINFO, &vi) < 0 || ioctl(fb, FBIOGET_FSCREENINFO, &fi) < 0 || vi.xres != GW || vi.yres != GH ||
       vi.bits_per_pixel != 32){ close(fb); fb = -1; return -1; }
    fboff = (size_t)vi.yoffset * fi.line_length + (size_t)vi.xoffset * 4;
    fbmap = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if(fbmap == MAP_FAILED){ close(fb); fb = -1; return -1; }
    return 0;
}
static void present(void){
    if(render_only || fb < 0) return;
    int first, stepx, rowstep;
    switch(rot){ case 1: first = GW - 1; stepx = GW; rowstep = -1; break; case 2: first = 0; stepx = 1; rowstep = GW; break;
                 case 3: first = (GH - 1) * GW; stepx = -GW; rowstep = 1; break; default: first = GW * GH - 1; stepx = -1; rowstep = -GW; break; }
    int native = vi.red.offset == 16 && vi.green.offset == 8 && vi.blue.offset == 0;
    for(int y = 0; y < GH; y++, first += rowstep){
        uint32_t *dst = (uint32_t *)(fbmap + fboff + (size_t)y * fi.line_length);
        for(int x = 0, i = first; x < GW; x++, i += stepx){
            uint32_t c = g_frame[i];
            dst[x] = native ? c : ((c >> 16 & 255) << vi.red.offset) | ((c >> 8 & 255) << vi.green.offset) | ((c & 255) << vi.blue.offset);
        }
    }
}
static void write_ppm(const char *path){
    FILE *f = fopen(path, "wb"); if(!f) return;
    fprintf(f, "P6\n%d %d\n255\n", GW, GH);
    for(int i = 0; i < GW * GH; i++){ unsigned char p[3] = { g_frame[i] >> 16, g_frame[i] >> 8, g_frame[i] }; fwrite(p, 1, 3, f); }
    fclose(f);
}

/* ---- keys -------------------------------------------------------------------------------------------------------- */
static struct { unsigned char key, pressed; } kq[64]; static int kq_r, kq_w;
static void key(unsigned char k, int down){ int n = (kq_w + 1) % 64; if(n == kq_r) return; kq[kq_w].key = k; kq[kq_w].pressed = (unsigned char)down; kq_w = n; }

/* ---- touch: several fingers, each with the job of where it landed ------------------------------------------------- */
enum { R_NONE, R_JOY, R_FIRE, R_USE, R_MENU, R_MAP, R_WPN, R_MTAP };   /* R_MTAP: a tap on Doom's menu */
int M_DiskTap(int x, int y, int *slider, int *already);
int M_DiskCanGoBack(void);
extern int messageToPrint;
#define SLOTS 6
typedef struct { int down, was, x, y, x0, y0, role, rx, ry, rx2; } finger_t;   /* rx2: the key MENU sent */
static finger_t fg[SLOTS];
static int tfd = -1, tmt, txmin, txmax, tymin, tymax, tslot;
static int joy_up, joy_dn, joy_l, joy_r, joy_run, fire_on, use_on;
static const struct { int role, x, y, r; const char *label; } BTN[] = {
    { R_MAP, 112, 44, 22, "MAP" }, { R_MENU, 180, 30, 22, "MENU" }, { R_WPN, 248, 44, 22, "WPN" },
    { R_USE, 318, 150, 26, "USE" },
};
static int axis(int v, int lo, int hi){ if(hi <= lo){ lo = 0; hi = 359; } if(v < lo) v = lo; if(v > hi) v = hi; return (int)(((int64_t)v - lo) * 359 / ((int64_t)hi - lo)); }
static void from_raw(int rx, int ry, int *x, int *y){                   /* the same mapping as the other games */
    switch(rot){ case 1: *x = GW - 1 - ry; *y = rx; break; case 2: *x = rx; *y = ry; break;
                 case 3: *x = ry; *y = GW - 1 - rx; break; default: *x = GW - 1 - rx; *y = GH - 1 - ry; break; }
}
static int open_touch(void){
    for(int i = 0; i < 32; i++){
        char path[40]; snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if(fd < 0) continue;
        struct input_absinfo ax, ay; int mt = 1;
        if(ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) < 0 || ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) < 0){
            mt = 0;
            if(ioctl(fd, EVIOCGABS(ABS_X), &ax) < 0 || ioctl(fd, EVIOCGABS(ABS_Y), &ay) < 0){ close(fd); continue; }
        }
        if(ax.maximum <= ax.minimum || ay.maximum <= ay.minimum){ close(fd); continue; }
        tfd = fd; tmt = mt; txmin = ax.minimum; txmax = ax.maximum; tymin = ay.minimum; tymax = ay.maximum;
        return 0;
    }
    return -1;
}
static int role_at(int x, int y){
    for(unsigned i = 0; i < sizeof BTN / sizeof BTN[0]; i++){
        int dx = x - BTN[i].x, dy = y - BTN[i].y, r = BTN[i].r + 10;
        if(dx * dx + dy * dy <= r * r) return BTN[i].role;
    }
    if(menuactive || messageToPrint) return R_MTAP;                         /* the menu is up: touch picks */
    if(y < 72) return R_NONE;
    return x < GW / 2 ? R_JOY : R_FIRE;
}
static void finger_down(finger_t *f){
    f->x0 = f->x; f->y0 = f->y; f->role = role_at(f->x, f->y);
    switch(f->role){
    case R_MENU: f->rx2 = M_DiskCanGoBack() ? KEY_BACKSPACE : KEY_ESCAPE; key((unsigned char)f->rx2, 1); break;   /* a page back, or close */
    case R_MAP:  key(KEY_TAB, 1); break;
    case R_WPN:  key(KEY_NEXTWPN, 1); break;
    case R_USE:  key(KEY_USE, 1); key(' ', 1); use_on = 1; break;
    case R_FIRE: key(KEY_FIRE, 1); key(KEY_ENTER, 1); key('y', 1); fire_on = 1; break;   /* Enter / Yes in the menus */
    default: break;
    }
}
static void tap_key(unsigned char k){ key(k, 1); key(k, 0); }
static void menu_tap(finger_t *f){                                        /* a tap (not a drag) on the menu */
    int dx = f->x - f->x0, dy = f->y - f->y0;
    if(dx * dx + dy * dy > 24 * 24){                                       /* a swipe: up / down moves the cursor */
        if(abs(dy) > abs(dx)) tap_key(dy < 0 ? KEY_UPARROW : KEY_DOWNARROW);
        else tap_key(dx < 0 ? KEY_LEFTARROW : KEY_RIGHTARROW);
        return;
    }
    if(messageToPrint){ tap_key('y'); return; }                            /* "are you sure?" - yes (MENU says no) */
    int mx = (f->x - IMG_X) * DOOMGENERIC_RESX / IMG_W, my = (f->y - IMG_Y) * DOOMGENERIC_RESY / IMG_H;
    int slider = 0, already = 0, item = M_DiskTap(mx, my, &slider, &already);
    if(item < 0) return;                                                   /* not on an item */
    if(slider) tap_key(mx < DOOMGENERIC_RESX / 2 ? KEY_LEFTARROW : KEY_RIGHTARROW);   /* left half less, right half more */
    else tap_key(KEY_ENTER);
}
static void finger_up(finger_t *f){
    switch(f->role){
    case R_MTAP: menu_tap(f); break;
    case R_MENU: key((unsigned char)f->rx2, 0); break;
    case R_MAP:  key(KEY_TAB, 0); break;
    case R_WPN:  key(KEY_NEXTWPN, 0); break;
    case R_USE:  key(KEY_USE, 0); key(' ', 0); use_on = 0; break;
    case R_FIRE: key(KEY_FIRE, 0); key(KEY_ENTER, 0); key('y', 0); fire_on = 0; break;
    default: break;
    }
    f->role = R_NONE;
}
static void joystick(void){                                                /* the joystick finger -> arrow keys */
    int up = 0, dn = 0, l = 0, r = 0, run = 0;
    for(int i = 0; i < SLOTS; i++) if(fg[i].down && fg[i].role == R_JOY){
        int dx = fg[i].x - fg[i].x0, dy = fg[i].y - fg[i].y0;
        up = dy < -16; dn = dy > 16; l = dx < -16; r = dx > 16; run = dx * dx + dy * dy > 60 * 60;
    }
    if(up != joy_up){ key(KEY_UPARROW, up); joy_up = up; }
    if(dn != joy_dn){ key(KEY_DOWNARROW, dn); joy_dn = dn; }
    if(l != joy_l){ key(KEY_LEFTARROW, l); joy_l = l; }
    if(r != joy_r){ key(KEY_RIGHTARROW, r); joy_r = r; }
    if(run != joy_run){ key(KEY_RSHIFT, run); joy_run = run; }
}
static void read_touch(void){
    if(tfd < 0) return;
    struct input_event ev[64]; ssize_t n;
    while((n = read(tfd, ev, sizeof ev)) > 0) for(size_t i = 0; i < (size_t)n / sizeof ev[0]; i++){
        struct input_event *e = &ev[i];
        if(e->type == EV_ABS && e->code == ABS_MT_SLOT){ tslot = e->value >= 0 && e->value < SLOTS ? e->value : -1; continue; }
        finger_t *f = tslot >= 0 ? &fg[tslot] : NULL;
        if(e->type == EV_ABS && f){
            if(e->code == (tmt ? ABS_MT_POSITION_X : ABS_X)) f->rx = e->value;
            else if(e->code == (tmt ? ABS_MT_POSITION_Y : ABS_Y)) f->ry = e->value;
            else if(e->code == ABS_MT_TRACKING_ID) f->down = e->value >= 0;
            continue;
        }
        if(e->type == EV_KEY && e->code == BTN_TOUCH && !tmt){ fg[0].down = e->value != 0; continue; }
        if(e->type != EV_SYN || e->code != SYN_REPORT) continue;
        for(int s = 0; s < SLOTS; s++){
            finger_t *g = &fg[s];
            from_raw(axis(g->rx, txmin, txmax), axis(g->ry, tymin, tymax), &g->x, &g->y);
            if(g->down && !g->was) finger_down(g);
            if(!g->down && g->was) finger_up(g);
            g->was = g->down;
        }
        joystick();
    }
}

/* ---- picture ------------------------------------------------------------------------------------------------------ */
static void draw_controls(void){
    uint32_t ink = 0xF2F2F2, red = 0xD71921;
    for(unsigned i = 0; i < sizeof BTN / sizeof BTN[0]; i++){
        int on = (BTN[i].role == R_USE && use_on);
        gp_disc(BTN[i].x, BTN[i].y, BTN[i].r, on ? red : 0x000000, on ? 200 : 120);
        gp_ring(BTN[i].x, BTN[i].y, BTN[i].r, 2, ink, 150);
        int w = gp_text_w(BTN[i].label, 1);
        gp_text_at(BTN[i].x - w / 2, BTN[i].y - 3, BTN[i].label, 1, ink);
    }
    static int touched; for(int i = 0; i < SLOTS; i++) if(fg[i].down) touched = 1;
    int hints = !touched || DG_GetTicksMs() < 8000;                         /* where MOVE and FIRE are: first seconds only */
    if(hints || fire_on){
        gp_ring(282, 262, 34, 2, fire_on ? red : ink, fire_on ? 230 : 110);  /* (the whole right half fires) */
        int w = gp_text_w("FIRE", 1); gp_text_at(282 - w / 2, 259, "FIRE", 1, fire_on ? red : ink);
    }
    for(int i = 0; i < SLOTS; i++) if(fg[i].down && fg[i].role == R_JOY){         /* the joystick under the finger */
        gp_ring(fg[i].x0, fg[i].y0, 44, 2, ink, 120);
        int dx = fg[i].x - fg[i].x0, dy = fg[i].y - fg[i].y0; float d = sqrtf((float)(dx * dx + dy * dy));
        if(d > 44){ dx = (int)(dx * 44 / d); dy = (int)(dy * 44 / d); }
        gp_disc(fg[i].x0 + dx, fg[i].y0 + dy, 16, ink, 150);
    }
    int stick = 0; for(int i = 0; i < SLOTS; i++) if(fg[i].down && fg[i].role == R_JOY) stick = 1;
    if(hints && !stick){                                                     /* a resting hint where the stick goes */
        gp_ring(84, 262, 34, 2, ink, 70);
        int w = gp_text_w("MOVE", 1); gp_text_at(84 - w / 2, 259, "MOVE", 1, ink);
    }
}
static void scale_frame(void){                                             /* 320 x 200 -> 288 x 216, bilinear */
    const uint32_t *src = DG_ScreenBuffer; const int SW = DOOMGENERIC_RESX, SH = DOOMGENERIC_RESY;
    for(int y = 0; y < IMG_H; y++){
        int fy = (int)(((int64_t)y * (SH - 1) << 8) / (IMG_H - 1)), iy = fy >> 8, wy = fy & 255; if(iy >= SH - 1){ iy = SH - 2; wy = 256; }
        uint32_t *o = g_frame + (IMG_Y + y) * GW + IMG_X;
        const uint32_t *r0 = src + iy * SW, *r1 = r0 + SW;
        for(int x = 0; x < IMG_W; x++){
            int fx = (int)(((int64_t)x * (SW - 1) << 8) / (IMG_W - 1)), ix = fx >> 8, wx = fx & 255; if(ix >= SW - 1){ ix = SW - 2; wx = 256; }
            uint32_t a = r0[ix], b = r0[ix + 1], c = r1[ix], d = r1[ix + 1];
            uint32_t rb = ((((a & 0xFF00FF) * (256 - wx) + (b & 0xFF00FF) * wx) >> 8 & 0xFF00FF) * (256 - wy) +
                           (((c & 0xFF00FF) * (256 - wx) + (d & 0xFF00FF) * wx) >> 8 & 0xFF00FF) * wy) >> 8 & 0xFF00FF;
            uint32_t g = ((((a & 0xFF00) * (256 - wx) + (b & 0xFF00) * wx) >> 8 & 0xFF00) * (256 - wy) +
                          (((c & 0xFF00) * (256 - wx) + (d & 0xFF00) * wx) >> 8 & 0xFF00) * wy) >> 8 & 0xFF00;
            o[x] = rb | g;
        }
    }
}
void DG_DrawFrame(void){
    read_touch();
    memset(g_frame, 0, sizeof g_frame);
    scale_frame(); draw_controls();
    present();
}
static double t_start;
void DG_Init(void){ t_start = gp_now(); }
void DG_SleepMs(uint32_t ms){ if(!render_only) usleep(ms * 1000); }
uint32_t DG_GetTicksMs(void){ return (uint32_t)((gp_now() - t_start) * 1000.0); }
int DG_GetKey(int *pressed, unsigned char *k){
    key_nextweapon = KEY_NEXTWPN;                                          /* (the config can't unbind it) */
    read_touch();
    if(kq_r == kq_w) return 0;
    *pressed = kq[kq_r].pressed; *k = kq[kq_r].key; kq_r = (kq_r + 1) % 64;
    return 1;
}
void DG_SetWindowTitle(const char *t){ (void)t; }

/* ---- messages (no WAD, or Doom stopped) --------------------------------------------------------------------------- */
static void message_screen(const char *l1, const char *l2, const char *l3){
    memset(g_frame, 0, sizeof g_frame);
    gp_text(100, "DOOM", 4, 0xD71921);
    gp_text(158, l1, 2, 0xF2F2F2);
    if(l2) gp_text(184, l2, 1, 0xB0B0B0);
    if(l3) gp_text(200, l3, 1, 0xB0B0B0);
    gp_text(262, "TAP TO LEAVE", 1, 0x808080);
    present();
    if(render_only){ if(render_path) write_ppm(render_path); return; }
    if(tfd < 0) { sleep(4); return; }
    double t0 = gp_now();
    for(;;){                                                                /* wait for a fresh tap */
        struct pollfd p = { .fd = tfd, .events = POLLIN };
        if(poll(&p, 1, 500) < 0 && errno != EINTR) break;
        int was = fg[0].down; read_touch();
        if(was && !fg[0].down && gp_now() - t0 > 0.4) break;
        if(gp_now() - t0 > 120) break;
    }
}
void DG_Error(const char *msg){
    char a[64]; snprintf(a, sizeof a, "%.56s", msg ? msg : "");
    for(char *c = a; *c; c++){ *c = (char)toupper((unsigned char)*c); if(*c == '\n') *c = ' '; }
    message_screen("DOOM STOPPED", a, "SEE /TMP/DOOM.LOG");
}

/* ---- the game file ------------------------------------------------------------------------------------------------- */
static int find_wad(char *out, size_t n){
    static const char *const ORDER[] = { "doom.wad", "doom2.wad", "plutonia.wad", "tnt.wad", "doom1.wad", "freedoom1.wad", "freedoom2.wad" };
    const char *base = getenv("DISKOS_DOOM_DIR");
    char dirs[5][96];
    if(base && *base){ snprintf(dirs[0], sizeof dirs[0], "%s", base); dirs[1][0] = 0; }
    else {
        snprintf(dirs[0], 96, "/tmp/sdcard/Doom"); snprintf(dirs[1], 96, "/tmp/sdcard/DOOM"); snprintf(dirs[2], 96, "/tmp/sdcard/doom");
        snprintf(dirs[3], 96, "/tmp/sdcard"); dirs[4][0] = 0;
    }
    for(unsigned o = 0; o < sizeof ORDER / sizeof ORDER[0]; o++)
        for(int d = 0; d < 5 && dirs[d][0]; d++){
            DIR *dp = opendir(dirs[d]); if(!dp) continue;
            struct dirent *de;
            while((de = readdir(dp))){
                if(strcasecmp(de->d_name, ORDER[o])) continue;
                snprintf(out, n, "%s/%s", dirs[d], de->d_name); closedir(dp); return 1;
            }
            closedir(dp);
        }
    return 0;
}
static void on_signal(int s){ (void)s; _exit(0); }

int doom_main(int argc, char **argv){
    if(argc >= 3 && !strcmp(argv[1], "--render")){ render_only = 1; render_path = argv[2]; }   /* host renders */
    if(argc >= 3 && !strcmp(argv[1], "--selftest")){ render_only = selftest = 1; render_path = argv[2]; }
    FILE *f = fopen("/usr/data/diskos.conf", "r");
    if(f){ char line[256]; int v; while(fgets(line, sizeof line, f)) if(sscanf(line, "screen_rot=%d", &v) == 1 && v >= 0 && v < 4) rot = v; fclose(f); }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGHUP, on_signal);
    if(!render_only){
        if(open_fb() < 0){ fprintf(stderr, "doom: no display: %s\n", strerror(errno)); return 1; }
        if(open_touch() < 0) fprintf(stderr, "doom: no touch panel\n");
    }
    char wad[256];
    if(!find_wad(wad, sizeof wad)){
        message_screen("NO GAME FILE", "COPY DOOM1.WAD OR FREEDOOM1.WAD", "TO THE DOOM FOLDER ON THE SD CARD");
        return 0;
    }
    const char *home = getenv("DISKOS_DOOM_HOME"); if(!home || !*home) home = "/usr/data/doom";
    mkdir(home, 0755);
    if(chdir(home) != 0) fprintf(stderr, "doom: can't use %s for saves\n", home);
    printf("doom: %s\n", wad); fflush(stdout);
    char *dargv[] = { "doom", "-iwad", wad, "-nogui", NULL, NULL, NULL, NULL };
    int dargc = 4;
    if(render_only){ dargv[dargc++] = "-warp"; dargv[dargc++] = "1"; dargv[dargc++] = "1"; }   /* a picture of E1M1 */
    doomgeneric_Create(dargc, dargv);
    if(render_only && selftest){                                           /* touch -> keys -> the game, end to end */
        for(int i = 0; i < 40; i++) doomgeneric_Tick();
        int x0 = players[0].mo->x, y0 = players[0].mo->y, a0 = players[0].mo->angle;
        finger_t *f = &fg[2]; f->rx = f->ry = 0;
        f->down = 1; f->x = f->x0 = 90; f->y = f->y0 = 250; finger_down(f); f->was = 1;   /* the stick, pushed up */
        f->y = 200; joystick();
        for(int i = 0; i < 40; i++) doomgeneric_Tick();
        int walked = players[0].mo->x != x0 || players[0].mo->y != y0;
        f->y = 250; f->x = 140; joystick();                                 /* now right: turn */
        for(int i = 0; i < 20; i++) doomgeneric_Tick();
        int turned = players[0].mo->angle != (unsigned)a0;
        f->down = 0; finger_up(f); f->was = 0; joystick();
        f->down = 1; f->x = f->x0 = 180; f->y = f->y0 = 30; finger_down(f);   /* MENU */
        for(int i = 0; i < 4; i++) doomgeneric_Tick();
        int menu = menuactive; f->down = 0; finger_up(f);
        for(int i = 0; i < 4; i++) doomgeneric_Tick();
        f->down = 1; f->x = f->x0 = 180; f->y = f->y0 = 30; finger_down(f); for(int i = 0; i < 4; i++) doomgeneric_Tick();
        f->down = 0; finger_up(f); for(int i = 0; i < 4; i++) doomgeneric_Tick();
        int closed = !menuactive;
        /* the menu by touch: open it, tap Options (a new page), Back, tap Quit Game (the question), MENU says no */
        extern void *currentMenu;
        #define TAP(px, py) do { f->down = 1; f->x = f->x0 = (px); f->y = f->y0 = (py); finger_down(f); for(int k_ = 0; k_ < 3; k_++) doomgeneric_Tick(); \
                                 f->down = 0; finger_up(f); for(int k_ = 0; k_ < 6; k_++) doomgeneric_Tick(); } while(0)
        #define DOOM_Y(dy) (IMG_Y + (dy) * IMG_H / DOOMGENERIC_RESY)
        TAP(180, 30); void *main_menu = currentMenu;
        TAP(170, DOOM_Y(64 + 16 + 6)); int options = menuactive && currentMenu != main_menu;
        TAP(180, 30); int back = menuactive && currentMenu == main_menu;
        TAP(170, DOOM_Y(64 + 5 * 16 + 6)); int asked = messageToPrint != 0;
        TAP(180, 30); int kept = !messageToPrint;
        printf("%s doom menu by touch: options %d, back %d, quit asks %d, no keeps playing %d\n",
               options && back && asked && kept ? "PASS" : "FAIL", options, back, asked, kept);
        closed = closed && options && back && asked && kept;
        /* a new game by touch: New Game > the first episode > Hurt Me Plenty; then save it in the first slot */
        TAP(180, 30); TAP(170, DOOM_Y(64 + 6));                             /* menu, New Game */
        TAP(170, DOOM_Y(63 + 6));                                           /* episode 1 */
        TAP(170, DOOM_Y(63 + 2 * 16 + 6));                                  /* Hurt Me Plenty */
        for(int i = 0; i < 20; i++) doomgeneric_Tick();
        int started = !menuactive && gamestate == GS_LEVEL;
        TAP(180, 30); TAP(170, DOOM_Y(64 + 3 * 16 + 6));                    /* menu, Save Game */
        TAP(170, DOOM_Y(54 + 6));                                           /* slot 1: named "SAVE 1", Enter saves */
        TAP(170, DOOM_Y(54 + 6));
        for(int i = 0; i < 10; i++) doomgeneric_Tick();
        struct stat sv; int saved = stat(".savegame/doomsav0.dsg", &sv) == 0;
        printf("%s doom new game and save by touch: started %d, saved %d\n", started && saved ? "PASS" : "FAIL", started, saved);
        closed = closed && started && saved;
        printf("%s doom touch: walked %d, turned %d, menu opened %d, closed %d\n", walked && turned && menu && closed ? "PASS" : "FAIL", walked, turned, menu, closed);
        fflush(stdout);
        write_ppm(render_path); return walked && turned && menu && closed ? 0 : 1;
    }
    if(render_only){
        for(int i = 0; i < 60; i++) doomgeneric_Tick();
        fg[1].down = 1; fg[1].role = R_JOY; fg[1].x0 = 90; fg[1].y0 = 250; fg[1].x = 90; fg[1].y = 214;   /* a finger on the stick */
        fire_on = 1; DG_DrawFrame(); write_ppm(render_path); return 0;
    }
    for(;;) doomgeneric_Tick();
}
