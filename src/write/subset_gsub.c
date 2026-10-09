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
 * The glyphs `GSUB` can reach from a set of glyphs.
 *
 * A subset that kept `f` and `i` and dropped the `fi` ligature would shape
 * differently from the font it came from, so the set is closed over every
 * substitution a lookup could make. The closure is a fixed point over two
 * things: the glyph set, and the set of lookups that are *active* - those a
 * chosen feature names, and those a contextual rule that can match calls.
 *
 * Contextual rules are tested against the set (a rule whose glyphs are not all
 * in it cannot match, so it calls nothing), except class-based ones, which are
 * taken to match whenever their first glyph is covered. That makes the result an
 * over-approximation: it can hold a glyph no text reaches, and never lacks one.
 * `FeatureVariations` substitutions are followed as if always in force.
 */

#include <stdlib.h>
#include <string.h>
#include "../layout_tables/layout.h"
#include "write.h"

typedef struct GFNT_Closure {
  const GFNT_Reader * r;   ///< The whole `GSUB` table.
  uint8_t * keep;          ///< One byte per glyph of the face.
  size_t count;
  uint8_t * active;        ///< One byte per lookup.
  uint32_t lookup_list;    ///< Offset of the LookupList.
  uint32_t lookup_count;
  bool changed;
  bool bad;
  bool grow;               ///< Add substituted glyphs (the closure); off: only find lookups.
  bool is_gpos;            ///< `GPOS`: only its contextual lookups call others.
} GFNT_Closure;

static uint16_t cu16(GFNT_Closure * c, size_t offset) {
  return gfnt_lr_u16(c->r, offset, &c->bad);
}

static void add_glyph(GFNT_Closure * c, uint32_t g) {
  if (c->grow && g < c->count && !c->keep[g]) {
    c->keep[g] = 1;
    c->changed = true;
  }
}

static bool has_glyph(const GFNT_Closure * c, uint32_t g) {
  return g < c->count && c->keep[g];
}

static void activate(GFNT_Closure * c, uint32_t lookup) {
  if (lookup < c->lookup_count && !c->active[lookup]) {
    c->active[lookup] = 1;
    c->changed = true;
  }
}

/** Called for each glyph of a coverage table with its coverage index. */
typedef bool (*GFNT_CoverageVisit)(GFNT_Closure * c, uint32_t glyph,
    uint32_t index, void * ctx);

/** Visit a coverage table's glyphs until @p visit returns false. */
static void coverage_each(GFNT_Closure * c, size_t coverage,
    GFNT_CoverageVisit visit, void * ctx) {
  uint16_t format;
  uint16_t count;
  size_t i;

  if (!coverage) {
    return;
  }
  format = cu16(c, coverage);
  count = cu16(c, coverage + 2);
  if (c->bad) {
    return;
  }
  if (format == 1) {
    for (i = 0; i < count; ++i) {
      uint16_t g = cu16(c, coverage + 4 + 2 * i);

      if (c->bad || !visit(c, g, (uint32_t)i, ctx)) {
        return;
      }
    }
  }
  else if (format == 2) {
    for (i = 0; i < count; ++i) {
      size_t at = coverage + 4 + 6 * i;
      uint16_t start = cu16(c, at);
      uint16_t end = cu16(c, at + 2);
      uint16_t first = cu16(c, at + 4);
      uint32_t g;

      if (c->bad) {
        return;
      }
      for (g = start; g <= end; ++g) {
        if (!visit(c, g, first + (g - start), ctx)) {
          return;
        }
      }
    }
  }
  else {
    c->bad = true;
  }
}

static bool visit_any(GFNT_Closure * c, uint32_t glyph, uint32_t index, void * ctx) {
  (void)index;
  if (has_glyph(c, glyph)) {
    *(bool *)ctx = true;
    return false;
  }
  return true;
}

/** Whether some glyph of the coverage table is kept. */
static bool coverage_meets(GFNT_Closure * c, size_t coverage) {
  bool found = false;

  coverage_each(c, coverage, visit_any, &found);
  return found;
}

static void add_sequence(GFNT_Closure * c, size_t seq) {
  uint16_t n = cu16(c, seq);
  uint16_t i;

  for (i = 0; i < n && !c->bad; ++i) {
    add_glyph(c, cu16(c, seq + 2 + 2 * (size_t)i));
  }
}

