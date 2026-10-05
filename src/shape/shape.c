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
 * The default shaper: the plan (which features, which lookups, in what order) and
 * the pipeline that runs it. See shape.h for what is and is not done.
 */

#include <ghoti.io/font/featurevar.h>
#include <ghoti.io/font/shape.h>
#include <stdlib.h>
#include <string.h>
#include "../layout_tables/layout.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"
#include "../var/var.h"
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/metrics.h>

#define GFNT_PF_GLOBAL 0x01u
#define GFNT_PF_MANUAL_ZWNJ 0x04u
#define GFNT_PF_MANUAL_ZWJ 0x08u

#define GFNT_NO_FEATURE 0xFFFFu

#define GFNT_TAG_DFLT GFNT_TAG('D', 'F', 'L', 'T')
#define GFNT_TAG_dflt GFNT_TAG('d', 'f', 'l', 't')
#define GFNT_TAG_latn GFNT_TAG('l', 'a', 't', 'n')

/** A feature the plan wants, before its bits are allocated. */
typedef struct GFNT_PlanFeature {
  GFNT_Tag tag;
  uint32_t flags;
  uint32_t default_value;
  uint32_t max_value;
  uint32_t seq;
  uint32_t shift;
  uint32_t mask;
  uint8_t stage;
} GFNT_PlanFeature;

/** One lookup to run: which, on which glyphs, in which pass. */
typedef struct GFNT_PlanLookup {
  uint32_t index;
  uint32_t mask;
  uint8_t stage;
  bool auto_zwnj;
  bool auto_zwj;
} GFNT_PlanLookup;

typedef struct GFNT_PlanTable {
  GFNT_PlanLookup * lookups;
  size_t count;
  size_t capacity;
  GFNT_Tag script;
  GFNT_Tag language;
  bool present;
  bool kern_found;   ///< GPOS only: the `kern` feature is in the language system.
} GFNT_PlanTable;

/**
 * Grow a vector of @p size-byte elements to hold @p needed, returning the new
 * storage, or NULL if there is none to be had (the old storage is then still
 * the caller's).
 */
static void * gfnt_vec_grow(const GFNT_Allocator * a, void * data,
    size_t * capacity, size_t needed, size_t size) {
  size_t wanted;
  void * grown;

  if (needed <= *capacity) {
    return data;
  }
  wanted = *capacity ? *capacity * 2 : 16;
  while (wanted < needed) {
    wanted *= 2;
  }
  grown = a->realloc_fn(a->ctx, data, wanted * size);
  if (grown) {
    *capacity = wanted;
  }
  return grown;
}

/* --- Unicode properties the default shaper needs ------------------------ */

/**
 * Whether a code point is default-ignorable, as HarfBuzz's table has it.
 *
 * Such a character has no visible form of its own and must not break a context:
 * the lookups skip it, and the shaper hides it and gives it no advance.
 */
static bool gfnt_default_ignorable(uint32_t u) {
  if (u < 0xAD) {
    return false;
  }
  return u == 0x00AD || u == 0x034F || u == 0x061C
      || (u >= 0x115F && u <= 0x1160) || (u >= 0x17B4 && u <= 0x17B5)
      || (u >= 0x180B && u <= 0x180F) || (u >= 0x200B && u <= 0x200F)
      || (u >= 0x202A && u <= 0x202E) || (u >= 0x2060 && u <= 0x206F)
      || u == 0x3164 || (u >= 0xFE00 && u <= 0xFE0F) || u == 0xFEFF
      || u == 0xFFA0 || (u >= 0xFFF0 && u <= 0xFFF8)
      || (u >= 0x1BCA0 && u <= 0x1BCA3) || (u >= 0x1D173 && u <= 0x1D17A)
      || (u >= 0xE0000 && u <= 0xE0FFF);
}

/**
 * Whether a code point is a combining mark, for a font whose `GDEF` has no glyph
 * classes. A coarse table of the blocks that are marks throughout: it stands in
 * for the general category, which this library does not carry.
 */
static bool gfnt_unicode_mark(uint32_t u) {
  return (u >= 0x0300 && u <= 0x036F) || (u >= 0x0483 && u <= 0x0489)
      || (u >= 0x0591 && u <= 0x05BD) || u == 0x05BF
      || (u >= 0x05C1 && u <= 0x05C2) || (u >= 0x05C4 && u <= 0x05C5)
      || u == 0x05C7 || (u >= 0x0610 && u <= 0x061A)
      || (u >= 0x064B && u <= 0x065F) || u == 0x0670
      || (u >= 0x06D6 && u <= 0x06DC) || (u >= 0x06DF && u <= 0x06E4)
      || (u >= 0x1AB0 && u <= 0x1AFF) || (u >= 0x1DC0 && u <= 0x1DFF)
      || (u >= 0x20D0 && u <= 0x20FF) || (u >= 0xFE20 && u <= 0xFE2F);
}

