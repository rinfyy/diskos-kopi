/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* theme_defs.c - the unique themes' definitions: fonts per role, texture, title case, component kit.
 * Colours are in theme_presets.inc (tools/gen_theme_presets.py); fonts in font_<theme>_*.c
 * (tools/gen_theme_fonts.py). Roles a theme leaves NULL keep Default's font (mostly the icon-bearing sizes 22+). */
#include "theme.h"
#include "theme_kit.h"
#include "theme_fonts.h"
#include <string.h>

LV_FONT_DECLARE(diskos_mono_14)
LV_FONT_DECLARE(diskos_mono_16)
LV_FONT_DECLARE(diskos_mono_18)
LV_FONT_DECLARE(diskos_mono_20)
LV_FONT_DECLARE(diskos_mono_24)
LV_FONT_DECLARE(diskos_mono_bold_12)
LV_FONT_DECLARE(diskos_dot_18)
LV_FONT_DECLARE(diskos_dot_28)

LV_FONT_DECLARE(font_nt_dot_28)
LV_FONT_DECLARE(font_nt_dot_48)
LV_FONT_DECLARE(font_nt_dot_72)
LV_FONT_DECLARE(font_nt_dot_96)
LV_FONT_DECLARE(font_nt_mono_12)
LV_FONT_DECLARE(font_nt_mono_14)
LV_FONT_DECLARE(font_nt_mono_16)
LV_FONT_DECLARE(font_nt_mono_18)
LV_FONT_DECLARE(font_nt_mono_20)
LV_FONT_DECLARE(font_nt_mono_22)
LV_FONT_DECLARE(font_nt_mono_24)
LV_FONT_DECLARE(font_nt_mono_28)
LV_FONT_DECLARE(font_nt_mono_64)
LV_FONT_DECLARE(font_nt_mono_88)
LV_FONT_DECLARE(font_nt_sans_12)
LV_FONT_DECLARE(font_nt_sans_14)
LV_FONT_DECLARE(font_nt_sans_16)
LV_FONT_DECLARE(font_nt_sans_18)
LV_FONT_DECLARE(font_nt_sans_20)
LV_FONT_DECLARE(font_nt_sans_22)
LV_FONT_DECLARE(font_nt_sans_24)
LV_FONT_DECLARE(font_nt_sans_28)
LV_FONT_DECLARE(font_id_text_12)
LV_FONT_DECLARE(font_id_text_14)
LV_FONT_DECLARE(font_id_text_16)
LV_FONT_DECLARE(font_id_text_18)
LV_FONT_DECLARE(font_id_text_20)
LV_FONT_DECLARE(font_id_text_22)
LV_FONT_DECLARE(font_id_text_24)
LV_FONT_DECLARE(font_id_text_28)
LV_FONT_DECLARE(font_id_semi_14)
LV_FONT_DECLARE(font_id_semi_16)
LV_FONT_DECLARE(font_id_semi_18)
LV_FONT_DECLARE(font_id_semi_20)
LV_FONT_DECLARE(font_id_semi_22)
LV_FONT_DECLARE(font_id_semi_24)
LV_FONT_DECLARE(font_id_semi_28)
LV_FONT_DECLARE(font_id_semi_34)
LV_FONT_DECLARE(font_id_clock_80)
LV_FONT_DECLARE(font_id_clock_100)

#define TRAIT(t) (1u << THEME_TRAIT_##t)
#define F(role) [THEME_FONT_##role]

extern const theme_kit_t theme_kit_ring, theme_kit_braun, theme_kit_zune, theme_kit_nt, theme_kit_idisc;
extern const theme_kit_t theme_kit_stone;

