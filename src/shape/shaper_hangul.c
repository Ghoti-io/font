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
 * The Hangul shaper.
 *
 * A Hangul syllable is a leading consonant (L), a vowel (V) and, sometimes, a
 * trailing consonant (T), and may be written as its three jamo or as one
 * precomposed character. Which the font draws decides what is done with the
 * text: jamo whose composite the font has become that one glyph, a precomposed
 * syllable the font lacks is taken apart, and jamo that stay jamo have the
 * font's `ljmo`, `vjmo` and `tjmo` turned on for them, each for its own kind.
 * The tone marks that go before a syllable are moved there.
 *
 * What the font has is asked of its `cmap`, so the text is rewritten before it
 * is normalised, and normalisation leaves it as it is.
 */

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/metrics.h>
#include <string.h>
#include "plan.h"
#include "uprops.h"

#define LBASE 0x1100u
#define VBASE 0x1161u
#define TBASE 0x11A7u
#define SBASE 0xAC00u
#define LCOUNT 19u
#define VCOUNT 21u
#define TCOUNT 28u
#define NCOUNT (VCOUNT * TCOUNT)
#define SCOUNT (LCOUNT * NCOUNT)

enum { HF_NONE, HF_LJMO, HF_VJMO, HF_TJMO };

typedef struct HangulData {
  uint32_t mask[3];
} HangulData;

static bool hangul_is_combining_l(uint32_t u) {
  return u >= LBASE && u < LBASE + LCOUNT;
}

static bool hangul_is_combining_v(uint32_t u) {
  return u >= VBASE && u < VBASE + VCOUNT;
}

static bool hangul_is_combining_t(uint32_t u) {
  return u > TBASE && u < TBASE + TCOUNT;
}

static bool hangul_is_combined_s(uint32_t u) {
  return u >= SBASE && u < SBASE + SCOUNT;
}

static bool hangul_is_l(uint32_t u) {
  return (u >= 0x1100 && u <= 0x115F) || (u >= 0xA960 && u <= 0xA97C);
}

static bool hangul_is_v(uint32_t u) {
  return (u >= 0x1160 && u <= 0x11A7) || (u >= 0xD7B0 && u <= 0xD7C6);
}

static bool hangul_is_t(uint32_t u) {
  return (u >= 0x11A8 && u <= 0x11FF) || (u >= 0xD7CB && u <= 0xD7FB);
}

static bool hangul_is_tone(uint32_t u) {
  return u >= 0x302E && u <= 0x302F;
}

static bool hangul_has_glyph(const GFNT_ShapeCtx * ctx, uint32_t u) {
  uint32_t glyph = 0;

  return gfnt_face_glyph_for_codepoint(ctx->face, u, &glyph, NULL) == GFNT_OK
      && glyph != 0;
}

/** Whether the font draws this character with no width. */
static bool hangul_is_zero_width(const GFNT_ShapeCtx * ctx, uint32_t u) {
  uint32_t glyph = 0;
  int32_t advance = 0;

  if (gfnt_face_glyph_for_codepoint(ctx->face, u, &glyph, NULL) != GFNT_OK
      || !glyph) {
    return false;
  }
  return gfnt_face_glyph_advance(ctx->face, glyph, ctx->options->variation,
             &advance, NULL) == GFNT_OK && advance == 0;
}

/** A record for a code point made out of @p from: its cluster and what follows. */
static GFNT_LInfo hangul_char(const GFNT_LInfo * from, uint32_t u, uint8_t feature) {
  GFNT_LInfo r = *from;

  r.unicode = u;
  gfnt_u_set_props(&r);
  r.category = feature;
  return r;
}

/**
 * Merge the clusters of out[start, end), and of whatever of the text still to be
 * read (in[next, count)) was already one with the last of them, such as a ZWJ.
 */
static void hangul_merge(GFNT_LInfo * out, size_t w, size_t start, size_t end,
    GFNT_LInfo * in, size_t count, size_t next) {
  uint32_t old = out[end - 1].cluster;

  gfnt_merge_clusters(out, w, start, end);
  for (; next < count && in[next].cluster == old; next++) {
    in[next].cluster = out[start].cluster;
  }
}

