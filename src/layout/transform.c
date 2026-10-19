/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Font.
 *
 * Ghoti.io Font is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Font is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * text-transform: case, full-width and full-size kana mappings.
 */

#include <ghoti.io/font/layout.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/unicode/bidi.h>
#include <ghoti.io/unicode/break.h>
#include <ghoti.io/unicode/case.h>
#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/norm.h>
#include <ghoti.io/unicode/script.h>
#include <ghoti.io/unicode/utf.h>
#include <stdlib.h>
#include <string.h>
#include "kashida.h"
#include "layout_internal.h"

static uint32_t full_width_scalar(uint32_t cp, GFNT_WhiteSpace white_space) {
  if (cp >= 0x21 && cp <= 0x7e) return cp + 0xfee0;
  if (cp == 0x20) {
    return white_space == GFNT_WHITE_SPACE_PRE || white_space == GFNT_WHITE_SPACE_PRE_WRAP
            || white_space == GFNT_WHITE_SPACE_BREAK_SPACES ? 0x3000 : cp;
  }
  switch (cp) {
    case 0x2985: return 0xff5f;
    case 0x2986: return 0xff60;
    case 0x00a2: return 0xffe0;
    case 0x00a3: return 0xffe1;
    case 0x00ac: return 0xffe2;
    case 0x00af: return 0xffe3;
    case 0x00a6: return 0xffe4;
    case 0x00a5: return 0xffe5;
    case 0x20a9: return 0xffe6;
    default: return cp;
  }
}

static uint32_t full_size_kana_scalar(uint32_t cp) {
  switch (cp) {
    case 0x3041: return 0x3042; case 0x3043: return 0x3044;
    case 0x3045: return 0x3046; case 0x3047: return 0x3048;
    case 0x3049: return 0x304a; case 0x3095: return 0x304b;
    case 0x3096: return 0x3051; case 0x1b132: return 0x3053;
    case 0x3063: return 0x3064; case 0x3083: return 0x3084;
    case 0x3085: return 0x3086; case 0x3087: return 0x3088;
    case 0x308e: return 0x308f; case 0x1b150: return 0x3090;
    case 0x1b151: return 0x3091; case 0x1b152: return 0x3092;
    case 0x30a1: return 0x30a2; case 0x30a3: return 0x30a4;
    case 0x30a5: return 0x30a6; case 0x30a7: return 0x30a8;
    case 0x30a9: return 0x30aa; case 0x30f5: return 0x30ab;
    case 0x31f0: return 0x30af; case 0x30f6: return 0x30b1;
    case 0x1b155: return 0x30b3; case 0x31f1: return 0x30b7;
    case 0x31f2: return 0x30b9; case 0x30c3: return 0x30c4;
    case 0x31f3: return 0x30c8; case 0x31f4: return 0x30cc;
    case 0x31f5: return 0x30cf; case 0x31f6: return 0x30d2;
    case 0x31f7: return 0x30d5; case 0x31f8: return 0x30d8;
    case 0x31f9: return 0x30db; case 0x31fa: return 0x30e0;
    case 0x30e3: return 0x30e4; case 0x30e5: return 0x30e6;
    case 0x30e7: return 0x30e8; case 0x31fb: return 0x30e9;
    case 0x31fc: return 0x30ea; case 0x31fd: return 0x30eb;
    case 0x31fe: return 0x30ec; case 0x31ff: return 0x30ed;
    case 0x30ee: return 0x30ef; case 0x1b164: return 0x30f0;
    case 0x1b165: return 0x30f1; case 0x1b166: return 0x30f2;
    case 0x1b167: return 0x30f3; case 0xff67: return 0xff71;
    case 0xff68: return 0xff72; case 0xff69: return 0xff73;
    case 0xff6a: return 0xff74; case 0xff6b: return 0xff75;
    case 0xff6f: return 0xff82; case 0xff6c: return 0xff94;
    case 0xff6d: return 0xff95; case 0xff6e: return 0xff96;
    default: return cp;
  }
}

