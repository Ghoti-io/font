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
 * Line breaking: greedy, balanced and the last-line adjustment.
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

static size_t break_lines(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    size_t * starts, bool * hyphened, size_t * cand_at, GFNT_F26Dot6 * cand_fit,
    GFNT_F26Dot6 * band_starts, GFNT_F26Dot6 * band_widths);



/** The width of a unit at @p pos from @p origin: a tab goes to the next stop. */
int64_t gfnt_lay_eff_width_at(const GFNT_Paragraph * p, const Unit * u,
    int64_t pos, int64_t origin) {
  if (!u->tab || p->tab_width <= 0) {
    return u->width;
  }
  int64_t remainder = (pos + origin) % p->tab_width;
  if (remainder < 0) remainder += p->tab_width;
  return p->tab_width - remainder + u->ruby_added;
}

/** The width of a unit measured from the line start (the legacy tab origin). */
int64_t gfnt_lay_eff_width(const GFNT_Paragraph * p, const Unit * u, int64_t pos) {
  return gfnt_lay_eff_width_at(p, u, pos, 0);
}

int32_t gfnt_lay_phrase_score_at(const GFNT_Paragraph * p, size_t line_start,
    size_t boundary, size_t fit_end, GFNT_F26Dot6 line_width) {
  const Unit * first, * left;
  const GFNT_TextStyle * line_style, * left_style, * right_style;

  if (p->options.word_break != GFNT_WORD_BREAK_AUTO_PHRASE
      || line_start >= boundary || boundary >= p->unit_count) return 0;
  first = &p->units[line_start];
  left = &p->units[boundary - 1];
  line_style = &p->styles[p->runs[first->run].span];
  left_style = &p->styles[p->runs[left->run].span];
  if (line_style->content_language != GFNT_CONTENT_LANGUAGE_JAPANESE
      || left_style->content_language != GFNT_CONTENT_LANGUAGE_JAPANESE) return 0;
  if (boundary < p->unit_count) {
    const Unit * right = &p->units[boundary];
    right_style = &p->styles[p->runs[right->run].span];
    if (right_style->content_language != GFNT_CONTENT_LANGUAGE_JAPANESE) return 0;
  }
  return gfnt_phrase_japanese_boundary_score(p->cps, p->length,
      first->cp_start, left->cp_end,
      fit_end == p->unit_count ? p->length : p->units[fit_end - 1].cp_end,
      line_width);
}

