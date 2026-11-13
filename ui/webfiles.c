/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* webfiles.c - Wi-Fi file transfer: the SD card in a PC's browser (kopi.27).
 *
 * Turn it on (Shortcuts, Quick Settings' tile, Settings > Connectivity > File transfer) and the Disc shows an address
 * and a 4-digit code: open the address in any browser on the same Wi-Fi, type the code, and the SD card is there as
 * a file manager - folders, sizes, upload (files or whole folders, drag and drop, with progress), download, play in
 * the browser, rename, new folder, delete. No app on the PC.
 *
 * Kept small and safe:
 *   - Off until turned on, and off again by itself after 10 minutes with no requests (Wi-Fi stays as it was), so it
 *     costs no battery when not in use. Listens on the Wi-Fi address only (never USB / loopback), port 80 (8080 if
 *     80 is taken).
 *   - The code is fresh each time; a right code gets a random session cookie. Ten wrong codes in a row stop it.
 *   - Every path is under the SD card: ".." is refused, and the real path of whatever exists (symlinks resolved) has
 *     to be inside /tmp/sdcard. Hidden entries (".diskos" settings and the like) are not listed or touched.
 *   - Uploads go to a hidden ".part" file first, are synced, then renamed, so a cut connection never leaves half a
 *     song where the library would find it; the free space is checked before the first byte.
 *   - Each connection is its own thread (at most 6 at once), so a long download doesn't block the page.
 *   - When it stops, if anything was added, removed or renamed, the library is scanned again.
 *   - Fast (kopi.29): 1 MB socket buffers (a wide TCP window), Wi-Fi power saving off while it's on (the radio
 *     otherwise naps between packets, which caps a transfer at a fraction of the link), the CPU at full speed while
 *     bytes are actually moving (back to normal 5 s after), 1 MB reads and writes to the card with read-ahead, and
 *     the page sends three files at once (small files no longer wait on each other's round trips).
 * The UI (the sheet, the timer) is on the UI thread; the server threads only share a few counters under a mutex. */
#ifndef WF_SERVER_ONLY                                                  /* (tests/webfiles_test.c builds just the server) */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "idisc.h"
#include "simple_theme.h"
#include "scanner.h"
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef WF_ROOT
#define WF_ROOT "/tmp/sdcard"
#endif
#ifndef WF_IFACE
#define WF_IFACE "wlan0"
#endif
#define WF_IDLE_MS (10 * 60 * 1000)
#define WF_MAXCONN 6
#define WF_MAXFAIL 10

/* ---- shared state (server threads <-> UI) ---- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_th; static int g_live;                 /* UI thread only */
static atomic_int g_run, g_conns, g_fails, g_changed;
static int g_fd = -1, g_port;
static char g_ip[24], g_pin[8], g_sess[40];
static volatile uint32_t g_last_req_s;                    /* monotonic seconds of the last request */
static char g_cur[96]; static long long g_cur_done, g_cur_total; static int g_received, g_sent;   /* under g_mu */

/* ---- speed: Wi-Fi power saving off and the CPU governor while transferring (kopi.29) ---- */
struct wf_iw_param { int32_t value; uint8_t fixed, disabled; uint16_t flags; };
struct wf_iwreq { char name[16]; union { struct wf_iw_param power; char pad[16]; } u; };
#define WF_SIOCSIWPOWER 0x8B2C
#define WF_SIOCGIWPOWER 0x8B2D
static int g_ps_saved = -1;                                /* the radio's power saving before (1 on, 0 off), -1 unknown */
static void wifi_powersave(int off){
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); if(s < 0) return;
    struct wf_iwreq r; memset(&r, 0, sizeof r); snprintf(r.name, sizeof r.name, "%s", WF_IFACE);
    if(off){
        if(ioctl(s, WF_SIOCGIWPOWER, &r) == 0) g_ps_saved = r.u.power.disabled ? 0 : 1;
        memset(&r.u, 0, sizeof r.u); r.u.power.disabled = 1;
        (void)ioctl(s, WF_SIOCSIWPOWER, &r);
    } else if(g_ps_saved == 1){
        memset(&r.u, 0, sizeof r.u); r.u.power.disabled = 0;
        (void)ioctl(s, WF_SIOCSIWPOWER, &r);
        g_ps_saved = -1;
    }
    close(s);
}
#define WF_GOV "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
static char g_gov_saved[32]; static int g_gov_fast;
static void cpu_fast(int on){
    if(on == g_gov_fast) return;
    if(on){
        FILE *f = fopen(WF_GOV, "r"); if(!f) return;
        if(!fgets(g_gov_saved, sizeof g_gov_saved, f)){ g_gov_saved[0] = 0; }
        fclose(f);
        g_gov_saved[strcspn(g_gov_saved, "\n")] = 0;
        if(!g_gov_saved[0] || !strcmp(g_gov_saved, "performance")) return;
        f = fopen(WF_GOV, "w"); if(!f) return; fputs("performance", f); if(fclose(f) == 0) g_gov_fast = 1;
    } else {
        FILE *f = fopen(WF_GOV, "w"); if(f){ fputs(g_gov_saved, f); fclose(f); }
        g_gov_fast = 0;
    }
}
static uint32_t now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)t.tv_sec; }
static int rnd_bytes(unsigned char *b, int n){
    int f = open("/dev/urandom", O_RDONLY | O_CLOEXEC); if(f < 0) return -1;
    int got = 0; while(got < n){ int r = (int)read(f, b + got, (size_t)(n - got)); if(r > 0) got += r; else if(r < 0 && errno == EINTR) continue; else break; }
    close(f); return got == n ? 0 : -1;
}
static int wlan_ip(char *out, int cap){
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); if(s < 0) return -1;
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr); snprintf(ifr.ifr_name, IFNAMSIZ, "%s", WF_IFACE);
    int r = ioctl(s, SIOCGIFADDR, &ifr); close(s); if(r < 0) return -1;
    const char *p = inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr); if(!p) return -1;
    snprintf(out, (size_t)cap, "%s", p); return 0;
}

