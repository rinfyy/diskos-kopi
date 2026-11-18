/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Spotify (kopi.35) - the UI side. The helper (spotify_helper.c + the Rust library in spotify/, run as "spot-disc")
 * is a Spotify Connect speaker and fetches your lists; its audio rides on the Disc's AirPlay receiver, as Music
 * Assistant's does. This file: the on/off switch, the Spotify screen (sign-in help, Liked Songs, Playlists, Search),
 * Music's buttons while Spotify plays, and Settings > Connectivity > Spotify.
 * Link: /tmp/spot/status (key=value), /tmp/spot/cmd (FIFO), /tmp/spot/list.tsv (the last list asked for). */
#include "screens.h"
#include "theme.h"
#include "config.h"
#include "ipc.h"
#include "ma.h"
#include "md5.h"
#include "vlist.h"
#include "spotify.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define SPOT_CACHE "/usr/data/diskos/spotify"     /* the saved sign-in (librespot's reusable credentials) */
enum { SP_OFF, SP_WAITING, SP_CONNECTING, SP_READY, SP_PLAYING, SP_ERROR };
static int g_state = SP_OFF, g_test = -1;
static char g_user[64], g_msg[96], g_title[160], g_artist[160], g_album[160], g_track[128];
static long g_dur, g_prog; static long long g_prog_at; static int g_speed;
static unsigned g_list_seq;                      /* the helper's answer counter (status "list=<seq> <kind>") */
static char g_list_kind[16], g_list_err[96];

char g_spot_name_lbl[48] = "Snowsky Disc";
char g_spot_cid_lbl[48] = "Built-in";
static void labels_load(void){
    const char *b = cfg_get_str("spot_name", "");
    snprintf(g_spot_name_lbl, sizeof g_spot_name_lbl, "%s", b && b[0] ? b : "Snowsky Disc");
    b = cfg_get_str("spot_cid", "");
    snprintf(g_spot_cid_lbl, sizeof g_spot_cid_lbl, "%s", b && b[0] ? b : "Built-in");
}
int spot_is_on(void){ return cfg_get_int("spot_on", 0); }

