/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* htmltext.c - see htmltext.h. A forgiving single-pass tag scanner, not a full HTML parser: it keeps the readable
 * text of a page and its links, which is what a 360 px round screen can show. Bounded: at most HT_MAXBLOCKS blocks
 * of at most HT_MAXTEXT bytes each, so a huge page costs a bounded amount of memory. */
#include "htmltext.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define HT_MAXBLOCKS 900
#define HT_MAXTEXT   3000

/* ---- small helpers ----------------------------------------------------------------------------------------------- */
static int ieq(const char *a, size_t n, const char *b){ return strlen(b) == n && !strncasecmp(a, b, n); }
static void put_utf8(char *o, size_t *n, size_t cap, unsigned cp){
    if(cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = '?';
    if(cp < 0x80){ if(*n + 1 < cap) o[(*n)++] = (char)cp; }
    else if(cp < 0x800){ if(*n + 2 < cap){ o[(*n)++] = (char)(0xC0 | cp >> 6); o[(*n)++] = (char)(0x80 | (cp & 63)); } }
    else if(cp < 0x10000){ if(*n + 3 < cap){ o[(*n)++] = (char)(0xE0 | cp >> 12); o[(*n)++] = (char)(0x80 | (cp >> 6 & 63)); o[(*n)++] = (char)(0x80 | (cp & 63)); } }
    else if(*n + 4 < cap){ o[(*n)++] = (char)(0xF0 | cp >> 18); o[(*n)++] = (char)(0x80 | (cp >> 12 & 63)); o[(*n)++] = (char)(0x80 | (cp >> 6 & 63)); o[(*n)++] = (char)(0x80 | (cp & 63)); }
}
static const struct { const char *n; unsigned cp; } ENT[] = {
    {"amp",'&'},{"lt",'<'},{"gt",'>'},{"quot",'"'},{"apos",'\''},{"nbsp",' '},{"copy",0xA9},{"reg",0xAE},{"deg",0xB0},
    {"mdash",0x2014},{"ndash",0x2013},{"hellip",0x2026},{"lsquo",0x2018},{"rsquo",0x2019},{"ldquo",0x201C},{"rdquo",0x201D},
    {"bull",0x2022},{"middot",0xB7},{"laquo",0xAB},{"raquo",0xBB},{"times",0xD7},{"euro",0x20AC},{"pound",0xA3},{"yen",0xA5},
    {"rarr",0x2192},{"larr",0x2190},{"thinsp",' '},{"ensp",' '},{"emsp",' '},{"zwj",0},{"zwnj",0},{"shy",0},{"eacute",0xE9},
    {"egrave",0xE8},{"aacute",0xE1},{"agrave",0xE0},{"oacute",0xF3},{"uacute",0xFA},{"iacute",0xED},{"ntilde",0xF1},
    {"uuml",0xFC},{"ouml",0xF6},{"auml",0xE4},{"ccedil",0xE7},{"szlig",0xDF},{"frac12",0xBD},{"plusmn",0xB1},{"micro",0xB5}
};
/* decode one entity at s ("&amp;" ...) into o; returns bytes consumed (0 if not an entity) */
static size_t entity(const char *s, const char *end, char *o, size_t *n, size_t cap){
    const char *p = s + 1, *semi = NULL;
    for(const char *q = p; q < end && q - p < 12; q++) if(*q == ';'){ semi = q; break; } else if(!isalnum((unsigned char)*q) && *q != '#') break;
    if(!semi) return 0;
    size_t len = (size_t)(semi - p);
    if(len >= 2 && p[0] == '#'){
        unsigned long cp = (p[1] == 'x' || p[1] == 'X') ? strtoul(p + 2, NULL, 16) : strtoul(p + 1, NULL, 10);
        if(cp >= 0x80 && cp < 0xA0) cp = '?';                       /* cp1252 control range: rare, never valid text */
        if(cp) put_utf8(o, n, cap, (unsigned)cp);
        return len + 2;
    }
    for(size_t i = 0; i < sizeof ENT / sizeof ENT[0]; i++) if(ieq(p, len, ENT[i].n)){ if(ENT[i].cp) put_utf8(o, n, cap, ENT[i].cp); return len + 2; }
    return 0;
}
/* the value of attribute `name` in a tag's attribute text, entity-decoded; "" if absent */
static void attr(const char *a, const char *end, const char *name, char *out, size_t cap){
    out[0] = 0; size_t nl = strlen(name);
    for(const char *p = a; p < end; ){
        while(p < end && (isspace((unsigned char)*p) || *p == '/')) p++;
        const char *k = p; while(p < end && *p != '=' && !isspace((unsigned char)*p) && *p != '>') p++;
        size_t kl = (size_t)(p - k);
        while(p < end && isspace((unsigned char)*p)) p++;
        const char *v = NULL, *ve = NULL;
        if(p < end && *p == '='){
            p++; while(p < end && isspace((unsigned char)*p)) p++;
            if(p < end && (*p == '"' || *p == '\'')){ char q = *p++; v = p; while(p < end && *p != q) p++; ve = p; if(p < end) p++; }
            else { v = p; while(p < end && !isspace((unsigned char)*p) && *p != '>') p++; ve = p; }
        }
        if(kl == nl && !strncasecmp(k, name, nl) && v){
            size_t n = 0;
            for(const char *q = v; q < ve && n + 1 < cap; ){
                if(*q == '&'){ size_t used = entity(q, ve, out, &n, cap); if(used){ q += used; continue; } }
                out[n++] = *q++;
            }
            out[n] = 0; return;
        }
        if(kl == 0 && p < end) p++;
    }
}

/* ---- the document being built ------------------------------------------------------------------------------------ */
typedef struct {
    hdoc_t *d;
    char text[HT_MAXTEXT + 8]; size_t tn; int kind, pre;
    hlink_t link[HB_MAXLINK]; int nlink; size_t link_chars;
    int in_link; char href[1024]; char ltext[256]; size_t lt;
    int oom;
} pb_t;

static void flush(pb_t *b){
    while(b->tn && (b->text[b->tn - 1] == ' ' || b->text[b->tn - 1] == '\n')) b->tn--;   /* trim the end */
    b->text[b->tn] = 0;
    char *t = b->text; while(*t == ' ' || (*t == '\n' && !b->pre)) t++;
    if(*t && !b->oom){
        hdoc_t *d = b->d;
        if(d->n >= HT_MAXBLOCKS){ d->truncated = 1; goto reset; }
        if(d->n == d->cap){
            int nc = d->cap ? d->cap * 2 : 64; hblock_t *nb = realloc(d->b, sizeof *nb * (size_t)nc);
            if(!nb){ b->oom = 1; goto reset; }
            d->b = nb; d->cap = nc;
        }
        hblock_t *hb = &d->b[d->n];
        memset(hb, 0, sizeof *hb);
        hb->text = strdup(t);
        if(!hb->text){ b->oom = 1; goto reset; }
        hb->kind = b->kind;
        size_t tl = strlen(t);
        if(b->nlink == 1 && b->kind != HB_HEADING && b->link_chars * 10 >= tl * 6) hb->kind = HB_LINK;   /* mostly one link */
        hb->nlink = b->nlink; memcpy(hb->link, b->link, sizeof b->link);
        b->nlink = 0;
        d->n++;
    }
reset:
    for(int i = 0; i < b->nlink; i++){ free(b->link[i].text); free(b->link[i].href); }
    b->nlink = 0; b->tn = 0; b->link_chars = 0; b->kind = HB_PARA;
}
static void add_char(pb_t *b, char c){
    if(!b->pre){
        if(c == '\n' || c == '\r' || c == '\t' || c == '\f') c = ' ';
        if(c == ' ' && (b->tn == 0 || b->text[b->tn - 1] == ' ' || b->text[b->tn - 1] == '\n')) return;
    }
    if(b->tn < HT_MAXTEXT) b->text[b->tn++] = c;
    if(b->in_link && b->lt + 1 < sizeof b->ltext && (c != ' ' || b->lt)) b->ltext[b->lt++] = c;
}
static void add_str(pb_t *b, const char *s){ while(*s) add_char(b, *s++); }
static void link_end(pb_t *b){
    if(!b->in_link) return;
    b->in_link = 0;
    while(b->lt && b->ltext[b->lt - 1] == ' ') b->lt--;
    b->ltext[b->lt] = 0;
    char abs[1024];
    if(b->href[0] && b->lt && b->nlink < HB_MAXLINK && url_resolve(b->d->base, b->href, abs, sizeof abs) == 0){
        b->link[b->nlink].text = strdup(b->ltext); b->link[b->nlink].href = strdup(abs);
        if(b->link[b->nlink].text && b->link[b->nlink].href){ b->nlink++; b->link_chars += b->lt; }
        else { free(b->link[b->nlink].text); free(b->link[b->nlink].href); }
    }
    b->lt = 0;
}

/* elements whose whole content is left out */
static int skipped(const char *t, size_t n){
    static const char *S[] = { "script", "style", "noscript", "svg", "template", "iframe", "object", "select", "textarea",
                               "button", "nav", "footer", "aside", "form", "canvas", "video", "audio", "math", "map" };
    for(size_t i = 0; i < sizeof S / sizeof S[0]; i++) if(ieq(t, n, S[i])) return 1;
    return 0;
}
static int blocky(const char *t, size_t n){
    static const char *B[] = { "p", "div", "ul", "ol", "li", "table", "tr", "section", "article", "main", "header", "blockquote",
                               "pre", "dl", "dt", "dd", "figure", "figcaption", "hr", "center", "address", "details", "summary",
                               "h1", "h2", "h3", "h4", "h5", "h6", "body", "caption", "tbody", "thead", "fieldset", "legend" };
    for(size_t i = 0; i < sizeof B / sizeof B[0]; i++) if(ieq(t, n, B[i])) return 1;
    return 0;
}

/* a page in another single-byte charset becomes UTF-8 (Latin-1, close enough for windows-1252 text) */
static char *to_utf8(const char *buf, size_t len, size_t *olen){
    char head[2048]; size_t hn = len < sizeof head - 1 ? len : sizeof head - 1;
    memcpy(head, buf, hn); head[hn] = 0;
    for(size_t i = 0; i < hn; i++) head[i] = (char)tolower((unsigned char)head[i]);
    const char *cs = strstr(head, "charset=");
    int latin = cs && (strncmp(cs + 8, "\"utf", 4) && strncmp(cs + 8, "utf", 3) && strncmp(cs + 8, "'utf", 4))
                   && (strstr(cs, "iso-8859-1") == cs + 8 || strstr(cs, "windows-1252") == cs + 8 || strstr(cs, "\"iso-8859-1") == cs + 8
                       || strstr(cs, "\"windows-1252") == cs + 8 || strstr(cs, "latin1") == cs + 8);
    char *o = malloc(len * (latin ? 2 : 1) + 1); if(!o) return NULL;
    size_t n = 0;
    for(size_t i = 0; i < len; ){
        unsigned char c = (unsigned char)buf[i];
        if(latin){ if(c < 0x80) o[n++] = (char)c; else { o[n++] = (char)(0xC0 | c >> 6); o[n++] = (char)(0x80 | (c & 63)); } i++; continue; }
        /* UTF-8 (or unknown): keep valid sequences, replace stray bytes so the screen never meets broken text */
        int k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        int ok = k > 0 && i + (size_t)k <= len;
        for(int j = 1; ok && j < k; j++) if(((unsigned char)buf[i + j] >> 6) != 2) ok = 0;
        if(ok){ memcpy(o + n, buf + i, (size_t)k); n += (size_t)k; i += (size_t)k; }
        else { o[n++] = '?'; i++; }
    }
    o[n] = 0; *olen = n;
    return o;
}

int html_parse(const char *raw, size_t rawlen, const char *url, hdoc_t *d){
    memset(d, 0, sizeof *d);
    snprintf(d->base, sizeof d->base, "%s", url ? url : "");
    size_t len; char *buf = to_utf8(raw, rawlen, &len); if(!buf) return -1;
    pb_t *b = calloc(1, sizeof *b); if(!b){ free(buf); return -1; }
    b->d = d;
    const char *p = buf, *end = buf + len;
    char skip_tag[16] = ""; int skip_depth = 0, in_head = 0, in_title = 0, list_depth = 0;
    size_t title_n = 0;
    while(p < end){
        if(*p == '<'){
            if(end - p >= 4 && !strncmp(p, "<!--", 4)){ const char *e = strstr(p + 4, "-->"); p = e ? e + 3 : end; continue; }
            if(p + 1 < end && (p[1] == '!' || p[1] == '?')){ const char *e = memchr(p, '>', (size_t)(end - p)); p = e ? e + 1 : end; continue; }
            int close = p + 1 < end && p[1] == '/';
            const char *t = p + 1 + close, *te = t;
            while(te < end && (isalnum((unsigned char)*te) || *te == '-')) te++;
            if(te == t){ if(!skip_depth && !in_head) add_char(b, '<'); p++; continue; }   /* a lone "<" in text */
            const char *a = te, *ae = a; char q = 0;                                      /* attributes, quote-aware */
            while(ae < end && (q || *ae != '>')){ if(q){ if(*ae == q) q = 0; } else if(*ae == '"' || *ae == '\'') q = *ae; ae++; }
            p = ae < end ? ae + 1 : end;
            size_t tn = (size_t)(te - t);
            if(skip_depth){                                                               /* inside a left-out element */
                if(ieq(t, tn, skip_tag)) skip_depth += close ? -1 : 1;
                continue;
            }
            if(ieq(t, tn, "head")){ in_head = !close; continue; }
            if(ieq(t, tn, "title")){ in_title = !close; continue; }
            if(ieq(t, tn, "base") && !close){ char h[1024]; attr(a, ae, "href", h, sizeof h); if(h[0]){ char r[1024]; if(url_resolve(d->base, h, r, sizeof r) == 0) snprintf(d->base, sizeof d->base, "%s", r); } continue; }
            if(in_head) continue;
            if(!close && skipped(t, tn) && !(ae > a && ae[-1] == '/')){ snprintf(skip_tag, sizeof skip_tag, "%.*s", (int)(tn < 15 ? tn : 15), t); skip_depth = 1; continue; }
            if(ieq(t, tn, "a")){
                if(close) link_end(b);
                else { link_end(b); attr(a, ae, "href", b->href, sizeof b->href); b->in_link = 1; b->lt = 0; }
                continue;
            }
            if(ieq(t, tn, "br")){ if(b->tn && b->text[b->tn - 1] != '\n'){ if(b->tn && b->text[b->tn - 1] == ' ') b->tn--; if(b->tn < HT_MAXTEXT) b->text[b->tn++] = '\n'; } continue; }
            if(ieq(t, tn, "img") && !close){
                char alt[160]; attr(a, ae, "alt", alt, sizeof alt);
                if(alt[0] && b->in_link && b->lt == 0) add_str(b, alt);          /* a picture link: its description */
                continue;
            }
            if(ieq(t, tn, "td") || ieq(t, tn, "th")){ if(!close && b->tn) add_str(b, "  "); continue; }
            if(blocky(t, tn)){
                int was_link = b->in_link; if(was_link) link_end(b);
                flush(b);
                if(ieq(t, tn, "ul") || ieq(t, tn, "ol")){ list_depth += close ? -1 : 1; if(list_depth < 0) list_depth = 0; }
                if(!close){
                    if(tn == 2 && (t[0] == 'h' || t[0] == 'H') && t[1] >= '1' && t[1] <= '6') b->kind = HB_HEADING;
                    else if(ieq(t, tn, "li")){ b->kind = HB_ITEM; }
                    else if(ieq(t, tn, "blockquote")) b->kind = HB_QUOTE;
                    else if(ieq(t, tn, "pre")){ b->kind = HB_PRE; b->pre = 1; }
                } else if(ieq(t, tn, "pre")) b->pre = 0;
                continue;
            }
            continue;                                                                     /* inline tags: just text */
        }
        if(skip_depth){ p++; continue; }
        if(in_head){
            if(in_title){
                if(*p == '&'){ size_t used = entity(p, end, d->title, &title_n, sizeof d->title); if(used){ p += used; continue; } }
                char c = *p++; if(c == '\n' || c == '\r' || c == '\t') c = ' ';
                if(title_n + 1 < sizeof d->title && !(c == ' ' && (title_n == 0 || d->title[title_n - 1] == ' '))) d->title[title_n++] = c;
                d->title[title_n] = 0;
            } else p++;
            continue;
        }
        if(in_title){ p++; continue; }
        if(*p == '&'){
            char tmp[8]; size_t n = 0; size_t used = entity(p, end, tmp, &n, sizeof tmp);
            if(used){ for(size_t i = 0; i < n; i++) add_char(b, tmp[i]); p += used; continue; }
        }
        add_char(b, *p++);
    }
    link_end(b); flush(b);
    while(title_n && d->title[title_n - 1] == ' ') d->title[--title_n] = 0;
    int oom = b->oom;
    free(b); free(buf);
    return oom && d->n == 0 ? -1 : 0;
}

int text_parse(const char *raw, size_t rawlen, const char *name, hdoc_t *d){
    memset(d, 0, sizeof *d);
    const char *slash = name ? strrchr(name, '/') : NULL;
    snprintf(d->title, sizeof d->title, "%s", slash ? slash + 1 : name ? name : "");
    snprintf(d->base, sizeof d->base, "%s", name ? name : "");
    size_t len; char *buf = to_utf8(raw, rawlen, &len); if(!buf) return -1;
    pb_t *b = calloc(1, sizeof *b); if(!b){ free(buf); return -1; }
    b->d = d; b->pre = 1;
    int nl = 0;
    for(size_t i = 0; i < len; i++){
        char c = buf[i];
        if(c == '\r') continue;
        if(c == '\n'){ if(++nl >= 2){ flush(b); continue; } }
        else nl = 0;
        if(c == '\n' && b->tn == 0) continue;
        if(b->tn >= HT_MAXTEXT - 2){ flush(b); }
        if(b->tn < HT_MAXTEXT) b->text[b->tn++] = c;
    }
    flush(b);
    free(b); free(buf);
    return 0;
}

void hdoc_free(hdoc_t *d){
    for(int i = 0; i < d->n; i++){
        free(d->b[i].text);
        for(int k = 0; k < d->b[i].nlink; k++){ free(d->b[i].link[k].text); free(d->b[i].link[k].href); }
    }
    free(d->b); memset(d, 0, sizeof *d);
}

/* ---- addresses --------------------------------------------------------------------------------------------------- */
static int hexv(char c){ return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void pct_decode(const char *s, size_t n, char *out, size_t cap){
    size_t o = 0;
    for(size_t i = 0; i < n && o + 1 < cap; i++){
        if(s[i] == '%' && i + 2 < n && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0){ out[o++] = (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2])); i += 2; }
        else if(s[i] == '+') out[o++] = ' ';
        else out[o++] = s[i];
    }
    out[o] = 0;
}
static void url_clean(char *u, size_t cap){                            /* spaces and stray quotes break the fetch */
    char o[2048]; size_t n = 0;
    for(const char *r = u; *r && n + 4 < sizeof o; r++){
        if(*r == ' '){ o[n++] = '%'; o[n++] = '2'; o[n++] = '0'; }
        else if((unsigned char)*r >= 0x20 && *r != '"' && *r != '\'' && *r != '\\') o[n++] = *r;
    }
    o[n] = 0;
    snprintf(u, cap, "%s", o);
}
int url_resolve(const char *base, const char *ref, char *out, size_t cap){
    while(*ref == ' ' || *ref == '\t' || *ref == '\n') ref++;
    if(!ref[0] || ref[0] == '#') return -1;
    if(!strncasecmp(ref, "javascript:", 11) || !strncasecmp(ref, "mailto:", 7) || !strncasecmp(ref, "tel:", 4) || !strncasecmp(ref, "data:", 5)) return -1;
    char tmp[2048];
    if(strstr(ref, "://")) snprintf(tmp, sizeof tmp, "%s", ref);
    else if(ref[0] == '/' && ref[1] == '/'){                              /* //host/path: the base's scheme */
        const char *sch = strstr(base, "://");
        snprintf(tmp, sizeof tmp, "%.*s:%s", sch ? (int)(sch - base) : 5, sch ? base : "https", ref);
    } else if(base[0] == '/' || !strncmp(base, "file://", 7)){             /* a page on the SD card */
        const char *b = !strncmp(base, "file://", 7) ? base + 7 : base;
        if(ref[0] == '/') snprintf(tmp, sizeof tmp, "%s", ref);
        else { const char *sl = strrchr(b, '/'); snprintf(tmp, sizeof tmp, "%.*s/%s", sl ? (int)(sl - b) : 0, b, ref); }
    } else {
        const char *sch = strstr(base, "://"); if(!sch) return -1;
        const char *host = sch + 3, *path = strchr(host, '/');
        size_t origin = path ? (size_t)(path - base) : strlen(base);
        if(ref[0] == '/') snprintf(tmp, sizeof tmp, "%.*s%s", (int)origin, base, ref);
        else if(ref[0] == '?'){ const char *q = strchr(base, '?'); snprintf(tmp, sizeof tmp, "%.*s%s", q ? (int)(q - base) : (int)strlen(base), base, ref); }
        else {
            const char *q = strchr(base, '?'); size_t bl = q ? (size_t)(q - base) : strlen(base);
            const char *sl = NULL; for(const char *s = base + origin; s < base + bl; s++) if(*s == '/') sl = s;
            if(sl) snprintf(tmp, sizeof tmp, "%.*s/%s", (int)(sl - base), base, ref);
            else snprintf(tmp, sizeof tmp, "%.*s/%s", (int)origin, base, ref);
        }
    }
    char *hash = strchr(tmp, '#'); if(hash) *hash = 0;                     /* fragments: the page itself */
    /* tidy "/./" and "/x/../" in the path */
    char *path = strstr(tmp, "://"); path = path ? strchr(path + 3, '/') : (tmp[0] == '/' ? tmp : NULL);
    if(path){
        char *qs = strchr(path, '?'); char save = 0; if(qs){ save = *qs; *qs = 0; }
        char seg[2048]; size_t sn = 0; seg[0] = 0;
        for(char *s = path; *s; ){
            char *e = strchr(s + 1, '/'); size_t l = e ? (size_t)(e - s) : strlen(s);
            if(l == 2 && !strncmp(s, "/.", 2)){ if(!e && sn + 1 < sizeof seg){ seg[sn++] = '/'; seg[sn] = 0; } }
            else if(l == 3 && !strncmp(s, "/..", 3)){ while(sn && seg[sn - 1] != '/') sn--; if(sn) sn--; seg[sn] = 0; if(!e && sn + 1 < sizeof seg){ seg[sn++] = '/'; seg[sn] = 0; } }
            else if(sn + l < sizeof seg){ memcpy(seg + sn, s, l); sn += l; seg[sn] = 0; }
            if(!e) break;
            s = e;
        }
        if(!sn){ seg[0] = '/'; seg[1] = 0; }
        if(qs){ *qs = save; size_t room = sizeof tmp - (size_t)(path - tmp); char rest[1024]; snprintf(rest, sizeof rest, "%s", qs); snprintf(path, room, "%s%s", seg, rest); }
        else snprintf(path, sizeof tmp - (size_t)(path - tmp), "%s", seg);
    }
    /* DuckDuckGo wraps every result: /l/?uddg=<the real address>&rut=... */
    const char *u = strstr(tmp, "duckduckgo.com/l/?");
    if(u){ const char *v = strstr(u, "uddg="); if(v){ v += 5; const char *amp = strchr(v, '&'); char real[2048];
           pct_decode(v, amp ? (size_t)(amp - v) : strlen(v), real, sizeof real); if(strstr(real, "://")) snprintf(tmp, sizeof tmp, "%s", real); } }
    if(strncasecmp(tmp, "http://", 7) && strncasecmp(tmp, "https://", 8) && tmp[0] != '/') return -1;
    url_clean(tmp, sizeof tmp);
    if(strlen(tmp) + 1 > cap) return -1;
    snprintf(out, cap, "%s", tmp);
    return 0;
}
void url_from_input(const char *in, char *out, size_t cap){
    while(*in == ' ') in++;
    char s[512]; snprintf(s, sizeof s, "%s", in);
    size_t n = strlen(s); while(n && s[n - 1] == ' ') s[--n] = 0;
    if(s[0] == '/' || strstr(s, "://")){ snprintf(out, cap, "%s", s); return; }
    int dot = 0, space = 0; for(char *c = s; *c; c++){ if(*c == '.') dot = 1; if(*c == ' ') space = 1; }
    if(dot && !space && strlen(s) > 3){ snprintf(out, cap, "https://%s", s); return; }
    char q[1536]; size_t qn = 0;                                           /* words: search for them */
    for(unsigned char *c = (unsigned char *)s; *c && qn + 4 < sizeof q; c++){
        if(isalnum(*c) || *c == '-' || *c == '_' || *c == '.') q[qn++] = (char)*c;
        else if(*c == ' ') q[qn++] = '+';
        else qn += (size_t)snprintf(q + qn, sizeof q - qn, "%%%02X", *c);
    }
    q[qn] = 0;
    snprintf(out, cap, "https://lite.duckduckgo.com/lite/?q=%s", q);
}
void url_host(const char *url, char *out, size_t cap){
    const char *h = strstr(url, "://");
    if(!h){ const char *sl = strrchr(url, '/'); snprintf(out, cap, "%s", sl ? sl + 1 : url); return; }
    h += 3; if(!strncasecmp(h, "www.", 4)) h += 4;
    size_t n = strcspn(h, "/?#:");
    snprintf(out, cap, "%.*s", (int)n, h);
}