/** Apply CSS text transforms while retaining a monotone map to source UTF-8 bytes. */
GFNT_Result gfnt_lay_transform_case(const uint32_t * source, size_t source_length,
    const size_t * source_bytes, const GFNT_TextSpan * spans, size_t span_count,
    const size_t * source_span_cp, GFNT_WhiteSpace white_space, const GFNT_Allocator * allocator,
    uint32_t ** result_cps, size_t ** result_bytes, size_t * result_length,
    size_t ** result_span_cp, size_t ** result_source_to_layout) {
  uint32_t * mapped = NULL;
  size_t * mapped_bytes = NULL;
  size_t * mapped_spans = NULL;
  size_t * source_to_layout = NULL;
  size_t * breaks = NULL;
  size_t break_count = 0;
  size_t out = 0, span = 0, break_index = 0;
  bool word_has_cased = false;
  GUNI_BreakOptions word_options = { .kind = GUNI_BREAK_WORD };

  if (source_length > (SIZE_MAX - 1) / (GUNI_CASE_MAX_EXPANSION * GUNI_NORM_MAX_EXPANSION_NFKD)) return GFNT_ERR_INVALID;
  const size_t capacity = source_length * GUNI_CASE_MAX_EXPANSION * GUNI_NORM_MAX_EXPANSION_NFKD;
  mapped = ALLOC(mapped, capacity);
  mapped_bytes = allocator->calloc_fn(allocator->ctx, capacity + 1, sizeof *mapped_bytes);
  mapped_spans = ALLOC(mapped_spans, span_count + 1);
  source_to_layout = ALLOC(source_to_layout, source_length + 1);
  breaks = ALLOC(breaks, source_length + 1);
  if (!mapped || !mapped_bytes || !mapped_spans || !source_to_layout || !breaks) goto oom;
  if (source_length && guni_break_all_codepoints(&word_options, source, source_length,
          breaks, source_length + 1, &break_count) != GUNI_OK) goto invalid;
  mapped_spans[0] = 0;
  source_to_layout[0] = 0;

  for (size_t i = 0; i < source_length; i++) {
    uint32_t piece[GUNI_CASE_MAX_EXPANSION * GUNI_NORM_MAX_EXPANSION_NFKD];
    size_t piece_length = 1;
    const GFNT_TextStyle * style;
    GUNI_CaseTailoring tailoring;
    GUNI_GeneralCategory category = guni_general_category(source[i]);

    while (span + 1 < span_count && source_span_cp[span + 1] <= i) {
      span++;
      mapped_spans[span] = out;
    }
    while (break_index < break_count && breaks[break_index] < i) break_index++;
    if (break_index < break_count && breaks[break_index] == i) word_has_cased = false;
    style = &spans[span].style;
    tailoring = (GUNI_CaseTailoring)style->case_tailoring;
    piece[0] = source[i];
    const unsigned case_mode = (unsigned)style->text_transform & GFNT_TEXT_TRANSFORM_CASE_MASK;
    if (case_mode == GFNT_TEXT_TRANSFORM_UPPERCASE) {
      if (guni_to_upper_at(source, source_length, i, tailoring, piece,
              GUNI_CASE_MAX_EXPANSION, &piece_length) != GUNI_OK) goto invalid;
    }
    else if (case_mode == GFNT_TEXT_TRANSFORM_LOWERCASE) {
      if (guni_to_lower_at(source, source_length, i, tailoring, piece,
              GUNI_CASE_MAX_EXPANSION, &piece_length) != GUNI_OK) goto invalid;
    }
    else if (case_mode == GFNT_TEXT_TRANSFORM_CAPITALIZE
        && !word_has_cased && (GUNI_GC_MASK(category) & GUNI_GC_MASK_LC)) {
      uint32_t lowered[GUNI_CASE_MAX_EXPANSION];
      size_t lowered_length = 0;
      if (guni_to_lower_at(source, source_length, i, tailoring, lowered,
              GUNI_CASE_MAX_EXPANSION, &lowered_length) != GUNI_OK) goto invalid;
      if (lowered_length == 1 && lowered[0] == source[i]
          && guni_to_title_at(source, source_length, i, tailoring, piece,
              GUNI_CASE_MAX_EXPANSION, &piece_length) != GUNI_OK) goto invalid;
    }
    if (GUNI_GC_MASK(category) & GUNI_GC_MASK_LC) word_has_cased = true;
    for (size_t j = 0; j < piece_length; j++) {
      uint32_t expanded[GUNI_NORM_MAX_EXPANSION_NFKD];
      size_t expanded_length = 1;
      expanded[0] = piece[j];
      if ((style->text_transform & GFNT_TEXT_TRANSFORM_FULL_WIDTH)
          && guni_decomposition_type(piece[j]) == GUNI_DT_NARROW) {
        if (guni_decompose(piece[j], true, expanded, GUNI_NORM_MAX_EXPANSION_NFKD,
                &expanded_length) != GUNI_OK) goto invalid;
      }
      else if (style->text_transform & GFNT_TEXT_TRANSFORM_FULL_WIDTH) {
        expanded[0] = full_width_scalar(piece[j], white_space);
      }
      for (size_t k = 0; k < expanded_length; k++) {
        if (style->text_transform & GFNT_TEXT_TRANSFORM_FULL_SIZE_KANA) {
          expanded[k] = full_size_kana_scalar(expanded[k]);
        }
        mapped[out] = expanded[k];
        mapped_bytes[out++] = source_bytes[i];
      }
    }
    mapped_bytes[out] = source_bytes[i + 1];
    source_to_layout[i + 1] = out;
  }
  while (span + 1 < span_count) {
    span++;
    mapped_spans[span] = out;
  }
  mapped_spans[span_count] = out;
  allocator->free_fn(allocator->ctx, breaks);
  *result_cps = mapped;
  *result_bytes = mapped_bytes;
  *result_length = out;
  *result_span_cp = mapped_spans;
  *result_source_to_layout = source_to_layout;
  return GFNT_OK;

oom:
  allocator->free_fn(allocator->ctx, mapped);
  allocator->free_fn(allocator->ctx, mapped_bytes);
  allocator->free_fn(allocator->ctx, mapped_spans);
  allocator->free_fn(allocator->ctx, source_to_layout);
  allocator->free_fn(allocator->ctx, breaks);
  return GFNT_ERR_OOM;
invalid:
  allocator->free_fn(allocator->ctx, mapped);
  allocator->free_fn(allocator->ctx, mapped_bytes);
  allocator->free_fn(allocator->ctx, mapped_spans);
  allocator->free_fn(allocator->ctx, source_to_layout);
  allocator->free_fn(allocator->ctx, breaks);
  return GFNT_ERR_INVALID;
}
