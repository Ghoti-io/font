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
 * Normalisation for shaping. See normalize.h.
 */

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/norm.h>
#include <ghoti.io/font/cmap.h>
#include <string.h>
#include "normalize.h"
#include "uprops.h"

/** Marks no more than this long a run are sorted; a longer one is left alone. */
#define GFNT_MAX_COMBINING_MARKS 32

typedef struct GFNT_NormCtx {
  const GFNT_Face * face;
  const GFNT_Allocator * allocator;
  const GFNT_NormHooks * hooks;
  const GFNT_LInfo * in;
  size_t count;
  size_t idx;
  GFNT_LInfo * out;
  size_t len;
  size_t capacity;
  bool oom;
} GFNT_NormCtx;

void gfnt_merge_clusters(GFNT_LInfo * info, size_t len, size_t start,
    size_t end) {
  uint32_t cluster;
  size_t i;

  if (end - start < 2) {
    return;
  }
  cluster = info[start].cluster;
  for (i = start + 1; i < end; i++) {
    if (info[i].cluster < cluster) {
      cluster = info[i].cluster;
    }
  }
  // A cluster is never split: whatever shares a value with the edge of the range
  // is in the range.
  while (end < len && info[end - 1].cluster == info[end].cluster) {
    end++;
  }
  while (start > 0 && info[start - 1].cluster == info[start].cluster) {
    start--;
  }
  for (i = start; i < end; i++) {
    info[i].cluster = cluster;
  }
}

/** Whether the font maps @p u, and to what. A glyph 0 is "no". */
static bool gfnt_nominal(const GFNT_NormCtx * c, uint32_t u, uint32_t * glyph) {
  uint32_t g = 0;

  if (gfnt_face_glyph_for_codepoint(c->face, u, &g, NULL) != GFNT_OK) {
    g = 0;
  }
  *glyph = g;
  return g != 0;
}

static GFNT_LInfo * gfnt_norm_push(GFNT_NormCtx * c) {
  if (c->len == c->capacity) {
    size_t wanted = c->capacity ? c->capacity * 2 : 16;
    GFNT_LInfo * grown = c->allocator->realloc_fn(c->allocator->ctx, c->out,
        wanted * sizeof *c->out);

    if (!grown) {
      c->oom = true;
      return NULL;
    }
    c->out = grown;
    c->capacity = wanted;
  }
  return &c->out[c->len++];
}

/** The current input character, kept, with the font's glyph for it. */
static void gfnt_next_char(GFNT_NormCtx * c, uint32_t glyph) {
  GFNT_LInfo * slot = gfnt_norm_push(c);

  if (slot) {
    *slot = c->in[c->idx];
    slot->glyph = glyph;
  }
  c->idx++;
}

/** A character that replaces the current one, in its cluster. */
static void gfnt_output_char(GFNT_NormCtx * c, uint32_t unicode,
    uint32_t glyph) {
  GFNT_LInfo * slot = gfnt_norm_push(c);

  if (slot) {
    *slot = c->in[c->idx];
    slot->unicode = unicode;
    slot->glyph = glyph;
    gfnt_u_set_props(slot);
  }
}

/**
 * The character whose whole decomposition is @p full[0..n), when it is one the
 * composition table does not know: the Hebrew presentation forms and the musical
 * symbols are *excluded* from composition, so composing the first characters of
 * a longer decomposition cannot find them. They are all in a few blocks.
 */
static uint32_t gfnt_excluded_prefix(const uint32_t * full, size_t n) {
  static const uint32_t ranges[][2] = {
    {0xFB1D, 0xFB4F}, {0x1D15E, 0x1D164}, {0x0958, 0x095F}, {0x09DC, 0x09DF},
    {0x0A33, 0x0A59}, {0x0F43, 0x0F69}, {0x0F73, 0x0F81},
  };
  size_t r;

  for (r = 0; r < sizeof ranges / sizeof ranges[0]; r++) {
    uint32_t u;

    for (u = ranges[r][0]; u <= ranges[r][1]; u++) {
      uint32_t candidate[32];
      size_t m = 0;
      size_t i;

      if (guni_decomposition_type(u) != GUNI_DT_CANONICAL
          || guni_decompose(u, false, candidate, 32, &m) != GUNI_OK
          || m != n) {
        continue;
      }
      for (i = 0; i < n && candidate[i] == full[i]; i++) {
      }
      if (i == n) {
        return u;
      }
    }
  }
  return 0;
}