/* ---- the page (one file, no outside resources; works on a PC or a phone) ---- */
static const char PAGE[] =
"<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Disc files</title><style>"
":root{--bg:#0e0e10;--card:#1a1a1e;--line:#2a2a30;--tx:#f2f2f4;--mu:#9a9aa3;--ac:#5aa9ff;--bad:#ff5a5f}"
"@media(prefers-color-scheme:light){:root{--bg:#f4f4f6;--card:#fff;--line:#e2e2e6;--tx:#111;--mu:#666;--ac:#0a6cff;--bad:#d22}}"
"*{box-sizing:border-box}body{margin:0;font:15px/1.4 system-ui,-apple-system,Segoe UI,sans-serif;background:var(--bg);color:var(--tx)}"
"header{position:sticky;top:0;background:var(--bg);padding:14px 16px 10px;border-bottom:1px solid var(--line);z-index:2}"
"h1{font-size:18px;margin:0 0 8px}#crumb a{color:var(--ac);text-decoration:none;cursor:pointer}#crumb span{color:var(--mu);margin:0 4px}"
".bar{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}button,.btn{font:inherit;padding:7px 12px;border-radius:9px;border:1px solid var(--line);"
"background:var(--card);color:var(--tx);cursor:pointer}button.pri{background:var(--ac);border-color:var(--ac);color:#fff}button.bad{color:var(--bad)}"
"main{max-width:960px;margin:0 auto;padding:8px 16px 80px}table{width:100%;border-collapse:collapse}td{padding:9px 6px;border-bottom:1px solid var(--line)}"
"td.n{word-break:break-word}td.n a{color:var(--tx);text-decoration:none;cursor:pointer}td.n a:hover{color:var(--ac)}"
"td.s,td.t{color:var(--mu);white-space:nowrap;font-size:13px}td.a{white-space:nowrap;text-align:right}td.a button{padding:4px 8px;font-size:13px;margin-left:4px}"
"#drop{display:none;position:fixed;inset:0;background:rgba(10,108,255,.18);border:3px dashed var(--ac);z-index:9;align-items:center;justify-content:center;font-size:22px}"
"#prog{position:fixed;left:0;right:0;bottom:0;background:var(--card);border-top:1px solid var(--line);padding:10px 16px;display:none}"
"#pb{height:6px;background:var(--line);border-radius:3px;overflow:hidden;margin-top:6px}#pb i{display:block;height:100%;width:0;background:var(--ac)}"
"#login{max-width:320px;margin:80px auto;text-align:center}#login input{font:28px monospace;letter-spacing:10px;width:180px;text-align:center;padding:8px;"
"border-radius:10px;border:1px solid var(--line);background:var(--card);color:var(--tx)}.mu{color:var(--mu)}#space{color:var(--mu);font-size:13px;margin-top:6px}"
"</style></head><body>"
"<div id=login style='display:none'><h1>Disc files</h1><p class=mu>Type the code shown on the Disc.</p>"
"<form onsubmit='login();return false'><input id=pin inputmode=numeric maxlength=4 autocomplete=off autofocus><br><br>"
"<button class=pri>Open</button></form><p id=lerr style='color:var(--bad)'></p></div>"
"<div id=app style='display:none'><header><h1>Disc files</h1><div id=crumb></div><div class=bar>"
"<button class=pri onclick=\"$('pick').click()\">Upload files</button><button onclick=\"$('pickd').click()\">Upload folder</button>"
"<button onclick=mkdir()>New folder</button><button onclick=load()>Refresh</button></div><div id=space></div></header>"
"<main><table id=list></table><p class=mu style='margin-top:18px'>Drop files or folders anywhere on this page to upload them here. "
"New music shows in the library when you stop file transfer on the Disc.</p></main></div>"
"<input type=file id=pick multiple hidden><input type=file id=pickd webkitdirectory multiple hidden>"
"<div id=drop>Drop to upload here</div><div id=prog><span id=pt></span><div id=pb><i></i></div></div>"
"<script>"
"let cwd='/';const $=id=>document.getElementById(id);"
"const enc=p=>p.split('/').map(encodeURIComponent).join('/');"
"const join=(a,b)=>(a.endsWith('/')?a:a+'/')+b;"
"function sz(n){if(n<1024)return n+' B';const u=['KB','MB','GB','TB'];let i=-1;do{n/=1024;i++}while(n>=1024&&i<3);return n.toFixed(n<10?1:0)+' '+u[i]}"
"function esc(s){return s.replace(/[&<>\"']/g,c=>'&#'+c.charCodeAt(0)+';')}"
"async function api(m,u){const r=await fetch(u,{method:m,credentials:'same-origin'});if(r.status==401){show(0);throw 0}return r}"
"function show(ok){$('login').style.display=ok?'none':'block';$('app').style.display=ok?'block':'none';if(!ok)$('pin').focus()}"
"async function login(){const r=await fetch('/api/login?pin='+encodeURIComponent($('pin').value),{method:'POST'});"
"if(r.ok){$('lerr').textContent='';show(1);load()}else{$('lerr').textContent=r.status==429?'Too many tries. Turn file transfer on again on the Disc.':'Wrong code';$('pin').value=''}}"
"async function load(p){if(p!==undefined)cwd=p;const r=await api('GET','/api/ls?p='+encodeURIComponent(cwd));"
"if(!r.ok){alert(await r.text());if(cwd!='/'){cwd='/';load()}return}const j=await r.json();"
"let c='<a onclick=\"load(\\'/\\')\">SD card</a>',acc='';for(const s of cwd.split('/').filter(x=>x)){acc+='/'+s;"
"c+='<span>/</span><a data-p=\"'+esc(acc)+'\" onclick=\"load(this.dataset.p)\">'+esc(s)+'</a>'}$('crumb').innerHTML=c;"
"$('space').textContent=sz(j.free)+' free of '+sz(j.total);"
"j.items.sort((a,b)=>b.d-a.d||a.n.localeCompare(b.n,undefined,{numeric:true,sensitivity:'base'}));let h='';"
"if(cwd!='/')h+='<tr><td class=n><a data-p=\"'+esc(cwd.replace(/\\/[^/]*$/,'')||'/')+'\" onclick=\"load(this.dataset.p)\">&#8617; Up</a></td><td></td><td></td><td></td></tr>';"
"for(const it of j.items){const p=join(cwd,it.n),d=new Date(it.t*1000).toLocaleDateString();"
"h+='<tr><td class=n>'+(it.d?'&#128193; <a data-p=\"'+esc(p)+'\" onclick=\"load(this.dataset.p)\">'+esc(it.n)+'</a>':"
"'<a href=\"/f'+esc(enc(p))+'\" target=_blank>'+esc(it.n)+'</a>')+'</td><td class=s>'+(it.d?'':sz(it.s))+'</td><td class=t>'+d+'</td>'+"
"'<td class=a>'+(it.d?'':'<a class=btn href=\"/f'+esc(enc(p))+'?dl=1\" style=\"padding:4px 8px;font-size:13px;text-decoration:none\">Download</a>')+"
"'<button data-p=\"'+esc(p)+'\" onclick=\"ren(this.dataset.p)\">Rename</button><button class=bad data-p=\"'+esc(p)+'\" data-d='+it.d+' onclick=\"del(this.dataset.p,+this.dataset.d)\">Delete</button></td></tr>'}"
"$('list').innerHTML=h||'<tr><td class=mu>This folder is empty.</td></tr>'}"
"async function mkdir(){const n=prompt('New folder name');if(!n)return;const r=await api('POST','/api/mkdir?p='+encodeURIComponent(join(cwd,n)));if(!r.ok)alert(await r.text());load()}"
"async function ren(p){const o=p.split('/').pop(),n=prompt('Rename to',o);if(!n||n==o)return;"
"const r=await api('POST','/api/mv?p='+encodeURIComponent(p)+'&to='+encodeURIComponent(join(p.slice(0,p.length-o.length)||'/',n)));if(!r.ok)alert(await r.text());load()}"
"async function del(p,d){if(!confirm('Delete '+p.split('/').pop()+(d?' and everything in it':'')+'?'))return;const r=await api('DELETE','/f'+enc(p));if(!r.ok)alert(await r.text());load()}"
"let up={tot:0,done:0,cur:{},t0:0,n:0,k:0};"
"function prog(){let d=up.done;for(const v of Object.values(up.cur))d+=v;const s=(Date.now()-up.t0)/1000,r=s>0.5?d/s:0;"
"$('pb').firstChild.style.width=(up.tot?d/up.tot*100:0)+'%';"
"$('pt').textContent='Uploading '+Math.min(up.k,up.n)+' of '+up.n+'  '+sz(d)+' / '+sz(up.tot)+(r?'  '+sz(r)+'/s':'')}"
"function put(path,file,id){return new Promise((ok,no)=>{const x=new XMLHttpRequest();x.open('PUT','/f'+enc(path));"
"x.upload.onprogress=e=>{up.cur[id]=e.loaded;prog()};"
"x.onload=()=>{delete up.cur[id];if(x.status<300){up.done+=file.size;prog();ok()}else no(x.responseText||x.status)};"
"x.onerror=()=>{delete up.cur[id];no('connection lost')};x.send(file)})}"
"async function upload(list){list=list.filter(f=>!(f.rel||f.webkitRelativePath||f.name).split('/').some(s=>s.startsWith('.')));if(!list.length)return;"
"$('prog').style.display='block';up={tot:list.reduce((a,f)=>a+f.size,0),done:0,cur:{},t0:Date.now(),n:list.length,k:0};"
"const BIG=16<<20,bad=[];let next=0,active=0,big=0,stop=0;"
/* three small files at once (their round trips overlap); a big one goes alone, so the card writes one stream */
"await new Promise(fin=>{const pump=()=>{if(stop&&!active)return fin();"
"while(!stop&&next<list.length){const f=list[next],isBig=f.size>=BIG;if(big||active>=3||(isBig&&active))break;"
"next++;active++;up.k++;if(isBig)big=1;const id=next;"
"put(join(cwd,f.rel||f.webkitRelativePath||f.name),f,id).catch(e=>{bad.push(f.name+': '+e);if(e==0)stop=1})"
".finally(()=>{active--;if(isBig)big=0;pump()})}"
"if(!active&&(next>=list.length||stop))fin()};pump()});"
"$('prog').style.display='none';if(bad.length)alert(bad.length+' file(s) were not uploaded:\\n'+bad.slice(0,8).join('\\n'));load()}"
"$('pick').onchange=e=>{upload([...e.target.files]);e.target.value=''};$('pickd').onchange=e=>{upload([...e.target.files]);e.target.value=''};"
"async function walk(en,pre,out){if(en.isFile){await new Promise(r=>en.file(f=>{f.rel=pre+f.name;out.push(f);r()},r))}else if(en.isDirectory){"
"const rd=en.createReader();for(;;){const b=await new Promise(r=>rd.readEntries(r,()=>r([])));if(!b.length)break;for(const e of b)await walk(e,pre+en.name+'/',out)}}}"
"let dc=0;addEventListener('dragenter',e=>{e.preventDefault();dc++;$('drop').style.display='flex'});"
"addEventListener('dragleave',e=>{if(--dc<=0){dc=0;$('drop').style.display='none'}});addEventListener('dragover',e=>e.preventDefault());"
"addEventListener('drop',async e=>{e.preventDefault();dc=0;$('drop').style.display='none';const out=[],its=[...e.dataTransfer.items];"
"const ents=its.map(i=>i.webkitGetAsEntry&&i.webkitGetAsEntry()).filter(x=>x);if(ents.length){for(const en of ents)await walk(en,'',out)}else out.push(...e.dataTransfer.files);upload(out)});"
"fetch('/api/ls?p=/').then(r=>{show(r.ok);if(r.ok)load()});"
"</script></body></html>";