/* ---------------------------------------------------------------- the helper: this binary, run as "spot-disc --spotify ..." */
static pid_t g_pid, g_dying; static uint32_t g_restart_at, g_dying_at; static int g_leaving;
static void device_id(char *out){                 /* stable per Disc: from the Wi-Fi MAC */
    char mac[32] = "", s[64]; FILE *f = fopen("/sys/class/net/wlan0/address", "r");
    if(f){ if(!fgets(mac, sizeof mac, f)) mac[0] = 0; fclose(f); }
    snprintf(s, sizeof s, "diskos-spotify-%s", mac); md5_hex(s, strlen(s), out);
}
static void helper_kill_stale(void){
    FILE *f = fopen("/tmp/spot/status", "r"); if(!f) return;
    char line[96]; int pid = 0;
    while(fgets(line, sizeof line, f)) if(!strncmp(line, "pid=", 4)) pid = atoi(line + 4);
    fclose(f);
    if(pid > 1){
        char p[64], cmd[64] = ""; snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
        FILE *c = fopen(p, "r");
        if(c){ size_t n = fread(cmd, 1, sizeof cmd - 1, c); fclose(c); cmd[n] = 0; if(!strcmp(cmd, "spot-disc")){ kill(-pid, SIGTERM); kill(pid, SIGTERM); } }
    }
    unlink("/tmp/spot/status");
}
static void helper_start(void){
    if(g_pid > 0) return;
    labels_load();
    char name[48], id[40], cid[48];
    snprintf(name, sizeof name, "%s", g_spot_name_lbl); device_id(id);
    snprintf(cid, sizeof cid, "%s", strcmp(g_spot_cid_lbl, "Built-in") ? g_spot_cid_lbl : "");
    mkdir("/usr/data/diskos", 0755); mkdir(SPOT_CACHE, 0700); mkdir("/tmp/spot", 0755);
    char *argv[] = { "spot-disc", "--spotify", name, id, SPOT_CACHE, cid, NULL };
    posix_spawn_file_actions_t fa; posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa); posix_spawnattr_init(&at);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/tmp/spot/log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for(int fd = 3; fd < 256; fd++) posix_spawn_file_actions_addclose(&fa, fd);
    sigset_t mask; sigemptyset(&mask);
    posix_spawnattr_setsigmask(&at, &mask); posix_spawnattr_setpgroup(&at, 0);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
    extern char **environ; pid_t pid = 0;
    int rc = posix_spawn(&pid, "/proc/self/exe", &fa, &at, argv, environ);    /* (see ma.c: the in-RAM copy) */
    if(rc != 0) rc = posix_spawn(&pid, "/usr/data/mq_ui", &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at);
    if(rc == 0){ g_pid = pid; fprintf(stderr, "spotify: helper started (pid %d)\n", (int)pid); }
    else { fprintf(stderr, "spotify: spawn failed: %s\n", strerror(rc)); g_restart_at = lv_tick_get() + 5000; }
}
static void helper_stop(void){
    g_restart_at = 0;
    if(g_pid > 0){ kill(-g_pid, SIGTERM); kill(g_pid, SIGTERM); g_dying = g_pid; g_dying_at = lv_tick_get(); g_pid = 0; }
    unlink("/tmp/spot/status");
    ipc_clear_external();
    g_title[0] = 0;
}
static void helper_watch(void){
    if(g_dying > 0){
        pid_t r = waitpid(g_dying, NULL, WNOHANG);
        if(r == g_dying || (r < 0 && errno == ECHILD)) g_dying = 0;
        else if(lv_tick_elaps(g_dying_at) > 2000){ kill(-g_dying, SIGKILL); kill(g_dying, SIGKILL); }
        if(g_dying > 0) return;
    }
    if(g_pid > 0){
        int st = 0; pid_t r = waitpid(g_pid, &st, WNOHANG);
        if(r == g_pid || (r < 0 && errno == ECHILD && kill(g_pid, 0) < 0)){
            fprintf(stderr, "spotify: helper exited (%d), restarting\n", st); g_pid = 0; g_restart_at = lv_tick_get() + 3000;
        }
    }
    if(!spot_is_on()) return;
    if(g_pid == 0 && g_restart_at && (int32_t)(lv_tick_get() - g_restart_at) >= 0){ g_restart_at = 0; helper_start(); }
    static int tries;                                /* AirPlay carries the sound: three quiet tries to switch it on */
    if(ui_get_source_mode() == 5) tries = 0;
    else if(!g_leaving && tries < 3){ static uint32_t last; if(!last || lv_tick_elaps(last) > 10000){ last = lv_tick_get(); tries++; ui_set_source_mode(5); } }
}
void spot_restart(void){ if(spot_is_on()){ helper_stop(); g_restart_at = lv_tick_get() + 300; } }