/** One step of canonical decomposition, derived from `unicode`'s full one. */
bool gfnt_unicode_decompose(uint32_t ab, uint32_t * a, uint32_t * b,
    void * ctx) {
  uint32_t full[32];
  size_t n = 0;
  uint32_t prefix;
  uint32_t composite;
  size_t i;

  (void)ctx;
  if (ab < 0xC0) {
    return false;
  }
  if (ab >= 0xAC00 && ab <= 0xD7A3) {
    uint32_t s = ab - 0xAC00;
    uint32_t t = s % 28;

    if (t) {
      *a = ab - t;
      *b = 0x11A7 + t;
    }
    else {
      *a = 0x1100 + s / 588;
      *b = 0x1161 + (s % 588) / 28;
    }
    return true;
  }
  if (guni_decompose(ab, false, full, 32, &n) != GUNI_OK || n == 0) {
    return false;
  }
  if (n == 1) {
    if (full[0] == ab) {
      return false;
    }
    *a = full[0];
    *b = 0;
    return true;
  }
  // The last character is the second of the one step; the first is whatever the
  // others compose to. If they do not compose, or compose to something other than
  // this character, then this one is a singleton (U+212B, whose whole expansion is
  // the same as U+00C5's) or an excluded composite, and the first two are the step.
  prefix = full[0];
  for (i = 1; i + 1 < n && prefix; i++) {
    prefix = guni_compose(prefix, full[i]);
  }
  if (!prefix && n >= 3) {
    prefix = gfnt_excluded_prefix(full, n - 1);
  }
  if (prefix) {
    composite = guni_compose(prefix, full[n - 1]);
    if (composite == ab || composite == 0) {
      // Composes back to this character, or is excluded from composing and so
      // cannot say: either way the last character is the second of the step.
      *a = prefix;
      *b = full[n - 1];
      return true;
    }
    *a = composite;   // a singleton, whose expansion is another character's
    *b = 0;
    return true;
  }
  *a = full[0];
  *b = full[1];
  return true;
}

static bool gfnt_decompose_step(const GFNT_NormCtx * c, uint32_t ab,
    uint32_t * a, uint32_t * b) {
  *a = ab;
  *b = 0;
  if (c->hooks && c->hooks->decompose) {
    return c->hooks->decompose(ab, a, b, c->hooks->ctx);
  }
  return gfnt_unicode_decompose(ab, a, b, NULL);
}

static bool gfnt_compose_pair(const GFNT_NormCtx * c, uint32_t a, uint32_t b,
    uint32_t * ab) {
  if (c->hooks && c->hooks->compose) {
    return c->hooks->compose(a, b, ab, c->hooks->ctx);
  }
  *ab = guni_compose(a, b);
  return *ab != 0;
}

/** HarfBuzz's decompose(): how many characters @p ab became, or 0. */
static int gfnt_decompose(GFNT_NormCtx * c, bool shortest, uint32_t ab) {
  uint32_t a;
  uint32_t b = 0;
  uint32_t a_glyph = 0;
  uint32_t b_glyph = 0;
  bool has_a;
  int ab_len;

  if (!gfnt_decompose_step(c, ab, &a, &b)
      || (b && !gfnt_nominal(c, b, &b_glyph))) {
    return 0;
  }
  has_a = gfnt_nominal(c, a, &a_glyph);
  if (shortest && has_a) {
    gfnt_output_char(c, a, a_glyph);
    if (b) {
      gfnt_output_char(c, b, b_glyph);
      return 2;
    }
    return 1;
  }
  ab_len = gfnt_decompose(c, shortest, a);
  if (ab_len) {
    if (b) {
      gfnt_output_char(c, b, b_glyph);
      return ab_len + 1;
    }
    return ab_len;
  }
  if (has_a) {
    gfnt_output_char(c, a, a_glyph);
    if (b) {
      gfnt_output_char(c, b, b_glyph);
      return 2;
    }
    return 1;
  }
  return 0;
}