/* ---- HTTP helpers ---- */
static int send_all(int c, const void *b, size_t n){
    const char *p = b;
    while(n){ ssize_t w = send(c, p, n, MSG_NOSIGNAL); if(w < 0){ if(errno == EINTR) continue; return -1; } p += w; n -= (size_t)w; }
    return 0;
}
static void reply(int c, const char *status, const char *type, const char *body, const char *extra){
    char h[512]; size_t bl = body ? strlen(body) : 0;
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n%sConnection: close\r\n\r\n",
                     status, type, bl, extra ? extra : "");
    send_all(c, h, (size_t)n); if(bl) send_all(c, body, bl);
}
static void text(int c, const char *status, const char *msg){ reply(c, status, "text/plain; charset=utf-8", msg, NULL); }
static int hexv(int ch){ return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }
static void urldec(char *s, int plus){
    char *o = s;
    for(char *p = s; *p; p++){
        if(plus && *p == '+') *o++ = ' ';
        else if(*p == '%' && hexv((unsigned char)p[1]) >= 0 && hexv((unsigned char)p[2]) >= 0){ *o++ = (char)(hexv((unsigned char)p[1]) << 4 | hexv((unsigned char)p[2])); p += 2; }
        else *o++ = *p;
    }
    *o = 0;
}
static int qparam(const char *q, const char *key, char *out, size_t cap){   /* ?a=1&b=2 -> decoded value */
    size_t kl = strlen(key);
    for(const char *p = q; p && *p; ){
        if(!strncmp(p, key, kl) && p[kl] == '='){
            const char *v = p + kl + 1, *e = strchr(v, '&'); size_t n = e ? (size_t)(e - v) : strlen(v);
            if(n >= cap) return -1;
            memcpy(out, v, n); out[n] = 0; urldec(out, 1); return 0;
        }
        p = strchr(p, '&'); if(p) p++;
    }
    return -1;
}
/* a page path ("/Music/a b.flac") -> the real path on the card, or -1. must_exist: the thing itself has to exist
 * (else its folder must). Refuses "..", hidden parts, control characters, and anything resolving outside the card. */
