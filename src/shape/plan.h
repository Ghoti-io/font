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
 * The shaping plan: which features a run asks for, which lookups they select in
 * the font, in what order, and which shaper decides the rest.
 *
 * This is HarfBuzz's `hb-ot-map` and the shaper interface around it, because the
 * order lookups run in is visible in the result. A feature is requested with a
 * *stage*: everything of one stage runs before anything of the next, and a
 * script's shaper may stop between stages to do work no lookup can (set which
 * glyphs a joining form applies to, reorder a syllable). Within a stage the
 * lookups run in the order the font lists them, not the order the features were
 * asked for.
 */

#ifndef GHOTI_IO_GFNT_PLAN_H
#define GHOTI_IO_GFNT_PLAN_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/shape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../layout_tables/layout.h"
#include "normalize.h"

#define GFNT_PF_GLOBAL 0x01u        ///< Applies to the whole run: a bit shared by all.
#define GFNT_PF_HAS_FALLBACK 0x02u  ///< Keep it though the font lacks it: the shaper has a fallback.
#define GFNT_PF_MANUAL_ZWNJ 0x04u   ///< Joiners are the lookup's business, not skipped.
#define GFNT_PF_MANUAL_ZWJ 0x08u
#define GFNT_PF_PER_SYLLABLE 0x10u  ///< Matches do not cross a syllable.
#define GFNT_PF_GLOBAL_SEARCH 0x20u ///< If the language system lacks it, any feature of the tag will do.

#define GFNT_NO_FEATURE 0xFFFFu
#define GFNT_MAX_STAGES 32

#define GFNT_TAG_DFLT GFNT_TAG('D', 'F', 'L', 'T')
#define GFNT_TAG_dflt GFNT_TAG('d', 'f', 'l', 't')
#define GFNT_TAG_latn GFNT_TAG('l', 'a', 't', 'n')

typedef struct GFNT_Shaper GFNT_Shaper;
typedef struct GFNT_Plan GFNT_Plan;

/** What a shaper's hook is given: the run, mid-way through being shaped. */
typedef struct GFNT_ShapeCtx {
  const GFNT_Face * face;
  const GFNT_ShapeOptions * options;
  const GFNT_Plan * plan;
  GFNT_LBuffer * buf;
  const GFNT_Gdef * gdef;
  GFNT_LApply * gsub;    ///< For asking `GSUB` what it would do; NULL if the face has none.
  const GFNT_Allocator * allocator;
  bool native_rtl;   ///< The run is in a right-to-left script's own order.
  bool oom;          ///< A hook ran out of memory; shaping gives up.
} GFNT_ShapeCtx;

/** Work done between two stages of substitution. */
typedef void (*GFNT_PauseFunc)(GFNT_ShapeCtx * ctx);

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
  bool found;   ///< The font has it, for the script and language, in either table.
} GFNT_PlanFeature;

/** One lookup to run: which, on which glyphs, in which pass. */
typedef struct GFNT_PlanLookup {
  uint32_t index;
  uint32_t mask;
  uint8_t stage;
  bool auto_zwnj;
  bool auto_zwj;
  bool per_syllable;
  uint32_t order;   ///< Where the plan added it, so a merge can keep the first's flags.
} GFNT_PlanLookup;

typedef struct GFNT_PlanTable {
  GFNT_PlanLookup * lookups;
  size_t count;
  size_t capacity;
  GFNT_Tag script;
  GFNT_Tag language;
  bool present;
  bool kern_found;   ///< GPOS only: the `kern` feature is in the language system.
  bool script_found; ///< The requested script itself, not a fallback, was found.
  size_t script_offset;
  size_t langsys_offset;
} GFNT_PlanTable;

/** A shaper: the parts of shaping that depend on the script. */
struct GFNT_Shaper {
  const char * name;
  /** Ask for the features the script wants, after the default shaper's first ones. */
  void (*collect_features)(GFNT_Plan * plan);
  /** Change what has been asked for, before the caller's own features. */
  void (*override_features)(GFNT_Plan * plan);
  /**
   * A pause after every feature there is, the caller's own included: the last
   * stage of the shaper's features is the one the common features join.
   */
  GFNT_PauseFunc trailing_pause;
  /** Per-plan data, made once the features have their bits. NULL for none. */
  void * (*data_create)(const GFNT_Plan * plan, const GFNT_Allocator * allocator);
  void (*data_destroy)(void * data, const GFNT_Allocator * allocator);
  /** Rewrite the characters before they are normalised. */
  void (*preprocess_text)(GFNT_ShapeCtx * ctx, GFNT_LInfo ** chars, size_t * len,
      size_t * capacity);
  /** Work on the glyphs after substitution and positioning. */
  void (*postprocess_glyphs)(GFNT_ShapeCtx * ctx);
  GFNT_NormMode normalization;
  const GFNT_NormHooks * normalization_hooks;
  /** Set the masks that say which glyphs a feature applies to. */
  void (*setup_masks)(GFNT_ShapeCtx * ctx);
  /** Zero the advance of GDEF marks: 0 never, 1 before positioning, 2 after. */
  int zero_width_marks;
  /** Place marks from the glyphs' boxes when the font has no `GPOS`. */
  bool fallback_position;
  /** The GPOS script this shaper insists on, or 0 for any. */
  GFNT_Tag gpos_tag;
};

