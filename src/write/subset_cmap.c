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

#include <stdlib.h>
#include <string.h>
#include "write.h"

#define GFNT_TAG_CMAP GFNT_TAG('c', 'm', 'a', 'p')

/** One format 4 segment. */
typedef struct GFNT_CmapSegment {
  uint32_t start;
  uint32_t end;
  uint32_t delta; ///< Glyph minus codepoint, modulo 65536.
} GFNT_CmapSegment;

/**
 * Format 4, delta segments only, for the codepoints up to U+FFFF.
 *
 * A segment is a run of consecutive codepoints whose glyphs are consecutive too,
 * so the run is one delta and `idRangeOffset` is zero. The last segment is the
 * mandatory 0xFFFF terminator.
 */
static GFNT_Result gfnt_cmap_format4(const uint32_t * cps, const uint32_t * gids,
    size_t bmp, GFNT_WBuf * out, bool * out_fits, GFNT_Error * error) {
  const GFNT_Allocator * a = out->allocator;
  GFNT_CmapSegment * seg;
  size_t nseg = 0;
  size_t i;
  size_t length;
  unsigned entry_selector = 0;
  unsigned search_range;

  *out_fits = true;
  seg = a->calloc_fn(a->ctx, bmp + 1, sizeof *seg);
  if (!seg) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_CMAP, 0,
        GFNT_GLYPH_NONE, "allocating the cmap segments");
  }
  for (i = 0; i < bmp; ++i) {
    uint32_t delta = (gids[i] - cps[i]) & 0xFFFFu;

    if (nseg && seg[nseg - 1].end + 1 == cps[i] && seg[nseg - 1].delta == delta) {
      seg[nseg - 1].end = cps[i];
    }
    else {
      seg[nseg].start = seg[nseg].end = cps[i];
      seg[nseg].delta = delta;
      ++nseg;
    }
  }
  if (nseg == 0 || seg[nseg - 1].end != 0xFFFFu) {
    seg[nseg].start = seg[nseg].end = 0xFFFFu;
    seg[nseg].delta = 1;
    ++nseg;
  }
  length = 16 + 8 * nseg;
  if (length > 0xFFFFu) {
    // The length field is 16 bits; a subset this sparse is written as format 12.
    *out_fits = false;
    a->free_fn(a->ctx, seg);
    return GFNT_OK;
  }
  while ((2u << entry_selector) <= nseg) {
    ++entry_selector;
  }
  search_range = 2u * (1u << entry_selector);
  gfnt_wbuf_u16(out, 4);
  gfnt_wbuf_u16(out, (uint32_t)length);
  gfnt_wbuf_u16(out, 0);
  gfnt_wbuf_u16(out, (uint32_t)(2 * nseg));
  gfnt_wbuf_u16(out, search_range);
  gfnt_wbuf_u16(out, entry_selector);
  gfnt_wbuf_u16(out, (uint32_t)(2 * nseg) - search_range);
  for (i = 0; i < nseg; ++i) {
    gfnt_wbuf_u16(out, seg[i].end);
  }
  gfnt_wbuf_u16(out, 0);
  for (i = 0; i < nseg; ++i) {
    gfnt_wbuf_u16(out, seg[i].start);
  }
  for (i = 0; i < nseg; ++i) {
    gfnt_wbuf_u16(out, seg[i].delta);
  }
  for (i = 0; i < nseg; ++i) {
    gfnt_wbuf_u16(out, 0);
  }
  a->free_fn(a->ctx, seg);
  return GFNT_OK;
}

/** Format 12: groups of consecutive codepoints with consecutive glyphs. */
static void gfnt_cmap_format12(const uint32_t * cps, const uint32_t * gids,
    size_t count, GFNT_WBuf * out) {
  size_t groups = 0;
  size_t i;

  for (i = 0; i < count; ++i) {
    if (i == 0 || cps[i] != cps[i - 1] + 1 || gids[i] != gids[i - 1] + 1) {
      ++groups;
    }
  }
  gfnt_wbuf_u16(out, 12);
  gfnt_wbuf_u16(out, 0);
  gfnt_wbuf_u32(out, (uint32_t)(16 + 12 * groups));
  gfnt_wbuf_u32(out, 0);
  gfnt_wbuf_u32(out, (uint32_t)groups);
  for (i = 0; i < count;) {
    size_t j = i;

    while (j + 1 < count && cps[j + 1] == cps[j] + 1 && gids[j + 1] == gids[j] + 1) {
      ++j;
    }
    gfnt_wbuf_u32(out, cps[i]);
    gfnt_wbuf_u32(out, cps[j]);
    gfnt_wbuf_u32(out, gids[i]);
    i = j + 1;
  }
}

GFNT_Result gfnt_subset_cmap(const uint32_t * cps, const uint32_t * gids,
    size_t count, GFNT_WBuf * out, GFNT_Error * error) {
  GFNT_WBuf f4;
  GFNT_WBuf f12;
  size_t bmp = 0;
  bool fits = true;
  bool want12;
  size_t records;
  size_t at;
  GFNT_Result result;

  while (bmp < count && cps[bmp] <= 0xFFFFu) {
    ++bmp;
  }
  gfnt_wbuf_init(&f4, out->allocator);
  gfnt_wbuf_init(&f12, out->allocator);
  result = gfnt_cmap_format4(cps, gids, bmp, &f4, &fits, error);
  if (result != GFNT_OK) {
    return result;
  }
  // Format 12 carries the codepoints above the BMP, and all of them when format 4
  // would not fit.
  want12 = bmp < count || !fits;
  if (want12) {
    gfnt_cmap_format12(cps, gids, count, &f12);
  }
  if (f4.oom || f12.oom) {
    gfnt_wbuf_free(&f4);
    gfnt_wbuf_free(&f12);
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_CMAP, 0, GFNT_GLYPH_NONE,
        "building the cmap");
  }
  records = (fits ? 2u : 0u) + (want12 ? 2u : 0u);
  at = 4 + 8 * records;
  gfnt_wbuf_u16(out, 0);
  gfnt_wbuf_u16(out, (uint32_t)records);
  // Records are in platform then encoding order: (0,3), (0,4), (3,1), (3,10).
  // The two that name the same format share one subtable.
  if (fits) {
    gfnt_wbuf_u16(out, 0);
    gfnt_wbuf_u16(out, 3);
    gfnt_wbuf_u32(out, (uint32_t)at);
  }
  if (want12) {
    gfnt_wbuf_u16(out, 0);
    gfnt_wbuf_u16(out, 4);
    gfnt_wbuf_u32(out, (uint32_t)(at + (fits ? f4.length : 0)));
  }
  if (fits) {
    gfnt_wbuf_u16(out, 3);
    gfnt_wbuf_u16(out, 1);
    gfnt_wbuf_u32(out, (uint32_t)at);
  }
  if (want12) {
    gfnt_wbuf_u16(out, 3);
    gfnt_wbuf_u16(out, 10);
    gfnt_wbuf_u32(out, (uint32_t)(at + (fits ? f4.length : 0)));
  }
  if (fits) {
    gfnt_wbuf_bytes(out, f4.data, f4.length);
  }
  if (want12) {
    gfnt_wbuf_bytes(out, f12.data, f12.length);
  }
  gfnt_wbuf_free(&f4);
  gfnt_wbuf_free(&f12);
  if (out->oom) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_CMAP, 0, GFNT_GLYPH_NONE,
        "building the cmap");
  }
  return GFNT_OK;
}
