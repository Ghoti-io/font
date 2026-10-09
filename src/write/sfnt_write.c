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
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/font/write.h>
#include "write.h"

#define GFNT_TAG_HEAD GFNT_TAG('h', 'e', 'a', 'd')
#define GFNT_WRITE_HEADER_BYTES 12u
#define GFNT_WRITE_ENTRY_BYTES 16u
#define GFNT_WOFF_HEADER 44u
#define GFNT_WOFF_ENTRY 20u

void gfnt_write_put16(uint8_t * p, uint32_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

void gfnt_write_put32(uint8_t * p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/** The sum of big-endian words, the last one zero-padded: the sfnt checksum. */
uint32_t gfnt_write_checksum(const uint8_t * data, size_t length) {
  uint32_t sum = 0;
  size_t i;

  for (i = 0; i + 4 <= length; i += 4) {
    sum += ((uint32_t)data[i] << 24) | ((uint32_t)data[i + 1] << 16)
        | ((uint32_t)data[i + 2] << 8) | (uint32_t)data[i + 3];
  }
  if (i < length) {
    uint32_t tail = 0;
    unsigned shift = 24;

    for (; i < length; ++i, shift -= 8) {
      tail |= (uint32_t)data[i] << shift;
    }
    sum += tail;
  }
  return sum;
}

static size_t gfnt_write_pad4(size_t n) {
  return (n + 3u) & ~(size_t)3u;
}

static int gfnt_write_by_tag(const void * a, const void * b) {
  const GFNT_WriteTable * x = *(const GFNT_WriteTable * const *)a;
  const GFNT_WriteTable * y = *(const GFNT_WriteTable * const *)b;

  return x->tag < y->tag ? -1 : x->tag > y->tag;
}

GFNT_Result gfnt_write_build(GFNT_Tag flavour, const GFNT_WriteTable * tables,
    size_t count, const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    uint8_t ** out_bytes, size_t * out_size, GFNT_Error * error) {
  const GFNT_WriteTable ** order;
  uint8_t * sfnt;
  size_t total;
  size_t at;
  size_t i;
  unsigned entry_selector = 0;
  unsigned search_range;
  size_t head_at = 0;
  bool has_head = false;
  uint32_t whole;

  if (!tables || count == 0 || !out_bytes || !out_size) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no tables to write, or nowhere to put the font");
  }
  if (count > limits->max_tables || count > 0xFFFFu) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "more tables than the limit allows");
  }
  order = allocator->calloc_fn(allocator->ctx, count, sizeof *order);
  if (!order) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the table order");
  }
  total = GFNT_WRITE_HEADER_BYTES + count * GFNT_WRITE_ENTRY_BYTES;
  for (i = 0; i < count; ++i) {
    if (!tables[i].data && tables[i].length != 0) {
      allocator->free_fn(allocator->ctx, order);
      return gfnt_error_set(error, GFNT_ERR_INVALID, tables[i].tag, 0,
          GFNT_GLYPH_NONE, "a table with a length and no bytes");
    }
    order[i] = &tables[i];
    if (tables[i].length > limits->max_blob_bytes
        || gfnt_write_pad4(tables[i].length) > limits->max_blob_bytes - total) {
      allocator->free_fn(allocator->ctx, order);
      return gfnt_error_set(error, GFNT_ERR_LIMIT, tables[i].tag, 0,
          GFNT_GLYPH_NONE, "the font would be larger than the limit allows");
    }
    total += gfnt_write_pad4(tables[i].length);
  }
  qsort(order, count, sizeof *order, gfnt_write_by_tag);
  for (i = 1; i < count; ++i) {
    if (order[i]->tag == order[i - 1]->tag) {
      const GFNT_Tag repeated = order[i]->tag;

      allocator->free_fn(allocator->ctx, order);
      return gfnt_error_set(error, GFNT_ERR_INVALID, repeated, 0,
          GFNT_GLYPH_NONE, "a tag appears twice");
    }
  }
  sfnt = allocator->calloc_fn(allocator->ctx, 1, total);
  if (!sfnt) {
    allocator->free_fn(allocator->ctx, order);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the font");
  }
  while ((2u << entry_selector) <= count) {
    ++entry_selector;
  }
  search_range = (1u << entry_selector) * 16u;
  gfnt_write_put32(sfnt, flavour);
  gfnt_write_put16(sfnt + 4, (uint32_t)count);
  gfnt_write_put16(sfnt + 6, search_range);
  gfnt_write_put16(sfnt + 8, entry_selector);
  gfnt_write_put16(sfnt + 10, (uint32_t)count * 16u - search_range);

  at = GFNT_WRITE_HEADER_BYTES + count * GFNT_WRITE_ENTRY_BYTES;
  for (i = 0; i < count; ++i) {
    uint8_t * d = sfnt + GFNT_WRITE_HEADER_BYTES + i * GFNT_WRITE_ENTRY_BYTES;
    const GFNT_WriteTable * t = order[i];

    if (t->length) {
      memcpy(sfnt + at, t->data, t->length);
    }
    if (t->tag == GFNT_TAG_HEAD && t->length >= 12) {
      has_head = true;
      head_at = at;
      // The directory's checksum of `head` is taken with the adjustment as zero.
      memset(sfnt + at + 8, 0, 4);
    }
    gfnt_write_put32(d, t->tag);
    gfnt_write_put32(d + 4, gfnt_write_checksum(sfnt + at, t->length));
    gfnt_write_put32(d + 8, (uint32_t)at);
    gfnt_write_put32(d + 12, (uint32_t)t->length);
    at += gfnt_write_pad4(t->length);
  }
  if (has_head) {
    whole = gfnt_write_checksum(sfnt, total);
    gfnt_write_put32(sfnt + head_at + 8, 0xB1B0AFBAu - whole);
  }
  allocator->free_fn(allocator->ctx, order);
  *out_bytes = sfnt;
  *out_size = total;
  return GFNT_OK;
}

