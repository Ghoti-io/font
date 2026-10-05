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
 * Applying lookups: the skipping iterator, matching a context's glyphs, the
 * recursion that lets one lookup call another, and the loop that runs a lookup
 * over the whole run. Shared by `GSUB` and `GPOS`, whose contextual lookups are
 * the same structure applied to different tables.
 */

#include <string.h>
#include "layout.h"

/* --- the skipping iterator --------------------------------------------- */

bool gfnt_l_check_glyph_property(GFNT_LApply * c, const GFNT_LInfo * info,
    uint32_t match_props) {
  uint32_t glyph_props = info->props;

  // A class the lookup says to ignore: ligature, base, or mark.
  if (glyph_props & match_props & GFNT_LF_IGNORE_FLAGS) {
    return false;
  }
  if (glyph_props & GFNT_PROP_MARK) {
    if (match_props & GFNT_LF_USE_MARK_FILTERING_SET) {
      return gfnt_l_mark_set_covers(c, match_props >> 16, info->glyph);
    }
    if (match_props & GFNT_LF_MARK_ATTACHMENT_TYPE) {
      return (match_props & GFNT_LF_MARK_ATTACHMENT_TYPE)
          == (glyph_props & GFNT_LF_MARK_ATTACHMENT_TYPE);
    }
  }
  return true;
}

void gfnt_liter_init(GFNT_LIter * it, GFNT_LApply * c, bool context_match) {
  memset(it, 0, sizeof *it);
  it->c = c;
  it->lookup_props = c->lookup_props;
  it->ignore_zwnj = c->is_gpos || (context_match && c->auto_zwnj);
  it->ignore_zwj = c->is_gpos || c->auto_zwj;
}

void gfnt_liter_reset(GFNT_LIter * it, size_t start, size_t num_items) {
  it->idx = start;
  it->num_items = num_items;
  it->end = it->c->buf->len;
}

void gfnt_liter_set_match(GFNT_LIter * it, GFNT_MatchKind kind, uint32_t base,
    size_t elements) {
  it->has_match = true;
  it->matcher.kind = kind;
  it->matcher.origin = base;
  it->elements_offset = elements;
}

GFNT_Skip gfnt_liter_may_skip(GFNT_LIter * it, const GFNT_LInfo * info) {
  if (!gfnt_l_check_glyph_property(it->c, info, it->lookup_props)) {
    return GFNT_SKIP_YES;
  }
  if ((info->flags & GFNT_GF_DEFAULT_IGNORABLE)
      && !(info->flags & GFNT_GF_HIDDEN)
      && !(info->props & GFNT_PROP_SUBSTITUTED)
      && (it->ignore_zwnj || !(info->flags & GFNT_GF_ZWNJ))
      && (it->ignore_zwj || !(info->flags & GFNT_GF_ZWJ))) {
    return GFNT_SKIP_MAYBE;
  }
  return GFNT_SKIP_NO;
}

static GFNT_Match gfnt_liter_may_match(GFNT_LIter * it,
    const GFNT_LInfo * info) {
  GFNT_LApply * c = it->c;
  uint32_t value;

  if (!(info->mask & c->lookup_mask)) {
    return GFNT_MATCH_NO;
  }
  if (!it->has_match) {
    return GFNT_MATCH_MAYBE;
  }
  value = gfnt_lu16(c, it->elements_offset);
  switch (it->matcher.kind) {
    case GFNT_MATCH_GLYPH:
      return info->glyph == value ? GFNT_MATCH_YES : GFNT_MATCH_NO;
    case GFNT_MATCH_CLASS:
      return gfnt_l_class(c, it->matcher.origin, info->glyph) == value
          ? GFNT_MATCH_YES : GFNT_MATCH_NO;
    case GFNT_MATCH_COVERAGE:
      return gfnt_l_coverage(c, gfnt_l_rel(it->matcher.origin, value),
                 info->glyph) != GFNT_LAYOUT_NOT_COVERED
          ? GFNT_MATCH_YES : GFNT_MATCH_NO;
  }
  return GFNT_MATCH_NO;
}