static int safe_path(const char *rel, char *out, size_t cap, int must_exist){
    if(!rel || rel[0] != '/') return -1;
    size_t L = strlen(rel); if(L > 900) return -1;
    for(const char *p = rel; *p; p++) if((unsigned char)*p < 0x20 || *p == '\\') return -1;
    for(const char *p = rel; *p; ){
        while(*p == '/') p++;
        const char *e = strchr(p, '/'); size_t n = e ? (size_t)(e - p) : strlen(p);
        if(n && p[0] == '.') return -1;                                  /* "..", ".", ".diskos", ".part" files */
        p += n;
    }
    char full[PATH_MAX]; if(snprintf(full, sizeof full, WF_ROOT "%s", rel) >= (int)sizeof full) return -1;
    while(strlen(full) > strlen(WF_ROOT) && full[strlen(full) - 1] == '/') full[strlen(full) - 1] = 0;
    char real[PATH_MAX];
    if(realpath(full, real)){
        if(strncmp(real, WF_ROOT, strlen(WF_ROOT)) || (real[strlen(WF_ROOT)] && real[strlen(WF_ROOT)] != '/')) return -1;
    } else {
        if(must_exist) return -1;
        char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", full);
        char *sl = strrchr(dir, '/'); if(!sl) return -1; *sl = 0;
        /* the nearest existing folder above must be inside the card (folders that don't exist yet are made) */
        while(!realpath(dir, real)){ sl = strrchr(dir, '/'); if(!sl || sl == dir) return -1; *sl = 0; }
        if(strncmp(real, WF_ROOT, strlen(WF_ROOT)) || (real[strlen(WF_ROOT)] && real[strlen(WF_ROOT)] != '/')) return -1;
    }
    if(strlen(full) >= cap) return -1;
    snprintf(out, cap, "%s", full); return 0;
}
static int is_root(const char *p){ return !strcmp(p, WF_ROOT); }
static int mkdir_p(const char *dir){                                     /* parents of an upload */
    char t[PATH_MAX]; snprintf(t, sizeof t, "%s", dir);
    if(strlen(t) <= strlen(WF_ROOT)) return 0;                           /* the card itself */
    for(char *p = t + strlen(WF_ROOT) + 1; *p; p++) if(*p == '/'){ *p = 0; if(mkdir(t, 0777) && errno != EEXIST) return -1; *p = '/'; }
    return (mkdir(t, 0777) && errno != EEXIST) ? -1 : 0;
}
static int rm_rf(const char *p, int depth){
    struct stat st; if(lstat(p, &st)) return -1;
    if(!S_ISDIR(st.st_mode)) return unlink(p);
    if(depth > 24) return -1;
    DIR *d = opendir(p); if(!d) return -1;
    struct dirent *e; int rc = 0;
    while((e = readdir(d))){
        if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[PATH_MAX]; if(snprintf(c, sizeof c, "%s/%s", p, e->d_name) >= (int)sizeof c){ rc = -1; continue; }
        if(rm_rf(c, depth + 1)) rc = -1;
    }
    closedir(d);
    return rc ? rc : rmdir(p);
}
static void json_str(char *o, size_t cap, size_t *at, const char *s){
    if(*at + 2 >= cap) return;
    o[(*at)++] = '"';
    for(; *s && *at + 8 < cap; s++){
        unsigned char ch = (unsigned char)*s;
        if(ch == '"' || ch == '\\'){ o[(*at)++] = '\\'; o[(*at)++] = (char)ch; }
        else if(ch < 0x20) *at += (size_t)snprintf(o + *at, cap - *at, "\\u%04x", ch);
        else o[(*at)++] = (char)ch;
    }
    o[(*at)++] = '"'; o[*at] = 0;
}
static const char *mime(const char *p){
    const char *e = strrchr(p, '.'); if(!e) return "application/octet-stream"; e++;
    static const char *const M[][2] = { {"mp3","audio/mpeg"}, {"flac","audio/flac"}, {"m4a","audio/mp4"}, {"m4b","audio/mp4"}, {"aac","audio/aac"},
        {"ogg","audio/ogg"}, {"opus","audio/ogg"}, {"wav","audio/wav"}, {"jpg","image/jpeg"}, {"jpeg","image/jpeg"}, {"png","image/png"},
        {"gif","image/gif"}, {"webp","image/webp"}, {"txt","text/plain; charset=utf-8"}, {"lrc","text/plain; charset=utf-8"},
        {"m3u","text/plain; charset=utf-8"}, {"cue","text/plain; charset=utf-8"}, {"mp4","video/mp4"}, {"mkv","video/x-matroska"}, {"pdf","application/pdf"} };
    for(size_t i = 0; i < sizeof M / sizeof M[0]; i++) if(!strcasecmp(e, M[i][0])) return M[i][1];
    return "application/octet-stream";
}

