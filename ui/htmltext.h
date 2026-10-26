/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* htmltext - turns a web page (or a plain text file) into readable blocks for the Browser: headings, paragraphs,
 * list items and links, with scripts, styles, menus and footers left out. No LVGL; usable from any thread. */
#ifndef HTMLTEXT_H
#define HTMLTEXT_H
#include <stddef.h>

#define HB_PARA    0
#define HB_HEADING 1
#define HB_LINK    2     /* a block that is one link (a menu item, a search result title): the whole row opens it */
#define HB_ITEM    3     /* a list item */
#define HB_PRE     4     /* preformatted: spacing kept */
#define HB_QUOTE   5
#define HB_MAXLINK 8

typedef struct { char *text, *href; } hlink_t;
typedef struct { int kind; char *text; int nlink; hlink_t link[HB_MAXLINK]; } hblock_t;
typedef struct { char title[160]; char base[1024]; hblock_t *b; int n, cap; int truncated; } hdoc_t;

/* Parse len bytes of HTML fetched from url (used to resolve the links). 0 ok, -1 out of memory. */
int  html_parse(const char *buf, size_t len, const char *url, hdoc_t *out);
/* Plain text: paragraphs at blank lines. */
int  text_parse(const char *buf, size_t len, const char *name, hdoc_t *out);
void hdoc_free(hdoc_t *d);

/* ref resolved against base into out (absolute http(s) URL or local path); a DuckDuckGo redirect link is unwrapped
 * to its target. 0 ok, -1 unusable (javascript:, mailto:, too long). */
int  url_resolve(const char *base, const char *ref, char *out, size_t cap);
/* What the user typed: an address (gets https:// if it has none), a local path, or words to search for. */
void url_from_input(const char *in, char *out, size_t cap);
/* "en.m.wikipedia.org" from a URL, for the address pill */
void url_host(const char *url, char *out, size_t cap);
#endif
