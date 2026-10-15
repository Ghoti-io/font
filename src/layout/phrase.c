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

#include <limits.h>
#include <stdint.h>

typedef struct PhraseFeature {
  uint8_t length;
  uint32_t units[3];
  int32_t weight;
} PhraseFeature;

#include "phrase_model_ja.h"

typedef struct PhraseTable {
  const PhraseFeature * features;
  size_t count;
} PhraseTable;

static uint32_t phrase_character_class(uint32_t cp);

static int compare_feature(const PhraseFeature * feature, uint8_t length,
    const uint32_t units[3]) {
  if (feature->length != length) return feature->length < length ? -1 : 1;
  for (size_t i = 0; i < 3; i++) {
    if (feature->units[i] != units[i]) {
      return feature->units[i] < units[i] ? -1 : 1;
    }
  }
  return 0;
}

static int32_t feature_weight(const PhraseTable * table, const uint32_t * cps,
    size_t cp_count, size_t boundary, int offset, uint8_t width, bool classes) {
  uint32_t units[3] = {0, 0, 0};
  int64_t start = (int64_t)boundary + offset;
  int64_t end = start + width;
  uint8_t length;
  size_t low = 0, high = table->count;

  if (start < 0) start = 0;
  if (end < 0) end = 0;
  if ((uint64_t)start > cp_count) start = (int64_t)cp_count;
  if ((uint64_t)end > cp_count) end = (int64_t)cp_count;
  if (end < start) {
    int64_t swap = start;
    start = end;
    end = swap;
  }
  length = (uint8_t)(end - start);
  for (uint8_t i = 0; i < length; i++) {
    units[i] = classes ? 0x130000u + phrase_character_class(cps[start + i])
        : cps[start + i];
  }

  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int comparison = compare_feature(&table->features[middle], length, units);
    if (!comparison) return table->features[middle].weight;
    if (comparison < 0) low = middle + 1;
    else high = middle;
  }
  return 0;
}

static uint32_t phrase_character_class(uint32_t cp) {
  if (cp == 0x20 || cp == 0x09 || cp == 0x0A || cp == 0x0D || cp == 0x3000) return 1;
  if ((cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF)
      || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x323AF)) return 2;
  if ((cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x1B001 && cp <= 0x1B11F)) return 3;
  if ((cp >= 0x30A0 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF)
      || (cp >= 0xFF66 && cp <= 0xFF9F)) return 4;
  if ((cp >= 0x30 && cp <= 0x39) || (cp >= 0xFF10 && cp <= 0xFF19)) return 5;
  if ((cp >= 0x41 && cp <= 0x5A) || (cp >= 0x61 && cp <= 0x7A)
      || (cp >= 0xFF21 && cp <= 0xFF3A) || (cp >= 0xFF41 && cp <= 0xFF5A)) return 6;
  if (cp == 0x300C || cp == 0x300E || cp == 0xFF62 || cp == 0xFF08 || cp == 0xFF3B
      || cp == 0x3010 || cp == 0x3008 || cp == 0x300A) return 7;
  if (cp == 0x300D || cp == 0x300F || cp == 0xFF63 || cp == 0xFF09 || cp == 0xFF3D
      || cp == 0x3011 || cp == 0x3009 || cp == 0x300B) return 8;
  if (cp == 0x3001 || cp == 0xFF0C || cp == 0x3002 || cp == 0xFF0E || cp == 0x30FB
      || cp == 0xFF1A || cp == 0xFF1B) return 9;
  if (cp == 0x30FC || cp == 0xFF70 || cp == 0x309D || cp == 0x309E
      || cp == 0x30FD || cp == 0x30FE) return 10;
  return 11;
}

int32_t gfnt_phrase_japanese_boundary_score(const uint32_t * cps, size_t cp_count,
    size_t line_start, size_t boundary, size_t fit_end, GFNT_F26Dot6 line_width) {
  static const int offsets[13] = {-3, -2, -1, 0, 1, 2, -2, -1, 0, -3, -2, -1, 0};
  static const uint8_t widths[13] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3};
  PhraseTable tables[35];
  int64_t score = (int64_t)phrase_model_ja_bias - phrase_model_ja_threshold;

  if (!cps || line_start >= boundary || boundary >= cp_count
      || fit_end < boundary || fit_end > cp_count) return INT32_MIN;
  for (size_t table = 0; table < 35; table++) {
    tables[table].features = phrase_model_ja_features[table];
    tables[table].count = phrase_model_ja_feature_counts[table];
  }
  for (size_t table = 0; table < 13; table++) {
    score += feature_weight(&tables[table], cps, cp_count, boundary,
        offsets[table], widths[table], false);
    score += feature_weight(&tables[13 + table], cps, cp_count, boundary,
        offsets[table], widths[table], true);
  }
  for (size_t offset = 0; offset < 3; offset++) {
    score += feature_weight(&tables[26 + offset], cps, cp_count, line_start,
        (int)offset, 1, false);
    score += feature_weight(&tables[29 + offset], cps, cp_count, line_start,
        (int)offset, 1, true);
  }
  {
    uint32_t units[3] = {0x110000u + (uint32_t)((boundary - line_start - 1) > 8
        ? 8 : boundary - line_start - 1), 0, 0};
    size_t low = 0, high = tables[32].count;
    while (low < high) {
      size_t middle = low + (high - low) / 2;
      int comparison = compare_feature(&tables[32].features[middle], 1, units);
      if (!comparison) {
        score += tables[32].features[middle].weight;
        break;
      }
      if (comparison < 0) low = middle + 1;
      else high = middle;
    }
  }
  {
    size_t width_bucket = line_width > 0 ? (size_t)line_width / (10u * 64u) : 0;
    uint32_t units[3] = {0x140000u + (uint32_t)(width_bucket > 63 ? 63 : width_bucket), 0, 0};
    size_t low = 0, high = tables[34].count;
    while (low < high) {
      size_t middle = low + (high - low) / 2;
      int comparison = compare_feature(&tables[34].features[middle], 1, units);
      if (!comparison) {
        score += tables[34].features[middle].weight;
        break;
      }
      if (comparison < 0) low = middle + 1;
      else high = middle;
    }
  }
  {
    size_t fit = fit_end - line_start - 1;
    uint32_t units[3] = {0x120000u + (uint32_t)(fit > 32 ? 32 : fit), 0, 0};
    size_t low = 0, high = tables[33].count;
    while (low < high) {
      size_t middle = low + (high - low) / 2;
      int comparison = compare_feature(&tables[33].features[middle], 1, units);
      if (!comparison) {
        score += tables[33].features[middle].weight;
        break;
      }
      if (comparison < 0) low = middle + 1;
      else high = middle;
    }
  }
  if (score > INT32_MAX) score = INT32_MAX;
  if (score < INT32_MIN) score = INT32_MIN;
  return (int32_t)score;
}
