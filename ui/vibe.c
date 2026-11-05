/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* vibe.c - "Vibe": the next songs chosen to match the one before (kopi.27).
 *
 * Used by the Vibe play mode (each song is followed by a similar one, so the music drifts slowly instead of jumping)
 * and when the Queue runs out (playback carries on in the same vibe instead of stopping).
 *
 * Similar means, most important first:
 *   - SongSphere: if the song (or its album / artist) is placed on the sphere, songs placed near it
 *   - the same genre / tag, or genres sharing a word ("indie rock" ~ "rock", "j-pop" ~ "pop")
 *   - the same artist (a little: at most two in a row-window), other albums of it
 *   - a similar length (a 3-minute pop song is less likely to be followed by a 12-minute track)
 * Songs heard lately in this vibe (the last 80) are not picked again; at most one song per album and two per artist
 * in each pick, so it never turns into the album. Chosen at random among the closest, weighted, so the same seed
 * doesn't always give the same song. All in memory (the library list is already loaded), UI thread only. */
#include "screens.h"
#include "musicdb.h"
#include "config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define HIST 80
static int g_hist[HIST], g_nhist, g_hpos;
static unsigned g_rnd;
static unsigned rnd(void){ g_rnd = g_rnd * 1103515245u + 12345u; return g_rnd >> 8; }
void vibe_note_played(int id){                                     /* (also the seed itself) */
    if(id <= 0) return;
    for(int i = 0; i < g_nhist; i++) if(g_hist[i] == id) return;
    g_hist[g_hpos] = id; g_hpos = (g_hpos + 1) % HIST; if(g_nhist < HIST) g_nhist++;
}
static int heard(int id){ for(int i = 0; i < g_nhist; i++) if(g_hist[i] == id) return 1; return 0; }

/* genre words: lower-case, split on anything not a letter / digit ("Hip-Hop/Rap" -> hip, hop, rap) */
#define GW 6
typedef struct { char w[GW][16]; int n; } words_t;
static void words_of(const char *g, words_t *o){
    o->n = 0;
    while(g && *g && o->n < GW){
        while(*g && !isalnum((unsigned char)*g)) g++;
        int k = 0; char *w = o->w[o->n];
        while(*g && isalnum((unsigned char)*g)){ if(k < 15) w[k++] = (char)tolower((unsigned char)*g); g++; }
        w[k] = 0;
        if(k >= 2 && strcmp(w, "and") && strcmp(w, "music")) o->n++;
    }
}
static int shared(const words_t *a, const words_t *b){
    int s = 0; for(int i = 0; i < a->n; i++) for(int j = 0; j < b->n; j++) if(!strcmp(a->w[i], b->w[j])) s++;
    return s;
}
static const char *artist_of(const mdb_song_t *s){ return s->artist_group[0] ? s->artist_group : s->artist; }

typedef struct { int i; float w; } cand_t;
static int by_weight(const void *a, const void *b){ float d = ((const cand_t *)b)->w - ((const cand_t *)a)->w; return d > 0 ? 1 : d < 0 ? -1 : 0; }

int vibe_pick(int seed_id, const char *seed_path, int *out, int want){
    if(want <= 0) return 0;
    if(!g_rnd) g_rnd = (unsigned)time(NULL);
    if(mdb_song_count() <= 0) mdb_load();
    int n = mdb_song_count(); if(n <= 0) return 0;
    const mdb_song_t *seed = NULL;
    for(int i = 0; i < n && !seed; i++){ const mdb_song_t *s = mdb_song(i); if(s && s->id == seed_id) seed = s; }
    mdb_song_t tmp;
    if(!seed && seed_path && mdb_song_by_path(seed_path, &tmp)) seed = &tmp;
    vibe_note_played(seed_id);
    float sx = 0, sy = 0; int have_spot = seed_path && sphere_ui_song_spot(seed_path, seed ? seed->album : NULL, seed ? artist_of(seed) : NULL, &sx, &sy);
    words_t sw = { .n = 0 }; if(seed) words_of(seed->genre, &sw);
    cand_t *c = malloc(sizeof *c * (size_t)n); if(!c) return 0;
    int nc = 0;
    /* SongSphere first: its own pick near the seed's spot (one query), each a strong match */
    int *near = NULL, nnear = 0; float r = 0;
    if(have_spot){ int ssdb_pick(float, float, int, int **, float *); nnear = ssdb_pick(sx, sy, 60, &near, &r); }
    for(int i = 0; i < n; i++){
        const mdb_song_t *s = mdb_song(i);
        if(!s || s->id == seed_id || heard(s->id)) continue;
        float w = 0.05f;
        for(int k = 0; k < nnear; k++) if(near[k] == s->id){ w += 6.0f - k * 0.05f; break; }
        if(seed){
            if(seed->genre[0] && !strcasecmp(seed->genre, s->genre)) w += 4.0f;
            else if(sw.n){ words_t cw; words_of(s->genre, &cw); w += 1.6f * (float)shared(&sw, &cw); }
            if(!strcasecmp(artist_of(seed), artist_of(s))) w += 1.2f;
            if(seed->album_h && seed->album_h == s->album_h) w -= 0.8f;            /* not the album again */
            if(seed->dur_ms > 0 && s->dur_ms > 0){ float q = (float)s->dur_ms / (float)seed->dur_ms; if(q > 0.6f && q < 1.6f) w += 0.4f; }
        }
        if(w <= 0.06f && nc > want * 8) continue;                               /* plenty of real matches: skip noise */
        c[nc].i = i; c[nc].w = w; nc++;
    }
    free(near);
    if(nc == 0){ free(c); if(g_nhist > 1){ g_nhist = 0; g_hpos = 0; return vibe_pick(seed_id, seed_path, out, want); } return 0; }   /* a small library: heard them all, start over */
    qsort(c, (size_t)nc, sizeof *c, by_weight);
    int pool = nc < 120 ? nc : 120;                                             /* among the closest, weighted */
    int got = 0; char al[8][MDB_STR]; int nal = 0; char ar[8][MDB_STR]; int arn[8] = {0}, nar = 0;
    for(int tries = 0; got < want && tries < want * 12; tries++){
        double tot = 0; for(int k = 0; k < pool; k++) if(c[k].w > 0) tot += c[k].w * c[k].w;
        if(tot <= 0) break;
        double t = (double)(rnd() % 100000) / 100000.0 * tot; int k = 0;
        for(; k < pool; k++){ if(c[k].w <= 0) continue; t -= c[k].w * c[k].w; if(t <= 0) break; }
        if(k >= pool) k = pool - 1;
        const mdb_song_t *s = mdb_song(c[k].i); c[k].w = 0;                     /* taken (or refused): not again */
        if(!s) continue;
        int dup = 0; for(int a = 0; a < nal; a++) if(!strcmp(al[a], s->album)) dup = 1;
        int ai = -1; for(int a = 0; a < nar; a++) if(!strcmp(ar[a], artist_of(s))) ai = a;
        if(dup || (ai >= 0 && arn[ai] >= 2)) continue;
        if(s->album[0] && nal < 8) snprintf(al[nal++], MDB_STR, "%s", s->album);
        if(ai < 0 && nar < 8){ snprintf(ar[nar], MDB_STR, "%s", artist_of(s)); arn[nar++] = 1; } else if(ai >= 0) arn[ai]++;
        out[got++] = s->id;
        if(got == pool) break;
    }
    free(c);
    if(got) vibe_note_played(out[0]);
    return got;
}
