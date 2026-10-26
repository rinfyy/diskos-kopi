/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* splash.c - the Disco! boot splash: a CD spins up and says its name.
 *
 * A CD fills the round screen, its rainbow rim turning; it spins up (the marks on its hub blur away at speed),
 * slows, and its hub becomes a "D" in Monoton with an accent ring drawn round it. The D rolls out to the left and
 * "isco!" unrolls behind it - "Disco!", the "!" in the accent colour. Then everything fades and Home is there.
 * 2.6 s, once per boot, on lv_layer_top; it swallows taps and deletes itself.
 *
 * Cost on the GPU-less renderer: the disc itself (grooves, its rainbow sheen, the hub) is one 360 px picture made once
 * and never redrawn - a real CD's reflections stay with the light, not the disc, so only what is printed on it turns:
 * two marks on the hub and the 36 coloured arcs of its rim. Those are small areas; nothing is scaled, masked or
 * blurred. The word unrolls inside a plain rectangular clip. */
#include "screens.h"
#include "theme.h"
#include "theme_fonts.h"
#include "theme_kit.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SP_MS   2600            /* the whole splash */
#define CDR     178             /* the disc's radius */
#define HUBR    54              /* its clear centre */
#define NRIM    36              /* coloured arcs round the rim */

static struct {
    lv_obj_t *root, *cd, *rim[NRIM], *mark, *hubring, *banner, *dl, *clip, *rest;
    int frames, dw, ww;         /* the D's width; the whole word's */
    uint32_t t0;
} g_sp;
static uint8_t *g_px;           /* the disc, XRGB8888, made once */
static lv_image_dsc_t g_dsc;
static lv_obj_t *g_cover;

static float clampf(float v){ return v < 0 ? 0 : v > 1 ? 1 : v; }
static float seg(float t, float a, float b){ return clampf((t - a) / (b - a)); }
static float smooth(float t){ t = clampf(t); return t * t * (3 - 2 * t); }
static float ease(float t){ t = clampf(t); float r = 1 - t; return 1 - r * r * r; }
static void rainbow(float h, float *r, float *g, float *b){   /* h 0..1 round the colour wheel */
    h -= floorf(h);
    *r = clampf(fabsf(h * 6 - 3) - 1); *g = clampf(2 - fabsf(h * 6 - 2)); *b = clampf(2 - fabsf(h * 6 - 4));
}
static uint32_t rainbow_rgb(float h){
    float r, g, b; rainbow(h, &r, &g, &b);
    return ((uint32_t)(255 * (0.4f + 0.6f * r)) << 16) | ((uint32_t)(255 * (0.4f + 0.6f * g)) << 8) | (uint32_t)(255 * (0.4f + 0.6f * b));
}