BreakResult gfnt_lay_break_one_line(const GFNT_Paragraph * p, size_t start,
    GFNT_F26Dot6 width, GFNT_F26Dot6 tab_origin, size_t * cand_at,
    GFNT_F26Dot6 * cand_fit) {
  BreakResult result = {p->unit_count, false};
  size_t i = start;
  GFNT_F26Dot6 total = 0, fit = 0;
  size_t emergency_at = 0;
  GFNT_F26Dot6 emergency_fit = 0;
  size_t cands = 0;
  bool have_emergency = false;
  bool bounded = width != GFNT_LAYOUT_UNBOUNDED;
  bool can_wrap = p->options.white_space != GFNT_WHITE_SPACE_PRE
      && p->options.white_space != GFNT_WHITE_SPACE_NOWRAP;

  for (size_t j = i; j < p->unit_count; j++) {
    const Unit * u = &p->units[j];
    int64_t trim_current = 0, trim_previous = 0;

    if (j > start) total += p->units[j - 1].autospace_after;
    if (j == start && gfnt_lay_text_spacing_open_at_start(p, start)) {
      trim_current = gfnt_lay_text_spacing_half_ic(p, j);
    }
    else if (j > start) {
      if (gfnt_lay_text_spacing_trim_open_pair(p, j - 1, j)) {
        trim_current = gfnt_lay_text_spacing_half_ic(p, j);
      }
      if (gfnt_lay_text_spacing_trim_close_pair(p, j - 1, j)) {
        trim_previous = gfnt_lay_text_spacing_half_ic(p, j - 1);
      }
    }
    if (trim_previous) {
      total -= trim_previous;
      fit -= trim_previous;
      for (size_t k = cands; k > 0; k--) {
        if (cand_at[k - 1] == j - 1) cand_fit[k - 1] -= trim_previous;
      }
      if (have_emergency && emergency_at == j - 1) emergency_fit -= trim_previous;
    }
    int64_t unit_width = gfnt_lay_eff_width_at(p, u, total, tab_origin);
    if (trim_current > unit_width) trim_current = unit_width;
    total += unit_width - trim_current;
    total -= gfnt_lay_ruby_right_refund(p, start, j, NULL, NULL);
    if (!u->space || p->options.white_space == GFNT_WHITE_SPACE_BREAK_SPACES) {
      fit = total;
    }
    if (u->hard) {
      result.end = j + 1;
      break;
    }
    if (can_wrap && bounded && u->emergency && j + 1 < p->unit_count && fit <= width) {
      emergency_at = j;
      emergency_fit = fit;
      have_emergency = true;
    }
    if (can_wrap && bounded && u->opportunity
        && fit > width && p->options.text_spacing_trim != GFNT_TEXT_SPACING_TRIM_SPACE_ALL
        && gfnt_lay_text_spacing_punctuation(p, u) == TEXT_SPACING_CLOSE) {
      int64_t trim_end = gfnt_lay_text_spacing_half_ic(p, j);
      if (trim_end && fit - trim_end <= width) {
        result.end = j + 1;
        break;
      }
    }
    if (can_wrap && bounded && fit > width && cands) {
      size_t k = cands;

      while (k > 0) {
        const Unit * c = &p->units[cand_at[k - 1]];

        if (!c->hyphen
            || cand_fit[k - 1] + p->hyphen_width[p->runs[c->run].span] <= width) {
          break;
        }
        k--;
      }
      if (!k && have_emergency && emergency_at < j && emergency_fit <= width) {
        result.end = emergency_at + 1;
        break;
      }
      if (k && p->options.word_break == GFNT_WORD_BREAK_AUTO_PHRASE) {
        size_t preferred = SIZE_MAX;
        size_t fit_end = cand_at[k - 1] + 1;
        int32_t best_score = 0;

        for (size_t c = 0; c < k; c++) {
          int32_t score = gfnt_lay_phrase_score_at(p, start, cand_at[c] + 1, fit_end, width);
          if (score > best_score) {
            best_score = score;
            preferred = c;
          }
        }
        if (preferred != SIZE_MAX) k = preferred + 1;
      }
      k = k ? k - 1 : 0;
      result.end = cand_at[k] + 1;
      result.hyphen = p->units[cand_at[k]].hyphen;
      break;
    }
    if (can_wrap && bounded && fit > width && !cands && have_emergency && emergency_at < j) {
      result.end = emergency_at + 1;
      break;
    }
    if (can_wrap && u->opportunity && j + 1 < p->unit_count) {
      if (bounded && fit > width) {
        result.end = j + 1;
        result.hyphen = u->hyphen;
        break;
      }
      cand_at[cands] = j;
      cand_fit[cands++] = fit;
    }
  }
  return result;
}

/** Apply the paragraph's first-line or hanging indent to a callback band. */
bool gfnt_lay_indent_band(const GFNT_Paragraph * p, size_t line, GFNT_Band * band) {
  const bool apply = p->options.hanging_indent ? line > 0 : line == 0;
  if (!apply || !p->options.text_indent) return true;
  int64_t start = (int64_t)band->start + p->options.text_indent;
  int64_t width = (int64_t)band->width - p->options.text_indent;
  if (band->width == GFNT_LAYOUT_UNBOUNDED && p->options.text_indent < 0) {
    width = GFNT_LAYOUT_UNBOUNDED;
  }
  if (width < 0) width = 0;
  if (start < INT32_MIN || start > INT32_MAX || width > INT32_MAX
      || start + width > INT32_MAX) return false;
  band->start = (GFNT_F26Dot6)start;
  band->width = (GFNT_F26Dot6)width;
  return true;
}



