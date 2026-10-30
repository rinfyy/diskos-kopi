/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* idisc.c - iDisc, a glassy theme in the spirit of a modern phone OS (rounded "liquid glass" surfaces over a soft
 * wallpaper, system colours, Inter). It runs on NTDisk's machinery (th_nt() is true: the edge-swipe menu, the
 * sliding Quick Settings, full-screen lyrics); this file draws the parts that look different:
 *
 *   - the kit: screen wallpaper, glass list rows with tinted icon squares, glass back button, centred bold titles
 *   - Music: the album blurred behind everything, the cover as a rounded card that shrinks a little when paused,
 *     the title and artist, a scrubber with elapsed / remaining time and previous / play-pause / next
 *   - nothing playing, and standby: a lock-screen face - the date over a big clock, the weather, a glass
 *     Now Playing card on standby
 *   - Control Center (Quick Settings): a media card, round toggles that light blue, a wide brightness capsule
 *   - the volume: a glass capsule at the top
 *
 * Glass (cfg id_glass): Clear = see-through surfaces over the wallpaper (white at low opacity, a bright rim and a
 * sheen fading down); Tinted = solid grouped rows on a plain background (fastest, most contrast).
 * No real blur: the album wash is made once per cover (a 40 x 40 box blur stretched back up).
 * All colours are theme roles (theme.c); no third-party artwork, icons or names. */
#include "lockcover.h"
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "simple_theme.h"
#include "onboarding.h"
#include "quicksettings.h"
#include "ipc.h"
#include "idisc.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void disco_track_overlay_open(void);
unsigned ui_art_revision(void);