/* ---- the disc, once ---- */
static int disc_make(uint32_t bg){
    g_px = malloc(360 * 360 * 4);
    if(!g_px) return 0;
    float bb = (float)(bg & 255), bgc = (float)((bg >> 8) & 255), br = (float)((bg >> 16) & 255);
    for(int j = 0; j < 360; j++) for(int i = 0; i < 360; i++){
        uint8_t *o = g_px + ((size_t)j * 360 + i) * 4;
        float dx = i + 0.5f - 180, dy = j + 0.5f - 180, d = sqrtf(dx * dx + dy * dy);
        float c[3] = { bb, bgc, br };                                          /* B, G, R: the room */
        if(d < CDR + 0.5f){
            float a = atan2f(dy, dx), base;
            if(d < 15){ /* the hole */ }
            else if(d < HUBR){                                                 /* clear plastic, two moulded rings */
                base = 26 + (fabsf(d - 30) < 1 || fabsf(d - 44) < 1 ? 22 : 0);
                c[0] = base + 4; c[1] = base + 2; c[2] = base;
            } else if(d < HUBR + 8){                                           /* the mirror band */
                base = 70 + 40 * (0.5f + 0.5f * cosf(a * 2 + 0.8f));
                c[0] = base + 6; c[1] = base + 3; c[2] = base;
            } else {                                                           /* the data side */
                float k = (d - HUBR - 8) / (CDR - HUBR - 8);
                base = 46 - 26 * k + 2 * sinf(d * 1.7f);                       /* darker outwards, fine grooves */
                c[0] = base + 4; c[1] = base + 2; c[2] = base;
                /* two rainbow reflections, fixed to the light (up-left and down-right), colour changing with radius */
                for(int w = 0; w < 2; w++){
                    float da = a - (-2.2f + w * 3.14159f);
                    while(da > 3.14159f) da -= 6.28318f;
                    while(da < -3.14159f) da += 6.28318f;
                    float s = expf(-da * da / 0.09f) * (0.35f + 0.65f * k);
                    if(s < 0.01f) continue;
                    float r, g, b; rainbow(k * 0.85f + w * 0.1f, &r, &g, &b);
                    c[2] += (r * 255 - c[2]) * s * 0.55f; c[1] += (g * 255 - c[1]) * s * 0.55f; c[0] += (b * 255 - c[0]) * s * 0.55f;
                }
                float sh = expf(-(a + 0.9f) * (a + 0.9f) / 0.02f) * 0.25f * k;   /* a thin white glint */
                for(int q = 0; q < 3; q++) c[q] += (255 - c[q]) * sh;
            }
            float cov = clampf(CDR + 0.5f - d);                                /* antialiased edge */
            c[0] = bb + (c[0] - bb) * cov; c[1] = bgc + (c[1] - bgc) * cov; c[2] = br + (c[2] - br) * cov;
        }
        for(int q = 0; q < 3; q++) o[q] = (uint8_t)(c[q] < 0 ? 0 : c[q] > 255 ? 255 : c[q]);
        o[3] = 255;
    }
    memset(&g_dsc, 0, sizeof g_dsc);
    g_dsc.header.magic = LV_IMAGE_HEADER_MAGIC; g_dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
    g_dsc.header.w = g_dsc.header.h = 360; g_dsc.header.stride = 360 * 4;
    g_dsc.data = g_px; g_dsc.data_size = 360 * 360 * 4;
    return 1;
}

