/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef JPCARDS_H
#define JPCARDS_H
#include <stddef.h>
/* one flashcard: vocabulary = { word, reading (kana; "" when the word is kana), NULL, meaning };
 * kanji = { kanji, on readings (katakana), kun readings (hiragana, okurigana in brackets), meaning } */
typedef struct { const char *front, *a, *b, *meaning; } jp_card_t;
extern const jp_card_t JP_VOCAB_N5[], JP_VOCAB_N4[], JP_KANJI_N5[], JP_KANJI_N4[];
extern const int JP_VOCAB_N5_N, JP_VOCAB_N4_N, JP_KANJI_N5_N, JP_KANJI_N4_N;
#endif