/** Greedy line count for one forced-break group at a candidate measure. */
static size_t count_group_lines(const GFNT_Paragraph * p, size_t start, size_t end,
    size_t first_line, GFNT_F26Dot6 width, size_t * starts, bool * hyphened,
    GFNT_F26Dot6 * band_starts, GFNT_F26Dot6 * band_widths,
    size_t * cand_at, GFNT_F26Dot6 * cand_fit) {
  size_t lines = 0;

  while (start < end) {
    GFNT_Band band = {0, width, 0};
    GFNT_F26Dot6 tab_origin;
    BreakResult result;

    if (!gfnt_lay_indent_band(p, first_line + lines, &band)) band.width = width;
    tab_origin = p->options.tabs_from_container ? band.start : 0;
    result = gfnt_lay_break_one_line(p, start, band.width, tab_origin, cand_at, cand_fit);
    if (result.end <= start || result.end > end) return SIZE_MAX;
    starts[first_line + lines] = start;
    hyphened[first_line + lines] = result.hyphen;
    band_starts[first_line + lines] = band.start;
    band_widths[first_line + lines] = band.width;
    start = result.end;
    lines++;
  }
  return lines;
}

/** Width of a candidate line, excluding collapsed hanging spaces and its forced break unit. */
static int64_t balanced_line_width(const GFNT_Paragraph * p, size_t start, size_t end,
    bool hyphenate, int64_t tab_origin, int64_t available) {
  int64_t width = 0;
  int64_t pos = 0;
  size_t content_end = end;

  if (content_end > start && p->units[content_end - 1].hard) content_end--;
  if (p->options.white_space != GFNT_WHITE_SPACE_PRE
      && p->options.white_space != GFNT_WHITE_SPACE_BREAK_SPACES) {
    while (content_end > start && p->units[content_end - 1].space) content_end--;
  }
  for (size_t i = start; i < content_end; i++) {
    int64_t advance;
    if (i > start) {
      const Unit * previous = &p->units[i - 1];
      width += previous->autospace_after;
      pos += previous->autospace_after;
      if (gfnt_lay_text_spacing_trim_close_pair(p, i - 1, i)) {
        int64_t trim = gfnt_lay_text_spacing_half_ic(p, i - 1);
        if (trim > p->units[i - 1].width) trim = p->units[i - 1].width;
        width -= trim;
        pos -= trim;
      }
    }
    if ((i == start && gfnt_lay_text_spacing_open_at_start(p, start))
        || (i > start && gfnt_lay_text_spacing_trim_open_pair(p, i - 1, i))) {
      int64_t trim = gfnt_lay_text_spacing_half_ic(p, i);
      advance = gfnt_lay_eff_width_at(p, &p->units[i], pos, tab_origin);
      if (trim > advance) trim = advance;
      advance -= trim;
    }
    else {
      advance = gfnt_lay_eff_width_at(p, &p->units[i], pos, tab_origin);
    }
    width += advance;
    pos += advance;
    /* AUTO ruby overhang reclaims advance from the annotated base at this
     * exact boundary; keep the optimizer's score consistent with placement. */
    if (i + 1 < content_end) {
      int64_t refund = gfnt_lay_ruby_right_refund(p, start, i + 1, NULL, NULL);
      if (refund > width) refund = width;
      width -= refund;
      pos -= refund;
    }
  }
  if (hyphenate && content_end > start && p->units[content_end - 1].hyphen) {
    const Unit * unit = &p->units[content_end - 1];
    width += p->hyphen_width[p->runs[unit->run].span];
  }
  /* Match gfnt_lay_measure_line's end punctuation trim when it is needed to fit. */
  if (width > available && content_end > start
      && p->options.text_spacing_trim != GFNT_TEXT_SPACING_TRIM_SPACE_ALL) {
    size_t last = content_end - 1;
    if (gfnt_lay_text_spacing_punctuation(p, &p->units[last]) == TEXT_SPACING_CLOSE) {
      int64_t trim = gfnt_lay_text_spacing_half_ic(p, last);
      if (trim > p->units[last].width) trim = p->units[last].width;
      if (trim > 0 && width - trim <= available) width -= trim;
    }
  }
  return width;
}

