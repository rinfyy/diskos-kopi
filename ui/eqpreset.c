#include "simple_theme.h"
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "screens.h"
#include "braun.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "musicdb.h"
#include "fwcaps.h"
#include "eqrate.h"
#include "eqprofile.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static lv_obj_t *name_label,*edit_button,*rename_button,*switch_button,*switch_label,*note_label;
static int rename_preset;
static lv_obj_t *switch_pointer;
static const char *const names[]={"Off","Jazz","Rock","R&B","Hip-Hop","Pop","Dance","Classical","Retro","Sibilance 1","Sibilance 2"};
static int selected(void){int p=simple_cfg_get_int("eq_preset",0);return p<0?0:p>20?20:p;}
static void name_for(int p,char *b,size_t n){
 if(p<=10){snprintf(b,n,"%s",names[p]);return;}
 char key[24];snprintf(key,sizeof key,"eq_name%d",p);const char *s=simple_cfg_get_str(key,NULL);
 if(s && *s)snprintf(b,n,"%s",s);else snprintf(b,n,"USER%d",p-10);
}
static unsigned profile_ticket;
static lv_timer_t *profile_timer;
static eq_profile_t g_prof; static int g_prof_ok;                 /* the selected preset's curve, once read */
static lv_obj_t *reset_button,*copy_button,*paste_button;
static void tools_paint(void);
static void profile_paint(int p,int read,int graphic,const int hz[10]){
 int user=p>=11 && read>=0 && graphic;
 if(user){lv_obj_remove_state(edit_button,LV_STATE_DISABLED);lv_obj_remove_state(rename_button,LV_STATE_DISABLED);}
 else {lv_obj_add_state(edit_button,LV_STATE_DISABLED);lv_obj_add_state(rename_button,LV_STATE_DISABLED);}
 tools_paint();
 lv_label_set_text(note_label,read==-2?"Loading profile...":read<0?"Profile unavailable":p<11?"Built-in preset":!graphic?"Parametric profile":!fw_custom_peq_curve_writable(hz,10)?"Custom editing unavailable":"Editable profile");
}
static void profile_poll(lv_timer_t *timer){
 (void)timer;if(screen_current()!=SCR_EQ || !profile_ticket)return;
 eq_profile_t result;
 if(eq_profile_take(profile_ticket,&result)){
  profile_ticket=0;
  if(result.preset==selected()){g_prof=result;g_prof_ok=result.read>=0;profile_paint(result.preset,result.read,result.graphic,result.hz);}
 }
}

/* ==== Zune (Figma 36): the presets on a dial, the one in use in the accent; "equalizer on" with a switch at the foot;
 * edit / rename under it while a user profile is on the focus line ==== */
