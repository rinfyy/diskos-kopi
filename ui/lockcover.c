/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* lockcover.c - the Disco lock screen's own picture ("Lock cover: Custom image").
 *
 * Pictures live in the CustomCover folder at the top of the SD card (the same folder FiiO's stock firmware uses for
 * its custom lock-screen image): JPEG, PNG, BMP or GIF, any size (the decoder squares and scales to 360 x 360 and
 * enforces its own size/pixel/memory limits). The chosen file NAME is kept in cfg "dlock_cover"; Settings' "Change
 * picture" steps through the folder in name order.
 *
 * Decoding never runs on the UI thread: a worker takes the shared decode lock and runs the artwork decoder in
 * "saver" mode (art_make_saver: the same SD lease, deadline and staged-publish rules as every cover). A poll timer
 * on the UI thread installs the finished pixels and tells the lock screen. One decoded picture (506 KiB) is kept in
 * RAM while it is the chosen one, so showing the lock screen again costs nothing. */
#include "lockcover.h"
#include "art.h"
#include "config.h"
#include "lvgl/lvgl.h"
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef LOCKCOVER_DIR
#define LOCKCOVER_DIR "/tmp/sdcard/CustomCover"
#endif
#define LC_W 360
#define LC_MAX 64

void ui_decode_lock(void); void ui_decode_unlock(void);

static const char *lc_dir(void){ const char *e = getenv("DISKOS_LOCKCOVER_DIR"); return e && *e ? e : LOCKCOVER_DIR; }

static int is_picture(const char *n){
    const char *dot = strrchr(n, '.');
    if(!dot || n[0] == '.') return 0;   /* also skips macOS "._name" resource forks */
    return !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png") ||
           !strcasecmp(dot, ".bmp") || !strcasecmp(dot, ".gif");
}
static int name_cmp(const void *a, const void *b){ return strcasecmp(*(char *const *)a, *(char *const *)b); }

/* the folder's pictures in name order; returns the count (names are malloc'd, free with list_free) */
static int list_pictures(char **out, int cap){
    DIR *d = opendir(lc_dir());
    if(!d) return 0;
    int n = 0; struct dirent *e;
    while(n < cap && (e = readdir(d))){
        if(!is_picture(e->d_name) || strlen(e->d_name) > 200) continue;
        char p[512]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", lc_dir(), e->d_name);
        if(stat(p, &st) || !S_ISREG(st.st_mode)) continue;
        if(!(out[n] = strdup(e->d_name))) break;
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, name_cmp);
    return n;
}
static void list_free(char **v, int n){ for(int i = 0; i < n; i++) free(v[i]); }

/* ---- worker ---------------------------------------------------------------------------------------------------- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static int g_started, g_want;                       /* under g_mu */
static char g_want_path[512];                       /* under g_mu */
static unsigned g_want_gen;                         /* under g_mu */
static uint8_t *g_res; static unsigned g_res_gen; static int g_res_ready;   /* under g_mu */