/** Scripts written right to left, by their OpenType tag. */
static bool gfnt_script_native_rtl(GFNT_Tag script) {
  static const GFNT_Tag rtl[] = {
    GFNT_TAG('a', 'r', 'a', 'b'), GFNT_TAG('h', 'e', 'b', 'r'),
    GFNT_TAG('s', 'y', 'r', 'c'), GFNT_TAG('t', 'h', 'a', 'a'),
    GFNT_TAG('n', 'k', 'o', ' '), GFNT_TAG('a', 'd', 'l', 'm'),
    GFNT_TAG('m', 'a', 'n', 'd'), GFNT_TAG('m', 'e', 'n', 'd'),
    GFNT_TAG('s', 'a', 'm', 'r'), GFNT_TAG('p', 'h', 'n', 'x'),
    GFNT_TAG('r', 'o', 'h', 'g'), GFNT_TAG('h', 'a', 't', 'r'),
  };
  size_t i;

  for (i = 0; i < sizeof rtl / sizeof rtl[0]; i++) {
    if (rtl[i] == script) {
      return true;
    }
  }
  return false;
}

/* --- script, language and feature selection ------------------------------ */

static GFNT_LApply gfnt_plan_context(const GFNT_LayoutTable * lt,
    const GFNT_Gdef * gdef) {
  GFNT_LApply c;

  memset(&c, 0, sizeof c);
  c.lt = lt;
  c.gdef = gdef;
  return c;
}

/** The offset of a script table, or 0; and the tag that was found. */
static size_t gfnt_select_script(GFNT_LApply * c, GFNT_Tag requested,
    GFNT_Tag * chosen) {
  const GFNT_LayoutTable * lt = c->lt;
  GFNT_Tag tries[4];
  size_t tried = 0;
  size_t i;
  uint32_t count;

  if (!lt->script_list) {
    return 0;
  }
  if (requested) {
    tries[tried++] = requested;
  }
  tries[tried++] = GFNT_TAG_DFLT;
  tries[tried++] = GFNT_TAG_dflt;
  tries[tried++] = GFNT_TAG_latn;
  count = gfnt_lu16(c, lt->script_list);
  for (i = 0; i < tried; i++) {
    uint32_t k;

    for (k = 0; k < count && !c->fault.bad; k++) {
      size_t record = lt->script_list + 2 + 6 * (size_t)k;

      if (gfnt_lu32(c, record) == tries[i]) {
        *chosen = tries[i];
        return gfnt_l_rel(lt->script_list, gfnt_lu16(c, record + 4));
      }
    }
  }
  return 0;
}

/** The offset of a LangSys: the requested one, else 'dflt', else the default. */
static size_t gfnt_select_langsys(GFNT_LApply * c, size_t script,
    GFNT_Tag requested, GFNT_Tag * chosen) {
  uint32_t count = gfnt_lu16(c, script + 2);
  GFNT_Tag tries[2];
  size_t tried = 0;
  size_t i;

  if (requested) {
    tries[tried++] = requested;
  }
  tries[tried++] = GFNT_TAG_dflt;
  for (i = 0; i < tried; i++) {
    uint32_t k;

    for (k = 0; k < count && !c->fault.bad; k++) {
      size_t record = script + 4 + 6 * (size_t)k;

      if (gfnt_lu32(c, record) == tries[i]) {
        *chosen = tries[i];
        return gfnt_l_rel(script, gfnt_lu16(c, record + 4));
      }
    }
  }
  *chosen = 0;
  return gfnt_l_rel(script, gfnt_lu16(c, script));
}

/** The feature index with this tag in the LangSys, or GFNT_NO_FEATURE. */
static uint32_t gfnt_find_feature(GFNT_LApply * c, size_t langsys,
    GFNT_Tag tag) {
  const GFNT_LayoutTable * lt = c->lt;
  uint32_t count;
  uint32_t i;

  if (!langsys) {
    return GFNT_NO_FEATURE;
  }
  count = gfnt_lu16(c, langsys + 4);
  for (i = 0; i < count && !c->fault.bad; i++) {
    uint32_t index = gfnt_lu16(c, langsys + 6 + 2 * (size_t)i);

    if (index < lt->feature_count
        && gfnt_lu32(c, lt->feature_list + 2 + 6 * (size_t)index) == tag) {
      return index;
    }
  }
  return GFNT_NO_FEATURE;
}