bool gfnt_liter_next(GFNT_LIter * it) {
  GFNT_LBuffer * b = it->c->buf;

  while (it->idx + it->num_items < it->end && !it->c->fault.bad) {
    const GFNT_LInfo * info;
    GFNT_Skip skip;
    GFNT_Match match;

    it->idx++;
    info = &b->info[it->idx];
    skip = gfnt_liter_may_skip(it, info);
    if (skip == GFNT_SKIP_YES) {
      continue;
    }
    match = gfnt_liter_may_match(it, info);
    if (match == GFNT_MATCH_YES
        || (match == GFNT_MATCH_MAYBE && skip == GFNT_SKIP_NO)) {
      it->num_items--;
      if (it->has_match) {
        it->elements_offset += 2;
      }
      return true;
    }
    if (skip == GFNT_SKIP_NO) {
      return false;
    }
  }
  return false;
}

bool gfnt_liter_prev(GFNT_LIter * it) {
  GFNT_LBuffer * b = it->c->buf;

  while (it->idx >= it->num_items && it->num_items > 0 && !it->c->fault.bad) {
    const GFNT_LInfo * info;
    GFNT_Skip skip;
    GFNT_Match match;

    it->idx--;
    info = &b->out_info[it->idx];
    skip = gfnt_liter_may_skip(it, info);
    if (skip == GFNT_SKIP_YES) {
      continue;
    }
    match = gfnt_liter_may_match(it, info);
    if (match == GFNT_MATCH_YES
        || (match == GFNT_MATCH_MAYBE && skip == GFNT_SKIP_NO)) {
      it->num_items--;
      if (it->has_match) {
        it->elements_offset += 2;
      }
      return true;
    }
    if (skip == GFNT_SKIP_NO) {
      return false;
    }
  }
  return false;
}

/* --- matching a context ------------------------------------------------- */

bool gfnt_l_match_input(GFNT_LApply * c, uint32_t count,
    GFNT_Matcher matcher, size_t elements, size_t * end_position,
    size_t * positions, uint32_t * total_components) {
  GFNT_LBuffer * b = c->buf;
  GFNT_LIter it;
  const GFNT_LInfo * cur = &b->info[b->idx];
  uint32_t components = gfnt_l_lig_num_comps(cur);
  uint32_t first_lig_id = gfnt_l_lig_id(cur);
  uint32_t first_lig_comp = gfnt_l_lig_comp(cur);
  enum { NOT_CHECKED, MAY_NOT_SKIP, MAY_SKIP } ligbase = NOT_CHECKED;
  uint32_t i;

  if (count > GFNT_LAYOUT_MAX_CONTEXT || count == 0) {
    return false;
  }
  // The glyphs being substituted are not a "context", and for those a ZWNJ is
  // not skipped; the backtrack and lookahead below are, and it is.
  gfnt_liter_init(&it, c, false);
  gfnt_liter_reset(&it, b->idx, count - 1);
  gfnt_liter_set_match(&it, matcher.kind, matcher.origin, elements);
  positions[0] = b->idx;
  for (i = 1; i < count; i++) {
    uint32_t this_lig_id;
    uint32_t this_lig_comp;

    if (!gfnt_liter_next(&it)) {
      return false;
    }
    positions[i] = it.idx;
    this_lig_id = gfnt_l_lig_id(&b->info[it.idx]);
    this_lig_comp = gfnt_l_lig_comp(&b->info[it.idx]);
    if (first_lig_id && first_lig_comp) {
      // The first glyph is a mark attached to a ligature's component, so the
      // rest have to be attached to the same one - unless that ligature is
      // itself skippable, in which case its components are free.
      if (first_lig_id != this_lig_id || first_lig_comp != this_lig_comp) {
        if (ligbase == NOT_CHECKED) {
          bool found = false;
          size_t j = b->out_len;

          while (j && gfnt_l_lig_id(&b->out_info[j - 1]) == first_lig_id) {
            if (gfnt_l_lig_comp(&b->out_info[j - 1]) == 0) {
              j--;
              found = true;
              break;
            }
            j--;
          }
          ligbase = found
                  && gfnt_liter_may_skip(&it, &b->out_info[j]) == GFNT_SKIP_YES
              ? MAY_SKIP : MAY_NOT_SKIP;
        }
        if (ligbase == MAY_NOT_SKIP) {
          return false;
        }
      }
    }
    else if (this_lig_id && this_lig_comp && this_lig_id != first_lig_id) {
      // The first glyph is not attached to a ligature component, so none of the
      // others may be attached to one either, except the first glyph's own.
      return false;
    }
    components += gfnt_l_lig_num_comps(&b->info[it.idx]);
  }
  *end_position = it.idx + 1;
  if (total_components) {
    *total_components = components;
  }
  return true;
}

