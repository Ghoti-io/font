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
 * Arabic fallback shaping. See arabic_fallback.h.
 */

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/norm.h>
#include <stdlib.h>
#include <string.h>
#include "arabic_fallback.h"
#include "uprops.h"

#define GFNT_AF_FIRST 0x0621u
#define GFNT_AF_LAST 0x06D3u
#define GFNT_AF_LETTERS (GFNT_AF_LAST - GFNT_AF_FIRST + 1)

// The columns of the forms table, which are also the lookups' order.
enum { GFNT_AF_ISOL = 0, GFNT_AF_FINA, GFNT_AF_INIT, GFNT_AF_MEDI, GFNT_AF_ROLES };

typedef struct GFNT_AfLigature {
  uint32_t comps[3];
  uint8_t count;
  uint32_t glyph;
} GFNT_AfLigature;

/** A growing byte string. */
typedef struct GFNT_AfBytes {
  uint8_t * data;
  size_t len;
  size_t capacity;
  bool oom;
} GFNT_AfBytes;

static void gfnt_af_put(GFNT_AfBytes * b, uint32_t value, int width) {
  int i;

  if (b->oom) {
    return;
  }
  if (b->len + (size_t)width > b->capacity) {
    size_t wanted = b->capacity ? b->capacity * 2 : 256;
    uint8_t * grown;

    while (wanted < b->len + (size_t)width) {
      wanted *= 2;
    }
    grown = realloc(b->data, wanted);
    if (!grown) {
      b->oom = true;
      return;
    }
    b->data = grown;
    b->capacity = wanted;
  }
  for (i = width - 1; i >= 0; i--) {
    b->data[b->len++] = (uint8_t)(value >> (8 * i));
  }
}

static void gfnt_af_put16(GFNT_AfBytes * b, uint32_t value) {
  gfnt_af_put(b, value, 2);
}

/** Overwrite a 16-bit field written earlier. */
static void gfnt_af_patch16(GFNT_AfBytes * b, size_t at, uint32_t value) {
  if (!b->oom && at + 2 <= b->len) {
    b->data[at] = (uint8_t)(value >> 8);
    b->data[at + 1] = (uint8_t)value;
  }
}

static int gfnt_af_pair_compare(const void * left, const void * right) {
  const uint32_t * a = left;
  const uint32_t * b = right;

  return a[0] < b[0] ? -1 : a[0] > b[0];
}

/**
 * The compatibility decomposition of a presentation form, with its canonical
 * parts put back together: U+FE83 decomposes to ALEF and HAMZA ABOVE, and what the
 * table wants is the one letter U+0623 that those compose to.
 */
static bool gfnt_af_decompose(uint32_t x, uint32_t * out, size_t * count) {
  uint32_t full[18];
  size_t n = 0;
  size_t starter = 0;
  size_t w = 1;
  size_t i;

  if (guni_decompose(x, true, full, 18, &n) != GUNI_OK || n == 0) {
    return false;
  }
  out[0] = full[0];
  for (i = 1; i < n; i++) {
    uint32_t composed = 0;

    if (guni_combining_class(full[i]) != 0
        && (starter == w - 1 || guni_combining_class(out[w - 1])
            < guni_combining_class(full[i]))) {
      composed = guni_compose(out[starter], full[i]);
    }
    if (composed) {
      out[starter] = composed;
    }
    else {
      out[w++] = full[i];
      if (guni_combining_class(full[i]) == 0) {
        starter = w - 1;
      }
    }
  }
  *count = w;
  return true;
}

/** The presentation forms of each Arabic letter, from the compatibility mappings. */
static void gfnt_af_forms(uint16_t forms[GFNT_AF_LETTERS][GFNT_AF_ROLES]) {
  static const uint32_t ranges[][2] = {{0xFB50, 0xFDFF}, {0xFE70, 0xFEFF}};
  uint32_t letters[GFNT_AF_LETTERS][4];
  size_t letter_len[GFNT_AF_LETTERS];
  size_t i;
  size_t r;

  memset(forms, 0, sizeof forms[0] * GFNT_AF_LETTERS);
  // Each letter's own full compatibility decomposition, to be matched against a
  // form's: U+FBDD is the isolated form of U+0677, and both decompose to the two
  // characters U+06C7 U+0674.
  for (i = 0; i < GFNT_AF_LETTERS; i++) {
    uint32_t full[18];
    size_t n = 0;

    letter_len[i] = 0;
    if (guni_decompose(GFNT_AF_FIRST + (uint32_t)i, true, full, 18, &n)
            == GUNI_OK && n >= 1 && n <= 4) {
      memcpy(letters[i], full, n * sizeof full[0]);
      letter_len[i] = n;
    }
  }
  for (r = 0; r < 2; r++) {
    uint32_t x;

    for (x = ranges[r][0]; x <= ranges[r][1]; x++) {
      uint32_t full[18];
      size_t n = 0;
      int role;

      switch (guni_decomposition_type(x)) {
        case GUNI_DT_ISOLATED: role = GFNT_AF_ISOL; break;
        case GUNI_DT_FINAL: role = GFNT_AF_FINA; break;
        case GUNI_DT_INITIAL: role = GFNT_AF_INIT; break;
        case GUNI_DT_MEDIAL: role = GFNT_AF_MEDI; break;
        default: continue;
      }
      if (guni_decompose(x, true, full, 18, &n) != GUNI_OK || n < 1 || n > 4) {
        continue;
      }
      for (i = 0; i < GFNT_AF_LETTERS; i++) {
        if (letter_len[i] == n && memcmp(letters[i], full, n * sizeof full[0])
                == 0) {
          // The letter itself, not a form of a form.
          if (!forms[i][role]) {
            forms[i][role] = (uint16_t)x;
          }
          break;
        }
      }
    }
  }
}

