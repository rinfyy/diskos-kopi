/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* video_disc.c - Videos: plays a video file full screen, run as `mq_ui --video <file>`, a child that owns the screen
 * until it exits, like the games.
 *
 * Files: MP4 / MOV / M4V, MKV / WebM, AVI and MPEG (.mpg / .ts), with H.264, H.265, MPEG-4 (Xvid / DivX), MPEG-1/2,
 * VP8 / VP9 pictures and AAC, MP3, AC-3, Opus, Vorbis, FLAC or MP2 sound - decoded by a small static FFmpeg build
 * (tools/build_ffmpeg.sh, LGPL), on the player's own CPU with no hardware help. Each picture is scaled straight to
 * the size it's shown at, and when decoding falls behind the clock it first skips H.264 / H.265's smoothing filter,
 * then pictures nothing depends on, rather than letting the picture drift; large videos (720p and up) are best
 * shrunk first - Files' README has the one-line command. Phone videos turned on their side are shown upright.
 * The picture keeps its shape inside the round screen (Fit) or fills the circle (Fill).
 *
 * Sound: mq_player owns the audio hardware, so - like MA Sendspin - the sound goes to the Disc's own AirPlay receiver
 * (ss_airplay.h); mq_ui switches the Disc to AirPlay for the video and back after. The receiver holds about two
 * seconds of sound, so the picture runs that far behind what's sent and stays in step. Without the receiver the
 * video plays silently.
 *
 * Touch: a tap shows the controls for 4 s - play / pause in the middle, back / forward 10 s either side, Fit / Fill,
 * close at the top - and the rim's dots are the progress (tap the rim to jump there). Vol-Up held also closes. */
#define _GNU_SOURCE
#include "ss_airplay.h"
#include "games/platform.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <ctype.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define LAT_S   2.0                                    /* the AirPlay receiver's hold-back (sendspin.c's default) */
#define CTRL_S  4.0

static int rot, fb = -1; static unsigned char *fbmap; static struct fb_var_screeninfo vi; static struct fb_fix_screeninfo fi; static size_t fboff;
static volatile int quit;
static void on_sig(int s){ (void)s; quit = 1; }

/* ---- screen ------------------------------------------------------------------------------------------------------ */
static int open_fb(void){
    fb = open("/dev/fb0", O_RDWR | O_CLOEXEC); if(fb < 0) return -1;
    if(ioctl(fb, FBIOGET_VSCREENINFO, &vi) < 0 || ioctl(fb, FBIOGET_FSCREENINFO, &fi) < 0 || vi.xres != GW || vi.yres != GH ||
       vi.bits_per_pixel != 32){ close(fb); fb = -1; return -1; }
    fboff = (size_t)vi.yoffset * fi.line_length + (size_t)vi.xoffset * 4;
    fbmap = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if(fbmap == MAP_FAILED){ close(fb); fb = -1; return -1; }
    return 0;
}
static void present(void){
    if(fb < 0) return;
    int first, stepx, rowstep;
    switch(rot){ case 1: first = GW - 1; stepx = GW; rowstep = -1; break; case 2: first = 0; stepx = 1; rowstep = GW; break;
                 case 3: first = (GH - 1) * GW; stepx = -GW; rowstep = 1; break; default: first = GW * GH - 1; stepx = -1; rowstep = -GW; break; }
    int native = vi.red.offset == 16 && vi.green.offset == 8 && vi.blue.offset == 0;
    for(int y = 0; y < GH; y++, first += rowstep){
        uint32_t *dst = (uint32_t *)(fbmap + fboff + (size_t)y * fi.line_length);
        for(int x = 0, i = first; x < GW; x++, i += stepx){
            uint32_t c = g_frame[i];
            dst[x] = native ? c : ((c >> 16 & 255) << vi.red.offset) | ((c >> 8 & 255) << vi.green.offset) | ((c & 255) << vi.blue.offset);
        }
    }
}
static void write_ppm(const char *path){
    FILE *f = fopen(path, "wb"); if(!f) return;
    fprintf(f, "P6\n%d %d\n255\n", GW, GH);
    for(int i = 0; i < GW * GH; i++){ unsigned char p[3] = { g_frame[i] >> 16, g_frame[i] >> 8, g_frame[i] }; fwrite(p, 1, 3, f); }
    fclose(f);
}