/* ---- the handlers ---- */
static void do_ls(int c, const char *q){
    char rel[1024] = "/", path[PATH_MAX];
    qparam(q, "p", rel, sizeof rel);
    if(safe_path(rel, path, sizeof path, 1)){ text(c, "404 Not Found", "That folder isn't there any more."); return; }
    DIR *d = opendir(path); if(!d){ text(c, "404 Not Found", "That folder isn't there any more."); return; }
    size_t cap = 256 * 1024, at = 0; char *o = malloc(cap); if(!o){ closedir(d); text(c, "500 Internal Server Error", "Out of memory"); return; }
    struct statvfs vf; unsigned long long fr = 0, tot = 0;
    if(!statvfs(WF_ROOT, &vf)){ fr = (unsigned long long)vf.f_bavail * vf.f_frsize; tot = (unsigned long long)vf.f_blocks * vf.f_frsize; }
    at += (size_t)snprintf(o, cap, "{\"free\":%llu,\"total\":%llu,\"items\":[", fr, tot);
    struct dirent *e; int first = 1;
    while((e = readdir(d)) && at + 1200 < cap){
        if(e->d_name[0] == '.') continue;
        char fp[PATH_MAX]; struct stat st;
        if(snprintf(fp, sizeof fp, "%s/%s", path, e->d_name) >= (int)sizeof fp || stat(fp, &st)) continue;
        if(!first){ o[at++] = ','; } first = 0;
        at += (size_t)snprintf(o + at, cap - at, "{\"n\":");
        json_str(o, cap, &at, e->d_name);
        at += (size_t)snprintf(o + at, cap - at, ",\"d\":%d,\"s\":%lld,\"t\":%lld}", S_ISDIR(st.st_mode) ? 1 : 0, (long long)st.st_size, (long long)st.st_mtime);
    }
    closedir(d);
    at += (size_t)snprintf(o + at, cap - at, "]}");
    reply(c, "200 OK", "application/json", o, NULL); free(o);
}
static void do_get(int c, const char *rel, const char *q, const char *hdr, int head){
    char path[PATH_MAX]; struct stat st;
    if(safe_path(rel, path, sizeof path, 1) || stat(path, &st) || !S_ISREG(st.st_mode)){ text(c, "404 Not Found", "Not found"); return; }
    int f = open(path, O_RDONLY | O_CLOEXEC); if(f < 0){ text(c, "404 Not Found", "Not found"); return; }
    char dl[4] = ""; qparam(q, "dl", dl, sizeof dl);
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    char disp[700] = "", *w = disp; size_t left = sizeof disp;
    int k = snprintf(w, left, "Content-Disposition: %s; filename*=UTF-8''", dl[0] == '1' ? "attachment" : "inline"); w += k; left -= (size_t)k;
    for(const unsigned char *s = (const unsigned char *)base; *s && left > 8; s++){
        if(isalnum(*s) || strchr("-_.~", *s)){ *w++ = (char)*s; left--; } else { k = snprintf(w, left, "%%%02X", *s); w += k; left -= (size_t)k; }
    }
    snprintf(w, left, "\r\n");
    /* one byte range ("bytes=a-" / "bytes=a-b"): the browser's player seeks with these */
    long long size = (long long)st.st_size, from = 0, to = size - 1; int partial = 0;
    const char *rg = strcasestr(hdr, "\r\nrange: bytes=");
    if(rg && size > 0){
        char *e; long long a = strtoll(rg + 15, &e, 10);
        if(e != rg + 15 && *e == '-' && a >= 0 && a < size){
            from = a; partial = 1;
            if(isdigit((unsigned char)e[1])){ long long b = strtoll(e + 1, NULL, 10); if(b >= from && b < size) to = b; }
        }
    }
    long long len = size > 0 ? to - from + 1 : 0;
    char h[1200], cr[96] = "";
    if(partial) snprintf(cr, sizeof cr, "Content-Range: bytes %lld-%lld/%lld\r\n", from, to, size);
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\nAccept-Ranges: bytes\r\n%s%sConnection: close\r\n\r\n",
                     partial ? "206 Partial Content" : "200 OK", mime(path), len, cr, disp);
    if(send_all(c, h, (size_t)n) || head){ close(f); return; }
    off_t off = (off_t)from; long long rest = len;
    (void)posix_fadvise(f, 0, 0, POSIX_FADV_SEQUENTIAL);                 /* read ahead harder */
    if(!partial){ pthread_mutex_lock(&g_mu); snprintf(g_cur, sizeof g_cur, "%.95s", base); g_cur_done = 0; g_cur_total = size; pthread_mutex_unlock(&g_mu); }
    while(rest > 0 && atomic_load(&g_run)){
        size_t chunk = rest > (1 << 20) ? (1 << 20) : (size_t)rest;
        ssize_t s = sendfile(c, f, &off, chunk);
        if(s <= 0){ if(s < 0 && errno == EINTR) continue; break; }
        rest -= s; g_last_req_s = now_s();
        if(!partial){ pthread_mutex_lock(&g_mu); g_cur_done = off; pthread_mutex_unlock(&g_mu); }
    }
    close(f);
    if(!partial){ pthread_mutex_lock(&g_mu); if(rest == 0) g_sent++; g_cur[0] = 0; pthread_mutex_unlock(&g_mu); }
}
static void do_put(int c, const char *rel, long long len, const char *have, size_t nhave){
    char path[PATH_MAX];
    if(len < 0){ text(c, "411 Length Required", "Length required"); return; }
    if(safe_path(rel, path, sizeof path, 0)){ text(c, "400 Bad Request", "That name can't be used on the card."); return; }
    struct stat st; if(!stat(path, &st) && S_ISDIR(st.st_mode)){ text(c, "409 Conflict", "A folder has that name."); return; }
    struct statvfs vf;
    if(!statvfs(WF_ROOT, &vf) && (unsigned long long)vf.f_bavail * vf.f_frsize < (unsigned long long)len + (4ull << 20)){ text(c, "507 Insufficient Storage", "The SD card is full."); return; }
    char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", path); char *sl = strrchr(dir, '/'); *sl = 0;
    if(mkdir_p(dir)){ text(c, "500 Internal Server Error", "Couldn't make the folder."); return; }
    char part[PATH_MAX]; unsigned char rb[4]; rnd_bytes(rb, 4);
    if(snprintf(part, sizeof part, "%s/.%02x%02x%02x%02x.part", dir, rb[0], rb[1], rb[2], rb[3]) >= (int)sizeof part){ text(c, "400 Bad Request", "Name too long"); return; }
    int f = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666); if(f < 0){ text(c, "500 Internal Server Error", "Couldn't write to the card."); return; }
    const char *base = strrchr(path, '/') + 1;
    pthread_mutex_lock(&g_mu); snprintf(g_cur, sizeof g_cur, "%.95s", base); g_cur_done = 0; g_cur_total = len; pthread_mutex_unlock(&g_mu);
    long long got = 0; int ok = 1;
    size_t bsz = 1 << 20; char *buf = malloc(bsz); if(!buf){ bsz = 256 * 1024; buf = malloc(bsz); } if(!buf) ok = 0;
    if(ok && nhave){ size_t k = (size_t)((long long)nhave > len ? len : (long long)nhave); if(write(f, have, k) != (ssize_t)k) ok = 0; got += (long long)k; }
    while(ok && got < len && g_run){
        size_t want = (size_t)(len - got > (long long)bsz ? (long long)bsz : len - got);
        ssize_t r = recv(c, buf, want, MSG_WAITALL);                     /* (whole chunks: fewer, bigger writes) */
        if(r < 0 && errno == EINTR) continue;
        if(r <= 0){ ok = 0; break; }
        for(ssize_t w = 0; w < r; ){ ssize_t k = write(f, buf + w, (size_t)(r - w)); if(k <= 0){ if(k < 0 && errno == EINTR) continue; ok = 0; break; } w += k; }
        got += r; g_last_req_s = now_s();
        pthread_mutex_lock(&g_mu); g_cur_done = got; pthread_mutex_unlock(&g_mu);
    }
    free(buf);
    if(ok && got == len && !fsync(f) && !close(f) && !rename(part, path)){
        atomic_store(&g_changed, 1);
        pthread_mutex_lock(&g_mu); g_received++; g_cur[0] = 0; pthread_mutex_unlock(&g_mu);
        text(c, "201 Created", "ok");
    } else {
        if(f >= 0) close(f);
        unlink(part);
        pthread_mutex_lock(&g_mu); g_cur[0] = 0; pthread_mutex_unlock(&g_mu);
        text(c, "500 Internal Server Error", got < len ? "The upload was cut off." : "Couldn't write to the card.");
    }
}