static void status_read(void){
    if(g_test >= 0){ g_state = g_test; return; }
    if(!spot_is_on()){ g_state = SP_OFF; return; }
    FILE *f = fopen("/tmp/spot/status", "r");
    if(!f){ g_state = SP_CONNECTING; return; }
    char line[320]; int have = 0;
    g_title[0] = g_artist[0] = g_album[0] = g_track[0] = g_msg[0] = g_list_err[0] = 0; g_speed = 0;
    while(fgets(line, sizeof line, f)){
        line[strcspn(line, "\r\n")] = 0;
        char *v = strchr(line, '='); if(!v) continue; *v++ = 0;
        if(!strcmp(line, "state")) g_state = !strcmp(v, "ready") ? SP_READY : !strcmp(v, "playing") ? SP_PLAYING : !strcmp(v, "waiting") ? SP_WAITING
                                           : !strcmp(v, "error") ? SP_ERROR : SP_CONNECTING;
        else if(!strcmp(line, "user")) snprintf(g_user, sizeof g_user, "%s", v);
        else if(!strcmp(line, "msg")) snprintf(g_msg, sizeof g_msg, "%s", v);
        else if(!strcmp(line, "title")){ snprintf(g_title, sizeof g_title, "%s", v); have = 1; }
        else if(!strcmp(line, "artist")) snprintf(g_artist, sizeof g_artist, "%s", v);
        else if(!strcmp(line, "album")) snprintf(g_album, sizeof g_album, "%s", v);
        else if(!strcmp(line, "track")) snprintf(g_track, sizeof g_track, "%s", v);
        else if(!strcmp(line, "duration_ms")) g_dur = atol(v);
        else if(!strcmp(line, "progress_ms")) g_prog = atol(v);
        else if(!strcmp(line, "progress_at_ms")) g_prog_at = atoll(v);
        else if(!strcmp(line, "speed")) g_speed = atoi(v);
        else if(!strcmp(line, "list")){ unsigned s = 0; char k[16] = ""; if(sscanf(v, "%u %15s", &s, k) >= 1){ g_list_seq = s; snprintf(g_list_kind, sizeof g_list_kind, "%s", k); } }
        else if(!strcmp(line, "list_err")) snprintf(g_list_err, sizeof g_list_err, "%s", v);
    }
    fclose(f);
    if(have && g_title[0] && (g_state == SP_READY || g_state == SP_PLAYING))
        ipc_set_external(g_title, g_artist, g_album, g_track, g_dur, g_prog, g_prog_at, g_speed > 0 ? 1 : 0);
    else ipc_clear_external();
}
void spot_set_on(int on){
    cfg_set_int("spot_on", on ? 1 : 0);
    if(on){
        if(!wifi_radio_live()) ui_toast("Wi-Fi is off - turn it on for Spotify");
        if(ma_is_on()) ma_mode_leaving();                                  /* one AirPlay sender at a time */
        helper_kill_stale();
        if(ui_get_source_mode() != 5) ui_set_source_mode(5);
        if(g_dying > 0) g_restart_at = lv_tick_get() + 300; else helper_start();
    } else { helper_stop(); if(ui_get_source_mode() == 5){ g_leaving = 1; ui_set_source_mode(0); g_leaving = 0; } }
    status_read();
    if(screen_current() == SCR_SPOTIFY) spot_refresh();
}
void spot_mode_leaving(void){ if(!g_leaving && spot_is_on()){ cfg_set_int("spot_on", 0); helper_stop(); } }
int spot_controls(void){ return spot_is_on() && g_pid > 0 && (g_state == SP_READY || g_state == SP_PLAYING) && g_title[0]; }
int spot_playing(void){ return spot_is_on() && g_state == SP_PLAYING; }
void spot_send(const char *cmd){
    int fd = open("/tmp/spot/cmd", O_WRONLY | O_NONBLOCK); if(fd < 0){ ui_toast("Spotify isn't running"); return; }
    size_t n = strlen(cmd); char b[4096];
    if(n + 2 > sizeof b){ close(fd); return; }
    memcpy(b, cmd, n); b[n++] = '\n';
    if(write(fd, b, n) != (ssize_t)n) fprintf(stderr, "spotify: command not delivered\n");
    close(fd);
}
int spot_seek(long ms){ char b[40]; snprintf(b, sizeof b, "seek %ld", ms < 0 ? 0 : ms); spot_send(b); return 0; }
static void boot_cb(lv_timer_t *t){ (void)t; if(spot_is_on()){ helper_kill_stale(); helper_start(); } }
void spot_boot(void){ lv_timer_t *t = lv_timer_create(boot_cb, 9000, NULL); lv_timer_set_repeat_count(t, 1); }