/** Add a feature's lookups to the table's list. */
static bool gfnt_plan_add_lookups(GFNT_LApply * c, const GFNT_Face * face,
    GFNT_PlanTable * pt, const GFNT_Allocator * a, uint32_t feature,
    size_t record, bool have_record, uint32_t mask, uint8_t stage,
    bool auto_zwnj, bool auto_zwj) {
  const GFNT_LayoutTable * lt = c->lt;
  size_t table;
  uint32_t count;
  uint32_t i;
  size_t substitutions = 0;
  size_t j;
  void * grown;

  if (feature >= lt->feature_count) {
    return true;
  }
  // A FeatureVariations record that substitutes this feature replaces its
  // lookups wholesale.
  if (have_record
      && gfnt_face_feature_substitution_count(face, lt->tag, record,
             &substitutions, NULL) == GFNT_OK) {
    for (j = 0; j < substitutions; j++) {
      uint16_t replaced = 0;
      size_t lookups = 0;
      size_t k;

      if (gfnt_face_feature_substitution_at(face, lt->tag, record, j, &replaced,
              &lookups, NULL) != GFNT_OK || replaced != feature) {
        continue;
      }
      for (k = 0; k < lookups; k++) {
        uint16_t lookup = 0;

        if (gfnt_face_feature_substitution_lookup(face, lt->tag, record, j, k,
                &lookup, NULL) != GFNT_OK) {
          continue;
        }
        grown = gfnt_vec_grow(a, pt->lookups, &pt->capacity, pt->count + 1,
            sizeof *pt->lookups);
        if (!grown) {
          return false;
        }
        pt->lookups = grown;
        pt->lookups[pt->count++] = (GFNT_PlanLookup){lookup, mask, stage,
            auto_zwnj, auto_zwj};
      }
      return true;
    }
  }
  table = gfnt_l_rel(lt->feature_list,
      gfnt_lu16(c, lt->feature_list + 2 + 6 * (size_t)feature + 4));
  if (!table) {
    return true;
  }
  count = gfnt_lu16(c, table + 2);
  for (i = 0; i < count && !c->fault.bad; i++) {
    uint32_t lookup = gfnt_lu16(c, table + 4 + 2 * (size_t)i);

    grown = gfnt_vec_grow(a, pt->lookups, &pt->capacity, pt->count + 1,
        sizeof *pt->lookups);
    if (!grown) {
      return false;
    }
    pt->lookups = grown;
    pt->lookups[pt->count++] = (GFNT_PlanLookup){lookup, mask, stage,
        auto_zwnj, auto_zwj};
  }
  return true;
}

static int gfnt_plan_lookup_compare(const void * left, const void * right) {
  const GFNT_PlanLookup * a = left;
  const GFNT_PlanLookup * b = right;

  if (a->stage != b->stage) {
    return a->stage < b->stage ? -1 : 1;
  }
  if (a->index != b->index) {
    return a->index < b->index ? -1 : 1;
  }
  return 0;
}

/** Sort the lookups into running order and merge those named twice. */
static void gfnt_plan_finish_table(GFNT_PlanTable * pt) {
  size_t i;
  size_t j = 0;

  if (!pt->count) {
    return;
  }
  qsort(pt->lookups, pt->count, sizeof *pt->lookups, gfnt_plan_lookup_compare);
  for (i = 1; i < pt->count; i++) {
    if (pt->lookups[i].index == pt->lookups[j].index
        && pt->lookups[i].stage == pt->lookups[j].stage) {
      pt->lookups[j].mask |= pt->lookups[i].mask;
      pt->lookups[j].auto_zwnj = pt->lookups[j].auto_zwnj
          && pt->lookups[i].auto_zwnj;
      pt->lookups[j].auto_zwj = pt->lookups[j].auto_zwj
          && pt->lookups[i].auto_zwj;
    }
    else {
      pt->lookups[++j] = pt->lookups[i];
    }
  }
  pt->count = j + 1;
}

static int gfnt_plan_feature_compare(const void * left, const void * right) {
  const GFNT_PlanFeature * a = left;
  const GFNT_PlanFeature * b = right;

  if (a->tag != b->tag) {
    return a->tag < b->tag ? -1 : 1;
  }
  if (a->seq != b->seq) {
    return a->seq < b->seq ? -1 : 1;
  }
  return 0;
}

static uint32_t gfnt_bit_storage(uint32_t value) {
  uint32_t bits = 0;

  while (value) {
    bits++;
    value >>= 1;
  }
  return bits;
}

/** The plan: the features with their bits, and the lookups of each table. */
typedef struct GFNT_Plan {
  GFNT_PlanFeature * features;
  size_t feature_count;
  size_t feature_capacity;
  uint32_t global_mask;
  uint32_t kern_mask;       ///< The `kern` feature's bits, or 0 when it is off.
  bool kern_fallback;       ///< `GPOS` does not kern, so the `kern` table does.
  GFNT_PlanTable tables[2]; ///< GSUB, GPOS.
} GFNT_Plan;

static void gfnt_plan_free(GFNT_Plan * plan, const GFNT_Allocator * a) {
  a->free_fn(a->ctx, plan->features);
  a->free_fn(a->ctx, plan->tables[0].lookups);
  a->free_fn(a->ctx, plan->tables[1].lookups);
  memset(plan, 0, sizeof *plan);
}

static bool gfnt_plan_add_feature(GFNT_Plan * plan, const GFNT_Allocator * a,
    GFNT_Tag tag, uint32_t flags, uint32_t value, uint8_t stage) {
  GFNT_PlanFeature * f;
  void * grown;

  grown = gfnt_vec_grow(a, plan->features, &plan->feature_capacity,
      plan->feature_count + 1, sizeof *plan->features);
  if (!grown) {
    return false;
  }
  plan->features = grown;
  f = &plan->features[plan->feature_count];
  memset(f, 0, sizeof *f);
  f->tag = tag;
  f->flags = flags;
  f->default_value = (flags & GFNT_PF_GLOBAL) ? value : 0;
  f->max_value = value;
  f->seq = (uint32_t)plan->feature_count;
  f->stage = stage;
  plan->feature_count++;
  return true;
}