static int check_auth(const char *hdr){
    const char *ck = strcasestr(hdr, "\r\ncookie:"); if(!ck) return 0;
    const char *e = strstr(ck + 2, "\r\n"); const char *s = strstr(ck, "dk=");
    if(!s || (e && s > e)) return 0;
    size_t L = strlen(g_sess); return !strncmp(s + 3, g_sess, L) && !isalnum((unsigned char)s[3 + L]);
}
static void handle(int c){
    struct timeval tv = { 30, 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char hb[8192]; size_t n = 0; char *he = NULL;
    while(n < sizeof hb - 1){
        ssize_t r = recv(c, hb + n, sizeof hb - 1 - n, 0);
        if(r < 0 && errno == EINTR) continue;
        if(r <= 0) return;
        n += (size_t)r; hb[n] = 0;
        if((he = strstr(hb, "\r\n\r\n"))) break;
    }
    if(!he){ text(c, "431 Request Header Fields Too Large", "Too large"); return; }
    g_last_req_s = now_s();
    *he = 0; const char *body = he + 4; size_t nbody = n - (size_t)(body - hb);
    char method[8] = "", target[2048] = "";
    if(sscanf(hb, "%7s %2047s", method, target) != 2){ text(c, "400 Bad Request", "Bad request"); return; }
    long long clen = -1; const char *cl = strcasestr(hb, "\r\ncontent-length:"); if(cl) clen = strtoll(cl + 17, NULL, 10);
    char *q = strchr(target, '?'); if(q) *q++ = 0; else q = (char *)"";
    if(!strcmp(target, "/") && !strcmp(method, "GET")){ reply(c, "200 OK", "text/html; charset=utf-8", PAGE, NULL); return; }
    if(!strcmp(target, "/favicon.ico")){ text(c, "404 Not Found", ""); return; }
    if(!strcmp(target, "/api/login") && !strcmp(method, "POST")){
        if(atomic_load(&g_fails) >= WF_MAXFAIL){ text(c, "429 Too Many Requests", "Locked"); return; }
        char pin[16] = ""; qparam(q, "pin", pin, sizeof pin);
        if(!strcmp(pin, g_pin)){
            atomic_store(&g_fails, 0);
            char ex[96]; snprintf(ex, sizeof ex, "Set-Cookie: dk=%s; Path=/; HttpOnly; SameSite=Strict\r\n", g_sess);
            reply(c, "200 OK", "text/plain", "ok", ex);
        } else { atomic_fetch_add(&g_fails, 1); usleep(400 * 1000); text(c, "403 Forbidden", "Wrong code"); }
        return;
    }
    if(!check_auth(hb)){ text(c, "401 Unauthorized", "Type the code shown on the Disc"); return; }
    if(!strcmp(target, "/api/ls")){ do_ls(c, q); return; }
    if(!strcmp(target, "/api/mkdir") && !strcmp(method, "POST")){
        char rel[1024], p[PATH_MAX];
        if(qparam(q, "p", rel, sizeof rel) || safe_path(rel, p, sizeof p, 0) || mkdir_p(p)){ text(c, "400 Bad Request", "Couldn't make that folder."); return; }
        atomic_store(&g_changed, 1); text(c, "200 OK", "ok"); return;
    }
    if(!strcmp(target, "/api/mv") && !strcmp(method, "POST")){
        char a[1024], b[1024], pa[PATH_MAX], pb[PATH_MAX]; struct stat st;
        if(qparam(q, "p", a, sizeof a) || qparam(q, "to", b, sizeof b) || safe_path(a, pa, sizeof pa, 1) || safe_path(b, pb, sizeof pb, 0) || is_root(pa)){
            text(c, "400 Bad Request", "That name can't be used."); return; }
        if(!stat(pb, &st)){ text(c, "409 Conflict", "Something already has that name."); return; }
        if(rename(pa, pb)){ text(c, "500 Internal Server Error", "Couldn't rename it."); return; }
        atomic_store(&g_changed, 1); text(c, "200 OK", "ok"); return;
    }
    if(!strncmp(target, "/f/", 3)){
        char rel[1200]; snprintf(rel, sizeof rel, "%.1199s", target + 2); urldec(rel, 0);
        if(!strcmp(method, "GET") || !strcmp(method, "HEAD")){ do_get(c, rel, q, hb, !strcmp(method, "HEAD")); return; }
        if(!strcmp(method, "PUT")){ do_put(c, rel, clen, body, nbody); return; }
        if(!strcmp(method, "DELETE")){
            char p[PATH_MAX];
            if(safe_path(rel, p, sizeof p, 1) || is_root(p)){ text(c, "404 Not Found", "Not found"); return; }
            if(rm_rf(p, 0)){ text(c, "500 Internal Server Error", "Couldn't delete all of it."); atomic_store(&g_changed, 1); return; }
            atomic_store(&g_changed, 1); text(c, "200 OK", "ok"); return;
        }
    }
    text(c, "404 Not Found", "Not found");
}
static void *conn_main(void *arg){
    int c = (int)(intptr_t)arg;
    int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    handle(c);
    shutdown(c, SHUT_WR); close(c);
    atomic_fetch_sub(&g_conns, 1);
    return NULL;
}
static void *srv_main(void *arg){
    (void)arg;
    while(atomic_load(&g_run)){
        struct pollfd pf = { g_fd, POLLIN, 0 };
        if(poll(&pf, 1, 500) <= 0) continue;
        int c = accept(g_fd, NULL, NULL); if(c < 0) continue;
        fcntl(c, F_SETFD, FD_CLOEXEC); fcntl(c, F_SETFL, fcntl(c, F_GETFL) & ~O_NONBLOCK);   /* blocking, with timeouts */
        if(atomic_load(&g_conns) >= WF_MAXCONN){ text(c, "503 Service Unavailable", "Busy, try again"); close(c); continue; }
        atomic_fetch_add(&g_conns, 1);
        pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&a, 64 * 1024 + PATH_MAX * 8);
        if(pthread_create(&t, &a, conn_main, (void *)(intptr_t)c)){ atomic_fetch_sub(&g_conns, 1); close(c); }
        pthread_attr_destroy(&a);
    }
    return NULL;
}

