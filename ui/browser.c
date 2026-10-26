/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* browser.c - Browser (SCR_BROWSER), from Apps: a text web reader for the round screen.
 *
 * It shows what a 360 px screen can: a page's headings, paragraphs, list items and links (htmltext.c), in the theme's
 * colours, at the user's font size. No pictures, no JavaScript, no logins. Type an address or some words (they are
 * searched with DuckDuckGo Lite) in the address pill; tap a link to follow it; Back returns to the previous page.
 * Files also opens .html, .htm, .txt and .md files from the SD card here.
 *
 * Fetching never runs on the UI thread: a worker runs the system's wget (as the lyrics and online covers do) with a
 * time limit and a 3 MB cap, parses the page there, and hands the finished blocks back to a poll timer. Long pages
 * are shown 80 blocks at a time ("Show more"). Recent pages are kept in cfg browser_h1..browser_h6. */
#include "screens.h"
#include "theme.h"
#include "config.h"
#include "htmltext.h"
#include "command_spawn.h"
#include "sdio.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BR_CAP      (3u << 20)        /* bytes of page read at most */
#define BR_TIMEOUT  25000             /* ms for a whole fetch */
#define BR_PAGE     80                /* blocks built per "Show more" */
#define BR_HIST     24
#define BR_W        264               /* text width: clear of the round edge in the reading band */
#define BR_START    "about:start"

/* ---- worker ---------------------------------------------------------------------------------------------------- */
typedef struct { hdoc_t doc; int err; char url[1024]; } br_result_t;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static int g_started, g_want, g_ready;
static char g_want_url[1024];
static unsigned g_want_gen, g_res_gen;
static br_result_t g_res;