/** The plan: the features with their bits, and the lookups of each table. */
struct GFNT_Plan {
  const GFNT_Allocator * allocator;
  const GFNT_ShapeOptions * options;
  size_t count;               ///< How many characters the run was given.
  GFNT_PlanFeature * features;
  size_t feature_count;
  size_t feature_capacity;
  uint32_t global_mask;
  uint32_t kern_mask;         ///< The `kern` feature's bits, or 0 when it is off.
  bool apply_kerx;            ///< The font has `kerx`: it kerns, and `GPOS` and `kern` do not run (unless below).
  bool gpos_over_kerx;        ///< ... but a font with `GSUB` and `GPOS` is positioned by the `GPOS`, not the `kerx`.
  bool kern_fallback;         ///< `GPOS` does not kern, so the `kern` table does.
  GFNT_PlanTable tables[2];   ///< GSUB, GPOS.
  uint8_t gsub_stage;         ///< The stage features are being asked for in.
  size_t gsub_stages;         ///< How many stages substitution has.
  GFNT_PauseFunc pause[GFNT_MAX_STAGES]; ///< Run after the stage of that index.
  const GFNT_Shaper * shaper;
  void * shaper_data;
  GFNT_Tag script;            ///< The script asked for, an OpenType tag (old form).
  bool oom;
  uint32_t frac_mask;
  uint32_t numr_mask;
  uint32_t dnom_mask;
  bool has_frac;
  bool has_mark_feature;      ///< `mark` is in a language system of either table.
  bool native_rtl;
  bool no_synthetic_classes; ///< `morx` stood in for a script's own shaper.
  bool vertical;              ///< The run is top to bottom or bottom to top.
};

/** Grow a vector, as in shape.c: the new storage, or NULL (the old stays valid). */
void * gfnt_vec_grow(const GFNT_Allocator * a, void * data, size_t * capacity,
    size_t needed, size_t size);

/** Ask for a feature on the whole run (so a shared bit), with this flag set. */
void gfnt_plan_enable(GFNT_Plan * plan, GFNT_Tag tag, uint32_t flags,
    uint32_t value);
/** Ask for a feature that is on only for the glyphs the shaper turns it on for. */
void gfnt_plan_add(GFNT_Plan * plan, GFNT_Tag tag, uint32_t flags);
/** Start a new stage; @p pause, if any, runs when the old one is done. */
void gfnt_plan_pause(GFNT_Plan * plan, GFNT_PauseFunc pause);
/** The feature of this tag, with its bits, or NULL. */
const GFNT_PlanFeature * gfnt_plan_find(const GFNT_Plan * plan, GFNT_Tag tag);
/** The mask of a feature that has bits, or 0. */
uint32_t gfnt_plan_mask(const GFNT_Plan * plan, GFNT_Tag tag);
/**
 * Whether the lookups of the stage a feature runs in would substitute this run of
 * glyphs, with nothing around it: HarfBuzz's hb_indic_would_substitute_feature_t.
 * False if the font does not have the feature.
 */
bool gfnt_plan_would_substitute(const GFNT_ShapeCtx * ctx, GFNT_Tag tag,
    const uint32_t * glyphs, size_t count, bool zero_context);
/** The mask of a feature the font has, or 0: HarfBuzz's get_1_mask(). */
uint32_t gfnt_plan_found_mask(const GFNT_Plan * plan, GFNT_Tag tag);
/** Whether the feature was asked for at all. */
bool gfnt_plan_has_feature(const GFNT_Plan * plan, GFNT_Tag tag);

/** The shaper for a script, by its OpenType tag, given the tag the font chose. */
const GFNT_Shaper * gfnt_shaper_select(GFNT_Tag script, GFNT_Tag chosen,
    bool horizontal);

/** The default shaper, and the others. */
extern const GFNT_Shaper gfnt_shaper_default;
extern const GFNT_Shaper gfnt_shaper_arabic;
extern const GFNT_Shaper gfnt_shaper_hebrew;
extern const GFNT_Shaper gfnt_shaper_thai;
extern const GFNT_Shaper gfnt_shaper_use;
extern const GFNT_Shaper gfnt_shaper_indic;
extern const GFNT_Shaper gfnt_shaper_khmer;
extern const GFNT_Shaper gfnt_shaper_myanmar;
extern const GFNT_Shaper gfnt_shaper_hangul;

/** The joining masks the Arabic shaper sets, for a shaper that borrows them. */
void * gfnt_arabic_masks_create(const GFNT_Plan * plan,
    const GFNT_Allocator * allocator);
void gfnt_arabic_masks_apply(const void * data, GFNT_ShapeCtx * ctx);
void gfnt_arabic_masks_destroy(void * data, const GFNT_Allocator * allocator);

/**
 * Insert a dotted circle at the start of every broken syllable, if the font has one.
 *
 * HarfBuzz's hb_syllabic_insert_dotted_circles(): the circle goes after a repha, in
 * the syllable and with its cluster and masks, with category @p dotted_category.
 * @return false on no memory.
 */
bool gfnt_syllabic_insert_dotted_circles(GFNT_ShapeCtx * ctx,
    unsigned broken_type, unsigned dotted_category, int repha_category,
    int dotted_position);
/** A pause: forget which glyphs have been substituted, to see what the next stage does. */
void gfnt_syllabic_clear_substitution_flags(GFNT_ShapeCtx * ctx);
/** A pause: the syllables are done with. */
void gfnt_syllabic_clear_syllables(GFNT_ShapeCtx * ctx);

/** Build the plan. On failure the plan is freed and zeroed. */
GFNT_Result gfnt_plan_build(const GFNT_Face * face,
    const GFNT_ShapeOptions * options, size_t count, GFNT_Plan * plan,
    const GFNT_Allocator * allocator, GFNT_Error * error);

void gfnt_plan_free(GFNT_Plan * plan);

/** Whether a script, by its OpenType tag, is written right to left. */
bool gfnt_script_native_rtl(GFNT_Tag script);

#endif
