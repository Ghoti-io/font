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

#include <stdint.h>
#include <string.h>
#include <ghoti.io/font/macros.h>
#include "write.h"

/*
 * A TrueType Collection: several fonts in one file, each table stored once however
 * many fonts name it.
 *
 * Two tables are the same table when their tag and every byte are the same, which is
 * how a collection earns its keep: the glyph outlines, the `cmap` and the layout
 * tables of fonts that differ only in `name` or weight are shared. The header is
 * version 1.0 (no signature fields). Each font's `head` keeps the
 * `checkSumAdjustment` it was given - the specification does not say which font of a
 * collection that field sums - and its directory checksum is taken with the field
 * zero, as everywhere else.
 */

#define TAG_HEAD GFNT_TAG('h', 'e', 'a', 'd')

typedef struct Entry {
  uint32_t tag;
  const uint8_t * data;
  size_t length;
  size_t offset;
  uint32_t checksum;
} Entry;

static size_t pad4(size_t n) {
  return (n + 3u) & ~(size_t)3u;
}

GFNT_Result gfnt_write_collection(const GFNT_WriteFace * faces, size_t face_count,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Blob ** out_blob, GFNT_Error * error) {
  GFNT_Limits defaults;
  Entry * unique = NULL;
  size_t ** ids = NULL;
  size_t * face_offset = NULL;
  uint8_t * file = NULL;
  size_t total_tables = 0;
  size_t used = 0;
  size_t total;
  size_t f, i, j;
  GFNT_Result result = GFNT_OK;

  if (!out_blob || !faces || face_count == 0 || face_count > 65535u) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no faces, too many, or nowhere to put the collection");
  }
  if (!limits) {
    gfnt_limits_default(&defaults);
    limits = &defaults;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  for (f = 0; f < face_count; ++f) {
    if (!faces[f].tables || faces[f].count == 0
        || faces[f].count > limits->max_tables) {
      return gfnt_error_set(error,
          !faces[f].tables || faces[f].count == 0 ? GFNT_ERR_INVALID : GFNT_ERR_LIMIT,
          0, 0, GFNT_GLYPH_NONE, "a face with no tables, or more than the limit");
    }
    total_tables += faces[f].count;
    for (i = 0; i < faces[f].count; ++i) {
      if (!faces[f].tables[i].data && faces[f].tables[i].length) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, faces[f].tables[i].tag, 0,
            GFNT_GLYPH_NONE, "a table with a length and no bytes");
      }
      for (j = 0; j < i; ++j) {
        if (faces[f].tables[j].tag == faces[f].tables[i].tag) {
          return gfnt_error_set(error, GFNT_ERR_INVALID, faces[f].tables[i].tag, 0,
              GFNT_GLYPH_NONE, "a tag appears twice in one font");
        }
      }
    }
  }
  unique = allocator->calloc_fn(allocator->ctx, total_tables, sizeof *unique);
  ids = allocator->calloc_fn(allocator->ctx, face_count, sizeof *ids);
  face_offset = allocator->calloc_fn(allocator->ctx, face_count, sizeof *face_offset);
  if (!unique || !ids || !face_offset) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the table list");
    goto done;
  }
  for (f = 0; f < face_count; ++f) {
    ids[f] = allocator->calloc_fn(allocator->ctx, faces[f].count, sizeof(size_t));
    if (!ids[f]) {
      result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "allocating the table list");
      goto done;
    }
    for (i = 0; i < faces[f].count; ++i) {
      const GFNT_WriteTable * t = &faces[f].tables[i];
      size_t id;

      for (id = 0; id < used; ++id) {
        if (unique[id].tag == t->tag && unique[id].length == t->length
            && (t->length == 0 || memcmp(unique[id].data, t->data, t->length) == 0)) {
          break;
        }
      }
      if (id == used) {
        unique[used].tag = t->tag;
        unique[used].data = t->data;
        unique[used].length = t->length;
        ++used;
      }
      ids[f][i] = id;
    }
  }
  // Layout: the header, each font's offset table and directory, then every table once.
  total = 12u + 4u * face_count;
  for (f = 0; f < face_count; ++f) {
    face_offset[f] = total;
    total += 12u + 16u * faces[f].count;
  }
  for (i = 0; i < used; ++i) {
    unique[i].offset = total;
    if (pad4(unique[i].length) > limits->max_blob_bytes
        || total > limits->max_blob_bytes - pad4(unique[i].length)) {
      result = gfnt_error_set(error, GFNT_ERR_LIMIT, unique[i].tag, 0,
          GFNT_GLYPH_NONE, "the collection would be larger than the limit allows");
      goto done;
    }
    total += pad4(unique[i].length);
  }
  file = allocator->calloc_fn(allocator->ctx, total, 1);
  if (!file) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the collection");
    goto done;
  }
  for (i = 0; i < used; ++i) {
    if (unique[i].length) {
      memcpy(file + unique[i].offset, unique[i].data, unique[i].length);
    }
    if (unique[i].tag == TAG_HEAD && unique[i].length >= 12) {
      uint8_t stored[4];

      memcpy(stored, file + unique[i].offset + 8, 4);
      memset(file + unique[i].offset + 8, 0, 4);
      unique[i].checksum = gfnt_write_checksum(file + unique[i].offset,
          unique[i].length);
      memcpy(file + unique[i].offset + 8, stored, 4);
    }
    else {
      unique[i].checksum = gfnt_write_checksum(file + unique[i].offset,
          unique[i].length);
    }
  }
  gfnt_write_put32(file, GFNT_TAG('t', 't', 'c', 'f'));
  gfnt_write_put32(file + 4, 0x00010000u);
  gfnt_write_put32(file + 8, (uint32_t)face_count);
  for (f = 0; f < face_count; ++f) {
    const size_t count = faces[f].count;
    uint8_t * p = file + face_offset[f];
    size_t * order = allocator->calloc_fn(allocator->ctx, count, sizeof *order);
    unsigned entry_selector = 0;
    unsigned search_range;

    if (!order) {
      result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "sorting a directory");
      goto done;
    }
    gfnt_write_put32(file + 12 + 4 * f, (uint32_t)face_offset[f]);
    for (i = 0; i < count; ++i) {
      size_t v = ids[f][i];

      j = i;
      while (j > 0 && unique[order[j - 1]].tag > unique[v].tag) {
        order[j] = order[j - 1];
        --j;
      }
      order[j] = v;
    }
    while ((2u << entry_selector) <= count) {
      ++entry_selector;
    }
    search_range = (1u << entry_selector) * 16u;
    gfnt_write_put32(p, faces[f].flavour);
    gfnt_write_put16(p + 4, (uint32_t)count);
    gfnt_write_put16(p + 6, search_range);
    gfnt_write_put16(p + 8, entry_selector);
    gfnt_write_put16(p + 10, (uint32_t)count * 16u - search_range);
    for (i = 0; i < count; ++i) {
      const Entry * e = &unique[order[i]];
      uint8_t * d = p + 12 + 16 * i;

      gfnt_write_put32(d, e->tag);
      gfnt_write_put32(d + 4, e->checksum);
      gfnt_write_put32(d + 8, (uint32_t)e->offset);
      gfnt_write_put32(d + 12, (uint32_t)e->length);
    }
    allocator->free_fn(allocator->ctx, order);
  }
  result = gfnt_blob_create_memory(file, total, GFNT_BLOB_COPY, limits, allocator,
      out_blob, error);

done:
  if (ids) {
    for (f = 0; f < face_count; ++f) {
      allocator->free_fn(allocator->ctx, ids[f]);
    }
  }
  allocator->free_fn(allocator->ctx, ids);
  allocator->free_fn(allocator->ctx, face_offset);
  allocator->free_fn(allocator->ctx, unique);
  allocator->free_fn(allocator->ctx, file);
  return result;
}