static void hangul_preprocess(GFNT_ShapeCtx * ctx, GFNT_LInfo ** chars,
    size_t * len, size_t * capacity) {
  GFNT_LInfo * in = *chars;
  size_t count = *len;
  GFNT_LInfo * out;
  size_t w = 0;
  size_t start = 0;
  size_t end = 0;
  size_t i = 0;

  out = ctx->allocator->calloc_fn(ctx->allocator->ctx, count * 3 + 1,
      sizeof *out);
  if (!out) {
    ctx->oom = true;
    return;
  }
  for (i = 0; i < count; i++) {
    in[i].category = HF_NONE;
  }
  i = 0;
  while (i < count) {
    uint32_t u = in[i].unicode;

    if (hangul_is_tone(u)) {
      if (start < end && end == w) {
        // A tone mark after a syllable goes in front of it, unless the font draws
        // it with no width.
        out[w] = in[i];
        w++;
        if (!hangul_is_zero_width(ctx, u)) {
          GFNT_LInfo tone = out[end];
          size_t k;

          for (k = end; k > start; k--) {
            out[k] = out[k - 1];
          }
          out[start] = tone;
          // What the tone mark passed is one cluster with it.
          gfnt_merge_clusters(out, w, start, end + 1);
        }
      }
      else if (hangul_has_glyph(ctx, 0x25CC)) {
        // There is no syllable for the tone mark to go with: give it the circle.
        GFNT_LInfo circle = hangul_char(&in[i], 0x25CC, HF_NONE);

        if (!hangul_is_zero_width(ctx, u)) {
          out[w++] = in[i];
          out[w++] = circle;
        }
        else {
          out[w++] = circle;
          out[w++] = in[i];
        }
      }
      else {
        out[w++] = in[i];
      }
      start = end = w;
      i++;
      continue;
    }

    start = w;
    if (hangul_is_l(u) && i + 1 < count) {
      uint32_t l = u;
      uint32_t v = in[i + 1].unicode;

      if (hangul_is_v(v)) {
        // <L,V> or <L,V,T>.
        uint32_t t = 0;
        uint32_t tindex = 0;
        size_t n = 2;

        if (i + 2 < count) {
          t = in[i + 2].unicode;
          if (hangul_is_t(t)) {
            tindex = t - TBASE;
            n = 3;
          }
          else {
            t = 0;
          }
        }
        if (hangul_is_combining_l(l) && hangul_is_combining_v(v)
            && (t == 0 || hangul_is_combining_t(t))) {
          uint32_t s = SBASE + (l - LBASE) * NCOUNT + (v - VBASE) * TCOUNT
              + tindex;

          if (hangul_has_glyph(ctx, s)) {
            // A precomposed syllable: the clusters of what it replaces are one.
            gfnt_merge_clusters(in, count, i, i + n);
            out[w++] = hangul_char(&in[i], s, HF_NONE);
            end = start + 1;
            i += n;
            continue;
          }
        }
        // It did not compose; the font's jamo features do the work.
        out[w++] = hangul_char(&in[i], l, HF_LJMO);
        out[w++] = hangul_char(&in[i + 1], v, HF_VJMO);
        if (t) {
          out[w++] = hangul_char(&in[i + 2], t, HF_TJMO);
        }
        end = start + n;
        i += n;
        hangul_merge(out, w, start, end, in, count, i);
        continue;
      }
    }
    else if (hangul_is_combined_s(u)) {
      // <LV>, <LVT> or <LV,T>.
      uint32_t s = u;
      bool has_glyph = hangul_has_glyph(ctx, s);
      uint32_t lindex = (s - SBASE) / NCOUNT;
      uint32_t nindex = (s - SBASE) % NCOUNT;
      uint32_t vindex = nindex / TCOUNT;
      uint32_t tindex = nindex % TCOUNT;

      if (!tindex && i + 1 < count && hangul_is_combining_t(in[i + 1].unicode)) {
        // <LV,T>: try to combine.
        uint32_t new_s = s + (in[i + 1].unicode - TBASE);

        if (hangul_has_glyph(ctx, new_s)) {
          gfnt_merge_clusters(in, count, i, i + 2);
          out[w++] = hangul_char(&in[i], new_s, HF_NONE);
          end = start + 1;
          i += 2;
          continue;
        }
      }
      // Otherwise take it apart, if the font has no <LV> or <LVT>, or if a
      // trailing jamo that cannot combine follows.
      if (!has_glyph || (!tindex && i + 1 < count
              && hangul_is_t(in[i + 1].unicode))) {
        uint32_t d0 = LBASE + lindex;
        uint32_t d1 = VBASE + vindex;
        uint32_t d2 = TBASE + tindex;

        if (hangul_has_glyph(ctx, d0) && hangul_has_glyph(ctx, d1)
            && (!tindex || hangul_has_glyph(ctx, d2))) {
          size_t s_len = tindex ? 3 : 2;
          size_t used = 1;

          out[w++] = hangul_char(&in[i], d0, HF_LJMO);
          out[w++] = hangul_char(&in[i], d1, HF_VJMO);
          if (tindex) {
            out[w++] = hangul_char(&in[i], d2, HF_TJMO);
          }
          // An <LV> the font has, taken apart because a jamo follows that it
          // cannot combine with, takes that jamo into the syllable. One the font
          // lacks is only taken apart: the jamo after it stands alone.
          if (has_glyph && !tindex && i + 1 < count
              && hangul_is_t(in[i + 1].unicode)) {
            out[w++] = hangul_char(&in[i + 1], in[i + 1].unicode, HF_TJMO);
            s_len++;
            used++;
          }
          end = start + s_len;
          i += used;
          hangul_merge(out, w, start, end, in, count, i);
          continue;
        }
      }
      if (has_glyph) {
        // Not taken apart: just step over it.
        out[w++] = in[i];
        end = start + 1;
        i++;
        continue;
      }
    }
    // Not a Hangul syllable.
    out[w++] = in[i];
    i++;
  }
  ctx->allocator->free_fn(ctx->allocator->ctx, in);
  *chars = out;
  *len = w;
  *capacity = count * 3 + 1;
}

