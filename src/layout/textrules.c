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
 * Character and text rules the layout shares: white-space, word-break and
 * ideograph predicates, text-autospace and text-spacing-trim.
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
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "kashida.h"
#include "layout_internal.h"

bool gfnt_lay_is_newline(uint32_t cp) {
  return cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x85
      || cp == 0x2028 || cp == 0x2029;
}

bool gfnt_lay_css_segment_break(uint32_t cp) {
  return cp == 0x0A || cp == 0x0D;
}

bool gfnt_lay_css_collapsible_space(uint32_t cp) {
  return cp == 0x20 || cp == 0x09 || gfnt_lay_css_segment_break(cp);
}

bool gfnt_lay_paragraph_hard_break(const GFNT_Paragraph * p, uint32_t cp) {
  if (p->options.white_space == GFNT_WHITE_SPACE_NORMAL
      || p->options.white_space == GFNT_WHITE_SPACE_NOWRAP) {
    return false;
  }
  if (p->options.white_space == GFNT_WHITE_SPACE_PRE_LINE) {
    return gfnt_lay_css_segment_break(cp);
  }
  return gfnt_lay_is_newline(cp);
}

/** CSS Text's typographic-letter classes used by word-break. */
static GUNI_LineBreak word_break_class(uint32_t cp, GUNI_LineBreakTailoring tailoring) {
  return guni_line_break_resolve(guni_line_break(cp), guni_general_category(cp), tailoring);
}

bool gfnt_lay_word_break_letter(uint32_t cp, GUNI_LineBreakTailoring tailoring) {
  GUNI_LineBreak lb = word_break_class(cp, tailoring);

  return lb == GUNI_LB_ALPHABETIC || lb == GUNI_LB_IDEOGRAPHIC || lb == GUNI_LB_NUMERIC
      || lb == GUNI_LB_COMPLEX_CONTEXT;
}

bool gfnt_lay_keep_all_pair(uint32_t left, uint32_t right, GUNI_LineBreakTailoring tailoring) {
  return (guni_script(left) == GUNI_SCRIPT_HANGUL && guni_script(right) == GUNI_SCRIPT_HANGUL)
      || ((word_break_class(left, tailoring) == GUNI_LB_ALPHABETIC
              || word_break_class(left, tailoring) == GUNI_LB_IDEOGRAPHIC
              || word_break_class(left, tailoring) == GUNI_LB_NUMERIC)
          && (word_break_class(right, tailoring) == GUNI_LB_ALPHABETIC
              || word_break_class(right, tailoring) == GUNI_LB_IDEOGRAPHIC
              || word_break_class(right, tailoring) == GUNI_LB_NUMERIC));
}

bool gfnt_lay_hangs(uint32_t cp) {
  if (cp == 0x20 || gfnt_lay_is_newline(cp)) {
    return true;
  }
  if (cp == 0xA0 || cp == 0x202F || cp == 0x2007) {
    return false;
  }
  return guni_general_category(cp) == GUNI_GC_ZS;
}

/**
 * Whether a character is one that justification opens gaps beside as it does between
 * ideographs. The set is what Chromium 154 does, measured block by block on a font made for the
 * purpose (css_layout_diff.py): the CJK radicals, symbols and punctuation, kana, bopomofo,
 * enclosed and compatibility forms, the ideographs and their extensions, the vertical and
 * small-form variants and the full- and half-width forms are in; Hangul, Yi, the katakana
 * phonetic extensions, and every other script are not.
 */