static void add_ligatures(GFNT_Closure * c, size_t set) {
  uint16_t n = cu16(c, set);
  uint16_t i;

  for (i = 0; i < n && !c->bad; ++i) {
    size_t lig = set + cu16(c, set + 2 + 2 * (size_t)i);
    uint16_t out = cu16(c, lig);
    uint16_t comps = cu16(c, lig + 2);
    uint16_t k;
    bool all = true;

    for (k = 1; k < comps && !c->bad; ++k) {
      if (!has_glyph(c, cu16(c, lig + 4 + 2 * (size_t)(k - 1)))) {
        all = false;
        break;
      }
    }
    if (all && !c->bad) {
      add_glyph(c, out);
    }
  }
}

/** Call every lookup of a run of SequenceLookupRecords. */
static void activate_records(GFNT_Closure * c, size_t at, uint16_t count) {
  uint16_t i;

  for (i = 0; i < count && !c->bad; ++i) {
    activate(c, cu16(c, at + 4 * (size_t)i + 2));
  }
}

/** Whether all @p n glyph ids at @p at are kept. */
static bool glyphs_all_kept(GFNT_Closure * c, size_t at, size_t n) {
  size_t i;

  for (i = 0; i < n; ++i) {
    uint16_t g = cu16(c, at + 2 * i);

    if (c->bad || !has_glyph(c, g)) {
      return false;
    }
  }
  return true;
}

/** Whether each of @p n coverage offsets (from @p base) meets the kept set. */
static bool coverages_all_meet(GFNT_Closure * c, size_t at, size_t n, size_t base) {
  size_t i;

  for (i = 0; i < n; ++i) {
    uint16_t off = cu16(c, at + 2 * i);

    if (c->bad || !off || !coverage_meets(c, base + off)) {
      return false;
    }
  }
  return true;
}

/* --- the coverage-driven lookup types ------------------------------------ */

/** What a visit does with each kept glyph its coverage holds. */
typedef struct GFNT_Subst {
  int kind;
  size_t origin;   ///< The subtable: offsets in the array are from here.
  size_t array;    ///< Where the per-coverage-index data begins.
  int32_t delta;
} GFNT_Subst;

enum { SUB_DELTA, SUB_ONE, SUB_SEQUENCES, SUB_LIGATURES };

static bool visit_substitute(GFNT_Closure * c, uint32_t glyph, uint32_t index,
    void * ctx) {
  const GFNT_Subst * s = ctx;

  if (!has_glyph(c, glyph)) {
    return true;
  }
  if (s->kind == SUB_DELTA) {
    add_glyph(c, (uint32_t)((int32_t)glyph + s->delta) & 0xFFFFu);
  }
  else if (s->kind == SUB_ONE) {
    add_glyph(c, cu16(c, s->array + 2 * (size_t)index));
  }
  else {
    uint16_t off = cu16(c, s->array + 2 * (size_t)index);

    if (!c->bad && off) {
      if (s->kind == SUB_SEQUENCES) {
        add_sequence(c, s->origin + off);
      }
      else {
        add_ligatures(c, s->origin + off);
      }
    }
  }
  return !c->bad;
}

/** A context rule set indexed by first glyph, for the formats that have one. */
typedef struct GFNT_RuleSets {
  size_t origin;
  size_t array;
  bool chaining;
} GFNT_RuleSets;

static void rule_set_format1(GFNT_Closure * c, size_t set, bool chaining) {
  uint16_t rules = cu16(c, set);
  uint16_t k;

  for (k = 0; k < rules && !c->bad; ++k) {
    size_t rule = set + cu16(c, set + 2 + 2 * (size_t)k);

    if (!chaining) {
      uint16_t glyphs = cu16(c, rule);
      uint16_t records = cu16(c, rule + 2);

      if (glyphs >= 1 && glyphs_all_kept(c, rule + 4, (size_t)glyphs - 1)) {
        activate_records(c, rule + 4 + 2 * (size_t)(glyphs - 1), records);
      }
    }
    else {
      uint16_t back = cu16(c, rule);
      size_t at = rule + 2 + 2 * (size_t)back;
      uint16_t input = cu16(c, at);
      size_t la_at = at + 2 + 2 * (size_t)(input ? input - 1 : 0);
      uint16_t ahead = cu16(c, la_at);
      size_t rec_at = la_at + 2 + 2 * (size_t)ahead;

      if (input >= 1 && glyphs_all_kept(c, rule + 2, back)
          && glyphs_all_kept(c, at + 2, (size_t)input - 1)
          && glyphs_all_kept(c, la_at + 2, ahead)) {
        activate_records(c, rec_at + 2, cu16(c, rec_at));
      }
    }
  }
}