/** Fixed-point variance score for a line sequence; smaller means better balance. */
static uint64_t balance_variance(const GFNT_Paragraph * p, size_t first_line,
    size_t line_count, size_t end, const size_t * starts, const bool * hyphened,
    const GFNT_F26Dot6 * band_starts) {
  int64_t widths[10], total = 0, largest = 0;
  uint64_t score = 0;

  for (size_t i = 0; i < line_count; i++) {
    size_t line_end = i + 1 < line_count ? starts[first_line + i + 1] : end;
    widths[i] = balanced_line_width(p, starts[first_line + i], line_end,
        hyphened[first_line + i], p->options.tabs_from_container
            ? band_starts[first_line + i] : 0, INT64_MAX);
    if (widths[i] > largest) largest = widths[i];
  }
  /* Keep the sum of at most ten squared deltas inside uint64_t, even for an
   * unbreakable line whose advance is much larger than the requested width. */
  while (largest > INT64_C(900000000)) {
    largest >>= 1;
    for (size_t i = 0; i < line_count; i++) widths[i] >>= 1;
  }
  for (size_t i = 0; i < line_count; i++) {
    total += widths[i];
  }
  const int64_t average = total / (int64_t)line_count;
  for (size_t i = 0; i < line_count; i++) {
    int64_t delta = widths[i] - average;
    if (delta < 0) delta = -delta;
    score += (uint64_t)(delta * delta);
  }
  return score;
}

/** Reflow the end of a short final line within its forced-break group. */
static void avoid_short_last_line(const GFNT_Paragraph * p, size_t line_count,
    GFNT_F26Dot6 * band_starts, GFNT_F26Dot6 * band_widths,
    size_t * starts, bool * hyphened) {
  size_t group_start = 0, first_line = 0;

  while (group_start < p->unit_count) {
    size_t group_end = group_start;
    size_t after_group = first_line;

    while (group_end < p->unit_count && !p->units[group_end].hard) group_end++;
    if (group_end < p->unit_count) group_end++;
    while (after_group < line_count && starts[after_group] < group_end) after_group++;
    if (after_group - first_line >= 2) {
      const size_t last = after_group - 1;
      const size_t previous = last - 1;
      const size_t last_start = starts[last];
      const size_t previous_start = starts[previous];
      const int64_t last_width = balanced_line_width(p, last_start, group_end,
          hyphened[last], p->options.tabs_from_container ? band_starts[last] : 0,
          band_widths[last]);
      const int64_t available = band_widths[last];
      size_t best_boundary = 0;
      int64_t best_last_width = INT64_MAX;

      /* One third of the line measure is Ghoti's documented short-line threshold. */
      if (last_start > previous_start && last_width < available / 3) {
        /* Only whitespace opportunities delimit words; character and hyphen breaks do not. */
        for (size_t boundary = last_start; boundary > previous_start + 1;) {
          int64_t before_width, after_width;
          boundary--;
          if (!p->units[boundary - 1].opportunity || !p->units[boundary - 1].space) continue;
          before_width = balanced_line_width(p, previous_start, boundary,
              p->units[boundary - 1].hyphen,
              p->options.tabs_from_container ? band_starts[previous] : 0,
              band_widths[previous]);
          after_width = balanced_line_width(p, boundary, group_end, false,
              p->options.tabs_from_container ? band_starts[last] : 0, available);
          if (before_width <= band_widths[previous] && after_width <= available
              && after_width >= available / 3 && after_width > last_width
              && after_width < best_last_width) {
            best_boundary = boundary;
            best_last_width = after_width;
          }
        }
      }
      if (best_boundary) {
        starts[last] = best_boundary;
        hyphened[previous] = p->units[best_boundary - 1].hyphen;
        hyphened[last] = false;
      }
    }
    group_start = group_end;
    first_line = after_group;
  }
}

