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
 * The text-wrap optimiser for flow layouts.
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

#define FLOW_WRAP_LINES_MAX 10
#define FLOW_AVOID_CALLBACK_LIMIT 512

typedef struct {
  const GFNT_Paragraph * paragraph;
  const GFNT_LineConstraint * constraint;
  LayoutState * state;
  size_t group_end, first_line, target_lines;
  size_t nodes, node_limit, callback_calls, callback_limit;
  size_t path_starts[FLOW_WRAP_LINES_MAX + 1];
  bool path_hyphened[FLOW_WRAP_LINES_MAX];
  GFNT_F26Dot6 path_starts_x[FLOW_WRAP_LINES_MAX];
  GFNT_F26Dot6 path_widths[FLOW_WRAP_LINES_MAX];
  GFNT_F26Dot6 path_tops[FLOW_WRAP_LINES_MAX];
  int64_t path_advances[FLOW_WRAP_LINES_MAX];
  size_t best_starts[FLOW_WRAP_LINES_MAX + 1];
  bool best_hyphened[FLOW_WRAP_LINES_MAX];
  GFNT_F26Dot6 best_starts_x[FLOW_WRAP_LINES_MAX];
  GFNT_F26Dot6 best_widths[FLOW_WRAP_LINES_MAX];
  GFNT_F26Dot6 best_tops[FLOW_WRAP_LINES_MAX];
  uint64_t best_score;
  GFNT_F26Dot6 best_end_top;
  bool found, exhausted;
} FlowWrapSearch;

static bool flow_band_valid(const GFNT_Band * band) {
  return band->width >= 0 && band->next_top >= 0
      && (int64_t)band->start + band->width <= INT32_MAX;
}

/** Measure a candidate against the callback's actual-height band, retrying changed bands. */
static bool flow_wrap_measure(FlowWrapSearch * search, size_t start, size_t end,
    bool hyphenated, size_t line, GFNT_F26Dot6 top, GFNT_F26Dot6 * out_top,
    GFNT_Band * out_band,
    LayoutLineState * out_measure, int64_t * out_advance) {
  const GFNT_Paragraph * p = search->paragraph;
  GFNT_Band raw = {0}, band = {0};
  GFNT_Result result;
  size_t retry_limit = p->unit_count > (SIZE_MAX - 16) / 2
      ? SIZE_MAX : p->unit_count * 2 + 16;

  if (++search->callback_calls > search->callback_limit) {
    search->exhausted = true;
    return false;
  }
  result = search->constraint->band(search->constraint->ctx, top, 0, &raw);
  if (result != GFNT_OK || !flow_band_valid(&raw)) return false;
  for (size_t retry = 0; retry <= retry_limit; retry++) {
    LayoutLineState measured;
    GFNT_F26Dot6 tab_origin;
    GFNT_Band actual = {0};

    band = raw;
    if (!gfnt_lay_indent_band(p, line, &band)) return false;
    if (!band.width && band.next_top > top) {
      top = band.next_top;
      raw = (GFNT_Band){0};
      if (++search->callback_calls > search->callback_limit) {
        search->exhausted = true;
        return false;
      }
      result = search->constraint->band(search->constraint->ctx, top, 0, &raw);
      if (result != GFNT_OK || !flow_band_valid(&raw)) return false;
      continue;
    }
    tab_origin = p->options.tabs_from_container ? band.start : 0;
    measured = gfnt_lay_measure_line(p, start, end, band.width, top, tab_origin,
        search->state->ew, search->state->tabadv, search->state->ruby_reclaim,
        search->state->spacing_trim);
    if (measured.invalid || measured.thickness < 0
        || top > INT32_MAX - measured.thickness) return false;
    if (++search->callback_calls > search->callback_limit) {
      search->exhausted = true;
      return false;
    }
    result = search->constraint->band(search->constraint->ctx, top,
        measured.thickness, &actual);
    if (result != GFNT_OK || !flow_band_valid(&actual)) return false;
    if (actual.start == raw.start && actual.width == raw.width) {
      band = actual;
      if (!gfnt_lay_indent_band(p, line, &band)) return false;
      /* The tab origin can differ when the callback changes the band. */
      tab_origin = p->options.tabs_from_container ? band.start : 0;
      measured = gfnt_lay_measure_line(p, start, end, band.width, top, tab_origin,
          search->state->ew, search->state->tabadv, search->state->ruby_reclaim,
          search->state->spacing_trim);
      if (measured.invalid || measured.thickness < 0
          || top > INT32_MAX - measured.thickness) return false;
      int64_t advance = measured.natural;
      if (hyphenated && end > start && p->units[end - 1].hyphen) {
        advance += p->hyphen_width[p->runs[p->units[end - 1].run].span];
      }
      if (advance > band.width && band.next_top > top) {
        top = band.next_top;
        if (++search->callback_calls > search->callback_limit) {
          search->exhausted = true;
          return false;
        }
        result = search->constraint->band(search->constraint->ctx, top, 0, &raw);
        if (result != GFNT_OK || !flow_band_valid(&raw)) return false;
        continue;
      }
      if (advance > band.width) {
        for (size_t i = start; i + 1 < end; i++) {
          if (p->units[i].opportunity) return false;
        }
      }
      *out_top = top;
      *out_band = band;
      *out_measure = measured;
      *out_advance = advance;
      return true;
    }
    raw = actual;
  }
  return false;
}