/**
 * Sort the features by tag and merge duplicates, then give each a mask.
 *
 * A later global request for a tag overrides the earlier one - which is how
 * `-liga` turns a default off - and a ranged request keeps the default for the
 * rest of the run and adds the value inside its range. A feature whose maximum
 * value is 0 is off and gets no bits and no lookups.
 */
static void gfnt_plan_compile_features(GFNT_Plan * plan) {
  size_t i;
  size_t j = 0;
  uint32_t next_bit = 1;

  qsort(plan->features, plan->feature_count, sizeof *plan->features,
      gfnt_plan_feature_compare);
  for (i = 1; i < plan->feature_count; i++) {
    GFNT_PlanFeature * a = &plan->features[j];
    const GFNT_PlanFeature * b = &plan->features[i];

    if (b->tag != a->tag) {
      plan->features[++j] = *b;
      continue;
    }
    if (b->flags & GFNT_PF_GLOBAL) {
      a->flags |= GFNT_PF_GLOBAL;
      a->max_value = b->max_value;
      a->default_value = b->default_value;
    }
    else {
      a->flags &= ~GFNT_PF_GLOBAL;
      if (b->max_value > a->max_value) {
        a->max_value = b->max_value;
      }
    }
    if (b->stage < a->stage) {
      a->stage = b->stage;
    }
  }
  if (plan->feature_count) {
    plan->feature_count = j + 1;
  }
  plan->global_mask = 1;
  for (i = 0; i < plan->feature_count; i++) {
    GFNT_PlanFeature * f = &plan->features[i];
    uint32_t bits = (f->flags & GFNT_PF_GLOBAL) && f->max_value == 1
        ? 0 : gfnt_bit_storage(f->max_value);

    f->mask = 0;
    if (!f->max_value || next_bit + bits >= 31) {
      continue;
    }
    if (bits == 0) {
      f->shift = 0;
      f->mask = 1;
    }
    else {
      f->shift = next_bit;
      f->mask = (1u << (next_bit + bits)) - (1u << next_bit);
      next_bit += bits;
      plan->global_mask |= (f->default_value << f->shift) & f->mask;
    }
  }
}

static const GFNT_PlanFeature * gfnt_plan_find(const GFNT_Plan * plan,
    GFNT_Tag tag) {
  size_t i;

  for (i = 0; i < plan->feature_count; i++) {
    if (plan->features[i].tag == tag) {
      return &plan->features[i];
    }
  }
  return NULL;
}

