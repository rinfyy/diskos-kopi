/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* video_disc.c - Videos: plays an MPEG-1 file (.mpg / .mpeg, MPEG-1 video + MP2 sound) full screen, run as
 * `mq_ui --video <file>`, a child that owns the screen until it exits, like the games.
 *
 * Decoding is pl_mpeg (vendor/pl_mpeg, MIT): MPEG-1 is light enough for the player's CPU in software, which phone
 * formats (H.264 / H.265) are not - Files says how to convert. The picture keeps its shape inside the round screen
 * (Fit) or fills the circle (Fill). Sound: mq_player owns the audio hardware, so - like MA Sendspin - the sound goes to
 * the Disc's own AirPlay receiver (ss_airplay.h); mq_ui switches the Disc to AirPlay for the video and back after.
 * The receiver holds about two seconds of sound, so the picture runs that far behind what's sent and stays in step.
 * Without the receiver the video plays silently.
 *
 * Touch: a tap shows the controls for 4 s - play / pause in the middle, back / forward 10 s either side, Fit / Fill,
 * close at the top - and the rim's dots are the progress (tap the rim to jump there). Vol-Up held also closes. */
#define _GNU_SOURCE
#include "ss_airplay.h"
#define PL_MPEG_IMPLEMENTATION
#include "vendor/pl_mpeg/pl_mpeg.h"
#include "games/platform.h"
#include <ctype.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define LAT_S   2.0                                    /* the AirPlay receiver's hold-back (sendspin.c's default) */
#define CTRL_S  4.0

static int rot, fb = -1; static unsigned char *fbmap; static struct fb_var_screeninfo vi; static struct fb_fix_screeninfo fi; static size_t fboff;
static volatile int quit;
static void on_sig(int s){ (void)s; quit = 1; }

/* ---- screen ------------------------------------------------------------------------------------------------------ */
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
    if(fb < 0) return;
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

