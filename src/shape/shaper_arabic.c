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
 * The Arabic shaper: HarfBuzz's `hb-ot-shape-complex-arabic.cc`, for Arabic and the
 * scripts that join the same way (Syriac, Mongolian, N'Ko, Mandaic, Adlam and the
 * rest).
 *
 * Which of a letter's forms it takes - isolated, initial, medial, final - is
 * decided by its neighbours, by walking the run once with a state machine over
 * each letter's joining type. The font does not decide it: it is told which glyphs
 * to apply `init`, `medi` and `fina` to, by setting a bit on each, and the lookups
 * of those features, run one feature at a time in the order the Arabic
 * specification gives, do the substituting.
 */

#include <ghoti.io/unicode/char.h>
#include <string.h>
#include "arabic_fallback.h"
#include "plan.h"
#include "uprops.h"

// The joining forms a letter can take, and "none" for a letter that takes none.
enum {
  GFNT_AR_ISOL = 0, GFNT_AR_FINA, GFNT_AR_FIN2, GFNT_AR_FIN3, GFNT_AR_MEDI,
  GFNT_AR_MED2, GFNT_AR_INIT, GFNT_AR_NONE, GFNT_AR_FEATURES = 7
};

// The state machine's columns: a letter's joining type, with Syriac's two groups
// that join differently from their type split out.
enum {
  GFNT_JT_U = 0, GFNT_JT_L, GFNT_JT_R, GFNT_JT_D, GFNT_JT_ALAPH,
  GFNT_JT_DALATH_RISH, GFNT_JT_COLUMNS, GFNT_JT_T = GFNT_JT_COLUMNS
};

typedef struct GFNT_ArabicStateEntry {
  uint8_t prev_action;
  uint8_t curr_action;
  uint8_t next_state;
} GFNT_ArabicStateEntry;

#define N GFNT_AR_NONE
#define I GFNT_AR_ISOL
#define F GFNT_AR_FINA
#define F2 GFNT_AR_FIN2
#define F3 GFNT_AR_FIN3
#define M GFNT_AR_MEDI
#define M2 GFNT_AR_MED2
#define IN GFNT_AR_INIT

static const GFNT_ArabicStateEntry gfnt_arabic_states[7][GFNT_JT_COLUMNS] = {
  /*   U         L          R          D          ALAPH      DALATH_RISH */
  /* 0: the letter before cannot join. */
  {{N, N, 0}, {N, I, 2}, {N, I, 1}, {N, I, 2}, {N, I, 1}, {N, I, 6}},
  /* 1: the letter before was R, or an isolated ALAPH, and does not join on. */
  {{N, N, 0}, {N, I, 2}, {N, I, 1}, {N, I, 2}, {N, F2, 5}, {N, I, 6}},
  /* 2: the letter before was D or L, isolated so far, and will join. */
  {{N, N, 0}, {N, I, 2}, {IN, F, 1}, {IN, F, 3}, {IN, F, 4}, {IN, F, 6}},
  /* 3: the letter before was D, final so far, and will join. */
  {{N, N, 0}, {N, I, 2}, {M, F, 1}, {M, F, 3}, {M, F, 4}, {M, F, 6}},
  /* 4: the letter before was a final ALAPH, and does not join on. */
  {{N, N, 0}, {N, I, 2}, {M2, I, 1}, {M2, I, 2}, {M2, F2, 5}, {M2, I, 6}},
  /* 5: the letter before was a FIN2 or FIN3 ALAPH, and does not join on. */
  {{N, N, 0}, {N, I, 2}, {I, I, 1}, {I, I, 2}, {I, F2, 5}, {I, I, 6}},
  /* 6: the letter before was DALATH or RISH, and does not join on. */
  {{N, N, 0}, {N, I, 2}, {N, I, 1}, {N, I, 2}, {N, F3, 5}, {N, I, 6}},
};

#undef N
#undef I
#undef F
#undef F2
#undef F3
#undef M
#undef M2
#undef IN

