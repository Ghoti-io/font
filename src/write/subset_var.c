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
 * variation store they keep their deltas in (`GDEF`'s store is carried whole elsewhere).
 *
 * documentation/design.md section 12.3. What names glyphs is the delta-set index map,
 * which sends a glyph to its row (outer, inner); it is rebuilt for the new numbering,
 * one entry for each kept glyph, and an advance map the font did not state (the glyph
 * id is then the row) is stated, because the ids have moved. The store is pruned to
 * the rows those maps now reach: a set with none left is dropped, the rest are
 * renumbered, and the region list is kept whole.
 */

#include <string.h>
#include "../sfnt/sfnt.h"
#include "write.h"
#include "lrewrite.h"

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

/**
 * The (outer, inner) pair of each kept glyph under one map, in new-glyph order: the row its
 * source glyph had. An advance map the font left out is the glyph id itself, in set 0.
 */
static GFNT_Result read_pairs(const uint8_t * old_map, size_t old_n, bool implicit,
    const uint32_t * old_of_new, size_t new_count, uint32_t * outers, uint32_t * inners,
    GFNT_Error * error) {
  size_t j;

  for (j = 0; j < new_count; ++j) {
    uint32_t g = old_of_new[j] == 0xFFFFFFFFu ? 0 : old_of_new[j];

    if (implicit) {
      outers[j] = 0;
      inners[j] = g;
    }
    else if (!map_lookup(old_map, old_n, g, &outers[j], &inners[j])) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_HVAR, 0, GFNT_GLYPH_NONE,
          "a delta-set index map that cannot be read");
    }
  }
  return GFNT_OK;
}