extern const theme_kit_t theme_kit_null, theme_kit_simple;
LV_FONT_DECLARE(font_simple_icons_36)
LV_FONT_DECLARE(font_simple_icons_80)
LV_FONT_DECLARE(font_simple_nunito_28)
LV_FONT_DECLARE(font_simple_nunito_32)
LV_FONT_DECLARE(font_simple_nunito_38)
LV_FONT_DECLARE(font_simple_nunito_26)
LV_FONT_DECLARE(font_simple_nunito_36)
LV_FONT_DECLARE(font_np_nunito_22)
LV_FONT_DECLARE(font_np_nunito_30)
LV_FONT_DECLARE(font_inter_medium_20)
LV_FONT_DECLARE(null_pixel_14) LV_FONT_DECLARE(null_pixel_21) LV_FONT_DECLARE(null_pixel_28) LV_FONT_DECLARE(null_pixel_56)
LV_FONT_DECLARE(null_fine_12) LV_FONT_DECLARE(null_fine_14) LV_FONT_DECLARE(null_fine_16) LV_FONT_DECLARE(null_fine_18) LV_FONT_DECLARE(null_fine_20) LV_FONT_DECLARE(null_fine_24)
LV_FONT_DECLARE(font_zune_light_18)
LV_FONT_DECLARE(font_zune_light_21)
LV_FONT_DECLARE(font_zune_light_24)
LV_FONT_DECLARE(font_zune_light_32)
LV_FONT_DECLARE(font_zune_light_46)
LV_FONT_DECLARE(font_zune_light_50)
LV_FONT_DECLARE(font_zune_light_88)
LV_FONT_DECLARE(font_zune_regular_12)
LV_FONT_DECLARE(font_zune_regular_13)
LV_FONT_DECLARE(font_zune_regular_14)
LV_FONT_DECLARE(font_zune_regular_15)
LV_FONT_DECLARE(font_zune_regular_19)
LV_FONT_DECLARE(font_zune_semibold_10)
LV_FONT_DECLARE(font_zune_semibold_11)
LV_FONT_DECLARE(font_zune_semibold_22)
LV_FONT_DECLARE(font_zune_semibold_12)
LV_FONT_DECLARE(font_zune_semibold_24)
LV_FONT_DECLARE(font_zune_regular_16)
LV_FONT_DECLARE(font_zune_regular_17)
#define NT_FONTS(T) F(UI_10)=&font_nt_##T##_12,F(UI_12)=&font_nt_##T##_12,F(UI_14)=&font_nt_##T##_14,F(UI_16)=&font_nt_##T##_16,\
    F(UI_18)=&font_nt_##T##_18,F(UI_20)=&font_nt_##T##_20,F(UI_22)=&font_nt_##T##_22,F(UI_24)=&font_nt_##T##_24,F(UI_26)=&font_nt_##T##_24,\
    F(UI_28)=&font_nt_##T##_28,F(UI_30)=&font_nt_##T##_28,F(UI_32)=&font_nt_##T##_28,F(UI_36)=&font_nt_##T##_28,F(UI_40)=&font_nt_##T##_28,\
    F(USER_14)=&font_nt_##T##_14,F(USER_16)=&font_nt_##T##_16,F(USER_18)=&font_nt_##T##_18,F(USER_20)=&font_nt_##T##_20,\
    F(HEADER)=&font_nt_##T##_22,F(HEADER_SM)=&font_nt_##T##_18,F(DATE)=&font_nt_##T##_16,\
    F(MUSIC_TITLE)=&font_nt_##T##_22,F(MUSIC_ARTIST)=&font_nt_##T##_16,F(MUSIC_ALBUM)=&font_nt_##T##_14
