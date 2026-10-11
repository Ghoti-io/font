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
 * The variation tables that are indexed by glyph: `HVAR` and `VVAR`, and the item
 * variation store they and `GDEF` keep their deltas in.
 *
 * documentation/design.md section 12.3. A store is carried as it is, whole: its
 * rows are addressed by (outer, inner) and nothing in it names a glyph. What names
 * glyphs is the delta-set index map, which sends a glyph to its row; it is rebuilt
 * for the new numbering, one entry for each kept glyph, and an advance map the font
 * did not state (the glyph id is then the row) is stated, because the ids have
 * moved.
 */

#include <string.h>
#include "../sfnt/sfnt.h"
#include "write.h"

#define T_HVAR GFNT_TAG('H', 'V', 'A', 'R')
#define T_VVAR GFNT_TAG('V', 'V', 'A', 'R')

static uint32_t be16(const uint8_t * p) {
  return ((uint32_t)p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t * p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool gfnt_subset_ivs_extent(const uint8_t * p, size_t avail, size_t * out_length) {
  size_t end;
  size_t data_count;
  size_t i;

  if (avail < 8 || be16(p) != 1) {
    return false;
  }
  {
    size_t regions = be32(p + 2);
    size_t axes;
    size_t count;

    if (regions + 4 > avail) {
      return false;
    }
    axes = be16(p + regions);
    count = be16(p + regions + 2);
    end = regions + 4 + axes * count * 6;
    if (end > avail) {
      return false;
    }
  }
  data_count = be16(p + 6);
  if (8 + 4 * data_count > avail) {
    return false;
  }
  for (i = 0; i < data_count; ++i) {
    size_t at = be32(p + 8 + 4 * i);
    size_t items;
    size_t words;
    size_t regions;
    size_t row;
    size_t stop;

    if (at + 6 > avail) {
      return false;
    }
    items = be16(p + at);
    words = be16(p + at + 2);
    regions = be16(p + at + 4);
    if (words & 0x8000u) {
      words &= 0x7FFFu;
      row = words * 4 + (regions > words ? regions - words : 0) * 2;
    }
    else {
      row = words * 2 + (regions > words ? regions - words : 0);
    }
    stop = at + 6 + 2 * regions + items * row;
    if (stop > avail) {
      return false;
    }
    if (stop > end) {
      end = stop;
    }
  }
  *out_length = end;
  return true;
}

/** One entry of a delta-set index map, with the last entry standing for any index past the end. */
static bool map_lookup(const uint8_t * m, size_t n, uint32_t index, uint32_t * outer,
    uint32_t * inner) {
  size_t count;
  size_t base;
  size_t size;
  unsigned bits;
  size_t at;
  uint32_t v = 0;
  size_t k;

  if (n < 4) {
    return false;
  }
  if (m[0] == 0) {
    count = be16(m + 2);
    base = 4;
  }
  else if (m[0] == 1 && n >= 6) {
    count = be32(m + 2);
    base = 6;
  }
  else {
    return false;
  }
  size = (size_t)((m[1] >> 4) & 3) + 1;
  bits = (unsigned)(m[1] & 15) + 1;
  if (count == 0) {
    return false;
  }
  at = base + (index < count ? index : count - 1) * size;
  if (at + size > n) {
    return false;
  }
  for (k = 0; k < size; ++k) {
    v = (v << 8) | m[at + k];
  }
  *outer = bits >= 32 ? 0 : v >> bits;
  *inner = bits >= 32 ? v : v & ((1u << bits) - 1u);
  return true;
}

static unsigned bits_for(uint32_t v) {
  unsigned b = 1;

  while (b < 32 && (v >> b)) {
    ++b;
  }
  return b;
}

/** A delta-set index map sending each kept glyph to the row its source glyph had. */
static GFNT_Result write_map(const uint8_t * old_map, size_t old_n, bool implicit,
    const uint32_t * old_of_new, size_t new_count, GFNT_WBuf * out, GFNT_Error * error) {
  uint32_t * outers;
  uint32_t * inners;
  uint32_t max_outer = 0;
  uint32_t max_inner = 0;
  size_t j;
  unsigned ibits;
  unsigned obits;
  size_t size;
  const GFNT_Allocator * a = out->allocator;

  outers = a->calloc_fn(a->ctx, new_count + 1, sizeof *outers);
  inners = a->calloc_fn(a->ctx, new_count + 1, sizeof *inners);
  if (!outers || !inners) {
    a->free_fn(a->ctx, outers);
    a->free_fn(a->ctx, inners);
    return gfnt_error_set(error, GFNT_ERR_OOM, T_HVAR, 0, GFNT_GLYPH_NONE,
        "rebuilding a delta-set index map");
  }
  for (j = 0; j < new_count; ++j) {
    uint32_t g = old_of_new[j] == 0xFFFFFFFFu ? 0 : old_of_new[j];

    if (implicit) {
      outers[j] = 0;
      inners[j] = g;
    }
    else if (!map_lookup(old_map, old_n, g, &outers[j], &inners[j])) {
      a->free_fn(a->ctx, outers);
      a->free_fn(a->ctx, inners);
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_HVAR, 0, GFNT_GLYPH_NONE,
          "a delta-set index map that cannot be read");
    }
    if (outers[j] > max_outer) {
      max_outer = outers[j];
    }
    if (inners[j] > max_inner) {
      max_inner = inners[j];
    }
  }
  ibits = bits_for(max_inner);
  obits = max_outer ? bits_for(max_outer) : 0;
  size = (ibits + obits + 7) / 8;
  if (size > 4 || new_count > 65535) {
    a->free_fn(a->ctx, outers);
    a->free_fn(a->ctx, inners);
    return gfnt_error_set(error, GFNT_ERR_LIMIT, T_HVAR, 0, GFNT_GLYPH_NONE,
        "a delta-set index map entry wider than four bytes");
  }
  gfnt_wbuf_u8(out, 0);
  gfnt_wbuf_u8(out, (uint32_t)(((size - 1) << 4) | (ibits - 1)));
  gfnt_wbuf_u16(out, (uint32_t)new_count);
  for (j = 0; j < new_count; ++j) {
    uint32_t v = (outers[j] << ibits) | inners[j];
    size_t k;

    for (k = size; k > 0; --k) {
      gfnt_wbuf_u8(out, (v >> (8 * (k - 1))) & 0xFF);
    }
  }
  a->free_fn(a->ctx, outers);
  a->free_fn(a->ctx, inners);
  return GFNT_OK;
}

GFNT_Result gfnt_subset_var_metrics(const GFNT_Face * face, GFNT_Tag tag,
    const uint32_t * old_of_new, size_t new_count, GFNT_WBuf * out, GFNT_Error * error) {
  GFNT_Reader reader;
  const uint8_t * t;
  size_t n;
  size_t maps = tag == T_HVAR ? 3 : 4;
  size_t header = 8 + 4 * maps;
  size_t store_len = 0;
  size_t base = out->length;
  size_t i;
  GFNT_Result result = GFNT_OK;

  if (!gfnt_face_has_table(face, tag)) {
    return GFNT_OK;
  }
  if (gfnt_face_table_reader(face, tag, &reader, NULL) != GFNT_OK
      || gfnt_read_bytes(&reader, reader.length, &t) != GFNT_OK) {
    return GFNT_OK;
  }
  n = reader.length;
  if (n < header || be16(t) != 1 || be32(t + 4) == 0 || be32(t + 4) >= n
      || !gfnt_subset_ivs_extent(t + be32(t + 4), n - be32(t + 4), &store_len)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0, GFNT_GLYPH_NONE,
        "a variation metrics table with no readable item variation store");
  }
  gfnt_wbuf_u16(out, 1);
  gfnt_wbuf_u16(out, 0);
  gfnt_wbuf_u32(out, (uint32_t)header);
  for (i = 0; i < maps; ++i) {
    gfnt_wbuf_u32(out, 0);
  }
  gfnt_wbuf_bytes(out, t + be32(t + 4), store_len);
  gfnt_wbuf_align(out, 4);
  for (i = 0; i < maps && result == GFNT_OK; ++i) {
    size_t off = be32(t + 8 + 4 * i);

    // The first map is the advance: stated even when the font left it to the glyph id.
    if (off == 0 && i != 0) {
      continue;
    }
    if (off >= n) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0, GFNT_GLYPH_NONE,
          "a delta-set index map outside its table");
    }
    if (!out->oom) {
      gfnt_write_put32(out->data + base + 8 + 4 * i, (uint32_t)(out->length - base));
    }
    result = write_map(off ? t + off : NULL, off ? n - off : 0, off == 0, old_of_new,
        new_count, out, error);
    gfnt_wbuf_align(out, 4);
  }
  if (result == GFNT_OK && out->oom) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE,
        "rewriting a variation metrics table");
  }
  return result;
}