/* ---- the timeline ---- */
static void zframe(float t);
static void ntframe(float t);
static void splash_frame(int ms){
    float t = ms / (float)SP_MS;
    if(th_zune()){ zframe(t); return; }
    if(th_nt()){ ntframe(t); return; }
    /* spin: up to speed by 0.42, then slowing to a stop at 0.64 (the angle is the integral of the speed) */
    float ang;
    if(t < 0.42f){ float u = t / 0.42f; ang = 2900 * u * u * u; }                  /* degrees, accelerating */
    else { float u = seg(t, 0.42f, 0.64f); ang = 2900 + 1100 * (1 - (1 - u) * (1 - u) * (1 - u)); }
    float speed = t < 0.42f ? t / 0.42f : 1 - seg(t, 0.42f, 0.64f);

    lv_obj_set_style_opa(g_sp.cd, (lv_opa_t)(255 * smooth(seg(t, 0, 0.12f))), 0);
    int a0 = ((int)ang % 360 + 360) % 360;
    for(int i = 0; i < NRIM; i++){
        int s = (a0 + i * 360 / NRIM) % 360;
        lv_arc_set_bg_angles(g_sp.rim[i], s, s + 360 / NRIM);
    }
    lv_arc_set_rotation(g_sp.mark, a0);
    lv_obj_set_style_opa(g_sp.mark, (lv_opa_t)(220 * (1 - speed * 0.85f) * (1 - seg(t, 0.56f, 0.62f))), 0);   /* blurs away at speed */

    /* the hub becomes the D, an accent ring drawn round it */
    float d = smooth(seg(t, 0.58f, 0.66f)), ring = ease(seg(t, 0.6f, 0.7f));
    lv_arc_set_bg_angles(g_sp.hubring, 270, 270 + (int)(360 * ring));
    lv_obj_set_style_opa(g_sp.hubring, (lv_opa_t)(255 * (1 - seg(t, 0.71f, 0.75f))), 0);
    /* then it rolls out: the D moves to where the word starts, "isco!" unrolls behind it on a growing dark pill */
    float roll = smooth(seg(t, 0.71f, 0.83f));
    int x0 = 180 - g_sp.dw / 2, x1 = 180 - g_sp.ww / 2;
    int dx = x0 + (int)((x1 - x0) * roll);
    lv_obj_set_x(g_sp.dl, dx);
    lv_obj_set_style_opa(g_sp.dl, (lv_opa_t)(255 * d), 0);
    lv_obj_set_style_transform_rotation(g_sp.dl, roll > 0 && roll < 1 ? (int)(-3600 * (1 - roll)) : 0, 0);   /* it rolls as it goes */
    int open = (int)((g_sp.ww - g_sp.dw) * roll);
    lv_obj_set_x(g_sp.clip, dx + g_sp.dw); lv_obj_set_width(g_sp.clip, open > 0 ? open : 1);
    if(open > 0) lv_obj_remove_flag(g_sp.clip, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(g_sp.clip, LV_OBJ_FLAG_HIDDEN);
    int bw = 2 * HUBR + 8 + (int)((g_sp.ww + 40 - 2 * HUBR - 8) * roll);
    lv_obj_set_x(g_sp.banner, 180 - bw / 2); lv_obj_set_width(g_sp.banner, bw);
    lv_obj_set_style_bg_opa(g_sp.banner, (lv_opa_t)(230 * smooth(seg(t, 0.58f, 0.66f))), 0);

    /* hand-over to Home */
    lv_obj_set_style_opa(g_sp.root, (lv_opa_t)(255 * (1 - seg(t, 0.9f, 1))), 0);   /* after a moment on the name */
}

static void splash_anim_cb(void *var, int32_t v){ (void)var; if(g_sp.root) splash_frame((int)v); }
static void splash_drawn_cb(lv_event_t *e){ (void)e; g_sp.frames++; }   /* a frame actually drawn to the screen */
static void splash_done(lv_anim_t *a){
    (void)a;
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), splash_drawn_cb, NULL);
    fprintf(stderr, "splash: %d frames drawn in %u ms\n", g_sp.frames, (unsigned)lv_tick_elaps(g_sp.t0));   /* how smoothly this Disc drew it */
    if(g_sp.root) lv_obj_delete(g_sp.root);
    memset(&g_sp, 0, sizeof g_sp);
    free(g_px); g_px = NULL;
}
static lv_obj_t *plain(lv_obj_t *parent){
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o); kit_keep(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
static lv_obj_t *bare_arc(lv_obj_t *parent, int r, int w, lv_color_t c, lv_opa_t opa){
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style_all(a); kit_keep(a);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(a, 2 * r, 2 * r); lv_obj_set_pos(a, 180 - r, 180 - r);
    lv_obj_set_style_arc_width(a, w, LV_PART_MAIN); lv_obj_set_style_arc_color(a, c, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, opa, LV_PART_MAIN); lv_obj_set_style_arc_rounded(a, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    return a;
}
static lv_obj_t *word_label(lv_obj_t *parent, const char *s, lv_color_t c){
    lv_obj_t *l = lv_label_create(parent); kit_keep(l);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, &diskos_splash_disco_72, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}
static int zbuild(void);
static int ntbuild(void);
static int build(void){
    if(th_zune()) return zbuild();
    if(th_nt()) return ntbuild();
    if(!disc_make((uint32_t)lv_color_to_u32(TC(FIXED_SPLASH_BG)) & 0xFFFFFF)) return 0;
    g_sp.root = plain(lv_layer_top());
    lv_obj_set_size(g_sp.root, 360, 360);
    lv_obj_set_style_bg_color(g_sp.root, TC(FIXED_SPLASH_BG), 0);
    lv_obj_set_style_bg_opa(g_sp.root, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_sp.root, LV_OBJ_FLAG_CLICKABLE);             /* swallow taps during the splash */

    g_sp.cd = lv_image_create(g_sp.root); kit_keep(g_sp.cd);
    lv_image_set_src(g_sp.cd, &g_dsc); lv_obj_set_pos(g_sp.cd, 0, 0);
    for(int i = 0; i < NRIM; i++)                                   /* the turning rainbow rim */
        g_sp.rim[i] = bare_arc(g_sp.cd, CDR - 1, 4, theme_color_from_rgb(rainbow_rgb((float)i / NRIM)), 130);
    g_sp.mark = bare_arc(g_sp.cd, 42, 4, TC(FIXED_SPLASH_3), LV_OPA_COVER);   /* two marks on the hub, opposite */
    lv_arc_set_bg_angles(g_sp.mark, 0, 14);
    lv_obj_set_style_arc_opa(g_sp.mark, LV_OPA_COVER, LV_PART_INDICATOR); lv_obj_set_style_arc_color(g_sp.mark, TC(FIXED_SPLASH_3), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_sp.mark, 4, LV_PART_INDICATOR); lv_obj_set_style_arc_rounded(g_sp.mark, 0, LV_PART_INDICATOR);
    lv_arc_set_range(g_sp.mark, 0, 360); lv_arc_set_bg_angles(g_sp.mark, 0, 14);
    lv_arc_set_angles(g_sp.mark, 180, 194);

    g_sp.banner = plain(g_sp.root);                                 /* the dark pill the word sits on */
    lv_obj_set_style_radius(g_sp.banner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_sp.banner, TC(FIXED_SPLASH_BG), 0);
    lv_obj_set_y(g_sp.banner, 180 - HUBR - 4); lv_obj_set_height(g_sp.banner, 2 * HUBR + 8);
    g_sp.hubring = bare_arc(g_sp.root, HUBR + 4, 3, ui_current_accent(), LV_OPA_COVER);
    lv_arc_set_bg_angles(g_sp.hubring, 270, 270);

    /* the word: the D on its own, and "isco!" inside a clip that widens as it unrolls */
    g_sp.dl = word_label(g_sp.root, "D", TC(FIXED_SPLASH_3));
    lv_obj_update_layout(g_sp.dl);
    g_sp.dw = lv_obj_get_width(g_sp.dl);
    int lh = lv_obj_get_height(g_sp.dl);
    lv_obj_set_y(g_sp.dl, 180 - lh / 2);
    lv_obj_set_style_transform_pivot_x(g_sp.dl, g_sp.dw / 2, 0); lv_obj_set_style_transform_pivot_y(g_sp.dl, lh / 2, 0);
    g_sp.clip = plain(g_sp.root);
    lv_obj_set_y(g_sp.clip, 180 - lh / 2); lv_obj_set_height(g_sp.clip, lh);
    g_sp.rest = plain(g_sp.clip);
    lv_obj_set_size(g_sp.rest, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_sp.rest, LV_FLEX_FLOW_ROW);
    word_label(g_sp.rest, "isco", TC(FIXED_SPLASH_3));
    word_label(g_sp.rest, "!", ui_current_accent());
    lv_obj_update_layout(g_sp.rest);
    g_sp.ww = g_sp.dw + lv_obj_get_width(g_sp.rest);
    lv_obj_add_flag(g_sp.clip, LV_OBJ_FLAG_HIDDEN);
    return 1;
}

/* A plain cover in the splash's colour, put up before the first frame is drawn: Home is built and painted during
 * startup, well before the splash can start (the loop isn't running yet), and showed for a moment without it. */
void splash_cover(void){
    if(g_cover || g_sp.root) return;
    g_cover = plain(lv_layer_top());
    lv_obj_set_size(g_cover, 360, 360);
    lv_obj_set_style_bg_color(g_cover, TC(FIXED_SPLASH_BG), 0); lv_obj_set_style_bg_opa(g_cover, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_cover, LV_OBJ_FLAG_CLICKABLE);
}
static void cover_off(void){ if(g_cover){ lv_obj_delete(g_cover); g_cover = NULL; } }
void splash_start(void){
    if(g_sp.root){ cover_off(); return; }                           /* one splash at a time */
    if(!build()){ free(g_px); g_px = NULL; cover_off(); return; }   /* no memory for the disc: straight to Home */
    cover_off();                                                    /* the splash's own black takes over from here */
    g_sp.t0 = lv_tick_get();
    lv_display_add_event_cb(lv_display_get_default(), splash_drawn_cb, LV_EVENT_RENDER_READY, NULL);
    splash_frame(0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_sp.root);
    lv_anim_set_values(&a, 0, SP_MS);
    lv_anim_set_duration(&a, SP_MS);
    lv_anim_set_exec_cb(&a, splash_anim_cb);
    lv_anim_set_completed_cb(&a, splash_done);
    lv_anim_start(&a);
}
/* host renders: the splash standing at `ms` (no animation, nothing deleted) */
void splash_test_frame(int ms){
    if(!g_sp.root && !build()) return;
    splash_frame(ms);
}

/* ==== Zune (Figma 01): "disc" in Light 64 with an accent dot, "snowsky" small and spaced under it, a hairline ring on
 * the rim with an accent loader sweeping round it; in, a turn or two, out to Home ==== */
LV_FONT_DECLARE(font_zune_light_64) LV_FONT_DECLARE(font_zune_semibold_11) LV_FONT_DECLARE(font_zune_regular_12)
const lv_font_t *zune_font(const lv_font_t *base);
static lv_obj_t *g_zword, *g_zdot, *g_zsub, *g_zload;
static int zbuild(void){
    g_sp.root = plain(lv_layer_top());
    lv_obj_set_size(g_sp.root, 360, 360);
    lv_obj_set_style_bg_color(g_sp.root, TC(CANVAS), 0); lv_obj_set_style_bg_opa(g_sp.root, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_sp.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *tr = bare_arc(g_sp.root, 176, 2, TC(CONTROL_TRACK), LV_OPA_COVER);
    lv_arc_set_bg_angles(tr, 0, 360);
    g_zload = bare_arc(g_sp.root, 176, 3, ui_current_accent(), LV_OPA_COVER);
    lv_obj_set_style_arc_rounded(g_zload, true, LV_PART_MAIN);
    lv_arc_set_bg_angles(g_zload, 270, 330);
    g_zword = lv_label_create(g_sp.root); kit_keep(g_zword);
    lv_label_set_text(g_zword, "disc");
    lv_obj_set_style_text_font(g_zword, &font_zune_light_64, 0); lv_obj_set_style_text_color(g_zword, TC(TEXT_PRIMARY), 0);
    lv_obj_update_layout(g_zword);
    int w = lv_obj_get_width(g_zword), h = lv_obj_get_height(g_zword);
    lv_obj_set_pos(g_zword, 180 - (w + 14) / 2, 174 - h / 2 - 8);
    g_zdot = plain(g_sp.root);
    lv_obj_set_size(g_zdot, 10, 10); lv_obj_set_style_radius(g_zdot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_zdot, ui_current_accent(), 0); lv_obj_set_style_bg_opa(g_zdot, LV_OPA_COVER, 0);
    lv_obj_set_pos(g_zdot, 180 - (w + 14) / 2 + w + 4, 174 - h / 2 - 8 + h - 18);
    g_zsub = lv_label_create(g_sp.root); kit_keep(g_zsub);
    lv_label_set_text(g_zsub, "snowsky");
    lv_obj_set_style_text_font(g_zsub, &font_zune_semibold_11, 0); lv_obj_set_style_text_letter_space(g_zsub, 3, 0);
    lv_obj_set_style_text_color(g_zsub, TC(TEXT_MUTED), 0);
    lv_obj_align(g_zsub, LV_ALIGN_TOP_MID, 0, 222);
    /* which build booted: VERSION from the launcher's run record ("2.6.1-zune27" -> "zune27"), so an update can be confirmed */
    char ver[48] = "";
    const char *ev = getenv("ZSPLASH_VER");
    if(ev) snprintf(ver, sizeof ver, "%s", ev);
    else { FILE *f = fopen("/tmp/.diskos_run", "r"); char ln[160];
           if(f){ while(fgets(ln, sizeof ln, f)) if(!strncmp(ln, "VERSION=", 8)){ ln[strcspn(ln, "\r\n")] = 0; snprintf(ver, sizeof ver, "%s", ln + 8); break; } fclose(f); } }
    if(ver[0] && strcmp(ver, "flashed")){
        const char *v = strchr(ver, '-'); v = v ? v + 1 : ver;
        lv_obj_t *vl = lv_label_create(g_sp.root); kit_keep(vl); lv_label_set_text(vl, v);
        lv_obj_set_style_text_font(vl, zune_font(&font_zune_regular_12), 0); lv_obj_set_style_text_color(vl, TC(TEXT_MUTED), 0);
        lv_obj_align(vl, LV_ALIGN_TOP_MID, 0, 290);
    }
    return 1;
}
static void zframe(float t){
    float in = smooth(seg(t, 0, 0.18f));
    lv_obj_set_style_opa(g_zword, (lv_opa_t)(255 * in), 0);
    lv_obj_set_style_opa(g_zsub, (lv_opa_t)(255 * smooth(seg(t, 0.1f, 0.3f))), 0);
    lv_obj_set_style_opa(g_zdot, (lv_opa_t)(255 * smooth(seg(t, 0.22f, 0.32f))), 0);
    int a = 270 + (int)(720 * ease(t));                                   /* two turns, easing out */
    int len = 40 + (int)(80 * smooth(seg(t, 0, 0.5f)));
    lv_arc_set_bg_angles(g_zload, a % 360, (a + len) % 360);
    lv_obj_set_style_opa(g_sp.root, (lv_opa_t)(255 * (1 - seg(t, 0.9f, 1))), 0);
}

/* ==== NTDisk: black, a ring of dots round the rim lighting up one by one, "NTDISK" spelled in round dots column by
 * column, a red dot, "snowsky disc" small and spaced under it; then it fades to Home. Monochrome + red. ==== */
static const uint8_t NTW[6][7] = {                                        /* N T D I S K, 5 x 7 (bit 4 = left) */
    {0x11,0x19,0x15,0x15,0x13,0x11,0x11}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
};
#define NT_CELL 7
#define NT_COLS 35
#define NT_RIM  40
static lv_obj_t *g_ntword, *g_ntdot, *g_ntsub, *g_ntrim[NT_RIM];
static int g_ntcols;                                                       /* columns shown so far */
static void ntword_draw(lv_event_t *e){
    lv_layer_t *layer = lv_event_get_layer(e); if(!layer) return;
    int g_ntcols = *(const int *)lv_event_get_user_data(e);               /* the boot's or the goodbye's count */
    lv_area_t a; lv_obj_get_coords(lv_event_get_target(e), &a);
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r); r.radius = LV_RADIUS_CIRCLE; r.bg_opa = LV_OPA_COVER;
    for(int col = 0; col < g_ntcols && col < NT_COLS; col++){
        int letter = col / 6, c = col % 6; if(c == 5) continue;
        r.bg_color = col == g_ntcols - 1 && g_ntcols < NT_COLS ? TC(ACCENT_PRIMARY) : TC(FIXED_MEDIA_WHITE);   /* the writing head is red */
        for(int row = 0; row < 7; row++) if(NTW[letter][row] & (0x10 >> c)){
            lv_area_t d = { a.x1 + col * NT_CELL, a.y1 + row * NT_CELL, 0, 0 }; d.x2 = d.x1 + NT_CELL - 2; d.y2 = d.y1 + NT_CELL - 2;
            lv_draw_rect(layer, &r, &d);
        }
    }
}
static int ntbuild(void){
    g_sp.root = plain(lv_layer_top());
    lv_obj_set_size(g_sp.root, 360, 360);
    lv_obj_set_style_bg_color(g_sp.root, TC(FIXED_MEDIA_BLACK), 0); lv_obj_set_style_bg_opa(g_sp.root, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_sp.root, LV_OBJ_FLAG_CLICKABLE);
    for(int i = 0; i < NT_RIM; i++){                                       /* the rim: dim dots, lit in turn */
        float an = (i * 360.0f / NT_RIM - 90) * 0.0174533f;
        lv_obj_t *d = g_ntrim[i] = plain(g_sp.root); lv_obj_set_size(d, 5, 5);
        lv_obj_set_pos(d, 180 + (int)lroundf(166 * cosf(an)) - 2, 180 + (int)lroundf(166 * sinf(an)) - 2);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, TC(FIXED_MEDIA_HINT), 0);
    }
    g_ntword = plain(g_sp.root); lv_obj_set_size(g_ntword, NT_COLS * NT_CELL, 7 * NT_CELL);
    lv_obj_set_pos(g_ntword, 180 - (NT_COLS * NT_CELL + 14) / 2, 180 - 7 * NT_CELL / 2 - 8);
    lv_obj_add_event_cb(g_ntword, ntword_draw, LV_EVENT_DRAW_MAIN, &g_ntcols);
    g_ntdot = plain(g_sp.root); lv_obj_set_size(g_ntdot, 8, 8);           /* the red full stop */
    lv_obj_set_pos(g_ntdot, 180 - (NT_COLS * NT_CELL + 14) / 2 + NT_COLS * NT_CELL + 6, 180 + 7 * NT_CELL / 2 - 8 - 8);
    lv_obj_set_style_radius(g_ntdot, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_bg_color(g_ntdot, TC(ACCENT_PRIMARY), 0); lv_obj_set_style_bg_opa(g_ntdot, LV_OPA_COVER, 0);
    g_ntsub = lv_label_create(g_sp.root); kit_keep(g_ntsub); lv_label_set_text(g_ntsub, "SNOWSKY DISC");
    lv_obj_set_style_text_font(g_ntsub, TF(UI_12), 0); lv_obj_set_style_text_letter_space(g_ntsub, 4, 0);
    lv_obj_set_style_text_color(g_ntsub, TC(FIXED_MEDIA_HINT), 0); lv_obj_align(g_ntsub, LV_ALIGN_TOP_MID, 0, 214);
    g_ntcols = 0;
    return 1;
}
static void ntframe(float t){
    int lit = (int)(NT_RIM * smooth(seg(t, 0.02f, 0.78f)));               /* the rim fills round once */
    for(int i = 0; i < NT_RIM; i++){
        lv_color_t c = i < lit ? (i == lit - 1 && lit < NT_RIM ? TC(ACCENT_PRIMARY) : TC(FIXED_MEDIA_WHITE)) : TC(FIXED_MEDIA_HINT);
        if(!lv_color_eq(lv_obj_get_style_bg_color(g_ntrim[i], 0), c)) lv_obj_set_style_bg_color(g_ntrim[i], c, 0);
    }
    int cols = (int)(NT_COLS * seg(t, 0.12f, 0.6f) + 0.5f);              /* the word, a column at a time */
    if(cols != g_ntcols){ g_ntcols = cols; lv_obj_invalidate(g_ntword); }
    float blink = seg(t, 0.6f, 0.86f);                                    /* the red dot: blinks twice, then stays */
    int dot_on = t >= 0.6f && !((blink > 0.15f && blink < 0.3f) || (blink > 0.45f && blink < 0.6f));
    lv_obj_set_style_opa(g_ntdot, dot_on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(g_ntsub, (lv_opa_t)(255 * smooth(seg(t, 0.55f, 0.72f))), 0);
    lv_obj_set_style_opa(g_sp.root, (lv_opa_t)(255 * (1 - seg(t, 0.9f, 1))), 0);
}

/* ==== NTDisk shutdown: the boot backwards. The rim's dots go out one by one, the word is wiped away a column at a time
 * (the red head now erasing), "SHUTTING DOWN" fades, and the red full stop is the last thing left before it shrinks to
 * nothing. 1.6 s on the shutdown screen, then black until the player switches off. ==== */
#define GB_MS 1600
static lv_obj_t *g_gb, *g_gbrim[NT_RIM], *g_gbword, *g_gbdot, *g_gbsub;
static int g_gbcols; static uint32_t g_gb_t0; static lv_timer_t *g_gb_tm;
static void gb_frame(lv_timer_t *tm){
    (void)tm;
    if(!g_gb) return;
    float t = clampf((float)lv_tick_elaps(g_gb_t0) / GB_MS);
    int lit = NT_RIM - (int)(NT_RIM * smooth(seg(t, 0.04f, 0.72f)));       /* going out, the last one red */
    for(int i = 0; i < NT_RIM; i++){
        lv_color_t c = i < lit ? (i == lit - 1 && lit < NT_RIM ? TC(ACCENT_PRIMARY) : TC(FIXED_MEDIA_WHITE)) : TC(FIXED_MEDIA_HINT);
        if(!lv_color_eq(lv_obj_get_style_bg_color(g_gbrim[i], 0), c)) lv_obj_set_style_bg_color(g_gbrim[i], c, 0);
        lv_obj_set_style_opa(g_gbrim[i], (lv_opa_t)(255 * (1 - seg(t, 0.72f, 0.82f))), 0);
    }
    int cols = NT_COLS - (int)(NT_COLS * seg(t, 0.14f, 0.62f) + 0.5f);
    if(cols != g_gbcols){ g_gbcols = cols; lv_obj_invalidate(g_gbword); }
    lv_obj_set_style_opa(g_gbsub, (lv_opa_t)(255 * (1 - smooth(seg(t, 0.45f, 0.66f)))), 0);
    float k = 1 - smooth(seg(t, 0.78f, 0.98f));                             /* the full stop: alone, then gone */
    int d = (int)(8 + 10 * smooth(seg(t, 0.62f, 0.76f)));                  /* it grows a little as the rest goes */
    d = (int)(d * k);
    float m = smooth(seg(t, 0.6f, 0.76f));                                  /* from the word's end to the middle */
    float x0 = 180 - (NT_COLS * NT_CELL + 14) / 2 + NT_COLS * NT_CELL + 6 + 4, y0 = 180 + 7 * NT_CELL / 2 - 8 - 8 + 4;
    lv_obj_set_size(g_gbdot, d, d);
    lv_obj_set_pos(g_gbdot, (int)(x0 + (180 - x0) * m) - d / 2, (int)(y0 + (180 - y0) * m) - d / 2);
    if(t >= 1 && g_gb_tm){ lv_timer_delete(g_gb_tm); g_gb_tm = NULL; }
}
static void gb_deleted(lv_event_t *e){ (void)e; g_gb = NULL; if(g_gb_tm){ lv_timer_delete(g_gb_tm); g_gb_tm = NULL; } }
void splash_nt_goodbye(lv_obj_t *parent){
    g_gb = plain(parent); lv_obj_set_size(g_gb, 360, 360);
    lv_obj_set_style_bg_color(g_gb, TC(FIXED_MEDIA_BLACK), 0); lv_obj_set_style_bg_opa(g_gb, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(g_gb, gb_deleted, LV_EVENT_DELETE, NULL);
    for(int i = 0; i < NT_RIM; i++){
        float an = (i * 360.0f / NT_RIM - 90) * 0.0174533f;
        lv_obj_t *d = g_gbrim[i] = plain(g_gb); lv_obj_set_size(d, 5, 5);
        lv_obj_set_pos(d, 180 + (int)lroundf(166 * cosf(an)) - 2, 180 + (int)lroundf(166 * sinf(an)) - 2);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, TC(FIXED_MEDIA_WHITE), 0);
    }
    g_gbcols = NT_COLS;
    g_gbword = plain(g_gb); lv_obj_set_size(g_gbword, NT_COLS * NT_CELL, 7 * NT_CELL);
    lv_obj_set_pos(g_gbword, 180 - (NT_COLS * NT_CELL + 14) / 2, 180 - 7 * NT_CELL / 2 - 8);
    lv_obj_add_event_cb(g_gbword, ntword_draw, LV_EVENT_DRAW_MAIN, &g_gbcols);
    g_gbdot = plain(g_gb); lv_obj_set_size(g_gbdot, 8, 8);
    lv_obj_set_pos(g_gbdot, 180 - (NT_COLS * NT_CELL + 14) / 2 + NT_COLS * NT_CELL + 6, 180 + 7 * NT_CELL / 2 - 8 - 8);
    lv_obj_set_style_radius(g_gbdot, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_bg_color(g_gbdot, TC(ACCENT_PRIMARY), 0); lv_obj_set_style_bg_opa(g_gbdot, LV_OPA_COVER, 0);
    g_gbsub = lv_label_create(g_gb); kit_keep(g_gbsub); lv_label_set_text(g_gbsub, "SHUTTING DOWN");
    lv_obj_set_style_text_font(g_gbsub, TF(UI_12), 0); lv_obj_set_style_text_letter_space(g_gbsub, 4, 0);
    lv_obj_set_style_text_color(g_gbsub, TC(FIXED_MEDIA_HINT), 0); lv_obj_align(g_gbsub, LV_ALIGN_TOP_MID, 0, 214);
    g_gb_t0 = lv_tick_get();
    if(g_gb_tm) lv_timer_delete(g_gb_tm);
    g_gb_tm = lv_timer_create(gb_frame, 33, NULL);
    gb_frame(NULL);
}
void splash_nt_goodbye_test(int ms){ g_gb_t0 = lv_tick_get() - (uint32_t)ms; gb_frame(NULL); }   /* host renders */
