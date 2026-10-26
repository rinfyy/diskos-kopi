/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* imageview.c - the picture viewer (SCR_IMAGE), opened from Files on a JPEG, PNG, BMP or GIF.
 *
 *   - The picture is shown whole inside the round screen (Fit). Double-tap the middle to fill the screen instead
 *     (Fill, the edges cropped by the circle); double-tap again to go back.
 *   - Tap the left or right side, or swipe, for the previous / next picture in the same folder (it wraps round).
 *   - Tap the middle to show or hide the name and position. Back (swipe from the edge) returns to the folder.
 *
 * Decoding never runs on the UI thread: a worker takes the shared decode lock and runs the artwork decoder in its
 * "view" mode (art_make_view: shape kept, 360 x 360, transparent border; the same SD lease, deadline, size and
 * memory limits as every cover - pictures over 4 megapixels or 8 MiB are refused as too large). One decoded picture
 * (506 KiB) is held while it is shown. Colours are theme roles; the stage is always black, like a photo viewer. */
#include "screens.h"
#include "theme.h"
#include "art.h"
#include "imageview.h"
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IV_W 360
#define IV_MAXN 2000
#define IV_EDGE 64                     /* a swipe that starts this close to a side belongs to the system's Back */

void ui_decode_lock(void); void ui_decode_unlock(void);

/* ---- worker ---------------------------------------------------------------------------------------------------- */
typedef struct { uint8_t *px; int rc, x0, y0, x1, y1; } iv_result_t;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static int g_started, g_want, g_ready;            /* under g_mu */
static char g_want_path[1280];                    /* under g_mu */
static unsigned g_want_gen, g_res_gen;            /* under g_mu */
static iv_result_t g_res;                         /* under g_mu */