/* ---- small pieces ------------------------------------------------------------------------------------------------ */
static lv_obj_t *plain(lv_obj_t *p){
    lv_obj_t *o = lv_obj_create(p); lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
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
static void set_text(lv_obj_t *l, const char *s){ if(l && strcmp(lv_label_get_text(l), s ? s : "")) lv_label_set_text(l, s ? s : ""); }
static void keep_all(lv_obj_t *o){ kit_keep(o); for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++) keep_all(lv_obj_get_child(o, (int32_t)i)); }
static void show(lv_obj_t *o, int on){ if(!o) return; if(on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
static void fmt_time(long ms, char *o, size_t n){ long s = ms > 0 ? ms / 1000 : 0; snprintf(o, n, "%ld:%02ld", s / 60, s % 60); }
static int dark(void){ return !theme_variant(); }

/* ---- liquid glass ---------------------------------------------------------------------------------------------------
 * The wallpaper is a picture made once at start: soft-edged discs of the system hues (blue, indigo, teal, pink) over
 * a dark (or pale) wash. Its "frost" is the same picture box-blurred three times and lightened. A glass surface
 * draws, under its own tint, the part of the frost that lies behind it - so the shapes behind a row show through
 * it blurred and brighter, and move behind it as it scrolls - then a bright rim along its top edge (the light
 * catching it) and a faint dark one along the bottom. Only a copy of pixels per surface, no blur while drawing.
 * Tinted glass: none of this, solid rows on the plain canvas. */
#define WN 360
static uint32_t *g_wall, *g_frost; static lv_image_dsc_t g_wall_dsc, g_frost_dsc; static int g_wall_ok;
static void dsc_init(lv_image_dsc_t *d, uint32_t *px){
    memset(d, 0, sizeof *d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC; d->header.cf = LV_COLOR_FORMAT_XRGB8888;
    d->header.w = d->header.h = WN; d->header.stride = WN * 4; d->data = (const uint8_t *)px; d->data_size = WN * WN * 4;
}
static float smoothstep(float a, float b, float x){ float t = (x - a) / (b - a); t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }
/* three box-blur passes of radius r each way on one 8-bit plane (running sums) */
static void blur_plane(uint8_t *p, int r){
    static uint8_t t[WN * WN];
    for(int pass = 0; pass < 3; pass++){
        for(int y = 0; y < WN; y++){ int sum = 0; uint8_t *row = p + y * WN;
            for(int x = -r; x <= r; x++) sum += row[x < 0 ? 0 : x];
            for(int x = 0; x < WN; x++){ t[y * WN + x] = (uint8_t)(sum / (2 * r + 1));
                int a = x - r, b = x + r + 1; sum += row[b >= WN ? WN - 1 : b] - row[a < 0 ? 0 : a]; } }
        for(int x = 0; x < WN; x++){ int sum = 0;
            for(int y = -r; y <= r; y++) sum += t[(y < 0 ? 0 : y) * WN + x];
            for(int y = 0; y < WN; y++){ p[y * WN + x] = (uint8_t)(sum / (2 * r + 1));
                int a = y - r, b = y + r + 1; sum += t[(b >= WN ? WN - 1 : b) * WN + x] - t[(a < 0 ? 0 : a) * WN + x]; } }
    }
}
static void wall_make(void){
    if(g_wall_ok) return;
    g_wall = malloc(WN * WN * 4); g_frost = malloc(WN * WN * 4);
    uint8_t *pl = malloc(WN * WN * 3);
    if(!g_wall || !g_frost || !pl){ free(g_wall); free(g_frost); free(pl); g_wall = g_frost = NULL; return; }
    int d = dark();
    struct { float x, y, r, a; lv_color_t c; } B[5] = {
        { 64, 84, 124, 0.95f, TC(ACCENT_PRIMARY) }, { 300, 252, 132, 0.95f, TC(DECOR_1) }, { 262, 44, 74, 0.85f, TC(STATUS_INFO) },
        { 104, 328, 84, 0.8f, TC(DECOR_2) }, { 200, 170, 40, 0.55f, TC(STATUS_INFO) } };
    lv_color_t base0 = theme_color_mix(TC(DECOR_1), TC(CANVAS), d ? 60 : 30), base1 = TC(CANVAS);
    float dim = d ? 0.62f : 1.0f;
    for(int y = 0; y < WN; y++) for(int x = 0; x < WN; x++){
        float ty = y / (float)(WN - 1);
        float r = base0.red + (base1.red - base0.red) * ty, g = base0.green + (base1.green - base0.green) * ty, b = base0.blue + (base1.blue - base0.blue) * ty;
        for(int k = 0; k < 5; k++){
            float dx = x - B[k].x, dy = y - B[k].y, dist = sqrtf(dx * dx + dy * dy);
            float a = B[k].a * (1 - smoothstep(B[k].r - 9, B[k].r + 9, dist));          /* a soft, still visible edge */
            a *= 0.75f + 0.25f * (1 - dist / (B[k].r + 1));                                 /* brighter towards the middle */
            if(a <= 0) continue;
            float cr = B[k].c.red, cg = B[k].c.green, cb = B[k].c.blue;
            if(!d){ cr = cr + (255 - cr) * 0.45f; cg = cg + (255 - cg) * 0.45f; cb = cb + (255 - cb) * 0.45f; }   /* pastel */
            else { cr *= dim; cg *= dim; cb *= dim; }
            r += (cr - r) * a; g += (cg - g) * a; b += (cb - b) * a;
        }
        uint8_t R = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r), G = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g), Bb = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
        g_wall[y * WN + x] = 0xFF000000u | (uint32_t)R << 16 | (uint32_t)G << 8 | Bb;
        pl[y * WN + x] = R; pl[WN * WN + y * WN + x] = G; pl[2 * WN * WN + y * WN + x] = Bb;
    }
    for(int k = 0; k < 3; k++) blur_plane(pl + k * WN * WN, 14);
    int lift = d ? 30 : 140;                                                  /* frosted: lighter than what's behind */
    for(int i = 0; i < WN * WN; i++){
        uint32_t v = 0xFF000000u;
        for(int k = 0; k < 3; k++){ int c = pl[k * WN * WN + i]; c += (255 - c) * lift / 255; v |= (uint32_t)c << (16 - 8 * k); }
        g_frost[i] = v;
    }
    free(pl);
    dsc_init(&g_wall_dsc, g_wall); dsc_init(&g_frost_dsc, g_frost); g_wall_ok = 1;
}
void idisc_wallpaper(lv_obj_t *o){
    lv_obj_set_style_bg_color(o, TC(CANVAS), 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_NONE, 0);
    if(!th_id_glass()) return;
    wall_make();
    if(g_wall_ok){ lv_obj_set_style_bg_image_src(o, &g_wall_dsc, 0); lv_obj_set_style_bg_image_opa(o, LV_OPA_COVER, 0); }
}
/* the frost behind a glass surface, and its rim */
typedef struct { lv_image_dsc_t d; const lv_image_dsc_t *src; } glass_t;
static int glass_r(lv_obj_t *o){ lv_area_t a; lv_obj_get_coords(o, &a); int r = lv_obj_get_style_radius(o, 0), m = LV_MIN(lv_area_get_width(&a), lv_area_get_height(&a)) / 2; return r > m ? m : r; }
static void glass_under(lv_event_t *e){
    glass_t *g = lv_event_get_user_data(e); lv_obj_t *o = lv_event_get_target(e); lv_layer_t *l = lv_event_get_layer(e);
    if(!g || !g->src || !l || lv_obj_get_style_opa_recursive(o, 0) < LV_OPA_MIN) return;
    lv_area_t a, s = { 0, 0, WN - 1, WN - 1 }, b; lv_obj_get_coords(o, &a);
    if(!_lv_area_intersect(&b, &a, &s)) return;
    const lv_image_dsc_t *src = g->src; uint32_t st = src->header.stride;
    g->d = *src; g->d.header.w = (uint32_t)lv_area_get_width(&b); g->d.header.h = (uint32_t)lv_area_get_height(&b);
    g->d.data = src->data + (size_t)b.y1 * st + (size_t)b.x1 * 4; g->d.data_size = st * g->d.header.h;
    lv_draw_image_dsc_t id; lv_draw_image_dsc_init(&id); id.src = &g->d;
    /* The software renderer doesn't round an image's corners, so the frost goes down in bands of rows, each clipped to
     * the rounded shape (rows of equal width share a band; the tint and rim drawn over it smooth the steps). */
    int r = glass_r(o); lv_area_t keep = l->_clip_area;
    for(int y = b.y1; y <= b.y2; ){
        int in = 0, k0 = 0, y2 = y;
        for(;;){
            int top = a.y1 + r - y2, bot = y2 - (a.y2 - r), dy = top > 0 ? top : bot > 0 ? bot : 0, k = 0;
            if(dy > 0){ float t = dy - 0.5f; k = (int)(r - sqrtf((float)r * r - t * t) + 0.5f); }
            if(y2 == y) in = k0 = k; else if(abs(k - k0) > 1) break; else if(k > in) in = k;   /* (a pixel either way: one band) */
            if(++y2 > b.y2) break;
        }
        lv_area_t band = { a.x1 + in, y, a.x2 - in, y2 - 1 }, c;
        if(band.x1 <= band.x2 && _lv_area_intersect(&c, &band, &keep)){ l->_clip_area = c; lv_draw_image(l, &id, &b); }
        y = y2;
    }
    l->_clip_area = keep;
}
static void glass_rim(lv_event_t *e){
    glass_t *g = lv_event_get_user_data(e); lv_obj_t *o = lv_event_get_target(e); lv_layer_t *l = lv_event_get_layer(e);
    if(!g || !g->src || !l) return;
    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r); r.bg_opa = LV_OPA_TRANSP; r.radius = glass_r(o); r.border_width = 1;
    r.border_color = TC(FIXED_MEDIA_WHITE); r.border_opa = dark() ? 150 : 255; r.border_side = LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT;
    lv_draw_rect(l, &r, &a);                                                  /* light from the top left */
    r.border_color = TC(FIXED_MEDIA_BLACK); r.border_opa = dark() ? 60 : 24; r.border_side = LV_BORDER_SIDE_BOTTOM;
    lv_draw_rect(l, &r, &a);
}
static void glass_free(lv_event_t *e){ free(lv_event_get_user_data(e)); }
static glass_t *glass_of(lv_obj_t *o){
    for(uint32_t i = 0; i < lv_obj_get_event_count(o); i++){ lv_event_dsc_t *d = lv_obj_get_event_dsc(o, i);
        if(d && lv_event_dsc_get_cb(d) == glass_under) return lv_event_dsc_get_user_data(d); }
    glass_t *g = calloc(1, sizeof *g); if(!g) return NULL;
    lv_obj_add_event_cb(o, glass_under, LV_EVENT_DRAW_MAIN_BEGIN, g);
    lv_obj_add_event_cb(o, glass_rim, LV_EVENT_DRAW_MAIN_END, g);
    lv_obj_add_event_cb(o, glass_free, LV_EVENT_DELETE, g);
    return g;
}
/* A glass surface over src (a 360 px frost, screen-aligned; NULL = no frost, the tint alone). Tinted glass: solid. */
void idisc_glass_on(lv_obj_t *o, int radius, const lv_image_dsc_t *src){
    lv_obj_set_style_radius(o, radius, 0);
    if(th_id_glass()){
        int d = dark();
        glass_t *g = glass_of(o); if(g) g->src = src;
        lv_obj_set_style_bg_color(o, TC(FIXED_MEDIA_WHITE), 0); lv_obj_set_style_bg_grad_color(o, TC(FIXED_MEDIA_WHITE), 0);
        lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_opa(o, d ? 40 : 120, 0); lv_obj_set_style_bg_grad_opa(o, d ? 8 : 50, 0);   /* a sheen, more at the top */
        lv_obj_set_style_border_width(o, 1, 0); lv_obj_set_style_border_color(o, TC(FIXED_MEDIA_WHITE), 0);
        lv_obj_set_style_border_opa(o, d ? 40 : 140, 0);
    } else {
        glass_t *g = glass_of(o); if(g) g->src = NULL;
        lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_bg_color(o, TC(SURFACE), 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_opa(o, LV_OPA_COVER, 0); lv_obj_set_style_bg_grad_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(o, 0, 0);
    }
    lv_obj_set_style_bg_color(o, TC(FIXED_MEDIA_WHITE), LV_STATE_PRESSED); lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_NONE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(o, th_id_glass() ? (dark() ? 90 : 200) : LV_OPA_COVER, LV_STATE_PRESSED);
    if(!th_id_glass()){ lv_obj_set_style_bg_color(o, TC(SURFACE_PRESSED), LV_STATE_PRESSED); }
}
void idisc_glass(lv_obj_t *o, int radius){ if(th_id_glass()) wall_make(); idisc_glass_on(o, radius, g_wall_ok ? &g_frost_dsc : NULL); }
/* a solid surface (a lit tile): no frost under it */
void idisc_solid(lv_obj_t *o){ glass_t *g = glass_of(o); if(g) g->src = NULL; }
/* ---- transport glyphs, drawn (filled triangles and rounded bars, not a font) ------------------------------------- */
enum { G_PLAY, G_PAUSE, G_PREV, G_NEXT };
typedef struct { int kind, size; lv_color_t c; } glyph_t;
static void tri(lv_layer_t *l, lv_color_t c, float x0, float y0, float x1, float y1, float x2, float y2){
    lv_draw_triangle_dsc_t t; lv_draw_triangle_dsc_init(&t); t.bg_color = c; t.bg_opa = LV_OPA_COVER;
    t.p[0].x = x0; t.p[0].y = y0; t.p[1].x = x1; t.p[1].y = y1; t.p[2].x = x2; t.p[2].y = y2;
    lv_draw_triangle(l, &t);
}
static void glyph_draw(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e); glyph_t *g = lv_obj_get_user_data(o); lv_layer_t *l = lv_event_get_layer(e);
    if(!g || !l) return;
    lv_area_t a; lv_obj_get_coords(o, &a);
    float cx = (a.x1 + a.x2 + 1) / 2.0f, cy = (a.y1 + a.y2 + 1) / 2.0f, s = (float)g->size, h = s / 2;
    switch(g->kind){
    case G_PLAY: tri(l, g->c, cx - h * 0.78f, cy - h, cx - h * 0.78f, cy + h, cx + h * 0.95f, cy); break;
    case G_PAUSE: {
        lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r); r.bg_color = g->c; r.bg_opa = LV_OPA_COVER; r.radius = (int)(s / 10);
        int bw = (int)(s * 0.27f), gap = (int)(s * 0.2f);
        lv_area_t b1 = { (int)cx - gap / 2 - bw, (int)(cy - h), (int)cx - gap / 2 - 1, (int)(cy + h) - 1 };
        lv_area_t b2 = { (int)cx + gap / 2, (int)(cy - h), (int)cx + gap / 2 + bw - 1, (int)(cy + h) - 1 };
        lv_draw_rect(l, &r, &b1); lv_draw_rect(l, &r, &b2); break; }
    case G_NEXT: case G_PREV: {
        float dir = g->kind == G_NEXT ? 1 : -1, w = h * 0.95f;              /* two triangles nose to tail */
        tri(l, g->c, cx - dir * w, cy - h * 0.8f, cx - dir * w, cy + h * 0.8f, cx, cy);
        tri(l, g->c, cx, cy - h * 0.8f, cx, cy + h * 0.8f, cx + dir * w, cy); break; }
    }
}
static void glyph_free(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static lv_obj_t *glyph(lv_obj_t *p, int kind, int size, int box, lv_color_t c){
    lv_obj_t *o = plain(p); lv_obj_set_size(o, box, box);
    glyph_t *g = calloc(1, sizeof *g); if(g){ g->kind = kind; g->size = size; g->c = c; }
    lv_obj_set_user_data(o, g);
    lv_obj_add_event_cb(o, glyph_draw, LV_EVENT_DRAW_MAIN, NULL); lv_obj_add_event_cb(o, glyph_free, LV_EVENT_DELETE, NULL);
    return o;
}
static void glyph_set(lv_obj_t *o, int kind){ glyph_t *g = o ? lv_obj_get_user_data(o) : NULL; if(g && g->kind != kind){ g->kind = kind; lv_obj_invalidate(o); } }

/* ---- the album wash: the cover area-averaged to 40 x 40, box-blurred three times, stretched back to 360 --------- */
#define BL_N 40
typedef struct { uint32_t *px, *fr; lv_image_dsc_t d, fd; int ok; } wash_t;
static wash_t g_aw, g_pw;                                                  /* the album's; your picture's */
static unsigned g_wash_rev = ~0u;
/* the picture as a dark wash (dim/256) and as its frost (full brightness, lifted toward white) */
static int wash_make(wash_t *w, const lv_image_dsc_t *a, int dim){
    w->ok = 0;
    if(!a || !a->data || !a->header.w) return 0;
    int bpp = a->header.cf == LV_COLOR_FORMAT_RGB888 ? 3 : (a->header.cf == LV_COLOR_FORMAT_XRGB8888 || a->header.cf == LV_COLOR_FORMAT_ARGB8888) ? 4 : 0;
    if(!bpp) return 0;
    if(!w->px) w->px = malloc(360 * 360 * 4);
    if(!w->fr) w->fr = malloc(360 * 360 * 4);
    if(!w->px || !w->fr) return 0;
    int wd = (int)a->header.w, h = (int)a->header.h, st = a->header.stride ? (int)a->header.stride : wd * bpp;
    static int sm[3][BL_N * BL_N], tmp[BL_N * BL_N];
    for(int y = 0; y < BL_N; y++) for(int x = 0; x < BL_N; x++){
        int y0 = y * h / BL_N, y1 = (y + 1) * h / BL_N, x0 = x * wd / BL_N, x1 = (x + 1) * wd / BL_N;
        if(y1 <= y0) y1 = y0 + 1;
        if(x1 <= x0) x1 = x0 + 1;
        unsigned c[3] = {0, 0, 0}, n = 0;
        for(int yy = y0; yy < y1 && yy < h; yy++){ const uint8_t *q = a->data + (size_t)yy * st + (size_t)x0 * bpp;
            for(int xx = x0; xx < x1 && xx < wd; xx++, q += bpp){ c[0] += q[0]; c[1] += q[1]; c[2] += q[2]; n++; } }
        for(int k = 0; k < 3; k++) sm[k][y * BL_N + x] = n ? (int)(c[k] / n) : 0;
    }
    for(int k = 0; k < 3; k++) for(int pass = 0; pass < 3; pass++) for(int dir = 0; dir < 2; dir++){
        for(int y = 0; y < BL_N; y++) for(int x = 0; x < BL_N; x++){
            int sum = 0;
            for(int o = -2; o <= 2; o++){
                int xx = dir ? x : x + o, yy = dir ? y + o : y;
                xx = xx < 0 ? 0 : xx >= BL_N ? BL_N - 1 : xx; yy = yy < 0 ? 0 : yy >= BL_N ? BL_N - 1 : yy;
                sum += sm[k][yy * BL_N + xx];
            }
            tmp[y * BL_N + x] = sum / 5;
        }
        memcpy(sm[k], tmp, sizeof tmp);
    }
    for(int y = 0; y < 360; y++){
        int fy = (y * (BL_N - 1) * 256) / 359, iy = fy >> 8; fy &= 255; if(iy >= BL_N - 1){ iy = BL_N - 2; fy = 256; }
        for(int x = 0; x < 360; x++){
            int fx = (x * (BL_N - 1) * 256) / 359, ix = fx >> 8; fx &= 255; if(ix >= BL_N - 1){ ix = BL_N - 2; fx = 256; }
            uint32_t v = 0xFF000000u, f = 0xFF000000u;
            for(int k = 0; k < 3; k++){
                const int *m = sm[k] + iy * BL_N + ix;
                int top = m[0] * (256 - fx) + m[1] * fx, bot = m[BL_N] * (256 - fx) + m[BL_N + 1] * fx;
                int c = (top * (256 - fy) + bot * fy) >> 16;
                v |= (uint32_t)(c * dim >> 8) << (k * 8);
                f |= (uint32_t)(c + (255 - c) * 24 / 255) << (k * 8);
            }
            w->px[y * 360 + x] = v; w->fr[y * 360 + x] = f;
        }
    }
    dsc_init(&w->d, w->px); dsc_init(&w->fd, w->fr);
    return w->ok = 1;
}
/* your picture (Idle screen: Wallpaper / My picture / My picture, blurred) */
static const void *idle_picture(void){
    int m = cfg_get_int("nt_idle", 0);
    if(m <= 0) return NULL;
    const lv_image_dsc_t *p = lockcover_img();
    if(!p){ lockcover_request(); return NULL; }
    if(m == 1) return p;
    if(!g_pw.ok) wash_make(&g_pw, p, 210);
    return g_pw.ok ? (const void *)&g_pw.d : (const void *)p;
}

/* ---- the lock-screen face (Music with nothing playing, and standby) ------------------------------------------------
 * "Saturday 3 October" over a big clock; under it the weather; the wallpaper (or your picture) behind. */
typedef struct { lv_obj_t *root, *bg, *veil, *date, *clock, *wx, *extra; int clock_y; } face_t;
static char g_clock[16], g_wx[96];
static void face_build(face_t *f, lv_obj_t *parent, int y, const lv_font_t *clock_font){
    memset(f, 0, sizeof *f); f->root = parent; f->clock_y = y;
    idisc_wallpaper(parent);
    f->bg = lv_image_create(parent); lv_obj_remove_flag(f->bg, LV_OBJ_FLAG_CLICKABLE); lv_obj_set_pos(f->bg, 0, 0); show(f->bg, 0);
    f->veil = plain(parent); lv_obj_set_size(f->veil, 360, 360);
    lv_obj_set_style_bg_color(f->veil, TC(FIXED_MEDIA_BLACK), 0); lv_obj_set_style_bg_opa(f->veil, 70, 0); show(f->veil, 0);
    f->date = text(parent, TF(DATE), TC(TEXT_PRIMARY), 0); lv_obj_set_style_text_opa(f->date, 225, 0);
    lv_obj_align(f->date, LV_ALIGN_TOP_MID, 0, y - 34);
    f->clock = text(parent, clock_font, TC(TEXT_PRIMARY), 0); lv_label_set_text(f->clock, "--:--");
    lv_obj_align(f->clock, LV_ALIGN_TOP_MID, 0, y - 6);
    f->wx = text(parent, TF(UI_16), TC(TEXT_PRIMARY), 0); lv_obj_set_style_text_opa(f->wx, 200, 0);
    lv_obj_align(f->wx, LV_ALIGN_TOP_MID, 0, y + lv_font_get_line_height(clock_font) - 8);
}
static void face_colors(face_t *f, int photo){              /* on a photo the type is white, as on a phone */
    lv_color_t c = photo ? TC(FIXED_MEDIA_WHITE) : TC(TEXT_PRIMARY);
    lv_obj_set_style_text_color(f->date, c, 0); lv_obj_set_style_text_color(f->clock, c, 0); lv_obj_set_style_text_color(f->wx, c, 0);
}
static void face_picture(face_t *f){
    if(!f->bg) return;
    const void *src = idle_picture();
    lv_image_set_src(f->bg, NULL);
    show(f->bg, src != NULL); show(f->veil, src != NULL);
    if(src){ lv_image_set_src(f->bg, src); lv_obj_set_pos(f->bg, 0, 0); lv_obj_move_to_index(f->veil, 0); lv_obj_move_to_index(f->bg, 0); }
    face_colors(f, src != NULL);
}
static void face_clock(face_t *f, const char *t){
    if(!f->clock) return;
    if(t){ char c[16]; int n = 0; while(t[n] && n < 15 && t[n] != ' '){ c[n] = t[n]; n++; } c[n] = 0; set_text(f->clock, c); }
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char w[16], m[16], b[48]; strftime(w, sizeof w, "%A", &tm); strftime(m, sizeof m, "%B", &tm);
    snprintf(b, sizeof b, "%s %d %s", w, tm.tm_mday, m); set_text(f->date, b);
}
/* "14°C  Partly cloudy" with its icon -> "Partly cloudy  14°" (the icon glyph kept from the weather font) */
static void face_weather(face_t *f, const char *s){
    if(!f->wx) return;
    char out[96] = ""; const char *d = s ? strstr(s, "\xC2\xB0") : NULL;
    if(s && s[0]){
        char temp[16] = "", cond[64] = "";
        if(d){ const char *b = d; while(b > s && ((b[-1] >= '0' && b[-1] <= '9') || b[-1] == '-')) b--; snprintf(temp, sizeof temp, "%.*s\xC2\xB0", (int)(d - b), b); }
        const char *c = d ? d + 2 : s; if(d && (*c == 'C' || *c == 'F')) c++;
        while(*c == ' ') c++;
        if(!d){ while(*c && (unsigned char)*c >= 0x80) c++; while(*c == ' ') c++; }
        snprintf(cond, sizeof cond, "%s", c);
        if(cond[0] && temp[0]) snprintf(out, sizeof out, "%s  %s", cond, temp); else snprintf(out, sizeof out, "%s%s", cond, temp);
    }
    set_text(f->wx, out); show(f->wx, out[0] != 0);
}

/* ---- Music ------------------------------------------------------------------------------------------------------- */
#define ART   156                 /* the cover card, playing */
#define ART_P 132                 /* ... paused (it settles back a little) */
#define ART_CY 104
static lv_obj_t *g_layer, *g_wash_img, *g_veil, *g_card, *g_art, *g_note, *g_info, *g_title, *g_artist, *g_bar, *g_fill, *g_el, *g_rem;
static lv_obj_t *g_prev, *g_pp, *g_next, *g_pp_g, *g_idle, *g_start, *g_ctl;
static face_t g_face;
static lv_timer_t *g_tm;
static int g_have, g_playing, g_lyr_on, g_drag, g_card_d = ART;
static char g_t[160], g_a[160];
static lyr_line_t *g_lyr; static int g_lyr_n; static unsigned g_lyr_rev = ~0u;

static void ctl_glass(void){                                              /* (always the dark glass: the type is white) */
    if(!g_ctl) return;
    idisc_glass_on(g_ctl, LV_RADIUS_CIRCLE, g_aw.ok ? &g_aw.fd : NULL);
    if(th_id_glass()){ lv_obj_set_style_bg_main_opa(g_ctl, 46, 0); lv_obj_set_style_bg_grad_opa(g_ctl, 10, 0); lv_obj_set_style_border_opa(g_ctl, 50, 0); }
    else { lv_obj_set_style_bg_color(g_ctl, TC(FIXED_MEDIA_WHITE), 0); lv_obj_set_style_bg_opa(g_ctl, 36, 0); }
    lv_obj_set_style_bg_opa(g_ctl, 0, LV_STATE_PRESSED);
}
static void card_size(void *o, int32_t d){
    (void)o; g_card_d = (int)d;
    lv_obj_set_size(g_card, d, d); lv_obj_set_pos(g_card, 180 - d / 2, ART_CY - d / 2);
    const lv_image_dsc_t *c = lv_image_get_src(g_art);
    if(c && c->header.w) lv_image_set_scale(g_art, (uint32_t)(d * 256 / c->header.w) + 1);
    lv_obj_center(g_art);
}
static void card_breathe(int playing){                                    /* playing: full size; paused: a step back */
    int to = playing ? ART : ART_P; if(to == g_card_d) return;
    lv_anim_delete(g_card, card_size);
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, g_card); lv_anim_set_exec_cb(&a, card_size);
    lv_anim_set_values(&a, g_card_d, to); lv_anim_set_duration(&a, screen_get_anim() ? 320 : 1);
    lv_anim_set_path_cb(&a, playing ? lv_anim_path_overshoot : lv_anim_path_ease_out); lv_anim_start(&a);
}
static void art_apply(void){
    if(!g_layer) return;
    const lv_image_dsc_t *c = g_have ? ui_current_sharp_img() : NULL;
    lv_image_set_src(g_art, NULL);
    if(c){ lv_image_set_src(g_art, c); show(g_art, 1); show(g_note, 0); card_size(NULL, g_card_d); }
    else { show(g_art, 0); show(g_note, 1); }
    unsigned rev = ui_art_revision();
    if(!c){ g_wash_rev = ~0u; show(g_wash_img, 0); }
    else if(rev != g_wash_rev || !g_aw.ok){
        g_wash_rev = rev;
        if(wash_make(&g_aw, c, 150)){ lv_image_set_src(g_wash_img, NULL); lv_image_set_src(g_wash_img, &g_aw.d); show(g_wash_img, 1); }
        else show(g_wash_img, 0);
        ctl_glass(); lv_obj_invalidate(g_ctl);
    } else show(g_wash_img, 1);
}
static void lyrics_refresh(long pos){
    const char *line = NULL;
    if(g_lyr_on && g_have){
        unsigned rev = 0; int n0 = lyrics_immersive_load(NULL, 0, &rev);
        if(n0 < 0){ g_lyr_n = 0; g_lyr_rev = ~0u; }
        else if(rev != g_lyr_rev){
            if(!g_lyr) g_lyr = malloc(sizeof *g_lyr * 240);
            int n = g_lyr ? lyrics_immersive_load(g_lyr, 240, &rev) : -1;
            g_lyr_n = n > 0 ? n : 0; g_lyr_rev = rev;
        }
        for(int i = 0; i < g_lyr_n; i++){ if(g_lyr[i].ms <= pos + 300) line = g_lyr[i].text; else break; }
        if(line && !line[0]) line = NULL;
    }
    set_text(g_title, line ? line : g_t);
}
static void progress_paint(void){
    if(!g_layer) return;
    track_state_t st; ipc_get_state(&st);
    long pos = g_have ? st.position_ms : 0, dur = g_have ? st.duration_ms : 0;
    int frac = dur > 0 ? (int)((long long)pos * 1000 / dur) : 0; if(frac > 1000) frac = 1000; if(frac < 0) frac = 0;
    if(!g_drag) lv_obj_set_width(g_fill, 216 * frac / 1000);
    char e[24], r[24], rr[28]; fmt_time(pos, e, sizeof e); fmt_time(dur > pos ? dur - pos : 0, r, sizeof r);
    snprintf(rr, sizeof rr, "-%s", r);
    set_text(g_el, e); set_text(g_rem, rr);
    lyrics_refresh(pos);
}
static void view_apply(void){
    if(!g_layer) return;
    show(g_idle, !g_have);
    glyph_set(g_pp_g, g_playing ? G_PAUSE : G_PLAY);
    card_breathe(g_playing || !g_have);
    art_apply(); progress_paint();
}
static void tick_cb(lv_timer_t *t){ (void)t; if(screen_current() == SCR_HOME) progress_paint(); }