static uint64_t flow_wrap_score(const int64_t * advances, size_t count) {
  int64_t scaled[FLOW_WRAP_LINES_MAX], total = 0, largest = 0;
  uint64_t score = 0;

  for (size_t i = 0; i < count; i++) {
    scaled[i] = advances[i];
    if (scaled[i] > largest) largest = scaled[i];
  }
  while (largest > INT64_C(900000000)) {
    largest >>= 1;
    for (size_t i = 0; i < count; i++) scaled[i] >>= 1;
  }
  for (size_t i = 0; i < count; i++) total += scaled[i];
  const int64_t average = total / (int64_t)count;
  for (size_t i = 0; i < count; i++) {
    int64_t delta = scaled[i] - average;
    if (delta < 0) delta = -delta;
    score += (uint64_t)(delta * delta);
  }
  return score;
}

static void flow_wrap_search_lines(FlowWrapSearch * search, size_t start,
    size_t slot, GFNT_F26Dot6 top) {
  const GFNT_Paragraph * p = search->paragraph;
  const size_t left = search->target_lines - slot;

  if (search->exhausted || ++search->nodes > search->node_limit) {
    search->exhausted = true;
    return;
  }
  if (slot == search->target_lines) {
    if (start != search->group_end) return;
    uint64_t score = flow_wrap_score(search->path_advances, search->target_lines);
    if (!search->found || score < search->best_score) {
      search->found = true;
      search->best_score = score;
      search->best_end_top = top;
      memcpy(search->best_starts, search->path_starts,
          (search->target_lines + 1) * sizeof search->best_starts[0]);
      memcpy(search->best_hyphened, search->path_hyphened,
          search->target_lines * sizeof search->best_hyphened[0]);
      memcpy(search->best_starts_x, search->path_starts_x,
          search->target_lines * sizeof search->best_starts_x[0]);
      memcpy(search->best_widths, search->path_widths,
          search->target_lines * sizeof search->best_widths[0]);
      memcpy(search->best_tops, search->path_tops,
          search->target_lines * sizeof search->best_tops[0]);
    }
    return;
  }
  if (start >= search->group_end || search->group_end - start < left) return;

  const size_t last_end = left == 1 ? search->group_end : search->group_end - (left - 1);
  bool has_preferred = false;
  if (p->options.word_break == GFNT_WORD_BREAK_AUTO_PHRASE && left > 1) {
    for (size_t i = start; i + 1 < search->group_end; i++) {
      if (p->units[i].opportunity
          && gfnt_lay_phrase_score_at(p, start, i + 1, last_end, 0) > 0) {
        has_preferred = true;
        break;
      }
    }
  }
  const size_t first_end = start + 1;
  for (size_t end = first_end; end <= last_end && !search->exhausted; end++) {
    bool hyphenated;
    GFNT_Band band = {0};
    LayoutLineState measured = {0};
    int64_t advance = 0;

    if (left > 1) {
      const Unit * boundary = &p->units[end - 1];
      if (boundary->hard) break;
      if (!boundary->opportunity
          || (has_preferred && gfnt_lay_phrase_score_at(p, start, end, last_end, 0) <= 0)) continue;
    }
    hyphenated = left > 1 && p->units[end - 1].hyphen;
    if (!flow_wrap_measure(search, start, end, hyphenated,
            search->first_line + slot, top, &search->path_tops[slot],
            &band, &measured, &advance)) {
      if (search->exhausted) break;
      continue;
    }
    search->path_starts[slot] = start;
    search->path_hyphened[slot] = hyphenated;
    search->path_starts_x[slot] = band.start;
    search->path_widths[slot] = band.width;
    search->path_advances[slot] = advance;
    search->path_starts[slot + 1] = end;
    if (search->path_tops[slot] > INT32_MAX - measured.thickness) continue;
    flow_wrap_search_lines(search, end, slot + 1,
        (GFNT_F26Dot6)(search->path_tops[slot] + measured.thickness));
  }
}

