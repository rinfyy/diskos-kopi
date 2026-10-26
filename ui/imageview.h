/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef IMAGEVIEW_H
#define IMAGEVIEW_H
#include "lvgl/lvgl.h"
/* The picture viewer (SCR_IMAGE, imageview.c). */
void imageview_create(lv_obj_t *root);
/* Show names[index] from dir; the other names are the pictures to browse with swipes/taps. 0 ok, -1 failed. */
int  imageview_open(const char *dir, char *const *names, int n, int index);
int  imageview_test_wait(int ms);     /* host renders */
void imageview_test_fill(int on);     /* host renders */
#endif