static bool visit_rule_set(GFNT_Closure * c, uint32_t glyph, uint32_t index,
    void * ctx) {
  const GFNT_RuleSets * rs = ctx;

  if (has_glyph(c, glyph)) {
    uint16_t off = cu16(c, rs->array + 2 * (size_t)index);

    if (!c->bad && off) {
      rule_set_format1(c, rs->origin + off, rs->chaining);
    }
  }
  return !c->bad;
}

/** Class-based rules: every rule in every set is taken to be able to match. */
static void rule_sets_by_class(GFNT_Closure * c, size_t base, size_t array,
    uint16_t sets, bool chaining) {
  uint16_t i;

  for (i = 0; i < sets && !c->bad; ++i) {
    size_t set_off = cu16(c, array + 2 * (size_t)i);
    size_t set;
    uint16_t rules;
    uint16_t k;

    if (!set_off) {
      continue;
    }
    set = base + set_off;
    rules = cu16(c, set);
    for (k = 0; k < rules && !c->bad; ++k) {
      size_t rule = set + cu16(c, set + 2 + 2 * (size_t)k);

      if (!chaining) {
        uint16_t glyphs = cu16(c, rule);

        activate_records(c, rule + 4 + 2 * (size_t)(glyphs ? glyphs - 1 : 0),
            cu16(c, rule + 2));
      }
      else {
        uint16_t back = cu16(c, rule);
        size_t at = rule + 2 + 2 * (size_t)back;
        uint16_t input = cu16(c, at);
        size_t la_at = at + 2 + 2 * (size_t)(input ? input - 1 : 0);
        uint16_t ahead = cu16(c, la_at);
        size_t rec_at = la_at + 2 + 2 * (size_t)ahead;

        activate_records(c, rec_at + 2, cu16(c, rec_at));
      }
    }
  }
}

/** A context (type 5) or chaining context (type 6) subtable, any format. */
static void context_subtable(GFNT_Closure * c, size_t st, bool chaining) {
  uint16_t format = cu16(c, st);

  if (c->bad) {
    return;
  }
  if (format == 1) {
    GFNT_RuleSets rs = {st, st + 6, chaining};

    coverage_each(c, st + cu16(c, st + 2), visit_rule_set, &rs);
  }
  else if (format == 2) {
    if (coverage_meets(c, st + cu16(c, st + 2))) {
      if (chaining) {
        rule_sets_by_class(c, st, st + 12, cu16(c, st + 10), true);
      }
      else {
        rule_sets_by_class(c, st, st + 8, cu16(c, st + 6), false);
      }
    }
  }
  else if (format == 3) {
    if (!chaining) {
      uint16_t glyphs = cu16(c, st + 2);
      uint16_t records = cu16(c, st + 4);

      if (coverages_all_meet(c, st + 6, glyphs, st)) {
        activate_records(c, st + 6 + 2 * (size_t)glyphs, records);
      }
    }
    else {
      uint16_t back = cu16(c, st + 2);
      size_t in_at = st + 4 + 2 * (size_t)back;
      uint16_t input = cu16(c, in_at);
      size_t la_at = in_at + 2 + 2 * (size_t)input;
      uint16_t ahead = cu16(c, la_at);
      size_t rec_at = la_at + 2 + 2 * (size_t)ahead;

      if (coverages_all_meet(c, st + 4, back, st)
          && coverages_all_meet(c, in_at + 2, input, st)
          && coverages_all_meet(c, la_at + 2, ahead, st)) {
        activate_records(c, rec_at + 2, cu16(c, rec_at));
      }
    }
  }
  else {
    c->bad = true;
  }
}