/** Optimize stable flow groups without changing their greedy line count or forced breaks. */
void gfnt_lay_flow_wrap_optimize(const GFNT_Paragraph * p,
    const GFNT_LineConstraint * constraint, LayoutState * state) {
  const GFNT_Allocator * allocator = p->allocator;
  const size_t count = state->line_count;
  size_t * old_starts = NULL;
  bool * old_hyphened = NULL;
  GFNT_F26Dot6 * old_band_starts = NULL, * old_band_widths = NULL, * old_tops = NULL;
  bool changed = false, failed = false;
  size_t first_line = 0, group_start = 0;

  if ((p->options.text_wrap_style != GFNT_TEXT_WRAP_BALANCE
          && p->options.text_wrap_style != GFNT_TEXT_WRAP_PRETTY)
      || count < 2) return;
  old_starts = allocator->calloc_fn(allocator->ctx, count + 1, sizeof *old_starts);
  old_hyphened = allocator->calloc_fn(allocator->ctx, count, sizeof *old_hyphened);
  old_band_starts = allocator->calloc_fn(allocator->ctx, count, sizeof *old_band_starts);
  old_band_widths = allocator->calloc_fn(allocator->ctx, count, sizeof *old_band_widths);
  old_tops = allocator->calloc_fn(allocator->ctx, count, sizeof *old_tops);
  if (!old_starts || !old_hyphened || !old_band_starts || !old_band_widths || !old_tops) {
    failed = true;
    goto done;
  }
  memcpy(old_starts, state->starts, (count + 1) * sizeof *old_starts);
  memcpy(old_hyphened, state->hyphened, count * sizeof *old_hyphened);
  memcpy(old_band_starts, state->band_starts, count * sizeof *old_band_starts);
  memcpy(old_band_widths, state->band_widths, count * sizeof *old_band_widths);
  memcpy(old_tops, state->line_tops, count * sizeof *old_tops);

  while (group_start < p->unit_count) {
    size_t group_end = group_start, after_group = first_line;
    size_t group_lines;
    GFNT_F26Dot6 group_top;
    FlowWrapSearch search = {0};

    while (group_end < p->unit_count && !p->units[group_end].hard) group_end++;
    if (group_end < p->unit_count) group_end++;
    while (after_group < count && old_starts[after_group] < group_end) after_group++;
    group_lines = after_group - first_line;
    if (!group_lines || group_lines > FLOW_WRAP_LINES_MAX) {
      /* A changed earlier group's height would invalidate these fixed greedy lines. */
      if (changed && after_group < count && state->line_tops[after_group] != old_tops[after_group]) {
        failed = true;
        break;
      }
      group_start = group_end;
      first_line = after_group;
      continue;
    }
    group_top = first_line ? state->line_tops[first_line] : state->line_tops[0];
    search.paragraph = p;
    search.constraint = constraint;
    search.state = state;
    search.group_end = group_end;
    search.first_line = first_line;
    search.target_lines = group_lines;
    search.node_limit = p->unit_count > 308 ? 20000 : p->unit_count * 64 + 256;
    if (search.node_limit > 20000) search.node_limit = 20000;
    search.callback_limit = search.node_limit > SIZE_MAX / 16
        ? SIZE_MAX : search.node_limit * 16;
    int64_t old_advances[FLOW_WRAP_LINES_MAX];
    bool old_measurable = true;
    for (size_t i = 0; i < group_lines; i++) {
      GFNT_Band band = {0};
      LayoutLineState measured = {0};
      GFNT_F26Dot6 measured_old_top = old_tops[first_line + i];
      if (!flow_wrap_measure(&search, old_starts[first_line + i],
              old_starts[first_line + i + 1], old_hyphened[first_line + i],
              first_line + i, old_tops[first_line + i], &measured_old_top,
              &band, &measured, &old_advances[i])) {
        old_measurable = false;
        break;
      }
      if (measured_old_top != old_tops[first_line + i]) {
        old_measurable = false;
        break;
      }
    }
    if (old_measurable) {
      flow_wrap_search_lines(&search, group_start, 0, group_top);
      /* If the bounded search did not finish, keep the stable greedy result. */
      if (search.exhausted) search.found = false;
    }
    if (search.found) {
      bool applied = false;
      uint64_t old_score = flow_wrap_score(old_advances, group_lines);
      if (search.best_score < old_score) {
        for (size_t i = 0; i < group_lines; i++) {
          state->starts[first_line + i] = search.best_starts[i];
          state->hyphened[first_line + i] = search.best_hyphened[i];
          state->band_starts[first_line + i] = search.best_starts_x[i];
          state->band_widths[first_line + i] = search.best_widths[i];
          state->line_tops[first_line + i] = search.best_tops[i];
        }
        state->starts[after_group] = group_end;
        changed = true;
        applied = true;
      }
      if (applied && after_group < count
          && search.best_end_top != old_tops[after_group]) {
        failed = true;
        break;
      }
    }
    else if (changed) {
      failed = true;
      break;
    }
    group_start = group_end;
    first_line = after_group;
  }
  if (failed) {
    memcpy(state->starts, old_starts, (count + 1) * sizeof *old_starts);
    memcpy(state->hyphened, old_hyphened, count * sizeof *old_hyphened);
    memcpy(state->band_starts, old_band_starts, count * sizeof *old_band_starts);
    memcpy(state->band_widths, old_band_widths, count * sizeof *old_band_widths);
    memcpy(state->line_tops, old_tops, count * sizeof *old_tops);
  }
done:
  allocator->free_fn(allocator->ctx, old_starts);
  allocator->free_fn(allocator->ctx, old_hyphened);
  allocator->free_fn(allocator->ctx, old_band_starts);
  allocator->free_fn(allocator->ctx, old_band_widths);
  allocator->free_fn(allocator->ctx, old_tops);
}