static GFNT_Result gfnt_plan_build(const GFNT_Face * face,
    const GFNT_ShapeOptions * options, size_t count, GFNT_Plan * plan,
    const GFNT_Allocator * a, GFNT_Error * error) {
  static const GFNT_Tag common[] = {
    GFNT_TAG('a', 'b', 'v', 'm'), GFNT_TAG('b', 'l', 'w', 'm'),
    GFNT_TAG('c', 'c', 'm', 'p'), GFNT_TAG('l', 'o', 'c', 'l'),
    GFNT_TAG('m', 'a', 'r', 'k'), GFNT_TAG('m', 'k', 'm', 'k'),
    GFNT_TAG('r', 'l', 'i', 'g'),
    GFNT_TAG('c', 'a', 'l', 't'), GFNT_TAG('c', 'l', 'i', 'g'),
    GFNT_TAG('c', 'u', 'r', 's'), GFNT_TAG('d', 'i', 's', 't'),
    GFNT_TAG('k', 'e', 'r', 'n'), GFNT_TAG('l', 'i', 'g', 'a'),
    GFNT_TAG('r', 'c', 'l', 't'),
  };
  size_t i;
  size_t t;
  const GFNT_Tag tables[2] = {GFNT_TAG_GSUB, GFNT_TAG_GPOS};

  memset(plan, 0, sizeof *plan);
  if (!gfnt_plan_add_feature(plan, a, GFNT_TAG('r', 'v', 'r', 'n'),
          GFNT_PF_GLOBAL, 1, 0)) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the shaping plan");
  }
  if (options->direction == GFNT_DIRECTION_RTL) {
    (void)(gfnt_plan_add_feature(plan, a, GFNT_TAG('r', 't', 'l', 'a'),
               GFNT_PF_GLOBAL, 1, 1)
        && gfnt_plan_add_feature(plan, a, GFNT_TAG('r', 't', 'l', 'm'),
               GFNT_PF_GLOBAL, 1, 1));
  }
  else {
    (void)(gfnt_plan_add_feature(plan, a, GFNT_TAG('l', 't', 'r', 'a'),
               GFNT_PF_GLOBAL, 1, 1)
        && gfnt_plan_add_feature(plan, a, GFNT_TAG('l', 't', 'r', 'm'),
               GFNT_PF_GLOBAL, 1, 1));
  }
  (void)gfnt_plan_add_feature(plan, a, GFNT_TAG('t', 'r', 'a', 'k'),
      GFNT_PF_GLOBAL, 1, 1);
  for (i = 0; i < sizeof common / sizeof common[0]; i++) {
    uint32_t flags = GFNT_PF_GLOBAL;

    // `mark` and `mkmk` manage joiners themselves, so a ZWJ or ZWNJ between a
    // base and its marks does not break the attachment.
    if (common[i] == GFNT_TAG('m', 'a', 'r', 'k')
        || common[i] == GFNT_TAG('m', 'k', 'm', 'k')) {
      flags |= GFNT_PF_MANUAL_ZWNJ | GFNT_PF_MANUAL_ZWJ;
    }
    if (!gfnt_plan_add_feature(plan, a, common[i], flags, 1, 1)) {
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory for the shaping plan");
    }
  }
  for (i = 0; i < options->feature_count; i++) {
    const GFNT_ShapeFeature * f = &options->features[i];
    bool whole = f->start == 0 && (f->end == GFNT_SHAPE_END || f->end >= count);

    if (f->end != GFNT_SHAPE_END && f->end < f->start) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "a feature's range ends before it starts");
    }
    if (!gfnt_plan_add_feature(plan, a, f->tag, whole ? GFNT_PF_GLOBAL : 0,
            f->value, 1)) {
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory for the shaping plan");
    }
  }
  gfnt_plan_compile_features(plan);
  {
    const GFNT_PlanFeature * kern = gfnt_plan_find(plan, GFNT_TAG('k', 'e', 'r', 'n'));

    plan->kern_mask = kern ? kern->mask : 0;
  }

  for (t = 0; t < 2; t++) {
    GFNT_LayoutTable lt;
    GFNT_Gdef gdef;
    GFNT_LApply c;
    GFNT_PlanTable * pt = &plan->tables[t];
    GFNT_Result result;
    size_t script;
    size_t langsys;
    size_t record = 0;
    bool have_record = false;

    if (!gfnt_face_has_table(face, tables[t])) {
      continue;
    }
    result = gfnt_layout_open(face, tables[t], &lt, error);
    if (result != GFNT_OK) {
      return result;
    }
    pt->present = true;
    gfnt_gdef_open(face, &gdef);
    c = gfnt_plan_context(&lt, &gdef);
    script = gfnt_select_script(&c, options->script, &pt->script);
    langsys = script ? gfnt_select_langsys(&c, script, options->language,
                           &pt->language) : 0;
    if (options->variation && options->variation->count && lt.variations) {
      size_t found = GFNT_FEATURE_VARIATIONS_NONE;

      result = gfnt_face_feature_variations_match(face, tables[t],
          options->variation->coords, options->variation->count, &found, error);
      if (result != GFNT_OK) {
        return result;
      }
      have_record = found != GFNT_FEATURE_VARIATIONS_NONE;
      record = found;
    }
    // The feature the language system requires always applies.
    if (langsys) {
      uint32_t required = gfnt_lu16(&c, langsys + 2);

      if (required != GFNT_NO_FEATURE
          && !gfnt_plan_add_lookups(&c, face, pt, a, required, record,
                 have_record, plan->global_mask, 1, true, true)) {
        return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
            "no memory for the shaping plan");
      }
    }
    for (i = 0; i < plan->feature_count; i++) {
      const GFNT_PlanFeature * f = &plan->features[i];
      uint32_t index;

      if (!f->mask) {
        continue;
      }
      index = langsys ? gfnt_find_feature(&c, langsys, f->tag) : GFNT_NO_FEATURE;
      if (f->tag == GFNT_TAG('k', 'e', 'r', 'n') && t == 1
          && index != GFNT_NO_FEATURE) {
        pt->kern_found = true;
      }
      if (index == GFNT_NO_FEATURE) {
        continue;
      }
      if (!gfnt_plan_add_lookups(&c, face, pt, a, index, record, have_record,
              f->mask, f->stage, !(f->flags & GFNT_PF_MANUAL_ZWNJ),
              !(f->flags & GFNT_PF_MANUAL_ZWJ))) {
        return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
            "no memory for the shaping plan");
      }
    }
    if (c.fault.bad) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, c.fault.table,
          c.fault.offset, GFNT_GLYPH_NONE,
          "a layout table's script or feature list reads past the table");
    }
    gfnt_plan_finish_table(pt);
  }
  // Kerning is the table's job when GPOS has none of its own: a font from before
  // OpenType layout, or one whose GPOS carries only marks.
  plan->kern_fallback = plan->kern_mask != 0 && !plan->tables[1].kern_found;
  return GFNT_OK;
}

/* --- the pipeline -------------------------------------------------------- */

void gfnt_shaped_run_free(GFNT_ShapedRun * run) {
  if (run && run->glyphs && run->allocator) {
    run->allocator->free_fn(run->allocator->ctx, run->glyphs);
  }
  if (run) {
    memset(run, 0, sizeof *run);
  }
}

