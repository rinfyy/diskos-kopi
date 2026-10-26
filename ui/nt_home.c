/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* nt_home.c - NTDisk's own Music screen and standby screen (the rest of NTDisk is Disco's machinery, restyled).
 *
 * Music, "CD + info" (cfg nt_music 0): the album as a CD rising from the top edge, a small dot "audio form" that
 * moves while music plays, then the artist, the title - or, when the song has synced lyrics and nt_lyrics is on, the
 * current lyric line in its place - a short progress line and "elapsed / total". "Disc only" (nt_music 1): just the
 * CD with the album as its label and a thin red progress ring round it.
 * Touch: tap the CD for play/pause, swipe it for next/previous, tap the text for the track actions, hold the text
 * for the configured Title hold (Immersive or favourite), drag the progress line to seek. Nothing playing: a dot
 * weather icon, the weather, a dot-matrix clock, the date and "tap to start" (the Library).
 * Standby (saver.c forwards here): the same clock face; while a song plays, its title and a small audio form.
 *
 * This layer is built on top of Disco's Music widgets (disco_home.c keeps running underneath for its popup, idle
 * logic and timers); it is opaque, so LVGL never draws what it covers. Colours and fonts are theme roles only. */
#include "screens.h"
#include "theme.h"
#include "config.h"
#include "theme_kit.h"
#include "onboarding.h"
#include "ipc.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define DISC_D   300              /* CD + info: the CD's diameter, its centre above the screen's top third */
#define DISC_CY  50
#define BIG_D    296              /* Disc only */
#define LYR_CAP  240

void disco_track_overlay_open(void);
unsigned ui_art_revision(void);

static void keep_all(lv_obj_t *o){                                       /* styled here: the theme kit leaves it alone */
    kit_keep(o);
    for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++) keep_all(lv_obj_get_child(o, (int32_t)i));
}
/* ---- small shared pieces ----------------------------------------------------------------------------------------- */
static lv_obj_t *plain(lv_obj_t *p){
    lv_obj_t *o = lv_obj_create(p); lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}