static bool gfnt_l_match_backtrack(GFNT_LApply * c, uint32_t count,
    GFNT_Matcher matcher, size_t elements, size_t * match_start) {
  GFNT_LIter it;
  uint32_t i;

  gfnt_liter_init(&it, c, true);
  gfnt_liter_reset(&it, gfnt_lbuf_backtrack_len(c->buf), count);
  gfnt_liter_set_match(&it, matcher.kind, matcher.origin, elements);
  for (i = 0; i < count; i++) {
    if (!gfnt_liter_prev(&it)) {
      return false;
    }
  }
  *match_start = it.idx;
  return true;
}

static bool gfnt_l_match_lookahead(GFNT_LApply * c, uint32_t count,
    GFNT_Matcher matcher, size_t elements, size_t start_index,
    size_t * end_index) {
  GFNT_LIter it;
  uint32_t i;

  gfnt_liter_init(&it, c, true);
  gfnt_liter_reset(&it, start_index - 1, count);
  gfnt_liter_set_match(&it, matcher.kind, matcher.origin, elements);
  for (i = 0; i < count; i++) {
    if (!gfnt_liter_next(&it)) {
      return false;
    }
  }
  *end_index = it.idx + 1;
  return true;
}

/* --- recursion ---------------------------------------------------------- */

static bool gfnt_l_recurse(GFNT_LApply * c, uint32_t lookup_index) {
  bool applied;

  if (c->nesting_left == 0 || c->buf->max_ops-- <= 0) {
    return false;
  }
  c->nesting_left--;
  applied = gfnt_l_apply_lookup_at_cursor(c, lookup_index);
  c->nesting_left++;
  return applied;
}

/**
 * Run a rule's nested lookups over the glyphs it matched.
 *
 * The positions are indices into the *output so far* plus what is left of the
 * input, and a nested lookup may change how many glyphs that is. What the rest
 * of the sequence is then taken to mean is HarfBuzz's rule, kept to the letter:
 * the matched positions after the change are shifted by the change, a deletion
 * past the start of the match is clamped, and the end of the match moves with
 * it.
 */
