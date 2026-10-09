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
 * The shaping plan. See plan.h.
 */

#include <ghoti.io/font/featurevar.h>
#include <stdlib.h>
#include <string.h>
#include "../layout_tables/layout.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"
#include "plan.h"

void * gfnt_vec_grow(const GFNT_Allocator * a, void * data, size_t * capacity,
    size_t needed, size_t size) {
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

/** Scripts written right to left, by their OpenType tag: HarfBuzz's list. */
bool gfnt_script_native_rtl(GFNT_Tag script) {
  static const GFNT_Tag rtl[] = {
    GFNT_TAG('a', 'r', 'a', 'b'), GFNT_TAG('h', 'e', 'b', 'r'),
    GFNT_TAG('s', 'y', 'r', 'c'), GFNT_TAG('t', 'h', 'a', 'a'),
    GFNT_TAG('c', 'p', 'r', 't'), GFNT_TAG('k', 'h', 'a', 'r'),
    GFNT_TAG('p', 'h', 'n', 'x'), GFNT_TAG('n', 'k', 'o', ' '),
    GFNT_TAG('l', 'y', 'd', 'i'), GFNT_TAG('a', 'v', 's', 't'),
    GFNT_TAG('a', 'r', 'm', 'i'), GFNT_TAG('p', 'h', 'l', 'i'),
    GFNT_TAG('p', 'r', 't', 'i'), GFNT_TAG('s', 'a', 'r', 'b'),
    GFNT_TAG('o', 'r', 'k', 'h'), GFNT_TAG('s', 'a', 'm', 'r'),
    GFNT_TAG('m', 'a', 'n', 'd'), GFNT_TAG('m', 'e', 'r', 'c'),
    GFNT_TAG('m', 'e', 'r', 'o'), GFNT_TAG('m', 'a', 'n', 'i'),
    GFNT_TAG('m', 'e', 'n', 'd'), GFNT_TAG('n', 'b', 'a', 't'),
    GFNT_TAG('n', 'a', 'r', 'b'), GFNT_TAG('p', 'a', 'l', 'm'),
    GFNT_TAG('p', 'h', 'l', 'p'), GFNT_TAG('h', 'a', 't', 'r'),
    GFNT_TAG('a', 'd', 'l', 'm'), GFNT_TAG('r', 'o', 'h', 'g'),
    GFNT_TAG('s', 'o', 'g', 'o'), GFNT_TAG('s', 'o', 'g', 'd'),
    GFNT_TAG('e', 'l', 'y', 'm'), GFNT_TAG('c', 'h', 'r', 's'),
    GFNT_TAG('y', 'e', 'z', 'i'), GFNT_TAG('o', 'u', 'g', 'r'),
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

/**
 * The OpenType tags a script is looked for under, in the order HarfBuzz tries
 * them: an Indic or Myanmar script has a new tag (`dev2`) and an old one
 * (`deva`), and the font is asked for the new one first.
 */
static size_t gfnt_script_tags(GFNT_Tag requested, GFNT_Tag out[2]) {
  static const GFNT_Tag pairs[][2] = {
    {GFNT_TAG('b', 'n', 'g', '2'), GFNT_TAG('b', 'e', 'n', 'g')},
    {GFNT_TAG('d', 'e', 'v', '2'), GFNT_TAG('d', 'e', 'v', 'a')},
    {GFNT_TAG('g', 'j', 'r', '2'), GFNT_TAG('g', 'u', 'j', 'r')},
    {GFNT_TAG('g', 'u', 'r', '2'), GFNT_TAG('g', 'u', 'r', 'u')},
    {GFNT_TAG('k', 'n', 'd', '2'), GFNT_TAG('k', 'n', 'd', 'a')},
    {GFNT_TAG('m', 'l', 'm', '2'), GFNT_TAG('m', 'l', 'y', 'm')},
    {GFNT_TAG('o', 'r', 'y', '2'), GFNT_TAG('o', 'r', 'y', 'a')},
    {GFNT_TAG('t', 'm', 'l', '2'), GFNT_TAG('t', 'a', 'm', 'l')},
    {GFNT_TAG('t', 'e', 'l', '2'), GFNT_TAG('t', 'e', 'l', 'u')},
    {GFNT_TAG('m', 'y', 'm', '2'), GFNT_TAG('m', 'y', 'm', 'r')},
  };
  size_t i;

  if (!requested) {
    return 0;
  }
  for (i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
    if (requested == pairs[i][0] || requested == pairs[i][1]) {
      out[0] = pairs[i][0];
      out[1] = pairs[i][1];
      return 2;
    }
  }
  out[0] = requested;
  return 1;
}

/**
 * The offset of a script table, or 0; the tag that was found; and whether it was
 * one the caller asked for and not one of the fallbacks.
 */
static size_t gfnt_select_script(GFNT_LApply * c, const GFNT_Tag * requested,
    size_t requested_count, GFNT_Tag * chosen, bool * exact) {
  const GFNT_LayoutTable * lt = c->lt;
  GFNT_Tag tries[8];
  size_t tried = 0;
  size_t i;
  uint32_t count;

  *exact = false;
  if (!lt->script_list) {
    return 0;
  }
  for (i = 0; i < requested_count && tried < 5; i++) {
    tries[tried++] = requested[i];
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
        *exact = i < requested_count;
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

/** The first feature in the table's list with this tag, or GFNT_NO_FEATURE. */
static uint32_t gfnt_find_feature_anywhere(GFNT_LApply * c, GFNT_Tag tag) {
  const GFNT_LayoutTable * lt = c->lt;
  uint32_t i;

  for (i = 0; i < lt->feature_count && !c->fault.bad; i++) {
    if (gfnt_lu32(c, lt->feature_list + 2 + 6 * (size_t)i) == tag) {
      return i;
    }
  }
  return GFNT_NO_FEATURE;
}

/** Add a feature's lookups to the table's list. */
static bool gfnt_plan_add_lookups(GFNT_LApply * c, const GFNT_Face * face,
    GFNT_PlanTable * pt, const GFNT_Allocator * a, uint32_t feature,
    size_t record, bool have_record, uint32_t mask, uint8_t stage,
    bool auto_zwnj, bool auto_zwj, bool per_syllable) {
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
        pt->lookups[pt->count] = (GFNT_PlanLookup){lookup, mask, stage,
            auto_zwnj, auto_zwj, per_syllable, (uint32_t)pt->count};
        pt->count++;
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
    pt->lookups[pt->count] = (GFNT_PlanLookup){lookup, mask, stage,
        auto_zwnj, auto_zwj, per_syllable, (uint32_t)pt->count};
    pt->count++;
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
  if (a->order != b->order) {
    return a->order < b->order ? -1 : 1;
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
      // Whether the lookup runs per syllable is the first feature's to say.
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

/** The `old` form of a script tag: `dev2` is `deva`. */
static GFNT_Tag gfnt_old_script_tag(GFNT_Tag tag) {
  GFNT_Tag tags[2];

  if (gfnt_script_tags(tag, tags) == 2) {
    return tags[1];
  }
  return tag;
}

void gfnt_plan_free(GFNT_Plan * plan) {
  const GFNT_Allocator * a = plan->allocator;

  if (!a) {
    return;
  }
  if (plan->shaper && plan->shaper->data_destroy && plan->shaper_data) {
    plan->shaper->data_destroy(plan->shaper_data, a);
  }
  a->free_fn(a->ctx, plan->features);
  a->free_fn(a->ctx, plan->tables[0].lookups);
  a->free_fn(a->ctx, plan->tables[1].lookups);
  memset(plan, 0, sizeof *plan);
}

static void gfnt_plan_add_feature(GFNT_Plan * plan, GFNT_Tag tag,
    uint32_t flags, uint32_t value) {
  GFNT_PlanFeature * f;
  void * grown;

  grown = gfnt_vec_grow(plan->allocator, plan->features,
      &plan->feature_capacity, plan->feature_count + 1, sizeof *plan->features);
  if (!grown) {
    plan->oom = true;
    return;
  }
  plan->features = grown;
  f = &plan->features[plan->feature_count];
  memset(f, 0, sizeof *f);
  f->tag = tag;
  f->flags = flags;
  f->default_value = (flags & GFNT_PF_GLOBAL) ? value : 0;
  f->max_value = value;
  f->seq = (uint32_t)plan->feature_count;
  f->stage = plan->gsub_stage;
  plan->feature_count++;
}

void gfnt_plan_enable(GFNT_Plan * plan, GFNT_Tag tag, uint32_t flags,
    uint32_t value) {
  gfnt_plan_add_feature(plan, tag, flags | GFNT_PF_GLOBAL, value);
}

void gfnt_plan_add(GFNT_Plan * plan, GFNT_Tag tag, uint32_t flags) {
  gfnt_plan_add_feature(plan, tag, flags & ~GFNT_PF_GLOBAL, 1);
}

void gfnt_plan_pause(GFNT_Plan * plan, GFNT_PauseFunc pause) {
  if (plan->gsub_stage + 1 >= GFNT_MAX_STAGES) {
    return;
  }
  plan->pause[plan->gsub_stage] = pause;
  plan->gsub_stage++;
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
    a->flags |= b->flags & GFNT_PF_HAS_FALLBACK;
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
    if (!f->max_value || next_bit + bits >= 31
        || (!f->found && !(f->flags & GFNT_PF_HAS_FALLBACK))) {
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


const GFNT_PlanFeature * gfnt_plan_find(const GFNT_Plan * plan,
    GFNT_Tag tag) {
  size_t i;

  for (i = 0; i < plan->feature_count; i++) {
    if (plan->features[i].tag == tag) {
      return &plan->features[i];
    }
  }
  return NULL;
}

/**
 * The feature's lowest bit: what a shaper sets on a glyph to switch the feature on
 * once (HarfBuzz's get_1_mask). A feature the caller gave a larger value to has
 * more bits, and setting them all would ask for an alternate that was not meant.
 */
static uint32_t gfnt_plan_first_bit(const GFNT_PlanFeature * f) {
  return (1u << f->shift) & f->mask;
}

uint32_t gfnt_plan_mask(const GFNT_Plan * plan, GFNT_Tag tag) {
  const GFNT_PlanFeature * f = gfnt_plan_find(plan, tag);

  return f ? gfnt_plan_first_bit(f) : 0;
}

uint32_t gfnt_plan_found_mask(const GFNT_Plan * plan, GFNT_Tag tag) {
  const GFNT_PlanFeature * f = gfnt_plan_find(plan, tag);

  return f && f->found ? gfnt_plan_first_bit(f) : 0;
}

bool gfnt_plan_would_substitute(const GFNT_ShapeCtx * ctx, GFNT_Tag tag,
    const uint32_t * glyphs, size_t count, bool zero_context) {
  const GFNT_PlanFeature * f = gfnt_plan_find(ctx->plan, tag);
  const GFNT_PlanTable * pt = &ctx->plan->tables[0];
  size_t i;

  if (!f || !f->found || !ctx->gsub || !pt->present) {
    return false;
  }
  for (i = 0; i < pt->count; i++) {
    if (pt->lookups[i].stage == f->stage
        && gfnt_l_would_apply(ctx->gsub, pt->lookups[i].index, glyphs, count,
            zero_context)) {
      return true;
    }
  }
  return false;
}

bool gfnt_plan_has_feature(const GFNT_Plan * plan, GFNT_Tag tag) {
  return gfnt_plan_find(plan, tag) != NULL;
}

/** What every run asks for before its script says anything: HarfBuzz's own list. */
static void gfnt_plan_collect_default(GFNT_Plan * plan) {
  static const GFNT_Tag common[] = {
    GFNT_TAG('a', 'b', 'v', 'm'), GFNT_TAG('b', 'l', 'w', 'm'),
    GFNT_TAG('c', 'c', 'm', 'p'), GFNT_TAG('l', 'o', 'c', 'l'),
    GFNT_TAG('m', 'a', 'r', 'k'), GFNT_TAG('m', 'k', 'm', 'k'),
    GFNT_TAG('r', 'l', 'i', 'g'),
  };
  static const GFNT_Tag horizontal[] = {
    GFNT_TAG('c', 'a', 'l', 't'), GFNT_TAG('c', 'l', 'i', 'g'),
    GFNT_TAG('c', 'u', 'r', 's'), GFNT_TAG('d', 'i', 's', 't'),
    GFNT_TAG('k', 'e', 'r', 'n'), GFNT_TAG('l', 'i', 'g', 'a'),
    GFNT_TAG('r', 'c', 'l', 't'),
  };
  size_t i;

  gfnt_plan_enable(plan, GFNT_TAG('r', 'v', 'r', 'n'), 0, 1);
  gfnt_plan_pause(plan, NULL);
  if (plan->options->direction == GFNT_DIRECTION_RTL) {
    gfnt_plan_enable(plan, GFNT_TAG('r', 't', 'l', 'a'), 0, 1);
    gfnt_plan_add(plan, GFNT_TAG('r', 't', 'l', 'm'), 0);
  }
  else if (plan->options->direction == GFNT_DIRECTION_LTR) {
    // Vertical text has neither pair.
    gfnt_plan_enable(plan, GFNT_TAG('l', 't', 'r', 'a'), 0, 1);
    gfnt_plan_enable(plan, GFNT_TAG('l', 't', 'r', 'm'), 0, 1);
  }
  // Automatic fractions: switched on around a FRACTION SLASH by the shaper.
  gfnt_plan_add(plan, GFNT_TAG('f', 'r', 'a', 'c'), 0);
  gfnt_plan_add(plan, GFNT_TAG('n', 'u', 'm', 'r'), 0);
  gfnt_plan_add(plan, GFNT_TAG('d', 'n', 'o', 'm'), 0);
  gfnt_plan_enable(plan, GFNT_TAG('t', 'r', 'a', 'k'), 0, 1);
  if (plan->shaper->collect_features) {
    plan->shaper->collect_features(plan);
  }
  for (i = 0; i < sizeof common / sizeof common[0]; i++) {
    uint32_t flags = 0;

    // `mark` and `mkmk` manage joiners themselves, so a ZWJ or ZWNJ between a
    // base and its marks does not break the attachment.
    if (common[i] == GFNT_TAG('m', 'a', 'r', 'k')
        || common[i] == GFNT_TAG('m', 'k', 'm', 'k')) {
      flags |= GFNT_PF_MANUAL_ZWNJ | GFNT_PF_MANUAL_ZWJ;
    }
    gfnt_plan_enable(plan, common[i], flags, 1);
  }
  if (plan->vertical) {
    // Only `vert`, and wherever the font keeps it.
    gfnt_plan_enable(plan, GFNT_TAG('v', 'e', 'r', 't'), GFNT_PF_GLOBAL_SEARCH, 1);
  }
  else {
    for (i = 0; i < sizeof horizontal / sizeof horizontal[0]; i++) {
      // Kerning has a fallback in the kern table, so `kern` keeps its mask though
      // the font has no such feature.
      gfnt_plan_enable(plan, horizontal[i],
          horizontal[i] == GFNT_TAG('k', 'e', 'r', 'n') ? GFNT_PF_HAS_FALLBACK : 0,
          1);
    }
  }
}

/** Per-table state kept from selecting the script to adding the lookups. */
typedef struct GFNT_TableState {
  GFNT_LayoutTable lt;
  GFNT_Gdef gdef;
  GFNT_LApply c;
  size_t script;
  size_t langsys;
  size_t record;
  bool have_record;
} GFNT_TableState;

GFNT_Result gfnt_plan_build(const GFNT_Face * face,
    const GFNT_ShapeOptions * options, size_t count, GFNT_Plan * plan,
    const GFNT_Allocator * a, GFNT_Error * error) {
  GFNT_TableState state[2];
  const GFNT_Tag tables[2] = {GFNT_TAG_GSUB, GFNT_TAG_GPOS};
  GFNT_Tag requested[2];
  size_t requested_count;
  size_t i;
  size_t t;
  GFNT_Result result;

  memset(plan, 0, sizeof *plan);
  memset(state, 0, sizeof state);
  plan->allocator = a;
  plan->options = options;
  plan->count = count;
  plan->script = gfnt_old_script_tag(options->script);
  requested_count = gfnt_script_tags(options->script, requested);

  // The tables, and which script and language system of each the run is shaped
  // by: the shaper is chosen by what the font has, as well as by the script.
  for (t = 0; t < 2; t++) {
    GFNT_PlanTable * pt = &plan->tables[t];
    GFNT_TableState * s = &state[t];

    if (!gfnt_face_has_table(face, tables[t])) {
      continue;
    }
    result = gfnt_layout_open(face, tables[t], &s->lt, error);
    if (result != GFNT_OK) {
      gfnt_plan_free(plan);
      return result;
    }
    pt->present = true;
    gfnt_gdef_open(face, &s->gdef);
    s->c = gfnt_plan_context(&s->lt, &s->gdef);
    s->script = gfnt_select_script(&s->c, requested, requested_count,
        &pt->script, &pt->script_found);
    s->langsys = s->script ? gfnt_select_langsys(&s->c, s->script,
                                 options->language, &pt->language) : 0;
    pt->script_offset = s->script;
    pt->langsys_offset = s->langsys;
    if (s->lt.variations) {
      // Even with no location: a variable font's default instance is the
      // location where every axis is zero, and a record may hold there.
      size_t found = GFNT_FEATURE_VARIATIONS_NONE;
      const GFNT_F2Dot14 * coords = options->variation
          ? options->variation->coords : NULL;
      size_t coord_count = options->variation && coords
          ? options->variation->count : 0;

      result = gfnt_face_feature_variations_match(face, tables[t], coords,
          coord_count, &found, error);
      if (result != GFNT_OK) {
        gfnt_plan_free(plan);
        return result;
      }
      s->have_record = found != GFNT_FEATURE_VARIATIONS_NONE;
      s->record = found;
    }
  }

  plan->vertical = options->direction == GFNT_DIRECTION_TTB
      || options->direction == GFNT_DIRECTION_BTT;
  // Vertical text has no native direction of its script: it is top to bottom.
  plan->native_rtl = !plan->vertical && gfnt_script_native_rtl(plan->script);
  plan->shaper = gfnt_shaper_select(plan->script, plan->tables[0].script,
      !plan->vertical);
  // Where a font carries Apple's substitution table it is used, and `GSUB` is
  // not: the font's own state machines make the forms, so no script's shaper (HarfBuzz issue 1528) is wanted.
  if (gfnt_morx_present(face, plan->vertical)) {
    // And the glyph classes the shaper would have made up from the characters
    // are not made either, for the scripts that had a shaper of their own.
    plan->no_synthetic_classes = plan->shaper != &gfnt_shaper_default;
    plan->shaper = &gfnt_shaper_default;
  }

  gfnt_plan_collect_default(plan);
  for (i = 0; i < options->feature_count; i++) {
    const GFNT_ShapeFeature * f = &options->features[i];
    bool whole = f->start == 0 && f->end == GFNT_SHAPE_END;

    if (f->end != GFNT_SHAPE_END && f->end < f->start) {
      gfnt_plan_free(plan);
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "a feature's range ends before it starts");
    }
    gfnt_plan_add_feature(plan, f->tag, whole ? GFNT_PF_GLOBAL : 0, f->value);
  }
  // After the caller's features, as HarfBuzz does it: a shaper that turns a
  // feature off (Khmer and Indic `liga`, Hangul `calt`) is not overruled by a
  // `+liga` in the caller's list.
  if (plan->shaper->override_features) {
    plan->shaper->override_features(plan);
  }
  if (plan->shaper->trailing_pause) {
    gfnt_plan_pause(plan, plan->shaper->trailing_pause);
  }
  if (plan->oom) {
    gfnt_plan_free(plan);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the shaping plan");
  }
  plan->gsub_stages = (size_t)plan->gsub_stage + 1;
  // A feature the font does not have in either table is given no mask bit, which is
  // what HarfBuzz does, and which matters to an alternate lookup that two features
  // share: it reads the feature's value out of the bits between them.
  for (i = 0; i < plan->feature_count; i++) {
    GFNT_PlanFeature * f = &plan->features[i];

    f->found = false;
    for (t = 0; t < 2; t++) {
      GFNT_TableState * s = &state[t];
      uint32_t index;

      if (!plan->tables[t].present) {
        continue;
      }
      index = s->langsys ? gfnt_find_feature(&s->c, s->langsys, f->tag)
                         : GFNT_NO_FEATURE;
      if (index == GFNT_NO_FEATURE && (f->flags & GFNT_PF_GLOBAL_SEARCH)) {
        index = gfnt_find_feature_anywhere(&s->c, f->tag);
      }
      if (index != GFNT_NO_FEATURE) {
        f->found = true;
      }
    }
  }
  gfnt_plan_compile_features(plan);
  {
    const GFNT_PlanFeature * kern = gfnt_plan_find(plan, plan->vertical
        ? GFNT_TAG('v', 'k', 'r', 'n') : GFNT_TAG('k', 'e', 'r', 'n'));

    plan->kern_mask = kern ? kern->mask : 0;
  }
  plan->frac_mask = gfnt_plan_mask(plan, GFNT_TAG('f', 'r', 'a', 'c'));
  plan->numr_mask = gfnt_plan_mask(plan, GFNT_TAG('n', 'u', 'm', 'r'));
  plan->dnom_mask = gfnt_plan_mask(plan, GFNT_TAG('d', 'n', 'o', 'm'));
  plan->has_frac = plan->frac_mask || (plan->numr_mask && plan->dnom_mask);

  for (t = 0; t < 2; t++) {
    GFNT_PlanTable * pt = &plan->tables[t];
    GFNT_TableState * s = &state[t];
    // `GPOS` has no pauses: everything it does is one stage.
    uint8_t table_stage = 0;

    if (!pt->present) {
      continue;
    }
    // The feature the language system requires always applies.
    if (s->langsys) {
      uint32_t required = gfnt_lu16(&s->c, s->langsys + 2);
      uint8_t required_stage = 0;

      // It runs in the stage of the requested feature that has its tag, if there is
      // one that is on (`-liga` is not a request for the stage), and in the first
      // stage if not.
      if (required != GFNT_NO_FEATURE && t == 0 && required < s->lt.feature_count) {
        GFNT_Tag required_tag = (GFNT_Tag)gfnt_lu32(&s->c,
            s->lt.feature_list + 2 + 6 * (size_t)required);

        for (i = 0; i < plan->feature_count; i++) {
          const GFNT_PlanFeature * f = &plan->features[i];

          if (f->tag == required_tag && f->max_value) {
            required_stage = f->stage;
          }
        }
      }
      if (required != GFNT_NO_FEATURE
          && !gfnt_plan_add_lookups(&s->c, face, pt, a, required, s->record,
                 s->have_record, 1u,
                 t == 0 ? required_stage : table_stage, true, true, false)) {
        gfnt_plan_free(plan);
        return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
            "no memory for the shaping plan");
      }
    }
    for (i = 0; i < plan->feature_count; i++) {
      GFNT_PlanFeature * f = &plan->features[i];
      uint32_t index;

      if (!f->mask) {
        continue;
      }
      index = s->langsys ? gfnt_find_feature(&s->c, s->langsys, f->tag)
                         : GFNT_NO_FEATURE;
      if (index == GFNT_NO_FEATURE && (f->flags & GFNT_PF_GLOBAL_SEARCH)) {
        index = gfnt_find_feature_anywhere(&s->c, f->tag);
      }
      if (index != GFNT_NO_FEATURE) {
        f->found = true;
      }
      if (f->tag == GFNT_TAG('k', 'e', 'r', 'n') && t == 1
          && index != GFNT_NO_FEATURE) {
        pt->kern_found = true;
      }
      if (f->tag == GFNT_TAG('m', 'a', 'r', 'k') && index != GFNT_NO_FEATURE) {
        plan->has_mark_feature = true;
      }
      if (index == GFNT_NO_FEATURE) {
        continue;
      }
      if (!gfnt_plan_add_lookups(&s->c, face, pt, a, index, s->record,
              s->have_record, f->mask, t == 0 ? f->stage : table_stage,
              !(f->flags & GFNT_PF_MANUAL_ZWNJ),
              !(f->flags & GFNT_PF_MANUAL_ZWJ),
              (f->flags & GFNT_PF_PER_SYLLABLE) != 0)) {
        gfnt_plan_free(plan);
        return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
            "no memory for the shaping plan");
      }
    }
    if (s->c.fault.bad) {
      gfnt_plan_free(plan);
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, s->c.fault.table,
          s->c.fault.offset, GFNT_GLYPH_NONE,
          "a layout table's script or feature list reads past the table");
    }
    gfnt_plan_finish_table(pt);
  }
  // A shaper may believe a font's positioning only if it names its own script. The
  // features were looked for all the same: a `mark` feature that goes unused still
  // says the font places its own marks.
  if (plan->shaper->gpos_tag && plan->tables[1].present
      && plan->tables[1].script != plan->shaper->gpos_tag) {
    plan->tables[1].present = false;
    plan->tables[1].kern_found = false;
  }
  // Apple's kerning replaces the font's own `GPOS`, and the `kern` table as well.
  // A font that has both a `GSUB` and a `GPOS` is positioned by the `GPOS`, which
  // HarfBuzz takes for an OpenType font (found with random fonts of each). In
  // horizontal text the `kerx` is then left out altogether; in vertical text, where
  // the `kerx` kerns nothing here anyway, marks still keep their widths for it.
  plan->gpos_over_kerx = gfnt_kerx_present(face)
      && gfnt_face_has_table(face, GFNT_TAG('G', 'S', 'U', 'B'))
      && gfnt_face_has_table(face, GFNT_TAG('G', 'P', 'O', 'S'));
  plan->apply_kerx = gfnt_kerx_present(face)
      && !(plan->gpos_over_kerx && !plan->vertical);
  if (plan->apply_kerx && !plan->gpos_over_kerx) {
    plan->tables[1].present = false;
    plan->tables[1].kern_found = false;
  }
  // Kerning is the table's job when GPOS has none of its own: a font from before
  // OpenType layout, or one whose GPOS carries only marks.
  plan->kern_fallback = !plan->vertical && plan->kern_mask != 0
      && !plan->tables[1].kern_found && !plan->apply_kerx;
  if (plan->shaper->data_create) {
    plan->shaper_data = plan->shaper->data_create(plan, a);
    if (!plan->shaper_data) {
      gfnt_plan_free(plan);
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory for the shaper's data");
    }
  }
  return GFNT_OK;
}