/** The features, in the order of the actions above. */
static const GFNT_Tag gfnt_arabic_features[GFNT_AR_FEATURES] = {
  GFNT_TAG('i', 's', 'o', 'l'), GFNT_TAG('f', 'i', 'n', 'a'),
  GFNT_TAG('f', 'i', 'n', '2'), GFNT_TAG('f', 'i', 'n', '3'),
  GFNT_TAG('m', 'e', 'd', 'i'), GFNT_TAG('m', 'e', 'd', '2'),
  GFNT_TAG('i', 'n', 'i', 't'),
};

typedef struct GFNT_ArabicData {
  uint32_t mask_array[GFNT_AR_FEATURES + 1];
  bool do_fallback;   ///< Arabic, and the font has none of the joining features.
  uint32_t fallback_masks[GFNT_AR_FALLBACK_LOOKUPS];
} GFNT_ArabicData;

/** After the specification's `rlig`: join from presentation forms if the font cannot. */
static void gfnt_arabic_fallback_pause(GFNT_ShapeCtx * ctx) {
  const GFNT_ArabicData * data = ctx->plan->shaper_data;

  if (data && data->do_fallback) {
    gfnt_arabic_fallback_shape(ctx, data->fallback_masks);
  }
}

/** Whether a feature is Syriac's own: only a Syriac font is expected to have it. */
static bool gfnt_arabic_syriac_feature(GFNT_Tag tag) {
  return tag == GFNT_TAG('f', 'i', 'n', '2') || tag == GFNT_TAG('f', 'i', 'n', '3')
      || tag == GFNT_TAG('m', 'e', 'd', '2');
}

static void gfnt_arabic_collect(GFNT_Plan * plan) {
  size_t i;

  gfnt_plan_enable(plan, GFNT_TAG('s', 't', 'c', 'h'), 0, 1);
  gfnt_plan_pause(plan, NULL);
  gfnt_plan_enable(plan, GFNT_TAG('c', 'c', 'm', 'p'), 0, 1);
  gfnt_plan_enable(plan, GFNT_TAG('l', 'o', 'c', 'l'), 0, 1);
  gfnt_plan_pause(plan, NULL);
  for (i = 0; i < GFNT_AR_FEATURES; i++) {
    bool has_fallback = plan->script == GFNT_TAG('a', 'r', 'a', 'b')
        && !gfnt_arabic_syriac_feature(gfnt_arabic_features[i]);

    gfnt_plan_add(plan, gfnt_arabic_features[i],
        has_fallback ? GFNT_PF_HAS_FALLBACK : 0);
    gfnt_plan_pause(plan, NULL);
  }
  // The specification applies `rlig`, then `calt` and `rclt`, after the joining
  // forms; a joiner between two letters does not stop a ligature.
  gfnt_plan_enable(plan, GFNT_TAG('r', 'l', 'i', 'g'),
      GFNT_PF_MANUAL_ZWJ | (plan->script == GFNT_TAG('a', 'r', 'a', 'b')
          ? GFNT_PF_HAS_FALLBACK : 0), 1);
  gfnt_plan_pause(plan, plan->script == GFNT_TAG('a', 'r', 'a', 'b')
      ? gfnt_arabic_fallback_pause : NULL);
  // No pause after `rclt`.
  gfnt_plan_enable(plan, GFNT_TAG('r', 'c', 'l', 't'), GFNT_PF_MANUAL_ZWJ, 1);
  gfnt_plan_enable(plan, GFNT_TAG('c', 'a', 'l', 't'), GFNT_PF_MANUAL_ZWJ, 1);
  gfnt_plan_pause(plan, NULL);
  gfnt_plan_enable(plan, GFNT_TAG('m', 's', 'e', 't'), 0, 1);
}