/** Run one table's lookups, stage by stage. */
static GFNT_Result gfnt_shape_apply(GFNT_LApply * c, const GFNT_PlanTable * pt,
    GFNT_Error * error, size_t * out_ran) {
  size_t i;
  uint8_t stage;

  for (stage = 0; stage < 2; stage++) {
    for (i = 0; i < pt->count; i++) {
      const GFNT_PlanLookup * l = &pt->lookups[i];

      if (l->stage != stage) {
        continue;
      }
      c->lookup_mask = l->mask;
      c->auto_zwnj = l->auto_zwnj;
      c->auto_zwj = l->auto_zwj;
      gfnt_l_apply_lookup_to_buffer(c, l->index);
      (*out_ran)++;
      if (c->fault.bad) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, c->fault.table,
            c->fault.offset, GFNT_GLYPH_NONE,
            "a layout lookup reads past its own table");
      }
      if (c->buf->limit) {
        return gfnt_error_set(error, GFNT_ERR_LIMIT, c->lt->tag, 0,
            GFNT_GLYPH_NONE,
            "a lookup grew the run past sixty-four times its length");
      }
      if (c->buf->oom) {
        return gfnt_error_set(error, GFNT_ERR_OOM, c->lt->tag, 0,
            GFNT_GLYPH_NONE, "no memory while applying a layout lookup");
      }
    }
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_shape(const GFNT_Face * face, const uint32_t * codepoints,
    size_t count, const GFNT_ShapeOptions * options,
    const GFNT_Allocator * allocator, GFNT_ShapedRun * out_run,
    GFNT_Error * error) {
  static const GFNT_ShapeOptions defaults;
  GFNT_Plan plan;
  GFNT_LBuffer buf;
  GFNT_LayoutTable gsub_table;
  GFNT_LayoutTable gpos_table;
  GFNT_Gdef gdef;
  GFNT_LApply gsub;
  GFNT_LApply gpos;
  GFNT_Result result;
  GFNT_ShapedRun run;
  uint32_t space = 0;
  bool native_rtl;
  bool reversed_first;
  size_t i;

  gfnt_error_clear(error);
  if (!face || !out_run || (count && !codepoints)) {
    return GFNT_ERR_INVALID;
  }
  if (!options) {
    options = &defaults;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  if (count > GFNT_LAYOUT_MAX_LEN) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "a run longer than the shaper takes");
  }
  memset(&run, 0, sizeof run);
  run.allocator = allocator;
  if (count == 0) {
    *out_run = run;
    return GFNT_OK;
  }

  result = gfnt_plan_build(face, options, count, &plan, allocator, error);
  if (result != GFNT_OK) {
    gfnt_plan_free(&plan, allocator);
    return result;
  }
  if (!gfnt_lbuf_init(&buf, allocator, count)) {
    gfnt_plan_free(&plan, allocator);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the glyph buffer");
  }
  gfnt_gdef_open(face, &gdef);
  memset(&gsub, 0, sizeof gsub);
  memset(&gpos, 0, sizeof gpos);

  // Map the text. A character the font does not map is the notdef glyph, and
  // stays in the run, as it does in HarfBuzz.
  for (i = 0; i < count; i++) {
    uint32_t glyph = 0;
    GFNT_LInfo * info = &buf.info[i];

    result = gfnt_face_glyph_for_codepoint(face, codepoints[i], &glyph, error);
    if (result != GFNT_OK) {
      goto done;
    }
    info->glyph = glyph;
    info->cluster = (uint32_t)i;
    info->mask = plan.global_mask;
    info->unicode = codepoints[i];
    if (gfnt_default_ignorable(codepoints[i])) {
      info->flags |= GFNT_GF_DEFAULT_IGNORABLE;
    }
    if (codepoints[i] == 0x200D) {
      info->flags |= GFNT_GF_ZWJ;
    }
    if (codepoints[i] == 0x200C) {
      info->flags |= GFNT_GF_ZWNJ;
    }
  }
  (void)gfnt_face_glyph_for_codepoint(face, 0x20, &space, NULL);

  // A mark belongs to the cluster of the character it follows, and so does a ZWJ:
  // a grapheme is never split across clusters, so that a caret or a selection
  // cannot land between a letter and its accent.
  for (i = 1; i < count; i++) {
    if (gfnt_unicode_mark(codepoints[i]) || codepoints[i] == 0x200D) {
      buf.info[i].cluster = buf.info[i - 1].cluster;
    }
  }

  // Features with a range of their own: set their value on the code points in it.
  for (i = 0; i < options->feature_count; i++) {
    const GFNT_ShapeFeature * f = &options->features[i];
    const GFNT_PlanFeature * pf = gfnt_plan_find(&plan, f->tag);
    size_t k;

    if (!pf || !pf->mask || (pf->flags & GFNT_PF_GLOBAL)) {
      continue;
    }
    for (k = 0; k < count; k++) {
      if (buf.info[k].cluster >= f->start
          && (f->end == GFNT_SHAPE_END || buf.info[k].cluster < f->end)) {
        buf.info[k].mask = (buf.info[k].mask & ~pf->mask)
            | ((f->value << pf->shift) & pf->mask);
      }
    }
  }

  // The shaper works in the script's own direction. Text the other way is turned
  // round first, so that the lookups see it in reading order, and the run is
  // turned back at the end if it reads right to left.
  native_rtl = gfnt_script_native_rtl(options->script);
  reversed_first = (options->direction == GFNT_DIRECTION_RTL) != native_rtl;
  if (reversed_first) {
    gfnt_lbuf_reverse(&buf);
  }

  // GSUB.
  if (plan.tables[0].present) {
    gsub.buf = &buf;
    gsub.face = face;
    gsub.variation = options->variation;
    gsub.gdef = &gdef;
    gsub.is_gpos = false;
    gsub.rtl = native_rtl;
    result = gfnt_layout_open(face, GFNT_TAG_GSUB, &gsub_table, error);
    if (result != GFNT_OK) {
      goto done;
    }
    gsub.lt = &gsub_table;
  }
  // Glyph properties, from GDEF where it has classes, from the character where
  // it does not.
  {
    GFNT_LApply props;

    memset(&props, 0, sizeof props);
    props.gdef = &gdef;
    for (i = 0; i < buf.len; i++) {
      GFNT_LInfo * info = &buf.info[i];

      if (gdef.has_glyph_classes) {
        info->props = gfnt_gdef_props(&props, info->glyph);
      }
      else {
        info->props = gfnt_unicode_mark(info->unicode)
                && !(info->flags & GFNT_GF_DEFAULT_IGNORABLE)
            ? GFNT_PROP_MARK : GFNT_PROP_BASE;
      }
      info->lig_props = 0;
    }
  }
  if (plan.tables[0].present) {
    result = gfnt_shape_apply(&gsub, &plan.tables[0], error,
        &run.gsub_lookups);
    if (result != GFNT_OK) {
      goto done;
    }
  }

  // Default-ignorables: drawn as nothing. The space glyph stands in for them if
  // the font has one, so that the run keeps its length and its clusters; a font
  // with no space glyph has them taken out instead.
  {
    size_t kept = 0;

    for (i = 0; i < buf.len; i++) {
      GFNT_LInfo * info = &buf.info[i];

      if ((info->flags & GFNT_GF_DEFAULT_IGNORABLE)
          && !(info->props & GFNT_PROP_SUBSTITUTED)) {
        if (space) {
          info->glyph = space;
        }
        else {
          // Its cluster survives in a neighbour, or is merged into one.
          uint32_t cluster = info->cluster;

          if (i + 1 < buf.len && cluster == buf.info[i + 1].cluster) {
            continue;
          }
          if (kept) {
            if (cluster < buf.info[kept - 1].cluster) {
              uint32_t old = buf.info[kept - 1].cluster;
              size_t k;

              for (k = kept; k && buf.info[k - 1].cluster == old; k--) {
                buf.info[k - 1].cluster = cluster;
              }
            }
            continue;
          }
          if (i + 1 < buf.len) {
            buf.info[i + 1].cluster = cluster < buf.info[i + 1].cluster
                ? cluster : buf.info[i + 1].cluster;
            cluster = buf.info[i + 1].cluster;
            (void)cluster;
          }
          continue;
        }
      }
      if (kept != i) {
        buf.info[kept] = buf.info[i];
      }
      kept++;
    }
    buf.len = kept;
  }

  // Default advances.
  for (i = 0; i < buf.len; i++) {
    int32_t advance = 0;

    if (gfnt_face_glyph_advance(face, buf.info[i].glyph, options->variation,
            &advance, NULL) != GFNT_OK) {
      advance = 0;
    }
    // Every field, not only the advance: the array grew with the run and holds
    // whatever the allocator last had in it.
    memset(&buf.pos[i], 0, sizeof buf.pos[i]);
    buf.pos[i].x_advance = advance;
  }

  // GPOS.
  if (plan.tables[1].present) {
    gpos.buf = &buf;
    gpos.face = face;
    gpos.variation = options->variation;
    gpos.gdef = &gdef;
    gpos.is_gpos = true;
    gpos.rtl = native_rtl;
    result = gfnt_layout_open(face, GFNT_TAG_GPOS, &gpos_table, error);
    if (result != GFNT_OK) {
      goto done;
    }
    gpos.lt = &gpos_table;
    gfnt_gpos_position_start(&buf);
    result = gfnt_shape_apply(&gpos, &plan.tables[1], error,
        &run.gpos_lookups);
    if (result != GFNT_OK) {
      goto done;
    }
  }

  if (plan.kern_fallback) {
    result = gfnt_kern_apply(face, &buf, &gdef, plan.kern_mask, error);
    if (result != GFNT_OK) {
      goto done;
    }
  }

  // A mark takes no room of its own: its advance is folded into where it sits.
  // With a GPOS the font has already said where; without one, the mark is pulled
  // back over the base it follows.
  for (i = 0; i < buf.len; i++) {
    if (gfnt_l_is_mark(&buf.info[i])) {
      if (!plan.tables[1].present) {
        buf.pos[i].x_offset -= buf.pos[i].x_advance;
        buf.pos[i].y_offset -= buf.pos[i].y_advance;
      }
      buf.pos[i].x_advance = 0;
      buf.pos[i].y_advance = 0;
    }
  }
  for (i = 0; i < buf.len; i++) {
    if ((buf.info[i].flags & GFNT_GF_DEFAULT_IGNORABLE)
        && !(buf.info[i].props & GFNT_PROP_SUBSTITUTED)) {
      buf.pos[i].x_advance = 0;
      buf.pos[i].y_advance = 0;
      buf.pos[i].x_offset = 0;
      buf.pos[i].y_offset = 0;
    }
  }
  if (plan.tables[1].present) {
    gfnt_gpos_position_finish_offsets(&buf, native_rtl);
  }
  if (native_rtl) {
    gfnt_lbuf_reverse(&buf);
  }

  run.glyphs = allocator->calloc_fn(allocator->ctx, buf.len ? buf.len : 1,
      sizeof *run.glyphs);
  if (!run.glyphs) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the shaped run");
    goto done;
  }
  for (i = 0; i < buf.len; i++) {
    run.glyphs[i] = (GFNT_ShapedGlyph){buf.info[i].glyph, buf.info[i].cluster,
        buf.pos[i].x_advance, buf.pos[i].y_advance, buf.pos[i].x_offset,
        buf.pos[i].y_offset};
  }
  run.count = buf.len;
  run.script = plan.tables[0].present ? plan.tables[0].script
                                      : plan.tables[1].script;
  run.language = plan.tables[0].present ? plan.tables[0].language
                                        : plan.tables[1].language;
  *out_run = run;
  result = GFNT_OK;

