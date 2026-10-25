/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef LOCKCOVER_H
#define LOCKCOVER_H
/* The Disco lock screen's custom picture, from <SD>/CustomCover (see lockcover.c). UI thread only. */
void        lockcover_request(void);                 /* decode the chosen picture in the background (returns at once) */
const void *lockcover_img(void);                     /* its 360x360 image descriptor, or NULL until decoded */
int         lockcover_failed(void);                  /* the chosen picture could not be decoded */
const char *lockcover_name(void);                    /* the chosen file name ("" = none yet) */
int         lockcover_next(char *msg, int cap);      /* choose the next picture; msg gets a status line; returns count */
void        lockcover_set_listener(void (*cb)(void));/* called when a decode finishes */
#endif