/* ---- touch (one finger) ------------------------------------------------------------------------------------------ */
static int tfd = -1, tmt, txmin, txmax, tymin, tymax, tslot, t_down, t_was, t_rx, t_ry, t_x, t_y, t_x0, t_y0;
static int axis(int v, int lo, int hi){ if(hi <= lo){ lo = 0; hi = 359; } if(v < lo) v = lo; if(v > hi) v = hi; return (int)(((int64_t)v - lo) * 359 / ((int64_t)hi - lo)); }
static void from_raw(int rx, int ry, int *x, int *y){
    switch(rot){ case 1: *x = GW - 1 - ry; *y = rx; break; case 2: *x = rx; *y = ry; break;
                 case 3: *x = ry; *y = GW - 1 - rx; break; default: *x = GW - 1 - rx; *y = GH - 1 - ry; break; }
}
static int open_touch(void){
    for(int i = 0; i < 32; i++){
        char path[40]; snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if(fd < 0) continue;
        struct input_absinfo ax, ay; int mt = 1;
        if(ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) < 0 || ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) < 0){
            mt = 0;
            if(ioctl(fd, EVIOCGABS(ABS_X), &ax) < 0 || ioctl(fd, EVIOCGABS(ABS_Y), &ay) < 0){ close(fd); continue; }
        }
        if(ax.maximum <= ax.minimum || ay.maximum <= ay.minimum){ close(fd); continue; }
        tfd = fd; tmt = mt; txmin = ax.minimum; txmax = ax.maximum; tymin = ay.minimum; tymax = ay.maximum;
        return 0;
    }
    return -1;
}
static void tap(int x, int y);
static void read_touch(void){
    if(tfd < 0) return;
    struct input_event ev[64]; ssize_t n;
    while((n = read(tfd, ev, sizeof ev)) > 0) for(size_t i = 0; i < (size_t)n / sizeof ev[0]; i++){
        struct input_event *e = &ev[i];
        if(e->type == EV_ABS && e->code == ABS_MT_SLOT){ tslot = e->value; continue; }
        if(e->type == EV_ABS && tslot == 0){
            if(e->code == (tmt ? ABS_MT_POSITION_X : ABS_X)) t_rx = e->value;
            else if(e->code == (tmt ? ABS_MT_POSITION_Y : ABS_Y)) t_ry = e->value;
            else if(e->code == ABS_MT_TRACKING_ID) t_down = e->value >= 0;
            continue;
        }
        if(e->type == EV_KEY && e->code == BTN_TOUCH && !tmt){ t_down = e->value != 0; continue; }
        if(e->type != EV_SYN || e->code != SYN_REPORT) continue;
        from_raw(axis(t_rx, txmin, txmax), axis(t_ry, tymin, tymax), &t_x, &t_y);
        if(t_down && !t_was){ t_x0 = t_x; t_y0 = t_y; }
        if(!t_down && t_was && abs(t_x - t_x0) < 24 && abs(t_y - t_y0) < 24) tap(t_x0, t_y0);
        t_was = t_down;
    }
}

/* ---- playback ---------------------------------------------------------------------------------------------------- */
static double dur, t0, pause_pos, ctrl_until, lat;
static int paused, ended, fill, fw, fh, have_pic, dirty = 1;
static uint32_t *pic;                                  /* the last frame shown, XRGB, at the size it's shown */
static airplay_t ap; static int ap_ok;
#define FIFO (44100 * 4)
static int16_t fifo[FIFO * 2]; static int f_r, f_w; static double f_time; static int f_started;   /* f_time: media time of f_r */

static double media_now(void){ return paused ? pause_pos : gp_now() - t0; }
static int fifo_len(void){ return (f_w - f_r + FIFO) % FIFO; }

/* the file: FFmpeg demuxes into two packet queues, so the picture and the sound (which runs ~2.4 s ahead of it) are
 * each decoded when they're wanted */
typedef struct pq { AVPacket *p[1024]; int r, n; } pq_t;
static AVFormatContext *fx; static AVCodecContext *vc, *ac; static int vs = -1, as = -1, src_eof, v_eof, a_eof;
static struct SwsContext *sws; static SwrContext *swr; static AVFrame *vfn, *vfc, *af; static pq_t vq, aq;   /* vfn: the next picture; vfc: the one to show */
static int rot90;                                      /* a quarter turn to stand the picture up (1 clockwise, 2 anticlockwise) */
static uint32_t *tmp;                                  /* the scaled picture before a quarter turn */
static double v_next = -1, last_vpts = -1;             /* the decoded picture waiting for its time, and its time */
static int v_have, drop_level;                         /* 0 everything, 1 no deblocking, 2 also no unreferenced pictures */