static void gfnt_l_apply_lookup(GFNT_LApply * c, uint32_t count,
    size_t * match_positions, uint32_t lookup_count, size_t records,
    size_t match_end) {
  GFNT_LBuffer * b = c->buf;
  int64_t end;
  int64_t delta;
  uint32_t i;
  uint32_t j;

  {
    int64_t backtrack = (int64_t)gfnt_lbuf_backtrack_len(b);

    end = backtrack + (int64_t)match_end - (int64_t)b->idx;
    delta = backtrack - (int64_t)b->idx;
    for (j = 0; j < count; j++) {
      match_positions[j] = (size_t)((int64_t)match_positions[j] + delta);
    }
  }
  for (i = 0; i < lookup_count && !b->oom && !c->fault.bad; i++) {
    uint32_t idx = gfnt_lu16(c, records + 4 * (size_t)i);
    uint32_t lookup = gfnt_lu16(c, records + 4 * (size_t)i + 2);
    size_t orig_len;
    size_t new_len;
    uint32_t next;

    if (idx >= count) {
      continue;
    }
    orig_len = gfnt_lbuf_backtrack_len(b) + gfnt_lbuf_lookahead_len(b);
    // An earlier nested lookup may have deleted what this one pointed at.
    if (match_positions[idx] >= orig_len) {
      continue;
    }
    if (!gfnt_lbuf_move_to(b, match_positions[idx])) {
      break;
    }
    if (b->max_ops <= 0) {
      break;
    }
    if (!gfnt_l_recurse(c, lookup)) {
      continue;
    }
    new_len = gfnt_lbuf_backtrack_len(b) + gfnt_lbuf_lookahead_len(b);
    delta = (int64_t)new_len - (int64_t)orig_len;
    if (!delta) {
      continue;
    }
    end += delta;
    if (end < (int64_t)match_positions[idx]) {
      // The nested lookup removed so much that the end would fall before the
      // position it ran at, which it cannot: never rewind past where it started.
      delta += (int64_t)match_positions[idx] - end;
      end = (int64_t)match_positions[idx];
    }
    next = idx + 1;
    if (delta > 0) {
      if (delta + (int64_t)count > (int64_t)GFNT_LAYOUT_MAX_CONTEXT) {
        break;
      }
    }
    else {
      if (delta < (int64_t)next - (int64_t)count) {
        delta = (int64_t)next - (int64_t)count;
      }
      next = (uint32_t)((int64_t)next - delta);
    }
    memmove(match_positions + (int64_t)next + delta, match_positions + next,
        (count - next) * sizeof match_positions[0]);
    next = (uint32_t)((int64_t)next + delta);
    count = (uint32_t)((int64_t)count + delta);
    for (j = idx + 1; j < next; j++) {
      match_positions[j] = match_positions[j - 1] + 1;
    }
    for (; next < count; next++) {
      match_positions[next] = (size_t)((int64_t)match_positions[next] + delta);
    }
  }
  (void)gfnt_lbuf_move_to(b, (size_t)end);
}

/* --- context lookups ---------------------------------------------------- */

/** One rule of a plain context: its glyphs or classes, then its nested lookups. */
static bool gfnt_l_context_rule(GFNT_LApply * c, size_t rule,
    GFNT_Matcher matcher) {
  uint32_t input_count = gfnt_lu16(c, rule);
  uint32_t lookup_count = gfnt_lu16(c, rule + 2);
  size_t elements = rule + 4;
  size_t records = elements + 2 * (size_t)(input_count ? input_count - 1 : 0);
  size_t positions[GFNT_LAYOUT_MAX_CONTEXT];
  size_t match_end = 0;

  if (!input_count
      || !gfnt_l_match_input(c, input_count, matcher, elements, &match_end,
          positions, NULL)) {
    return false;
  }
  gfnt_l_apply_lookup(c, input_count, positions, lookup_count, records,
      match_end);
  return true;
}