/** Balance short forced-break groups by selecting soft-wrap points near their average line width. */
size_t gfnt_lay_break_lines_balanced(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    size_t * starts, bool * hyphened, size_t * cand_at, GFNT_F26Dot6 * cand_fit,
    GFNT_F26Dot6 * band_starts, GFNT_F26Dot6 * band_widths) {
  size_t lines = 0, group_start = 0;

  if (p->options.text_wrap_style == GFNT_TEXT_WRAP_AVOID_SHORT_LAST_LINE) {
    lines = break_lines(p, width, starts, hyphened, cand_at, cand_fit,
        band_starts, band_widths);
    if (width != GFNT_LAYOUT_UNBOUNDED && lines > 1) {
      avoid_short_last_line(p, lines, band_starts, band_widths, starts, hyphened);
    }
    return lines;
  }
  if (width == GFNT_LAYOUT_UNBOUNDED || p->options.text_wrap_style == GFNT_TEXT_WRAP_AUTO
      || p->options.text_wrap_style == GFNT_TEXT_WRAP_STABLE) {
    return break_lines(p, width, starts, hyphened, cand_at, cand_fit,
        band_starts, band_widths);
  }
  if (!p->unit_count) return break_lines(p, width, starts, hyphened, cand_at, cand_fit,
      band_starts, band_widths);
  while (group_start < p->unit_count) {
    size_t group_end = group_start;
    size_t auto_lines, point_count = 0, group_first_line = lines;
    bool balanced = false;
    while (group_end < p->unit_count && !p->units[group_end].hard) group_end++;
    if (group_end < p->unit_count) group_end++;
    auto_lines = count_group_lines(p, group_start, group_end, lines, width, starts,
        hyphened, band_starts, band_widths, cand_at, cand_fit);
    if (auto_lines > 1 && auto_lines <= 10 && auto_lines != SIZE_MAX) {
      const uint64_t auto_variance = balance_variance(p, group_first_line, auto_lines,
          group_end, starts, hyphened, band_starts);
      if (p->options.word_break == GFNT_WORD_BREAK_AUTO_PHRASE) {
        for (size_t i = group_start; i + 1 < group_end; i++) {
          if (p->units[i].opportunity) cand_at[point_count++] = i + 1;
        }
      }
      if (!point_count) {
        for (size_t i = group_start; i + 1 < group_end; i++) {
          if (p->units[i].opportunity) cand_at[point_count++] = i + 1;
        }
      }
      cand_at[point_count++] = group_end;
      if (point_count >= auto_lines) {
        size_t current = group_start, next_point = 0;
        bool feasible = true;
        for (size_t line = 0; line + 1 < auto_lines; line++) {
          const size_t lines_left = auto_lines - line;
          const size_t max_point = point_count - lines_left;
          GFNT_Band band = {0, width, 0};
          int64_t remaining, target;
          int64_t best_difference = INT64_MAX;
          size_t best_point = SIZE_MAX, best_end = 0;

          if (!gfnt_lay_indent_band(p, lines + line, &band)) band.width = width;
          remaining = balanced_line_width(p, current, group_end, false,
              p->options.tabs_from_container ? band.start : 0, INT64_MAX);
          target = remaining / (int64_t)lines_left;
          for (size_t point = next_point; point <= max_point; point++) {
            size_t end = cand_at[point];
            int64_t candidate_width = balanced_line_width(p, current, end, end < group_end,
                p->options.tabs_from_container ? band.start : 0, band.width);
            int64_t difference = candidate_width - target;
            bool has_preferred = false;
            if (p->options.word_break == GFNT_WORD_BREAK_AUTO_PHRASE
                && end < group_end) {
              for (size_t candidate = next_point; candidate <= max_point; candidate++) {
                size_t candidate_end = cand_at[candidate];
                if (candidate_end >= end) break;
                if (gfnt_lay_phrase_score_at(p, current, candidate_end, end, band.width) > 0
                    && balanced_line_width(p, current, candidate_end, true,
                        p->options.tabs_from_container ? band.start : 0, band.width)
                        <= band.width) {
                  has_preferred = true;
                }
              }
            }
            if (has_preferred && gfnt_lay_phrase_score_at(p, current, end, end, band.width) <= 0) continue;
            if (difference < 0) difference = -difference;
            if (candidate_width <= band.width && difference < best_difference) {
              best_difference = difference;
              best_point = point;
              best_end = end;
            }
          }
          if (best_point == SIZE_MAX) {
            feasible = false;
            break;
          }
          starts[lines] = current;
          band_starts[lines] = band.start;
          band_widths[lines] = band.width;
          hyphened[lines] = best_end < group_end
              && p->units[best_end - 1].hyphen;
          lines++;
          current = best_end;
          next_point = best_point + 1;
        }
        if (feasible) {
          GFNT_Band band = {0, width, 0};
          if (!gfnt_lay_indent_band(p, lines, &band)) band.width = width;
          int64_t last_width = balanced_line_width(p, current, group_end, false,
              p->options.tabs_from_container ? band.start : 0, band.width);
          if (last_width <= band.width) {
            starts[lines] = current;
            band_starts[lines] = band.start;
            band_widths[lines] = band.width;
            hyphened[lines] = false;
            lines++;
            if (balance_variance(p, group_first_line, auto_lines, group_end,
                    starts, hyphened, band_starts) < auto_variance) {
              group_start = group_end;
              balanced = true;
            }
          }
        }
      }
    }
    if (!balanced) {
      lines = group_first_line;
      while (group_start < group_end) {
        GFNT_Band band = {0, width, 0};
        BreakResult result;

        if (!gfnt_lay_indent_band(p, lines, &band)) band.width = width;
        starts[lines] = group_start;
        band_starts[lines] = band.start;
        band_widths[lines] = band.width;
        result = gfnt_lay_break_one_line(p, group_start, band.width,
            p->options.tabs_from_container ? band.start : 0, cand_at, cand_fit);
        if (result.end <= group_start || result.end > group_end) {
          result.end = group_end;
          result.hyphen = false;
        }
        hyphened[lines++] = result.hyphen;
        group_start = result.end;
      }
    }
  }
  if (p->units[p->unit_count - 1].hard) {
    GFNT_Band band = {0, width, 0};
    (void)gfnt_lay_indent_band(p, lines, &band);
    band_starts[lines] = band.start;
    band_widths[lines] = band.width;
    hyphened[lines] = false;
    starts[lines++] = p->unit_count;
  }
  return lines;
}

