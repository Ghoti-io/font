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
 * Whether a lookup would substitute a given run of glyphs, with no buffer to
 * apply it to: HarfBuzz's hb_ot_layout_lookup_would_substitute(). The Indic
 * shapers ask it of `rphf`, `pref`, `blwf` and `pstf` to learn what the font does
 * with a consonant and a virama before deciding where the base of a syllable is.
 *
 * Like HarfBuzz's it looks at the glyphs themselves and at no flags, no
 * skipping and no context before or after - and when @p zero_context is set, at
 * no chained rule that has any.
 */

#include "layout.h"

static bool gfnt_would_covers(GFNT_LApply * c, size_t sub, uint32_t glyph) {
  return gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, sub + 2)), glyph)
      != GFNT_LAYOUT_NOT_COVERED;
}

/** Whether input element @p value (a glyph, class or coverage offset) matches. */
static bool gfnt_would_match(GFNT_LApply * c, GFNT_Matcher m, uint32_t glyph,
    uint32_t value) {
  switch (m.kind) {
    case GFNT_MATCH_GLYPH:
      return glyph == value;
    case GFNT_MATCH_CLASS:
      return gfnt_l_class(c, m.origin, glyph) == value;
    case GFNT_MATCH_COVERAGE:
      return gfnt_l_coverage(c, gfnt_l_rel(m.origin, value), glyph)
          != GFNT_LAYOUT_NOT_COVERED;
  }
  return false;
}

/** The rest of the input, from element 1, all equal to the run. */
static bool gfnt_would_input(GFNT_LApply * c, GFNT_Matcher m, size_t elements,
    const uint32_t * glyphs, size_t n) {
  size_t i;

  for (i = 1; i < n; i++) {
    if (!gfnt_would_match(c, m, glyphs[i],
            gfnt_lu16(c, elements + 2 * (i - 1)))) {
      return false;
    }
  }
  return true;
}

static bool gfnt_would_ligature(GFNT_LApply * c, size_t sub,
    const uint32_t * glyphs, size_t n) {
  int32_t index = gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, sub + 2)),
      glyphs[0]);
  size_t set;
  uint32_t count;
  uint32_t i;

  if (index == GFNT_LAYOUT_NOT_COVERED
      || (uint32_t)index >= gfnt_lu16(c, sub + 4)) {
    return false;
  }
  set = gfnt_l_rel(sub, gfnt_lu16(c, sub + 6 + 2 * (size_t)index));
  if (!set) {
    return false;
  }
  count = gfnt_lu16(c, set);
  for (i = 0; i < count && !c->fault.bad; i++) {
    size_t lig = gfnt_l_rel(set, gfnt_lu16(c, set + 2 + 2 * (size_t)i));
    GFNT_Matcher glyph_match = {GFNT_MATCH_GLYPH, 0};

    if (lig && gfnt_lu16(c, lig + 2) == n
        && gfnt_would_input(c, glyph_match, lig + 4, glyphs, n)) {
      return true;
    }
  }
  return false;
}

/** One rule set, found by @p index, whose rules begin with a count and offsets. */
static bool gfnt_would_context_set(GFNT_LApply * c, size_t set,
    GFNT_Matcher m, const uint32_t * glyphs, size_t n) {
  uint32_t rules;
  uint32_t i;

  if (!set) {
    return false;
  }
  rules = gfnt_lu16(c, set);
  for (i = 0; i < rules && !c->fault.bad; i++) {
    size_t rule = gfnt_l_rel(set, gfnt_lu16(c, set + 2 + 2 * (size_t)i));

    if (rule && gfnt_lu16(c, rule) == n
        && gfnt_would_input(c, m, rule + 4, glyphs, n)) {
      return true;
    }
  }
  return false;
}