/** Type 8: the substitutes are indexed by coverage, the context must be kept. */
static void reverse_subtable(GFNT_Closure * c, size_t st) {
  uint16_t back;
  size_t la_at;
  uint16_t ahead;
  size_t sub_at;
  GFNT_Subst s;

  if (cu16(c, st) != 1) {
    c->bad = true;
    return;
  }
  back = cu16(c, st + 4);
  la_at = st + 6 + 2 * (size_t)back;
  ahead = cu16(c, la_at);
  sub_at = la_at + 2 + 2 * (size_t)ahead;
  if (c->bad || !coverages_all_meet(c, st + 6, back, st)
      || !coverages_all_meet(c, la_at + 2, ahead, st)) {
    return;
  }
  s.kind = SUB_ONE;
  s.origin = st;
  s.array = sub_at + 2;
  s.delta = 0;
  coverage_each(c, st + cu16(c, st + 2), visit_substitute, &s);
}

static void close_subtable(GFNT_Closure * c, uint16_t type, size_t st) {
  GFNT_Subst s;
  uint16_t format;

  switch (type) {
  case 1:
    format = cu16(c, st);
    s.origin = st;
    s.delta = 0;
    if (format == 1) {
      s.kind = SUB_DELTA;
      s.delta = (int16_t)cu16(c, st + 4);
      s.array = 0;
    }
    else if (format == 2) {
      s.kind = SUB_ONE;
      s.array = st + 6;
    }
    else {
      c->bad = true;
      return;
    }
    coverage_each(c, st + cu16(c, st + 2), visit_substitute, &s);
    break;
  case 2:
  case 3:
    if (cu16(c, st) != 1) {
      c->bad = true;
      return;
    }
    s.kind = SUB_SEQUENCES;
    s.origin = st;
    s.array = st + 6;
    s.delta = 0;
    coverage_each(c, st + cu16(c, st + 2), visit_substitute, &s);
    break;
  case 4:
    if (cu16(c, st) != 1) {
      c->bad = true;
      return;
    }
    s.kind = SUB_LIGATURES;
    s.origin = st;
    s.array = st + 6;
    s.delta = 0;
    coverage_each(c, st + cu16(c, st + 2), visit_substitute, &s);
    break;
  case 5:
    context_subtable(c, st, false);
    break;
  case 6:
    context_subtable(c, st, true);
    break;
  case 8:
    reverse_subtable(c, st);
    break;
  default:
    c->bad = true;
    break;
  }
}

static void close_lookup(GFNT_Closure * c, uint32_t index) {
  size_t lookup = c->lookup_list + cu16(c, c->lookup_list + 2 + 2 * (size_t)index);
  uint16_t type = cu16(c, lookup);
  uint16_t subtables = cu16(c, lookup + 4);
  uint16_t extension = c->is_gpos ? 9 : 7;
  uint16_t i;

  for (i = 0; i < subtables && !c->bad; ++i) {
    size_t st = lookup + cu16(c, lookup + 6 + 2 * (size_t)i);
    uint16_t effective = type;

    if (type == extension) {
      uint32_t offset;

      if (cu16(c, st) != 1) {
        c->bad = true;
        return;
      }
      effective = cu16(c, st + 2);
      offset = gfnt_lr_u32(c->r, st + 4, &c->bad);
      st += offset;
    }
    if (c->bad) {
      return;
    }
    if (!c->is_gpos) {
      close_subtable(c, effective, st);
    }
    else if (effective == 7) {
      context_subtable(c, st, false);
    }
    else if (effective == 8) {
      context_subtable(c, st, true);
    }
  }
}

/* --- which lookups the features name ------------------------------------- */

static bool tag_wanted(const GFNT_Tag * features, size_t count, GFNT_Tag tag) {
  size_t i;

  if (!features || count == 0) {
    return true;
  }
  for (i = 0; i < count; ++i) {
    if (features[i] == tag) {
      return true;
    }
  }
  return false;
}

/** Activate the lookups of a Feature table (or an alternate of one). */
static void activate_feature(GFNT_Closure * c, size_t feature) {
  uint16_t n = cu16(c, feature + 2);
  uint16_t i;

  for (i = 0; i < n && !c->bad; ++i) {
    activate(c, cu16(c, feature + 4 + 2 * (size_t)i));
  }
}