static lv_obj_t *dot(lv_obj_t *p, int d, lv_color_t c){
    lv_obj_t *o = plain(p); lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_bg_color(o, c, 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}
static lv_obj_t *text(lv_obj_t *p, const lv_font_t *f, lv_color_t c, int w){
    lv_obj_t *l = lv_label_create(p); lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, f, 0); lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if(w > 0){ lv_obj_set_width(l, w); lv_label_set_long_mode(l, LV_LABEL_LONG_SCROLL_CIRCULAR); }
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/* Dot-matrix weather icons (original drawings): an 11 x 11 grid of dots, computed, not stored. */
enum { WX_NONE, WX_SUN, WX_MOON, WX_CLOUD, WX_RAIN, WX_BOLT, WX_SNOW };
static int wx_kind(const char *s){
    if(!s) return WX_NONE;
    if(strstr(s, "\xEF\x86\x85")) return WX_SUN;
    if(strstr(s, "\xEF\x86\x86")) return WX_MOON;
    if(strstr(s, "\xEF\x83\xA9")) return WX_RAIN;
    if(strstr(s, "\xEF\x83\xA7")) return WX_BOLT;
    if(strstr(s, "\xEF\x8B\x9C")) return WX_SNOW;
    if(strstr(s, "\xEF\x83\x82")) return WX_CLOUD;
    return WX_NONE;
}
/* 11 x 11, '#' = a dot */
static const char *const WXB[7][11] = {
    [WX_SUN]   = { ".....#.....", ".#.......#.", "...........", "....###....", "...#####...", "#..#####..#",
                   "...#####...", "....###....", "...........", ".#.......#.", ".....#....." },
    [WX_MOON]  = { "...####....", ".####......", ".###.......", "###........", "###........", "###........",
                   "###........", "####......#", ".####....##", "..#######..", "...#####..." },
    [WX_CLOUD] = { "...........", "...........", ".....###...", "....#####..", "..########.", ".##########",
                   "###########", "###########", ".#########.", "...........", "..........." },
    [WX_RAIN]  = { ".....###...", "....#####..", "..########.", ".##########", "###########", ".#########.",
                   "...........", "..#..#..#..", "...........", ".#..#..#...", "..........." },
    [WX_BOLT]  = { ".....###...", "....#####..", "..########.", ".##########", "###########", ".#########.",
                   "...........", ".....##....", "....##.....", ".....##....", "....#......" },
    [WX_SNOW]  = { ".....#.....", ".#...#...#.", "..#..#..#..", "...#.#.#...", "....###....", "###########",
                   "....###....", "...#.#.#...", "..#..#..#..", ".#...#...#.", ".....#....." },
};
static int wx_on(int k, int x, int y){ return k > WX_NONE && k <= WX_SNOW && WXB[k][y][x] == '#'; }

/* A dot matrix drawn in one object: each lit cell is a round dot (the clock digits and the weather icons). Cheap to
 * keep - one object, redrawn only when its bits change. */
#define DM_MAXC 40
#define DM_MAXR 12
typedef struct { int cols, rows, cell, dot; lv_color_t color; uint8_t on[DM_MAXR][DM_MAXC]; } dm_t;
static void dm_draw_cb(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e); dm_t *d = lv_obj_get_user_data(o); lv_layer_t *layer = lv_event_get_layer(e);
    if(!d || !layer) return;
    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.radius = LV_RADIUS_CIRCLE; r.bg_color = d->color; r.bg_opa = LV_OPA_COVER;
    int off = (d->cell - d->dot) / 2;
    for(int y = 0; y < d->rows; y++) for(int x = 0; x < d->cols; x++) if(d->on[y][x]){
        lv_area_t c = { a.x1 + x * d->cell + off, a.y1 + y * d->cell + off, 0, 0 };
        c.x2 = c.x1 + d->dot - 1; c.y2 = c.y1 + d->dot - 1;
        lv_draw_rect(layer, &r, &c);
    }
}
static void dm_free_cb(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static lv_obj_t *dm_create(lv_obj_t *p, int cell, int dot, lv_color_t c){
    lv_obj_t *o = plain(p); dm_t *d = calloc(1, sizeof *d);
    if(d){ d->cell = cell; d->dot = dot; d->color = c; }
    lv_obj_set_user_data(o, d); lv_obj_set_size(o, 0, 0);
    lv_obj_add_event_cb(o, dm_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(o, dm_free_cb, LV_EVENT_DELETE, NULL);
    return o;
}
static void dm_commit(lv_obj_t *o){
    dm_t *d = lv_obj_get_user_data(o); if(!d) return;
    lv_obj_set_size(o, d->cols * d->cell, d->rows * d->cell); lv_obj_invalidate(o);
}
/* the clock's digits: an original 5 x 7 face with soft corners (bit 4 = left column) */
static const uint8_t DIG[10][7] = {
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}, {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
};
static void dm_clock(lv_obj_t *o, const char *t){
    dm_t *d = lv_obj_get_user_data(o); if(!d || !t) return;
    memset(d->on, 0, sizeof d->on); d->rows = 7; int x = 0;
    for(const char *p = t; *p && x < DM_MAXC - 5; p++){
        if(*p >= '0' && *p <= '9'){
            if(x) x++;
            for(int r = 0; r < 7; r++) for(int c = 0; c < 5; c++) if(DIG[*p - '0'][r] & (0x10 >> c)) d->on[r][x + c] = 1;
            x += 5;
        } else if(*p == ':'){ x++; d->on[2][x] = d->on[4][x] = 1; x += 1; }
        else if(*p == '-'){ if(x) x++; for(int c = 0; c < 5; c++) d->on[3][x + c] = 1; x += 5; }
    }
    d->cols = x; dm_commit(o);
}
static void dm_icon(lv_obj_t *o, int kind){
    dm_t *d = lv_obj_get_user_data(o); if(!d) return;
    memset(d->on, 0, sizeof d->on);
    d->cols = d->rows = kind == WX_NONE ? 0 : 11;
    for(int y = 0; y < d->rows; y++) for(int x = 0; x < d->cols; x++) d->on[y][x] = (uint8_t)wx_on(kind, x, y);
    dm_commit(o);
}

/* the date, "SAT. 03 OCT" with the day number in red (from the clock itself, so the format never matters);
 * "14°C  Partly cloudy" -> "PARTLY CLOUDY" + "14°" */
static void date_text(char *a, size_t an, char *day, size_t dn, char *b, size_t bn){
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char w[8], m[8]; strftime(w, sizeof w, "%a", &tm); strftime(m, sizeof m, "%b", &tm);
    for(char *p = w; *p; p++) *p = (char)toupper((unsigned char)*p);
    for(char *p = m; *p; p++) *p = (char)toupper((unsigned char)*p);
    snprintf(a, an, "%s. ", w); snprintf(day, dn, "%02d", tm.tm_mday); snprintf(b, bn, " %s", m);
}
static void weather_text(const char *s, char *cond, size_t cn, char *temp, size_t tn){
    cond[0] = temp[0] = 0; if(!s) return;
    const char *d = strstr(s, "\xC2\xB0");                                /* the degree sign */
    if(d){ const char *b = d; while(b > s && ((b[-1] >= '0' && b[-1] <= '9') || b[-1] == '-')) b--; snprintf(temp, tn, "%.*s\xC2\xB0", (int)(d - b), b); }
    const char *c = d ? d + 2 : s;
    if(d && (*c == 'C' || *c == 'F')) c++;                                /* the unit, once */
    while(*c == ' ') c++;
    if(!d){ while(*c && (unsigned char)*c >= 0x80) c++; while(*c == ' ') c++; }   /* no temperature: past the icon */
    size_t k = 0; for(; *c && k + 1 < cn; c++) cond[k++] = (char)toupper((unsigned char)*c);
    cond[k] = 0;
}

/* the clock face, shared by idle Music and standby. With the NTDisk (dot) font the clock is a dot matrix; with
 * Sans or Mono it is that font's clock. */
typedef struct { lv_obj_t *root, *icon, *wx_row, *cond, *temp, *clock, *dclock, *date_row, *d_a, *d_day, *d_b; } face_t;
static void face_build(face_t *f, lv_obj_t *parent, int y_icon, int y_wx, int y_clock, int y_date, const lv_font_t *clock_font, int cell, int music){
#define FC(role) (music ? th_nt_music_color(THEME_CLR_##role) : TC(role))
    f->root = parent;
    f->icon = dm_create(parent, cell * 7 / 11 > 4 ? cell * 7 / 11 : 4, cell * 7 / 11 > 4 ? cell * 7 / 11 - 1 : 3, FC(TEXT_PRIMARY));
    lv_obj_align(f->icon, LV_ALIGN_TOP_MID, 0, y_icon);
    f->wx_row = plain(parent); lv_obj_set_size(f->wx_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(f->wx_row, LV_FLEX_FLOW_ROW); lv_obj_set_style_pad_column(f->wx_row, 10, 0);
    lv_obj_align(f->wx_row, LV_ALIGN_TOP_MID, 0, y_wx);
    f->cond = text(f->wx_row, TF(UI_16), FC(TEXT_SECONDARY), 0); f->temp = text(f->wx_row, TF(UI_16), FC(ACCENT_PRIMARY), 0);
    if(cfg_get_int("nt_font", 0) != 2){
        f->dclock = dm_create(parent, cell, cell - 2, FC(TEXT_PRIMARY)); dm_clock(f->dclock, "--:--");
        lv_obj_align(f->dclock, LV_ALIGN_TOP_MID, 0, y_clock + 4);
    } else {
        f->clock = text(parent, clock_font, FC(TEXT_PRIMARY), 0); lv_label_set_text(f->clock, "--:--");
        lv_obj_align(f->clock, LV_ALIGN_TOP_MID, 0, y_clock);
    }
    f->date_row = plain(parent); lv_obj_set_size(f->date_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(f->date_row, LV_FLEX_FLOW_ROW);
    lv_obj_align(f->date_row, LV_ALIGN_TOP_MID, 0, y_date);
    f->d_a = text(f->date_row, TF(UI_18), FC(TEXT_PRIMARY), 0); f->d_day = text(f->date_row, TF(UI_18), FC(ACCENT_PRIMARY), 0);
    f->d_b = text(f->date_row, TF(UI_18), FC(TEXT_PRIMARY), 0);
#undef FC
}
static void face_clock(face_t *f, const char *t, const char *sub){
    (void)sub;
    if(!f->d_a) return;
    if(t){ char c[16]; int n = 0; while(t[n] && n < 15 && t[n] != ' ') { c[n] = t[n]; n++; } c[n] = 0;
           if(f->dclock){ dm_clock(f->dclock, c); lv_obj_align(f->dclock, LV_ALIGN_TOP_MID, 0, lv_obj_get_y_aligned(f->dclock)); }
           if(f->clock) lv_label_set_text(f->clock, c); }
    char a[24], d[24], b[24]; date_text(a, sizeof a, d, sizeof d, b, sizeof b);
    lv_label_set_text(f->d_a, a); lv_label_set_text(f->d_day, d); lv_label_set_text(f->d_b, b);
}
static void face_weather(face_t *f, const char *s){
    if(!f->cond) return;
    char c[48], t[16]; weather_text(s, c, sizeof c, t, sizeof t);
    lv_label_set_text(f->cond, c); lv_label_set_text(f->temp, t);
    if(c[0]) lv_obj_remove_flag(f->cond, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(f->cond, LV_OBJ_FLAG_HIDDEN);
    dm_icon(f->icon, wx_kind(s));
}

#define MC(role) th_nt_music_color(THEME_CLR_##role)
/* ---- Music --------------------------------------------------------------------------------------------------------- */
static lv_obj_t *g_layer, *g_disc, *g_art, *g_hub, *g_hole, *g_state, *g_ring;
static lv_obj_t *g_info, *g_artist, *g_title, *g_lyric, *g_line, *g_line_fill, *g_el, *g_tot, *g_idle_box, *g_start;
static face_t g_face;
static lv_timer_t *g_tm;
static int g_playing, g_have, g_mode, g_lyr_on;
static char g_t[160], g_a[160], g_clock[16], g_sub[32], g_wx[96];
static lyr_line_t *g_lyr; static int g_lyr_n; static unsigned g_lyr_rev = ~0u;
static int g_drag;

static void fmt_time(long ms, char *o, size_t n){ long s = ms > 0 ? ms / 1000 : 0; snprintf(o, n, "%ld:%02ld", s / 60, s % 60); }
/* The CD turns while music plays (about one turn every 8 s). The cover is resampled once per album into a square
 * buffer; each frame rotates it into a round ARGB image (bilinear, antialiased edge) - only the rows that are on screen,
 * and only while Music is showing and playing. No clip mask, so LVGL just blends one image. */
#define SPIN_MS   66
#define SPIN_TURN 8000
static uint32_t *g_src, *g_out; static uint8_t *g_mask; static int g_sd;   /* d x d */
static lv_image_dsc_t g_spin_dsc; static int g_angle10, g_src_ok, g_row0;   /* angle in tenths of a degree */
static unsigned g_src_rev = ~0u; static lv_timer_t *g_spin_tm;
static uint32_t px_of(lv_color_t c){ return 0xFF000000u | ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | c.blue; }
static void spin_alloc(int d){
    if(g_sd == d) return;
    free(g_src); free(g_out); free(g_mask);
    g_src = malloc((size_t)d * d * 4); g_out = calloc((size_t)d * d, 4); g_mask = malloc((size_t)d * d);
    g_sd = (g_src && g_out && g_mask) ? d : 0;
    if(!g_sd) return;
    float c = d / 2.0f, r = d / 2.0f - 0.5f;
    for(int y = 0; y < d; y++) for(int x = 0; x < d; x++){
        float dx = x + 0.5f - c, dy = y + 0.5f - c, cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
        g_mask[y * d + x] = cov <= 0 ? 0 : cov >= 1 ? 255 : (uint8_t)(cov * 255);
    }
    memset(&g_spin_dsc, 0, sizeof g_spin_dsc);
    g_spin_dsc.header.magic = LV_IMAGE_HEADER_MAGIC; g_spin_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    g_spin_dsc.header.w = g_spin_dsc.header.h = (uint32_t)d; g_spin_dsc.header.stride = (uint32_t)d * 4;
    g_spin_dsc.data = (const uint8_t *)g_out; g_spin_dsc.data_size = (uint32_t)d * d * 4;
}
/* the album into g_src (or, with no cover, a plain disc with grooves and a red mark so the turn is visible) */
static void spin_source(void){
    int d = g_sd; if(!d) return;
    const lv_image_dsc_t *a = g_have ? ui_current_sharp_img() : NULL;
    int ok = a && a->data && a->header.w > 0 &&
             (a->header.cf == LV_COLOR_FORMAT_RGB888 || a->header.cf == LV_COLOR_FORMAT_XRGB8888 || a->header.cf == LV_COLOR_FORMAT_ARGB8888);
    if(ok){
        int w = (int)a->header.w, h = (int)a->header.h, bpp = a->header.cf == LV_COLOR_FORMAT_RGB888 ? 3 : 4;
        int st = a->header.stride ? (int)a->header.stride : w * bpp;
        for(int y = 0; y < d; y++){                                       /* area-averaged (a box of source pixels) */
            int y0 = y * h / d, y1 = (y + 1) * h / d; if(y1 <= y0) y1 = y0 + 1; if(y1 > h) y1 = h;
            for(int x = 0; x < d; x++){
                int x0 = x * w / d, x1 = (x + 1) * w / d; if(x1 <= x0) x1 = x0 + 1; if(x1 > w) x1 = w;
                unsigned r = 0, g = 0, b = 0, n = 0;
                for(int yy = y0; yy < y1; yy++){ const uint8_t *p = a->data + (size_t)yy * st + (size_t)x0 * bpp;
                    for(int xx = x0; xx < x1; xx++, p += bpp){ b += p[0]; g += p[1]; r += p[2]; n++; } }
                g_src[y * d + x] = 0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
            }
        }
    } else {
        uint32_t bg = px_of(MC(SURFACE)), gr = px_of(MC(BORDER_STRONG)), red = px_of(MC(ACCENT_PRIMARY));
        float c = d / 2.0f;
        for(int y = 0; y < d; y++) for(int x = 0; x < d; x++){
            float dx = x + 0.5f - c, dy = y + 0.5f - c, r = sqrtf(dx * dx + dy * dy);
            uint32_t v = bg;
            for(int i = 0; i < 4; i++){ float gr0 = (d - 30 - i * 34) / 2.0f; if(fabsf(r - gr0) < 0.6f) v = gr; }
            float mx = dx, my = dy + (d / 2.0f - 22);                  /* a red mark near the top edge */
            if(mx * mx + my * my < 16) v = red;
            g_src[y * d + x] = v;
        }
    }
    g_src_ok = 1; g_src_rev = ui_art_revision();
}
static void spin_render(void){
    int d = g_sd; if(!d || !g_src_ok) return;
    float rad = g_angle10 * 0.1f * 0.01745329f;
    int32_t cs = (int32_t)(cosf(rad) * 65536), sn = (int32_t)(sinf(rad) * 65536), c16 = d * 32768;   /* centre, 16.16 */
    for(int y = g_row0; y < d; y++){
        int32_t yy = y * 65536 + 32768 - c16;
        uint32_t *o = g_out + (size_t)y * d; const uint8_t *m = g_mask + (size_t)y * d;
        for(int x = 0; x < d; x++){
            if(!m[x]){ o[x] = 0; continue; }
            int32_t xx = x * 65536 + 32768 - c16;
            /* inverse rotation: where this pixel comes from in the unrotated cover */
            int32_t sx = (int32_t)(((int64_t)xx * cs + (int64_t)yy * sn) >> 16) + c16 - 32768;
            int32_t sy = (int32_t)((-(int64_t)xx * sn + (int64_t)yy * cs) >> 16) + c16 - 32768;
            int ix = sx >> 16, iy = sy >> 16; unsigned fx = (unsigned)(sx >> 8) & 255, fy = (unsigned)(sy >> 8) & 255;
            if(ix < 0){ ix = 0; fx = 0; } if(iy < 0){ iy = 0; fy = 0; }
            if(ix >= d - 1){ ix = d - 2; fx = 255; } if(iy >= d - 1){ iy = d - 2; fy = 255; }
            const uint32_t *p = g_src + (size_t)iy * d + ix;
            uint32_t a = p[0], b = p[1], c = p[d], e = p[d + 1];
            unsigned w00 = (256 - fx) * (256 - fy) >> 8, w10 = fx * (256 - fy) >> 8, w01 = (256 - fx) * fy >> 8, w11 = fx * fy >> 8;
            uint32_t rb = (((a & 0xFF00FF) * w00 + (b & 0xFF00FF) * w10 + (c & 0xFF00FF) * w01 + (e & 0xFF00FF) * w11) >> 8) & 0xFF00FF;
            uint32_t g = (((a & 0x00FF00) * w00 + (b & 0x00FF00) * w10 + (c & 0x00FF00) * w01 + (e & 0x00FF00) * w11) >> 8) & 0x00FF00;
            o[x] = (uint32_t)m[x] << 24 | rb | g;
        }
    }
    if(g_art) lv_obj_invalidate(g_art);
}
static void spin_tick(lv_timer_t *t){
    (void)t;
    if(!g_playing || !g_have || screen_current() != SCR_HOME){ lv_timer_pause(g_spin_tm); return; }
    g_angle10 = (g_angle10 + 3600 * SPIN_MS / SPIN_TURN) % 3600;
    spin_render();
}
static void spin_run(void){                                              /* start or stop the turning */
    if(!g_spin_tm) return;
    if(g_playing && g_have && screen_current() == SCR_HOME) lv_timer_resume(g_spin_tm); else lv_timer_pause(g_spin_tm);
}
static void art_apply(int force){
    if(!g_art) return;
    static int last_have = -1;
    spin_alloc(g_mode ? BIG_D : DISC_D);
    if(!g_sd){ lv_obj_add_flag(g_art, LV_OBJ_FLAG_HIDDEN); return; }
    if(!force && g_src_ok && last_have == g_have && g_src_rev == ui_art_revision()) return;   /* nothing new */
    last_have = g_have;
    spin_source(); spin_render();
    lv_image_set_src(g_art, NULL); lv_image_set_src(g_art, &g_spin_dsc);   /* same buffer, new pixels */
    lv_obj_remove_flag(g_art, LV_OBJ_FLAG_HIDDEN); lv_obj_center(g_art);
}
/* the lyric line sits centred between the artist and the progress line: one line or two (then it ends in ...) */
static void lyric_place(void){
    const lv_font_t *f = ui_font_cjk(18); int lh = lv_font_get_line_height(f);
    lv_point_t sz; lv_text_get_size(&sz, lv_label_get_text(g_lyric), f, 0, 0, 280, LV_TEXT_FLAG_NONE);
    int h = sz.y > 2 * lh ? 2 * lh : sz.y < lh ? lh : sz.y;
    lv_obj_set_height(g_lyric, h); lv_obj_align(g_lyric, LV_ALIGN_TOP_MID, 0, 50 - h / 2);
}
/* the play / pause mark in the hub shows on a touch or a change, then fades away after 2 s */
static void hub_opa(void *o, int32_t v){ lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void hub_flash(void){
    if(!g_state) return;
    lv_anim_delete(g_state, hub_opa); lv_obj_set_style_opa(g_state, LV_OPA_COVER, 0);
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, g_state); lv_anim_set_exec_cb(&a, hub_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP); lv_anim_set_delay(&a, 2000); lv_anim_set_duration(&a, 400);
    lv_anim_start(&a);
}
static void lyrics_refresh(long pos){
    if(!g_lyric) return;
    const char *line = NULL;
    if(g_lyr_on && g_have && !g_mode){
        unsigned rev = 0; int n0 = lyrics_immersive_load(NULL, 0, &rev);  /* cheap: asks for this song's lyrics */
        if(n0 < 0){ g_lyr_n = 0; g_lyr_rev = ~0u; }                      /* not ready (or a new song): the title */
        else if(rev != g_lyr_rev){
            if(!g_lyr) g_lyr = malloc(sizeof *g_lyr * LYR_CAP);
            int n = g_lyr ? lyrics_immersive_load(g_lyr, LYR_CAP, &rev) : -1;
            g_lyr_n = n > 0 ? n : 0; g_lyr_rev = rev;
        }
        for(int i = 0; i < g_lyr_n; i++){ if(g_lyr[i].ms <= pos + 300) line = g_lyr[i].text; else break; }
        if(line && !line[0]) line = NULL;
        if(!line && g_lyr_n > 0 && pos < g_lyr[0].ms) line = NULL;
    }
    if(line){
        if(strcmp(lv_label_get_text(g_lyric), line)){ lv_label_set_text(g_lyric, line); lyric_place(); }
        lv_obj_remove_flag(g_lyric, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(g_title, LV_OBJ_FLAG_HIDDEN);
    } else { lv_obj_add_flag(g_lyric, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(g_title, LV_OBJ_FLAG_HIDDEN); }
}
static void progress_paint(void){
    track_state_t st; ipc_get_state(&st);
    long pos = st.position_ms, dur = st.duration_ms;
    if(!g_have){ pos = dur = 0; }
    int frac = dur > 0 ? (int)((long long)pos * 1000 / dur) : 0; if(frac > 1000) frac = 1000;
    if(g_line_fill && !g_drag) lv_obj_set_width(g_line_fill, 120 * frac / 1000 > 0 ? 120 * frac / 1000 : 0);
    if(g_ring) lv_arc_set_value(g_ring, frac);
    char e[24], t[24]; fmt_time(pos, e, sizeof e); fmt_time(dur, t, sizeof t);
    if(g_el){ lv_label_set_text(g_el, e); char tt[32]; snprintf(tt, sizeof tt, " / %s", t); lv_label_set_text(g_tot, tt); }
    lyrics_refresh(pos);
}
static void tick_cb(lv_timer_t *t){
    (void)t;
    if(screen_current() != SCR_HOME) return;
    progress_paint(); spin_run();
}
static void view_apply(void){
    if(!g_layer) return;
    int idle = !g_have;
    if(idle){ lv_obj_add_flag(g_disc, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(g_info, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(g_idle_box, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_remove_flag(g_disc, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(g_idle_box, LV_OBJ_FLAG_HIDDEN);
           if(g_mode) lv_obj_add_flag(g_info, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(g_info, LV_OBJ_FLAG_HIDDEN); }
    if(strcmp(lv_label_get_text(g_state), g_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY)){
        lv_label_set_text(g_state, g_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY); hub_flash(); }
    art_apply(0); progress_paint();
    spin_run();
}

/* touch */
static lv_point_t g_down;
/* The CD's gestures: tap = play / pause; swipe left / right = next / previous; swipe up = Immersive; hold = track
 * actions; put a finger on the outer part of the CD and turn it = scrub (one turn = 60 s), the time shows in the hub
 * and the CD turns with the finger. */
#define SCRUB_TURN_MS 60000
static int g_g_mode, g_g_moved; static float g_g_ang0, g_g_acc; static long g_g_pos0, g_g_dur, g_g_target;
static uint32_t g_g_t0;
static int disc_cx(void){ return 180; }
static int disc_cy(void){ return g_mode ? 180 : DISC_CY; }
static float pt_angle(lv_point_t p){ return atan2f((float)(p.y - disc_cy()), (float)(p.x - disc_cx())) * 57.29578f; }
static void scrub_show(long ms){
    char b[16]; fmt_time(ms, b, sizeof b);
    lv_anim_delete(g_state, hub_opa); lv_obj_set_style_opa(g_state, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(g_state, TF(UI_12), 0); lv_label_set_text(g_state, b);
}
static void disc_cb(lv_event_t *e){
    lv_event_code_t c = lv_event_get_code(e); lv_indev_t *in = lv_indev_active(); lv_point_t p = { 0, 0 };
    if(in) lv_indev_get_point(in, &p);
    if(c == LV_EVENT_PRESSED){
        g_down = p; g_g_mode = 0; g_g_moved = 0; g_g_acc = 0; g_g_ang0 = pt_angle(p); g_g_t0 = lv_tick_get();
        int dx = p.x - disc_cx(), dy = p.y - disc_cy(), r = (g_mode ? BIG_D : DISC_D) / 2;
        if(dx * dx + dy * dy > (r * 55 / 100) * (r * 55 / 100)) g_g_mode = 1;           /* the outer part: may become a turn */
        track_state_t st; ipc_get_state(&st); g_g_pos0 = st.position_ms; g_g_dur = st.duration_ms; g_g_target = g_g_pos0;
        hub_flash();
        return;
    }
    if(c == LV_EVENT_PRESSING){
        int dx = p.x - g_down.x, dy = p.y - g_down.y;
        if(abs(dx) > 10 || abs(dy) > 10) g_g_moved = 1;
        if(g_g_mode == 1 || g_g_mode == 2){
            float a = pt_angle(p), d = a - g_g_ang0; while(d > 180) d -= 360; while(d < -180) d += 360;
            g_g_ang0 = a; g_g_acc += d;
            if(g_g_mode == 1 && fabsf(g_g_acc) > 14) g_g_mode = 2;                   /* it's a turn, not a swipe */
            if(g_g_mode == 2 && g_g_dur > 0){
                g_g_target = g_g_pos0 + (long)(g_g_acc / 360.0f * SCRUB_TURN_MS);
                if(g_g_target < 0) g_g_target = 0;
                if(g_g_target > g_g_dur - 1000) g_g_target = g_g_dur - 1000;
                scrub_show(g_g_target);
                g_angle10 = ((g_angle10 + (int)(d * 10)) % 3600 + 3600) % 3600; spin_render();   /* the CD follows */
            }
        }
        return;
    }
    if(c == LV_EVENT_RELEASED){
        int dx = p.x - g_down.x, dy = p.y - g_down.y; uint32_t dt = lv_tick_elaps(g_g_t0);
        if(g_g_mode == 2){
            if(g_g_dur > 0) ui_seek_to(g_g_target);
            lv_obj_set_style_text_font(g_state, TF(UI_14), 0);
            lv_label_set_text(g_state, g_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY); hub_flash();
        } else if(dt < 800 && abs(dx) >= 50 && abs(dx) > 2 * abs(dy)){
            if(dx < 0){ ui_transport_command("0201000C0001"); ui_toast_icon(LV_SYMBOL_NEXT, MC(ACCENT_PRIMARY), "Next"); }
            else { ui_transport_command("0201000C0002"); ui_toast_icon(LV_SYMBOL_PREV, MC(ACCENT_PRIMARY), "Previous"); }
        } else if(dt < 800 && dy <= -50 && -dy > 2 * abs(dx)) disco_open_np_immersive();
        g_g_mode = 0;
        return;
    }
    if(c == LV_EVENT_SHORT_CLICKED && !g_g_moved) ui_transport_command("0201000C0000");
    else if(c == LV_EVENT_LONG_PRESSED && !g_g_moved && g_g_mode != 2) disco_track_overlay_open();
}
static void info_cb(lv_event_t *e){
    lv_event_code_t c = lv_event_get_code(e);
    if(c == LV_EVENT_SHORT_CLICKED){ onboarding_discovered(HINT_MUSIC); disco_track_overlay_open(); }
    else if(c == LV_EVENT_LONG_PRESSED){
        int h = cfg_get_int("disco_title_hold", 0);
        if(h == 0) disco_open_np_immersive(); else if(h == 1) ui_np_fav_toggle();
    }
}
static void seek_cb(lv_event_t *e){
    lv_event_code_t c = lv_event_get_code(e); lv_indev_t *in = lv_indev_active(); lv_point_t p;
    if(!in) return;
    lv_indev_get_point(in, &p);
    lv_area_t a; lv_obj_get_coords(g_line, &a);
    int frac = (int)((p.x - a.x1) * 1000 / (a.x2 - a.x1 + 1)); if(frac < 0) frac = 0; if(frac > 1000) frac = 1000;
    if(c == LV_EVENT_PRESSED || c == LV_EVENT_PRESSING){ g_drag = 1; lv_obj_set_width(g_line_fill, 120 * frac / 1000); }
    else if(c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST){
        g_drag = 0;
        track_state_t st; ipc_get_state(&st);
        if(c == LV_EVENT_RELEASED && st.have_track && st.duration_ms > 0) ui_seek_to((long)((long long)st.duration_ms * frac / 1000));
    }
}
static void start_cb(lv_event_t *e){ (void)e; screen_show(SCR_LIBRARY); }

void nt_home_create(lv_obj_t *root){
    g_mode = cfg_get_int("nt_music", 0) == 1;
    g_lyr_on = cfg_get_int("nt_lyrics", 1) != 0;
    g_layer = plain(root); lv_obj_set_size(g_layer, 360, 360);
    lv_obj_set_style_bg_color(g_layer, MC(CANVAS), 0); lv_obj_set_style_bg_opa(g_layer, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_layer, LV_OBJ_FLAG_CLICKABLE);                      /* nothing underneath takes a touch */

    /* the CD */
    int d = g_mode ? BIG_D : DISC_D;
    g_disc = plain(g_layer); lv_obj_set_size(g_disc, d, d);
    if(g_mode) lv_obj_center(g_disc); else lv_obj_set_pos(g_disc, 180 - d / 2, DISC_CY - d / 2);
    lv_obj_set_style_radius(g_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_disc, MC(SURFACE), 0); lv_obj_set_style_bg_opa(g_disc, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_disc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK); lv_obj_remove_flag(g_disc, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(g_disc, disc_cb, LV_EVENT_ALL, NULL);
    g_art = lv_image_create(g_disc); lv_obj_remove_flag(g_art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *rim = plain(g_disc); lv_obj_set_size(rim, d, d); lv_obj_center(rim);             /* a fine edge */
    lv_obj_set_style_radius(rim, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_border_width(rim, 2, 0);
    lv_obj_set_style_border_color(rim, MC(TEXT_PRIMARY), 0); lv_obj_set_style_border_opa(rim, LV_OPA_20, 0);
    g_hub = dot(g_disc, 96, MC(TEXT_PRIMARY)); lv_obj_set_style_bg_opa(g_hub, LV_OPA_30, 0); lv_obj_center(g_hub);   /* the clear hub */
    lv_obj_set_style_border_width(g_hub, 1, 0); lv_obj_set_style_border_color(g_hub, MC(TEXT_PRIMARY), 0); lv_obj_set_style_border_opa(g_hub, LV_OPA_40, 0);
    g_hole = dot(g_disc, 42, MC(CANVAS)); lv_obj_center(g_hole);
    lv_obj_set_style_border_width(g_hole, 3, 0); lv_obj_set_style_border_color(g_hole, MC(SURFACE_STRONG), 0);
    g_state = text(g_hole, TF(UI_14), MC(TEXT_SECONDARY), 0); lv_obj_center(g_state);
    if(g_mode){                                                           /* Disc only: progress round the CD */
        g_ring = lv_arc_create(g_layer); lv_obj_remove_style(g_ring, NULL, LV_PART_KNOB); lv_obj_remove_flag(g_ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(g_ring, d + 30, d + 30); lv_obj_center(g_ring);
        lv_arc_set_bg_angles(g_ring, 0, 360); lv_arc_set_rotation(g_ring, 270); lv_arc_set_range(g_ring, 0, 1000);
        lv_obj_set_style_arc_width(g_ring, 3, LV_PART_MAIN); lv_obj_set_style_arc_color(g_ring, MC(CONTROL_TRACK), LV_PART_MAIN);
        lv_obj_set_style_arc_width(g_ring, 3, LV_PART_INDICATOR); lv_obj_set_style_arc_color(g_ring, MC(ACCENT_PRIMARY), LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(g_ring, true, LV_PART_INDICATOR);
    }

    /* the information under the CD */
    g_info = plain(g_layer); lv_obj_set_size(g_info, 360, 160); lv_obj_set_pos(g_info, 0, 200);
    lv_obj_t *hit = plain(g_info); lv_obj_set_size(hit, 280, 76); lv_obj_align(hit, LV_ALIGN_TOP_MID, 0, 14);   /* the text: actions */
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(hit, info_cb, LV_EVENT_ALL, NULL);
    g_artist = text(hit, ui_font_cjk(16), MC(TEXT_MUTED), 250); lv_obj_align(g_artist, LV_ALIGN_TOP_MID, 0, 0);
    g_title = text(hit, ui_font_cjk(22), MC(TEXT_PRIMARY), 270); lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 22);
    g_lyric = text(hit, ui_font_cjk(18), MC(TEXT_PRIMARY), 0); lv_obj_set_width(g_lyric, 280);
    lv_label_set_long_mode(g_lyric, LV_LABEL_LONG_DOT); lv_obj_set_height(g_lyric, 2 * lv_font_get_line_height(ui_font_cjk(18)));
    lv_obj_align(g_lyric, LV_ALIGN_TOP_MID, 0, 38); lv_obj_add_flag(g_lyric, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *seek = plain(g_info); lv_obj_set_size(seek, 160, 26); lv_obj_align(seek, LV_ALIGN_TOP_MID, 0, 82);   /* the line: seek */
    lv_obj_add_flag(seek, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(seek, seek_cb, LV_EVENT_ALL, NULL);
    g_line = plain(seek); lv_obj_set_size(g_line, 120, 3); lv_obj_center(g_line);
    lv_obj_set_style_radius(g_line, 2, 0); lv_obj_set_style_bg_color(g_line, MC(CONTROL_TRACK), 0); lv_obj_set_style_bg_opa(g_line, LV_OPA_COVER, 0);
    g_line_fill = plain(g_line); lv_obj_set_size(g_line_fill, 0, 3);
    lv_obj_set_style_radius(g_line_fill, 2, 0); lv_obj_set_style_bg_color(g_line_fill, MC(TEXT_PRIMARY), 0); lv_obj_set_style_bg_opa(g_line_fill, LV_OPA_COVER, 0);
    lv_obj_t *times = plain(g_info); lv_obj_set_size(times, LV_SIZE_CONTENT, LV_SIZE_CONTENT); lv_obj_set_flex_flow(times, LV_FLEX_FLOW_ROW);
    lv_obj_align(times, LV_ALIGN_TOP_MID, 0, 106);
    g_el = text(times, TF(UI_16), MC(TEXT_PRIMARY), 0); g_tot = text(times, TF(UI_16), MC(TEXT_MUTED), 0);

    /* nothing playing: the clock face */
    g_idle_box = plain(g_layer); lv_obj_set_size(g_idle_box, 360, 360);
    face_build(&g_face, g_idle_box, 52, 126, 156, 236, TF(CLOCK), 9, 1);
    g_start = plain(g_idle_box); lv_obj_set_size(g_start, 178, 40); lv_obj_align(g_start, LV_ALIGN_TOP_MID, 0, 286);
    lv_obj_set_style_radius(g_start, 20, 0); lv_obj_add_flag(g_start, LV_OBJ_FLAG_CLICKABLE); lv_obj_set_ext_click_area(g_start, 8);
    lv_obj_set_style_bg_color(g_start, MC(TEXT_PRIMARY), 0); lv_obj_set_style_bg_opa(g_start, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_start, MC(SURFACE_PRESSED), LV_STATE_PRESSED); lv_obj_set_style_bg_opa(g_start, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(g_start, start_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = text(g_start, TF(UI_14), MC(CANVAS), 0); lv_label_set_text(sl, "tap to start"); lv_obj_center(sl);

    keep_all(g_layer);
    if(g_tm) lv_timer_delete(g_tm);                                     /* built again (a screen rebuild) */
    if(g_spin_tm) lv_timer_delete(g_spin_tm);
    g_spin_tm = lv_timer_create(spin_tick, SPIN_MS, NULL); lv_timer_pause(g_spin_tm);
    g_row0 = g_mode ? 0 : (DISC_D / 2 - DISC_CY);                       /* CD + info: the rows above the screen are never drawn */
    g_tm = lv_timer_create(tick_cb, 500, NULL);
    face_clock(&g_face, g_clock[0] ? g_clock : NULL, g_sub[0] ? g_sub : NULL);
    face_weather(&g_face, g_wx);
    view_apply();
}
void nt_home_now_playing(const char *title, const char *artist, int playing){
    int have = title && title[0];
    int changed = have != g_have || !!playing != g_playing || strcmp(g_t, title ? title : "") || strcmp(g_a, artist ? artist : "");
    g_have = have; g_playing = !!playing;                                 /* (called on every position frame: work only on a change) */
    snprintf(g_t, sizeof g_t, "%s", title ? title : ""); snprintf(g_a, sizeof g_a, "%s", artist ? artist : "");
    if(!g_layer || !changed) return;
    if(strcmp(lv_label_get_text(g_title), g_t)) lv_label_set_text(g_title, g_t);
    if(strcmp(lv_label_get_text(g_artist), g_a)) lv_label_set_text(g_artist, g_a);
    view_apply();
}
void nt_home_art_changed(void){ art_apply(1); }
void nt_home_lyrics_apply(int on){ g_lyr_on = on; g_lyr_rev = ~0u; if(g_layer) progress_paint(); }
void nt_home_clock(const char *t, const char *sub){
    if(t) snprintf(g_clock, sizeof g_clock, "%s", t);
    if(sub) snprintf(g_sub, sizeof g_sub, "%s", sub);
    face_clock(&g_face, t, sub);
}
void nt_home_weather(const char *s){ snprintf(g_wx, sizeof g_wx, "%s", s ? s : ""); face_weather(&g_face, s); }
/* main.c: a press here belongs to the CD or the seek line, not to the screen's swipes */
int nt_home_owns_point(int x, int y){
    if(!g_layer || !g_have) return 0;
    int cx = 180, cy = g_mode ? 180 : DISC_CY, r = (g_mode ? BIG_D : DISC_D) / 2;
    if((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) return 1;
    return !g_mode && y >= 200 + 78 && y <= 200 + 112 && x >= 90 && x <= 270;
}

/* ---- full-screen lyrics (NTDisk's Immersive) ---------------------------------------------------------------------
 * The cover fills the screen under a dark veil; the song's lines run over it, the current one bright and kept in the
 * middle. Scroll to read ahead (it follows again 4 s later); tap a line to jump there; tap the top handle, or hold
 * anywhere, to close. A song without synced lyrics shows its title and artist. */
static lv_obj_t *l_root, *l_cover, *l_list, *l_none, *l_time, **l_lab; static int l_n, l_cur = -1; static unsigned l_rev = ~0u;
static lv_timer_t *l_tm; static uint32_t l_touch; static lyr_line_t *l_lines;
static void l_close_cb(lv_event_t *e){ (void)e; nt_lyrics_close(); }
static void l_hold_cb(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) nt_lyrics_close(); }
static void l_gesture_cb(lv_event_t *e){                                  /* a swipe sideways closes it, like Back */
    (void)e; lv_indev_t *in = lv_indev_active(); lv_dir_t d = in ? lv_indev_get_gesture_dir(in) : LV_DIR_NONE;
    if(d == LV_DIR_RIGHT || d == LV_DIR_LEFT){ nt_lyrics_close(); if(in) lv_indev_wait_release(in); }
}
static void l_scroll_cb(lv_event_t *e){ (void)e; l_touch = lv_tick_get(); }
static void l_line_cb(lv_event_t *e){
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(screen_press_was_long() || !l_lines || i < 0 || i >= l_n) return;
    ui_seek_to(l_lines[i].ms); l_touch = 0; l_cur = -1;
}
static void l_build(void){
    lv_obj_clean(l_list); l_n = 0; l_cur = -1; free(l_lab); l_lab = NULL;
    if(!l_lines) l_lines = malloc(sizeof *l_lines * LYR_CAP);
    unsigned rev = 0; int n = l_lines ? lyrics_immersive_load(l_lines, LYR_CAP, &rev) : -1;
    if(n < 0) return;                                                     /* not ready yet: the tick asks again */
    l_rev = rev; l_n = n;
    if(n == 0){
        char b[360]; snprintf(b, sizeof b, "%s\n%s\n\nNo synced lyrics", g_t, g_a);
        lv_label_set_text(l_none, b); lv_obj_remove_flag(l_none, LV_OBJ_FLAG_HIDDEN); return;
    }
    lv_obj_add_flag(l_none, LV_OBJ_FLAG_HIDDEN);
    l_lab = calloc((size_t)n, sizeof *l_lab);
    for(int i = 0; i < n && l_lab; i++){
        lv_obj_t *l = l_lab[i] = lv_label_create(l_list);
        lv_label_set_text(l, l_lines[i].text[0] ? l_lines[i].text : "\xE2\x80\xA2");
        lv_obj_set_width(l, 276); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_style_text_font(l, ui_font_cjk(20), 0);
        lv_obj_set_style_text_color(l, TC(FIXED_MEDIA_WHITE), 0); lv_obj_set_style_text_opa(l, 110, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(l, l_line_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(l, l_hold_cb, LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
        kit_keep(l);
    }
    lv_obj_update_layout(l_list);
}
static void l_tick(lv_timer_t *t){
    (void)t;
    if(!nt_lyrics_visible()){ lv_timer_pause(l_tm); return; }
    unsigned rev = 0; int n0 = lyrics_immersive_load(NULL, 0, &rev);
    if(n0 >= 0 && rev != l_rev) l_build();
    track_state_t st; ipc_get_state(&st);
    char e[24], d[24], b[56]; fmt_time(st.position_ms, e, sizeof e); fmt_time(st.duration_ms, d, sizeof d);
    snprintf(b, sizeof b, "%s / %s", e, d); if(strcmp(lv_label_get_text(l_time), b)) lv_label_set_text(l_time, b);
    if(!l_lab || l_n <= 0) return;
    int cur = -1; for(int i = 0; i < l_n && l_lines[i].ms <= st.position_ms + 200; i++) cur = i;
    if(cur == l_cur) return;
    if(l_cur >= 0 && l_cur < l_n){ lv_obj_set_style_text_opa(l_lab[l_cur], 110, 0); lv_obj_set_style_text_font(l_lab[l_cur], ui_font_cjk(20), 0); }
    l_cur = cur;
    if(cur < 0) return;
    lv_obj_set_style_text_opa(l_lab[cur], LV_OPA_COVER, 0); lv_obj_set_style_text_font(l_lab[cur], ui_font_cjk(24), 0);
    if(l_touch && lv_tick_elaps(l_touch) < 4000) return;                  /* the reader is scrolling: leave it */
    lv_obj_update_layout(l_list);
    lv_area_t la, ll; lv_obj_get_coords(l_lab[cur], &la); lv_obj_get_coords(l_list, &ll);
    int y = (la.y1 + la.y2) / 2 - ll.y1 + lv_obj_get_scroll_y(l_list) - 180;
    lv_obj_scroll_to_y(l_list, y < 0 ? 0 : y, LV_ANIM_ON);
}
void nt_lyrics_open(void){
    if(!l_root){
        l_root = plain(lv_layer_top()); lv_obj_set_size(l_root, 360, 360);
        lv_obj_set_style_bg_color(l_root, TC(FIXED_MEDIA_BLACK), 0); lv_obj_set_style_bg_opa(l_root, LV_OPA_COVER, 0);
        lv_obj_add_flag(l_root, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(l_root, l_hold_cb, LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_add_event_cb(l_root, l_gesture_cb, LV_EVENT_GESTURE, NULL);
        l_cover = lv_image_create(l_root); lv_obj_remove_flag(l_cover, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *veil = plain(l_root); lv_obj_set_size(veil, 360, 360);
        lv_obj_set_style_bg_color(veil, TC(FIXED_MEDIA_BLACK), 0); lv_obj_set_style_bg_opa(veil, 175, 0);
        l_list = plain(l_root); lv_obj_set_size(l_list, 300, 360); lv_obj_align(l_list, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_add_flag(l_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE); lv_obj_set_scroll_dir(l_list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(l_list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(l_list, LV_FLEX_FLOW_COLUMN); lv_obj_set_flex_align(l_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(l_list, 14, 0); lv_obj_set_style_pad_top(l_list, 160, 0); lv_obj_set_style_pad_bottom(l_list, 160, 0);
        lv_obj_add_event_cb(l_list, l_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL); lv_obj_add_event_cb(l_list, l_scroll_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(l_list, l_hold_cb, LV_EVENT_LONG_PRESSED, NULL);
        l_none = text(l_root, ui_font_cjk(20), TC(FIXED_MEDIA_WHITE), 0); lv_obj_set_width(l_none, 270);
        lv_label_set_long_mode(l_none, LV_LABEL_LONG_WRAP); lv_obj_center(l_none);
        for(int k = 0; k < 2; k++){                                         /* the lines fade out at the top and bottom */
            lv_obj_t *f = plain(l_root); lv_obj_set_size(f, 360, k ? 84 : 64); lv_obj_align(f, k ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_TOP_MID, 0, 0);
            lv_obj_set_style_bg_color(f, TC(FIXED_MEDIA_BLACK), 0); lv_obj_set_style_bg_grad_color(f, TC(FIXED_MEDIA_BLACK), 0);
            lv_obj_set_style_bg_grad_dir(f, LV_GRAD_DIR_VER, 0); lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_main_opa(f, k ? 0 : 230, 0); lv_obj_set_style_bg_grad_opa(f, k ? 230 : 0, 0);
        }
        lv_obj_t *h = plain(l_root); lv_obj_set_size(h, 90, 34); lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 6);   /* the close handle */
        lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(h, l_close_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *bar = plain(h); lv_obj_set_size(bar, 36, 4); lv_obj_align(bar, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(bar, 2, 0); lv_obj_set_style_bg_color(bar, TC(FIXED_MEDIA_WHITE), 0); lv_obj_set_style_bg_opa(bar, 160, 0);
        lv_obj_t *dotr = plain(l_root); lv_obj_set_size(dotr, 6, 6); lv_obj_align(dotr, LV_ALIGN_BOTTOM_MID, -46, -30);
        lv_obj_set_style_radius(dotr, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_bg_color(dotr, TC(ACCENT_PRIMARY), 0); lv_obj_set_style_bg_opa(dotr, LV_OPA_COVER, 0);
        l_time = text(l_root, TF(UI_12), TC(FIXED_MEDIA_WHITE), 0); lv_obj_set_style_text_opa(l_time, 180, 0);
        lv_obj_align(l_time, LV_ALIGN_BOTTOM_MID, 6, -26);
        l_tm = lv_timer_create(l_tick, 250, NULL); lv_timer_pause(l_tm);
        keep_all(l_root);
    }
    const lv_image_dsc_t *c = g_have ? ui_current_sharp_img() : NULL;
    lv_image_set_src(l_cover, NULL);
    if(c){ lv_image_set_src(l_cover, c); lv_image_set_scale(l_cover, (uint32_t)(360 * 256 / (c->header.w ? c->header.w : 360)) + 1); lv_obj_center(l_cover); lv_obj_remove_flag(l_cover, LV_OBJ_FLAG_HIDDEN); }
    else lv_obj_add_flag(l_cover, LV_OBJ_FLAG_HIDDEN);
    l_rev = ~0u; l_touch = 0; lv_label_set_text(l_none, ""); l_build();
    lv_obj_remove_flag(l_root, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(l_root);
    lv_obj_set_style_opa(l_root, LV_OPA_COVER, 0);
    lv_timer_resume(l_tm); l_tick(NULL);
}
void nt_lyrics_close(void){ if(l_root) lv_obj_add_flag(l_root, LV_OBJ_FLAG_HIDDEN); if(l_tm) lv_timer_pause(l_tm); }
int nt_lyrics_visible(void){ return l_root && !lv_obj_has_flag(l_root, LV_OBJ_FLAG_HIDDEN); }

/* ---- standby ------------------------------------------------------------------------------------------------------- */
static face_t g_sface;
static lv_obj_t *g_strack, *g_snote;
void nt_saver_create(lv_obj_t *root){
    memset(&g_sface, 0, sizeof g_sface);
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0); lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_src(root, NULL, 0);
    face_build(&g_sface, root, 46, 120, 150, 242, TF(SAVER_CLOCK), 10, 0);
    g_snote = plain(root); lv_obj_set_size(g_snote, LV_SIZE_CONTENT, LV_SIZE_CONTENT);    /* while a song plays */
    lv_obj_set_flex_flow(g_snote, LV_FLEX_FLOW_ROW); lv_obj_set_style_pad_column(g_snote, 8, 0);
    lv_obj_set_flex_align(g_snote, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(g_snote, LV_ALIGN_TOP_MID, 0, 292);
    lv_obj_t *d = dot(g_snote, 7, TC(ACCENT_PRIMARY)); (void)d;
    g_strack = text(g_snote, ui_font_cjk(14), TC(TEXT_SECONDARY), 0);
    lv_label_set_long_mode(g_strack, LV_LABEL_LONG_DOT); lv_obj_set_width(g_strack, 190);
    lv_obj_add_flag(g_snote, LV_OBJ_FLAG_HIDDEN);
    face_clock(&g_sface, g_clock[0] ? g_clock : NULL, g_sub[0] ? g_sub : NULL);
    face_weather(&g_sface, g_wx);
    keep_all(root);
}
void nt_saver_clock(const char *t){ face_clock(&g_sface, t, g_sub[0] ? g_sub : NULL); }
void nt_saver_weather(const char *s){ face_weather(&g_sface, s); }
void nt_saver_track(const char *title, const char *artist){
    if(!g_strack) return;
    (void)artist;
    lv_label_set_text(g_strack, title ? title : "");
    if(title && title[0]){ lv_obj_set_width(g_strack, LV_SIZE_CONTENT); lv_obj_update_layout(g_strack);
                           if(lv_obj_get_width(g_strack) > 200) lv_obj_set_width(g_strack, 200);
                           lv_obj_remove_flag(g_snote, LV_OBJ_FLAG_HIDDEN); }
    else lv_obj_add_flag(g_snote, LV_OBJ_FLAG_HIDDEN);
}