/* ---- the server itself, on / off ---- */
static int core_start(const int *ports, int nports){
    unsigned char r[20]; if(rnd_bytes(r, sizeof r)) return -1;
    snprintf(g_pin, sizeof g_pin, "%04u", (unsigned)(((unsigned)r[0] << 8 | r[1]) % 10000));
    static const char hx[] = "0123456789abcdef";
    for(int i = 0; i < 16; i++){ g_sess[i * 2] = hx[r[4 + i] >> 4]; g_sess[i * 2 + 1] = hx[r[4 + i] & 15]; } g_sess[32] = 0;
    int fd = -1;
    for(int k = 0; k < nports && fd < 0; k++){
        fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); if(fd < 0) break;
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        int big = 1 << 20;                                                /* a wide window (accepted sockets inherit it) */
        if(setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &big, sizeof big)) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
        if(setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, &big, sizeof big)) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
        struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)ports[k]); a.sin_addr.s_addr = inet_addr(g_ip);
        if(a.sin_addr.s_addr == INADDR_NONE || bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 8)){ close(fd); fd = -1; continue; }
        g_port = ports[k];
    }
    if(fd < 0) return -1;
    g_fd = fd; atomic_store(&g_run, 1); atomic_store(&g_fails, 0); atomic_store(&g_changed, 0);
    g_last_req_s = now_s(); g_received = g_sent = 0; g_cur[0] = 0;
    if(pthread_create(&g_th, NULL, srv_main, NULL)){ close(fd); g_fd = -1; atomic_store(&g_run, 0); return -1; }
    g_live = 1;
    wifi_powersave(1);
    return 0;
}
static void core_stop(void){
    if(!g_live) return;
    atomic_store(&g_run, 0);
    pthread_join(g_th, NULL);
    for(int i = 0; i < 40 && atomic_load(&g_conns) > 0; i++) usleep(50 * 1000);   /* the transfers notice g_run */
    if(g_fd >= 0){ close(g_fd); g_fd = -1; }
    g_live = 0;
    wifi_powersave(0); cpu_fast(0);
}

#ifndef WF_SERVER_ONLY
/* ---- on / off from the UI ---- */
static void sheet_paint(void);
static lv_timer_t *g_tick;
static void stop_server(int why){                                      /* 0 asked, 1 idle, 2 Wi-Fi gone, 3 locked */
    if(!g_live) return;
    core_stop();
    if(g_tick){ lv_timer_delete(g_tick); g_tick = NULL; }
    static const char *const WHY[4] = { "File transfer off", "File transfer off (unused for 10 min)", "File transfer off: Wi-Fi disconnected", "File transfer off: too many wrong codes" };
    if(atomic_exchange(&g_changed, 0)){
        ui_toast("Files changed - updating the library");
        if(!scanner_active()) ui_rescan_library();
    } else ui_toast(WHY[why < 0 || why > 3 ? 0 : why]);
    sheet_paint();
}
static void tick_cb(lv_timer_t *t){
    (void)t;
    if(!g_live) return;
    char ip[24];
    if(wlan_ip(ip, sizeof ip) || strcmp(ip, g_ip)){ stop_server(2); return; }
    if(atomic_load(&g_fails) >= WF_MAXFAIL){ stop_server(3); return; }
    int busy; pthread_mutex_lock(&g_mu); busy = g_cur[0] != 0; pthread_mutex_unlock(&g_mu);
    if(!busy && atomic_load(&g_conns) == 0 && (now_s() - g_last_req_s) * 1000u > WF_IDLE_MS){ stop_server(1); return; }
    static uint32_t busy_at;                                              /* full CPU speed while bytes move, 5 s more */
    if(busy){ busy_at = now_s(); cpu_fast(1); } else if(g_gov_fast && now_s() - busy_at >= 5) cpu_fast(0);
    sheet_paint();
}
int webfiles_busy(void){ if(!g_live) return 0; int b; pthread_mutex_lock(&g_mu); b = g_cur[0] != 0; pthread_mutex_unlock(&g_mu); return b; }
static int start_server(void){
    if(g_live) return 0;
    if(wlan_ip(g_ip, sizeof g_ip)){ ui_toast("Connect to Wi-Fi first"); wifi_open(); return -1; }
    static const int PORTS[2] = { 80, 8080 };
    if(core_start(PORTS, 2)){ ui_toast("Couldn't start file transfer"); return -1; }
    g_tick = lv_timer_create(tick_cb, 1000, NULL);
    return 0;
}
int webfiles_running(void){ return g_live; }