static void pq_clear(pq_t *q){ while(q->n){ av_packet_free(&q->p[q->r]); q->r = (q->r + 1) % 1024; q->n--; } q->r = 0; }
static int pq_push(pq_t *q, AVPacket *pk){ if(q->n >= 1024) return 0; q->p[(q->r + q->n) % 1024] = pk; q->n++; return 1; }
static AVPacket *pq_pop(pq_t *q){ if(!q->n) return NULL; AVPacket *pk = q->p[q->r]; q->r = (q->r + 1) % 1024; q->n--; return pk; }
static int demux_one(void){                                           /* read a packet into its queue; 0 at the end */
    if(src_eof) return 0;
    AVPacket *pk = av_packet_alloc(); if(!pk) return 0;
    if(av_read_frame(fx, pk) < 0){ av_packet_free(&pk); src_eof = 1; return 0; }
    if(pk->stream_index == vs && pq_push(&vq, pk)) return 1;
    if(pk->stream_index == as && ac && ap_ok && pq_push(&aq, pk)) return 1;
    av_packet_free(&pk); return 1;
}
static double ts_of(AVFrame *f, int st){
    int64_t t = f->best_effort_timestamp; if(t == AV_NOPTS_VALUE) t = f->pts;
    if(t == AV_NOPTS_VALUE) return -1;
    double s0 = fx->streams[st]->start_time != AV_NOPTS_VALUE ? fx->streams[st]->start_time * av_q2d(fx->streams[st]->time_base) : 0;
    return t * av_q2d(fx->streams[st]->time_base) - s0;
}
static int video_decode_next(void){                                   /* one more picture into vf; 0 when there are none */
    for(;;){
        int r = avcodec_receive_frame(vc, vfn);
        if(r == 0){ double t = ts_of(vfn, vs); if(t < 0) t = last_vpts < 0 ? 0 : last_vpts + 1.0 / 30; last_vpts = t; v_next = t; return 1; }
        if(r == AVERROR_EOF){ v_eof = 1; return 0; }
        if(r != AVERROR(EAGAIN)) return 0;
        AVPacket *pk;
        while(!(pk = pq_pop(&vq))) if(!demux_one()) break;
        if(pk){ avcodec_send_packet(vc, pk); av_packet_free(&pk); } else avcodec_send_packet(vc, NULL);   /* the end: drain */
    }
}
static void set_drop(int lv){
    if(lv == drop_level) return;
    drop_level = lv;
    vc->skip_loop_filter = lv >= 1 ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
    vc->skip_frame = lv >= 2 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
}
static void show_frame(AVFrame *vf){                                  /* a picture -> pic, scaled to its shown size */
    int w = vf->width, h = vf->height;
    double sar = vf->sample_aspect_ratio.num > 0 && vf->sample_aspect_ratio.den > 0 ? av_q2d(vf->sample_aspect_ratio) : 1;
    double a = w * sar / h; if(rot90) a = 1 / a;                        /* the shape it's seen in */
    int W, H;
    if(fill){ if(a >= 1){ H = GH; W = (int)(GH * a); } else { W = GW; H = (int)(GW / a); } }
    else { H = (int)(360.0 / sqrt(1 + a * a)); W = (int)(H * a); }
    if(W < 2) W = 2;
    if(H < 2) H = 2;
    int dw = rot90 ? H : W, dh = rot90 ? W : H;                         /* what swscale makes, before the turn */
    sws = sws_getCachedContext(sws, w, h, vf->format, dw, dh, AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if(!sws) return;
    if(!pic || fw != W || fh != H){ free(pic); free(tmp); fw = W; fh = H; pic = malloc((size_t)W * H * 4); tmp = rot90 ? malloc((size_t)W * H * 4) : NULL; }
    if(!pic || (rot90 && !tmp)) return;
    uint8_t *dst[4] = { (uint8_t *)(rot90 ? tmp : pic), NULL, NULL, NULL }; int ls[4] = { dw * 4, 0, 0, 0 };
    sws_scale(sws, (const uint8_t *const *)vf->data, vf->linesize, 0, h, dst, ls);
    if(rot90 == 1)      for(int y = 0; y < dh; y++) for(int x = 0; x < dw; x++) pic[x * W + (W - 1 - y)] = tmp[y * dw + x];   /* clockwise */
    else if(rot90 == 2) for(int y = 0; y < dh; y++) for(int x = 0; x < dw; x++) pic[(H - 1 - x) * W + y] = tmp[y * dw + x];   /* anticlockwise */
    have_pic = 1; dirty = 1;
}
static double shown_t = -1;                                          /* the time of the picture on screen */
static void decode_video(double m){
    /* the newest picture that's due is the one shown; any due before it are decoded (later ones need them) but not
     * scaled. Falling behind turns on the decoder's shortcuts; keeping up for a while turns them off again. */
    static int bad, good;
    int cand = 0, skipped = 0; double cand_t = 0;
    for(;;){
        if(!v_have){ if(!video_decode_next()){ if(v_eof) ended = !cand; break; } v_have = 1; }
        if(v_next > m) break;
        if(cand) skipped++;
        AVFrame *t = vfc; vfc = vfn; vfn = t; cand = 1; cand_t = v_next; v_have = 0;
    }
    if(!cand) return;
    show_frame(vfc); shown_t = cand_t;
    if(skipped || m - cand_t > 0.12){ good = 0; if(++bad >= 3){ bad = 0; if(drop_level < 2) set_drop(drop_level + 1); } }
    else { bad = 0; if(++good >= 90){ good = 0; if(drop_level > 0) set_drop(drop_level - 1); } }
}
static void push_audio(void){                                         /* decoded sound -> 44.1 kHz stereo -> the fifo */
    int out_max = av_rescale_rnd(swr_get_delay(swr, ac->sample_rate) + af->nb_samples, 44100, ac->sample_rate, AV_ROUND_UP);
    if(out_max <= 0) return;
    int16_t *buf = malloc((size_t)out_max * 4); if(!buf) return;
    uint8_t *o[1] = { (uint8_t *)buf };
    int n = swr_convert(swr, o, out_max, (const uint8_t **)af->extended_data, af->nb_samples);
    if(n > 0 && !f_started){ double t = ts_of(af, as); f_time = t < 0 ? 0 : t; f_started = 1; }
    for(int i = 0; i < n && fifo_len() < FIFO - 1; i++){ fifo[2 * f_w] = buf[2 * i]; fifo[2 * f_w + 1] = buf[2 * i + 1]; f_w = (f_w + 1) % FIFO; }
    free(buf);
}
static void feed_audio(double m){
    if(!ap_ok || !ac) return;
    /* keep the sound decoded up to ~0.4 s past what's due to be sent */
    while(!a_eof && (!f_started || f_time + (double)fifo_len() / 44100.0 <= m + lat + 0.4)){
        int r = avcodec_receive_frame(ac, af);
        if(r == 0){
            double t = ts_of(af, as);
            if(t >= 0 && t + (double)af->nb_samples / ac->sample_rate < m - 0.2 && !f_started) continue;   /* behind (after a seek) */
            push_audio(); continue;
        }
        if(r == AVERROR_EOF){ a_eof = 1; break; }
        AVPacket *pk;
        while(!(pk = pq_pop(&aq))) if(!demux_one()) break;
        if(pk){ avcodec_send_packet(ac, pk); av_packet_free(&pk); } else { avcodec_send_packet(ac, NULL); }
        if(!pk && src_eof && !aq.n){ /* drained below */ }
        if(vq.n >= 1000) break;                                        /* (a file with sound far from its picture) */
    }
    double now = gp_now();
    while(fifo_len() >= AP_FRAMES){                                     /* each packet goes when its time minus the hold-back comes */
        double play_at = t0 + f_time;
        if(play_at - lat > now + 0.05) break;
        int16_t pk2[AP_FRAMES * 2];
        for(int i = 0; i < AP_FRAMES; i++){ pk2[2 * i] = fifo[2 * f_r]; pk2[2 * i + 1] = fifo[2 * f_r + 1]; f_r = (f_r + 1) % FIFO; }
        if(play_at > now - 0.2) ap_send(&ap, pk2);                      /* too late to hear: skip it */
        f_time += (double)AP_FRAMES / 44100.0;
    }
}
static void restart_at(double t){                              /* after a seek, a resume or the start */
    if(t < 0) t = 0;
    if(dur > 0 && t > dur - 0.5) t = dur > 0.5 ? dur - 0.5 : 0;
    av_seek_frame(fx, -1, (int64_t)(t * AV_TIME_BASE) + (fx->start_time != AV_NOPTS_VALUE ? fx->start_time : 0), AVSEEK_FLAG_BACKWARD);
    pq_clear(&vq); pq_clear(&aq); src_eof = v_eof = a_eof = 0; v_have = 0; last_vpts = -1;
    avcodec_flush_buffers(vc); if(ac) avcodec_flush_buffers(ac);
    if(swr) swr_init(swr);
    f_r = f_w = 0; f_started = 0;
    if(ap_ok) ap_flush(&ap);
    t0 = gp_now() + lat - t; ended = 0; dirty = 1;
}
static void set_paused(int p){
    if(p == paused) return;
    if(p){ pause_pos = media_now(); paused = 1; if(ap_ok) ap_flush(&ap); f_r = f_w = 0; f_started = 0; }
    else { paused = 0; restart_at(pause_pos); }          /* resume = start again from here (the receiver dropped what it held) */
    dirty = 1;
}
static int big_w, big_h;                                                   /* a video too big to play: its size */
static int open_file(const char *path){
    if(avformat_open_input(&fx, path, NULL, NULL) < 0) return -1;
    /* (MP4 and MKV give the picture size in their header: a 4K film is turned away here, before probing it decodes a
     * frame - which alone can take more memory than the Disc has) */
    { int ok = 0, w = 0, h = 0;
      for(unsigned i = 0; i < fx->nb_streams; i++){ AVCodecParameters *c = fx->streams[i]->codecpar;
          if(c->codec_type != AVMEDIA_TYPE_VIDEO || (fx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
          if(!c->width || (long)c->width * c->height <= 1920L * 1088) ok = 1; else { w = c->width; h = c->height; } }
      if(!ok && w){ big_w = w; big_h = h; return -3; } }
    fx->max_analyze_duration = 2 * AV_TIME_BASE;
    if(avformat_find_stream_info(fx, NULL) < 0) return -1;
    const AVCodec *vcd = NULL, *acd = NULL;
    vs = av_find_best_stream(fx, AVMEDIA_TYPE_VIDEO, -1, -1, &vcd, 0);
    if(vs < 0 || !vcd) return -2;                                       /* no picture we can decode */
    vc = avcodec_alloc_context3(vcd); if(!vc) return -1;
    avcodec_parameters_to_context(vc, fx->streams[vs]->codecpar);
    /* Too big for the Disc? A 4K picture is 12 MB a frame (24 MB at 10 bits) and H.264 / H.265 keep up to 16 of them,
     * which is more memory than the whole player has - the decoder would be killed half-way through opening. Work
     * out what the decoder will hold and compare it with the memory that's free, and say so instead. */
    { long px = (long)vc->width * vc->height, bpp = vc->bits_per_raw_sample > 8 || strstr(av_get_pix_fmt_name(vc->pix_fmt) ? av_get_pix_fmt_name(vc->pix_fmt) : "", "10") ? 3 : 2;
      long need = px * bpp / 2 * 3 / 2 * 10 / 1024;                          /* KiB: ~10 pictures in flight, 4:2:0 */
      long avail = 0; FILE *m = fopen("/proc/meminfo", "r");
      if(m){ char l[128]; while(fgets(l, sizeof l, m)) if(sscanf(l, "MemAvailable: %ld", &avail) == 1) break; fclose(m); }
      big_w = vc->width; big_h = vc->height;
      if(px > 1920L * 1088 || (avail > 0 && need > avail * 3 / 4)) return -3; }
    vc->thread_count = 2; vc->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;   /* the Disc's two cores, no more frames in flight */
    if((long)vc->width * vc->height > 1280L * 720){ vc->skip_loop_filter = AVDISCARD_ALL; vc->flags2 |= AV_CODEC_FLAG2_FAST; }   /* 1080p: every shortcut */
    if(avcodec_open2(vc, vcd, NULL) < 0) return -2;
    as = av_find_best_stream(fx, AVMEDIA_TYPE_AUDIO, -1, vs, &acd, 0);
    if(as >= 0 && acd){
        ac = avcodec_alloc_context3(acd);
        if(ac){ avcodec_parameters_to_context(ac, fx->streams[as]->codecpar); if(avcodec_open2(ac, acd, NULL) < 0) avcodec_free_context(&ac); }
    }
    for(unsigned i = 0; i < fx->nb_streams; i++) if((int)i != vs && (int)i != as) fx->streams[i]->discard = AVDISCARD_ALL;
    /* a phone held upright stores the picture on its side with a turn to apply */
    const AVPacketSideData *sd = av_packet_side_data_get(fx->streams[vs]->codecpar->coded_side_data, fx->streams[vs]->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if(sd && sd->size >= 9 * 4){ double r = av_display_rotation_get((const int32_t *)sd->data); if(fabs(r + 90) < 10) rot90 = 1; else if(fabs(r - 90) < 10) rot90 = 2; }
    dur = fx->duration != AV_NOPTS_VALUE ? fx->duration / (double)AV_TIME_BASE : 0;
    vfn = av_frame_alloc(); vfc = av_frame_alloc(); af = av_frame_alloc();
    return vfn && vfc && af ? 0 : -1;
}
static void audio_setup(void){
    if(!ac) return;
    AVChannelLayout st = AV_CHANNEL_LAYOUT_STEREO;
    if(swr_alloc_set_opts2(&swr, &st, AV_SAMPLE_FMT_S16, 44100, &ac->ch_layout, ac->sample_fmt, ac->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0){
        swr_free(&swr); avcodec_free_context(&ac);
    }
}

/* ---- picture and controls ---------------------------------------------------------------------------------------- */
static void blit(void){
    memset(g_frame, 0, sizeof g_frame);
    if(!have_pic || !pic) return;
    double a = (double)fw / fh; int W, H;
    if(fill){ if(a >= 1){ H = GH; W = (int)(GH * a); } else { W = GW; H = (int)(GW / a); } }   /* covers the circle */
    else { H = (int)(360.0 / sqrt(1 + a * a)); W = (int)(H * a); }      /* the biggest that fits inside it */
    int X = (GW - W) / 2, Y = (GH - H) / 2;
    static int16_t tx[GW], tw[GW]; static int k_fw, k_W, k_X;            /* each column's source, until the shape changes */
    if(k_fw != fw || k_W != W || k_X != X){
        for(int x = 0; x < GW; x++){
            int fx = (int)((int64_t)(x - X) * (fw - 1) * 256 / (W > 1 ? W - 1 : 1)), ix = fx >> 8, wx = fx & 255;
            if(ix < 0){ ix = 0; wx = 0; }
            if(ix >= fw - 1){ ix = fw > 1 ? fw - 2 : 0; wx = fw > 1 ? 256 : 0; }
            tx[x] = (int16_t)ix; tw[x] = (int16_t)wx;
        }
        k_fw = fw; k_W = W; k_X = X;
    }
    for(int y = 0; y < GH; y++){
        int sy = (y - Y); if(sy < 0 || sy >= H) continue;
        int fy = (int)((int64_t)sy * (fh - 1) * 256 / (H > 1 ? H - 1 : 1)), iy = fy >> 8, wy = fy & 255; if(iy >= fh - 1){ iy = fh - 2 < 0 ? 0 : fh - 2; wy = fh > 1 ? 256 : 0; }
        const uint32_t *r0 = pic + (size_t)iy * fw, *r1 = fh > 1 ? r0 + fw : r0;
        uint32_t *o = g_frame + y * GW;
        int xa = X < 0 ? 0 : X, xb = X + W > GW ? GW : X + W;
        for(int x = xa; x < xb; x++){
            int ix = tx[x], wx = tw[x];
            uint32_t p = r0[ix], q = r0[ix + (fw > 1)], u = r1[ix], v = r1[ix + (fw > 1)];
            uint32_t rb = ((((p & 0xFF00FF) * (256 - wx) + (q & 0xFF00FF) * wx) >> 8 & 0xFF00FF) * (256 - wy) +
                           (((u & 0xFF00FF) * (256 - wx) + (v & 0xFF00FF) * wx) >> 8 & 0xFF00FF) * wy) >> 8 & 0xFF00FF;
            uint32_t g = ((((p & 0xFF00) * (256 - wx) + (q & 0xFF00) * wx) >> 8 & 0xFF00) * (256 - wy) +
                          (((u & 0xFF00) * (256 - wx) + (v & 0xFF00) * wx) >> 8 & 0xFF00) * wy) >> 8 & 0xFF00;
            o[x] = rb | g;
        }
    }
}
/* the theme's colours (mq_ui writes them to /tmp/diskos-app-env before a launch); NTDisk's look if there are none */
static uint32_t C_TEXT = 0xF2F2F2, C_DIM = 0x8C8C8C, C_ACC = 0xD71921, C_ON = 0xFFFFFF;
static void load_colours(void){
    FILE *f = fopen("/tmp/diskos-app-env", "r"); if(!f) return;
    char line[96]; unsigned v; uint32_t canvas = 0, surf = 0; int have_c = 0;
    while(fgets(line, sizeof line, f)){
        if(sscanf(line, "text=%6x", &v) == 1) C_TEXT = v;
        else if(sscanf(line, "text_muted=%6x", &v) == 1) C_DIM = v;
        else if(sscanf(line, "accent=%6x", &v) == 1) C_ACC = v;
        else if(sscanf(line, "on_accent=%6x", &v) == 1) C_ON = v;
        else if(sscanf(line, "canvas=%6x", &v) == 1){ canvas = v; have_c = 1; }
        else if(sscanf(line, "surface=%6x", &v) == 1) surf = v;
    }
    fclose(f); (void)surf;
    /* the video is always on black: a light theme's dark text would vanish, so take the light side */
    if(have_c && ((canvas >> 16 & 255) + (canvas >> 8 & 255) + (canvas & 255)) > 3 * 128){ C_TEXT = 0xF2F2F2; C_DIM = 0x9A9A9A; }
}
static void shade(int cx, int cy, int rx, int ry, int amax){          /* darkens an ellipse, fading out to its edge */
    for(int y = cy - ry; y <= cy + ry; y++) for(int x = cx - rx; x <= cx + rx; x++){
        float dx = (float)(x - cx) / rx, dy = (float)(y - cy) / ry, d = dx * dx + dy * dy;
        if(d < 1) gp_put(x, y, 0x000000, (int)(amax * (1 - d) * (1 - d)));
    }
}
static void tri(int cx, int cy, int s, uint32_t c){                    /* a play triangle pointing right */
    for(int y = -s; y <= s; y++){ int w = (s - abs(y)) * 2 * 87 / 100; for(int x = 0; x <= w; x++) gp_put(cx - s * 2 / 3 + x, cy + y, c, 255); }
}
static void fmt(double s, char *o, size_t n){ int t = s > 0 ? (int)s : 0; snprintf(o, n, "%d:%02d", t / 60, t % 60); }
static void line(float x0, float y0, float x1, float y1, uint32_t c){  /* a thin anti-aliased line, by small discs */
    int n = (int)(fabsf(x1 - x0) + fabsf(y1 - y0)) + 1;
    for(int i = 0; i <= n; i++){ float t = (float)i / n; gp_disc(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, 0.9f, c, 255); }
}
/* Quiet controls in the theme's colours: a fine ring with play / pause, -10 and +10 as plain text either side, a thin
 * close mark at the top, the time below, FIT / FILL under it, and the rim's dots as the progress (the head in the
 * accent). A soft shade only where the controls are, not over the whole picture. */
static void draw_controls(void){
    double m = media_now(); if(m < 0) m = 0;
    if(dur > 0 && m > dur) m = dur;
    int lit = dur > 0 ? (int)(72 * m / dur) : 0;
    for(int i = 0; i < 72; i++){
        double an = (i * 5.0 - 90) * M_PI / 180;
        gp_disc((float)(180 + 170 * cos(an)), (float)(180 + 170 * sin(an)), i == lit ? 3.0f : 1.8f, i == lit ? C_ACC : i < lit ? C_TEXT : C_DIM, i < lit || i == lit ? 255 : 150);
    }
    shade(180, 190, 120, 70, 150);                                     /* a soft shade behind the controls */
    int still = paused || ended;
    gp_disc(180, 180, 30, still ? C_ACC : 0x000000, still ? 255 : 140);
    if(!still) gp_ring(180, 180, 30, 1.5f, C_TEXT, 230);
    if(still) tri(184, 180, 12, C_ON);
    else { gp_rect(170, 168, 6, 24, C_TEXT, 2, 255); gp_rect(184, 168, 6, 24, C_TEXT, 2, 255); }
    gp_text_at(98 - gp_text_w("-10", 2) / 2, 173, "-10", 2, C_TEXT);
    gp_text_at(262 - gp_text_w("+10", 2) / 2, 173, "+10", 2, C_TEXT);
    line(172, 54, 188, 70, C_TEXT); line(188, 54, 172, 70, C_TEXT);     /* close */
    char a[16], b[16]; fmt(m, a, sizeof a); fmt(dur, b, sizeof b);
    int wa = gp_text_w(a, 2), wb = gp_text_w(b, 1), x = 180 - (wa + 8 + wb) / 2;
    gp_text_at(x, 248, a, 2, C_TEXT); gp_text_at(x + wa + 8, 255, b, 1, C_DIM);
    gp_text(274, fill ? "FIT" : "FILL", 1, C_DIM);
    if(!ap_ok) gp_text(300, "NO SOUND", 1, C_DIM);
}
static void render(void){
    blit();
    if(ended || paused || gp_now() < ctrl_until) draw_controls();
    present();
}
static void tap(int x, int y){
    int dx = x - 180, dy = y - 180, r2 = dx * dx + dy * dy;
    int showing = ended || paused || gp_now() < ctrl_until;
    dirty = 1;
    if(r2 > 150 * 150 && dur > 0 && showing){                          /* the rim: jump there */
        double an = atan2(dy, dx) * 180 / M_PI + 90; if(an < 0) an += 360;
        double t = dur * an / 360; if(paused){ pause_pos = t; } else restart_at(t);
        ctrl_until = gp_now() + CTRL_S; return;
    }
    if(!showing){ ctrl_until = gp_now() + CTRL_S; return; }            /* the first tap just shows them */
    ctrl_until = gp_now() + CTRL_S;
    if(abs(x - 180) < 30 && abs(y - 62) < 30){ quit = 1; return; }
    if(r2 < 44 * 44){ if(ended){ paused = 0; restart_at(0); } else set_paused(!paused); return; }
    if(abs(x - 98) < 38 && abs(y - 180) < 38){ double t = media_now() - 10; if(paused) pause_pos = t < 0 ? 0 : t; else restart_at(t); return; }
    if(abs(x - 262) < 38 && abs(y - 180) < 38){ double t = media_now() + 10; if(paused) pause_pos = dur > 0 && t > dur ? dur : t; else restart_at(t); return; }
    if(abs(x - 180) < 44 && y > 264 && y < 296){ fill = !fill; return; }
    ctrl_until = 0;                                                    /* anywhere else: hide them */
}
static void message(const char *l1, const char *l2, const char *l3){
    memset(g_frame, 0, sizeof g_frame);
    gp_text(110, "VIDEOS", 3, C_ACC); gp_text(160, l1, 2, C_TEXT);
    if(l2) gp_text(190, l2, 1, C_DIM);
    if(l3) gp_text(206, l3, 1, C_DIM);
    gp_text(262, "TAP TO LEAVE", 1, C_DIM);
    present();
}

int video_main(int argc, char **argv){
    /* argv: [--selftest out.ppm] file */
    int selftest = argc >= 4 && !strcmp(argv[1], "--selftest");
    const char *out = selftest ? argv[2] : NULL, *path = argc >= 2 ? argv[argc - 1] : NULL;
    FILE *f = fopen("/usr/data/diskos.conf", "r");
    if(f){ char line[256]; int v; while(fgets(line, sizeof line, f)) if(sscanf(line, "screen_rot=%d", &v) == 1 && v >= 0 && v < 4) rot = v; fclose(f); }
    load_colours();
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGHUP, on_sig); signal(SIGPIPE, SIG_IGN);
    av_log_set_level(AV_LOG_ERROR);
    if(!selftest){ if(open_fb() < 0){ fprintf(stderr, "video: no display\n"); return 1; } open_touch(); }
    if(!selftest) message("OPENING", NULL, NULL);
    int r = path ? open_file(path) : -1;
    if(r < 0){
        fprintf(stderr, "video: can't play %s (%d)\n", path ? path : "(none)", r);
        if(selftest){ printf("FAIL video: can't open %s\n", path ? path : ""); return 1; }
        char sz[48]; snprintf(sz, sizeof sz, "%dx%d: MORE THAN THE DISC CAN DECODE", big_w, big_h);
        if(r == -3) message("THIS VIDEO IS TOO BIG", sz, "MAKE A SMALL COPY: SEE VIDEOS/README");
        else if(r == -2) message("CAN'T PLAY THIS VIDEO", "ITS PICTURE FORMAT ISN'T SUPPORTED", "H.264 / H.265 / MPEG-4 / VP9 PLAY");
        else message("CAN'T OPEN THIS FILE", "IT MAY BE DAMAGED OR NOT A VIDEO", NULL);
        double t = gp_now(); int was = 0;
        while(!quit && gp_now() - t < 60){ usleep(30000); int w = t_down; read_touch(); if(was && !t_down) break; was = w || t_down; }
        return 0;
    }
    if(ac && !selftest){                                                /* the receiver comes up just after the mode switch */
        message("STARTING SOUND", NULL, NULL);
        ap.port = 5000; ap.rtsp = ap.aud = ap.ctl = ap.tim = -1; ap.vol_db = 0;
        for(int i = 0; i < 16 && !quit; i++){ if(ap_open(&ap) == 0){ ap_ok = 1; break; } usleep(500000); }
        if(!ap_ok) fprintf(stderr, "video: no AirPlay receiver - playing without sound\n");
    }
    if(ap_ok) audio_setup();
    lat = ap_ok ? LAT_S : 0.15;
    printf("video: %s, %s %dx%d%s, %.1f s, sound %s%s\n", path, vc->codec->name, vc->width, vc->height, rot90 ? " (turned)" : "", dur,
           ac ? ac->codec->name : "none", ap_ok ? " via AirPlay" : " off"); fflush(stdout);
    restart_at(0); ctrl_until = gp_now() + 2.5;
    if(selftest){                                                       /* decode 3 s of film as fast as it goes, seek, render */
        double film = dur > 3.5 ? 3.0 : dur * 0.8, w0 = gp_now();
        t0 = 0; paused = 0; int shown = 0;
        for(double m = 0; m < film; m += 1.0 / 30){ int h = have_pic; have_pic = 0; decode_video(m); if(have_pic) shown++; else have_pic = h; }
        double secs = gp_now() - w0;
        double want = dur > 8 ? 6 : dur / 2; restart_at(want); t0 = 0;
        decode_video(want + 0.05); paused = 1; pause_pos = want;
        int jumped = fabs(shown_t - want) < 0.25;
        render(); write_ppm(out);
        int ok = shown >= (int)(film * 20) && have_pic && jumped;
        printf("%s video: %s %dx%d -> %dx%d, %.1f s, %d pictures for %.1f s of film in %.2f s (%.0f fps), seek to %.0f s %s, drop level %d\n",
               ok ? "PASS" : "FAIL", vc->codec->name, vc->width, vc->height, fw, fh, dur, shown, film, secs, shown / (secs > 0 ? secs : 1), want, jumped ? "ok" : "missed", drop_level);
        return ok ? 0 : 1;
    }
    double last_draw = 0;
    while(!quit){
        read_touch();
        double m = media_now();
        if(!paused && !ended){ if(m >= 0) decode_video(m); feed_audio(m); }
        if(ended && !paused){ paused = 1; pause_pos = dur; dirty = 1; }
        double now = gp_now();
        int ctl = ended || paused || now < ctrl_until;
        if(dirty || (ctl && now - last_draw > 0.25)){ render(); dirty = 0; last_draw = now; }
        struct pollfd p = { .fd = tfd, .events = POLLIN };
        poll(&p, tfd >= 0 ? 1 : 0, paused ? 40 : 4);
    }
    if(ap_ok) ap_close(&ap);
    pq_clear(&vq); pq_clear(&aq);
    avcodec_free_context(&vc); avcodec_free_context(&ac); avformat_close_input(&fx);
    sws_freeContext(sws); swr_free(&swr); av_frame_free(&vfn); av_frame_free(&vfc); av_frame_free(&af);
    free(pic); free(tmp);
    return 0;
}
