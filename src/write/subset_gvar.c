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
 * `gvar`: the per-glyph deltas of a variable TrueType font.
 *
 * documentation/design.md section 12.3. Each glyph's variation data is a
 * self-contained block (its tuple headers, its point numbers and its deltas name
 * points of that glyph and shared tuples by index, and nothing in it names another
 * glyph), so it is copied as it is. What is rebuilt is the offset array, one entry
 * for each kept glyph: a glyph the subset drops, or keeps only as a numbered gap,
 * has no data. The shared tuples are carried whole.
 */

#include "../sfnt/sfnt.h"
#include "write.h"

#define T_GVAR GFNT_TAG('g', 'v', 'a', 'r')

static uint32_t be16(const uint8_t * p) {
  return ((uint32_t)p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t * p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

GFNT_Result gfnt_subset_gvar(const GFNT_Face * face, const uint32_t * old_of_new,
    size_t new_count, GFNT_WBuf * out, GFNT_Error * error) {
  GFNT_Reader reader;
  const uint8_t * t;
  size_t n;
  size_t glyphs;
  size_t offsets_at;
  size_t data_at;
  size_t shared_at;
  size_t shared_len;
  size_t entry;
  bool long_offsets;
  size_t base = out->length;
  size_t j;

  if (!gfnt_face_has_table(face, T_GVAR)) {
    return GFNT_OK;
  }
  if (gfnt_face_table_reader(face, T_GVAR, &reader, NULL) != GFNT_OK
      || gfnt_read_bytes(&reader, reader.length, &t) != GFNT_OK) {
    return GFNT_OK;
  }
  n = reader.length;
  if (n < 20 || be16(t) != 1 || new_count > 65535) {
    return gfnt_error_set(error, new_count > 65535 ? GFNT_ERR_LIMIT : GFNT_ERR_CORRUPT,
        T_GVAR, 0, GFNT_GLYPH_NONE, "a gvar that cannot be subset");
  }
  glyphs = be16(t + 12);
  long_offsets = (be16(t + 14) & 1) != 0;
  entry = long_offsets ? 4 : 2;
  shared_at = be32(t + 8);
  shared_len = (size_t)be16(t + 6) * be16(t + 4) * 2;
  data_at = be32(t + 16);
  offsets_at = 20;
  if (offsets_at + (glyphs + 1) * entry > n || data_at > n
      || shared_at > n || shared_len > n - shared_at) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_GVAR, 0, GFNT_GLYPH_NONE,
        "a gvar whose arrays run past the table");
  }

  // Header, with the offsets and the shared tuples laid out after it; the data
  // offset is patched once the offsets array's size is known.
  gfnt_wbuf_u16(out, 1);
  gfnt_wbuf_u16(out, 0);
  gfnt_wbuf_u16(out, be16(t + 4));
  gfnt_wbuf_u16(out, be16(t + 6));
  gfnt_wbuf_u32(out, 0);
  gfnt_wbuf_u16(out, (uint32_t)new_count);
  gfnt_wbuf_u16(out, 0); // flags, patched below
  gfnt_wbuf_u32(out, 0);

  // The data is gathered first so the offsets can choose their width.
  {
    GFNT_WBuf data;
    uint32_t * offs = out->allocator->calloc_fn(out->allocator->ctx, new_count + 1, sizeof *offs);
    GFNT_Result result = GFNT_OK;

    gfnt_wbuf_init(&data, out->allocator);
    if (!offs) {
      return gfnt_error_set(error, GFNT_ERR_OOM, T_GVAR, 0, GFNT_GLYPH_NONE,
          "rewriting gvar");
    }
    for (j = 0; j < new_count; ++j) {
      uint32_t g = old_of_new[j];

      offs[j] = (uint32_t)data.length;
      if (g != 0xFFFFFFFFu && g < glyphs) {
        size_t a, b;

        if (long_offsets) {
          a = be32(t + offsets_at + g * 4);
          b = be32(t + offsets_at + g * 4 + 4);
        }
        else {
          a = (size_t)be16(t + offsets_at + g * 2) * 2;
          b = (size_t)be16(t + offsets_at + g * 2 + 2) * 2;
        }
        if (b < a || data_at + b > n) {
          result = gfnt_error_set(error, GFNT_ERR_CORRUPT, T_GVAR, 0, g,
              "a glyph's variation data runs past the table");
          break;
        }
        gfnt_wbuf_bytes(&data, t + data_at + a, b - a);
        // Short offsets count in two-byte units.
        if (!long_offsets) {
          gfnt_wbuf_align(&data, 2);
        }
      }
    }
    offs[new_count] = (uint32_t)data.length;
    if (result == GFNT_OK && !long_offsets && data.length > 0x1FFFEu) {
      long_offsets = true;
    }
    if (result == GFNT_OK) {
      size_t shared_out = (20 + (new_count + 1) * (long_offsets ? 4 : 2) + 3) & ~(size_t)3;
      size_t data_out = (shared_out + shared_len + 3) & ~(size_t)3;

      for (j = 0; j <= new_count; ++j) {
        if (long_offsets) {
          gfnt_wbuf_u32(out, offs[j]);
        }
        else {
          gfnt_wbuf_u16(out, offs[j] / 2);
        }
      }
      gfnt_wbuf_align(out, 4);
      gfnt_wbuf_bytes(out, t + shared_at, shared_len);
      gfnt_wbuf_align(out, 4);
      if (!out->oom) {
        gfnt_write_put32(out->data + base + 8, (uint32_t)shared_out);
        gfnt_write_put16(out->data + base + 14, long_offsets ? 1u : 0u);
        gfnt_write_put32(out->data + base + 16, (uint32_t)data_out);
      }
      gfnt_wbuf_bytes(out, data.data, data.length);
      gfnt_wbuf_align(out, 4);
    }
    bool oom = out->oom || data.oom;

    gfnt_wbuf_free(&data);
    out->allocator->free_fn(out->allocator->ctx, offs);
    if (result == GFNT_OK && oom) {
      result = gfnt_error_set(error, GFNT_ERR_OOM, T_GVAR, 0, GFNT_GLYPH_NONE,
          "rewriting gvar");
    }
    return result;
  }
}
