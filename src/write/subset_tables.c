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
 * The tables indexed by glyph that are not layout tables: horizontal and
 * vertical metrics, `VORG`, and the legacy `kern` table, for a new numbering.
 */

#include <stdlib.h>
#include <string.h>
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "write.h"

#define NO_GLYPH 0xFFFFFFFFu

GFNT_Result gfnt_subset_metrics(const GFNT_Allocator * a, const uint8_t * data,
    size_t span, size_t nhm, const uint32_t * old_of_new, size_t new_count,
    GFNT_WBuf * out, size_t * out_metrics, GFNT_Tag tag, GFNT_Error * error) {
  uint16_t * adv = a->calloc_fn(a->ctx, new_count ? new_count : 1, sizeof *adv);
  int16_t * lsb = a->calloc_fn(a->ctx, new_count ? new_count : 1, sizeof *lsb);
  size_t metrics = new_count;
  size_t i;

  if (!adv || !lsb) {
    a->free_fn(a->ctx, adv);
    a->free_fn(a->ctx, lsb);
    return gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE,
        "allocating the metrics");
  }
  for (i = 0; i < new_count; ++i) {
    uint32_t old = old_of_new[i];
    size_t a_at;
    size_t l_at;

    if (old == NO_GLYPH) {
      continue;
    }
    a_at = 4 * (old < nhm ? (size_t)old : nhm - 1);
    l_at = old < nhm ? 4 * (size_t)old + 2 : 4 * nhm + 2 * ((size_t)old - nhm);
    if (a_at + 2 <= span) {
      adv[i] = (uint16_t)((data[a_at] << 8) | data[a_at + 1]);
    }
    if (l_at + 2 <= span) {
      lsb[i] = (int16_t)((data[l_at] << 8) | data[l_at + 1]);
    }
  }
  // The trailing advance repeats, so metrics that end the same collapse into it.
  while (metrics > 1 && adv[metrics - 2] == adv[new_count - 1]) {
    --metrics;
  }
  for (i = 0; i < new_count; ++i) {
    if (i < metrics) {
      gfnt_wbuf_u16(out, adv[i]);
    }
    gfnt_wbuf_u16(out, (uint16_t)lsb[i]);
  }
  a->free_fn(a->ctx, adv);
  a->free_fn(a->ctx, lsb);
  *out_metrics = metrics;
  return out->oom ? gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE,
      "building the metrics") : GFNT_OK;
}

GFNT_Result gfnt_subset_vorg(const GFNT_Face * face, const uint32_t * new_of_old,
    size_t count, GFNT_WBuf * out, GFNT_Error * error) {
  GFNT_Reader r;
  uint16_t major = 0;
  uint16_t minor = 0;
  uint16_t def = 0;
  uint16_t n = 0;
  uint16_t i;
  size_t kept = 0;
  GFNT_WBuf body;

  if (!gfnt_face_has_table(face, GFNT_TAG('V', 'O', 'R', 'G'))
      || gfnt_face_table_reader(face, GFNT_TAG('V', 'O', 'R', 'G'), &r, NULL) != GFNT_OK) {
    return GFNT_OK;
  }
  r.error = NULL;
  if (gfnt_reader_u16_at(&r, 0, &major) != GFNT_OK
      || gfnt_reader_u16_at(&r, 2, &minor) != GFNT_OK
      || gfnt_reader_u16_at(&r, 4, &def) != GFNT_OK
      || gfnt_reader_u16_at(&r, 6, &n) != GFNT_OK || major != 1) {
    return GFNT_OK;   // A table nothing can read says nothing.
  }
  gfnt_wbuf_init(&body, out->allocator);
  for (i = 0; i < n; ++i) {
    uint16_t glyph = 0;
    uint16_t y = 0;

    if (gfnt_reader_u16_at(&r, 8 + 4 * (size_t)i, &glyph) != GFNT_OK
        || gfnt_reader_u16_at(&r, 10 + 4 * (size_t)i, &y) != GFNT_OK) {
      gfnt_wbuf_free(&body);
      return GFNT_OK;
    }
    if (glyph < count && new_of_old[glyph] != NO_GLYPH) {
      gfnt_wbuf_u16(&body, new_of_old[glyph]);
      gfnt_wbuf_u16(&body, y);
      ++kept;
    }
  }
  gfnt_wbuf_u16(out, 1);
  gfnt_wbuf_u16(out, minor);
  gfnt_wbuf_u16(out, def);
  gfnt_wbuf_u16(out, (uint32_t)kept);
  gfnt_wbuf_bytes(out, body.data, body.length);
  gfnt_wbuf_free(&body);
  return out->oom ? gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG('V', 'O', 'R', 'G'), 0,
      GFNT_GLYPH_NONE, "building VORG") : GFNT_OK;
}