bool gfnt_l_context_apply(GFNT_LApply * c, size_t subtable) {
  GFNT_LBuffer * b = c->buf;
  uint32_t format = gfnt_lu16(c, subtable);
  uint32_t glyph = b->info[b->idx].glyph;

  if (format == 1 || format == 2) {
    size_t coverage = gfnt_l_rel(subtable, gfnt_lu16(c, subtable + 2));
    int32_t index = gfnt_l_coverage(c, coverage, glyph);
    size_t sets;
    uint32_t set_count;
    size_t set;
    uint32_t rules;
    GFNT_Matcher matcher;
    uint32_t i;

    if (index == GFNT_LAYOUT_NOT_COVERED) {
      return false;
    }
    if (format == 1) {
      sets = subtable + 6;
      set_count = gfnt_lu16(c, subtable + 4);
      matcher.kind = GFNT_MATCH_GLYPH;
      matcher.origin = 0;
    }
    else {
      size_t classdef = gfnt_l_rel(subtable, gfnt_lu16(c, subtable + 4));

      sets = subtable + 8;
      set_count = gfnt_lu16(c, subtable + 6);
      matcher.kind = GFNT_MATCH_CLASS;
      matcher.origin = (uint32_t)classdef;
      index = (int32_t)gfnt_l_class(c, classdef, glyph);
    }
    if ((uint32_t)index >= set_count) {
      return false;
    }
    set = gfnt_l_rel(subtable, gfnt_lu16(c, sets + 2 * (size_t)index));
    if (!set) {
      return false;
    }
    rules = gfnt_lu16(c, set);
    for (i = 0; i < rules && !c->fault.bad; i++) {
      size_t rule = gfnt_l_rel(set, gfnt_lu16(c, set + 2 + 2 * (size_t)i));

      if (rule && gfnt_l_context_rule(c, rule, matcher)) {
        return true;
      }
    }
    return false;
  }
  if (format == 3) {
    uint32_t input_count = gfnt_lu16(c, subtable + 2);
    uint32_t lookup_count = gfnt_lu16(c, subtable + 4);
    size_t coverages = subtable + 6;
    size_t records = coverages + 2 * (size_t)input_count;
    size_t positions[GFNT_LAYOUT_MAX_CONTEXT];
    size_t match_end = 0;
    GFNT_Matcher matcher = {GFNT_MATCH_COVERAGE, (uint32_t)subtable};

    if (!input_count
        || gfnt_l_coverage(c,
               gfnt_l_rel(subtable, gfnt_lu16(c, coverages)), glyph)
            == GFNT_LAYOUT_NOT_COVERED) {
      return false;
    }
    if (!gfnt_l_match_input(c, input_count, matcher, coverages + 2, &match_end,
            positions, NULL)) {
      return false;
    }
    gfnt_l_apply_lookup(c, input_count, positions, lookup_count, records,
        match_end);
    return true;
  }
  c->fault.bad = true;
  c->fault.table = c->lt->tag;
  c->fault.offset = subtable;
  return false;
}

/** The matchers of a chain rule's three parts. */
typedef struct GFNT_ChainMatchers {
  GFNT_Matcher backtrack;
  GFNT_Matcher input;
  GFNT_Matcher lookahead;
} GFNT_ChainMatchers;

/**
 * Match and apply one chained rule whose arrays begin at @p at: backtrack count
 * and elements, input count and elements, lookahead count and elements, then the
 * nested lookup records.
 *
 * @p input_first says the first input element is already in the array (formats 1
 * and 2 omit it, because it is the glyph that selected the rule; format 3 has all
 * of them and is told to start one in).
 */
static bool gfnt_l_chain_rule(GFNT_LApply * c, size_t at,
    const GFNT_ChainMatchers * m, bool input_includes_first) {
  uint32_t backtrack_count = gfnt_lu16(c, at);
  size_t backtrack = at + 2;
  size_t input_at = backtrack + 2 * (size_t)backtrack_count;
  uint32_t input_count = gfnt_lu16(c, input_at);
  size_t input = input_at + 2;
  size_t skipped = input_includes_first ? 1 : 0;
  size_t lookahead_at;
  uint32_t lookahead_count;
  size_t lookahead;
  size_t records_at;
  uint32_t lookup_count;
  size_t positions[GFNT_LAYOUT_MAX_CONTEXT];
  size_t match_end = 0;
  size_t end_index;
  size_t start_index = 0;

  if (c->fault.bad || !input_count) {
    return false;
  }
  // Formats 1 and 2 store inputCount - 1 elements, format 3 stores inputCount.
  lookahead_at = input + 2 * (size_t)(input_includes_first ? input_count
                                                           : input_count - 1);
  lookahead_count = gfnt_lu16(c, lookahead_at);
  lookahead = lookahead_at + 2;
  records_at = lookahead + 2 * (size_t)lookahead_count;
  lookup_count = gfnt_lu16(c, records_at);
  records_at += 2;
  if (!gfnt_l_match_input(c, input_count, m->input, input + 2 * skipped,
          &match_end, positions, NULL)) {
    return false;
  }
  end_index = match_end;
  if (!gfnt_l_match_lookahead(c, lookahead_count, m->lookahead, lookahead,
          match_end, &end_index)) {
    return false;
  }
  if (!gfnt_l_match_backtrack(c, backtrack_count, m->backtrack, backtrack,
          &start_index)) {
    return false;
  }
  gfnt_l_apply_lookup(c, input_count, positions, lookup_count, records_at,
      match_end);
  return true;
}

