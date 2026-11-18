/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Spotify helper program (kopi.35): "spot-disc <name> <device_id> <cache_dir> [client_id]", its own static binary
 * (make spot-disc; kept out of mq_ui so updates fit in /usr/data). mq_ui copies it from the SD card into /tmp/spot.
 * The Spotify side (Connect, sign-in, decoding, lists) is the Rust library in spotify/ (librespot); this file is its
 * audio out: librespot's player thread hands 16-bit stereo PCM to spot_c_pcm(), which paces it in real time into the
 * Disc's own AirPlay receiver on 127.0.0.1 (mq_player owns the audio hardware - the same road as Music Assistant). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "version.h"
#include "ss_airplay.h"

#define SP_LEAD_US 500000LL                       /* audio sent ahead of the clock: the receiver's cushion */

static airplay_t g_ap;
static long long g_t0, g_frames;                  /* when the stream's frame 0 was due, frames sent since */
static int16_t g_pkt[AP_FRAMES * 2]; static int g_fill;
static long long g_next_try;                      /* no receiver: when to try again */

static long long sp_now_us(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }
static void sp_sleep_us(long long us){ if(us <= 0) return; struct timespec t = { us / 1000000, (us % 1000000) * 1000 }; while(nanosleep(&t, &t) < 0 && errno == EINTR){} }

static int sp_open(void){
    if(g_ap.open) return 0;
    long long now = sp_now_us();
    if(now < g_next_try) return -1;
    if(ap_open(&g_ap) < 0){ g_next_try = now + 3000000; fprintf(stderr, "spot: AirPlay receiver not answering (is the Disc in AirPlay mode?)\n"); return -1; }
    g_t0 = sp_now_us(); g_frames = 0;
    return 0;
}
static void sp_send_packet(void){
    long long due = g_t0 + g_frames * 1000000LL / AP_RATE, now = sp_now_us();
    if(now - due > 300000){ g_t0 = now - g_frames * 1000000LL / AP_RATE; due = now; }   /* fell behind (a stall): carry on from now */
    if(due - now > SP_LEAD_US) sp_sleep_us(due - now - SP_LEAD_US);                       /* ahead: wait, so this runs in real time */
    if(ap_send(&g_ap, g_pkt) < 0){ ap_close(&g_ap); g_ap.open = 0; }
    g_frames += AP_FRAMES;
}
/* librespot's player thread: start, stop and the samples all come from it, one at a time */
int spot_c_pcm(const int16_t *pcm, size_t frames){
    if(sp_open() < 0){ sp_sleep_us((long long)frames * 1000000LL / AP_RATE); return -1; }   /* keep time, so the song still moves */
    while(frames > 0){
        size_t k = AP_FRAMES - g_fill; if(k > frames) k = frames;
        memcpy(g_pkt + 2 * g_fill, pcm, k * 4);
        g_fill += (int)k; pcm += 2 * k; frames -= k;
        if(g_fill == AP_FRAMES){ sp_send_packet(); g_fill = 0; }
    }
    return 0;
}
void spot_c_audio(int ev){
    if(ev){ g_t0 = sp_now_us(); g_frames = 0; g_fill = 0; return; }   /* start: a fresh clock */
    g_fill = 0;
    if(g_ap.open) ap_flush(&g_ap);                                      /* stop / pause / seek: what the receiver holds goes */
}

int spot_rs_main(const char *name, const char *device_id, const char *cache_dir, const char *client_id);
int spotify_main(int argc, char **argv){
    setvbuf(stderr, NULL, _IOLBF, 0);
    if(argc < 4){ fprintf(stderr, "usage: %s <name> <device_id> <cache_dir> [client_id]\n", argv[0]); return 2; }
    signal(SIGPIPE, SIG_IGN);
    g_ap.port = 5000; g_ap.rtsp = g_ap.aud = g_ap.ctl = g_ap.tim = -1; g_ap.vol_db = 0.0;   /* full scale: Spotify's volume is applied before */
    mkdir("/tmp/spot", 0755);
    fprintf(stderr, DISCO_NAME " " DISCO_VERSION " spotify: \"%s\", cache %s\n", argv[1], argv[3]);
    int rc = spot_rs_main(argv[1], argv[2], argv[3], argc > 4 ? argv[4] : "");
    if(g_ap.open) ap_close(&g_ap);
    unlink("/tmp/spot/status");
    return rc;
}
#ifdef SPOT_STANDALONE
int main(int argc, char **argv){ return spotify_main(argc, argv); }
#endif
