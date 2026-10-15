/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Font.
 *
 * Ghoti.io Font is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 */
#include "layout_internal.h"

#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/enums.h>
#include <ghoti.io/unicode/script.h>
#include <limits.h>
#include <stdint.h>

/* Independent C implementation of ICU's pinned Japanese phrase predictor.
 * The table data and full license are in third_party/icu/. */
typedef struct PhraseFeature {
  uint8_t length;
  uint32_t units[3];
  int32_t weight;
} PhraseFeature;

#include "phrase_model_ja.h"

#define TABLE_COUNT(table) (sizeof(table) / sizeof((table)[0]))

typedef struct PhraseTable {
  const PhraseFeature * features;
  size_t count;
} PhraseTable;

static int compare_feature(const PhraseFeature * a, uint8_t length,
    const uint32_t units[3]) {
  if (a->length != length) return a->length < length ? -1 : 1;
  for (size_t i = 0; i < 3; i++) {
    if (a->units[i] != units[i]) return a->units[i] < units[i] ? -1 : 1;
  }
  return 0;
}

static int32_t feature_weight(const PhraseTable * table, const uint32_t * text,
    size_t text_length, int64_t start, int64_t end) {
  uint32_t units[3] = {0, 0, 0};
  uint8_t length;
  size_t lo = 0, hi = table->count;

  if (start < 0) start = 0;
  if (end < 0) end = 0;
  if ((uint64_t)start > text_length) start = (int64_t)text_length;
  if ((uint64_t)end > text_length) end = (int64_t)text_length;
  if (end < start) {
    int64_t swap = start;
    start = end;
    end = swap;
  }
  if (end - start > 3) return 0;
  length = (uint8_t)(end - start);
  for (uint8_t i = 0; i < length; i++) units[i] = text[start + i];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp = compare_feature(&table->features[mid], length, units);
    if (!cmp) return table->features[mid].weight;
    if (cmp < 0) lo = mid + 1;
    else hi = mid;
  }
  return 0;
}

static bool japanese_dictionary_character(uint32_t cp) {
  GUNI_Script script = guni_script(cp);

  return script == GUNI_SCRIPT_HAN || script == GUNI_SCRIPT_HIRAGANA
      || script == GUNI_SCRIPT_KATAKANA || (cp >= 0xAC00 && cp <= 0xD7A3);
}

static bool phrase_context_character(uint32_t cp) {
  const uint32_t category = guni_general_category_mask(cp);

  return (category & GUNI_GC_MASK_L) != 0
      || (category & (GUNI_GC_MASK(GUNI_GC_DECIMAL_NUMBER)
          | GUNI_GC_MASK(GUNI_GC_INITIAL_PUNCTUATION)
          | GUNI_GC_MASK(GUNI_GC_OPEN_PUNCTUATION))) != 0;
}

static bool phrase_close_punctuation(uint32_t cp) {
  const uint32_t category = guni_general_category_mask(cp);
  const uint32_t punctuation = GUNI_GC_MASK(GUNI_GC_CONNECTOR_PUNCTUATION)
      | GUNI_GC_MASK(GUNI_GC_DASH_PUNCTUATION)
      | GUNI_GC_MASK(GUNI_GC_CLOSE_PUNCTUATION)
      | GUNI_GC_MASK(GUNI_GC_FINAL_PUNCTUATION)
      | GUNI_GC_MASK(GUNI_GC_OTHER_PUNCTUATION);

  return (category & punctuation) != 0;
}

GFNT_Result gfnt_phrase_japanese_boundaries(const uint32_t * cps, size_t cp_count,
    const GFNT_Allocator * allocator, bool ** out) {
  const PhraseTable tables[13] = {
    {phrase_uw1, TABLE_COUNT(phrase_uw1)}, {phrase_uw2, TABLE_COUNT(phrase_uw2)},
    {phrase_uw3, TABLE_COUNT(phrase_uw3)}, {phrase_uw4, TABLE_COUNT(phrase_uw4)},
    {phrase_uw5, TABLE_COUNT(phrase_uw5)}, {phrase_uw6, TABLE_COUNT(phrase_uw6)},
    {phrase_bw1, TABLE_COUNT(phrase_bw1)}, {phrase_bw2, TABLE_COUNT(phrase_bw2)},
    {phrase_bw3, TABLE_COUNT(phrase_bw3)}, {phrase_tw1, TABLE_COUNT(phrase_tw1)},
    {phrase_tw2, TABLE_COUNT(phrase_tw2)}, {phrase_tw3, TABLE_COUNT(phrase_tw3)},
    {phrase_tw4, TABLE_COUNT(phrase_tw4)}
  };
  bool * boundaries = NULL;

  if (!out || (!cps && cp_count) || !allocator || !allocator->calloc_fn
      || !allocator->free_fn) {
    return GFNT_ERR_INVALID;
  }
  if (cp_count > (size_t)(INT64_MAX - 3)) return GFNT_ERR_LIMIT;
  *out = NULL;
  boundaries = allocator->calloc_fn(allocator->ctx, cp_count + 1, sizeof *boundaries);
  if (!boundaries) {
    return GFNT_ERR_OOM;
  }

  for (size_t start = 0; start < cp_count;) {
    size_t end;

    while (start < cp_count && !japanese_dictionary_character(cps[start])) start++;
    if (start == cp_count) break;
    end = start + 1;
    while (end < cp_count && japanese_dictionary_character(cps[end])) end++;
    if (start > 0 && phrase_close_punctuation(cps[start - 1])) boundaries[start] = true;
    for (size_t cp = start + 1; cp < end; cp++) {
      const int64_t at = (int64_t)(cp - start);
      const size_t run_length = end - start;
      const uint32_t * run = cps + start;
      int64_t score = phrase_ja_base_score;

      score += feature_weight(&tables[0], run, run_length, at - 3, at - 2);
      score += feature_weight(&tables[1], run, run_length, at - 2, at - 1);
      score += feature_weight(&tables[2], run, run_length, at - 1, at);
      score += feature_weight(&tables[3], run, run_length, at, at + 1);
      score += feature_weight(&tables[4], run, run_length, at + 1, at + 2);
      score += feature_weight(&tables[5], run, run_length, at + 2, at + 3);
      score += feature_weight(&tables[6], run, run_length, at - 2, at);
      score += feature_weight(&tables[7], run, run_length, at - 1, at + 1);
      score += feature_weight(&tables[8], run, run_length, at, at + 2);
      score += feature_weight(&tables[9], run, run_length, at - 3, at);
      score += feature_weight(&tables[10], run, run_length, at - 2, at + 1);
      score += feature_weight(&tables[11], run, run_length, at - 1, at + 2);
      score += feature_weight(&tables[12], run, run_length, at, at + 3);
      boundaries[cp] = score > 0;
    }
    if (end < cp_count && phrase_context_character(cps[end])) boundaries[end] = true;
    start = end;
  }
  *out = boundaries;
  return GFNT_OK;
}