bool gfnt_lay_is_ideographic(uint32_t cp) {
  return (cp >= 0x2E80 && cp <= 0x2FFF)
      || (cp >= 0x3001 && cp <= 0x30FF)    // U+3000 is a space
      || (cp >= 0x3100 && cp <= 0x312F)
      || (cp >= 0x3190 && cp <= 0x319F)
      || (cp >= 0x3200 && cp <= 0x4DBF)
      || (cp >= 0x4E00 && cp <= 0x9FFF)
      || (cp >= 0xF900 && cp <= 0xFAFF)
      || (cp >= 0xFE10 && cp <= 0xFE1F)
      || (cp >= 0xFE30 && cp <= 0xFE6F)
      || (cp >= 0xFF01 && cp <= 0xFF9F)
      || (cp >= 0xFFE0 && cp <= 0xFFEE)
      || (cp >= 0x1B000 && cp <= 0x1B16F)
      || (cp >= 0x1F200 && cp <= 0x1F2FF)
      || (cp >= 0x20000 && cp <= 0x3FFFF);
}

/** CSS Text's ideograph class for text-autospace (narrower than justification's measured set). */
static bool autospace_ideograph(uint32_t cp) {
  return (cp >= 0x3041 && cp <= 0x30FF
          && !(guni_general_category_mask(cp) & GUNI_GC_MASK_P))
      || (cp >= 0x31C0 && cp <= 0x31FF) || guni_script(cp) == GUNI_SCRIPT_HAN;
}

uint32_t gfnt_lay_autospace_base_cp(const GFNT_Paragraph * p, const Unit * unit) {
  for (size_t i = unit->cp_start; i < unit->cp_end; i++) {
    if (!(guni_general_category_mask(p->cps[i]) & GUNI_GC_MASK_M)) return p->cps[i];
  }
  return p->cps[unit->cp_start];
}

static bool autospace_non_ideographic_letter(const GFNT_Paragraph * p, const Unit * unit,
    uint32_t cp) {
  uint32_t category = guni_general_category_mask(cp);
  GUNI_EastAsianWidth width = guni_east_asian_width(cp);

  if (autospace_ideograph(cp) || width == GUNI_EAW_WIDE || width == GUNI_EAW_FULLWIDTH
      || (p->vertical && p->runs[unit->run].orient == GFNT_ORIENT_RUN_UPRIGHT)) {
    return false;
  }
  return (category & (GUNI_GC_MASK_L | GUNI_GC_MASK_M)) != 0;
}

static bool autospace_non_ideographic_number(const GFNT_Paragraph * p, const Unit * unit,
    uint32_t cp) {
  GUNI_EastAsianWidth width = guni_east_asian_width(cp);

  return guni_general_category(cp) == GUNI_GC_DECIMAL_NUMBER
      && width != GUNI_EAW_FULLWIDTH
      && !(p->vertical && p->runs[unit->run].orient == GFNT_ORIENT_RUN_UPRIGHT);
}

/** Return the CSS 0.125ic advance at an ideograph/non-ideograph boundary. */
GFNT_F26Dot6 gfnt_lay_autospace_boundary(const GFNT_Paragraph * p,
    const Unit * left, const Unit * right) {
  bool left_ideo, right_ideo, left_alpha, right_alpha, left_number, right_number;
  const Unit * ideograph;
  const GFNT_TextStyle * style;
  int64_t ic, gap;

  if (p->options.text_autospace == GFNT_TEXT_AUTOSPACE_NONE || left->hard || right->hard
      || left->object || right->object || left->space || right->space
      || left->cp_end != right->cp_start) {
    return 0;
  }
  uint32_t left_cp = gfnt_lay_autospace_base_cp(p, left), right_cp = gfnt_lay_autospace_base_cp(p, right);

  left_ideo = autospace_ideograph(left_cp);
  right_ideo = autospace_ideograph(right_cp);
  left_alpha = autospace_non_ideographic_letter(p, left, left_cp);
  right_alpha = autospace_non_ideographic_letter(p, right, right_cp);
  left_number = autospace_non_ideographic_number(p, left, left_cp);
  right_number = autospace_non_ideographic_number(p, right, right_cp);
  if (p->options.text_autospace == GFNT_TEXT_AUTOSPACE_ALPHA) {
    if (!((left_ideo && right_alpha) || (left_alpha && right_ideo))) return 0;
  }
  else if (p->options.text_autospace == GFNT_TEXT_AUTOSPACE_NUMERIC) {
    if (!((left_ideo && right_number) || (left_number && right_ideo))) return 0;
  }
  else if (!((left_ideo && (right_alpha || right_number))
          || ((left_alpha || left_number) && right_ideo))) {
    return 0;
  }
  ideograph = left_ideo ? left : right;
  style = &p->styles[p->runs[ideograph->run].span];
  ic = ideograph->base_width - style->letter_spacing;
  if (ic <= 0) ic = style->size;
  gap = (ic + 4) / 8;
  return gap > INT32_MAX ? INT32_MAX : (GFNT_F26Dot6)gap;
}