bool gfnt_l_chain_context_apply(GFNT_LApply * c, size_t subtable) {
  GFNT_LBuffer * b = c->buf;
  uint32_t format = gfnt_lu16(c, subtable);
  uint32_t glyph = b->info[b->idx].glyph;

  if (format == 1 || format == 2) {
    size_t coverage = gfnt_l_rel(subtable, gfnt_lu16(c, subtable + 2));
    int32_t index = gfnt_l_coverage(c, coverage, glyph);
    GFNT_ChainMatchers m;
    size_t sets;
    uint32_t set_count;
    size_t set;
    uint32_t rules;
    uint32_t i;

    if (index == GFNT_LAYOUT_NOT_COVERED) {
      return false;
    }
    if (format == 1) {
      sets = subtable + 6;
      set_count = gfnt_lu16(c, subtable + 4);
      m.backtrack = (GFNT_Matcher){GFNT_MATCH_GLYPH, 0};
      m.input = m.backtrack;
      m.lookahead = m.backtrack;
    }
    else {
      size_t back = gfnt_l_rel(subtable, gfnt_lu16(c, subtable + 4));
      size_t in = gfnt_l_rel(subtable, gfnt_lu16(c, subtable + 6));
      size_t ahead = gfnt_l_rel(subtable, gfnt_lu16(c, subtable + 8));

      sets = subtable + 12;
      set_count = gfnt_lu16(c, subtable + 10);
      m.backtrack = (GFNT_Matcher){GFNT_MATCH_CLASS, (uint32_t)back};
      m.input = (GFNT_Matcher){GFNT_MATCH_CLASS, (uint32_t)in};
      m.lookahead = (GFNT_Matcher){GFNT_MATCH_CLASS, (uint32_t)ahead};
      index = (int32_t)gfnt_l_class(c, in, glyph);
    }
    if ((uint32_t)index >= set_count) {
      return false;
    }
    set = gfnt_l_rel(subtable, gfnt_lu16(c, sets + 2 * (size_t)index));
    if (!set) {
      return false;
    }
    rules = gfnt_lu16(c, set);
    for (i = 0; i < rules && !c->fault.bad; i++) {
      size_t rule = gfnt_l_rel(set, gfnt_lu16(c, set + 2 + 2 * (size_t)i));

      if (rule && gfnt_l_chain_rule(c, rule, &m, false)) {
        return true;
      }
    }
    return false;
  }
  if (format == 3) {
    GFNT_Matcher cover = {GFNT_MATCH_COVERAGE, (uint32_t)subtable};
    GFNT_ChainMatchers m = {cover, cover, cover};
    uint32_t backtrack_count = gfnt_lu16(c, subtable + 2);
    size_t input_at = subtable + 4 + 2 * (size_t)backtrack_count;
    uint32_t input_count = gfnt_lu16(c, input_at);

    if (!input_count
        || gfnt_l_coverage(c, gfnt_l_rel(subtable, gfnt_lu16(c, input_at + 2)),
               glyph) == GFNT_LAYOUT_NOT_COVERED) {
      return false;
    }
    // The first input coverage is the one just tested; matching starts after it.
    return gfnt_l_chain_rule(c, subtable + 2, &m, true);
  }
  c->fault.bad = true;
  c->fault.table = c->lt->tag;
  c->fault.offset = subtable;
  return false;
}

/* --- lookups and the loop that runs them -------------------------------- */