static bool gfnt_would_context(GFNT_LApply * c, size_t sub,
    const uint32_t * glyphs, size_t n) {
  uint32_t format = gfnt_lu16(c, sub);

  if (format == 1) {
    int32_t index = gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, sub + 2)),
        glyphs[0]);
    GFNT_Matcher m = {GFNT_MATCH_GLYPH, 0};

    if (index == GFNT_LAYOUT_NOT_COVERED
        || (uint32_t)index >= gfnt_lu16(c, sub + 4)) {
      return false;
    }
    return gfnt_would_context_set(c,
        gfnt_l_rel(sub, gfnt_lu16(c, sub + 6 + 2 * (size_t)index)), m, glyphs,
        n);
  }
  if (format == 2) {
    size_t classdef = gfnt_l_rel(sub, gfnt_lu16(c, sub + 4));
    uint32_t index = gfnt_l_class(c, classdef, glyphs[0]);
    GFNT_Matcher m = {GFNT_MATCH_CLASS, (uint32_t)classdef};

    if (index >= gfnt_lu16(c, sub + 6)) {
      return false;
    }
    return gfnt_would_context_set(c,
        gfnt_l_rel(sub, gfnt_lu16(c, sub + 8 + 2 * (size_t)index)), m, glyphs,
        n);
  }
  if (format == 3) {
    GFNT_Matcher m = {GFNT_MATCH_COVERAGE, (uint32_t)sub};

    return gfnt_lu16(c, sub + 2) == n && n > 0
        && gfnt_would_input(c, m, sub + 8, glyphs, n);
  }
  return false;
}

/** A chained rule: backtrack, input, lookahead. */
static bool gfnt_would_chain_rule(GFNT_LApply * c, size_t rule, GFNT_Matcher m,
    const uint32_t * glyphs, size_t n, bool zero_context) {
  uint32_t backtrack = gfnt_lu16(c, rule);
  size_t input_at = rule + 2 + 2 * (size_t)backtrack;
  uint32_t input = gfnt_lu16(c, input_at);
  size_t lookahead_at = input_at + 2 + 2 * (size_t)(input ? input - 1 : 0);
  uint32_t lookahead = gfnt_lu16(c, lookahead_at);

  if (zero_context && (backtrack || lookahead)) {
    return false;
  }
  return input == n && gfnt_would_input(c, m, input_at + 2, glyphs, n);
}

static bool gfnt_would_chain_set(GFNT_LApply * c, size_t set, GFNT_Matcher m,
    const uint32_t * glyphs, size_t n, bool zero_context) {
  uint32_t rules;
  uint32_t i;

  if (!set) {
    return false;
  }
  rules = gfnt_lu16(c, set);
  for (i = 0; i < rules && !c->fault.bad; i++) {
    size_t rule = gfnt_l_rel(set, gfnt_lu16(c, set + 2 + 2 * (size_t)i));

    if (rule && gfnt_would_chain_rule(c, rule, m, glyphs, n, zero_context)) {
      return true;
    }
  }
  return false;
}

static bool gfnt_would_chain(GFNT_LApply * c, size_t sub,
    const uint32_t * glyphs, size_t n, bool zero_context) {
  uint32_t format = gfnt_lu16(c, sub);

  if (format == 1) {
    int32_t index = gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, sub + 2)),
        glyphs[0]);
    GFNT_Matcher m = {GFNT_MATCH_GLYPH, 0};

    if (index == GFNT_LAYOUT_NOT_COVERED
        || (uint32_t)index >= gfnt_lu16(c, sub + 4)) {
      return false;
    }
    return gfnt_would_chain_set(c,
        gfnt_l_rel(sub, gfnt_lu16(c, sub + 6 + 2 * (size_t)index)), m, glyphs,
        n, zero_context);
  }
  if (format == 2) {
    size_t classdef = gfnt_l_rel(sub, gfnt_lu16(c, sub + 6));
    uint32_t index = gfnt_l_class(c, classdef, glyphs[0]);
    GFNT_Matcher m = {GFNT_MATCH_CLASS, (uint32_t)classdef};

    if (index >= gfnt_lu16(c, sub + 10)) {
      return false;
    }
    return gfnt_would_chain_set(c,
        gfnt_l_rel(sub, gfnt_lu16(c, sub + 12 + 2 * (size_t)index)), m, glyphs,
        n, zero_context);
  }
  if (format == 3) {
    uint32_t backtrack = gfnt_lu16(c, sub + 2);
    size_t input_at = sub + 4 + 2 * (size_t)backtrack;
    uint32_t input = gfnt_lu16(c, input_at);
    size_t lookahead_at = input_at + 2 + 2 * (size_t)input;
    uint32_t lookahead = gfnt_lu16(c, lookahead_at);
    GFNT_Matcher m = {GFNT_MATCH_COVERAGE, (uint32_t)sub};

    if (zero_context && (backtrack || lookahead)) {
      return false;
    }
    return input == n && input > 0
        && gfnt_would_input(c, m, input_at + 4, glyphs, n);
  }
  return false;
}