static void * gfnt_arabic_data_create(const GFNT_Plan * plan,
    const GFNT_Allocator * allocator) {
  GFNT_ArabicData * data = allocator->calloc_fn(allocator->ctx, 1,
      sizeof *data);
  size_t i;

  if (!data) {
    return NULL;
  }
  for (i = 0; i < GFNT_AR_FEATURES; i++) {
    data->mask_array[i] = gfnt_plan_mask(plan, gfnt_arabic_features[i]);
  }
  // The fallback is for Arabic alone and only for a font with none of the four
  // joining features of its own.
  data->do_fallback = plan->script == GFNT_TAG('a', 'r', 'a', 'b');
  for (i = 0; i < GFNT_AR_FEATURES; i++) {
    const GFNT_PlanFeature * f;

    if (gfnt_arabic_syriac_feature(gfnt_arabic_features[i])) {
      continue;
    }
    f = gfnt_plan_find(plan, gfnt_arabic_features[i]);
    if (!f || !(f->flags & GFNT_PF_HAS_FALLBACK) || f->found) {
      data->do_fallback = false;
    }
  }
  data->fallback_masks[0] = data->mask_array[GFNT_AR_ISOL];
  data->fallback_masks[1] = data->mask_array[GFNT_AR_FINA];
  data->fallback_masks[2] = data->mask_array[GFNT_AR_INIT];
  data->fallback_masks[3] = data->mask_array[GFNT_AR_MEDI];
  data->fallback_masks[4] = gfnt_plan_mask(plan, GFNT_TAG('r', 'l', 'i', 'g'));
  data->fallback_masks[5] = data->fallback_masks[4];   // the mark ligatures answer to rlig too
  return data;
}

static void gfnt_arabic_data_destroy(void * data,
    const GFNT_Allocator * allocator) {
  allocator->free_fn(allocator->ctx, data);
}

/** A letter's column in the state machine; GFNT_JT_T for one that is skipped. */
static unsigned gfnt_arabic_joining(const GFNT_LInfo * info) {
  uint32_t u = info->unicode;
  GUNI_JoiningType type = guni_joining_type(u);
  GUNI_JoiningGroup group;

  if (type == GUNI_JT_NON_JOINING && u >= 0x80
      && (info->gc == GUNI_GC_NONSPACING_MARK
          || info->gc == GUNI_GC_ENCLOSING_MARK
          || info->gc == GUNI_GC_FORMAT)) {
    // Not listed: a mark or a format character is transparent by default, except
    // the two that are listed (ZWJ joins, ZWNJ does not).
    if (u != 0x200C && u != 0x200D) {
      return GFNT_JT_T;
    }
  }
  switch (type) {
    case GUNI_JT_TRANSPARENT:
      return GFNT_JT_T;
    case GUNI_JT_LEFT_JOINING:
      return GFNT_JT_L;
    case GUNI_JT_DUAL_JOINING:
    case GUNI_JT_JOIN_CAUSING:
      return GFNT_JT_D;
    case GUNI_JT_RIGHT_JOINING:
      group = guni_joining_group(u);
      if (group == GUNI_JG_ALAPH) {
        return GFNT_JT_ALAPH;
      }
      if (group == GUNI_JG_DALATH_RISH) {
        return GFNT_JT_DALATH_RISH;
      }
      return GFNT_JT_R;
    default:
      return GFNT_JT_U;
  }
}

static void gfnt_arabic_apply_masks(const GFNT_ArabicData * data,
    GFNT_ShapeCtx * ctx) {
  GFNT_LBuffer * buf = ctx->buf;
  uint8_t * action;
  size_t prev = (size_t)-1;
  unsigned state = 0;
  size_t i;

  if (!buf->len) {
    return;
  }
  action = ctx->allocator->calloc_fn(ctx->allocator->ctx, buf->len, 1);
  if (!action) {
    ctx->oom = true;
    return;
  }
  for (i = 0; i < buf->len; i++) {
    unsigned type = gfnt_arabic_joining(&buf->info[i]);
    const GFNT_ArabicStateEntry * entry;

    if (type == GFNT_JT_T) {
      action[i] = GFNT_AR_NONE;
      continue;
    }
    entry = &gfnt_arabic_states[state][type];
    if (entry->prev_action != GFNT_AR_NONE && prev != (size_t)-1) {
      action[prev] = entry->prev_action;
    }
    action[i] = entry->curr_action;
    prev = i;
    state = entry->next_state;
  }
  // A Mongolian free variation selector takes the form of the letter before it,
  // so that a lookup for the form can take the pair together.
  for (i = 1; i < buf->len; i++) {
    uint32_t u = buf->info[i].unicode;

    if ((u >= 0x180B && u <= 0x180D) || u == 0x180F) {
      action[i] = action[i - 1];
    }
  }
  for (i = 0; i < buf->len; i++) {
    buf->info[i].mask |= data->mask_array[action[i]];
  }
  ctx->allocator->free_fn(ctx->allocator->ctx, action);
}