static uint32_t gfnt_af_glyph(const GFNT_Face * face, uint32_t u) {
  uint32_t glyph = 0;

  if (!u || gfnt_face_glyph_for_codepoint(face, u, &glyph, NULL) != GFNT_OK) {
    return 0;
  }
  return glyph;
}

/** One single-substitution lookup, appended; false when there is nothing for it. */
static bool gfnt_af_single(GFNT_AfBytes * out, const GFNT_Face * face,
    uint16_t forms[GFNT_AF_LETTERS][GFNT_AF_ROLES], int role) {
  uint32_t pairs[GFNT_AF_LETTERS][2];
  size_t n = 0;
  size_t i;
  size_t base;

  for (i = 0; i < GFNT_AF_LETTERS; i++) {
    uint32_t s = forms[i][role];
    uint32_t from;
    uint32_t to;

    if (!s) {
      continue;
    }
    from = gfnt_af_glyph(face, GFNT_AF_FIRST + (uint32_t)i);
    to = gfnt_af_glyph(face, s);
    if (!from || !to) {
      continue;
    }
    pairs[n][0] = from;
    pairs[n][1] = to;
    n++;
  }
  if (!n) {
    return false;
  }
  qsort(pairs, n, sizeof pairs[0], gfnt_af_pair_compare);
  base = out->len;
  // Lookup: type 1, flag IgnoreMarks, one subtable 8 bytes in.
  gfnt_af_put16(out, 1);
  gfnt_af_put16(out, 0x0008);
  gfnt_af_put16(out, 1);
  gfnt_af_put16(out, 8);
  // SingleSubstFormat2, then the coverage it points at.
  gfnt_af_put16(out, 2);
  gfnt_af_put16(out, (uint32_t)(6 + 2 * n));
  gfnt_af_put16(out, (uint32_t)n);
  for (i = 0; i < n; i++) {
    gfnt_af_put16(out, pairs[i][1]);
  }
  gfnt_af_put16(out, 1);
  gfnt_af_put16(out, (uint32_t)n);
  for (i = 0; i < n; i++) {
    gfnt_af_put16(out, pairs[i][0]);
  }
  (void)base;
  return !out->oom;
}

static int gfnt_af_ligature_compare(const void * left, const void * right) {
  const GFNT_AfLigature * a = left;
  const GFNT_AfLigature * b = right;

  if (a->comps[0] != b->comps[0]) {
    return a->comps[0] < b->comps[0] ? -1 : 1;
  }
  // The longer match first, as a ligature set is tried in order.
  if (a->count != b->count) {
    return a->count > b->count ? -1 : 1;
  }
  return 0;
}

/**
 * A ligature lookup: either the compatibility ligatures of letters (which skip
 * marks, so a vowel between a lam and an alef does not stop them) or those of two
 * marks (which cannot skip them, being made of them).
 */