/**
 * Whether a character is a variation selector the font's cmap is asked about. HarfBuzz
 * does not ask for Mongolian's free ones.
 */
static bool gfnt_is_variation_selector(uint32_t u) {
  return (u >= 0xFE00 && u <= 0xFE0F) || (u >= 0xE0100 && u <= 0xE01EF);
}

static void gfnt_decompose_current(GFNT_NormCtx * c, bool shortest) {
  uint32_t u = c->in[c->idx].unicode;
  uint32_t glyph = 0;

  // A character the font has a variation sequence for with the selector after it
  // takes the glyph of the sequence, and the selector goes.
  if (c->idx + 1 < c->count && gfnt_is_variation_selector(c->in[c->idx + 1].unicode)) {
    GFNT_UvsKind kind = GFNT_UVS_NONE;
    uint32_t variant = 0;

    if (gfnt_face_variation_glyph(c->face, u, c->in[c->idx + 1].unicode, &variant,
            &kind, NULL) == GFNT_OK
        && (kind == GFNT_UVS_GLYPH
            || (kind == GFNT_UVS_DEFAULT && gfnt_nominal(c, u, &variant)))) {
      uint32_t selector_cluster = c->in[c->idx + 1].cluster;

      gfnt_next_char(c, variant);
      if (!c->oom) {
        GFNT_LInfo * slot = &c->out[c->len - 1];

        if (selector_cluster < slot->cluster) {
          slot->cluster = selector_cluster;
        }
      }
      c->idx++;
      return;
    }
  }

  if (shortest && gfnt_nominal(c, u, &glyph)) {
    gfnt_next_char(c, glyph);
    return;
  }
  if (gfnt_decompose(c, shortest, u)) {
    c->idx++;
    return;
  }
  if (!shortest && gfnt_nominal(c, u, &glyph)) {
    gfnt_next_char(c, glyph);
    return;
  }
  if (c->in[c->idx].gc == GUNI_GC_SPACE_SEPARATOR) {
    uint32_t space = 0;
    uint8_t type = gfnt_u_space_type(u);

    if (type != GFNT_SPACE_NONE && gfnt_nominal(c, 0x20, &space)) {
      // The font has no glyph for this space, and the shaper will size its own.
      gfnt_next_char(c, space);
      if (!c->oom) {
        c->out[c->len - 1].space = type;
      }
      return;
    }
  }
  if (u == 0x2011) {
    uint32_t hyphen = 0;

    // The one no-break form that is not a space: the ordinary hyphen will do.
    if (gfnt_nominal(c, 0x2010, &hyphen)) {
      gfnt_next_char(c, hyphen);
      return;
    }
  }
  gfnt_next_char(c, glyph);
}

/** Sort marks by modified combining class, stably, merging the clusters moved. */
static void gfnt_sort_marks(GFNT_LInfo * info, size_t len, size_t start,
    size_t end) {
  size_t j;

  for (j = start + 1; j < end; j++) {
    size_t i = j;
    GFNT_LInfo moved;

    while (i > start && info[i - 1].mcc > info[j].mcc) {
      i--;
    }
    if (i == j) {
      continue;
    }
    gfnt_merge_clusters(info, len, i, j + 1);
    moved = info[j];
    memmove(&info[i + 1], &info[i], (j - i) * sizeof *info);
    info[i] = moved;
  }
}