/* ---- touch (one finger) ------------------------------------------------------------------------------------------ */
static int tfd = -1, tmt, txmin, txmax, tymin, tymax, tslot, t_down, t_was, t_rx, t_ry, t_x, t_y, t_x0, t_y0;
static int axis(int v, int lo, int hi){ if(hi <= lo){ lo = 0; hi = 359; } if(v < lo) v = lo; if(v > hi) v = hi; return (int)(((int64_t)v - lo) * 359 / ((int64_t)hi - lo)); }
static void from_raw(int rx, int ry, int *x, int *y){
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
static void tap(int x, int y);
static void read_touch(void){
    if(tfd < 0) return;
    struct input_event ev[64]; ssize_t n;
    while((n = read(tfd, ev, sizeof ev)) > 0) for(size_t i = 0; i < (size_t)n / sizeof ev[0]; i++){
        struct input_event *e = &ev[i];
        if(e->type == EV_ABS && e->code == ABS_MT_SLOT){ tslot = e->value; continue; }
        if(e->type == EV_ABS && tslot == 0){
            if(e->code == (tmt ? ABS_MT_POSITION_X : ABS_X)) t_rx = e->value;
            else if(e->code == (tmt ? ABS_MT_POSITION_Y : ABS_Y)) t_ry = e->value;
            else if(e->code == ABS_MT_TRACKING_ID) t_down = e->value >= 0;
            continue;
        }
        if(e->type == EV_KEY && e->code == BTN_TOUCH && !tmt){ t_down = e->value != 0; continue; }
        if(e->type != EV_SYN || e->code != SYN_REPORT) continue;
        from_raw(axis(t_rx, txmin, txmax), axis(t_ry, tymin, tymax), &t_x, &t_y);
        if(t_down && !t_was){ t_x0 = t_x; t_y0 = t_y; }
        if(!t_down && t_was && abs(t_x - t_x0) < 24 && abs(t_y - t_y0) < 24) tap(t_x0, t_y0);
        t_was = t_down;
    }
}

/* ---- playback ---------------------------------------------------------------------------------------------------- */
static plm_t *plm;
static double dur, t0, pause_pos, ctrl_until, lat;
static int paused, ended, fill, fw, fh, have_pic, dirty = 1;
static uint32_t *pic;                                  /* the last frame shown, XRGB */
static plm_frame_t *next_frame;
static plm_samples_t *next_samples;
static airplay_t ap; static int ap_ok;
#define FIFO (44100 * 4)
static int16_t fifo[FIFO * 2]; static int f_r, f_w; static double f_time; static int f_started;   /* f_time: media time of f_r */
static double rs_phase; static float rs_prev[2]; static int sr = 44100;

static double media_now(void){ return paused ? pause_pos : gp_now() - t0; }
static int fifo_len(void){ return (f_w - f_r + FIFO) % FIFO; }
static void fifo_put(float l, float r){
    if(fifo_len() >= FIFO - 1) return;
    int L = (int)lrintf(l * 32767.0f), R = (int)lrintf(r * 32767.0f);
    if(L > 32767) L = 32767;
    if(L < -32768) L = -32768;
    if(R > 32767) R = 32767;
    if(R < -32768) R = -32768;
    fifo[2 * f_w] = (int16_t)L; fifo[2 * f_w + 1] = (int16_t)R; f_w = (f_w + 1) % FIFO;
}
static void push_samples(plm_samples_t *s){                    /* resampled to 44.1 kHz (linear) */
    if(!f_started){ f_time = s->time; f_started = 1; rs_phase = 0; rs_prev[0] = s->interleaved[0]; rs_prev[1] = s->interleaved[1]; }
    if(sr == 44100){ for(unsigned i = 0; i < s->count; i++) fifo_put(s->interleaved[2 * i], s->interleaved[2 * i + 1]); return; }
    double step = (double)sr / 44100.0;
    /* rs_phase: position between rs_prev (0) and this block's first sample (1), counted in source samples */
    for(;;){
        double p = rs_phase; int i = (int)floor(p); double fr = p - i;
        if(i >= (int)s->count) break;
        float a0, a1, b0, b1;
        if(i == 0){ a0 = rs_prev[0]; a1 = rs_prev[1]; } else { a0 = s->interleaved[2 * (i - 1)]; a1 = s->interleaved[2 * (i - 1) + 1]; }
        b0 = s->interleaved[2 * i]; b1 = s->interleaved[2 * i + 1];
        fifo_put(a0 + (b0 - a0) * (float)fr, a1 + (b1 - a1) * (float)fr);
        rs_phase += step;
    }
    rs_phase -= s->count; rs_prev[0] = s->interleaved[2 * (s->count - 1)]; rs_prev[1] = s->interleaved[2 * (s->count - 1) + 1];
}
static void restart_at(double t){                              /* after a seek, a resume or the start */
    if(t < 0) t = 0;
    if(dur > 0 && t > dur - 0.5) t = dur > 0.5 ? dur - 0.5 : 0;
    if(t > 0.05) plm_seek(plm, t, 0); else plm_rewind(plm);
    next_frame = NULL; next_samples = NULL;
    f_r = f_w = 0; f_started = 0;
    if(ap_ok) ap_flush(&ap);
    t0 = gp_now() + lat - t; ended = 0; dirty = 1;
}
static void set_paused(int p){
    if(p == paused) return;
    if(p){ pause_pos = media_now(); paused = 1; if(ap_ok) ap_flush(&ap); f_r = f_w = 0; f_started = 0; }
    else { paused = 0; restart_at(pause_pos); }          /* resume = start again from here (the receiver dropped what it held) */
    dirty = 1;
}
static void decode_video(double m){
    plm_frame_t *show = NULL;
    for(;;){
        if(!next_frame){ next_frame = plm_decode_video(plm); if(!next_frame){ if(plm_has_ended(plm)) ended = 1; break; } }
        if(next_frame->time > m) break;
        show = next_frame; next_frame = NULL;
        if(!show) break;
        /* convert only the newest frame that's due (a late one is dropped, not shown late) */
        plm_frame_t *peek = plm_decode_video(plm);
        if(peek && peek->time <= m){ next_frame = peek; continue; }
        if(!pic || (int)show->width != fw || (int)show->height != fh){
            free(pic); fw = (int)show->width; fh = (int)show->height; pic = malloc((size_t)fw * fh * 4);
        }
        if(pic){ plm_frame_to_bgra(show, (uint8_t *)pic, fw * 4); have_pic = 1; dirty = 1; }
        next_frame = peek;
        break;
    }
}
static void feed_audio(double m){
    if(!ap_ok) return;
    while(!next_samples || next_samples->time <= m + lat + 0.4){      /* keep ~0.4 s decoded ahead of what's sent */
        if(next_samples){ push_samples(next_samples); next_samples = NULL; }
        next_samples = plm_decode_audio(plm);
        if(!next_samples) break;
        if(next_samples->time < m - 0.2){ next_samples = NULL; continue; }   /* behind (after a seek): skip */
    }
    double now = gp_now();
    while(fifo_len() >= AP_FRAMES){                                     /* each packet goes when its time minus the hold-back comes */
        double play_at = t0 + f_time;
        if(play_at - lat > now + 0.05) break;
        int16_t pkt[AP_FRAMES * 2];
        for(int i = 0; i < AP_FRAMES; i++){ pkt[2 * i] = fifo[2 * f_r]; pkt[2 * i + 1] = fifo[2 * f_r + 1]; f_r = (f_r + 1) % FIFO; }
        if(play_at > now - 0.2) ap_send(&ap, pkt);                      /* too late to hear: skip it */
        f_time += (double)AP_FRAMES / 44100.0;
    }
}

/* ---- picture and controls ---------------------------------------------------------------------------------------- */
static void blit(void){
    memset(g_frame, 0, sizeof g_frame);
    if(!have_pic || !pic) return;
    double a = (double)fw / fh; int W, H;
    if(fill){ if(a >= 1){ H = GH; W = (int)(GH * a); } else { W = GW; H = (int)(GW / a); } }   /* covers the circle */
    else { H = (int)(360.0 / sqrt(1 + a * a)); W = (int)(H * a); }      /* the biggest that fits inside it */
    int X = (GW - W) / 2, Y = (GH - H) / 2;
    static int16_t tx[GW], tw[GW]; static int k_fw, k_W, k_X;            /* each column's source, until the shape changes */
    if(k_fw != fw || k_W != W || k_X != X){
        for(int x = 0; x < GW; x++){
            int fx = (int)((int64_t)(x - X) * (fw - 1) * 256 / (W > 1 ? W - 1 : 1)), ix = fx >> 8, wx = fx & 255;
            if(ix < 0){ ix = 0; wx = 0; }
            if(ix >= fw - 1){ ix = fw > 1 ? fw - 2 : 0; wx = fw > 1 ? 256 : 0; }
            tx[x] = (int16_t)ix; tw[x] = (int16_t)wx;
        }
        k_fw = fw; k_W = W; k_X = X;
    }
    for(int y = 0; y < GH; y++){
        int sy = (y - Y); if(sy < 0 || sy >= H) continue;
        int fy = (int)((int64_t)sy * (fh - 1) * 256 / (H > 1 ? H - 1 : 1)), iy = fy >> 8, wy = fy & 255; if(iy >= fh - 1){ iy = fh - 2 < 0 ? 0 : fh - 2; wy = fh > 1 ? 256 : 0; }
        const uint32_t *r0 = pic + (size_t)iy * fw, *r1 = fh > 1 ? r0 + fw : r0;
        uint32_t *o = g_frame + y * GW;
        int xa = X < 0 ? 0 : X, xb = X + W > GW ? GW : X + W;
        for(int x = xa; x < xb; x++){
            int ix = tx[x], wx = tw[x];
            uint32_t p = r0[ix], q = r0[ix + (fw > 1)], u = r1[ix], v = r1[ix + (fw > 1)];
            uint32_t rb = ((((p & 0xFF00FF) * (256 - wx) + (q & 0xFF00FF) * wx) >> 8 & 0xFF00FF) * (256 - wy) +
                           (((u & 0xFF00FF) * (256 - wx) + (v & 0xFF00FF) * wx) >> 8 & 0xFF00FF) * wy) >> 8 & 0xFF00FF;
            uint32_t g = ((((p & 0xFF00) * (256 - wx) + (q & 0xFF00) * wx) >> 8 & 0xFF00) * (256 - wy) +
                          (((u & 0xFF00) * (256 - wx) + (v & 0xFF00) * wx) >> 8 & 0xFF00) * wy) >> 8 & 0xFF00;
            o[x] = rb | g;
        }
    }
}
/* the theme's colours (mq_ui writes them to /tmp/diskos-app-env before a launch); NTDisk's look if there are none */
static uint32_t C_TEXT = 0xF2F2F2, C_DIM = 0x8C8C8C, C_ACC = 0xD71921, C_ON = 0xFFFFFF;
static void load_colours(void){
    FILE *f = fopen("/tmp/diskos-app-env", "r"); if(!f) return;
    char line[96]; unsigned v; uint32_t canvas = 0, surf = 0; int have_c = 0;
    while(fgets(line, sizeof line, f)){
        if(sscanf(line, "text=%6x", &v) == 1) C_TEXT = v;
        else if(sscanf(line, "text_muted=%6x", &v) == 1) C_DIM = v;
        else if(sscanf(line, "accent=%6x", &v) == 1) C_ACC = v;
        else if(sscanf(line, "on_accent=%6x", &v) == 1) C_ON = v;
        else if(sscanf(line, "canvas=%6x", &v) == 1){ canvas = v; have_c = 1; }
        else if(sscanf(line, "surface=%6x", &v) == 1) surf = v;
    }
    fclose(f); (void)surf;
    /* the video is always on black: a light theme's dark text would vanish, so take the light side */
    if(have_c && ((canvas >> 16 & 255) + (canvas >> 8 & 255) + (canvas & 255)) > 3 * 128){ C_TEXT = 0xF2F2F2; C_DIM = 0x9A9A9A; }
}
static void shade(int cx, int cy, int rx, int ry, int amax){          /* darkens an ellipse, fading out to its edge */
    for(int y = cy - ry; y <= cy + ry; y++) for(int x = cx - rx; x <= cx + rx; x++){
        float dx = (float)(x - cx) / rx, dy = (float)(y - cy) / ry, d = dx * dx + dy * dy;
        if(d < 1) gp_put(x, y, 0x000000, (int)(amax * (1 - d) * (1 - d)));
    }
}
static void tri(int cx, int cy, int s, uint32_t c){                    /* a play triangle pointing right */
    for(int y = -s; y <= s; y++){ int w = (s - abs(y)) * 2 * 87 / 100; for(int x = 0; x <= w; x++) gp_put(cx - s * 2 / 3 + x, cy + y, c, 255); }
}
static void fmt(double s, char *o, size_t n){ int t = s > 0 ? (int)s : 0; snprintf(o, n, "%d:%02d", t / 60, t % 60); }
static void line(float x0, float y0, float x1, float y1, uint32_t c){  /* a thin anti-aliased line, by small discs */
    int n = (int)(fabsf(x1 - x0) + fabsf(y1 - y0)) + 1;
    for(int i = 0; i <= n; i++){ float t = (float)i / n; gp_disc(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, 0.9f, c, 255); }
}
/* Quiet controls in the theme's colours: a fine ring with play / pause, -10 and +10 as plain text either side, a thin
 * close mark at the top, the time below, FIT / FILL under it, and the rim's dots as the progress (the head in the
 * accent). A soft shade only where the controls are, not over the whole picture. */
static void draw_controls(void){
    double m = media_now(); if(m < 0) m = 0;
    if(dur > 0 && m > dur) m = dur;
    int lit = dur > 0 ? (int)(72 * m / dur) : 0;
    for(int i = 0; i < 72; i++){
        double an = (i * 5.0 - 90) * M_PI / 180;
        gp_disc((float)(180 + 170 * cos(an)), (float)(180 + 170 * sin(an)), i == lit ? 3.0f : 1.8f, i == lit ? C_ACC : i < lit ? C_TEXT : C_DIM, i < lit || i == lit ? 255 : 150);
    }
    shade(180, 190, 120, 70, 150);                                     /* a soft shade behind the controls */
    int still = paused || ended;
    gp_disc(180, 180, 30, still ? C_ACC : 0x000000, still ? 255 : 140);
    if(!still) gp_ring(180, 180, 30, 1.5f, C_TEXT, 230);
    if(still) tri(184, 180, 12, C_ON);
    else { gp_rect(170, 168, 6, 24, C_TEXT, 2, 255); gp_rect(184, 168, 6, 24, C_TEXT, 2, 255); }
    gp_text_at(98 - gp_text_w("-10", 2) / 2, 173, "-10", 2, C_TEXT);
    gp_text_at(262 - gp_text_w("+10", 2) / 2, 173, "+10", 2, C_TEXT);
    line(172, 54, 188, 70, C_TEXT); line(188, 54, 172, 70, C_TEXT);     /* close */
    char a[16], b[16]; fmt(m, a, sizeof a); fmt(dur, b, sizeof b);
    int wa = gp_text_w(a, 2), wb = gp_text_w(b, 1), x = 180 - (wa + 8 + wb) / 2;
    gp_text_at(x, 248, a, 2, C_TEXT); gp_text_at(x + wa + 8, 255, b, 1, C_DIM);
    gp_text(274, fill ? "FIT" : "FILL", 1, C_DIM);
    if(!ap_ok) gp_text(300, "NO SOUND", 1, C_DIM);
}
static void render(void){
    blit();
    if(ended || paused || gp_now() < ctrl_until) draw_controls();
    present();
}
static void tap(int x, int y){
    int dx = x - 180, dy = y - 180, r2 = dx * dx + dy * dy;
    int showing = ended || paused || gp_now() < ctrl_until;
    dirty = 1;
    if(r2 > 150 * 150 && dur > 0 && showing){                          /* the rim: jump there */
        double an = atan2(dy, dx) * 180 / M_PI + 90; if(an < 0) an += 360;
        double t = dur * an / 360; if(paused){ pause_pos = t; } else restart_at(t);
        ctrl_until = gp_now() + CTRL_S; return;
    }
    if(!showing){ ctrl_until = gp_now() + CTRL_S; return; }            /* the first tap just shows them */
    ctrl_until = gp_now() + CTRL_S;
    if(abs(x - 180) < 30 && abs(y - 62) < 30){ quit = 1; return; }
    if(r2 < 44 * 44){ if(ended){ paused = 0; restart_at(0); } else set_paused(!paused); return; }
    if(abs(x - 98) < 38 && abs(y - 180) < 38){ double t = media_now() - 10; if(paused) pause_pos = t < 0 ? 0 : t; else restart_at(t); return; }
    if(abs(x - 262) < 38 && abs(y - 180) < 38){ double t = media_now() + 10; if(paused) pause_pos = dur > 0 && t > dur ? dur : t; else restart_at(t); return; }
    if(abs(x - 180) < 44 && y > 264 && y < 296){ fill = !fill; return; }
    ctrl_until = 0;                                                    /* anywhere else: hide them */
}
static void message(const char *l1, const char *l2, const char *l3){
    memset(g_frame, 0, sizeof g_frame);
    gp_text(110, "VIDEOS", 3, C_ACC); gp_text(160, l1, 2, C_TEXT);
    if(l2) gp_text(190, l2, 1, C_DIM);
    if(l3) gp_text(206, l3, 1, C_DIM);
    gp_text(262, "TAP TO LEAVE", 1, C_DIM);
    present();
}

int video_main(int argc, char **argv){
    /* argv: [--selftest out.ppm] file */
    int selftest = argc >= 4 && !strcmp(argv[1], "--selftest");
    const char *out = selftest ? argv[2] : NULL, *path = argc >= 2 ? argv[argc - 1] : NULL;
    FILE *f = fopen("/usr/data/diskos.conf", "r");
    if(f){ char line[256]; int v; while(fgets(line, sizeof line, f)) if(sscanf(line, "screen_rot=%d", &v) == 1 && v >= 0 && v < 4) rot = v; fclose(f); }
    load_colours();
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGHUP, on_sig); signal(SIGPIPE, SIG_IGN);
    if(!selftest){ if(open_fb() < 0){ fprintf(stderr, "video: no display\n"); return 1; } open_touch(); }
    plm = path ? plm_create_with_filename(path) : NULL;
    if(plm && !plm_probe(plm, 5000 * 1024)){ plm_destroy(plm); plm = NULL; }
    if(!plm || !plm_get_num_video_streams(plm)){
        fprintf(stderr, "video: can't play %s\n", path ? path : "(none)");
        if(selftest){ printf("FAIL video: can't open %s\n", path ? path : ""); return 1; }
        message("CAN'T PLAY THIS FILE", "VIDEOS PLAYS MPEG-1 .MPG FILES", "CONVERT OTHERS FIRST - SEE THE README");
        double t = gp_now(); int was = 0;
        while(!quit && gp_now() - t < 60){ usleep(30000); int w = t_down; read_touch(); if(was && !t_down) break; was = w || t_down; }
        return 0;
    }
    dur = plm_get_duration(plm);
    sr = plm_get_samplerate(plm); if(sr <= 0) sr = 44100;
    int audio = plm_get_num_audio_streams(plm) > 0 && !selftest;
    plm_set_audio_enabled(plm, audio);
    plm_set_loop(plm, 0);
    if(audio){                                                          /* the receiver comes up just after the mode switch */
        message("STARTING SOUND", NULL, NULL);
        ap.port = 5000; ap.rtsp = ap.aud = ap.ctl = ap.tim = -1; ap.vol_db = 0;
        for(int i = 0; i < 16 && !quit; i++){ if(ap_open(&ap) == 0){ ap_ok = 1; break; } usleep(500000); }
        if(!ap_ok) fprintf(stderr, "video: no AirPlay receiver - playing without sound\n");
    }
    lat = ap_ok ? LAT_S : 0.15;
    printf("video: %s, %dx%d, %.1f s, %d Hz, sound %s\n", path, plm_get_width(plm), plm_get_height(plm), dur, sr, ap_ok ? "on" : "off"); fflush(stdout);
    restart_at(0); ctrl_until = gp_now() + 2.5;
    if(selftest){                                                       /* decode 3 s of picture as fast as it goes, seek, render */
        int frames = 0; double t_end = 3.0;
        t0 = 0; paused = 0;
        for(double m = 0; m < t_end; m += 1.0 / 30){ decode_video(m); if(have_pic) frames++; }
        int jumped = 0; restart_at(dur > 8 ? 6 : dur / 2); t0 = 0;
        double want = dur > 8 ? 6 : dur / 2; next_frame = plm_decode_video(plm);
        if(next_frame && fabs(next_frame->time - want) < 2.5) jumped = 1;
        decode_video(want); paused = 1; pause_pos = want;
        render(); write_ppm(out);
        int ok = frames > 30 && have_pic && jumped;
        printf("%s video: %dx%d, %.1f s, %d frames decoded in 3 s of film, seek to %.0f s %s\n", ok ? "PASS" : "FAIL",
               fw, fh, dur, frames, want, jumped ? "ok" : "missed");
        return ok ? 0 : 1;
    }
    double last_draw = 0;
    while(!quit){
        read_touch();
        double m = media_now();
        if(!paused && !ended){ if(m >= 0) decode_video(m); feed_audio(m); }
        if(ended && !paused){ paused = 1; pause_pos = dur; dirty = 1; }
        double now = gp_now();
        int ctl = ended || paused || now < ctrl_until;
        if(dirty || (ctl && now - last_draw > 0.25)){ render(); dirty = 0; last_draw = now; }
        struct pollfd p = { .fd = tfd, .events = POLLIN };
        poll(&p, tfd >= 0 ? 1 : 0, paused ? 40 : 4);
    }
    if(ap_ok) ap_close(&ap);
    plm_destroy(plm); free(pic);
    return 0;
}