/** Whether the bit-set array already holds @p v; record it. */
static void mark(uint8_t * set, uint32_t v) {
  set[v] = 1;
}

/**
 * One class-based (format 2) subtable, rewritten for the glyphs that can occur.
 *
 * A pair kerns when its left glyph is in the left class table with a row at or
 * after the array, and its right glyph is in the right class table (see
 * layout_tables/kern.c). Rows and columns that no kept glyph uses are dropped and
 * the offsets renumbered. A kept glyph that was outside a class table stays
 * outside: a gap in a rewritten right table holds an offset past the subtable's
 * end, which names no cell, and a gap in the left table holds 0.
 */
static GFNT_Result kern_format2(const GFNT_Reader * r, size_t at, size_t header,
    size_t length, const uint8_t * shape, const uint32_t * new_of_old, size_t count,
    const GFNT_Allocator * a, GFNT_WBuf * body, bool * kept, GFNT_Error * error) {
  const GFNT_Tag tag = GFNT_TAG('k', 'e', 'r', 'n');
  size_t b = at + header;
  uint16_t row_w = 0, left_off = 0, right_off = 0, array_off = 0;
  uint16_t lf = 0, ln = 0, rf = 0, rn = 0;
  size_t lt;
  size_t rt;
  uint8_t * lused = NULL;
  uint8_t * rused = NULL;
  uint16_t * lnew = NULL;
  uint16_t * rnew = NULL;
  uint16_t * lval = NULL;
  uint16_t * rval = NULL;
  uint8_t * rin = NULL;
  uint32_t maxnew = 0;
  size_t rows = 0;
  size_t cols = 0;
  size_t g;
  size_t i;
  size_t j;
  GFNT_Result result = GFNT_OK;

  *kept = false;
  if (gfnt_reader_u16_at(r, b, &row_w) != GFNT_OK
      || gfnt_reader_u16_at(r, b + 2, &left_off) != GFNT_OK
      || gfnt_reader_u16_at(r, b + 4, &right_off) != GFNT_OK
      || gfnt_reader_u16_at(r, b + 6, &array_off) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, b, GFNT_GLYPH_NONE,
        "a kern subtable cut short");
  }
  (void)row_w;
  lt = at + left_off;
  rt = at + right_off;
  if (gfnt_reader_u16_at(r, lt, &lf) != GFNT_OK || gfnt_reader_u16_at(r, lt + 2, &ln) != GFNT_OK
      || gfnt_reader_u16_at(r, rt, &rf) != GFNT_OK
      || gfnt_reader_u16_at(r, rt + 2, &rn) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, at, GFNT_GLYPH_NONE,
        "a kern class table cut short");
  }
  for (g = 0; g < count; ++g) {
    if (shape[g] && new_of_old[g] != NO_GLYPH && new_of_old[g] > maxnew) {
      maxnew = new_of_old[g];
    }
  }
  lused = a->calloc_fn(a->ctx, 65536, 1);
  rused = a->calloc_fn(a->ctx, 65536, 1);
  lnew = a->calloc_fn(a->ctx, 65536, sizeof *lnew);
  rnew = a->calloc_fn(a->ctx, 65536, sizeof *rnew);
  lval = a->calloc_fn(a->ctx, (size_t)maxnew + 1, sizeof *lval);
  rval = a->calloc_fn(a->ctx, (size_t)maxnew + 1, sizeof *rval);
  rin = a->calloc_fn(a->ctx, (size_t)maxnew + 1, 1);
  if (!lused || !rused || !lnew || !rnew || !lval || !rval || !rin) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE, "rewriting kern");
    goto done;
  }
  for (g = 0; g < count; ++g) {
    uint16_t l = 0;
    uint16_t k = 0;

    if (!shape[g] || new_of_old[g] == NO_GLYPH) {
      continue;
    }
    if (g >= lf && g - lf < ln) {
      if (gfnt_reader_u16_at(r, lt + 4 + 2 * (g - lf), &l) != GFNT_OK) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, lt, (uint32_t)g,
            "a kern class table cut short");
        goto done;
      }
      if (l < array_off) {
        l = 0;     // A row before the array names no cell.
      }
    }
    if (g >= rf && g - rf < rn) {
      if (gfnt_reader_u16_at(r, rt + 4 + 2 * (g - rf), &k) != GFNT_OK) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, rt, (uint32_t)g,
            "a kern class table cut short");
        goto done;
      }
      rin[new_of_old[g]] = 1;
      rval[new_of_old[g]] = k;
      mark(rused, k);
    }
    lval[new_of_old[g]] = l;
    if (l) {
      mark(lused, l);
    }
  }
  for (i = 0; i < 65536; ++i) {
    if (lused[i]) {
      lnew[i] = (uint16_t)rows++;
    }
    if (rused[i]) {
      rnew[i] = (uint16_t)cols++;
    }
  }
  if (rows == 0 || cols == 0) {
    goto done;    // No kept pair can kern: nothing of this subtable is kept.
  }
  {
    size_t lo_l = (size_t)-1, hi_l = 0, lo_r = (size_t)-1, hi_r = 0;
    size_t row_width = 2 * cols;
    size_t left_at = header + 8;
    size_t span_l;
    size_t span_r;
    size_t right_at;
    size_t array_at;
    size_t total;

    for (i = 0; i <= maxnew; ++i) {
      if (lval[i]) {
        lo_l = lo_l == (size_t)-1 ? i : lo_l;
        hi_l = i;
      }
      if (rin[i]) {
        lo_r = lo_r == (size_t)-1 ? i : lo_r;
        hi_r = i;
      }
    }
    span_l = hi_l - lo_l + 1;
    span_r = hi_r - lo_r + 1;
    right_at = left_at + 4 + 2 * span_l;
    array_at = right_at + 4 + 2 * span_r;
    total = array_at + rows * row_width;
    if (total > 0xFFFFu || array_at + rows * row_width + 2 > 0xFFFFu) {
      result = gfnt_error_set(error, GFNT_ERR_LIMIT, tag, at, GFNT_GLYPH_NONE,
          "a rewritten kern subtable does not fit its 16-bit offsets");
      goto done;
    }
    gfnt_wbuf_u16(body, (uint32_t)row_width);
    gfnt_wbuf_u16(body, (uint32_t)left_at);
    gfnt_wbuf_u16(body, (uint32_t)right_at);
    gfnt_wbuf_u16(body, (uint32_t)array_at);
    gfnt_wbuf_u16(body, (uint32_t)lo_l);
    gfnt_wbuf_u16(body, (uint32_t)span_l);
    for (i = 0; i < span_l; ++i) {
      gfnt_wbuf_u16(body, lval[lo_l + i]
          ? (uint32_t)(array_at + (size_t)lnew[lval[lo_l + i]] * row_width) : 0);
    }
    gfnt_wbuf_u16(body, (uint32_t)lo_r);
    gfnt_wbuf_u16(body, (uint32_t)span_r);
    for (i = 0; i < span_r; ++i) {
      gfnt_wbuf_u16(body, rin[lo_r + i] ? (uint32_t)rnew[rval[lo_r + i]] * 2u : 0xFFFEu);
    }
    for (i = 0; i < 65536; ++i) {
      if (!lused[i]) {
        continue;
      }
      for (j = 0; j < 65536; ++j) {
        uint16_t value = 0;

        if (!rused[j]) {
          continue;
        }
        if (i + j + 2 > length || gfnt_reader_u16_at(r, at + i + j, &value) != GFNT_OK) {
          value = 0;   // Past the subtable: the shaper reads zero there.
        }
        gfnt_wbuf_u16(body, value);
      }
    }
    *kept = true;
  }