static bool gfnt_af_ligatures(GFNT_AfBytes * out, const GFNT_Face * face,
    uint16_t forms[GFNT_AF_LETTERS][GFNT_AF_ROLES], bool marks) {
  static const uint32_t ranges[][2] = {{0xFB50, 0xFDFF}, {0xFE70, 0xFEFF}};
  GFNT_AfLigature * ligs = NULL;
  size_t count = 0;
  size_t capacity = 0;
  size_t r;
  size_t i;
  size_t sets = 0;
  size_t set_offsets_at;
  size_t lig_offsets[64];
  bool ok = true;

  for (r = 0; r < 2; r++) {
    uint32_t x;

    for (x = ranges[r][0]; x <= ranges[r][1]; x++) {
      uint32_t out_cps[18];
      size_t n = 0;
      GFNT_AfLigature lig;
      size_t k;
      int outer;

      switch (guni_decomposition_type(x)) {
        case GUNI_DT_ISOLATED: outer = 0; break;
        case GUNI_DT_FINAL: outer = 1; break;
        case GUNI_DT_INITIAL: outer = 2; break;
        case GUNI_DT_MEDIAL: outer = 3; break;
        default: continue;
      }
      if (!gfnt_af_decompose(x, out_cps, &n) || n < 2 || n > 3) {
        continue;
      }
      if (marks != (outer == 0 && n == 3 && out_cps[0] == 0x0020)) {
        continue;
      }
      if (outer == 0 && n == 3 && out_cps[0] == 0x0020) {
        // A space and two marks: the isolated forms of a shadda with a vowel. What
        // the font gets is the two marks, in the order they sort to, and not the
        // space, which only names the form.
        uint32_t a = out_cps[1];
        uint32_t b = out_cps[2];

        if (gfnt_u_modified_ccc(a) > gfnt_u_modified_ccc(b)) {
          uint32_t t = a;

          a = b;
          b = t;
        }
        memset(&lig, 0, sizeof lig);
        lig.count = 2;
        lig.glyph = gfnt_af_glyph(face, x);
        lig.comps[0] = gfnt_af_glyph(face, a);
        lig.comps[1] = gfnt_af_glyph(face, b);
        if (lig.glyph && lig.comps[0] && lig.comps[1]) {
          if (count == capacity) {
            size_t wanted = capacity ? capacity * 2 : 64;
            GFNT_AfLigature * grown = realloc(ligs, wanted * sizeof *ligs);

            if (!grown) {
              free(ligs);
              return false;
            }
            ligs = grown;
            capacity = wanted;
          }
          ligs[count++] = lig;
        }
        continue;
      }
      memset(&lig, 0, sizeof lig);
      lig.count = (uint8_t)n;
      lig.glyph = gfnt_af_glyph(face, x);
      for (k = 0; k < n && lig.glyph; k++) {
        // The first letter is initial or medial, the last final or medial, and the
        // ones between medial: the form each takes when the ligature is the form
        // asked for.
        int role;
        uint32_t c = out_cps[k];

        if (c < GFNT_AF_FIRST || c > GFNT_AF_LAST) {
          lig.glyph = 0;
          break;
        }
        if (k == 0) {
          role = (outer == 0 || outer == 2) ? GFNT_AF_INIT : GFNT_AF_MEDI;
        }
        else if (k + 1 == n) {
          role = (outer == 0 || outer == 1) ? GFNT_AF_FINA : GFNT_AF_MEDI;
        }
        else {
          role = GFNT_AF_MEDI;
        }
        lig.comps[k] = gfnt_af_glyph(face, forms[c - GFNT_AF_FIRST][role]);
        if (!lig.comps[k]) {
          lig.glyph = 0;
        }
      }
      if (!lig.glyph) {
        continue;
      }
      if (count == capacity) {
        size_t wanted = capacity ? capacity * 2 : 64;
        GFNT_AfLigature * grown = realloc(ligs, wanted * sizeof *ligs);

        if (!grown) {
          free(ligs);
          return false;
        }
        ligs = grown;
        capacity = wanted;
      }
      ligs[count++] = lig;
    }
  }
  if (!count) {
    free(ligs);
    return false;
  }
  qsort(ligs, count, sizeof *ligs, gfnt_af_ligature_compare);
  // Lookup: type 4, one subtable.
  gfnt_af_put16(out, 4);
  gfnt_af_put16(out, marks ? 0 : 0x0008);
  gfnt_af_put16(out, 1);
  gfnt_af_put16(out, 8);
  {
    size_t subtable = out->len;
    size_t first;
    size_t cover_at;

    for (i = 0; i < count; i++) {
      if (i == 0 || ligs[i].comps[0] != ligs[i - 1].comps[0]) {
        sets++;
      }
    }
    gfnt_af_put16(out, 1);                 // format
    cover_at = out->len;
    gfnt_af_put16(out, 0);                 // coverage offset, patched below
    gfnt_af_put16(out, (uint32_t)sets);
    set_offsets_at = out->len;
    for (i = 0; i < sets; i++) {
      gfnt_af_put16(out, 0);
    }
    // One ligature set per first glyph.
    first = 0;
    for (i = 0; i < sets && first < count; i++) {
      size_t last = first;
      size_t set_start = out->len;
      size_t k;
      size_t n = 0;

      while (last < count && ligs[last].comps[0] == ligs[first].comps[0]) {
        last++;
      }
      n = last - first;
      if (n > 64) {
        ok = false;
        n = 64;
      }
      gfnt_af_patch16(out, set_offsets_at + 2 * i,
          (uint32_t)(set_start - subtable));
      gfnt_af_put16(out, (uint32_t)n);
      for (k = 0; k < n; k++) {
        gfnt_af_put16(out, 0);
      }
      for (k = 0; k < n; k++) {
        const GFNT_AfLigature * lig = &ligs[first + k];
        size_t c;

        lig_offsets[k] = out->len - set_start;
        gfnt_af_patch16(out, set_start + 2 + 2 * k, (uint32_t)lig_offsets[k]);
        gfnt_af_put16(out, lig->glyph);
        gfnt_af_put16(out, lig->count);
        for (c = 1; c < lig->count; c++) {
          gfnt_af_put16(out, lig->comps[c]);
        }
      }
      first = last;
    }
    gfnt_af_patch16(out, cover_at, (uint32_t)(out->len - subtable));
    gfnt_af_put16(out, 1);
    gfnt_af_put16(out, (uint32_t)sets);
    for (first = 0; first < count; first++) {
      if (first == 0 || ligs[first].comps[0] != ligs[first - 1].comps[0]) {
        gfnt_af_put16(out, ligs[first].comps[0]);
      }
    }
  }
  free(ligs);
  return ok && !out->oom;
}

