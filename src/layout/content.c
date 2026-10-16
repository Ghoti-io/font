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
 * Min-content and max-content widths.
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

/* ------------------------------------------------------------ content widths */

GFNT_Result gfnt_paragraph_min_content(const GFNT_Paragraph * p,
    GFNT_F26Dot6 * out) {
  int64_t total = 0, fit = 0, widest = 0;
  bool line_start = true;

  if (!p || !out) {
    return GFNT_ERR_INVALID;
  }
  for (size_t j = 0; j < p->unit_count; j++) {
    const Unit * u = &p->units[j];
    int64_t trim_current = 0, trim_previous = 0;
    int64_t shown;

    if (line_start && gfnt_lay_text_spacing_open_at_start(p, j)) {
      trim_current = gfnt_lay_text_spacing_half_ic(p, j);
    }
    else if (j && !p->units[j - 1].hard) {
      if (gfnt_lay_text_spacing_trim_open_pair(p, j - 1, j)) {
        trim_current = gfnt_lay_text_spacing_half_ic(p, j);
      }
      if (gfnt_lay_text_spacing_trim_close_pair(p, j - 1, j)) {
        trim_previous = gfnt_lay_text_spacing_half_ic(p, j - 1);
      }
    }
    if (j && !p->units[j - 1].hard && !p->units[j - 1].opportunity) {
      total += p->units[j - 1].autospace_after;
    }
    if (trim_previous) {
      total -= trim_previous;
      fit -= trim_previous;
    }
    int64_t unit_width = gfnt_lay_eff_width(p, u, total);
    if (trim_current > unit_width) trim_current = unit_width;
    total += unit_width - trim_current;
    if (!u->hard && (!u->space || p->options.white_space == GFNT_WHITE_SPACE_PRE
            || p->options.white_space == GFNT_WHITE_SPACE_BREAK_SPACES)) {
      fit = total;
    }
    if (total > INT32_MAX || total < INT32_MIN || fit > INT32_MAX || fit < INT32_MIN) {
      return GFNT_ERR_INVALID;
    }
    // A stretch that ends at a hyphenation point ends in the hyphen; the end of the text is not one.
    shown = fit + (u->hyphen && u->opportunity && j + 1 < p->unit_count
                       ? p->hyphen_width[p->runs[u->run].span] : 0);
    if (!u->hard && u->opportunity && p->options.text_spacing_trim
            != GFNT_TEXT_SPACING_TRIM_SPACE_ALL
        && gfnt_lay_text_spacing_punctuation(p, u) == TEXT_SPACING_CLOSE) {
      int64_t trim_end = gfnt_lay_text_spacing_half_ic(p, j);
      if (trim_end && shown >= trim_end) shown -= trim_end;
    }
    if (shown > INT32_MAX || shown < INT32_MIN) return GFNT_ERR_INVALID;
    if (shown > widest) {
      widest = shown;
    }
    if (u->hard || (p->options.white_space != GFNT_WHITE_SPACE_PRE
            && (u->opportunity
                || ((p->options.overflow_wrap == GFNT_OVERFLOW_WRAP_ANYWHERE
                     || p->options.word_break == GFNT_WORD_BREAK_BREAK_WORD)
                    && u->emergency)))) {
      total = fit = 0;
      line_start = true;
    }
    else line_start = false;
  }
  {
    // A tab at the start of a stretch is a whole tab stop wide there and only the way to the next
    // stop in the unbroken line, so the stretches can add up to more than the line: a width at
    // which the line does not wrap needs no more than the line.
    GFNT_F26Dot6 max = 0;

    if (gfnt_paragraph_max_content(p, &max) != GFNT_OK) return GFNT_ERR_INVALID;
    if (widest > max) {
      widest = max;
    }
  }
  *out = widest;
  return GFNT_OK;
}

GFNT_Result gfnt_paragraph_max_content(const GFNT_Paragraph * p,
    GFNT_F26Dot6 * out) {
  int64_t total = 0, fit = 0, widest = 0;
  bool line_start = true;

  if (!p || !out) {
    return GFNT_ERR_INVALID;
  }
  for (size_t j = 0; j < p->unit_count; j++) {
    const Unit * u = &p->units[j];
    int64_t trim_current = 0, trim_previous = 0;

    if (line_start && gfnt_lay_text_spacing_open_at_start(p, j)) {
      trim_current = gfnt_lay_text_spacing_half_ic(p, j);
    }
    else if (j && !p->units[j - 1].hard) {
      if (gfnt_lay_text_spacing_trim_open_pair(p, j - 1, j)) {
        trim_current = gfnt_lay_text_spacing_half_ic(p, j);
      }
      if (gfnt_lay_text_spacing_trim_close_pair(p, j - 1, j)) {
        trim_previous = gfnt_lay_text_spacing_half_ic(p, j - 1);
      }
    }
    if (j && !p->units[j - 1].hard) total += p->units[j - 1].autospace_after;
    if (trim_previous) {
      total -= trim_previous;
      fit -= trim_previous;
    }
    int64_t unit_width = gfnt_lay_eff_width(p, u, total);
    if (trim_current > unit_width) trim_current = unit_width;
    total += unit_width - trim_current;
    if (!u->hard && (!u->space || p->options.white_space == GFNT_WHITE_SPACE_PRE
            || p->options.white_space == GFNT_WHITE_SPACE_BREAK_SPACES)) {
      fit = total;
    }
    if (total > INT32_MAX || total < INT32_MIN || fit > INT32_MAX || fit < INT32_MIN) {
      return GFNT_ERR_INVALID;
    }
    if (fit > widest) {
      widest = fit;
    }
    if (u->hard) {
      total = fit = 0;
      line_start = true;
    }
    else line_start = false;
  }
  *out = (GFNT_F26Dot6)widest;
  return GFNT_OK;
}