#define TEXT_SPACING_GC_OPEN (GUNI_GC_MASK(GUNI_GC_INITIAL_PUNCTUATION) \
    | GUNI_GC_MASK(GUNI_GC_OPEN_PUNCTUATION))
#define TEXT_SPACING_GC_CLOSE (GUNI_GC_MASK(GUNI_GC_FINAL_PUNCTUATION) \
    | GUNI_GC_MASK(GUNI_GC_CLOSE_PUNCTUATION))

static uint32_t text_spacing_cp(const GFNT_Paragraph * p, const Unit * unit) {
  return gfnt_lay_autospace_base_cp(p, unit);
}

bool gfnt_lay_text_spacing_ideograph(uint32_t cp) {
  uint32_t category = guni_general_category_mask(cp);
  GUNI_EastAsianWidth width = guni_east_asian_width(cp);

  return guni_script(cp) == GUNI_SCRIPT_HAN
      && !(category & GUNI_GC_MASK_P)
      && (width == GUNI_EAW_WIDE || width == GUNI_EAW_FULLWIDTH);
}

TextSpacingPunctuation gfnt_lay_text_spacing_punctuation(const GFNT_Paragraph * p,
    const Unit * unit) {
  uint32_t cp = text_spacing_cp(p, unit);
  uint32_t category = guni_general_category_mask(cp);
  bool in_cjk_block = cp >= 0x3000 && cp <= 0x303F;
  bool fullwidth = in_cjk_block || guni_east_asian_width(cp) == GUNI_EAW_FULLWIDTH;

  if (cp == 0x00B7 || cp == 0x2027 || cp == 0x30FB) return TEXT_SPACING_MIDDLE;
  if ((cp == 0x2018 || cp == 0x201C)
      || (fullwidth && (category & TEXT_SPACING_GC_OPEN))) {
    return TEXT_SPACING_OPEN;
  }
  if ((cp == 0x2019 || cp == 0x201D)
      || (fullwidth && (category & TEXT_SPACING_GC_CLOSE))) {
    return TEXT_SPACING_CLOSE;
  }
  /* CSS Text permits language conventions and writing mode to classify these.
   * Japanese convention treats fullwidth colon/semicolon as middle dots and
   * ideographic/fullwidth comma and full stop as closing punctuation. */
  if (cp == 0xFF1A || cp == 0xFF1B) {
    const GFNT_TextStyle * style = &p->styles[p->runs[unit->run].span];
    if (style->content_language == GFNT_CONTENT_LANGUAGE_JAPANESE) {
      return TEXT_SPACING_MIDDLE;
    }
    return TEXT_SPACING_CLOSE;
  }
  if (cp == 0x3001 || cp == 0x3002 || cp == 0xFF0C || cp == 0xFF0E) {
    return TEXT_SPACING_CLOSE;
  }
  return TEXT_SPACING_NONE;
}