void gfnt_arabic_fallback_shape(GFNT_ShapeCtx * ctx,
    const uint32_t masks[GFNT_AR_FALLBACK_LOOKUPS]) {
  uint16_t (*forms)[GFNT_AF_ROLES];
  GFNT_AfBytes lookups = {0};
  size_t offsets[GFNT_AR_FALLBACK_LOOKUPS];
  uint32_t lookup_mask[GFNT_AR_FALLBACK_LOOKUPS];
  size_t n = 0;
  int i;
  GFNT_AfBytes table = {0};
  GFNT_LayoutTable lt;
  GFNT_LApply c;
  size_t list_at;

  forms = malloc(sizeof *forms * GFNT_AF_LETTERS);
  if (!forms) {
    ctx->oom = true;
    return;
  }
  gfnt_af_forms(forms);
  for (i = 0; i < GFNT_AR_FALLBACK_LOOKUPS; i++) {
    size_t at = lookups.len;
    bool made;

    if (!masks[i]) {
      continue;
    }
    made = i < 4 ? gfnt_af_single(&lookups, ctx->face, forms, i)
                 : gfnt_af_ligatures(&lookups, ctx->face, forms, i == 5);
    if (made) {
      offsets[n] = at;
      lookup_mask[n] = masks[i];
      n++;
    }
  }
  free(forms);
  if (lookups.oom) {
    free(lookups.data);
    ctx->oom = true;
    return;
  }
  if (!n) {
    free(lookups.data);
    return;
  }
  // A `GSUB` table: the header, a lookup list, and the lookups.
  gfnt_af_put16(&table, 1);
  gfnt_af_put16(&table, 0);
  gfnt_af_put16(&table, 0);     // no scripts
  gfnt_af_put16(&table, 0);     // no features
  gfnt_af_put16(&table, 10);    // the lookup list
  list_at = table.len;
  gfnt_af_put16(&table, (uint32_t)n);
  for (i = 0; (size_t)i < n; i++) {
    gfnt_af_put16(&table, (uint32_t)(2 + 2 * n + offsets[i]));
  }
  for (i = 0; (size_t)i < lookups.len; i++) {
    gfnt_af_put(&table, lookups.data[i], 1);
  }
  (void)list_at;
  free(lookups.data);
  if (table.oom) {
    free(table.data);
    ctx->oom = true;
    return;
  }
  memset(&lt, 0, sizeof lt);
  if (gfnt_reader_init(&lt.table, table.data, table.len, GFNT_TAG_GSUB, NULL)
      != GFNT_OK) {
    free(table.data);
    return;
  }
  lt.tag = GFNT_TAG_GSUB;
  lt.lookup_list = 10;
  lt.lookup_count = (uint32_t)n;
  memset(&c, 0, sizeof c);
  c.buf = ctx->buf;
  c.face = ctx->face;
  c.variation = ctx->options->variation;
  c.lt = &lt;
  c.gdef = ctx->gdef;
  c.is_gpos = false;
  c.rtl = ctx->native_rtl;
  c.auto_zwnj = true;
  c.auto_zwj = true;
  for (i = 0; (size_t)i < n; i++) {
    c.lookup_mask = lookup_mask[i];
    gfnt_l_apply_lookup_to_buffer(&c, (uint32_t)i);
    if (c.buf->oom) {
      ctx->oom = true;
      break;
    }
  }
  free(table.data);
}