static const theme_def_t DEFS[] = {
 { .name="NTDisk", .font={NT_FONTS(mono),F(CLOCK)=&font_nt_dot_72,F(SAVER_CLOCK)=&font_nt_dot_96,F(UI_48)=&font_nt_dot_48},
   .texture=THEME_TEX_NONE, .title_case=THEME_CASE_UPPER, .fixed_accent=1, .kit=&theme_kit_nt },
 { .name="NTDisk Sans", .font={NT_FONTS(sans),F(CLOCK)=&font_nt_dot_72,F(SAVER_CLOCK)=&font_nt_dot_96,F(UI_48)=&font_nt_dot_48},
   .texture=THEME_TEX_NONE, .title_case=THEME_CASE_UPPER, .fixed_accent=1, .kit=&theme_kit_nt },
 { .name="NTDisk Mono", .font={NT_FONTS(mono),F(CLOCK)=&font_nt_mono_64,F(SAVER_CLOCK)=&font_nt_mono_88,F(UI_48)=&font_nt_mono_28},
   .texture=THEME_TEX_NONE, .title_case=THEME_CASE_UPPER, .fixed_accent=1, .kit=&theme_kit_nt },
 { .name="iDisc", .font={F(UI_10)=&font_id_text_12,F(UI_12)=&font_id_text_12,F(UI_14)=&font_id_text_14,F(UI_16)=&font_id_text_16,
     F(UI_18)=&font_id_text_18,F(UI_20)=&font_id_text_20,F(UI_22)=&font_id_text_22,F(UI_24)=&font_id_text_24,F(UI_26)=&font_id_text_24,
     F(UI_28)=&font_id_text_28,F(UI_30)=&font_id_text_28,F(UI_32)=&font_id_semi_28,F(UI_36)=&font_id_semi_34,F(UI_40)=&font_id_semi_34,
     F(USER_14)=&font_id_text_14,F(USER_16)=&font_id_text_16,F(USER_18)=&font_id_text_18,F(USER_20)=&font_id_text_20,
     F(HEADER)=&font_id_semi_20,F(HEADER_SM)=&font_id_semi_18,F(DATE)=&font_id_semi_18,
     F(MUSIC_TITLE)=&font_id_semi_22,F(MUSIC_ARTIST)=&font_id_text_18,F(MUSIC_ALBUM)=&font_id_text_14,
     F(CLOCK)=&font_id_clock_80,F(SAVER_CLOCK)=&font_id_clock_100},
   .texture=THEME_TEX_NONE, .title_case=THEME_CASE_KEEP, .fixed_accent=1, .kit=&theme_kit_idisc },
 { .name="Zune", .font={F(UI_10)=&font_zune_regular_14,F(UI_12)=&font_zune_regular_16,F(UI_14)=&font_zune_regular_16,F(UI_16)=&font_zune_regular_17,F(UI_18)=&font_zune_light_18,F(UI_20)=&font_zune_regular_19,F(UI_22)=&font_zune_light_21,F(UI_24)=&font_zune_light_24,F(UI_28)=&font_zune_light_32,F(UI_32)=&font_zune_light_32,F(UI_46)=&font_zune_light_46,F(USER_14)=&font_zune_regular_16,F(USER_16)=&font_zune_regular_17,F(USER_18)=&font_zune_light_18,F(USER_20)=&font_zune_light_21,F(HEADER)=&font_zune_light_32,F(HEADER_SM)=&font_zune_light_24,F(CLOCK)=&font_zune_light_88,F(SAVER_CLOCK)=&font_zune_light_88,F(DATE)=&font_zune_regular_16,F(MUSIC_TITLE)=&font_zune_semibold_22,F(MUSIC_ARTIST)=&font_zune_regular_17,F(MUSIC_ALBUM)=&font_zune_regular_14}, .texture=THEME_TEX_NONE, .title_case=THEME_CASE_LOWER, .kit=&theme_kit_zune },
 { .name="Squash", .font={F(MUSIC_TITLE)=&font_simple_nunito_36,F(MUSIC_ARTIST)=&font_simple_nunito_32,F(MUSIC_ALBUM)=&font_simple_nunito_28,F(QS_ICON_36)=&font_simple_icons_36,F(LAUNCHER_ICON_80)=&font_simple_icons_80,F(UI_26)=&font_simple_nunito_26,F(UI_36)=&font_simple_nunito_36,F(UI_22)=&font_np_nunito_22,F(UI_24)=&font_np_nunito_22,F(UI_28)=&font_np_nunito_30,F(UI_30)=&font_np_nunito_30,F(UI_32)=&font_np_nunito_30,F(HEADER)=&font_np_nunito_30,F(HEADER_SM)=&font_np_nunito_22,F(UI_20)=&font_inter_medium_20}, .texture=THEME_TEX_NONE, .title_case=THEME_CASE_KEEP, .kit=&theme_kit_simple },
 { .name="Null",.font={F(UI_10)=&null_fine_12,F(UI_12)=&null_fine_12,F(UI_14)=&null_fine_16,F(UI_16)=&null_fine_18,F(UI_18)=&null_fine_20,F(UI_20)=&null_fine_24,F(UI_22)=&null_pixel_21,F(UI_24)=&null_pixel_21,F(UI_28)=&null_pixel_28,F(UI_32)=&null_pixel_28,F(UI_36)=&null_pixel_28,F(UI_40)=&null_pixel_28,F(USER_14)=&null_fine_16,F(USER_16)=&null_fine_18,F(USER_18)=&null_fine_20,F(USER_20)=&null_fine_24,F(HEADER)=&null_pixel_21,F(HEADER_SM)=&null_pixel_14,F(CLOCK)=&null_pixel_28,F(SAVER_CLOCK)=&null_pixel_56,F(DATE)=&null_pixel_14},.traits=TRAIT(FLAT_LISTS),.title_case=THEME_CASE_UPPER,.texture=THEME_TEX_NONE,.kit=&theme_kit_null },
    { .name="Ring", .texture=THEME_TEX_NONE, .title_case=THEME_CASE_KEEP, .kit=&theme_kit_ring },
    { .name="Braun", .texture=THEME_TEX_NONE, .title_case=THEME_CASE_KEEP, .fixed_accent=1, .kit=&theme_kit_braun },
    { .name="Disco", .texture=THEME_TEX_NONE, .title_case=THEME_CASE_KEEP, .kit=&theme_kit_ring },   /* Ring's components */
    { .name = "stone",
      .font = { F(UI_12) = &diskos_stone_ui_12, F(UI_14) = &diskos_stone_ui_14, F(USER_14) = &diskos_stone_ui_14,
                F(UI_16) = &diskos_stone_ui_16, F(USER_16) = &diskos_stone_ui_16, F(UI_18) = &diskos_stone_ui_18,
                F(USER_18) = &diskos_stone_ui_18, F(UI_20) = &diskos_stone_ui_20, F(USER_20) = &diskos_stone_ui_20,
                F(UI_24) = &diskos_stone_ui_24, F(HEADER) = &diskos_stone_ui_20, F(CLOCK) = &diskos_stone_clk_64,
                F(SAVER_CLOCK) = &diskos_stone_clk_64, F(DATE) = &diskos_stone_ui_16 },
      .texture = THEME_TEX_NONE, .title_case = THEME_CASE_KEEP, .fixed_accent = 1, .kit = &theme_kit_stone },

};

const theme_def_t *theme_def_find(const char *name){
    for(unsigned i = 0; name && i < sizeof DEFS / sizeof DEFS[0]; i++) if(!strcmp(DEFS[i].name, name)) return &DEFS[i];
    return NULL;
}