static bool gfnt_would_subtable(GFNT_LApply * c, uint16_t type, size_t sub,
    const uint32_t * glyphs, size_t n, bool zero_context) {
  if (type == 7) {
    if (gfnt_lu16(c, sub) != 1) {
      return false;
    }
    type = gfnt_lu16(c, sub + 2);
    sub += gfnt_lu32(c, sub + 4);
    if (type == 7) {
      return false;
    }
  }
  switch (type) {
    case 1:
    case 2:
    case 3:
      return n == 1 && gfnt_would_covers(c, sub, glyphs[0]);
    case 4:
      return gfnt_would_ligature(c, sub, glyphs, n);
    case 5:
      return gfnt_would_context(c, sub, glyphs, n);
    case 6:
      return gfnt_would_chain(c, sub, glyphs, n, zero_context);
    default:
      return false;
  }
}

/**
 * Whether the first coverage of subtable @p sub holds @p glyph. HarfBuzz tests the
 * first glyph against the union of these over a lookup's subtables - its digest -
 * and, for the class and coverage formats, nowhere else.
 */
static bool gfnt_would_digest(GFNT_LApply * c, uint16_t type, size_t sub,
    uint32_t glyph) {
  size_t at;

  if (type == 7) {
    if (gfnt_lu16(c, sub) != 1) {
      return false;
    }
    type = gfnt_lu16(c, sub + 2);
    sub += gfnt_lu32(c, sub + 4);
    if (type == 7) {
      return false;
    }
  }
  if (type < 1 || type > 6) {
    return false;
  }
  at = sub + 2;
  if (type == 5 && gfnt_lu16(c, sub) == 3) {
    at = sub + 6;
  }
  else if (type == 6 && gfnt_lu16(c, sub) == 3) {
    size_t input_at = sub + 4 + 2 * (size_t)gfnt_lu16(c, sub + 2);

    at = input_at + 2;
  }
  return gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, at)), glyph)
      != GFNT_LAYOUT_NOT_COVERED;
}

bool gfnt_l_would_apply(GFNT_LApply * c, uint32_t lookup_index,
    const uint32_t * glyphs, size_t n, bool zero_context) {
  size_t offset;
  uint16_t type;
  uint16_t flag;
  uint16_t count;
  uint16_t i;

  if (!n || !gfnt_l_lookup(c, lookup_index, &offset, &type, &flag, &count)) {
    return false;
  }
  for (i = 0; i < count && !c->fault.bad; i++) {
    size_t sub = gfnt_l_rel(offset, gfnt_lu16(c, offset + 6 + 2 * (size_t)i));

    if (sub && gfnt_would_digest(c, type, sub, glyphs[0])) {
      break;
    }
  }
  if (i == count) {
    return false;
  }
  for (i = 0; i < count && !c->fault.bad; i++) {
    size_t sub = gfnt_l_rel(offset, gfnt_lu16(c, offset + 6 + 2 * (size_t)i));

    if (sub && gfnt_would_subtable(c, type, sub, glyphs, n, zero_context)) {
      return true;
    }
  }
  return false;
}