static int64_t now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static int is_local(const char *u){ return u[0] == '/' || !strncmp(u, "file://", 7); }
static int looks_text(const char *u){
    const char *d = strrchr(u, '.'), *s = strrchr(u, '/');
    return d && (!s || d > s) && (!strcasecmp(d, ".txt") || !strcasecmp(d, ".md") || !strcasecmp(d, ".log") || !strcasecmp(d, ".csv"));
}
/* run wget for url into a heap buffer; 0 ok (some bytes), else an error code: 1 no network/failed, 2 too slow */
static int fetch(const char *url, int with_agent, char **out, size_t *olen){
    *out = NULL; *olen = 0;
    int fd[2]; if(pipe(fd)) return 1;
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
    char *av_a[] = { "wget", "-q", "-O", "-", "-T", "15", "-U", "Mozilla/5.0 (Linux; diskOS Disco) TextBrowser", (char *)url, NULL };
    char *av_b[] = { "wget", "-q", "-O", "-", "-T", "15", (char *)url, NULL };
    pid_t pid = -1;
    int rc = command_spawn_io(&pid, with_agent ? av_a : av_b, fd[1], -1);
    close(fd[1]);
    if(rc){ close(fd[0]); return 1; }
    size_t cap = 256 * 1024, n = 0; char *buf = malloc(cap + 1); int err = 0;
    int64_t deadline = now_ms() + BR_TIMEOUT;
    while(buf){
        int64_t left = deadline - now_ms(); if(left <= 0){ err = 2; break; }
        struct pollfd p = { .fd = fd[0], .events = POLLIN };
        int r = poll(&p, 1, (int)(left > 1000 ? 1000 : left));
        if(r < 0){ if(errno == EINTR) continue; err = 1; break; }
        if(r == 0) continue;
        if(n == cap){
            if(cap >= BR_CAP) break;                                     /* enough: show what we have */
            size_t nc = cap * 2 > BR_CAP ? BR_CAP : cap * 2; char *nb = realloc(buf, nc + 1);
            if(!nb) break; buf = nb; cap = nc;
        }
        ssize_t got = read(fd[0], buf + n, cap - n);
        if(got < 0){ if(errno == EINTR) continue; err = 1; break; }
        if(got == 0) break;
        n += (size_t)got;
    }
    close(fd[0]);
    if(pid > 0){ kill(-pid, SIGKILL); kill(pid, SIGKILL); waitpid(pid, NULL, 0); }   /* done or given up: never left running */
    if(!buf) return 1;
    if(!err && n == 0) err = 1;
    if(err){ free(buf); return err; }
    buf[n] = 0; *out = buf; *olen = n;
    return 0;
}
static int read_local(const char *path, char **out, size_t *olen){
    if(!strncmp(path, "file://", 7)) path += 7;
    if(!sd_io_begin()) return 3;
    int fd = open(path, O_RDONLY | O_CLOEXEC), rc = 3;
    struct stat st;
    if(fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode)){
        size_t want = (size_t)st.st_size > BR_CAP ? BR_CAP : (size_t)st.st_size, n = 0;
        char *b = malloc(want + 1);
        while(b && n < want){ ssize_t r = read(fd, b + n, want - n); if(r <= 0) break; n += (size_t)r; }
        if(b){ b[n] = 0; *out = b; *olen = n; rc = 0; }
    }
    if(fd >= 0) close(fd);
    sd_io_end();
    return rc;
}
static void *worker(void *arg){
    (void)arg;
    for(;;){
        pthread_mutex_lock(&g_mu);
        while(!g_want) pthread_cond_wait(&g_cv, &g_mu);
        char url[1024]; snprintf(url, sizeof url, "%s", g_want_url);
        unsigned gen = g_want_gen; g_want = 0;
        pthread_mutex_unlock(&g_mu);
        br_result_t r; memset(&r, 0, sizeof r); snprintf(r.url, sizeof r.url, "%s", url);
        char *buf = NULL; size_t n = 0;
        int e = is_local(url) ? read_local(url, &buf, &n) : fetch(url, 1, &buf, &n);
        if(e == 1 && !is_local(url)) e = fetch(url, 0, &buf, &n);      /* an old wget without -U: try plainly */
        if(!e){
            const char *s = buf; while(*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t' || (unsigned char)*s == 0xEF || (unsigned char)*s == 0xBB || (unsigned char)*s == 0xBF) s++;
            int html = !looks_text(url) && (*s == '<');
            int pr = html ? html_parse(buf, n, url, &r.doc) : text_parse(buf, n, url, &r.doc);
            if(pr){ hdoc_free(&r.doc); e = 4; }
            else if(r.doc.n == 0) e = 5;
        }
        free(buf);
        r.err = e;
        pthread_mutex_lock(&g_mu);
        if(g_ready) hdoc_free(&g_res.doc);
        g_res = r; g_res_gen = gen; g_ready = 1;
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

/* ---- UI -------------------------------------------------------------------------------------------------------- */
static lv_obj_t *g_root, *g_list, *g_addr, *g_addr_l, *g_title;
static lv_timer_t *g_poll;
static hdoc_t g_doc; static int g_shown;
static char g_url[1024];
static char g_hist[BR_HIST][1024]; static int g_nhist;
static unsigned g_gen;
static int g_loading;

static void load(const char *url, int push);
static void render_start(void);

static void addr_paint(void){
    char h[96];
    if(!strcmp(g_url, BR_START) || !g_url[0]) snprintf(h, sizeof h, LV_SYMBOL_EDIT "  Search or address");
    else { char host[80]; url_host(g_url, host, sizeof host); snprintf(h, sizeof h, "%s%s", g_loading ? LV_SYMBOL_REFRESH "  " : "", host); }
    lv_label_set_text(g_addr_l, h);
}
static void recent_add(const char *url){
    if(is_local(url) || !strcmp(url, BR_START)) return;
    char keep[6][1024]; int n = 0;
    snprintf(keep[n++], sizeof keep[0], "%s", url);
    for(int i = 1; i <= 6 && n < 6; i++){
        char k[16]; snprintf(k, sizeof k, "browser_h%d", i);
        const char *v = cfg_get_str(k, "");
        if(v[0] && strcmp(v, url)) snprintf(keep[n++], sizeof keep[0], "%s", v);
    }
    cfg_begin();
    for(int i = 0; i < 6; i++){ char k[16]; snprintf(k, sizeof k, "browser_h%d", i + 1); cfg_set_str_deferred(k, i < n ? keep[i] : ""); }
    cfg_commit();
}

/* A tapped row is deleted when the next page is built, and LVGL must not delete an object inside its own event:
 * the navigation runs just after the event instead. */
static char g_go[1024];
static void go_async(void *u){ (void)u; char x[1024]; snprintf(x, sizeof x, "%s", g_go); load(x, 1); }
static void go_later(const char *href){
    if(!href || !href[0]) return;
    snprintf(g_go, sizeof g_go, "%s", href);
    lv_async_call_cancel(go_async, NULL); lv_async_call(go_async, NULL);
}
static void link_cb(lv_event_t *e){ go_later(lv_event_get_user_data(e)); }
static lv_obj_t *text_label(const char *s, const lv_font_t *f, lv_color_t c, int w){
    lv_obj_t *l = lv_label_create(g_list);
    lv_label_set_text(l, s); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, w); lv_obj_set_style_text_font(l, f, 0); lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_line_space(l, 2, 0);
    return l;
}
static lv_obj_t *link_row(const char *text, const char *href, int small){
    lv_obj_t *r = lv_obj_create(g_list); lv_obj_remove_style_all(r);
    lv_obj_set_width(r, BR_W); lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(r, small ? 3 : 6, 0); lv_obj_set_style_pad_hor(r, 8, 0);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE), 0); lv_obj_set_style_bg_opa(r, small ? LV_OPA_TRANSP : LV_OPA_60, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE_PRESSED), LV_STATE_PRESSED); lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(r, link_cb, LV_EVENT_SHORT_CLICKED, (void *)href);   /* the string lives in g_doc until the next page */
    lv_obj_t *l = lv_label_create(r);
    char t[300]; snprintf(t, sizeof t, "%s%s", small ? LV_SYMBOL_RIGHT " " : "", text);
    lv_label_set_text(l, t); lv_label_set_long_mode(l, small ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, BR_W - 16);
    lv_obj_set_style_text_font(l, small ? ui_font_cjk(14) : ui_font_user(16), 0);
    lv_obj_set_style_text_color(l, TC(ACCENT_PRIMARY), 0);
    return r;
}
static void more_cb(lv_event_t *e);
static void render_more(void){
    lv_obj_t *old = lv_obj_get_child(g_list, -1);                       /* the previous "Show more" */
    if(old && lv_obj_get_user_data(old) == (void *)1) lv_obj_delete(old);
    int end = g_shown + BR_PAGE; if(end > g_doc.n) end = g_doc.n;
    for(int i = g_shown; i < end; i++){
        hblock_t *b = &g_doc.b[i];
        switch(b->kind){
        case HB_HEADING: { lv_obj_t *l = text_label(b->text, ui_font_cjk(20), TC(TEXT_PRIMARY), BR_W);
                           lv_obj_set_style_pad_top(l, i ? 10 : 0, 0); break; }
        case HB_LINK:    link_row(b->text, b->link[0].href, 0); break;
        case HB_ITEM:    { char t[3100]; snprintf(t, sizeof t, "\xE2\x80\xA2 %s", b->text);
                           text_label(t, ui_font_user(16), TC(TEXT_PRIMARY), BR_W - 8); break; }
        case HB_PRE:     text_label(b->text, ui_font_cjk(14), TC(TEXT_SECONDARY), BR_W); break;
        case HB_QUOTE:   { lv_obj_t *l = text_label(b->text, ui_font_user(16), TC(TEXT_SECONDARY), BR_W - 14);
                           lv_obj_set_style_border_side(l, LV_BORDER_SIDE_LEFT, 0); lv_obj_set_style_border_width(l, 2, 0);
                           lv_obj_set_style_border_color(l, TC(ACCENT_PRIMARY), 0); lv_obj_set_style_pad_left(l, 8, 0); break; }
        default:         text_label(b->text, ui_font_user(16), TC(TEXT_PRIMARY), BR_W); break;
        }
        if(b->kind != HB_LINK) for(int k = 0; k < b->nlink && k < 4; k++) link_row(b->link[k].text, b->link[k].href, 1);
    }
    g_shown = end;
    if(g_shown < g_doc.n || g_doc.truncated){
        lv_obj_t *m = lv_obj_create(g_list); lv_obj_remove_style_all(m); lv_obj_set_user_data(m, (void *)1);
        lv_obj_set_size(m, 160, 40); lv_obj_set_style_radius(m, 20, 0);
        lv_obj_set_style_bg_color(m, TC(SURFACE_RAISED), 0); lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
        lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = lv_label_create(m); lv_label_set_text(l, g_shown < g_doc.n ? "Show more" : "End of what fits");
        lv_obj_set_style_text_font(l, TF(UI_16), 0); lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0); lv_obj_center(l);
        if(g_shown < g_doc.n) lv_obj_add_event_cb(m, more_cb, LV_EVENT_CLICKED, NULL);
    }
}
static void more_async(void *u){ (void)u; render_more(); }
static void more_cb(lv_event_t *e){ (void)e; lv_async_call(more_async, NULL); }
static void message(const char *head, const char *body){
    lv_obj_clean(g_list);
    lv_obj_t *h = text_label(head, ui_font_cjk(20), TC(TEXT_PRIMARY), BR_W);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_style_pad_top(h, 30, 0);
    if(body){ lv_obj_t *b = text_label(body, TF(UI_16), TC(TEXT_SECONDARY), BR_W); lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0); }
}
static void poll_cb(lv_timer_t *t){
    (void)t;
    pthread_mutex_lock(&g_mu);
    if(!g_ready){ pthread_mutex_unlock(&g_mu); return; }
    br_result_t r = g_res; unsigned gen = g_res_gen; g_ready = 0; memset(&g_res, 0, sizeof g_res);
    pthread_mutex_unlock(&g_mu);
    if(gen != g_gen){ hdoc_free(&r.doc); return; }                       /* the user went elsewhere meanwhile */
    g_loading = 0; addr_paint();
    if(r.err){
        hdoc_free(&r.doc);
        if(r.err == 1) message("Couldn't load the page", wifi_radio_live() ? "Check the address, or try again." : "Wi-Fi is off. Turn it on in Quick Settings.");
        else if(r.err == 2) message("The page took too long", "Try again, or a lighter page.");
        else if(r.err == 3) message("Couldn't open the file", NULL);
        else message("Nothing readable here", "This page needs a full browser (pictures or scripts only).");
        return;
    }
    hdoc_free(&g_doc); g_doc = r.doc; g_shown = 0;
    lv_label_set_text(g_title, g_doc.title[0] ? g_doc.title : "");
    lv_obj_clean(g_list);
    render_more();
    lv_obj_scroll_to_y(g_list, 0, LV_ANIM_OFF);
    recent_add(r.url);
}
static void load(const char *url, int push){
    if(!url || !url[0]) return;
    if(push && g_url[0] && strcmp(g_url, url)){
        if(g_nhist == BR_HIST){ memmove(g_hist[0], g_hist[1], sizeof g_hist[0] * (BR_HIST - 1)); g_nhist--; }
        snprintf(g_hist[g_nhist++], sizeof g_hist[0], "%s", g_url);
    }
    char u[1024]; snprintf(u, sizeof u, "%s", url);                      /* url may point into g_doc, freed below */
    snprintf(g_url, sizeof g_url, "%s", u);
    if(!strcmp(u, BR_START)){ g_gen++; g_loading = 0; addr_paint(); render_start(); return; }
    g_loading = 1; addr_paint();
    lv_label_set_text(g_title, "");
    message("Loading", is_local(u) ? NULL : "Fetching the page over Wi-Fi");
    if(!g_poll) g_poll = lv_timer_create(poll_cb, 80, NULL);
    pthread_mutex_lock(&g_mu);
    if(!g_started){ pthread_t th; if(pthread_create(&th, NULL, worker, NULL) == 0){ pthread_detach(th); g_started = 1; } }
    if(g_started){ snprintf(g_want_url, sizeof g_want_url, "%s", u); g_want_gen = ++g_gen; g_want = 1; pthread_cond_signal(&g_cv); }
    pthread_mutex_unlock(&g_mu);
    if(!g_started){ g_loading = 0; message("Couldn't start loading", NULL); }
}
static void typed(const char *text){
    if(!text || !text[0]) return;
    char u[1024]; url_from_input(text, u, sizeof u);
    load(u, 1);
}
static void addr_cb(lv_event_t *e){
    (void)e;
    const char *init = (!strcmp(g_url, BR_START) || is_local(g_url)) ? "" : g_url;
    kbinput_open("Search or address", init, typed);
}
static void start_link_cb(lv_event_t *e){ go_later(lv_event_get_user_data(e)); }
static void render_start(void){
    hdoc_free(&g_doc); g_shown = 0;
    lv_label_set_text(g_title, "");
    lv_obj_clean(g_list);
    lv_obj_t *h = text_label("Read the web as text: no pictures or videos. Tap the address pill to search or go to a site.",
                             TF(UI_14), TC(TEXT_SECONDARY), BR_W);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    static const char *const QUICK[][2] = {
        { "Wikipedia", "https://en.m.wikipedia.org/wiki/Special:Random" },
        { "Text-only news (NPR)", "https://text.npr.org/" },
        { "CNN lite", "https://lite.cnn.com/" },
        { "Hacker News", "https://news.ycombinator.com/" } };
    text_label("Start", ui_font_cjk(20), TC(TEXT_PRIMARY), BR_W);
    for(unsigned i = 0; i < sizeof QUICK / sizeof QUICK[0]; i++){
        lv_obj_t *r = link_row(QUICK[i][0], QUICK[i][1], 0);
        lv_obj_remove_event_cb(r, link_cb); lv_obj_add_event_cb(r, start_link_cb, LV_EVENT_SHORT_CLICKED, (void *)QUICK[i][1]);
    }
    static char recent[6][1024]; int nr = 0;
    for(int i = 1; i <= 6; i++){
        char k[16]; snprintf(k, sizeof k, "browser_h%d", i);
        const char *v = cfg_get_str(k, ""); if(v[0]) snprintf(recent[nr++], sizeof recent[0], "%s", v);
    }
    if(nr){
        text_label("Recent", ui_font_cjk(20), TC(TEXT_PRIMARY), BR_W);
        for(int i = 0; i < nr; i++){
            char host[96]; url_host(recent[i], host, sizeof host);
            const char *path = strstr(recent[i], "://"); path = path ? strchr(path + 3, '/') : NULL;
            char show[200]; snprintf(show, sizeof show, "%s%s", host, path && path[1] ? path : "");
            lv_obj_t *r = link_row(show, recent[i], 1);
            lv_obj_remove_event_cb(r, link_cb); lv_obj_add_event_cb(r, start_link_cb, LV_EVENT_SHORT_CLICKED, recent[i]);
        }
    }
    lv_obj_scroll_to_y(g_list, 0, LV_ANIM_OFF);
}
/* screen_back(): a page back first; at the start page, leave */
int browser_back_consumed(void){
    if(g_nhist <= 0) return 0;
    char u[1024]; snprintf(u, sizeof u, "%s", g_hist[--g_nhist]);
    load(u, 0);
    return 1;
}
static void header_back_cb(lv_event_t *e){ (void)e; screen_back(); }
void browser_create(lv_obj_t *root){
    g_root = root;
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0); lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    g_list = lv_obj_create(root); lv_obj_remove_style_all(g_list);
    lv_obj_set_size(g_list, BR_W + 8, 360); lv_obj_set_pos(g_list, (360 - BR_W - 8) / 2, 0);
    int z = th_zune() ? 50 : 0;                                       /* Zune: its tall title sits above everything */
    lv_obj_set_style_pad_top(g_list, 116 + z, 0); lv_obj_set_style_pad_bottom(g_list, 150, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN); lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_list, 8, 0);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER); lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_t *top = lv_obj_create(root); lv_obj_remove_style_all(top);  /* header band: the page scrolls under it */
    lv_obj_set_size(top, 360, 112 + z); lv_obj_set_style_bg_color(top, TC(CANVAS), 0);
    lv_obj_set_style_bg_grad_color(top, TC(CANVAS), 0); lv_obj_set_style_bg_main_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_opa(top, LV_OPA_TRANSP, 0); lv_obj_set_style_bg_main_stop(top, 200, 0);
    lv_obj_set_style_bg_grad_stop(top, 255, 0); lv_obj_set_style_bg_grad_dir(top, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    ui_header_cb(root, "Browser", header_back_cb);
    g_addr = lv_obj_create(root); lv_obj_remove_style_all(g_addr);
    lv_obj_set_size(g_addr, 236, 34); lv_obj_align(g_addr, LV_ALIGN_TOP_MID, 0, 56 + z);
    lv_obj_set_style_radius(g_addr, 17, 0); lv_obj_set_style_bg_color(g_addr, TC(SURFACE), 0); lv_obj_set_style_bg_opa(g_addr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_addr, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(g_addr, 1, 0); lv_obj_set_style_border_color(g_addr, TC(ACCENT_PRIMARY), 0); lv_obj_set_style_border_opa(g_addr, LV_OPA_40, 0);
    lv_obj_add_flag(g_addr, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(g_addr, LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_ext_click_area(g_addr, 6);
    lv_obj_add_event_cb(g_addr, addr_cb, LV_EVENT_CLICKED, NULL);
    g_addr_l = lv_label_create(g_addr); lv_label_set_long_mode(g_addr_l, LV_LABEL_LONG_DOT); lv_obj_set_size(g_addr_l, 210, 20);
    lv_obj_set_style_text_align(g_addr_l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_addr_l, TF(UI_14), 0); lv_obj_set_style_text_color(g_addr_l, TC(TEXT_SECONDARY), 0); lv_obj_center(g_addr_l);
    g_title = lv_label_create(root); lv_label_set_text(g_title, "");               /* the page's own title, small */
    lv_label_set_long_mode(g_title, LV_LABEL_LONG_DOT); lv_obj_set_width(g_title, 250);
    lv_obj_set_style_text_align(g_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_title, ui_font_cjk(14), 0); lv_obj_set_style_text_color(g_title, TC(TEXT_MUTED), 0);
    lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 94 + z);
    snprintf(g_url, sizeof g_url, "%s", BR_START);
    addr_paint();
}
/* open the browser: url NULL = where it was (the start page the first time); a path or address opens that page */
void browser_open(const char *url){
    screen_show(SCR_BROWSER);
    if(url && url[0]){ g_nhist = 0; load(url, 0); }
    else if(!strcmp(g_url, BR_START)) render_start();
}
/* host renders: wait for the current page to land */
int browser_test_wait(int ms){ for(int i = 0; i < ms / 20; i++){ poll_cb(NULL); if(!g_loading) return 1; usleep(20000); } return 0; }