done:
  a->free_fn(a->ctx, lused);
  a->free_fn(a->ctx, rused);
  a->free_fn(a->ctx, lnew);
  a->free_fn(a->ctx, rnew);
  a->free_fn(a->ctx, lval);
  a->free_fn(a->ctx, rval);
  a->free_fn(a->ctx, rin);
  return result;
}

GFNT_Result gfnt_subset_kern(const GFNT_Face * face, const uint8_t * shape,
    const uint32_t * new_of_old, size_t count, GFNT_WBuf * out,
    const GFNT_Allocator * a, GFNT_Error * error) {
  const GFNT_Tag tag = GFNT_TAG('k', 'e', 'r', 'n');
  GFNT_Reader r;
  uint16_t version = 0;
  uint16_t minor = 0;
  uint32_t tables = 0;
  bool apple;
  size_t at;
  size_t written = 0;
  size_t t;
  GFNT_WBuf body;
  GFNT_Result result = GFNT_OK;

  if (!gfnt_face_has_table(face, tag) || gfnt_face_table_reader(face, tag, &r, NULL) != GFNT_OK) {
    return GFNT_OK;
  }
  r.error = NULL;
  if (gfnt_reader_u16_at(&r, 0, &version) != GFNT_OK
      || gfnt_reader_u16_at(&r, 2, &minor) != GFNT_OK) {
    return GFNT_OK;   // Too short to say anything.
  }
  apple = version == 1 && minor == 0;
  if (apple) {
    if (gfnt_reader_u32_at(&r, 4, &tables) != GFNT_OK) {
      return GFNT_OK;
    }
    at = 8;
  }
  else if (version == 0) {
    tables = minor;
    at = 4;
  }
  else {
    return GFNT_OK;   // A version the shaper does not read either.
  }
  gfnt_wbuf_init(&body, a);
  for (t = 0; t < tables; ++t) {
    size_t header = apple ? 8 : 6;
    uint32_t length = 0;
    uint16_t coverage = 0;
    uint16_t pairs = 0;
    uint32_t format;
    size_t start = body.length;
    size_t kept_pairs = 0;
    bool kept = true;
    size_t i;

    if (apple) {
      if (gfnt_reader_u32_at(&r, at, &length) != GFNT_OK
          || gfnt_reader_u16_at(&r, at + 4, &coverage) != GFNT_OK) {
        break;   // The shaper stops reading here too.
      }
    }
    else {
      uint16_t l16 = 0;

      if (gfnt_reader_u16_at(&r, at + 2, &l16) != GFNT_OK
          || gfnt_reader_u16_at(&r, at + 4, &coverage) != GFNT_OK) {
        break;
      }
      length = l16;
    }
    format = apple ? (coverage & 0xFFu) : (uint32_t)(coverage >> 8);
    if (length < header) {
      break;
    }
    if (format != 0 && format != 2) {
      result = gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, at, GFNT_GLYPH_NONE,
          "a kern subtable of a form that cannot be renumbered");
      goto done;
    }
    // The header: length (patched below), coverage, and Apple's tuple index.
    if (apple) {
      uint16_t tuple = 0;

      gfnt_reader_u16_at(&r, at + 6, &tuple);
      gfnt_wbuf_u32(&body, 0);
      gfnt_wbuf_u16(&body, coverage);
      gfnt_wbuf_u16(&body, tuple);
    }
    else {
      gfnt_wbuf_u16(&body, 0);
      gfnt_wbuf_u16(&body, 0);
      gfnt_wbuf_u16(&body, coverage);
    }
    if (format == 0) {
      size_t power = 1;
      size_t entry = 0;

      if (gfnt_reader_u16_at(&r, at + header, &pairs) != GFNT_OK) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, at, GFNT_GLYPH_NONE,
            "a kern subtable cut short");
        goto done;
      }
      gfnt_wbuf_u16(&body, 0);
      gfnt_wbuf_u16(&body, 0);
      gfnt_wbuf_u16(&body, 0);
      gfnt_wbuf_u16(&body, 0);
      for (i = 0; i < pairs; ++i) {
        uint16_t left = 0;
        uint16_t right = 0;
        uint16_t value = 0;
        size_t p = at + header + 8 + 6 * i;

        if (gfnt_reader_u16_at(&r, p, &left) != GFNT_OK
            || gfnt_reader_u16_at(&r, p + 2, &right) != GFNT_OK
            || gfnt_reader_u16_at(&r, p + 4, &value) != GFNT_OK) {
          result = gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, p, GFNT_GLYPH_NONE,
              "a kern subtable cut short");
          goto done;
        }
        if (left < count && right < count && shape[left] && shape[right]) {
          gfnt_wbuf_u16(&body, new_of_old[left]);
          gfnt_wbuf_u16(&body, new_of_old[right]);
          gfnt_wbuf_u16(&body, value);
          ++kept_pairs;
        }
      }
      kept = kept_pairs > 0;
      while (power * 2 <= kept_pairs) {
        power *= 2;
        ++entry;
      }
      if (kept && !body.oom) {
        gfnt_write_put16(body.data + start + header, (uint32_t)kept_pairs);
        gfnt_write_put16(body.data + start + header + 2, (uint32_t)(6 * power));
        gfnt_write_put16(body.data + start + header + 4, (uint32_t)entry);
        gfnt_write_put16(body.data + start + header + 6, (uint32_t)(6 * (kept_pairs - power)));
      }
    }
    else {
      result = kern_format2(&r, at, header, length, shape, new_of_old, count, a, &body, &kept, error);
      if (result != GFNT_OK) {
        goto done;
      }
    }
    if (kept) {
      size_t made = body.length - start;

      if (!apple && made > 0xFFFFu) {
        result = gfnt_error_set(error, GFNT_ERR_LIMIT, tag, at, GFNT_GLYPH_NONE,
            "a rewritten kern subtable does not fit its 16-bit length");
        goto done;
      }
      if (!body.oom) {
        if (apple) {
          gfnt_write_put32(body.data + start, (uint32_t)made);
        }
        else {
          gfnt_write_put16(body.data + start + 2, (uint32_t)made);
        }
      }
      ++written;
    }
    else {
      body.length = start;
    }
    at += length;
  }
  if (written && !body.oom) {
    if (apple) {
      gfnt_wbuf_u16(out, 1);
      gfnt_wbuf_u16(out, 0);
      gfnt_wbuf_u32(out, (uint32_t)written);
    }
    else {
      gfnt_wbuf_u16(out, 0);
      gfnt_wbuf_u16(out, (uint32_t)written);
    }
    gfnt_wbuf_bytes(out, body.data, body.length);
  }
done:
  gfnt_wbuf_free(&body);
  if (result == GFNT_OK && (out->oom || body.oom)) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE, "building kern");
  }
  return result;
}