/** A delta-set index map holding @p outers and @p inners as they are. */
static GFNT_Result emit_map(const uint32_t * outers, const uint32_t * inners, size_t new_count,
    GFNT_WBuf * out, GFNT_Error * error) {
  uint32_t max_outer = 0;
  uint32_t max_inner = 0;
  size_t j;
  unsigned ibits;
  unsigned obits;
  size_t size;

  for (j = 0; j < new_count; ++j) {
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
  return GFNT_OK;
}

/** The bytes of one row of the ItemVariationData at @p d (whose layout the extent check vouched for). */
static size_t row_size(const uint8_t * d) {
  size_t words = be16(d + 2);
  size_t regions = be16(d + 4);

  if (words & 0x8000u) {
    words &= 0x7FFFu;
    return words * 4 + (regions > words ? regions - words : 0) * 2;
  }
  return words * 2 + (regions > words ? regions - words : 0);
}

#define IVS_NONE 0xFFFFFFFFu

struct GFNT_IvsMap {
  const GFNT_Allocator * a;
  const uint8_t * s;      ///< The source store.
  size_t sets;
  size_t * first_row;     ///< Per set: its first row in the rows laid end to end.
  uint32_t * row_new;     ///< Per row: its new inner index, or IVS_NONE.
  uint32_t * set_new;     ///< Per set: its new outer index, or IVS_NONE.
  uint32_t * set_old;     ///< Per new set: its old outer index.
  uint32_t * set_next;    ///< Per set: the next new inner index.
  uint32_t new_sets;
  size_t new_rows;
  bool oom;
};

GFNT_IvsMap * gfnt_ivs_map_create(const uint8_t * store, size_t avail,
    const GFNT_Allocator * a) {
  GFNT_IvsMap * m;
  size_t length = 0;
  size_t rows = 0;
  size_t i;

  if (!gfnt_subset_ivs_extent(store, avail, &length)) {
    return NULL;
  }
  m = a->calloc_fn(a->ctx, 1, sizeof *m);
  if (!m) {
    return NULL;
  }
  m->a = a;
  m->s = store;
  m->sets = be16(store + 6);
  m->first_row = a->calloc_fn(a->ctx, m->sets + 1, sizeof *m->first_row);
  m->set_new = a->calloc_fn(a->ctx, m->sets + 1, sizeof *m->set_new);
  m->set_old = a->calloc_fn(a->ctx, m->sets + 1, sizeof *m->set_old);
  m->set_next = a->calloc_fn(a->ctx, m->sets + 1, sizeof *m->set_next);
  if (!m->first_row || !m->set_new || !m->set_old || !m->set_next) {
    gfnt_ivs_map_free(m);
    return NULL;
  }
  for (i = 0; i < m->sets; ++i) {
    m->first_row[i] = rows;
    rows += be16(store + be32(store + 8 + 4 * i));
    m->set_new[i] = IVS_NONE;
  }
  m->row_new = a->calloc_fn(a->ctx, rows + 1, sizeof *m->row_new);
  if (!m->row_new) {
    gfnt_ivs_map_free(m);
    return NULL;
  }
  for (i = 0; i < rows; ++i) {
    m->row_new[i] = IVS_NONE;
  }
  return m;
}

void gfnt_ivs_map_free(GFNT_IvsMap * m) {
  const GFNT_Allocator * a;

  if (!m) {
    return;
  }
  a = m->a;
  a->free_fn(a->ctx, m->first_row);
  a->free_fn(a->ctx, m->set_new);
  a->free_fn(a->ctx, m->set_old);
  a->free_fn(a->ctx, m->set_next);
  a->free_fn(a->ctx, m->row_new);
  a->free_fn(a->ctx, m);
}

bool gfnt_ivs_map_assign(GFNT_IvsMap * m, uint32_t outer, uint32_t inner, uint32_t * new_outer,
    uint32_t * new_inner) {
  size_t row;

  if (outer >= m->sets || inner >= be16(m->s + be32(m->s + 8 + 4 * (size_t)outer))) {
    return false;
  }
  if (m->set_new[outer] == IVS_NONE) {
    m->set_new[outer] = m->new_sets;
    m->set_old[m->new_sets++] = outer;
  }
  row = m->first_row[outer] + inner;
  if (m->row_new[row] == IVS_NONE) {
    m->row_new[row] = m->set_next[outer]++;
    ++m->new_rows;
  }
  *new_outer = m->set_new[outer];
  *new_inner = m->row_new[row];
  return true;
}

size_t gfnt_ivs_map_rows(const GFNT_IvsMap * m) {
  return m->new_rows;
}

uint32_t gfnt_ivs_map_sets(const GFNT_IvsMap * m) {
  return m->new_sets;
}

bool gfnt_ivs_map_failed(const GFNT_IvsMap * m) {
  return m->oom;
}

void gfnt_ivs_map_write(GFNT_IvsMap * m, GFNT_WBuf * out) {
  const uint8_t * s = m->s;
  const size_t regions = be32(s + 2);
  const size_t region_len = 4 + (size_t)be16(s + regions) * be16(s + regions + 2) * 6;
  const size_t base = out->length;
  uint32_t j;

  gfnt_wbuf_u16(out, 1);
  gfnt_wbuf_u32(out, (uint32_t)(8 + 4 * (size_t)m->new_sets));
  gfnt_wbuf_u16(out, m->new_sets);
  for (j = 0; j < m->new_sets; ++j) {
    gfnt_wbuf_u32(out, 0);
  }
  gfnt_wbuf_bytes(out, s + regions, region_len);
  for (j = 0; j < m->new_sets; ++j) {
    const uint32_t old = m->set_old[j];
    const uint8_t * d = s + be32(s + 8 + 4 * (size_t)old);
    const size_t items = be16(d);
    const size_t rows = row_size(d);
    const size_t index_count = be16(d + 4);
    const size_t kept = m->set_next[old];
    uint32_t * from = m->a->calloc_fn(m->a->ctx, kept ? kept : 1, sizeof *from);
    size_t r;
    size_t k;

    if (!from) {
      m->oom = true;
      out->oom = true;
      return;
    }
    for (r = 0; r < items; ++r) {
      const uint32_t at = m->row_new[m->first_row[old] + r];

      if (at != IVS_NONE) {
        from[at] = (uint32_t)r;
      }
    }
    if (!out->oom) {
      gfnt_write_put32(out->data + base + 8 + 4 * (size_t)j, (uint32_t)(out->length - base));
    }
    gfnt_wbuf_u16(out, (uint32_t)kept);
    gfnt_wbuf_u16(out, be16(d + 2));
    gfnt_wbuf_u16(out, (uint32_t)index_count);
    gfnt_wbuf_bytes(out, d + 6, 2 * index_count);
    for (k = 0; k < kept; ++k) {
      gfnt_wbuf_bytes(out, d + 6 + 2 * index_count + (size_t)from[k] * rows, rows);
    }
    m->a->free_fn(m->a->ctx, from);
  }
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
  size_t j;
  GFNT_IvsMap * ivs = NULL;
  uint32_t * pairs = NULL; // per map: new_count outers then new_count inners
  bool present[4] = {false, false, false, false};
  const GFNT_Allocator * a = out->allocator;
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
  ivs = gfnt_ivs_map_create(t + be32(t + 4), n - be32(t + 4), a);
  pairs = a->calloc_fn(a->ctx, 2 * maps * (new_count + 1), sizeof *pairs);
  if (!ivs || !pairs) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE,
        "rewriting a variation metrics table");
    goto done;
  }
  // Read every map's pairs, and give the rows the kept glyphs use their new places.
  for (i = 0; i < maps; ++i) {
    size_t off = be32(t + 8 + 4 * i);
    uint32_t * po = pairs + 2 * i * (new_count + 1);
    uint32_t * pi = po + new_count + 1;

    // The first map is the advance: stated even when the font left it to the glyph id.
    if (off == 0 && i != 0) {
      continue;
    }
    if (off >= n) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0, GFNT_GLYPH_NONE,
          "a delta-set index map outside its table");
      goto done;
    }
    present[i] = true;
    result = read_pairs(off ? t + off : NULL, off ? n - off : 0, off == 0, old_of_new,
        new_count, po, pi, error);
    if (result != GFNT_OK) {
      goto done;
    }
    for (j = 0; j < new_count; ++j) {
      if (!gfnt_ivs_map_assign(ivs, po[j], pi[j], &po[j], &pi[j])) {
        // A row the font never had stays a row nobody has: no delta.
        po[j] = 0xFFFFFFFFu;
      }
    }
  }
  gfnt_wbuf_u16(out, 1);
  gfnt_wbuf_u16(out, 0);
  gfnt_wbuf_u32(out, (uint32_t)header);
  for (i = 0; i < maps; ++i) {
    gfnt_wbuf_u32(out, 0);
  }
  gfnt_ivs_map_write(ivs, out);
  gfnt_wbuf_align(out, 4);
  for (i = 0; i < maps && result == GFNT_OK; ++i) {
    uint32_t * po = pairs + 2 * i * (new_count + 1);
    uint32_t * pi = po + new_count + 1;

    if (!present[i]) {
      continue;
    }
    for (j = 0; j < new_count; ++j) {
      if (po[j] == 0xFFFFFFFFu) {
        po[j] = gfnt_ivs_map_sets(ivs);
        pi[j] = 0;
      }
    }
    if (!out->oom) {
      gfnt_write_put32(out->data + base + 8 + 4 * i, (uint32_t)(out->length - base));
    }
    result = emit_map(po, pi, new_count, out, error);
    gfnt_wbuf_align(out, 4);
  }
  if (result == GFNT_OK && (out->oom || gfnt_ivs_map_failed(ivs))) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE,
        "rewriting a variation metrics table");
  }
done:
  gfnt_ivs_map_free(ivs);
  a->free_fn(a->ctx, pairs);
  return result;
}