GFNT_Result gfnt_write_sfnt(GFNT_Tag flavour, const GFNT_WriteTable * tables,
    size_t count, const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Blob ** out_blob, GFNT_Error * error) {
  GFNT_Limits defaults;
  uint8_t * bytes = NULL;
  size_t size = 0;
  GFNT_Result result;

  if (!out_blob) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "nowhere to put the font");
  }
  if (!limits) {
    gfnt_limits_default(&defaults);
    limits = &defaults;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  result = gfnt_write_build(flavour, tables, count, limits, allocator, &bytes,
      &size, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_blob_create_memory(bytes, size, GFNT_BLOB_COPY, limits,
      allocator, out_blob, error);
  allocator->free_fn(allocator->ctx, bytes);
  return result;
}

/** zlib-compress one table; NULL out when it did not get smaller. */
static GFNT_Result gfnt_write_deflate(const uint8_t * data, size_t length,
    uint32_t tag, void ** out, size_t * out_length, GFNT_Error * error) {
  gcomp_options_t * options = NULL;
  gcomp_status_t status;

  *out = NULL;
  *out_length = 0;
  if (length == 0) {
    return GFNT_OK;
  }
  if (gcomp_options_create(&options) != GCOMP_OK) {
    return gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE,
        "allocating the options for a deflate");
  }
  status = gcomp_encode_alloc(NULL, "zlib", options, data, length, out,
      out_length);
  gcomp_options_destroy(options);
  if (status != GCOMP_OK) {
    return gfnt_error_set(error,
        status == GCOMP_ERR_MEMORY ? GFNT_ERR_OOM : GFNT_ERR_INTERNAL, tag, 0,
        GFNT_GLYPH_NONE, gcomp_status_to_string(status));
  }
  return GFNT_OK;
}