/* ---------------------------------------------------------------- lists (from /tmp/spot/list.tsv) */
typedef struct { char kind; char *a, *b, *uri; } row_t;   /* P: a=name b=detail; T: a=title b=artist */
#define MAXROWS 400
static row_t g_rows[MAXROWS]; static int g_nrows; static char *g_buf; static char g_ctx[96];
static void rows_free(void){ free(g_buf); g_buf = NULL; g_nrows = 0; g_ctx[0] = 0; }
static void rows_parse(const char *text){
    rows_free();
    g_buf = strdup(text ? text : ""); if(!g_buf) return;
    for(char *line = g_buf, *nx; line && *line && g_nrows < MAXROWS; line = nx){
        nx = strchr(line, '\n'); if(nx) *nx++ = 0;
        char *f[5] = {0}; int k = 0; f[k++] = line;
        for(char *p = line; *p && k < 5; p++) if(*p == '\t'){ *p = 0; f[k++] = p + 1; }
        if(f[0][0] == 'C' && k >= 2){ snprintf(g_ctx, sizeof g_ctx, "%s", f[1]); continue; }
        if(f[0][0] == 'P' && k >= 4) g_rows[g_nrows++] = (row_t){ 'P', f[1], f[3], f[2] };
        else if(f[0][0] == 'T' && k >= 4) g_rows[g_nrows++] = (row_t){ 'T', f[1], f[2], f[3] };
    }
}
static char *slurp(const char *path){
    FILE *f = fopen(path, "rb"); if(!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if(n < 0 || n > 512 * 1024){ fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1); if(!b){ fclose(f); return NULL; }
    size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; fclose(f); return b;
}

/* ---------------------------------------------------------------- SCR_SPOTIFY */
enum { PG_STATUS, PG_HOME, PG_LIST };
static int g_pg = PG_STATUS, g_level;            /* list depth: 1 a list, 2 a playlist opened from Playlists */
static unsigned g_want_seq;                      /* waiting for this answer (0: none) */
static char g_list_title[96], g_saved_title[96]; static char *g_saved;   /* Playlists, kept to step back to */
static lv_obj_t *g_root, *g_hdr, *g_ring, *g_glyph, *g_status, *g_d1, *g_d2, *g_pill, *g_pill_lbl, *g_list, *g_note;
static lv_timer_t *g_timer;
static vlist_t g_vl;
static int g_lead;                               /* rows before the data rows (Shuffle) */

static void txt(lv_obj_t *l, const char *t){ if(l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t); }
static void show(lv_obj_t *o, int on){ if(!o) return; if(on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
static lv_obj_t *row2(const char *a, const char *b, lv_event_cb_t cb, intptr_t ud, int accent){
    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 280, 46);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(r, cb, LV_EVENT_SHORT_CLICKED, (void *)ud);
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, a);
    lv_label_set_long_mode(t, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(t, 12, b && b[0] ? 5 : 13); lv_obj_set_size(t, 256, 19);
    lv_obj_set_style_text_font(t, ui_font_user(16), 0);
    lv_obj_set_style_text_color(t, accent ? ui_current_accent() : TC(TEXT_PRIMARY), 0);
    if(b && b[0]){
        lv_obj_t *s = lv_label_create(r);
        lv_label_set_text(s, b);
        lv_label_set_long_mode(s, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_pos(s, 12, 25); lv_obj_set_size(s, 256, 16);
        lv_obj_set_style_text_font(s, ui_font_user(14), 0);
        lv_obj_set_style_text_color(s, TC(TEXT_LYRICS), 0);
    }
    ui_library_row(r);
    return r;
}
static unsigned g_req;                           /* our request number: the helper answers "list=<n> <kind>" */
static void ask(const char *cmd, const char *title, int level){   /* cmd: "list <what>" or "search <words>" */
    snprintf(g_list_title, sizeof g_list_title, "%s", title); g_level = level;
    g_want_seq = ++g_req;
    const char *sp = strchr(cmd, ' ');
    char c[400]; snprintf(c, sizeof c, "%.*s %u%s", sp ? (int)(sp - cmd) : (int)strlen(cmd), cmd, g_want_seq, sp ? sp : "");
    spot_send(c);
    g_pg = PG_LIST; rows_free();
    if(g_timer) lv_timer_set_period(g_timer, 250);
    spot_refresh();
}
static void row_cb(lv_event_t *e){
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i == -1){ char c[128]; snprintf(c, sizeof c, "shuffle %s", g_ctx); spot_send(c); screen_show(SCR_NOWPLAYING); return; }
    if(i < 0 || i >= g_nrows) return;
    row_t *r = &g_rows[i];
    if(r->kind == 'P'){
        if(strstr(r->uri, ":collection")){ ask("list liked", "Liked Songs", 2); }
        else {
            free(g_saved); g_saved = slurp("/tmp/spot/list.tsv"); snprintf(g_saved_title, sizeof g_saved_title, "%s", g_list_title);
            char c[160]; snprintf(c, sizeof c, "list playlist %s", r->uri); ask(c, r->a, 2);
        }
        return;
    }
    if(g_ctx[0]){ char c[256]; snprintf(c, sizeof c, "play %s %s", g_ctx, r->uri); spot_send(c); }
    else {                                           /* search results: the found songs, from this one */
        char c[4000]; int n = snprintf(c, sizeof c, "tracks %d", i);
        for(int k = 0; k < g_nrows && n < (int)sizeof c - 48; k++) if(g_rows[k].kind == 'T') n += snprintf(c + n, sizeof c - n, " %s", g_rows[k].uri);
        spot_send(c);
    }
    screen_show(SCR_NOWPLAYING);
}
static void add_row(int i){
    if(i < 0 || i >= g_nrows) return;
    row2(g_rows[i].a, g_rows[i].b, row_cb, i, 0);
}
static void search_done(const char *t){
    if(!t || !t[0]) return;
    char c[200]; snprintf(c, sizeof c, "search %s", t);
    char ti[96]; snprintf(ti, sizeof ti, "\"%s\"", t);
    ask(c, ti, 1);
}
static void home_cb(lv_event_t *e){
    switch((int)(intptr_t)lv_event_get_user_data(e)){
        case 0: ask("list liked", "Liked Songs", 1); break;
        case 1: ask("list playlists", "Playlists", 1); break;
        case 2: kbinput_open("Search Spotify", "", search_done); break;
        case 3: screen_show(SCR_NOWPLAYING); break;
        case 4: spot_set_on(0); break;
    }
}
static void fill_list(void){
    if(!g_list) return;
    vlist_end(&g_vl);
    lv_obj_clean(g_list);
    lv_obj_scroll_to_y(g_list, 0, LV_ANIM_OFF);
    g_lead = 0;
    if(g_pg == PG_HOME){
        row2("Liked Songs", "Your saved songs", home_cb, 0, 0);
        row2("Playlists", "Yours and the ones you follow", home_cb, 1, 0);
        row2("Search", "Find any song", home_cb, 2, 0);
        if(g_title[0]) row2("Now Playing", g_title, home_cb, 3, 1);
        char u[96]; snprintf(u, sizeof u, "Signed in%s%s", g_user[0] ? " as " : "", g_user);
        row2("Turn off", u, home_cb, 4, 0);
        return;
    }
    if(g_nrows > 0 && g_rows[0].kind == 'T' && g_ctx[0]){ row2(LV_SYMBOL_SHUFFLE "  Shuffle", NULL, row_cb, -1, 1); g_lead = 52; }
    if(g_nrows >= VLIST_MIN) vlist_begin(&g_vl, g_list, g_nrows, 46, 6, g_lead, 286, add_row);
    else for(int i = 0; i < g_nrows; i++) add_row(i);
}
static int g_shown_pg = -1; static unsigned g_shown_seq = ~0u; static int g_shown_title;
void spot_refresh(void){
    if(!g_root) return;
    labels_load(); status_read();
    int signed_in = g_state == SP_READY || g_state == SP_PLAYING;
    if(!signed_in) g_pg = PG_STATUS;
    else if(g_pg == PG_STATUS) g_pg = PG_HOME;
    show(g_ring, g_pg == PG_STATUS); show(g_glyph, g_pg == PG_STATUS); show(g_status, g_pg == PG_STATUS);
    show(g_d1, g_pg == PG_STATUS); show(g_d2, g_pg == PG_STATUS); show(g_pill, g_pg == PG_STATUS);
    show(g_list, g_pg != PG_STATUS);
    if(g_pg == PG_STATUS){
        txt(g_hdr, "Spotify"); show(g_note, 0);
        char st[64] = "", d1[120] = "", d2[120] = "";
        switch(g_state){
            case SP_OFF: snprintf(st, sizeof st, "Off"); snprintf(d1, sizeof d1, "Play Spotify on the Disc"); snprintf(d2, sizeof d2, "Premium and Wi-Fi needed"); break;
            case SP_WAITING: snprintf(st, sizeof st, "%s", g_msg[0] ? g_msg : "Sign in once");
                             snprintf(d1, sizeof d1, "Spotify > Devices:");
                             snprintf(d2, sizeof d2, "%s", g_spot_name_lbl); break;
            case SP_CONNECTING: snprintf(st, sizeof st, "%s", wifi_radio_live() ? (g_msg[0] ? g_msg : "Connecting") : "Wi-Fi is off");
                             snprintf(d1, sizeof d1, "%s", wifi_radio_live() ? "Reaching Spotify" : "Turn on Wi-Fi for Spotify"); break;
            default: snprintf(st, sizeof st, "Can't reach Spotify"); snprintf(d1, sizeof d1, "%s", g_msg[0] ? g_msg : "Trying again"); break;
        }
        txt(g_status, st); txt(g_d1, d1); txt(g_d2, d2);
        lv_obj_set_style_arc_color(g_ring, ui_current_accent(), LV_PART_INDICATOR);
        lv_arc_set_value(g_ring, g_state == SP_OFF ? 0 : g_state == SP_CONNECTING ? 500 : 1000);
        int on = g_state != SP_OFF;
        txt(g_pill_lbl, on ? "Turn off" : "Turn on");
        lv_obj_set_style_bg_color(g_pill, theme_color_from_rgb(on ? TH_SURF1 : TH_ACCENT), 0);
        g_shown_pg = PG_STATUS; return;
    }
    if(g_pg == PG_HOME){
        txt(g_hdr, "Spotify"); show(g_note, 0);
        int t = g_title[0] != 0;
        if(g_shown_pg != PG_HOME || g_shown_title != t){ fill_list(); g_shown_pg = PG_HOME; g_shown_title = t; }
        return;
    }
    txt(g_hdr, g_list_title);
    if(g_want_seq && g_list_seq == g_want_seq){                     /* our answer came */
        g_want_seq = 0; if(g_timer) lv_timer_set_period(g_timer, 1000);
        if(!strcmp(g_list_kind, "error")) rows_free();
        else { char *b = slurp("/tmp/spot/list.tsv"); rows_parse(b); free(b); }
        g_shown_seq = ~0u;
    }
    if(g_want_seq){ show(g_note, 1); txt(g_note, "Loading..."); if(g_shown_pg != PG_LIST){ lv_obj_clean(g_list); g_shown_pg = PG_LIST; } return; }
    if(g_shown_pg != PG_LIST || g_shown_seq != g_list_seq){
        fill_list(); g_shown_pg = PG_LIST; g_shown_seq = g_list_seq;
        if(g_nrows == 0){ show(g_note, 1); txt(g_note, !strcmp(g_list_kind, "error") ? (g_list_err[0] ? g_list_err : "Couldn't load it") : "Nothing here"); }
        else show(g_note, 0);
    }
}
/* swipe / Back: a playlist steps back to Playlists, a list to Spotify's page; then out */
int spot_back_consumed(void){
    if(g_pg != PG_LIST) return 0;
    vlist_end(&g_vl);
    if(g_level == 2 && g_saved){                   /* the saved Playlists list, no new request */
        rows_parse(g_saved); free(g_saved); g_saved = NULL;
        snprintf(g_list_title, sizeof g_list_title, "%s", g_saved_title); g_level = 1; g_want_seq = 0;
        g_shown_seq = ~0u; g_shown_pg = -1; spot_refresh(); return 1;
    }
    g_pg = PG_HOME; g_want_seq = 0; g_shown_pg = -1; rows_free(); spot_refresh();
    return 1;
}
void spot_test_state(int s){ g_test = s; spot_refresh(); }          /* host renders */
void spot_test_list(const char *tsv, const char *title){              /* host renders */
    g_test = SP_READY; snprintf(g_list_title, sizeof g_list_title, "%s", title); g_pg = PG_LIST; g_want_seq = 0;
    rows_parse(tsv); g_shown_pg = -1; spot_refresh();
}
static void tick(lv_timer_t *t){ (void)t; helper_watch(); if(screen_current() == SCR_SPOTIFY) spot_refresh(); else status_read(); }
static void follow_cb(lv_event_t *e){ (void)e; vlist_follow(&g_vl); }
static void pill_cb(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED) spot_set_on(!spot_is_on()); }
static void back_cb(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED) screen_back(); }
static lv_obj_t *label(lv_obj_t *root, const lv_font_t *font, uint32_t col, int y, int w){
    lv_obj_t *l = lv_label_create(root);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0); lv_obj_set_style_text_color(l, theme_color_from_rgb(col), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(l, w); lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}
