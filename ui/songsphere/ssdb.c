/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SongSphere data. Its own file (sphere.db in the app's folder) holds the placements: one row per placed song, album
 * or artist (kind, key, x, y, when). Songs are keyed by file path, albums by album name (as the Library groups them),
 * artists by album artist, else artist. The library itself (song.db) is only read, never written. A song's spot is
 * its own, else its album's, else its artist's. */
#include "ssdb.h"
#include "sqlite3.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static sqlite3 *g_lib, *g_ss;                          /* the library (read-only, with ours attached), ours (writes) */
static int g_has_stats;                                /* PLAY_STATS exists: plays and recent plays are known */

static const char *g_lib_path, *g_ss_path;            /* set by ssdb_paths() (mq_ui), else the app's own */
void ssdb_paths(const char *lib, const char *ss){ g_lib_path = lib; g_ss_path = ss; }
static const char *lib_path(void){ const char *p = getenv("SS_LIB"); return p ? p : g_lib_path ? g_lib_path : "/usr/data/fiio/db/song.db"; }
static const char *ss_path(void){ const char *p = getenv("SS_DB"); return p ? p : g_ss_path ? g_ss_path : "/usr/data/apps/songsphere/sphere.db"; }

int ssdb_open(void){
    if(g_lib) return 1;
    if(sqlite3_open_v2(ss_path(), &g_ss, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) goto fail;
    sqlite3_busy_timeout(g_ss, 1500);
    if(sqlite3_exec(g_ss, "CREATE TABLE IF NOT EXISTS PLACE(KIND INTEGER NOT NULL, KEY TEXT NOT NULL, X REAL NOT NULL, "
                          "Y REAL NOT NULL, AT INTEGER NOT NULL, PRIMARY KEY(KIND, KEY)) WITHOUT ROWID;", 0, 0, 0) != SQLITE_OK) goto fail;
    /* (kopi.32) where a placed album / artist lives: its folders, so a placement follows its music when the tags change */
    sqlite3_exec(g_ss, "CREATE TABLE IF NOT EXISTS HOME(KIND INTEGER NOT NULL, KEY TEXT NOT NULL, DIR TEXT NOT NULL, "
                       "PRIMARY KEY(KIND, KEY, DIR)) WITHOUT ROWID;", 0, 0, 0);
    if(sqlite3_open_v2(lib_path(), &g_lib, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) goto fail;
    sqlite3_busy_timeout(g_lib, 1500);
    char *q = sqlite3_mprintf("ATTACH %Q AS ss;", ss_path());
    int rc = sqlite3_exec(g_lib, q, 0, 0, 0); sqlite3_free(q);
    if(rc != SQLITE_OK) goto fail;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(g_lib, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='PLAY_STATS';", -1, &st, NULL) == SQLITE_OK){
        g_has_stats = sqlite3_step(st) == SQLITE_ROW; sqlite3_finalize(st);
    }
    ssdb_relink();
    return 1;
fail:
    ssdb_close();
    return 0;
}
void ssdb_close(void){
    if(g_lib) sqlite3_close(g_lib);
    if(g_ss) sqlite3_close(g_ss);
    g_lib = g_ss = NULL;
}
static const char *col(sqlite3_stmt *st, int i){ const char *t = (const char *)sqlite3_column_text(st, i); return t ? t : ""; }

#define OWNER "COALESCE(NULLIF(ALBUM_ARTIST,''),ARTIST,'')"
int ssdb_items(int kind, ss_item_t **out){
    *out = NULL;
    if(!g_lib) return -1;
    char sql[1400];
    const char *grp = kind == SS_ALBUM ? "ALBUM" : OWNER;
    snprintf(sql, sizeof sql,
        "WITH A AS (SELECT %s AS K, MIN(O) AS MN, MAX(O) AS MX, COUNT(*) AS N FROM (SELECT ALBUM, ARTIST, ALBUM_ARTIST, %s AS O "
        "FROM SONG WHERE PATH NOT LIKE '%%.m4b') WHERE IFNULL(%s,'')<>'' GROUP BY 1)%s "
        "SELECT A.K, A.MN, A.MX, A.N, %s, X.X, X.Y FROM A %s LEFT JOIN ss.PLACE X ON X.KIND=%d AND X.KEY=A.K "
        "ORDER BY 5 DESC, A.K COLLATE NOCASE;",
        grp, OWNER, grp,
        g_has_stats ? (kind == SS_ALBUM ? ", P AS (SELECT S.ALBUM AS K, SUM(P.PLAYS) AS PL FROM PLAY_STATS P JOIN SONG S ON S.PATH=P.PATH GROUP BY 1)"
                                        : ", P AS (SELECT " "COALESCE(NULLIF(S.ALBUM_ARTIST,''),S.ARTIST,'')" " AS K, SUM(P.PLAYS) AS PL FROM PLAY_STATS P JOIN SONG S ON S.PATH=P.PATH GROUP BY 1)")
                    : "",
        g_has_stats ? "IFNULL(P.PL,0)" : "0", g_has_stats ? "LEFT JOIN P ON P.K=A.K" : "", kind);
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(g_lib, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    int cap = 256, n = 0; ss_item_t *v = malloc((size_t)cap * sizeof *v);
    while(v && sqlite3_step(st) == SQLITE_ROW){
        if(n == cap){ cap *= 2; ss_item_t *nv = realloc(v, (size_t)cap * sizeof *v); if(!nv){ free(v); v = NULL; break; } v = nv; }
        ss_item_t *it = &v[n++]; memset(it, 0, sizeof *it);
        snprintf(it->name, sizeof it->name, "%s", col(st, 0));
        if(kind == SS_ALBUM) snprintf(it->sub, sizeof it->sub, "%s", strcmp(col(st, 1), col(st, 2)) ? "Various Artists" : col(st, 1));
        it->songs = sqlite3_column_int(st, 3); it->plays = sqlite3_column_int(st, 4);
        it->placed = sqlite3_column_type(st, 5) != SQLITE_NULL;
        it->x = (float)sqlite3_column_double(st, 5); it->y = (float)sqlite3_column_double(st, 6);
    }
    sqlite3_finalize(st);
    if(!v) return -1;
    *out = v;
    return n;
}
int ssdb_get(int kind, const char *key, float *x, float *y){
    sqlite3_stmt *st; int ok = 0;
    if(!g_ss || sqlite3_prepare_v2(g_ss, "SELECT X,Y FROM PLACE WHERE KIND=?1 AND KEY=?2;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, kind); sqlite3_bind_text(st, 2, key, -1, SQLITE_STATIC);
    if(sqlite3_step(st) == SQLITE_ROW){ *x = (float)sqlite3_column_double(st, 0); *y = (float)sqlite3_column_double(st, 1); ok = 1; }
    sqlite3_finalize(st);
    return ok;
}
static void remember_homes(void);
int ssdb_put(int kind, const char *key, float x, float y){
    sqlite3_stmt *st; int ok = 0;
    if(!g_ss || sqlite3_prepare_v2(g_ss, "INSERT OR REPLACE INTO PLACE(KIND,KEY,X,Y,AT) VALUES(?1,?2,?3,?4,?5);", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, kind); sqlite3_bind_text(st, 2, key, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 3, roundf(x * 1000) / 1000); sqlite3_bind_double(st, 4, roundf(y * 1000) / 1000);
    sqlite3_bind_int64(st, 5, (long long)time(NULL));
    ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if(ok && kind != SS_SONG && g_lib) remember_homes();
    return ok;
}
int ssdb_del(int kind, const char *key){
    sqlite3_stmt *st; int ok = 0;
    if(!g_ss || sqlite3_prepare_v2(g_ss, "DELETE FROM PLACE WHERE KIND=?1 AND KEY=?2;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, kind); sqlite3_bind_text(st, 2, key, -1, SQLITE_STATIC);
    ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if(ok && sqlite3_prepare_v2(g_ss, "DELETE FROM HOME WHERE KIND=?1 AND KEY=?2;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int(st, 1, kind); sqlite3_bind_text(st, 2, key, -1, SQLITE_STATIC); sqlite3_step(st); sqlite3_finalize(st);
    }
    return ok;
}
int ssdb_coverage(int *placed, int *albums){
    sqlite3_stmt *st; *placed = *albums = 0;
    if(!g_lib || sqlite3_prepare_v2(g_lib, "SELECT COUNT(DISTINCT S.ALBUM), COUNT(DISTINCT X.KEY) FROM SONG S LEFT JOIN ss.PLACE X "
                                           "ON X.KIND=1 AND X.KEY=S.ALBUM WHERE IFNULL(S.ALBUM,'')<>'' AND S.PATH NOT LIKE '%.m4b';",
                                    -1, &st, NULL) != SQLITE_OK) return 0;
    if(sqlite3_step(st) == SQLITE_ROW){ *albums = sqlite3_column_int(st, 0); *placed = sqlite3_column_int(st, 1); }
    sqlite3_finalize(st);
    return 1;
}

/* ---- the playlist ---- */
typedef struct { int id; float d; unsigned alb, art; int recent, taken; } cand_t;
static unsigned hstr(const char *s){ unsigned h = 2166136261u; for(; *s; s++){ h ^= (unsigned char)*s; h *= 16777619u; } return h ? h : 1; }
#define HN 4096
typedef struct { unsigned k[HN]; unsigned char c[HN]; } counts_t;   /* how many picked per album / artist */
/* Only songs actually picked are counted (at most `want` keys, far below HN), so the table never fills; the probe is
 * bounded all the same. cnt_get: 0 for a key not seen; cnt_add: count one more (0 if, impossibly, the table is full). */
static int cnt_slot(const counts_t *t, unsigned k){
    unsigned i = k & (HN - 1);
    for(int n = 0; n < HN; n++, i = (i + 1) & (HN - 1)) if(!t->k[i] || t->k[i] == k) return (int)i;
    return -1;
}
static unsigned cnt_get(const counts_t *t, unsigned k){ int i = cnt_slot(t, k); return i >= 0 && t->k[i] == k ? t->c[i] : 0; }
static int cnt_add(counts_t *t, unsigned k){
    int i = cnt_slot(t, k); if(i < 0) return 0;
    t->k[i] = k; if(t->c[i] < 255) t->c[i]++;
    return 1;
}
static unsigned rnd_state;
static float frand(void){ rnd_state = rnd_state * 1103515245u + 12345u; return (float)((rnd_state >> 8) & 0xFFFFFF) / 16777216.0f; }

int ssdb_pick(float x, float y, int want, int **ids, float *radius_out){
    *ids = NULL; if(radius_out) *radius_out = 0;
    if(!g_lib || want <= 0) return 0;
    char sql[1200];
    snprintf(sql, sizeof sql,
        "SELECT S.ID, IFNULL(S.ALBUM,''), " "COALESCE(NULLIF(S.ALBUM_ARTIST,''),S.ARTIST,'')" ", COALESCE(a.X,b.X,c.X), COALESCE(a.Y,b.Y,c.Y), %s "
        "FROM SONG S LEFT JOIN ss.PLACE a ON a.KIND=0 AND a.KEY=S.PATH LEFT JOIN ss.PLACE b ON b.KIND=1 AND b.KEY=S.ALBUM "
        "LEFT JOIN ss.PLACE c ON c.KIND=2 AND c.KEY=COALESCE(NULLIF(S.ALBUM_ARTIST,''),S.ARTIST) %s "
        "WHERE S.PATH NOT LIKE '%%.m4b' AND COALESCE(a.X,b.X,c.X) IS NOT NULL;",
        g_has_stats ? "IFNULL(P.LAST_PLAYED,0)" : "0", g_has_stats ? "LEFT JOIN PLAY_STATS P ON P.PATH=S.PATH" : "");
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(g_lib, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    long long recent_after = (long long)time(NULL) - 3 * 86400;
    int cap = 1024, n = 0; cand_t *c = malloc((size_t)cap * sizeof *c);
    while(c && sqlite3_step(st) == SQLITE_ROW){
        if(n == cap){ cap *= 2; cand_t *nc = realloc(c, (size_t)cap * sizeof *c); if(!nc){ free(c); c = NULL; break; } c = nc; }
        float dx = (float)sqlite3_column_double(st, 3) - x, dy = (float)sqlite3_column_double(st, 4) - y;
        c[n].id = sqlite3_column_int(st, 0); c[n].d = sqrtf(dx * dx + dy * dy);
        c[n].alb = hstr(col(st, 1)); c[n].art = hstr(col(st, 2));
        long long last = sqlite3_column_int64(st, 5);
        c[n].recent = last > recent_after; c[n].taken = 0; n++;
    }
    sqlite3_finalize(st);
    if(!c || n == 0){ free(c); return 0; }
    rnd_state = (unsigned)time(NULL) ^ (unsigned)(x * 7919) ^ (unsigned)(y * 104729);
    int *out = malloc((size_t)want * sizeof *out); counts_t *t = calloc(2, sizeof *t);
    float *w = malloc((size_t)n * sizeof *w);
    if(!out || !t || !w){ free(out); free(t); free(w); free(c); return 0; }
    /* Look a little further each round until there are enough. Nearer songs are likelier, never certain (so the same
     * spot doesn't always give the same list); at most 3 per album and 4 per artist; songs played in the last three
     * days much less likely. If the caps leave too few even on the whole sphere, a last round drops them. */
    int got = 0; float r = 0.30f;
    for(int round = 0; got < want; round++){
        int capped = r < 2.0f;
        double total = 0; int avail = 0;
        for(int i = 0; i < n; i++){
            w[i] = 0;
            if(c[i].taken || c[i].d > r) continue;
            float k = 1 - c[i].d / (r + 1e-3f); w[i] = k * k + 0.05f;
            if(c[i].recent) w[i] *= 0.15f;
            total += w[i]; avail++;
        }
        while(got < want && total > 1e-9 && avail > 0){
            double pick = frand() * total; int i = 0;
            for(; i < n; i++){ if(w[i] <= 0) continue; pick -= w[i]; if(pick <= 0) break; }
            if(i >= n){ for(i = n - 1; i >= 0 && w[i] <= 0; i--); if(i < 0) break; }
            total -= w[i]; avail--; float wi = w[i]; w[i] = 0; (void)wi;
            if(capped && (cnt_get(&t[0], c[i].alb) >= 3 || cnt_get(&t[1], c[i].art) >= 4)) continue;   /* full for now: may come back in the last round */
            cnt_add(&t[0], c[i].alb); cnt_add(&t[1], c[i].art);         /* only what is taken is ever counted */
            c[i].taken = 1; out[got++] = c[i].id;
        }
        if(got >= want) break;
        if(r >= 2.0f){ if(!capped || round > 40) break; }
        r = r >= 2.0f ? 2.01f : fminf(2.0f, r + 0.12f);
    }
    free(t); free(w); free(c);
    if(radius_out) *radius_out = r > 2.0f ? 2.0f : r;
    if(!got){ free(out); return 0; }
    *ids = out;
    return got;
}

/* ---- placements that follow their music (kopi.32) ----
 * Albums and artists are placed by name. When a library update changes a name (tags edited on a PC, an album renamed,
 * "Artist" vs "artist"), the old placement would match nothing and SongSphere looked reset. So each placed album /
 * artist also remembers the folders its songs are in, and after every rescan a placement whose name has gone is moved
 * to the name the songs in those folders carry now: the same name in other letter case first, else the name most of
 * those songs share. Never onto a name that is already placed. Song placements are by file, so they need nothing. */
#define OWNER_S "COALESCE(NULLIF(S.ALBUM_ARTIST,''),S.ARTIST,'')"
static void dir_of(const char *path, char *out, size_t cap){
    snprintf(out, cap, "%s", path); char *sl = strrchr(out, '/'); if(sl && sl != out) *sl = 0; else out[0] = 0;
}
static void remember_homes(void){                                    /* placed and present: note their folders */
    sqlite3_stmt *st, *del = NULL, *ins = NULL;
    if(sqlite3_prepare_v2(g_lib,
        "SELECT * FROM (SELECT 1 AS K, S.ALBUM AS N, S.PATH FROM SONG S JOIN ss.PLACE P ON P.KIND=1 AND P.KEY=S.ALBUM "
        "WHERE S.PATH NOT LIKE '%.m4b' UNION ALL SELECT 2, " OWNER_S ", S.PATH FROM SONG S JOIN ss.PLACE P ON P.KIND=2 AND "
        "P.KEY=" OWNER_S " WHERE S.PATH NOT LIKE '%.m4b') ORDER BY 1, 2, 3;",        /* one pass over the library */
        -1, &st, NULL) != SQLITE_OK) return;
    if(sqlite3_prepare_v2(g_ss, "DELETE FROM HOME WHERE KIND=?1 AND KEY=?2;", -1, &del, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(g_ss, "INSERT OR IGNORE INTO HOME(KIND,KEY,DIR) VALUES(?1,?2,?3);", -1, &ins, NULL) != SQLITE_OK){
        sqlite3_finalize(st); sqlite3_finalize(del); sqlite3_finalize(ins); return;
    }
    /* collect first (the read handle sees our file attached; write through the other one after the read is done) */
    typedef struct { int kind; char key[200], dir[400]; } row_t;
    int cap = 256, n = 0; row_t *v = malloc((size_t)cap * sizeof *v);
    char lastkey[200] = ""; int lastkind = -1, ndirs = 0; char lastdir[400] = "";
    while(v && sqlite3_step(st) == SQLITE_ROW){
        int k = sqlite3_column_int(st, 0); const char *key = col(st, 1); char d[400]; dir_of(col(st, 2), d, sizeof d);
        if(!d[0]) continue;
        if(k != lastkind || strcmp(key, lastkey)){ lastkind = k; snprintf(lastkey, sizeof lastkey, "%s", key); ndirs = 0; lastdir[0] = 0; }
        if(!strcmp(d, lastdir) || ndirs >= 24) continue;                           /* a few folders are plenty */
        int dup = 0; for(int i = n - ndirs; i < n; i++) if(!strcmp(v[i].dir, d)){ dup = 1; break; }
        snprintf(lastdir, sizeof lastdir, "%s", d);
        if(dup) continue;
        if(n == cap){ cap *= 2; row_t *nv = realloc(v, (size_t)cap * sizeof *v); if(!nv) break; v = nv; }
        v[n].kind = k; snprintf(v[n].key, sizeof v[n].key, "%s", key); snprintf(v[n].dir, sizeof v[n].dir, "%s", d); n++; ndirs++;
    }
    sqlite3_finalize(st);
    if(v && n){
        sqlite3_exec(g_ss, "BEGIN;", 0, 0, 0);
        for(int i = 0; i < n; i++){
            if(i == 0 || v[i].kind != v[i-1].kind || strcmp(v[i].key, v[i-1].key)){                  /* fresh list per name */
                sqlite3_reset(del); sqlite3_bind_int(del, 1, v[i].kind); sqlite3_bind_text(del, 2, v[i].key, -1, SQLITE_STATIC); sqlite3_step(del);
            }
            sqlite3_reset(ins); sqlite3_bind_int(ins, 1, v[i].kind); sqlite3_bind_text(ins, 2, v[i].key, -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 3, v[i].dir, -1, SQLITE_STATIC); sqlite3_step(ins);
        }
        sqlite3_exec(g_ss, "COMMIT;", 0, 0, 0);
    }
    free(v); sqlite3_finalize(del); sqlite3_finalize(ins);
}
/* the name the songs in a placement's folders carry now ("" none) */
static void new_name(int kind, const char *old, char *out, size_t cap){
    out[0] = 0;
    sqlite3_stmt *st;
    const char *same = kind == SS_ALBUM ? "SELECT ALBUM FROM SONG WHERE ALBUM=?1 COLLATE NOCASE AND TRIM(ALBUM)<>'' LIMIT 1;"
                                        : "SELECT " OWNER_S " FROM SONG S WHERE " OWNER_S "=?1 COLLATE NOCASE AND TRIM(" OWNER_S ")<>'' LIMIT 1;";
    if(sqlite3_prepare_v2(g_lib, same, -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, old, -1, SQLITE_STATIC);
        if(sqlite3_step(st) == SQLITE_ROW) snprintf(out, cap, "%s", col(st, 0));
        sqlite3_finalize(st);
        if(out[0]) return;
    }
    char q[700];
    snprintf(q, sizeof q, "SELECT %s AS K, COUNT(*) FROM ss.HOME H JOIN SONG S ON substr(S.PATH,1,length(H.DIR)+1)=H.DIR||'/' "
             "AND instr(substr(S.PATH,length(H.DIR)+2),'/')=0 WHERE H.KIND=?1 AND H.KEY=?2 AND S.PATH NOT LIKE '%%.m4b' "
             "AND TRIM(K)<>'' GROUP BY 1 ORDER BY 2 DESC LIMIT 1;", kind == SS_ALBUM ? "S.ALBUM" : OWNER_S);
    if(sqlite3_prepare_v2(g_lib, q, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(st, 1, kind); sqlite3_bind_text(st, 2, old, -1, SQLITE_STATIC);
    if(sqlite3_step(st) == SQLITE_ROW) snprintf(out, cap, "%s", col(st, 0));
    sqlite3_finalize(st);
}
int ssdb_relink(void){
    if(!g_lib || !g_ss) return 0;
    /* placements whose name the library no longer has */
    typedef struct { int kind; char key[200]; } gone_t;
    gone_t *g = NULL; int ng = 0, cap = 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(g_lib, "SELECT KIND, KEY FROM ss.PLACE WHERE (KIND=1 AND KEY NOT IN (SELECT ALBUM FROM SONG WHERE ALBUM "
                                 "IS NOT NULL)) OR (KIND=2 AND KEY NOT IN (SELECT " OWNER_S " FROM SONG S));", -1, &st, NULL) != SQLITE_OK) return 0;
    while(sqlite3_step(st) == SQLITE_ROW){
        int k = sqlite3_column_int(st, 0); const char *key = col(st, 1);
        if(ng == cap){ cap = cap ? cap * 2 : 16; gone_t *ngp = realloc(g, (size_t)cap * sizeof *g); if(!ngp) break; g = ngp; }
        g[ng].kind = k; snprintf(g[ng].key, sizeof g[ng].key, "%s", key); ng++;
    }
    sqlite3_finalize(st);
    int moved = 0;
    for(int i = 0; i < ng; i++){
        char nn[200]; new_name(g[i].kind, g[i].key, nn, sizeof nn);
        if(!nn[0] || !strcmp(nn, g[i].key)) continue;
        float x, y; if(ssdb_get(g[i].kind, nn, &x, &y)) continue;                  /* the new name has its own spot */
        sqlite3_stmt *u;
        if(sqlite3_prepare_v2(g_ss, "UPDATE PLACE SET KEY=?3 WHERE KIND=?1 AND KEY=?2;", -1, &u, NULL) != SQLITE_OK) continue;
        sqlite3_bind_int(u, 1, g[i].kind); sqlite3_bind_text(u, 2, g[i].key, -1, SQLITE_STATIC); sqlite3_bind_text(u, 3, nn, -1, SQLITE_STATIC);
        if(sqlite3_step(u) == SQLITE_DONE && sqlite3_changes(g_ss) > 0) moved++;
        sqlite3_finalize(u);
        if(sqlite3_prepare_v2(g_ss, "UPDATE OR REPLACE HOME SET KEY=?3 WHERE KIND=?1 AND KEY=?2;", -1, &u, NULL) == SQLITE_OK){
            sqlite3_bind_int(u, 1, g[i].kind); sqlite3_bind_text(u, 2, g[i].key, -1, SQLITE_STATIC); sqlite3_bind_text(u, 3, nn, -1, SQLITE_STATIC);
            sqlite3_step(u); sqlite3_finalize(u);
        }
    }
    free(g);
    remember_homes();                                                    /* then note where everything placed lives now */
    return moved;
}