static size_t break_lines(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    size_t * starts, bool * hyphened, size_t * cand_at, GFNT_F26Dot6 * cand_fit,
    GFNT_F26Dot6 * band_starts, GFNT_F26Dot6 * band_widths) {
  size_t lines = 0;
  size_t i = 0;

  if (p->unit_count == 0) {
    starts[0] = 0;
    hyphened[0] = false;
    GFNT_Band band = {0, width, 0};
    if (!gfnt_lay_indent_band(p, 0, &band)) return 1;
    band_starts[0] = band.start;
    band_widths[0] = band.width;
    return 1;
  }
  while (i < p->unit_count) {
    BreakResult result;

    starts[lines] = i;
    GFNT_Band band = {0, width, 0};
    if (!gfnt_lay_indent_band(p, lines, &band)) {
      // Creation has no error channel here; an unrepresentable indent is rejected by layout.
      band.width = width;
    }
    band_starts[lines] = band.start;
    band_widths[lines] = band.width;
    GFNT_F26Dot6 tab_origin = p->options.tabs_from_container ? band.start : 0;
    result = gfnt_lay_break_one_line(p, i, band.width, tab_origin, cand_at, cand_fit);
    hyphened[lines++] = result.hyphen;
    i = result.end;
  }
  // A text ending in a newline has an empty last line.
  if (p->units[p->unit_count - 1].hard) {
    GFNT_Band band = {0, width, 0};
    (void)gfnt_lay_indent_band(p, lines, &band);
    band_starts[lines] = band.start;
    band_widths[lines] = band.width;
    hyphened[lines] = false;
    starts[lines++] = p->unit_count;
  }
  return lines;
}