GFNT_Result gfnt_write_woff(GFNT_Tag flavour, const GFNT_WriteTable * tables,
    size_t count, const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Blob ** out_blob, GFNT_Error * error) {
  GFNT_Limits defaults;
  uint8_t * sfnt = NULL;
  size_t sfnt_size = 0;
  void ** packed = NULL;
  size_t * packed_length = NULL;
  uint8_t * woff = NULL;
  size_t total;
  size_t at;
  size_t i;
  uint32_t revision = 0;
  GFNT_Result result;

  if (!out_blob) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "nowhere to put the font");
  }
  if (!limits) {
    gfnt_limits_default(&defaults);
    limits = &defaults;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  result = gfnt_write_build(flavour, tables, count, limits, allocator, &sfnt,
      &sfnt_size, error);
  if (result != GFNT_OK) {
    return result;
  }
  packed = allocator->calloc_fn(allocator->ctx, count, sizeof *packed);
  packed_length = allocator->calloc_fn(allocator->ctx, count,
      sizeof *packed_length);
  if (!packed || !packed_length) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the compressed tables");
    goto done;
  }
  total = GFNT_WOFF_HEADER + count * GFNT_WOFF_ENTRY;
  // The built sfnt is the source: its directory is already sorted and its
  // tables already in place, so each entry is read back from it.
  for (i = 0; i < count; ++i) {
    const uint8_t * d = sfnt + GFNT_WRITE_HEADER_BYTES
        + i * GFNT_WRITE_ENTRY_BYTES;
    uint32_t tag = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16)
        | ((uint32_t)d[2] << 8) | d[3];
    uint32_t offset = ((uint32_t)d[8] << 24) | ((uint32_t)d[9] << 16)
        | ((uint32_t)d[10] << 8) | d[11];
    uint32_t length = ((uint32_t)d[12] << 24) | ((uint32_t)d[13] << 16)
        | ((uint32_t)d[14] << 8) | d[15];
    size_t used = length;

    result = gfnt_write_deflate(sfnt + offset, length, tag, &packed[i],
        &packed_length[i], error);
    if (result != GFNT_OK) {
      goto done;
    }
    if (packed[i] && packed_length[i] < length) {
      used = packed_length[i];
    }
    else {
      if (packed[i]) {
        gcomp_buffer_free(NULL, packed[i]);
      }
      packed[i] = NULL;
    }
    packed_length[i] = used;
    if (used > limits->max_blob_bytes
        || gfnt_write_pad4(used) > limits->max_blob_bytes - total) {
      result = gfnt_error_set(error, GFNT_ERR_LIMIT, tag, 0, GFNT_GLYPH_NONE,
          "the font would be larger than the limit allows");
      goto done;
    }
    total += gfnt_write_pad4(used);
    if (tag == GFNT_TAG_HEAD && length >= 8) {
      const uint8_t * p = sfnt + offset + 4;

      revision = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
          | ((uint32_t)p[2] << 8) | p[3];
    }
  }
  woff = allocator->calloc_fn(allocator->ctx, 1, total);
  if (!woff) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the WOFF file");
    goto done;
  }
  gfnt_write_put32(woff, GFNT_TAG('w', 'O', 'F', 'F'));
  gfnt_write_put32(woff + 4, flavour);
  gfnt_write_put32(woff + 8, (uint32_t)total);
  gfnt_write_put16(woff + 12, (uint32_t)count);
  gfnt_write_put16(woff + 14, 0);
  gfnt_write_put32(woff + 16, (uint32_t)sfnt_size);
  gfnt_write_put16(woff + 20, revision >> 16);
  gfnt_write_put16(woff + 22, revision & 0xFFFFu);
  at = GFNT_WOFF_HEADER + count * GFNT_WOFF_ENTRY;
  for (i = 0; i < count; ++i) {
    const uint8_t * d = sfnt + GFNT_WRITE_HEADER_BYTES
        + i * GFNT_WRITE_ENTRY_BYTES;
    uint8_t * e = woff + GFNT_WOFF_HEADER + i * GFNT_WOFF_ENTRY;
    uint32_t offset = ((uint32_t)d[8] << 24) | ((uint32_t)d[9] << 16)
        | ((uint32_t)d[10] << 8) | d[11];
    uint32_t length = ((uint32_t)d[12] << 24) | ((uint32_t)d[13] << 16)
        | ((uint32_t)d[14] << 8) | d[15];

    memcpy(e, d, 4);
    gfnt_write_put32(e + 4, (uint32_t)at);
    gfnt_write_put32(e + 8, (uint32_t)packed_length[i]);
    gfnt_write_put32(e + 12, length);
    memcpy(e + 16, d + 4, 4);
    if (packed_length[i]) {
      memcpy(woff + at, packed[i] ? (const uint8_t *)packed[i] : sfnt + offset,
          packed_length[i]);
    }
    at += gfnt_write_pad4(packed_length[i]);
  }
  result = gfnt_blob_create_memory(woff, total, GFNT_BLOB_COPY, limits,
      allocator, out_blob, error);

done:
  if (packed) {
    for (i = 0; i < count; ++i) {
      if (packed[i]) {
        gcomp_buffer_free(NULL, packed[i]);
      }
    }
  }
  allocator->free_fn(allocator->ctx, woff);
  allocator->free_fn(allocator->ctx, packed_length);
  allocator->free_fn(allocator->ctx, packed);
  allocator->free_fn(allocator->ctx, sfnt);
  return result;
}