static void activate_features(GFNT_Closure * c, const GFNT_LayoutTable * lt,
    const GFNT_Tag * features, size_t feature_count) {
  size_t list = lt->feature_list;
  uint16_t n = cu16(c, list);
  uint16_t i;

  for (i = 0; i < n && !c->bad; ++i) {
    size_t record = list + 2 + 6 * (size_t)i;
    uint32_t tag = gfnt_lr_u32(c->r, record, &c->bad);

    if (!c->bad && tag_wanted(features, feature_count, tag)) {
      activate_feature(c, list + cu16(c, record + 4));
    }
  }
  if (lt->variations) {
    size_t v = lt->variations;
    uint32_t records = gfnt_lr_u32(c->r, v + 4, &c->bad);
    uint32_t r;

    for (r = 0; r < records && !c->bad; ++r) {
      uint32_t sub = gfnt_lr_u32(c->r, v + 8 + 8 * (size_t)r + 4, &c->bad);
      size_t table;
      uint16_t subs;
      uint16_t k;

      if (c->bad || !sub) {
        continue;
      }
      table = v + sub;
      subs = cu16(c, table + 4);
      for (k = 0; k < subs && !c->bad; ++k) {
        size_t rec = table + 6 + 6 * (size_t)k;
        uint16_t index = cu16(c, rec);
        uint32_t off = gfnt_lr_u32(c->r, rec + 2, &c->bad);
        uint32_t tag = gfnt_lr_u32(c->r, list + 2 + 6 * (size_t)index, &c->bad);

        if (!c->bad && index < n && tag_wanted(features, feature_count, tag)) {
          activate_feature(c, table + off);
        }
      }
    }
  }
}

GFNT_Result gfnt_subset_layout_scan(const GFNT_Face * face, GFNT_Tag tag,
    uint8_t * keep, size_t count, bool grow, const GFNT_Tag * features,
    size_t feature_count, const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    uint8_t ** out_active, uint32_t * out_count, GFNT_Error * error) {
  GFNT_LayoutTable lt;
  GFNT_Closure c;
  GFNT_Result result;
  size_t rounds = 0;
  uint32_t i;

  if (out_active) {
    *out_active = NULL;
  }
  if (out_count) {
    *out_count = 0;
  }
  if (!gfnt_face_has_table(face, tag)) {
    return GFNT_OK;
  }
  result = gfnt_layout_open(face, tag, &lt, error);
  if (result != GFNT_OK) {
    return result;
  }
  memset(&c, 0, sizeof c);
  c.r = &lt.table;
  c.keep = keep;
  c.count = count;
  c.grow = grow;
  c.is_gpos = tag == GFNT_TAG_GPOS;
  c.lookup_list = lt.lookup_list;
  c.lookup_count = cu16(&c, lt.lookup_list);
  if (c.bad) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0,
        GFNT_GLYPH_NONE, "the lookup list cannot be read");
  }
  if (c.lookup_count == 0) {
    return GFNT_OK;
  }
  c.active = allocator->calloc_fn(allocator->ctx, c.lookup_count, 1);
  if (!c.active) {
    return gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE,
        "allocating the active lookups");
  }
  activate_features(&c, &lt, features, feature_count);
  // Every round either adds a glyph or activates a lookup, both bounded, so this
  // ends; the cap is for the time a hostile table could make it take.
  do {
    c.changed = false;
    for (i = 0; i < c.lookup_count && !c.bad; ++i) {
      if (c.active[i]) {
        close_lookup(&c, i);
      }
    }
    if (++rounds > limits->max_lookup_depth * 1024u + count) {
      allocator->free_fn(allocator->ctx, c.active);
      return gfnt_error_set(error, GFNT_ERR_LIMIT, tag, 0,
          GFNT_GLYPH_NONE, "the layout closure did not settle");
    }
  } while (c.changed && !c.bad);
  if (c.bad) {
    allocator->free_fn(allocator->ctx, c.active);
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0,
        GFNT_GLYPH_NONE, "a layout subtable cannot be read");
  }
  if (out_active) {
    *out_active = c.active;
    if (out_count) {
      *out_count = c.lookup_count;
    }
  }
  else {
    allocator->free_fn(allocator->ctx, c.active);
  }
  return GFNT_OK;
}

GFNT_Result gfnt_subset_gsub_closure(const GFNT_Face * face, uint8_t * keep,
    size_t count, const GFNT_Tag * features, size_t feature_count,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Error * error) {
  return gfnt_subset_layout_scan(face, GFNT_TAG_GSUB, keep, count, true, features,
      feature_count, limits, allocator, NULL, NULL, error);
}