bool gfnt_normalize(const GFNT_Face * face, GFNT_LInfo ** info, size_t * len,
    size_t * capacity, const GFNT_Allocator * allocator, GFNT_NormMode mode,
    const GFNT_NormHooks * hooks) {
  GFNT_NormCtx c;
  bool saw_marks;
  bool always_short_circuit = mode == GFNT_NORM_NONE;
  bool might_short_circuit = always_short_circuit
      || (mode != GFNT_NORM_DECOMPOSED
          && mode != GFNT_NORM_COMPOSED_DIACRITICS_NO_SHORT_CIRCUIT);
  size_t count = *len;
  size_t i;

  if (count == 0) {
    return true;
  }
  memset(&c, 0, sizeof c);
  c.face = face;
  c.allocator = allocator;
  c.hooks = hooks;
  c.in = *info;
  c.count = count;
  saw_marks = false;

  // First pass: decompose. A run of characters that are not marks is a run of
  // simple clusters; the last of them is left to cluster with the marks that
  // follow it.
  do {
    size_t end;

    for (end = c.idx + 1; end < count; end++) {
      if (c.in[end].flags & GFNT_GF_MARK) {
        break;
      }
    }
    if (end < count) {
      end--;
    }
    if (might_short_circuit) {
      while (c.idx < end) {
        uint32_t glyph = 0;

        if (!gfnt_nominal(&c, c.in[c.idx].unicode, &glyph)) {
          break;
        }
        gfnt_next_char(&c, glyph);
      }
    }
    while (c.idx < end && !c.oom) {
      gfnt_decompose_current(&c, might_short_circuit);
    }
    if (c.idx == count || c.oom) {
      break;
    }
    saw_marks = true;
    for (end = c.idx + 1; end < count; end++) {
      if (!(c.in[end].flags & GFNT_GF_MARK)) {
        break;
      }
    }
    while (c.idx < end && !c.oom) {
      gfnt_decompose_current(&c, always_short_circuit);
    }
  } while (c.idx < count && !c.oom);
  if (c.oom) {
    allocator->free_fn(allocator->ctx, c.out);
    return false;
  }

  // Second pass: put each run of marks in canonical order.
  {
    GFNT_LInfo * out = c.out;
    size_t n = c.len;

    for (i = 0; i < n; i++) {
      size_t end;

      if (out[i].mcc == 0) {
        continue;
      }
      for (end = i + 1; end < n; end++) {
        if (out[end].mcc == 0) {
          break;
        }
      }
      if (end - i <= GFNT_MAX_COMBINING_MARKS) {
        gfnt_sort_marks(out, n, i, end);
        if (hooks && hooks->reorder_marks) {
          hooks->reorder_marks(out, i, end, hooks->ctx);
        }
      }
      i = end;
    }
    // A CGJ that stopped nothing from reordering need not stop a lookup either.
    for (i = 1; i + 1 < n; i++) {
      if (out[i].unicode == 0x034F
          && (out[i + 1].mcc == 0 || out[i - 1].mcc <= out[i + 1].mcc)) {
        out[i].flags &= (uint8_t)~GFNT_GF_HIDDEN;
      }
    }

    // Third pass: recompose, where the font has the composite.
    // Text with no marks to speak of is not put together again: HarfBuzz leaves
    // Myanmar's UU as the two letters it made when nothing follows it to
    // cluster with.
    if (saw_marks && (mode == GFNT_NORM_COMPOSED_DIACRITICS
        || mode == GFNT_NORM_COMPOSED_DIACRITICS_NO_SHORT_CIRCUIT)) {
      size_t starter = 0;
      size_t w = 1;

      for (i = 1; i < n; i++) {
        uint32_t composed;
        uint32_t glyph = 0;

        if ((out[i].flags & GFNT_GF_MARK)
            && (starter == w - 1 || out[w - 1].mcc < out[i].mcc)
            && gfnt_compose_pair(&c, out[starter].unicode, out[i].unicode,
                   &composed)
            && gfnt_nominal(&c, composed, &glyph)) {
          uint32_t old_cluster = out[i].cluster;
          size_t j;

          out[w] = out[i];
          gfnt_merge_clusters(out, w + 1, starter, w + 1);
          out[starter].cluster = out[w].cluster < out[starter].cluster
              ? out[w].cluster : out[starter].cluster;
          // What follows in the cluster just merged, not yet copied, goes too.
          for (j = i + 1; j < n && out[j].cluster == old_cluster; j++) {
            out[j].cluster = out[starter].cluster;
          }
          out[starter].unicode = composed;
          out[starter].glyph = glyph;
          gfnt_u_set_props(&out[starter]);
          continue;
        }
        out[w++] = out[i];
        if (out[w - 1].mcc == 0) {
          starter = w - 1;
        }
      }
      c.len = w;
    }
  }

  allocator->free_fn(allocator->ctx, *info);
  *info = c.out;
  *len = c.len;
  *capacity = c.capacity;
  return true;
}