bool gfnt_l_apply_subtables(GFNT_LApply * c, size_t lookup_offset) {
  uint16_t type = gfnt_lu16(c, lookup_offset);
  uint16_t count = gfnt_lu16(c, lookup_offset + 4);
  uint16_t i;

  for (i = 0; i < count && !c->fault.bad; i++) {
    size_t subtable = gfnt_l_rel(lookup_offset,
        gfnt_lu16(c, lookup_offset + 6 + 2 * (size_t)i));
    bool applied;

    if (!subtable) {
      continue;
    }
    applied = c->is_gpos ? gfnt_gpos_apply_subtable(c, type, subtable)
                         : gfnt_gsub_apply_subtable(c, type, subtable);
    if (applied) {
      return true;
    }
  }
  return false;
}

/** Load a lookup's flags and mark filtering set into the context. */
static bool gfnt_l_enter_lookup(GFNT_LApply * c, uint32_t lookup_index,
    size_t * out_offset) {
  uint16_t type;
  uint16_t flag;
  uint16_t count;
  size_t offset;
  uint32_t props;

  if (!gfnt_l_lookup(c, lookup_index, &offset, &type, &flag, &count)) {
    return false;
  }
  props = flag;
  if (flag & GFNT_LF_USE_MARK_FILTERING_SET) {
    props |= (uint32_t)gfnt_lu16(c, offset + 6 + 2 * (size_t)count) << 16;
  }
  c->lookup_props = props;
  c->lookup_index = lookup_index;
  *out_offset = offset;
  return !c->fault.bad;
}

bool gfnt_l_apply_lookup_at_cursor(GFNT_LApply * c, uint32_t lookup_index) {
  uint32_t saved_props = c->lookup_props;
  uint32_t saved_index = c->lookup_index;
  size_t offset;
  bool applied = false;

  if (gfnt_l_enter_lookup(c, lookup_index, &offset)) {
    applied = gfnt_l_apply_subtables(c, offset);
  }
  c->lookup_props = saved_props;
  c->lookup_index = saved_index;
  return applied;
}

void gfnt_l_apply_lookup_to_buffer(GFNT_LApply * c, uint32_t lookup_index) {
  GFNT_LBuffer * b = c->buf;
  size_t offset;
  bool reverse = false;

  if (!b->len || !c->lookup_mask
      || !gfnt_l_enter_lookup(c, lookup_index, &offset)) {
    return;
  }
  if (!c->is_gpos) {
    reverse = gfnt_gsub_is_reverse(gfnt_lu16(c, offset));
    if (gfnt_lu16(c, offset) == 7 && gfnt_lu16(c, offset + 4) > 0) {
      size_t first = gfnt_l_rel(offset, gfnt_lu16(c, offset + 6));

      reverse = first && gfnt_lu16(c, first + 2) == 8;
    }
  }
  c->nesting_left = GFNT_LAYOUT_MAX_NESTING;
  if (!reverse) {
    if (!c->is_gpos) {
      gfnt_lbuf_clear_output(b);
    }
    b->idx = 0;
    while (b->idx < b->len && !b->oom && !c->fault.bad) {
      GFNT_LInfo * cur = &b->info[b->idx];
      bool applied = false;

      if ((cur->mask & c->lookup_mask)
          && gfnt_l_check_glyph_property(c, cur, c->lookup_props)) {
        applied = gfnt_l_apply_subtables(c, offset);
      }
      if (!applied) {
        gfnt_lbuf_next_glyph(b);
      }
    }
    if (!c->is_gpos) {
      gfnt_lbuf_sync(b);
    }
  }
  else {
    b->idx = b->len - 1;
    for (;;) {
      GFNT_LInfo * cur = &b->info[b->idx];

      if ((cur->mask & c->lookup_mask)
          && gfnt_l_check_glyph_property(c, cur, c->lookup_props)) {
        (void)gfnt_l_apply_subtables(c, offset);
      }
      // A reverse lookup does not advance the cursor by itself.
      if (b->idx == 0 || b->oom || c->fault.bad) {
        break;
      }
      b->idx--;
    }
  }
}