done:
  gfnt_lbuf_free(&buf);
  gfnt_plan_free(&plan, allocator);
  return result;
}

/* --- the dump ------------------------------------------------------------ */

static void gfnt_tag_text(GFNT_Tag tag, char out[5]) {
  size_t i;

  for (i = 0; i < 4; i++) {
    unsigned char ch = (unsigned char)(tag >> (24 - 8 * i));

    out[i] = ch >= 0x20 && ch < 0x7F ? (char)ch : '?';
  }
  out[4] = 0;
}

GFNT_Result gfnt_face_layout_dump(const GFNT_Face * face, GFNT_Tag table,
    FILE * out) {
  GFNT_LayoutTable lt;
  GFNT_Gdef gdef;
  GFNT_LApply c;
  GFNT_Error error;
  GFNT_Result result;
  uint32_t i;
  uint32_t scripts;

  if (!out) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_layout_open(face, table, &lt, &error);
  if (result != GFNT_OK) {
    return result;
  }
  gfnt_gdef_open(face, &gdef);
  c = gfnt_plan_context(&lt, &gdef);
  scripts = lt.script_list ? gfnt_lu16(&c, lt.script_list) : 0;
  fprintf(out, "%s: %u scripts, %u features, %u lookups\n",
      table == GFNT_TAG_GSUB ? "GSUB" : "GPOS", scripts, lt.feature_count,
      lt.lookup_count);
  for (i = 0; i < scripts && !c.fault.bad; i++) {
    size_t record = lt.script_list + 2 + 6 * (size_t)i;
    size_t script = gfnt_l_rel(lt.script_list, gfnt_lu16(&c, record + 4));
    char tag[5];
    uint32_t langs = script ? gfnt_lu16(&c, script + 2) : 0;
    uint32_t k;

    gfnt_tag_text(gfnt_lu32(&c, record), tag);
    fprintf(out, "  script %s: %u language systems%s\n", tag, langs,
        script && gfnt_lu16(&c, script) ? ", with a default" : "");
    for (k = 0; k < langs && !c.fault.bad; k++) {
      char lang[5];

      gfnt_tag_text(gfnt_lu32(&c, script + 4 + 6 * (size_t)k), lang);
      fprintf(out, "    language %s\n", lang);
    }
  }
  for (i = 0; i < lt.feature_count && !c.fault.bad; i++) {
    size_t record = lt.feature_list + 2 + 6 * (size_t)i;
    size_t feature = gfnt_l_rel(lt.feature_list, gfnt_lu16(&c, record + 4));
    char tag[5];
    uint32_t lookups = feature ? gfnt_lu16(&c, feature + 2) : 0;
    uint32_t k;

    gfnt_tag_text(gfnt_lu32(&c, record), tag);
    fprintf(out, "  feature %u %s:", i, tag);
    for (k = 0; k < lookups && !c.fault.bad; k++) {
      fprintf(out, " %u", gfnt_lu16(&c, feature + 4 + 2 * (size_t)k));
    }
    fputc('\n', out);
  }
  for (i = 0; i < lt.lookup_count && !c.fault.bad; i++) {
    size_t offset;
    uint16_t type;
    uint16_t flag;
    uint16_t subtables;

    if (gfnt_l_lookup(&c, i, &offset, &type, &flag, &subtables)) {
      fprintf(out, "  lookup %u: type %u flag 0x%04X, %u subtables\n", i, type,
          flag, subtables);
    }
  }
  if (c.fault.bad) {
    return GFNT_ERR_CORRUPT;
  }
  return GFNT_OK;
}