/** Half an ideographic advance, only for punctuation that is actually full-width in this face. */
int64_t gfnt_lay_text_spacing_half_ic(const GFNT_Paragraph * p, size_t index) {
  const Unit * unit = &p->units[index];
  const PRun * run = &p->runs[unit->run];
  const GFNT_TextStyle * style = &p->styles[run->span];
  int64_t ic = p->ideographic_advance[run->span] > 0
      ? p->ideographic_advance[run->span] : style->size;

  if (unit->object || unit->tab || gfnt_lay_text_spacing_punctuation(p, unit) == TEXT_SPACING_NONE) {
    return 0;
  }
  if (ic <= 0) return 0;
  /* CSS leaves proportional punctuation alone. A narrow punctuation advance therefore does not
   * acquire a synthetic half-width trim merely because its code point is fullwidth. */
  int64_t delta = (int64_t)unit->base_width - style->letter_spacing - ic;
  if (delta < 0) delta = -delta;
  if (delta * 8 > ic) return 0;
  return ic / 2;
}

static bool text_spacing_is_space(const Unit * unit, const GFNT_Paragraph * p) {
  return unit->cp_end == unit->cp_start + 1 && p->cps[unit->cp_start] == 0x3000;
}

bool gfnt_lay_text_spacing_open_at_start(const GFNT_Paragraph * p, size_t start) {
  if (start >= p->unit_count
      || gfnt_lay_text_spacing_punctuation(p, &p->units[start]) != TEXT_SPACING_OPEN) return false;
  if (p->options.text_spacing_trim == GFNT_TEXT_SPACING_TRIM_TRIM_START) return true;
  if (p->options.text_spacing_trim != GFNT_TEXT_SPACING_TRIM_SPACE_FIRST) return false;
  return start > 0 && !p->units[start - 1].hard;
}

/** Whether an opening mark collapses against its previous character on this line. */
bool gfnt_lay_text_spacing_trim_open_pair(const GFNT_Paragraph * p, size_t left, size_t right) {
  const Unit * before = &p->units[left], *open = &p->units[right];
  TextSpacingPunctuation previous = gfnt_lay_text_spacing_punctuation(p, before);
  TextSpacingPunctuation current = gfnt_lay_text_spacing_punctuation(p, open);
  const GFNT_TextStyle * before_style = &p->styles[p->runs[before->run].span];
  const GFNT_TextStyle * open_style = &p->styles[p->runs[open->run].span];

  if (p->options.text_spacing_trim == GFNT_TEXT_SPACING_TRIM_SPACE_ALL || before->hard) {
    return false;
  }
  return current == TEXT_SPACING_OPEN
      && (previous == TEXT_SPACING_OPEN || previous == TEXT_SPACING_MIDDLE
          || text_spacing_is_space(before, p)
          || (previous == TEXT_SPACING_CLOSE && before_style->size >= open_style->size)
          || (guni_general_category_mask(text_spacing_cp(p, before))
              & TEXT_SPACING_GC_OPEN));
}

/** Whether a closing mark collapses against its following character on this line. */
bool gfnt_lay_text_spacing_trim_close_pair(const GFNT_Paragraph * p, size_t left, size_t right) {
  const Unit * close = &p->units[left], *after = &p->units[right];
  TextSpacingPunctuation previous = gfnt_lay_text_spacing_punctuation(p, close);
  TextSpacingPunctuation current = gfnt_lay_text_spacing_punctuation(p, after);
  const GFNT_TextStyle * close_style = &p->styles[p->runs[close->run].span];
  const GFNT_TextStyle * after_style = &p->styles[p->runs[after->run].span];

  if (p->options.text_spacing_trim == GFNT_TEXT_SPACING_TRIM_SPACE_ALL || close->hard) {
    return false;
  }
  return previous == TEXT_SPACING_CLOSE
      && (current == TEXT_SPACING_CLOSE || current == TEXT_SPACING_MIDDLE
          || text_spacing_is_space(after, p)
          || (current == TEXT_SPACING_OPEN && after_style->size > close_style->size)
          || (guni_general_category_mask(text_spacing_cp(p, after))
              & TEXT_SPACING_GC_CLOSE));
}