/* touch: the cover - tap for the lyrics, hold for the track actions, swipe sideways to skip, up for Immersive;
 * the title - tap for the track actions, hold for the Title hold; the bar - drag to seek; the buttons. */
static lv_point_t g_down; static uint32_t g_t0; static int g_moved;
static void card_cb(lv_event_t *e){
    lv_event_code_t c = lv_event_get_code(e); lv_indev_t *in = lv_indev_active(); lv_point_t p = { 0, 0 };
    if(in) lv_indev_get_point(in, &p);
    if(c == LV_EVENT_PRESSED){ g_down = p; g_t0 = lv_tick_get(); g_moved = 0; return; }
    if(c == LV_EVENT_PRESSING){ if(abs(p.x - g_down.x) > 10 || abs(p.y - g_down.y) > 10) g_moved = 1; return; }
    if(c == LV_EVENT_RELEASED){
        int dx = p.x - g_down.x, dy = p.y - g_down.y; uint32_t dt = lv_tick_elaps(g_t0);
        if(dt < 800 && abs(dx) >= 50 && abs(dx) > 2 * abs(dy)){
            if(dx < 0){ ui_transport_command("0201000C0001"); ui_toast_icon(LV_SYMBOL_NEXT, TC(ACCENT_PRIMARY), "Next"); }
            else { ui_transport_command("0201000C0002"); ui_toast_icon(LV_SYMBOL_PREV, TC(ACCENT_PRIMARY), "Previous"); }
        } else if(dt < 800 && dy <= -50 && -dy > 2 * abs(dx)) disco_open_np_immersive();
        return;
    }
    if(c == LV_EVENT_SHORT_CLICKED && !g_moved){ onboarding_discovered(HINT_MUSIC); nt_lyrics_open(); }
    else if(c == LV_EVENT_LONG_PRESSED && !g_moved) disco_track_overlay_open();
}
static void info_cb(lv_event_t *e){
    lv_event_code_t c = lv_event_get_code(e);
    if(c == LV_EVENT_SHORT_CLICKED){ onboarding_discovered(HINT_MUSIC); disco_track_overlay_open(); }
    else if(c == LV_EVENT_LONG_PRESSED){ int h = cfg_get_int("disco_title_hold", 0); if(h == 0) disco_open_np_immersive(); else if(h == 1) ui_np_fav_toggle(); }
}
static void seek_cb(lv_event_t *e){
    lv_event_code_t c = lv_event_get_code(e); lv_indev_t *in = lv_indev_active(); lv_point_t p;
    if(!in) return;
    lv_indev_get_point(in, &p);
    lv_area_t a; lv_obj_get_coords(g_bar, &a);
    int frac = (int)((p.x - a.x1) * 1000 / (a.x2 - a.x1 + 1)); if(frac < 0) frac = 0; if(frac > 1000) frac = 1000;
    if(c == LV_EVENT_PRESSED || c == LV_EVENT_PRESSING){
        if(!g_drag){ g_drag = 1; lv_obj_set_height(g_bar, 10); lv_obj_set_style_radius(g_bar, 5, 0); lv_obj_set_height(g_fill, 10); }
        lv_obj_set_width(g_fill, 216 * frac / 1000);
        track_state_t st; ipc_get_state(&st);
        if(st.duration_ms > 0){ char b[24]; fmt_time((long)((long long)st.duration_ms * frac / 1000), b, sizeof b); set_text(g_el, b); }
    } else if(c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST){
        g_drag = 0; lv_obj_set_height(g_bar, 6); lv_obj_set_style_radius(g_bar, 3, 0); lv_obj_set_height(g_fill, 6);
        track_state_t st; ipc_get_state(&st);
        if(c == LV_EVENT_RELEASED && st.have_track && st.duration_ms > 0) ui_seek_to((long)((long long)st.duration_ms * frac / 1000));
        progress_paint();
    }
}
static void btn_cb(lv_event_t *e){ ui_transport_command(lv_event_get_user_data(e)); }
static void start_cb(lv_event_t *e){ (void)e; screen_show(SCR_LIBRARY); }
static lv_obj_t *tbtn(lv_obj_t *p, int cx, int cy, int box, int kind, int size, const char *cmd){
    lv_obj_t *b = plain(p); lv_obj_set_size(b, box, box); lv_obj_set_pos(b, cx - box / 2, cy - box / 2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, TC(FIXED_MEDIA_WHITE), LV_STATE_PRESSED); lv_obj_set_style_bg_opa(b, 50, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE); lv_obj_set_ext_click_area(b, 6);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *)cmd);
    lv_obj_t *g = glyph(b, kind, size, box, TC(FIXED_MEDIA_WHITE)); lv_obj_center(g);
    return b;
}
void idisc_home_create(lv_obj_t *root){
    g_lyr_on = cfg_get_int("nt_lyrics", 1) != 0;
    for(lv_obj_t *o = root; o; o = lv_obj_get_parent(o)) lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    g_layer = plain(root); lv_obj_set_size(g_layer, 360, 360);
    lv_obj_set_style_bg_color(g_layer, TC(FIXED_MEDIA_BLACK), 0); lv_obj_set_style_bg_opa(g_layer, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_layer, LV_OBJ_FLAG_CLICKABLE);
    g_wash_img = lv_image_create(g_layer); lv_obj_remove_flag(g_wash_img, LV_OBJ_FLAG_CLICKABLE); show(g_wash_img, 0);
    g_wash_rev = ~0u;
    g_veil = plain(g_layer); lv_obj_set_size(g_veil, 360, 360);                  /* the bottom darker, for the controls */
    lv_obj_set_style_bg_color(g_veil, TC(FIXED_MEDIA_BLACK), 0); lv_obj_set_style_bg_grad_color(g_veil, TC(FIXED_MEDIA_BLACK), 0);
    lv_obj_set_style_bg_grad_dir(g_veil, LV_GRAD_DIR_VER, 0); lv_obj_set_style_bg_opa(g_veil, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_opa(g_veil, 20, 0); lv_obj_set_style_bg_grad_opa(g_veil, 120, 0);

    /* the cover card: a soft shadow, the art clipped to rounded corners, a note when there is no art */
    g_card_d = ART;
    g_card = plain(g_layer); lv_obj_set_size(g_card, ART, ART); lv_obj_set_pos(g_card, 180 - ART / 2, ART_CY - ART / 2);
    lv_obj_set_style_radius(g_card, 16, 0); lv_obj_set_style_clip_corner(g_card, true, 0);
    lv_obj_set_style_bg_color(g_card, TC(FIXED_MEDIA_WHITE), 0); lv_obj_set_style_bg_opa(g_card, 40, 0);
    lv_obj_add_flag(g_card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK); lv_obj_remove_flag(g_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(g_card, card_cb, LV_EVENT_ALL, NULL);
    g_art = lv_image_create(g_card); lv_obj_remove_flag(g_art, LV_OBJ_FLAG_CLICKABLE);
    g_note = text(g_card, theme_font_original(28), TC(FIXED_MEDIA_WHITE), 0); lv_label_set_text(g_note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_opa(g_note, 170, 0); lv_obj_center(g_note);

    /* title, artist */
    g_info = plain(g_layer); lv_obj_set_size(g_info, 280, 56); lv_obj_set_pos(g_info, 40, 190);
    lv_obj_add_flag(g_info, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(g_info, info_cb, LV_EVENT_ALL, NULL);
    g_title = text(g_info, ui_font_cjk(20), TC(FIXED_MEDIA_WHITE), 250); lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_text_font(g_title, TF(MUSIC_TITLE), 0);
    g_artist = text(g_info, ui_font_cjk(16), TC(FIXED_MEDIA_WHITE), 230); lv_obj_align(g_artist, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_text_opa(g_artist, 165, 0);

    /* the scrubber and its times */
    lv_obj_t *seek = plain(g_layer); lv_obj_set_size(seek, 236, 30); lv_obj_set_pos(seek, 62, 244);
    lv_obj_add_flag(seek, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(seek, seek_cb, LV_EVENT_ALL, NULL);
    g_bar = plain(seek); lv_obj_set_size(g_bar, 216, 6); lv_obj_center(g_bar);
    lv_obj_set_style_radius(g_bar, 3, 0); lv_obj_set_style_clip_corner(g_bar, true, 0);
    lv_obj_set_style_bg_color(g_bar, TC(FIXED_MEDIA_WHITE), 0); lv_obj_set_style_bg_opa(g_bar, 64, 0);
    g_fill = plain(g_bar); lv_obj_set_size(g_fill, 0, 6); lv_obj_align(g_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(g_fill, TC(FIXED_MEDIA_WHITE), 0); lv_obj_set_style_bg_opa(g_fill, 235, 0);
    g_el = text(g_layer, TF(UI_12), TC(FIXED_MEDIA_WHITE), 0); lv_obj_set_style_text_opa(g_el, 160, 0);
    lv_obj_set_style_text_align(g_el, LV_TEXT_ALIGN_LEFT, 0); lv_obj_set_width(g_el, 80); lv_obj_set_pos(g_el, 72, 270);
    g_rem = text(g_layer, TF(UI_12), TC(FIXED_MEDIA_WHITE), 0); lv_obj_set_style_text_opa(g_rem, 160, 0);
    lv_obj_set_style_text_align(g_rem, LV_TEXT_ALIGN_RIGHT, 0); lv_obj_set_width(g_rem, 80); lv_obj_set_pos(g_rem, 208, 270);

    /* previous / play-pause / next, on a glass capsule over the album wash */
    g_ctl = plain(g_layer); lv_obj_set_size(g_ctl, 216, 62); lv_obj_set_pos(g_ctl, 72, 283);
    ctl_glass();
    g_prev = tbtn(g_layer, 112, 314, 54, G_PREV, 22, "0201000C0002");
    g_pp = tbtn(g_layer, 180, 314, 62, G_PLAY, 30, "0201000C0000"); g_pp_g = lv_obj_get_child(g_pp, 0);
    g_next = tbtn(g_layer, 248, 314, 54, G_NEXT, 22, "0201000C0001");

    /* nothing playing: the lock-screen face and a glass "Play music" button */
    g_idle = plain(g_layer); lv_obj_set_size(g_idle, 360, 360); lv_obj_add_flag(g_idle, LV_OBJ_FLAG_CLICKABLE);
    face_build(&g_face, g_idle, 122, TF(CLOCK));
    g_start = plain(g_idle); lv_obj_set_size(g_start, 176, 48); lv_obj_align(g_start, LV_ALIGN_TOP_MID, 0, 262);
    idisc_glass(g_start, LV_RADIUS_CIRCLE); lv_obj_add_flag(g_start, LV_OBJ_FLAG_CLICKABLE); lv_obj_set_ext_click_area(g_start, 8);
    lv_obj_add_event_cb(g_start, start_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *row = plain(g_start); lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT); lv_obj_center(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    glyph(row, G_PLAY, 14, 18, TC(TEXT_PRIMARY));
    lv_obj_t *sl = text(row, TF(HEADER_SM), TC(TEXT_PRIMARY), 0); lv_label_set_text(sl, "Play music");

    keep_all(g_layer);
    if(g_tm) lv_timer_delete(g_tm);
    g_tm = lv_timer_create(tick_cb, 500, NULL);
    face_clock(&g_face, g_clock[0] ? g_clock : NULL); face_weather(&g_face, g_wx); face_picture(&g_face);
    set_text(g_title, g_t); set_text(g_artist, g_a);
    lockcover_set_listener(idisc_pictures_apply);
    view_apply();
}
void idisc_pictures_apply(void){ g_pw.ok = 0; face_picture(&g_face); idisc_saver_pictures(); }
void idisc_home_now_playing(const char *title, const char *artist, int playing){
    int have = title && title[0];
    int changed = have != g_have || !!playing != g_playing || strcmp(g_t, title ? title : "") || strcmp(g_a, artist ? artist : "");
    g_have = have; g_playing = !!playing;
    snprintf(g_t, sizeof g_t, "%s", title ? title : ""); snprintf(g_a, sizeof g_a, "%s", artist ? artist : "");
    if(!g_layer || !changed) return;
    set_text(g_title, g_t); set_text(g_artist, g_a);
    view_apply();
}
const lv_image_dsc_t *idisc_wash(void){ return g_aw.ok ? &g_aw.d : NULL; }
void idisc_home_art_changed(void){ g_wash_rev = ~0u; art_apply(); }
void idisc_home_lyrics_apply(int on){ g_lyr_on = on; g_lyr_rev = ~0u; progress_paint(); }
void idisc_home_clock(const char *t){ if(t) snprintf(g_clock, sizeof g_clock, "%s", t); face_clock(&g_face, t); idisc_saver_clock(t); }
void idisc_home_weather(const char *s){ snprintf(g_wx, sizeof g_wx, "%s", s ? s : ""); face_weather(&g_face, s); }
int idisc_home_owns_point(int x, int y){
    if(!g_layer || !g_have) return 0;
    int r = g_card_d / 2;
    if(abs(x - 180) <= r && abs(y - ART_CY) <= r) return 1;
    return y >= 244 && y <= 344 && x >= 60 && x <= 300;                  /* the scrubber and the buttons */
}

/* ---- standby ----------------------------------------------------------------------------------------------------- */
static face_t g_sface;
static lv_obj_t *g_scard, *g_sart, *g_stitle, *g_sartist, *g_sstat, *g_sbat, *g_sbat_fill, *g_spct;
static int g_st_b = -1, g_st_c;
static void sstat_paint(void){
    if(!g_sstat) return;
    show(g_sstat, g_st_b >= 0);
    if(g_st_b < 0) return;
    int b = g_st_b > 100 ? 100 : g_st_b;
    lv_obj_set_width(g_sbat_fill, 18 * b / 100 < 2 ? 2 : 18 * b / 100);
    lv_obj_set_style_bg_color(g_sbat_fill, g_st_c ? TC(STATUS_SUCCESS) : b <= 20 ? TC(STATUS_DANGER) : lv_obj_get_style_text_color(g_spct, 0), 0);
    char p[16]; snprintf(p, sizeof p, "%d%%", b); set_text(g_spct, p);
}
void idisc_saver_create(lv_obj_t *root){
    lv_obj_set_style_bg_image_src(root, NULL, 0);
    face_build(&g_sface, root, 104, TF(SAVER_CLOCK));
    /* a small status line at the top: battery */
    g_sstat = plain(root); lv_obj_set_size(g_sstat, LV_SIZE_CONTENT, 16); lv_obj_align(g_sstat, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_flex_flow(g_sstat, LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(g_sstat, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_sstat, 6, 0);
    g_spct = text(g_sstat, TF(UI_12), TC(TEXT_PRIMARY), 0);
    g_sbat = plain(g_sstat); lv_obj_set_size(g_sbat, 26, 12); lv_obj_set_style_radius(g_sbat, 4, 0);
    lv_obj_set_style_border_width(g_sbat, 1, 0); lv_obj_set_style_border_color(g_sbat, TC(TEXT_PRIMARY), 0); lv_obj_set_style_border_opa(g_sbat, 120, 0);
    lv_obj_set_style_pad_all(g_sbat, 2, 0);
    g_sbat_fill = plain(g_sbat); lv_obj_set_size(g_sbat_fill, 18, 6); lv_obj_set_style_radius(g_sbat_fill, 2, 0);
    lv_obj_set_style_bg_opa(g_sbat_fill, LV_OPA_COVER, 0); lv_obj_align(g_sbat_fill, LV_ALIGN_LEFT_MID, 0, 0);
    /* the Now Playing card */
    g_scard = plain(root); lv_obj_set_size(g_scard, 248, 64); lv_obj_align(g_scard, LV_ALIGN_TOP_MID, 0, 250);
    idisc_glass(g_scard, 22);
    lv_obj_t *ab = plain(g_scard); lv_obj_set_size(ab, 44, 44); lv_obj_align(ab, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_radius(ab, 9, 0); lv_obj_set_style_clip_corner(ab, true, 0);
    lv_obj_set_style_bg_color(ab, TC(SURFACE_RAISED), 0); lv_obj_set_style_bg_opa(ab, LV_OPA_COVER, 0);
    g_sart = lv_image_create(ab); lv_obj_remove_flag(g_sart, LV_OBJ_FLAG_CLICKABLE);
    g_stitle = text(g_scard, TF(HEADER_SM), TC(TEXT_PRIMARY), 170); lv_obj_set_style_text_align(g_stitle, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(g_stitle, LV_LABEL_LONG_DOT); lv_obj_align(g_stitle, LV_ALIGN_TOP_LEFT, 64, 11);
    lv_obj_set_style_text_font(g_stitle, ui_font_cjk(16), 0);
    g_sartist = text(g_scard, ui_font_cjk(14), TC(TEXT_SECONDARY), 170); lv_obj_set_style_text_align(g_sartist, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(g_sartist, LV_LABEL_LONG_DOT); lv_obj_align(g_sartist, LV_ALIGN_TOP_LEFT, 64, 33);
    show(g_scard, 0);
    face_clock(&g_sface, g_clock[0] ? g_clock : NULL); face_weather(&g_sface, g_wx); idisc_saver_pictures();
    sstat_paint();
    keep_all(root);
}
void idisc_saver_pictures(void){
    if(!g_sface.root) return;
    face_picture(&g_sface);
    if(g_spct){ lv_color_t c = lv_obj_get_style_text_color(g_sface.clock, 0);
        lv_obj_set_style_text_color(g_spct, c, 0); lv_obj_set_style_border_color(g_sbat, c, 0); sstat_paint(); }
}
void idisc_saver_clock(const char *t){ face_clock(&g_sface, t); }
void idisc_saver_weather(const char *s){ face_weather(&g_sface, s); }
void idisc_saver_track(const char *title, const char *artist){
    if(!g_scard) return;
    int have = title && title[0];
    set_text(g_stitle, title); set_text(g_sartist, artist);
    const lv_image_dsc_t *c = have ? ui_current_sharp_img() : NULL;
    lv_image_set_src(g_sart, NULL);
    if(c){ lv_image_set_src(g_sart, c); if(c->header.w) lv_image_set_scale(g_sart, (uint32_t)(44 * 256 / c->header.w) + 1); lv_obj_center(g_sart); }
    show(g_scard, have);
    show(g_sface.wx, !have && lv_label_get_text(g_sface.wx)[0]);           /* the card takes the weather's place */
}
void idisc_status(int batt, int charging){ g_st_b = batt; g_st_c = charging; sstat_paint(); }

/* ---- Control Center ------------------------------------------------------------------------------------------------
 * A media card (cover, title, play-pause, next), eight round toggles in two rows (blue when on; hold one for its
 * settings), a wide brightness capsule, the battery in the top line, a handle to close. */
static lv_obj_t *q_root, *q_panel, *q_top, *q_tile[8], *q_icon[8], *q_media, *q_art, *q_mt, *q_ma, *q_pp, *q_pp_g, *q_bright, *q_cap;
static int q_ids[8], q_n, q_open, q_batt = -1, q_chg;
static lv_timer_t *q_tm;
static void q_tile_cb(lv_event_t *e){
    int k = (int)(intptr_t)lv_event_get_user_data(e); if(k < 0 || k >= q_n) return;
    lv_event_code_t c = lv_event_get_code(e);
    if(c == LV_EVENT_LONG_PRESSED) quicksettings_null_action(q_ids[k], 1);
    else if(c == LV_EVENT_CLICKED && !screen_press_was_long()){ quicksettings_null_action(q_ids[k], 0); idisc_qs_refresh(); }
}
static void q_transport_cb(lv_event_t *e){ ui_transport_command(lv_event_get_user_data(e)); idisc_qs_refresh(); }
static void q_bright_cb(lv_event_t *e){
    int v = lv_slider_get_value(lv_event_get_target(e));
    if(lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) ui_backlight(v); else ui_set_brightness(v);
}
static void q_close_cb(lv_event_t *e){ (void)e; screen_back(); }
static void q_tick(lv_timer_t *t){ (void)t; if(screen_current() == SCR_QUICK) idisc_qs_refresh(); else lv_timer_pause(q_tm); }
static const lv_font_t *icon_font(void){
    static lv_font_t f, g; extern const lv_font_t font_weather16;
    if(!f.get_glyph_dsc){ g = *TF(ICON_20); g.fallback = &font_weather16; f = *theme_font_original(18); f.fallback = &g; }
    return &f;
}
static void qs_construct(void){
    if(q_panel) return;
    q_panel = plain(q_root); lv_obj_set_size(q_panel, 360, 360); lv_obj_set_pos(q_panel, 0, -360);
    lv_obj_set_style_bg_color(q_panel, TC(CANVAS), 0); lv_obj_set_style_bg_opa(q_panel, LV_OPA_COVER, 0);
    if(th_id_glass()){ wall_make(); if(g_wall_ok){ lv_obj_set_style_bg_image_src(q_panel, &g_wall_dsc, 0);     /* the wallpaper, dimmed */
        lv_obj_set_style_bg_image_recolor(q_panel, TC(CANVAS), 0); lv_obj_set_style_bg_image_recolor_opa(q_panel, dark() ? 110 : 90, 0); } }
    lv_obj_add_flag(q_panel, LV_OBJ_FLAG_CLICKABLE);
    q_top = text(q_panel, TF(UI_14), TC(TEXT_PRIMARY), 0); lv_obj_align(q_top, LV_ALIGN_TOP_MID, 0, 22);
    /* media */
    q_media = plain(q_panel); lv_obj_set_size(q_media, 244, 70); lv_obj_set_pos(q_media, 58, 46); idisc_glass(q_media, 24);
    lv_obj_t *ab = plain(q_media); lv_obj_set_size(ab, 50, 50); lv_obj_align(ab, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_radius(ab, 10, 0); lv_obj_set_style_clip_corner(ab, true, 0);
    lv_obj_set_style_bg_color(ab, TC(SURFACE_RAISED), 0); lv_obj_set_style_bg_opa(ab, LV_OPA_COVER, 0);
    q_art = lv_image_create(ab); lv_obj_remove_flag(q_art, LV_OBJ_FLAG_CLICKABLE);
    q_mt = text(q_media, ui_font_cjk(14), TC(TEXT_PRIMARY), 108); lv_obj_set_style_text_align(q_mt, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(q_mt, LV_LABEL_LONG_DOT); lv_obj_align(q_mt, LV_ALIGN_TOP_LEFT, 70, 16);
    q_ma = text(q_media, ui_font_cjk(14), TC(TEXT_SECONDARY), 108); lv_obj_set_style_text_align(q_ma, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(q_ma, LV_LABEL_LONG_DOT); lv_obj_align(q_ma, LV_ALIGN_TOP_LEFT, 70, 36);
    q_pp = plain(q_media); lv_obj_set_size(q_pp, 40, 40); lv_obj_align(q_pp, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_radius(q_pp, LV_RADIUS_CIRCLE, 0); lv_obj_add_flag(q_pp, LV_OBJ_FLAG_CLICKABLE); lv_obj_set_ext_click_area(q_pp, 6);
    lv_obj_set_style_bg_color(q_pp, TC(SURFACE_PRESSED), LV_STATE_PRESSED); lv_obj_set_style_bg_opa(q_pp, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(q_pp, q_transport_cb, LV_EVENT_CLICKED, (void *)"0201000C0000");
    q_pp_g = glyph(q_pp, G_PLAY, 18, 40, TC(TEXT_PRIMARY)); lv_obj_center(q_pp_g);
    /* toggles: 4 x 2 circles */
    for(int k = 0; k < 8; k++){
        int col = k % 4, row = k / 4;
        lv_obj_t *t = q_tile[k] = plain(q_panel); lv_obj_set_size(t, 52, 52); lv_obj_set_pos(t, 58 + col * 64, 128 + row * 62);
        lv_obj_set_style_radius(t, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, q_tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k); lv_obj_add_event_cb(t, q_tile_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)k);
        q_icon[k] = text(t, icon_font(), TC(TEXT_PRIMARY), 0); lv_obj_center(q_icon[k]);
    }
    /* brightness: a wide capsule that fills white, a sun at its left end */
    q_bright = lv_slider_create(q_panel); lv_obj_remove_style_all(q_bright);
    lv_obj_set_size(q_bright, 244, 44); lv_obj_set_pos(q_bright, 58, 256); lv_obj_set_ext_click_area(q_bright, 8);
    lv_slider_set_range(q_bright, 4, 40); lv_slider_set_value(q_bright, ui_get_brightness(), LV_ANIM_OFF);
    idisc_glass(q_bright, 22); lv_obj_set_style_clip_corner(q_bright, true, 0);
    if(!dark()){ lv_obj_set_style_bg_grad_dir(q_bright, LV_GRAD_DIR_NONE, 0); lv_obj_set_style_bg_color(q_bright, TC(SURFACE_STRONG), 0);
                 lv_obj_set_style_bg_main_opa(q_bright, LV_OPA_COVER, 0); }   /* light: grey, so the white fill shows */
    lv_obj_set_style_radius(q_bright, 22, LV_PART_INDICATOR); lv_obj_set_style_bg_color(q_bright, TC(FIXED_MEDIA_WHITE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(q_bright, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_add_event_cb(q_bright, q_bright_cb, LV_EVENT_VALUE_CHANGED, NULL); lv_obj_add_event_cb(q_bright, q_bright_cb, LV_EVENT_RELEASED, NULL);
    extern const lv_font_t font_weather16;
    q_cap = text(q_panel, &font_weather16, TC(FIXED_MEDIA_BLACK), 0); lv_obj_set_style_text_opa(q_cap, 150, 0); lv_label_set_text(q_cap, "\xEF\x86\x85"); lv_obj_set_pos(q_cap, 74, 270);
    /* the handle */
    lv_obj_t *h = plain(q_panel); lv_obj_set_size(h, 90, 30); lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 316);
    lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(h, q_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bar = plain(h); lv_obj_set_size(bar, 40, 5); lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_radius(bar, 3, 0); lv_obj_set_style_bg_color(bar, TC(TEXT_PRIMARY), 0); lv_obj_set_style_bg_opa(bar, 140, 0);
    keep_all(q_panel);
}
void idisc_qs_create(lv_obj_t *root){
    q_root = root; q_panel = NULL;
    lv_obj_set_style_bg_opa(root, 0, 0); lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    qs_construct();
    if(!q_tm){ q_tm = lv_timer_create(q_tick, 1000, NULL); lv_timer_pause(q_tm); }
    idisc_qs_build();
}
void idisc_qs_build(void){
    if(!q_root) return;
    qs_construct();
    int items[QS_MAX_ITEMS]; q_n = quicksettings_items_load(items); if(q_n > 8) q_n = 8;
    for(int k = 0; k < 8; k++){
        q_ids[k] = k < q_n ? items[k] : -1;
        show(q_tile[k], k < q_n);
        if(k < q_n) set_text(q_icon[k], quicksettings_item_glyph(q_ids[k]));
    }
    idisc_qs_refresh();
}
void idisc_qs_refresh(void){
    if(!q_panel) return;
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char b[64], d[32]; int h24 = cfg_get_int("time_24h", 1);
    strftime(d, sizeof d, "%a %d %b", &tm);
    int hh = h24 ? tm.tm_hour : (tm.tm_hour % 12 ? tm.tm_hour % 12 : 12);
    if(q_batt >= 0) snprintf(b, sizeof b, "%d:%02d   %s   %s%d%%", hh, tm.tm_min, d, q_chg ? LV_SYMBOL_CHARGE " " : "", q_batt);
    else snprintf(b, sizeof b, "%d:%02d   %s", hh, tm.tm_min, d);
    set_text(q_top, b);
    for(int k = 0; k < q_n; k++){
        int on = quicksettings_item_active(q_ids[k]);
        if(on){ idisc_solid(q_tile[k]); lv_obj_set_style_bg_color(q_tile[k], TC(ACCENT_PRIMARY), 0); lv_obj_set_style_bg_grad_dir(q_tile[k], LV_GRAD_DIR_NONE, 0);
                lv_obj_set_style_bg_opa(q_tile[k], LV_OPA_COVER, 0); lv_obj_set_style_bg_main_opa(q_tile[k], LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(q_tile[k], 0, 0); lv_obj_set_style_text_color(q_icon[k], TC(ON_ACCENT), 0);
                lv_obj_set_style_bg_color(q_tile[k], TC(PRIMARY_PRESSED), LV_STATE_PRESSED); }
        else { idisc_glass(q_tile[k], LV_RADIUS_CIRCLE); lv_obj_set_style_text_color(q_icon[k], TC(TEXT_PRIMARY), 0); }
    }
    track_state_t st; ipc_get_state(&st);
    if(st.have_track && st.title[0]){ set_text(q_mt, st.title); set_text(q_ma, st.artist); }
    else { set_text(q_mt, "Not Playing"); set_text(q_ma, ""); }
    glyph_set(q_pp_g, ui_is_playing() ? G_PAUSE : G_PLAY);
    if(q_bright && !lv_obj_has_state(q_bright, LV_STATE_PRESSED)) lv_slider_set_value(q_bright, ui_get_brightness(), LV_ANIM_OFF);
}
void idisc_qs_art(const void *cover){
    if(!q_art) return;
    lv_image_set_src(q_art, NULL);
    if(cover){ const lv_image_dsc_t *d = cover; lv_image_set_src(q_art, cover);
        if(d->header.w) lv_image_set_scale(q_art, (uint32_t)(50 * 256 / d->header.w) + 1);
        lv_obj_center(q_art); show(q_art, 1); }
    else show(q_art, 0);
}
void idisc_qs_battery(int pct, int charging){ if(pct < 0) return; q_batt = pct > 100 ? 100 : pct; q_chg = charging; idisc_qs_refresh(); }
static void q_pos(void *o, int32_t v){ lv_obj_set_y(o, v); }
static void q_hidden(lv_anim_t *a){ (void)a; if(!q_open && q_root) lv_obj_add_flag(q_root, LV_OBJ_FLAG_HIDDEN); }
void idisc_qs_slide(int open){
    if(!q_panel) return;
    q_open = !!open;
    if(open){ idisc_qs_refresh(); lv_timer_resume(q_tm); }
    lv_anim_delete(q_panel, q_pos);
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, q_panel); lv_anim_set_exec_cb(&a, q_pos);
    lv_anim_set_values(&a, lv_obj_get_y(q_panel), open ? 0 : -360);
    lv_anim_set_duration(&a, screen_get_anim() ? 260 : 1); lv_anim_set_path_cb(&a, open ? lv_anim_path_overshoot : lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, q_hidden); lv_anim_start(&a);
}

/* ---- volume: a glass capsule that drops in at the top ---------------------------------------------------------- */
static lv_obj_t *v_root, *v_card, *v_bar, *v_fill, *v_icon, *v_num;
static lv_timer_t *v_tm;
static int v_val, v_pending, v_closing; static uint32_t v_sent;
static int vmax(void){ int m = simple_cfg_get_int("max_vol", VOL_MAX); if(m < 1) m = 1; if(m > VOL_MAX) m = VOL_MAX; return m; }
static void v_paint(void){
    char b[8]; snprintf(b, sizeof b, "%d", v_val); set_text(v_num, b);
    set_text(v_icon, v_val == 0 ? LV_SYMBOL_MUTE : v_val < vmax() / 2 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
    int w = 132, f = v_val * w / vmax();                                 /* (the bar's width, fixed: no layout needed) */ lv_obj_set_width(v_fill, f < 0 ? 0 : f > w ? w : f);
}
static void v_close(void);
static void v_bar_cb(lv_event_t *e){
    lv_event_code_t c = lv_event_get_code(e); lv_indev_t *in = lv_indev_active(); if(!in) return;
    lv_point_t p; lv_indev_get_point(in, &p);
    lv_area_t a; lv_obj_get_coords(v_bar, &a);
    int w = lv_area_get_width(&a), v = (int)((long)(p.x - a.x1) * vmax() / (w > 1 ? w : 1));
    if(v < 0) v = 0;
    if(v > vmax()) v = vmax();
    if(c == LV_EVENT_PRESSED || c == LV_EVENT_PRESSING){
        if(v != v_val){ v_val = v; v_paint(); if(lv_tick_elaps(v_sent) >= 120){ ui_set_volume(v); v_sent = lv_tick_get(); v_pending = 0; } else v_pending = 1; }
    } else if((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && v_pending){ ui_set_volume(v_val); v_sent = lv_tick_get(); v_pending = 0; }
    lv_timer_reset(v_tm);
}
static void v_outside_cb(lv_event_t *e){ if(lv_event_get_target(e) == v_root) v_close(); }
static void v_anim(void *o, int32_t v){ (void)o; lv_obj_set_style_opa(v_card, (lv_opa_t)v, 0); lv_obj_set_y(v_card, 34 - (255 - v) * 30 / 255); }
static void v_hidden(lv_anim_t *a){ (void)a; v_closing = 0; lv_obj_add_flag(v_root, LV_OBJ_FLAG_HIDDEN); }
static void v_close(void){
    if(!v_root || v_closing || lv_obj_has_flag(v_root, LV_OBJ_FLAG_HIDDEN)) return;
    lv_timer_pause(v_tm); v_closing = 1;
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, v_root); lv_anim_set_exec_cb(&a, v_anim);
    lv_anim_set_values(&a, 255, 0); lv_anim_set_duration(&a, screen_get_anim() ? 160 : 1);
    lv_anim_set_completed_cb(&a, v_hidden); lv_anim_start(&a);
}
static void v_timeout(lv_timer_t *t){ if(v_bar && lv_obj_has_state(v_bar, LV_STATE_PRESSED)){ lv_timer_reset(t); return; } v_close(); }
void idisc_volume_show(int vol){
    if(!v_root){
        v_root = plain(lv_layer_top()); lv_obj_set_size(v_root, 360, 360);
        lv_obj_add_flag(v_root, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(v_root, v_outside_cb, LV_EVENT_CLICKED, NULL);
        v_card = plain(v_root); lv_obj_set_size(v_card, 236, 52); lv_obj_set_pos(v_card, 62, 34);
        idisc_glass(v_card, LV_RADIUS_CIRCLE);
        if(th_id_glass()){ lv_obj_set_style_bg_color(v_card, TC(SURFACE_RAISED), 0); lv_obj_set_style_bg_grad_color(v_card, TC(SURFACE), 0);
                           lv_obj_set_style_bg_main_opa(v_card, 245, 0); lv_obj_set_style_bg_grad_opa(v_card, 235, 0); }
        lv_obj_add_flag(v_card, LV_OBJ_FLAG_CLICKABLE);
        v_icon = text(v_card, theme_font_original(16), TC(TEXT_PRIMARY), 0); lv_obj_align(v_icon, LV_ALIGN_LEFT_MID, 18, 0);
        v_bar = plain(v_card); lv_obj_set_size(v_bar, 132, 10); lv_obj_align(v_bar, LV_ALIGN_LEFT_MID, 50, 0);
        lv_obj_set_style_radius(v_bar, 5, 0); lv_obj_set_style_clip_corner(v_bar, true, 0);
        lv_obj_set_style_bg_color(v_bar, TC(CONTROL_TRACK_STRONG), 0); lv_obj_set_style_bg_opa(v_bar, LV_OPA_COVER, 0);
        lv_obj_add_flag(v_bar, LV_OBJ_FLAG_CLICKABLE); lv_obj_set_ext_click_area(v_bar, 18);
        v_fill = plain(v_bar); lv_obj_set_size(v_fill, 0, 10); lv_obj_set_style_bg_color(v_fill, TC(TEXT_PRIMARY), 0); lv_obj_set_style_bg_opa(v_fill, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(v_bar, v_bar_cb, LV_EVENT_PRESSED, NULL); lv_obj_add_event_cb(v_bar, v_bar_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(v_bar, v_bar_cb, LV_EVENT_RELEASED, NULL); lv_obj_add_event_cb(v_bar, v_bar_cb, LV_EVENT_PRESS_LOST, NULL);
        v_num = text(v_card, TF(HEADER_SM), TC(TEXT_PRIMARY), 0); lv_obj_set_width(v_num, 40);
        lv_obj_set_style_text_align(v_num, LV_TEXT_ALIGN_RIGHT, 0); lv_obj_align(v_num, LV_ALIGN_RIGHT_MID, -16, 0);
        v_tm = lv_timer_create(v_timeout, 2500, NULL); lv_timer_pause(v_tm);
        lv_obj_add_flag(v_root, LV_OBJ_FLAG_HIDDEN);
        keep_all(v_root);
    }
    if(vol < 0) vol = 0;
    if(vol > VOL_MAX) vol = VOL_MAX;
    if(!lv_obj_has_state(v_bar, LV_STATE_PRESSED)){ v_val = vol; v_paint(); }
    int hidden = lv_obj_has_flag(v_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(v_root, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(v_root);
    if(hidden || v_closing){
        lv_anim_delete(v_root, NULL); v_closing = 0;
        lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, v_root); lv_anim_set_exec_cb(&a, v_anim);
        lv_anim_set_values(&a, hidden ? 0 : lv_obj_get_style_opa(v_card, 0), 255); lv_anim_set_duration(&a, screen_get_anim() ? 220 : 1);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out); lv_anim_start(&a);
    }
    lv_timer_reset(v_tm); lv_timer_resume(v_tm);
}
int idisc_volume_visible(void){ return v_root && !lv_obj_has_flag(v_root, LV_OBJ_FLAG_HIDDEN); }
lv_obj_t *idisc_volume_root(void){ return v_root; }

/* ---- the kit ------------------------------------------------------------------------------------------------------
 * Titles: bold, centred, in the text colour. Back: a glass circle with a chevron. Rows: glass cards with rounded
 * corners; a row's icon sits in a small rounded square of a system colour, white on top. Chevrons quiet. */
void idisc_title_style(lv_obj_t *l){
    if(!l) return;
    lv_obj_set_style_text_font(l, TF(HEADER), 0);
    lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_letter_space(l, 0, 0);
}
static lv_obj_t *id_header(lv_obj_t *root, const char *title, lv_event_cb_t back_cb){
    lv_obj_t *t = theme_kit_default.header(root, title, back_cb);
    lv_obj_t *back = lv_obj_get_child(root, lv_obj_get_index(t) - 1);
    if(back){
        lv_obj_set_pos(back, 66, 26); lv_obj_set_size(back, 38, 38);
        idisc_glass(back, LV_RADIUS_CIRCLE);
        lv_obj_t *ic = lv_obj_get_child(back, 0);
        if(ic){ lv_obj_set_style_text_font(ic, theme_font_original(16), 0); lv_obj_set_style_text_color(ic, TC(TEXT_PRIMARY), 0); lv_obj_center(ic); }
        kit_keep(back);
    }
    lv_obj_set_pos(t, 108, 34); lv_obj_set_size(t, 144, 26);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    idisc_title_style(t);
    return t;
}
static void id_title(lv_obj_t *l, const char *s){ lv_label_set_text(l, s); idisc_title_style(l); }
static int is_icon(const char *s){ return s && (unsigned char)s[0] == 0xEF && strlen(s) <= 4; }
static int tint_of(const char *s){                                         /* a steady colour per icon */
    if(!strcmp(s, LV_SYMBOL_WIFI) || !strcmp(s, LV_SYMBOL_BLUETOOTH) || !strcmp(s, LV_SYMBOL_IMAGE)) return 0;
    if(!strcmp(s, LV_SYMBOL_PLAY) || !strcmp(s, LV_SYMBOL_AUDIO)) return 5;
    if(!strcmp(s, LV_SYMBOL_SETTINGS) || !strcmp(s, LV_SYMBOL_LIST)) return 7;
    if(!strcmp(s, LV_SYMBOL_BATTERY_FULL) || !strcmp(s, LV_SYMBOL_POWER) || !strcmp(s, LV_SYMBOL_CHARGE)) return 1;
    unsigned h = 0; for(const unsigned char *p = (const unsigned char *)s; *p; p++) h = h * 31 + *p;
    return (int)(h % 7);
}
static void id_style(lv_obj_t *o, kit_role_t role){
    if(role == KIT_SCREEN){ idisc_wallpaper(o); return; }
    if(role == KIT_ROW && lv_obj_get_width(o) >= 160 && !(lv_obj_get_style_bg_opa(o, 0) >= LV_OPA_50
       && (lv_color_eq(lv_obj_get_style_bg_color(o, 0), TC(SURFACE_SELECTED)) || lv_color_eq(lv_obj_get_style_bg_color(o, 0), TC(ACCENT_PRIMARY))))){
        idisc_glass(o, 18); return;                                       /* (a picked row keeps its highlight) */
    }
    if(role == KIT_BUTTON && lv_obj_get_width(o) >= 40){
        lv_color_t bg = lv_obj_get_style_bg_color(o, 0);                    /* a coloured action keeps its colour */
        int coloured = lv_obj_get_style_bg_opa(o, 0) >= LV_OPA_50 && (lv_color_eq(bg, TC(ACCENT_PRIMARY)) || lv_color_eq(bg, TC(STATUS_DANGER)) || lv_color_eq(bg, TC(SWITCH_ON)));
        if(!coloured){ idisc_glass(o, LV_RADIUS_CIRCLE); lv_obj_set_style_shadow_width(o, 0, 0); }
        return;
    }
    if(role == KIT_ICON_BUTTON && lv_obj_get_width(o) >= 30 && lv_obj_get_style_bg_opa(o, 0) < LV_OPA_50){ idisc_glass(o, LV_RADIUS_CIRCLE); return; }
    if(role == KIT_SLIDER){
        lv_obj_set_style_bg_color(o, TC(CONTROL_FILL), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(o, TC(CONTROL_KNOB), LV_PART_KNOB);
        lv_obj_set_style_shadow_width(o, 8, LV_PART_KNOB); lv_obj_set_style_shadow_opa(o, 60, LV_PART_KNOB);
        lv_obj_set_style_shadow_color(o, TC(FIXED_MEDIA_BLACK), LV_PART_KNOB);
        return;
    }
    if(!lv_obj_check_type(o, &lv_label_class)) return;
    const char *s = lv_label_get_text(o);
    if(role == KIT_TITLE){ idisc_title_style(o); return; }
    if(role == KIT_ROW_CHEVRON){ lv_obj_set_style_text_color(o, TC(TEXT_MUTED), 0); return; }
    if((role == KIT_ROW_TEXT || role == KIT_ROW_SUB) && is_icon(s)){
        lv_obj_t *row = lv_obj_get_parent(o);
        if(!row || lv_obj_get_width(row) < 160) return;
        const lv_font_t *f = lv_obj_get_style_text_font(o, 0); int lh = lv_font_get_line_height(f);
        lv_obj_set_size(o, 30, 30); lv_obj_set_style_pad_top(o, (30 - lh) / 2, 0);
        lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_radius(o, 8, 0); lv_obj_set_style_bg_color(o, th_id_tint(tint_of(s)), 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(o, TC(FIXED_MEDIA_WHITE), 0);
        lv_obj_set_style_translate_x(o, -6, 0);                            /* the wider square: clear of the name */
    }
}
const theme_kit_t theme_kit_idisc = { .id = "idisc", .header = id_header, .style = id_style, .title = id_title };