#include "zune_dial.h"
static void toggle(lv_event_t *e); static void renamed(const char *text);
LV_FONT_DECLARE(font_zune_semibold_11) LV_FONT_DECLARE(font_zune_regular_14) LV_FONT_DECLARE(font_zune_regular_19) LV_FONT_DECLARE(font_zune_regular_13)
static lv_obj_t *g_zroot, *g_zsw, *g_zlinks; static lv_timer_t *g_zfoc_tm; static int g_zlast_focus = -1;
static char g_znames[20][40];
static void zlow(char *o, size_t n, const char *s){ size_t i = 0; for(; s[i] && i + 1 < n; i++) o[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]; o[i] = 0; }
static void zsw_paint(void){
    if(!g_zsw) return;
    int on = selected() != 0;
    lv_obj_set_style_bg_color(g_zsw, on ? TC(ACCENT_PRIMARY) : TC(CONTROL_TRACK), 0);
    lv_obj_t *k = lv_obj_get_child(g_zsw, 0); if(k) lv_obj_align(k, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -3 : 3, 0);
}
static void zfoc_tick(lv_timer_t *t){
    (void)t; if(screen_current() != SCR_EQ || !g_zlinks) return;
    int f = zdial_focus(); if(f == g_zlast_focus) return; g_zlast_focus = f;
    if(f >= 10) lv_obj_remove_flag(g_zlinks, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(g_zlinks, LV_OBJ_FLAG_HIDDEN);
}
static void zeq_build(void);
static void zeq_rebuild(void *u){ (void)u; zeq_build(); }
static void zeq_pick(int i){
    int p = i + 1;
    if(ui_eq_select(p) < 0){ ui_toast("Couldn't switch EQ"); return; }
    lv_async_call(zeq_rebuild, NULL);
}
static void zeq_toggle(lv_event_t *e){ toggle(e); lv_async_call(zeq_rebuild, NULL); }
static void zeq_edit(lv_event_t *e){ (void)e; int f = zdial_focus() + 1; if(f != selected() && ui_eq_select(f) < 0) return; screen_show(SCR_EQ_EDITOR); }
static void zeq_rename(lv_event_t *e){ (void)e; rename_preset = zdial_focus() + 1; char b[80]; name_for(rename_preset, b, sizeof b); kbinput_open("Rename EQ", b, renamed); }
static lv_obj_t *zlink(lv_obj_t *p, const char *t, lv_color_t c, lv_event_cb_t cb){
    lv_obj_t *l = lv_label_create(p); kit_keep(l); lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, zune_font(&font_zune_regular_19), 0); lv_obj_set_style_text_color(l, c, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE); lv_obj_set_ext_click_area(l, 12);
    lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, NULL); return l;
}
static void zeq_build(void){
    lv_obj_t *root = g_zroot; if(!root) return;
    lv_obj_clean(root); g_zsw = g_zlinks = NULL; g_zlast_focus = -1;
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0); lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    const char *lb[20];
    for(int i = 0; i < 20; i++){ char b[40]; name_for(i + 1, b, sizeof b); zlow(g_znames[i], sizeof g_znames[i], b); lb[i] = g_znames[i]; }
    int cur = selected(), focus = cur ? cur - 1 : simple_cfg_get_int("eq_last", 1) - 1;
    if(focus < 0 || focus >= 20) focus = 0;
    zdial_build(root, lb, 20, focus, zeq_pick, 44, "eq.zune");
    zdial_set_bottom(250);
    if(cur) zdial_set_color(cur - 1, TC(ACCENT_PRIMARY));
    for(int k = 0; k < 2; k++){                                   /* fades so the dial melts into the caption and the foot */
        lv_obj_t *f = lv_obj_create(root); lv_obj_remove_style_all(f); kit_keep(f);
        lv_obj_set_size(f, 360, k ? 96 : 62); lv_obj_align(f, k ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(f, TC(CANVAS), 0); lv_obj_set_style_bg_grad_color(f, TC(CANVAS), 0);
        lv_obj_set_style_bg_grad_dir(f, LV_GRAD_DIR_VER, 0); lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_opa(f, k ? LV_OPA_TRANSP : LV_OPA_COVER, 0); lv_obj_set_style_bg_grad_opa(f, k ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_main_stop(f, k ? 0 : 140, 0); lv_obj_set_style_bg_grad_stop(f, k ? 120 : 255, 0);
        lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *cap = lv_label_create(root); kit_keep(cap); lv_label_set_text(cap, "EQUALIZER");
    lv_obj_set_style_text_font(cap, &font_zune_semibold_11, 0); lv_obj_set_style_text_letter_space(cap, 2, 0);
    lv_obj_set_style_text_color(cap, TC(TEXT_MUTED), 0); lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 26);
    /* the foot: "equalizer on" + switch, and edit / rename for user profiles */
    lv_obj_t *row = lv_obj_create(root); lv_obj_remove_style_all(row); kit_keep(row);
    lv_obj_set_size(row, 200, 42); lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 284);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, zeq_toggle, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = lv_label_create(row); kit_keep(t); lv_label_set_text(t, "equalizer on");
    lv_obj_set_style_text_font(t, zune_font(&font_zune_regular_19), 0); lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 8, 0);
    g_zsw = lv_obj_create(row); lv_obj_remove_style_all(g_zsw); kit_keep(g_zsw);
    lv_obj_set_size(g_zsw, 36, 20); lv_obj_set_style_radius(g_zsw, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_bg_opa(g_zsw, LV_OPA_COVER, 0);
    lv_obj_align(g_zsw, LV_ALIGN_RIGHT_MID, -8, 0); lv_obj_remove_flag(g_zsw, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *k = lv_obj_create(g_zsw); lv_obj_remove_style_all(k); kit_keep(k);
    lv_obj_set_size(k, 14, 14); lv_obj_set_style_radius(k, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(k, TC(FIXED_MEDIA_WHITE), 0); lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    zsw_paint();
    g_zlinks = lv_obj_create(root); lv_obj_remove_style_all(g_zlinks); kit_keep(g_zlinks);
    lv_obj_set_size(g_zlinks, 180, 32); lv_obj_align(g_zlinks, LV_ALIGN_TOP_MID, 0, 250);
    lv_obj_remove_flag(g_zlinks, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ed = zlink(g_zlinks, "edit", TC(ACCENT_PRIMARY), zeq_edit); lv_obj_align(ed, LV_ALIGN_LEFT_MID, 26, 0);
    lv_obj_t *rn = zlink(g_zlinks, "rename", TC(TEXT_PRIMARY), zeq_rename); lv_obj_align(rn, LV_ALIGN_RIGHT_MID, -16, 0);
    if(!g_zfoc_tm) g_zfoc_tm = lv_timer_create(zfoc_tick, 150, NULL);
    zfoc_tick(NULL);
}
void eqpreset_refresh(void){
 if(th_zune()){ if(fw_os_ver()==257)eq_rate_request(); zeq_build(); return; }
 if(!name_label)return;if(fw_os_ver()==257)eq_rate_request();int p=selected();char b[80];name_for(p,b,sizeof b);if(th_null()){char up[80];theme_upper(up,sizeof up,b);lv_label_set_text(name_label,up);}else lv_label_set_text(name_label,b);
 profile_ticket=0;g_prof_ok=0;
 if(p>=11){profile_ticket=eq_profile_request(p);profile_paint(p,profile_ticket?-2:-1,0,NULL);}
 else {if(p>=1)profile_ticket=eq_profile_request(p);profile_paint(p,0,0,NULL);}   /* built-ins are read too: they can be copied */
 lv_label_set_text(switch_label,p?"On":"Off");
 if(switch_pointer&&th_braun())br_pointer_set(switch_pointer,58,p);
 if(switch_pointer)lv_obj_set_style_bg_color(switch_pointer,p?theme_on_color(ui_current_accent()):TC(TEXT_MUTED),0);
 lv_obj_set_style_bg_color(switch_button,p?ui_current_accent():TC(SURFACE),0);
 lv_obj_set_style_text_color(switch_label,p?theme_on_color(ui_current_accent()):TC(TEXT_PRIMARY),0);
 if(th_simple()){if(p)lv_obj_add_state(switch_button,LV_STATE_CHECKED);else lv_obj_remove_state(switch_button,LV_STATE_CHECKED);lv_obj_set_style_bg_grad_dir(switch_button,LV_GRAD_DIR_NONE,0);lv_obj_set_style_bg_color(switch_button,p?TC(ACCENT_PRIMARY):theme_color_mix(TC(ACCENT_PRIMARY),TC(CANVAS),24),0);lv_obj_set_style_text_color(switch_label,p?theme_on_color(TC(ACCENT_PRIMARY)):TC(TEXT_PRIMARY),0);lv_obj_add_flag(switch_pointer,LV_OBJ_FLAG_HIDDEN);}
 if(th_null()){
  lv_obj_set_style_bg_opa(switch_button,0,0);lv_obj_set_style_border_width(switch_button,1,0);
  lv_obj_set_style_border_color(switch_button,p?TC(TEXT_PRIMARY):TC(TEXT_SECONDARY),0);
  lv_obj_set_style_text_color(switch_label,p?TC(TEXT_PRIMARY):TC(TEXT_SECONDARY),0);
 }
}
static void cycle(lv_event_t *e){int dir=(int)(intptr_t)lv_event_get_user_data(e);int p=(selected()+dir+21)%21;
 if(ui_eq_select(p)<0){ui_toast("Couldn't switch EQ");return;}eqpreset_refresh();}
static void toggle(lv_event_t *e){(void)e;int p=0;if(!selected()){p=simple_cfg_get_int("eq_last",1);if(p<1||p>20)p=1;}
 if(ui_eq_select(p)<0){ui_toast("Couldn't switch EQ");return;}eqpreset_refresh();}
static void edit(lv_event_t *e){(void)e;screen_show(SCR_EQ_EDITOR);}
/* "IM4 Harman" (kopi.27, in place of the JM-1 add-on): the Fosi Audio IM4 open-back IEM brought toward the Harman
 * in-ear 2019 target. The IM4 is reserved in the bass (an open back), a little recessed in the lower mids, forward
 * above 2 kHz with peaks near 5 and 8 kHz, and soft in the top octave; so: a bass lift falling away by 150 Hz, a touch
 * of lower mids, the upper mids and the two treble peaks taken down (narrower filters there), and some air back.
 * The -5 dB preamp keeps the biggest boost from clipping. Tuned from published descriptions of the IM4 (stock nozzle
 * and tips), not a measurement of your pair: fit and tips move the bass and treble, so treat it as a starting point
 * (Edit adjusts it like any USER preset). One tap stores it in a USER slot of its own - the one already called
 * "IM4 Harman" (or the old "JM-1 (IM4)"), else the first unnamed one - and turns it on. */
#define IM4_NAME "IM4 Harman"
static void im4_cb(lv_event_t *e){
 (void)e;
 static const int hz[10]={30,70,150,400,1000,2500,5000,8000,12000,16000},t[10]={55,45,20,10,0,-10,-30,-30,15,10};
 static const double q[10]={0.7,0.7,0.8,0.9,1.0,1.4,2.0,2.5,1.0,0.8};
 int eqcustom_store(int slot,const int *hz,const int *tenths,const double *q,double master);
 int slot=-1;char key[24];
 for(int p=11;p<=20&&slot<0;p++){snprintf(key,sizeof key,"eq_name%d",p);const char *n=simple_cfg_get_str(key,NULL);if(n&&(!strcmp(n,IM4_NAME)||!strcmp(n,"JM-1 (IM4)")))slot=p;}
 for(int p=11;p<=20&&slot<0;p++){snprintf(key,sizeof key,"eq_name%d",p);const char *n=simple_cfg_get_str(key,NULL);if(!n||!*n)slot=p;}
 if(slot<0){ui_toast("All 10 USER presets are in use - rename or reset one first");return;}
 if(eqcustom_store(slot,hz,t,q,-5.0)<0)return;
 snprintf(key,sizeof key,"eq_name%d",slot);simple_cfg_set_str(key,IM4_NAME);
 char b[64];snprintf(b,sizeof b,"IM4 Harman on (USER%d)",slot-10);ui_toast(b);eqpreset_refresh();
}
/* ---- Reset / Copy / Paste ----
 * Copy keeps the selected preset's curve (bands, Q, preamp) - a built-in one too - in cfg "eq_clip", so it survives a
 * restart. Paste writes it over the selected USER preset; Reset makes that preset flat. Both ask first. */
int eqcustom_store(int slot,const int *hz,const int *tenths,const double *q,double master);
int eqcustom_store_flat(int slot);
static int clip_get(int hz[10],int t[10],double q[10],double *m){
 const char *s=simple_cfg_get_str("eq_clip",NULL);if(!s||!*s)return 0;
 int n=0;for(int i=0;i<10;i++){int k=0;if(sscanf(s,"%d:%d:%lf%n",&hz[i],&t[i],&q[i],&k)!=3)return 0;s+=k;if(*s==',')s++;n++;}
 if(sscanf(s,";%lf",m)!=1)*m=0;
 return n==10;
}
static void tools_paint(void){
 if(!reset_button)return;
 int p=selected(),hz[10],t[10];double q[10],m;
 if(p>=11){lv_obj_remove_state(reset_button,LV_STATE_DISABLED);}else lv_obj_add_state(reset_button,LV_STATE_DISABLED);
 if(p>=1&&g_prof_ok&&g_prof.graphic)lv_obj_remove_state(copy_button,LV_STATE_DISABLED);else lv_obj_add_state(copy_button,LV_STATE_DISABLED);
 if(p>=11&&clip_get(hz,t,q,&m))lv_obj_remove_state(paste_button,LV_STATE_DISABLED);else lv_obj_add_state(paste_button,LV_STATE_DISABLED);
}
static int tool_target;
static void do_reset(void){
 if(tool_target!=selected()||tool_target<11)return;
 if(eqcustom_store_flat(tool_target)>0){char b[80],n[64];name_for(tool_target,n,sizeof n);snprintf(b,sizeof b,"%s reset to flat",n);ui_toast(b);eqpreset_refresh();}
}
static void reset_cb(lv_event_t *e){
 (void)e;int p=selected();if(p<11){ui_toast("Built-in presets can't be changed");return;}
 tool_target=p;char t[96],n[64];name_for(p,n,sizeof n);snprintf(t,sizeof t,"Reset %s?",n);
 fileops_confirm(t,"Back to flat: every band at 0 dB on its standard frequency, preamp 0.","Reset",do_reset);
}
static void copy_cb(lv_event_t *e){
 (void)e;int p=selected();
 if(p<1){ui_toast("EQ is off - nothing to copy");return;}
 if(!g_prof_ok){ui_toast("Still reading this preset");return;}
 if(!g_prof.graphic){ui_toast("This preset uses filters the editor can't copy");return;}
 char b[400];int n=0;
 for(int i=0;i<10;i++)n+=snprintf(b+n,sizeof b-(size_t)n,"%s%d:%d:%.2f",i?",":"",g_prof.hz[i],g_prof.gains[i],g_prof.q[i]>0?g_prof.q[i]:0.7);
 snprintf(b+n,sizeof b-(size_t)n,";%.1f",g_prof.master);
 if(simple_cfg_set_str("eq_clip",b)){ui_toast("Couldn't copy");return;}
 char nm[64],t[96];name_for(p,nm,sizeof nm);snprintf(t,sizeof t,"Copied %s",nm);ui_toast(t);tools_paint();
}
static void do_paste(void){
 int hz[10],t[10];double q[10],m;
 if(tool_target!=selected()||tool_target<11||!clip_get(hz,t,q,&m))return;
 if(eqcustom_store(tool_target,hz,t,q,m)>0){char b[80],n[64];name_for(tool_target,n,sizeof n);snprintf(b,sizeof b,"Pasted into %s",n);ui_toast(b);eqpreset_refresh();}
}
static void paste_cb(lv_event_t *e){
 (void)e;int p=selected(),hz[10],t[10];double q[10],m;
 if(p<11){ui_toast("Paste into a USER preset");return;}
 if(!clip_get(hz,t,q,&m)){ui_toast("Copy a preset first");return;}
 tool_target=p;char tt[96],n[64];name_for(p,n,sizeof n);snprintf(tt,sizeof tt,"Paste into %s?",n);
 fileops_confirm(tt,"The copied curve replaces this preset's curve.","Paste",do_paste);
}
static void renamed(const char *text){if(!text||!*text)return;char key[24];snprintf(key,sizeof key,"eq_name%d",rename_preset);
 if(simple_cfg_set_str(key,text)){ui_toast("Couldn't save profile name");return;}eqpreset_refresh();}
static void rename_cb(lv_event_t *e){(void)e;rename_preset=selected();char b[80];name_for(rename_preset,b,sizeof b);kbinput_open("Rename EQ",b,renamed);}
static lv_obj_t *button(lv_obj_t *root,const char *text,int x,int y,int w,int h,lv_event_cb_t cb,void *data,const char *action){
 lv_obj_t *b=lv_button_create(root);lv_obj_set_size(b,w,h);lv_obj_align(b,LV_ALIGN_TOP_MID,x,y);
 lv_obj_set_style_bg_color(b,TC(SURFACE),0);lv_obj_set_style_radius(b,w==h?LV_RADIUS_CIRCLE:14,0);lv_obj_set_style_border_width(b,0,0);
 lv_obj_t *l=lv_label_create(b);lv_label_set_text(l,text);lv_obj_set_style_text_font(l,TF(UI_18),0);lv_obj_set_style_text_color(l,TC(TEXT_PRIMARY),0);lv_obj_center(l);
 ui_on(b,cb,LV_EVENT_CLICKED,data,action,UI_CORE);return b;
}
void eqpreset_create(lv_obj_t *root){
 if(th_zune()){ g_zroot=root; return; }
 lv_obj_set_style_bg_color(root,TC(CANVAS),0);ui_header(root,"Equalizer");
 lv_obj_t *prev_b=button(root,LV_SYMBOL_LEFT,-118,123,42,42,cycle,(void *)(intptr_t)-1,"eq.previous");
 lv_obj_t *next_b=button(root,LV_SYMBOL_RIGHT,118,123,42,42,cycle,(void *)(intptr_t)1,"eq.next");
 name_label=lv_label_create(root);lv_obj_set_width(name_label,178);lv_label_set_long_mode(name_label,LV_LABEL_LONG_SCROLL_CIRCULAR);lv_obj_align(name_label,LV_ALIGN_TOP_MID,0,128);
 lv_obj_set_style_text_align(name_label,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_text_font(name_label,TF(UI_24),0);lv_obj_set_style_text_color(name_label,TC(TEXT_PRIMARY),0);
 edit_button=button(root,"Edit",-60,176,104,44,edit,NULL,"eq.edit");rename_button=button(root,"Rename",60,176,104,44,rename_cb,NULL,"eq.rename");
 reset_button=button(root,"Reset",-94,228,84,30,reset_cb,NULL,"eq.reset");copy_button=button(root,"Copy",0,228,84,30,copy_cb,NULL,"eq.copy");
 paste_button=button(root,"Paste",94,228,84,30,paste_cb,NULL,"eq.paste");
 {lv_obj_t *tb[3]={reset_button,copy_button,paste_button};for(int i=0;i<3;i++){lv_obj_set_style_text_font(lv_obj_get_child(tb[i],0),TF(UI_14),0);
   lv_obj_set_style_opa(tb[i],LV_OPA_40,LV_STATE_DISABLED);}}   /* greyed out when it can't apply here */
 {lv_obj_t *jm=button(root,"IM4 Harman",0,72,150,32,im4_cb,NULL,"eq.im4");lv_obj_set_style_text_font(lv_obj_get_child(jm,0),TF(UI_14),0);
  if(th_null()){lv_obj_set_style_radius(jm,0,0);lv_obj_set_style_bg_opa(jm,0,0);lv_obj_set_style_border_width(jm,1,0);lv_obj_set_style_border_color(jm,TC(TEXT_SECONDARY),0);kit_keep(jm);}}
 note_label=lv_label_create(root);lv_obj_set_width(note_label,280);lv_obj_align(note_label,LV_ALIGN_TOP_MID,0,266);lv_obj_set_style_text_align(note_label,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_text_font(note_label,TF(UI_12),0);lv_obj_set_style_text_color(note_label,TC(TEXT_SECONDARY),0);
 switch_button=button(root,"Off",0,286,54,54,toggle,NULL,"eq.toggle");switch_label=lv_obj_get_child(switch_button,0);
 switch_pointer=lv_obj_create(switch_button);lv_obj_remove_style_all(switch_pointer);
 lv_obj_set_size(switch_pointer,3,9);lv_obj_align(switch_pointer,LV_ALIGN_TOP_MID,10,4);
 lv_obj_set_style_bg_opa(switch_pointer,LV_OPA_COVER,0);lv_obj_set_style_radius(switch_pointer,1,0);
 lv_obj_set_style_transform_rotation(switch_pointer,300,0);lv_obj_clear_flag(switch_pointer,LV_OBJ_FLAG_CLICKABLE);
 if(th_disco()&&!th_nt()){   /* fork: Disco keeps the navigation circle's sliver on the right rim - the controls step left of it */
  for(uint32_t k=0;k<lv_obj_get_child_count(root);k++){
   lv_obj_t *o=lv_obj_get_child(root,(int32_t)k);
   if(o==name_label||o==edit_button||o==rename_button||o==note_label||o==switch_button||o==reset_button||o==copy_button||o==paste_button) lv_obj_set_style_translate_x(o,disco_mirror()?20:-20,0);   /* Left: step right of it */
  }
  lv_obj_set_width(name_label,150);
  lv_obj_set_style_translate_x(next_b,disco_mirror()?4:-36,0); lv_obj_set_style_translate_x(prev_b,disco_mirror()?36:-4,0);
  lv_obj_add_flag(switch_pointer,LV_OBJ_FLAG_HIDDEN);
 }
 if(th_null()){
  {lv_obj_t *tb[3]={reset_button,copy_button,paste_button};for(int i=0;i<3;i++){lv_obj_set_style_radius(tb[i],0,0);lv_obj_set_style_bg_opa(tb[i],0,0);
    lv_obj_set_style_border_width(tb[i],1,0);lv_obj_set_style_border_color(tb[i],TC(TEXT_SECONDARY),0);kit_keep(tb[i]);}}
  lv_label_set_long_mode(name_label,LV_LABEL_LONG_SCROLL_CIRCULAR);lv_obj_set_width(name_label,192);
  lv_obj_align(name_label,LV_ALIGN_TOP_MID,0,125);lv_obj_set_style_text_font(name_label,TF(HEADER_SM),0);
  lv_obj_t *buttons[]={prev_b,next_b,edit_button,rename_button,switch_button};
  for(int i=0;i<5;i++){
   lv_obj_t *b=buttons[i];lv_obj_set_style_radius(b,0,0);lv_obj_set_style_bg_opa(b,0,0);
   lv_obj_set_style_text_color(b,TC(TEXT_PRIMARY),0);kit_keep(b);
   lv_obj_t *l=lv_obj_get_child(b,0);if(l){lv_obj_set_style_text_color(l,TC(TEXT_SECONDARY),0);kit_keep(l);}
  }
  lv_obj_set_style_text_font(lv_obj_get_child(edit_button,0),TF(HEADER_SM),0);
  lv_obj_set_style_text_font(lv_obj_get_child(rename_button,0),TF(HEADER_SM),0);
  lv_obj_set_style_text_font(switch_label,TF(HEADER_SM),0);
  lv_obj_set_size(switch_button,104,44);lv_obj_align(switch_button,LV_ALIGN_TOP_MID,0,288);
  lv_obj_add_flag(switch_pointer,LV_OBJ_FLAG_HIDDEN);kit_keep(name_label);
 }
 if(th_nt())lv_obj_add_flag(switch_pointer,LV_OBJ_FLAG_HIDDEN);
 if(!profile_timer)profile_timer=lv_timer_create(profile_poll,100,NULL);
 /* Screen entry schedules a read; this page never waits on SQLite. */
}