void spot_create(lv_obj_t *root){
    g_root = root;
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    g_hdr = ui_header_cb(root, "Spotify", back_cb);
    lv_obj_set_style_text_align(g_hdr, LV_TEXT_ALIGN_CENTER, 0);
    if(!ui_edge_nav()) lv_obj_align(g_hdr, LV_ALIGN_TOP_MID, 0, 30);
    g_ring = lv_arc_create(root);
    lv_obj_remove_style(g_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(g_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g_ring, 112, 112); lv_obj_align(g_ring, LV_ALIGN_CENTER, 0, -62);
    lv_arc_set_bg_angles(g_ring, 0, 360); lv_arc_set_range(g_ring, 0, 1000); lv_arc_set_rotation(g_ring, 270);
    lv_obj_set_style_arc_width(g_ring, 6, LV_PART_MAIN); lv_obj_set_style_arc_color(g_ring, TC(CONTROL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_ring, 6, LV_PART_INDICATOR); lv_obj_set_style_arc_rounded(g_ring, true, LV_PART_INDICATOR);
    g_glyph = lv_label_create(root); lv_label_set_text(g_glyph, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(g_glyph, TF(UI_28), 0); lv_obj_set_style_text_color(g_glyph, TC(TEXT_PRIMARY), 0);
    lv_obj_align(g_glyph, LV_ALIGN_CENTER, 0, -62);
    g_status = label(root, TF(UI_16), TH_ACCENT, 20, 260);
    g_d1     = label(root, TH_F_DETAIL, TH_TXT2, 48, 250);
    g_d2     = label(root, TH_F_DETAIL, TH_TXT3, 70, 250);
    g_pill = lv_button_create(root);
    lv_obj_remove_style_all(g_pill);
    lv_obj_set_size(g_pill, 112, 40); lv_obj_align(g_pill, LV_ALIGN_CENTER, 0, 120);
    lv_obj_set_ext_click_area(g_pill, 4);
    lv_obj_set_style_radius(g_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g_pill, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(g_pill, pill_cb, LV_EVENT_CLICKED, NULL);
    g_pill_lbl = lv_label_create(g_pill);
    lv_obj_set_style_text_font(g_pill_lbl, TH_F_DETAIL, 0); lv_obj_set_style_text_color(g_pill_lbl, TC(TEXT_PRIMARY), 0);
    lv_label_set_text(g_pill_lbl, "Turn on"); lv_obj_center(g_pill_lbl);
    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 41, 70); lv_obj_set_size(g_list, 290, 290);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_list, 6, 0);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    ui_library_list_watch(g_list);
    lv_obj_add_event_cb(g_list, follow_cb, LV_EVENT_SCROLL, NULL);
    g_note = label(root, TH_F_DETAIL, TH_TXT2, 0, 260);
    g_timer = lv_timer_create(tick, 1000, NULL);
    spot_refresh();
}

/* ---------------------------------------------------------------- Settings > Connectivity > Spotify: text rows */
static void name_done(const char *t){ if(t && t[0]){ cfg_set_str("spot_name", t); labels_load(); setlist_refresh(); spot_restart(); } }
static void cid_done(const char *t){
    if(!t) return;
    cfg_set_str("spot_cid", (t[0] && strcasecmp(t, "built-in")) ? t : "");
    labels_load(); setlist_refresh(); spot_restart();
}
void spot_edit_name(void){ kbinput_open("Device name", g_spot_name_lbl, name_done); }
void spot_edit_cid(void){ kbinput_open("Client ID (empty = built-in)", strcmp(g_spot_cid_lbl, "Built-in") ? g_spot_cid_lbl : "", cid_done); }
void spot_sign_out(void){
    if(spot_is_on() && g_pid > 0) spot_send("logout");
    unlink(SPOT_CACHE "/credentials.json");
    ui_toast("Signed out of Spotify");
}
void spot_settings_load(void){ labels_load(); }