static int64_t now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void *worker(void *arg){
    (void)arg;
    const size_t need = (size_t)IV_W * IV_W * 4;
    for(;;){
        pthread_mutex_lock(&g_mu);
        while(!g_want) pthread_cond_wait(&g_cv, &g_mu);
        char path[1280]; snprintf(path, sizeof path, "%s", g_want_path);
        unsigned gen = g_want_gen; g_want = 0;
        pthread_mutex_unlock(&g_mu);

        iv_result_t r = { NULL, -1, IV_W, IV_W, -1, -1 };
        char out[64]; snprintf(out, sizeof out, "/tmp/.diskos-view-%u.bgra", gen);
        ui_decode_lock();
        pthread_mutex_lock(&g_mu); int stale = g_want && g_want_gen != gen; pthread_mutex_unlock(&g_mu);
        int rc = stale ? -2 : art_make_view(path, out, now_ms() + 20000);
        int code = rc == 0 ? 0 : art_last_exit();
        ui_decode_unlock();
        if(rc == 0){
            int fd = open(out, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            uint8_t *buf = malloc(need); size_t got = 0;
            if(fd >= 0 && buf){ ssize_t n; while(got < need && (n = read(fd, buf + got, need - got)) > 0) got += (size_t)n; }
            if(fd >= 0) close(fd);
            if(got == need){
                for(int y = 0; y < IV_W; y++) for(int x = 0; x < IV_W; x++) if(buf[((size_t)y * IV_W + x) * 4 + 3]){
                    if(x < r.x0) r.x0 = x;
                    if(y < r.y0) r.y0 = y;
                    if(x > r.x1) r.x1 = x;
                    if(y > r.y1) r.y1 = y;
                }
                r.px = buf; r.rc = 0;
            } else { free(buf); r.rc = 7; }
        } else r.rc = code > 0 ? code : 7;
        unlink(out);
        pthread_mutex_lock(&g_mu);
        free(g_res.px); g_res = r; g_res_gen = gen; g_ready = 1;
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

/* ---- UI -------------------------------------------------------------------------------------------------------- */
static lv_obj_t *g_root, *g_img, *g_msg, *g_cap, *g_name, *g_count, *g_hint;
static char g_dir[1024];
static char **g_names; static int g_n, g_cur;
static unsigned g_gen;
static uint8_t *g_px; static lv_image_dsc_t g_dsc; static int g_bw = IV_W, g_bh = IV_W;
static int g_fill;                                 /* 0 Fit (whole picture), 1 Fill (screen covered) */
static lv_timer_t *g_poll, *g_cap_tm, *g_tap_tm;
static lv_point_t g_press; static int g_hint_shown;

static void apply_scale(void){
    if(!g_px) return;
    float diag = sqrtf((float)g_bw * g_bw + (float)g_bh * g_bh), small = (float)(g_bw < g_bh ? g_bw : g_bh);
    float s = g_fill ? (IV_W / small) : (352.0f / diag);
    if(!g_fill && s > 1.0f) s = 1.0f;
    lv_image_set_scale(g_img, (uint32_t)(s * 256.0f + 0.5f));
    lv_obj_center(g_img);
}
static void caption_show(int on){
    if(!g_cap) return;
    if(on) lv_obj_remove_flag(g_cap, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(g_cap, LV_OBJ_FLAG_HIDDEN);
    if(g_cap_tm){ lv_timer_delete(g_cap_tm); g_cap_tm = NULL; }
}
static void cap_hide_cb(lv_timer_t *t){ (void)t; g_cap_tm = NULL; lv_obj_add_flag(g_cap, LV_OBJ_FLAG_HIDDEN); }
static void caption_flash(void){
    caption_show(1);
    g_cap_tm = lv_timer_create(cap_hide_cb, 2600, NULL); lv_timer_set_repeat_count(g_cap_tm, 1);
}
static void message(const char *m){
    if(m){ lv_label_set_text(g_msg, m); lv_obj_remove_flag(g_msg, LV_OBJ_FLAG_HIDDEN); }
    else lv_obj_add_flag(g_msg, LV_OBJ_FLAG_HIDDEN);
}
static void poll_cb(lv_timer_t *t){
    (void)t;
    pthread_mutex_lock(&g_mu);
    if(!g_ready){ pthread_mutex_unlock(&g_mu); return; }
    iv_result_t r = g_res; unsigned gen = g_res_gen; g_res.px = NULL; g_ready = 0;
    pthread_mutex_unlock(&g_mu);
    if(gen != g_gen){ free(r.px); return; }                     /* the user has already moved on */
    if(!r.px){
        message(r.rc == 6 ? "Picture too large\n(up to 4 megapixels, 8 MB)" : r.rc == 4 ? "This picture format\nisn't supported"
                : r.rc == 5 || r.rc == 3 ? "Couldn't read this picture" : "Couldn't open this picture");
        return;
    }
    if(g_px) lv_image_cache_drop(&g_dsc);
    free(g_px); g_px = r.px;
    g_bw = r.x1 >= r.x0 ? r.x1 - r.x0 + 1 : IV_W; g_bh = r.y1 >= r.y0 ? r.y1 - r.y0 + 1 : IV_W;
    memset(&g_dsc, 0, sizeof g_dsc);
    g_dsc.header.magic = LV_IMAGE_HEADER_MAGIC; g_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    g_dsc.header.w = IV_W; g_dsc.header.h = IV_W; g_dsc.header.stride = IV_W * 4;
    g_dsc.data = g_px; g_dsc.data_size = (uint32_t)IV_W * IV_W * 4;
    lv_image_set_src(g_img, &g_dsc);
    lv_image_set_pivot(g_img, IV_W / 2, IV_W / 2);
    lv_obj_remove_flag(g_img, LV_OBJ_FLAG_HIDDEN);
    message(NULL);
    apply_scale();
}
static void load(void){
    if(g_n <= 0) return;
    char cnt[32]; snprintf(cnt, sizeof cnt, "%d / %d", g_cur + 1, g_n);
    lv_label_set_text(g_count, cnt); lv_label_set_text(g_name, g_names[g_cur]);
    lv_obj_add_flag(g_img, LV_OBJ_FLAG_HIDDEN);                 /* never the previous picture under a new name */
    message("Loading");
    if(!g_poll) g_poll = lv_timer_create(poll_cb, 60, NULL);
    pthread_mutex_lock(&g_mu);
    if(!g_started){ pthread_t th; if(pthread_create(&th, NULL, worker, NULL) == 0){ pthread_detach(th); g_started = 1; } }
    if(g_started){
        snprintf(g_want_path, sizeof g_want_path, "%s/%s", g_dir, g_names[g_cur]);
        g_want_gen = ++g_gen; g_want = 1; pthread_cond_signal(&g_cv);
    }
    pthread_mutex_unlock(&g_mu);
    if(!g_started) message("Couldn't open this picture");
    caption_flash();
}
static void go(int d){
    if(g_n <= 1) return;
    g_cur = (g_cur + d + g_n) % g_n;
    load();
}

static void tap_single_cb(lv_timer_t *t){                       /* a middle tap that didn't become a double-tap */
    (void)t; g_tap_tm = NULL;
    caption_show(lv_obj_has_flag(g_cap, LV_OBJ_FLAG_HIDDEN));
}
static void pad_cb(lv_event_t *e){
    lv_event_code_t c = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if(c == LV_EVENT_PRESSED){ if(in) lv_indev_get_point(in, &g_press); return; }
    if(c == LV_EVENT_GESTURE){
        lv_dir_t d = in ? lv_indev_get_gesture_dir(in) : LV_DIR_NONE;
        if(g_press.x < IV_EDGE || g_press.x > IV_W - IV_EDGE) return;   /* an edge swipe is the system's Back */
        if(d == LV_DIR_LEFT) go(+1); else if(d == LV_DIR_RIGHT) go(-1);
        if(in) lv_indev_wait_release(in);
        return;
    }
    if(c != LV_EVENT_SHORT_CLICKED) return;
    if(g_press.x < 110){ go(-1); return; }
    if(g_press.x > IV_W - 110){ go(+1); return; }
    if(g_tap_tm){                                               /* second tap in the middle: Fit <-> Fill */
        lv_timer_delete(g_tap_tm); g_tap_tm = NULL;
        g_fill = !g_fill; apply_scale();
        ui_toast(g_fill ? "Fill screen" : "Whole picture");
        return;
    }
    g_tap_tm = lv_timer_create(tap_single_cb, 300, NULL); lv_timer_set_repeat_count(g_tap_tm, 1);
}

void imageview_create(lv_obj_t *root){
    g_root = root;
    lv_obj_set_style_bg_color(root, TC(FIXED_MEDIA_BLACK), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    g_img = lv_image_create(root); lv_obj_center(g_img); lv_obj_add_flag(g_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_img, LV_OBJ_FLAG_CLICKABLE);
    g_msg = lv_label_create(root); lv_label_set_text(g_msg, "");
    lv_obj_set_style_text_font(g_msg, TF(UI_18), 0); lv_obj_set_style_text_color(g_msg, TC(FIXED_MEDIA_WHITE), 0);
    lv_obj_set_style_text_opa(g_msg, 190, 0); lv_obj_set_style_text_align(g_msg, LV_TEXT_ALIGN_CENTER, 0); lv_obj_center(g_msg);

    g_cap = lv_obj_create(root); lv_obj_remove_style_all(g_cap);       /* name and position, over the picture */
    lv_obj_set_size(g_cap, 236, 64); lv_obj_align(g_cap, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_radius(g_cap, 32, 0); lv_obj_set_style_bg_color(g_cap, TC(FIXED_MEDIA_BLACK), 0);
    lv_obj_set_style_bg_opa(g_cap, 170, 0);
    lv_obj_remove_flag(g_cap, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    g_count = lv_label_create(g_cap); lv_label_set_text(g_count, "");
    lv_obj_set_style_text_font(g_count, TF(UI_14), 0); lv_obj_set_style_text_color(g_count, TC(FIXED_MEDIA_WHITE), 0);
    lv_obj_set_style_text_opa(g_count, 170, 0); lv_obj_align(g_count, LV_ALIGN_TOP_MID, 0, 8);
    g_name = lv_label_create(g_cap); lv_label_set_text(g_name, "");
    lv_label_set_long_mode(g_name, LV_LABEL_LONG_SCROLL_CIRCULAR); lv_obj_set_width(g_name, 200);
    lv_obj_set_style_text_align(g_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_name, ui_font_cjk(16), 0); lv_obj_set_style_text_color(g_name, TC(FIXED_MEDIA_WHITE), 0);
    lv_obj_align(g_name, LV_ALIGN_TOP_MID, 0, 30);

    g_hint = lv_obj_create(root); lv_obj_remove_style_all(g_hint);   /* once per session: how to browse */
    lv_obj_set_size(g_hint, 220, 58); lv_obj_align(g_hint, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_radius(g_hint, 29, 0); lv_obj_set_style_bg_color(g_hint, TC(FIXED_MEDIA_BLACK), 0);
    lv_obj_set_style_bg_opa(g_hint, 170, 0); lv_obj_remove_flag(g_hint, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ht = lv_label_create(g_hint);
    lv_label_set_text(ht, "Swipe to browse\nDouble-tap to zoom");
    lv_obj_set_style_text_font(ht, TF(UI_14), 0); lv_obj_set_style_text_color(ht, TC(FIXED_MEDIA_WHITE), 0);
    lv_obj_set_style_text_align(ht, LV_TEXT_ALIGN_CENTER, 0); lv_obj_center(ht);
    lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *pad = lv_obj_create(root); lv_obj_remove_style_all(pad);   /* takes every touch on the picture */
    lv_obj_set_size(pad, IV_W, IV_W); lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(pad, pad_cb, LV_EVENT_ALL, NULL);
}

static void hint_hide_cb(lv_timer_t *t){ (void)t; lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN); }
int imageview_open(const char *dir, char *const *names, int n, int index){
    if(!g_root || !dir || !names || n <= 0 || index < 0 || index >= n) return -1;
    if(n > IV_MAXN){                                            /* keep a window round the tapped picture */
        int lo = index - IV_MAXN / 2; if(lo < 0) lo = 0; if(lo + IV_MAXN > n) lo = n - IV_MAXN;
        names += lo; index -= lo; n = IV_MAXN;
    }
    char **v = calloc((size_t)n, sizeof *v);
    if(!v) return -1;
    for(int i = 0; i < n; i++) if(!(v[i] = strdup(names[i]))){ for(int k = 0; k < i; k++) free(v[k]); free(v); return -1; }
    for(int i = 0; i < g_n; i++) free(g_names[i]);
    free(g_names); g_names = v; g_n = n; g_cur = index;
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    g_fill = 0;
    screen_show(SCR_IMAGE);
    load();
    if(!g_hint_shown){
        g_hint_shown = 1; lv_obj_remove_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
        lv_timer_t *t = lv_timer_create(hint_hide_cb, 3500, NULL); lv_timer_set_repeat_count(t, 1);
    }
    return 0;
}
/* host renders: wait for the current decode to land */
int imageview_test_wait(int ms){ for(int i = 0; i < ms / 20; i++){ poll_cb(NULL); if(g_px && !lv_obj_has_flag(g_img, LV_OBJ_FLAG_HIDDEN)) return 1; usleep(20000); } return 0; }
void imageview_test_fill(int on){ g_fill = on; apply_scale(); }
