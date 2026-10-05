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
 * The Thai and Lao shaper: HarfBuzz's `hb-ot-shape-complex-thai.cc`.
 *
 * SARA AM is a vowel that is drawn as two things, a ring above the consonant and
 * a vowel after it, and written as one character. Uniscribe pulls it apart before
 * a font sees it, putting the ring (NIKHAHIT) in front of any tone mark on the
 * consonant so that the tone sits above it; fonts are made for that, so this does
 * the same.
 */

#include <string.h>
#include "plan.h"
#include "uprops.h"

/** SARA AM in Thai (U+0E33) or in Lao (U+0EB3). */
static bool gfnt_thai_is_sara_am(uint32_t u) {
  return (u & ~0x0080u) == 0x0E33u;
}

/** The tone marks and the other signs above a consonant, which NIKHAHIT goes below. */
static bool gfnt_thai_is_tone_mark(uint32_t u) {
  uint32_t x = u & ~0x0080u;

  return (x >= 0x0E34 && x <= 0x0E37) || (x >= 0x0E47 && x <= 0x0E4E)
      || x == 0x0E31;
}

static void gfnt_thai_preprocess(GFNT_ShapeCtx * ctx, GFNT_LInfo ** chars,
    size_t * len, size_t * capacity) {
  GFNT_LInfo * in = *chars;
  size_t count = *len;
  size_t extra = 0;
  GFNT_LInfo * out;
  size_t w = 0;
  size_t i;

  for (i = 0; i < count; i++) {
    if (gfnt_thai_is_sara_am(in[i].unicode)) {
      extra++;
    }
  }
  if (!extra) {
    return;
  }
  out = ctx->allocator->calloc_fn(ctx->allocator->ctx, count + extra,
      sizeof *out);
  if (!out) {
    ctx->oom = true;
    return;
  }
  for (i = 0; i < count; i++) {
    uint32_t u = in[i].unicode;
    size_t end;
    size_t start;
    size_t k;
    uint32_t old_cluster;

    if (!gfnt_thai_is_sara_am(u)) {
      out[w++] = in[i];
      continue;
    }
    // NIKHAHIT, then SARA AA in the place of SARA AM.
    old_cluster = in[i].cluster;
    out[w] = in[i];
    out[w].unicode = u - 0x0E33 + 0x0E4D;
    gfnt_u_set_props(&out[w]);
    out[w].flags |= GFNT_GF_CONTINUATION;
    // It is a combining mark whatever the table says of the Lao one.
    out[w].gc = 17;  // GUNI_GC_NONSPACING_MARK
    out[w].flags |= GFNT_GF_MARK;
    w++;
    out[w] = in[i];
    out[w].unicode = u - 1;
    gfnt_u_set_props(&out[w]);
    w++;
    end = w;
    start = end - 2;
    while (start > 0 && gfnt_thai_is_tone_mark(out[start - 1].unicode)) {
      start--;
    }
    if (start + 2 < end) {
      // Move NIKHAHIT in front of the tone marks.
      GFNT_LInfo t;
      size_t m;

      gfnt_merge_clusters(out, w, start, end);
      t = out[end - 2];
      for (m = end - 2; m > start; m--) {
        out[m] = out[m - 1];
      }
      out[start] = t;
    }
    else if (start) {
      // NIKHAHIT is combining: it belongs to the cluster before it.
      gfnt_merge_clusters(out, w, start - 1, end);
    }
    // And what follows in the same cluster, still to be copied, goes with it.
    for (k = i + 1; k < count && in[k].cluster == old_cluster; k++) {
      in[k].cluster = out[w - 1].cluster;
    }
  }
  ctx->allocator->free_fn(ctx->allocator->ctx, in);
  *chars = out;
  *len = w;
  *capacity = count + extra;
}

const GFNT_Shaper gfnt_shaper_thai = {
  .name = "thai",
  .preprocess_text = gfnt_thai_preprocess,
  .normalization = GFNT_NORM_COMPOSED_DIACRITICS,
  .zero_width_marks = 2,
  .fallback_position = false,
};