/** Apply the whitespace-only last-line move using the callback band at each candidate height. */
void gfnt_lay_flow_avoid_short_last_line(const GFNT_Paragraph * p,
    const GFNT_LineConstraint * constraint, LayoutState * state) {
  size_t group_start = 0, first_line = 0;
  FlowWrapSearch measure_search = {0};

  if (p->options.text_wrap_style != GFNT_TEXT_WRAP_AVOID_SHORT_LAST_LINE
      || state->line_count < 2) return;
  measure_search.paragraph = p;
  measure_search.constraint = constraint;
  measure_search.state = state;
  measure_search.callback_limit = FLOW_AVOID_CALLBACK_LIMIT;
  while (group_start < p->unit_count && !measure_search.exhausted) {
    size_t group_end = group_start, after_group = first_line;
    while (group_end < p->unit_count && !p->units[group_end].hard) group_end++;
    if (group_end < p->unit_count) group_end++;
    while (after_group < state->line_count && state->starts[after_group] < group_end) after_group++;
    if (after_group - first_line >= 2) {
      const size_t last = after_group - 1, previous = last - 1;
      const size_t previous_start = state->starts[previous];
      const size_t last_start = state->starts[last];
      GFNT_Band old_last_band = {0};
      LayoutLineState old_last_measure = {0};
      int64_t old_last_advance = 0;
      GFNT_F26Dot6 old_last_top = state->line_tops[last];

      if (flow_wrap_measure(&measure_search, last_start, group_end,
              state->hyphened[last], last, state->line_tops[last], &old_last_top,
              &old_last_band,
              &old_last_measure, &old_last_advance)
          && old_last_band.width > 0
          && old_last_advance < old_last_band.width / 3) {
        size_t best_boundary = 0;
        int64_t best_advance = INT64_MAX;
        GFNT_Band best_previous_band = {0}, best_last_band = {0};
        LayoutLineState best_last_measure = {0};
        GFNT_F26Dot6 best_previous_top = 0, best_last_top = 0;

        for (size_t boundary = last_start; boundary > previous_start + 1
                && !measure_search.exhausted;) {
          GFNT_Band previous_band = {0}, last_band = {0};
          LayoutLineState previous_measure = {0}, last_measure = {0};
          int64_t previous_advance = 0, last_advance = 0;
          GFNT_F26Dot6 new_previous_top = state->line_tops[previous];
          GFNT_F26Dot6 measured_last_top = 0;
          boundary--;
          if (!p->units[boundary - 1].opportunity || !p->units[boundary - 1].space) continue;
          if (!flow_wrap_measure(&measure_search, previous_start, boundary,
                  p->units[boundary - 1].hyphen, previous,
                  state->line_tops[previous], &new_previous_top,
                  &previous_band, &previous_measure,
                  &previous_advance)
              || previous_advance > previous_band.width) continue;
          const int64_t new_last_top64 = (int64_t)new_previous_top
              + previous_measure.thickness;
          if (new_last_top64 > INT32_MAX) continue;
          GFNT_F26Dot6 new_last_top = (GFNT_F26Dot6)new_last_top64;
          if (!flow_wrap_measure(&measure_search, boundary, group_end, false, last,
                  new_last_top, &measured_last_top,
                  &last_band, &last_measure, &last_advance)
              || last_advance > last_band.width
              || last_advance < last_band.width / 3
              || last_advance <= old_last_advance) continue;
          if (last_advance < best_advance) {
            best_boundary = boundary;
            best_advance = last_advance;
            best_previous_band = previous_band;
            best_last_band = last_band;
            best_last_measure = last_measure;
            best_previous_top = new_previous_top;
            best_last_top = measured_last_top;
          }
        }
        if (best_boundary && !measure_search.exhausted) {
          const int64_t after_group_top = (int64_t)best_last_top
              + best_last_measure.thickness;
          if (after_group == state->line_count || after_group_top == state->line_tops[after_group]) {
            state->starts[last] = best_boundary;
            state->hyphened[previous] = p->units[best_boundary - 1].hyphen;
            state->hyphened[last] = false;
            state->band_starts[previous] = best_previous_band.start;
            state->band_widths[previous] = best_previous_band.width;
            state->line_tops[previous] = best_previous_top;
            state->band_starts[last] = best_last_band.start;
            state->band_widths[last] = best_last_band.width;
            state->line_tops[last] = best_last_top;
          }
        }
      }
    }
    group_start = group_end;
    first_line = after_group;
  }
}