static int64_t now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void *worker(void *arg){
    (void)arg;
    const size_t need = (size_t)LC_W * LC_W * 4;
    for(;;){
        pthread_mutex_lock(&g_mu);
        while(!g_want) pthread_cond_wait(&g_cv, &g_mu);
        char path[512]; snprintf(path, sizeof path, "%s", g_want_path);
        unsigned gen = g_want_gen; g_want = 0;
        pthread_mutex_unlock(&g_mu);

        char out[64]; snprintf(out, sizeof out, "/tmp/.diskos-lockcover-%u.bgra", gen);
        uint8_t *buf = NULL;
        ui_decode_lock();
        int rc = art_make_saver(path, out, now_ms() + 15000);
        ui_decode_unlock();
        if(rc == 0){
            int fd = open(out, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            buf = malloc(need);
            size_t got = 0;
            if(fd >= 0 && buf){
                ssize_t r;
                while(got < need && (r = read(fd, buf + got, need - got)) > 0) got += (size_t)r;
                char extra; if(got == need && read(fd, &extra, 1) != 0) got = 0;
            }
            if(fd >= 0) close(fd);
            if(got != need){ free(buf); buf = NULL; }
            if(buf) for(size_t i = 3; i < need; i += 4) buf[i] = 255;   /* opaque: the picture fills the round screen */
        }
        unlink(out);
        pthread_mutex_lock(&g_mu);
        free(g_res); g_res = buf; g_res_gen = gen; g_res_ready = 1;
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

/* ---- UI thread ------------------------------------------------------------------------------------------------- */
static uint8_t *g_buf;                  /* installed pixels, or NULL */
static lv_image_dsc_t g_dsc;
static char g_have[256];                /* the name g_buf was decoded from */
static char g_asked[256];               /* the name last requested */
static unsigned g_gen;
static int g_failed;                    /* the requested picture could not be decoded */
static lv_timer_t *g_poll;
static void (*g_listener)(void);

void lockcover_set_listener(void (*cb)(void)){ g_listener = cb; }

const char *lockcover_name(void){ return cfg_get_str("dlock_cover", ""); }

static void poll_cb(lv_timer_t *t){
    (void)t;
    pthread_mutex_lock(&g_mu);
    if(!g_res_ready){ pthread_mutex_unlock(&g_mu); return; }
    uint8_t *buf = g_res; unsigned gen = g_res_gen; g_res = NULL; g_res_ready = 0;
    pthread_mutex_unlock(&g_mu);
    if(gen != g_gen){ free(buf); return; }                /* a newer choice is on its way */
    if(g_poll) lv_timer_pause(g_poll);                    /* (kopi.32) delivered: rest until the next request */
    if(g_buf) lv_image_cache_drop(&g_dsc);
    free(g_buf); g_buf = buf; g_failed = !buf;
    if(buf){
        memset(&g_dsc, 0, sizeof g_dsc);
        g_dsc.header.magic = LV_IMAGE_HEADER_MAGIC; g_dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
        g_dsc.header.w = LC_W; g_dsc.header.h = LC_W; g_dsc.header.stride = LC_W * 4;
        g_dsc.data = g_buf; g_dsc.data_size = (uint32_t)LC_W * LC_W * 4;
        snprintf(g_have, sizeof g_have, "%s", g_asked);
    } else g_have[0] = 0;
    if(g_listener) g_listener();
}

/* make sure the chosen picture is decoded (or being decoded); returns at once */
void lockcover_request(void){
    const char *name = lockcover_name();
    if(!name[0]){                                         /* nothing chosen yet: take the folder's first picture */
        char *v[LC_MAX]; int n = list_pictures(v, LC_MAX);
        if(n > 0){ cfg_set_str("dlock_cover", v[0]); }
        list_free(v, n);
        name = lockcover_name();
        if(!name[0]) return;
    }
    if(g_buf && !strcmp(g_have, name)) return;            /* already showing it */
    if(!strcmp(g_asked, name)) return;                    /* already asked (or it failed: don't loop) */
    snprintf(g_asked, sizeof g_asked, "%s", name); g_failed = 0;
    if(!g_poll) g_poll = lv_timer_create(poll_cb, 150, NULL); else lv_timer_resume(g_poll);
    pthread_mutex_lock(&g_mu);
    if(!g_started){ pthread_t th; if(pthread_create(&th, NULL, worker, NULL) == 0){ pthread_detach(th); g_started = 1; } }
    if(g_started){
        snprintf(g_want_path, sizeof g_want_path, "%s/%s", lc_dir(), name);
        g_want_gen = ++g_gen; g_want = 1;
        pthread_cond_signal(&g_cv);
    }
    pthread_mutex_unlock(&g_mu);
}

const void *lockcover_img(void){
    const char *name = lockcover_name();
    return (g_buf && name[0] && !strcmp(g_have, name)) ? &g_dsc : NULL;
}
int lockcover_failed(void){ return g_failed; }

/* the next picture in the folder (wrapping); writes a short status line into msg */
int lockcover_next(char *msg, int cap){
    char *v[LC_MAX]; int n = list_pictures(v, LC_MAX);
    if(n == 0){ snprintf(msg, cap, "Put JPG or PNG pictures in the CustomCover folder on the SD card"); return 0; }
    const char *cur = lockcover_name(); int at = -1;
    for(int i = 0; i < n; i++) if(!strcmp(v[i], cur)){ at = i; break; }
    int next = (at + 1) % n;
    cfg_set_str("dlock_cover", v[next]); g_asked[0] = 0;   /* also retries a picture that failed before */
    snprintf(msg, cap, "Lock cover %d of %d: %.60s", next + 1, n, v[next]);
    list_free(v, n);
    lockcover_request();
    return n;
}