static void gfnt_arabic_setup_masks(GFNT_ShapeCtx * ctx) {
  gfnt_arabic_apply_masks(ctx->plan->shaper_data, ctx);
}

/** Whether a code point is one of the combining marks that sit above or below. */
static bool gfnt_arabic_mcm(uint32_t u) {
  switch (u) {
    case 0x0654: case 0x0655: case 0x0658: case 0x06DC: case 0x06E3:
    case 0x06E7: case 0x06E8: case 0x08D3: case 0x08F3:
      return true;
    default:
      return false;
  }
}

/**
 * The modifier combining marks go before the other marks of their class: a hamza
 * above, drawn next to the letter, then a fatha over it.
 */
static void gfnt_arabic_reorder_marks(GFNT_LInfo * info, size_t start,
    size_t end, void * ctx) {
  size_t i = start;
  uint8_t cc;

  (void)ctx;
  for (cc = 220; cc <= 230; cc = (uint8_t)(cc + 10)) {
    size_t j;

    while (i < end && info[i].mcc < cc) {
      i++;
    }
    if (i == end) {
      break;
    }
    if (info[i].mcc > cc) {
      continue;
    }
    j = i;
    while (j < end && info[j].mcc == cc && gfnt_arabic_mcm(info[j].unicode)) {
      j++;
    }
    if (i == j) {
      continue;
    }
    {
      GFNT_LInfo temp[32];
      size_t n = j - i;
      size_t new_start;
      size_t k;
      uint8_t new_cc;

      if (n > sizeof temp / sizeof temp[0]) {
        i = j;
        continue;
      }
      for (k = 0; k < n; k++) {
        temp[k] = info[i + k];
      }
      for (k = i - start; k > 0; k--) {
        info[start + n + k - 1] = info[start + k - 1];
      }
      for (k = 0; k < n; k++) {
        info[start + k] = temp[k];
      }
      // Renumber so that the reordered sequence is still in order: 22 and 26 are
      // below every Arabic class and used by nothing else.
      new_start = start + n;
      new_cc = cc == 220 ? 22 : 26;
      while (start < new_start) {
        info[start].mcc = new_cc;
        start++;
      }
    }
    i = j;
  }
}

static const GFNT_NormHooks gfnt_arabic_hooks = {
  .reorder_marks = gfnt_arabic_reorder_marks,
};

const GFNT_Shaper gfnt_shaper_arabic = {
  .name = "arabic",
  .collect_features = gfnt_arabic_collect,
  .data_create = gfnt_arabic_data_create,
  .data_destroy = gfnt_arabic_data_destroy,
  .normalization = GFNT_NORM_COMPOSED_DIACRITICS,
  .normalization_hooks = &gfnt_arabic_hooks,
  .setup_masks = gfnt_arabic_setup_masks,
  .zero_width_marks = 2,
  .fallback_position = true,
};

/* --- for the shapers that join the way Arabic does without being it ------ */

void * gfnt_arabic_masks_create(const GFNT_Plan * plan,
    const GFNT_Allocator * allocator) {
  return gfnt_arabic_data_create(plan, allocator);
}

void gfnt_arabic_masks_apply(const void * data, GFNT_ShapeCtx * ctx) {
  gfnt_arabic_apply_masks(data, ctx);
}

void gfnt_arabic_masks_destroy(void * data, const GFNT_Allocator * allocator) {
  gfnt_arabic_data_destroy(data, allocator);
}