/* ---- the sheet: address, code, what's happening, Stop ---- */
static lv_obj_t *g_root, *g_card, *g_addr, *g_code, *g_stat, *g_btn_l;
static void sheet_close(void){ if(g_root){ lv_obj_t *r = g_root; g_root = NULL; g_addr = g_code = g_stat = g_btn_l = NULL; idisc_sheet_out(r, g_card); g_card = NULL; } }
static void fmt_size(char *o, size_t n, long long b){
    if(b < 1024 * 1024) snprintf(o, n, "%lld KB", b / 1024);
    else if(b < 1024LL * 1024 * 1024) snprintf(o, n, "%.1f MB", (double)b / 1048576.0);
    else snprintf(o, n, "%.2f GB", (double)b / 1073741824.0);
}
static void sheet_paint(void){
    if(!g_root) return;
    char b[160];
    if(!g_live){
        lv_label_set_text(g_addr, "Off"); lv_label_set_text(g_code, "");
        lv_label_set_text(g_stat, "Copy files between the Disc and a PC on the same Wi-Fi, in the PC's browser.");
        lv_label_set_text(g_btn_l, "Turn on"); return;
    }
    if(g_port == 80) snprintf(b, sizeof b, "%s", g_ip); else snprintf(b, sizeof b, "%s:%d", g_ip, g_port);   /* (browsers add the http://) */
    lv_label_set_text(g_addr, b);
    snprintf(b, sizeof b, "Code  %s", g_pin); lv_label_set_text(g_code, b);
    pthread_mutex_lock(&g_mu);
    if(g_cur[0]){ char d[24], t[24]; fmt_size(d, sizeof d, g_cur_done); fmt_size(t, sizeof t, g_cur_total);
                  snprintf(b, sizeof b, "%.40s\n%s of %s", g_cur, d, t); }
    else if(g_received || g_sent) snprintf(b, sizeof b, "%d received, %d sent", g_received, g_sent);
    else snprintf(b, sizeof b, "In a browser on your PC, open\nthe address above, type the code");
    pthread_mutex_unlock(&g_mu);
    lv_label_set_text(g_stat, b);
    lv_label_set_text(g_btn_l, "Stop");
}
static void outside_cb(lv_event_t *e){ if(lv_event_get_target(e) == g_root) sheet_close(); }
static void btn_cb(lv_event_t *e){ (void)e; if(g_live) stop_server(0); else start_server(); sheet_paint(); }
static lv_obj_t *lbl(lv_obj_t *p, const lv_font_t *f, lv_color_t c){
    lv_obj_t *l = lv_label_create(p); lv_label_set_text(l, ""); lv_obj_set_style_text_font(l, f, 0); lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0); lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE); kit_keep(l); return l;
}
void webfiles_open(void){
    sheet_close();
    g_root = lv_obj_create(lv_layer_top()); lv_obj_remove_style_all(g_root); lv_obj_set_size(g_root, 360, 360); kit_keep(g_root);
    lv_obj_set_style_bg_color(g_root, TC(SCRIM), 0); lv_obj_set_style_bg_opa(g_root, 150, 0);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_root, outside_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *card = lv_obj_create(g_root); lv_obj_remove_style_all(card); kit_keep(card);
    lv_obj_set_size(card, 296, 248); lv_obj_center(card); lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(card, 44, 0); lv_obj_set_style_bg_color(card, TC(SURFACE), 0); lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, th_null() ? 2 : 1, 0); lv_obj_set_style_border_color(card, th_null() ? TC(ACCENT_PRIMARY) : TC(BORDER), 0);
    if(th_idisc()) idisc_glass(card, 44);
    lv_obj_t *t = lbl(card, TF(HEADER), TC(TEXT_PRIMARY)); lv_label_set_text(t, th_nt_caps() ? "FILE TRANSFER" : "File transfer");
    if(th_nt()) nt_title_style(t);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 18);
    g_addr = lbl(card, th_nt_caps() ? TF(UI_20) : TF(UI_24), TC(TEXT_PRIMARY)); lv_obj_set_width(g_addr, 276); lv_obj_align(g_addr, LV_ALIGN_TOP_MID, 0, 54);
    lv_label_set_long_mode(g_addr, LV_LABEL_LONG_CLIP);
    g_code = lbl(card, TF(UI_32), th_null() ? TC(ACCENT_PRIMARY) : TC(TEXT_PRIMARY)); lv_obj_align(g_code, LV_ALIGN_TOP_MID, 0, 88);
    g_stat = lbl(card, TF(UI_14), TC(TEXT_SECONDARY)); lv_obj_set_width(g_stat, 250); lv_label_set_long_mode(g_stat, LV_LABEL_LONG_WRAP);
    lv_obj_align(g_stat, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_t *b = lv_button_create(card); lv_obj_remove_style_all(b); kit_keep(b);
    lv_obj_set_size(b, 140, 40); lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -16); lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(b, th_nt_caps() ? TC(TEXT_PRIMARY) : TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, NULL);
    g_btn_l = lbl(b, TF(UI_18), th_nt_caps() ? TC(CANVAS) : TC(ON_ACCENT)); lv_obj_center(g_btn_l);
    sheet_paint();
    g_card = card; idisc_sheet_in(g_root, card);
}
int webfiles_sheet_visible(void){ return g_root != NULL; }
void webfiles_demo(void){                                              /* host renders only: the sheet as if on */
    snprintf(g_ip, sizeof g_ip, "192.168.1.42"); snprintf(g_pin, sizeof g_pin, "4821"); g_port = 80; g_live = 1; g_received = 12; g_sent = 3;
}
/* Shortcuts / Quick Settings: on (and show the address), or open the sheet to see it / stop it */
void webfiles_toggle(void){
    if(!g_live) start_server();
    webfiles_open();
}
#endif /* WF_SERVER_ONLY */