static void hangul_collect_features(GFNT_Plan * plan) {
  gfnt_plan_add(plan, GFNT_TAG('l', 'j', 'm', 'o'), 0);
  gfnt_plan_add(plan, GFNT_TAG('v', 'j', 'm', 'o'), 0);
  gfnt_plan_add(plan, GFNT_TAG('t', 'j', 'm', 'o'), 0);
}

static void hangul_override_features(GFNT_Plan * plan) {
  // Uniscribe does not apply `calt` for Hangul, and some fonts put all their jamo
  // lookups under it.
  gfnt_plan_enable(plan, GFNT_TAG('c', 'a', 'l', 't'), 0, 0);
}

static void * hangul_data_create(const GFNT_Plan * plan,
    const GFNT_Allocator * allocator) {
  HangulData * data = allocator->calloc_fn(allocator->ctx, 1, sizeof *data);

  if (!data) {
    return NULL;
  }
  data->mask[0] = gfnt_plan_found_mask(plan, GFNT_TAG('l', 'j', 'm', 'o'));
  data->mask[1] = gfnt_plan_found_mask(plan, GFNT_TAG('v', 'j', 'm', 'o'));
  data->mask[2] = gfnt_plan_found_mask(plan, GFNT_TAG('t', 'j', 'm', 'o'));
  return data;
}

static void hangul_data_destroy(void * data, const GFNT_Allocator * allocator) {
  allocator->free_fn(allocator->ctx, data);
}

static void hangul_setup_masks(GFNT_ShapeCtx * ctx) {
  const HangulData * data = ctx->plan->shaper_data;
  size_t i;

  for (i = 0; i < ctx->buf->len; i++) {
    GFNT_LInfo * info = &ctx->buf->info[i];

    if (info->category >= HF_LJMO && info->category <= HF_TJMO) {
      info->mask |= data->mask[info->category - HF_LJMO];
    }
  }
}

const GFNT_Shaper gfnt_shaper_hangul = {
  .name = "hangul",
  .collect_features = hangul_collect_features,
  .override_features = hangul_override_features,
  .data_create = hangul_data_create,
  .data_destroy = hangul_data_destroy,
  .preprocess_text = hangul_preprocess,
  .normalization = GFNT_NORM_NONE,
  .setup_masks = hangul_setup_masks,
  .zero_width_marks = 0,
  .fallback_position = false,
};
