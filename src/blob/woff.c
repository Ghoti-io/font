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

#include <string.h>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/font/macros.h>
#include "woff.h"

#define GFNT_TAG_WOFF GFNT_TAG('W', 'O', 'F', 'F')
#define GFNT_WOFF_HEADER_BYTES 44u
#define GFNT_WOFF_ENTRY_BYTES 20u
#define GFNT_SFNT_HEADER_BYTES 12u
#define GFNT_SFNT_ENTRY_BYTES 16u

/** One directory entry, as the WOFF file states it. */
typedef struct GFNT_WoffEntry {
  uint32_t tag;
  uint32_t offset;
  uint32_t comp_length;
  uint32_t orig_length;
  uint32_t checksum;
  size_t out_offset; ///< Where it lands in the rebuilt sfnt.
} GFNT_WoffEntry;

bool gfnt_woff_looks_like(const GFNT_Reader * blob) {
  uint32_t signature = 0;

  return gfnt_reader_u32_at(blob, 0, &signature) == GFNT_OK
      && signature == GFNT_WOFF_SIGNATURE;
}

static size_t gfnt_woff_pad4(size_t n) {
  return (n + 3u) & ~(size_t)3u;
}

/** Inflate one zlib-wrapped table to exactly @p orig bytes into @p dest. */
static GFNT_Result gfnt_woff_inflate_table(const uint8_t * bytes, size_t comp,
    size_t orig, uint8_t * dest, uint32_t tag, GFNT_Error * error) {
  gcomp_options_t * options = NULL;
  gcomp_status_t status;
  void * inflated = NULL;
  size_t length = 0;

  if (gcomp_options_create(&options) != GCOMP_OK) {
    return gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE,
        "allocating the options for an inflate");
  }
  // The table says how long it is once inflated, so that is the ceiling: a
  // stream that would produce more is refused by the decoder and not after it
  // has filled memory.
  if (gcomp_options_set_uint64(options, "limits.max_output_bytes",
          (uint64_t)orig) != GCOMP_OK) {
    gcomp_options_destroy(options);
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, tag, 0, GFNT_GLYPH_NONE,
        "the compression library refused an option this library sets");
  }
  status = gcomp_decode_alloc(NULL, "zlib", options, bytes, comp, &inflated,
      &length);
  gcomp_options_destroy(options);
  if (status != GCOMP_OK) {
    return gfnt_error_set(error,
        status == GCOMP_ERR_MEMORY ? GFNT_ERR_OOM
            : (status == GCOMP_ERR_LIMIT || status == GCOMP_ERR_CORRUPT
                  || status == GCOMP_ERR_UNSUPPORTED) ? GFNT_ERR_CORRUPT
            : GFNT_ERR_INTERNAL,
        tag, 0, GFNT_GLYPH_NONE, gcomp_status_to_string(status));
  }
  if (length != orig) {
    gcomp_buffer_free(NULL, inflated);
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0, GFNT_GLYPH_NONE,
        "a WOFF table inflated to a different length than its directory states");
  }
  memcpy(dest, inflated, orig);
  gcomp_buffer_free(NULL, inflated);
  return GFNT_OK;
}

static void gfnt_woff_put32(uint8_t * p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void gfnt_woff_put16(uint8_t * p, uint32_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

GFNT_Result gfnt_woff_to_sfnt(const GFNT_Blob * in, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob, GFNT_Error * error) {
  GFNT_Reader reader;
  GFNT_WoffEntry * entries = NULL;
  uint8_t * sfnt = NULL;
  uint32_t flavor = 0;
  uint32_t length = 0;
  uint16_t count = 0;
  uint16_t reserved = 0;
  size_t total;
  size_t i;
  GFNT_Result result;

  if (!in || !limits || !out_blob) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_WOFF, 0,
        GFNT_GLYPH_NONE, "no input, no limits, or nowhere to put the blob");
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  result = gfnt_reader_init_blob(&reader, in, GFNT_TAG_WOFF, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (reader.length < GFNT_WOFF_HEADER_BYTES
      || gfnt_reader_u32_at(&reader, 4, &flavor) != GFNT_OK
      || gfnt_reader_u32_at(&reader, 8, &length) != GFNT_OK
      || gfnt_reader_u16_at(&reader, 12, &count) != GFNT_OK
      || gfnt_reader_u16_at(&reader, 14, &reserved) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF, 0,
        GFNT_GLYPH_NONE, "too short to hold a WOFF header");
  }
  if (reserved != 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF, 14,
        GFNT_GLYPH_NONE, "the WOFF header's reserved field is not zero");
  }
  if (length > reader.length) {
    // Trailing bytes after the stated end are tolerated, as every sfnt reader
    // tolerates them; a file shorter than it says is cut off.
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF, 8,
        GFNT_GLYPH_NONE, "the WOFF header states more bytes than the file has");
  }
  if (count == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF, 12,
        GFNT_GLYPH_NONE, "a WOFF font with no tables");
  }
  if (count > limits->max_tables) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_WOFF, 12,
        GFNT_GLYPH_NONE, "more tables than the limit allows");
  }
  if ((size_t)count * GFNT_WOFF_ENTRY_BYTES
      > reader.length - GFNT_WOFF_HEADER_BYTES) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF,
        GFNT_WOFF_HEADER_BYTES, GFNT_GLYPH_NONE,
        "the table directory runs past the end of the file");
  }
  entries = allocator->calloc_fn(allocator->ctx, count, sizeof *entries);
  if (!entries) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_WOFF, 0, GFNT_GLYPH_NONE,
        "allocating the table directory");
  }

  total = GFNT_SFNT_HEADER_BYTES + (size_t)count * GFNT_SFNT_ENTRY_BYTES;
  for (i = 0; i < count; ++i) {
    size_t at = GFNT_WOFF_HEADER_BYTES + i * GFNT_WOFF_ENTRY_BYTES;
    GFNT_WoffEntry * e = &entries[i];

    if (gfnt_reader_u32_at(&reader, at, &e->tag) != GFNT_OK
        || gfnt_reader_u32_at(&reader, at + 4, &e->offset) != GFNT_OK
        || gfnt_reader_u32_at(&reader, at + 8, &e->comp_length) != GFNT_OK
        || gfnt_reader_u32_at(&reader, at + 12, &e->orig_length) != GFNT_OK
        || gfnt_reader_u32_at(&reader, at + 16, &e->checksum) != GFNT_OK) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF, at,
          GFNT_GLYPH_NONE, "a WOFF directory entry cut short");
      goto fail;
    }
    if (i > 0 && e->tag <= entries[i - 1].tag) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, e->tag, at,
          GFNT_GLYPH_NONE, "the WOFF directory is not in ascending tag order, "
          "or a tag is repeated");
      goto fail;
    }
    if (e->comp_length > e->orig_length) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, e->tag, at + 8,
          GFNT_GLYPH_NONE, "a WOFF table is stored larger than its own length");
      goto fail;
    }
    if ((uint64_t)e->offset + e->comp_length > reader.length) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, e->tag, at + 4,
          GFNT_GLYPH_NONE, "a WOFF table runs past the end of the file");
      goto fail;
    }
    if (e->offset < GFNT_WOFF_HEADER_BYTES
            + (size_t)count * GFNT_WOFF_ENTRY_BYTES) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, e->tag, at + 4,
          GFNT_GLYPH_NONE, "a WOFF table starts inside the directory");
      goto fail;
    }
    e->out_offset = total;
    if (e->orig_length > limits->max_blob_bytes
        || gfnt_woff_pad4(e->orig_length) > limits->max_blob_bytes - total) {
      result = gfnt_error_set(error, GFNT_ERR_LIMIT, e->tag, at + 12,
          GFNT_GLYPH_NONE, "the font would be larger than the limit allows");
      goto fail;
    }
    total += gfnt_woff_pad4(e->orig_length);
  }

  sfnt = allocator->calloc_fn(allocator->ctx, 1, total);
  if (!sfnt) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_WOFF, 0,
        GFNT_GLYPH_NONE, "allocating the rebuilt font");
    goto fail;
  }
  {
    // The offset table. The search fields are what the specification derives
    // from the table count: the largest power of two not above it.
    unsigned entry_selector = 0;
    unsigned search_range;

    while ((2u << entry_selector) <= count) {
      ++entry_selector;
    }
    search_range = (1u << entry_selector) * 16u;
    gfnt_woff_put32(sfnt, flavor);
    gfnt_woff_put16(sfnt + 4, count);
    gfnt_woff_put16(sfnt + 6, search_range);
    gfnt_woff_put16(sfnt + 8, entry_selector);
    gfnt_woff_put16(sfnt + 10, (unsigned)count * 16u - search_range);
  }
  for (i = 0; i < count; ++i) {
    const GFNT_WoffEntry * e = &entries[i];
    uint8_t * d = sfnt + GFNT_SFNT_HEADER_BYTES + i * GFNT_SFNT_ENTRY_BYTES;
    const uint8_t * bytes = NULL;
    GFNT_Reader table;

    gfnt_woff_put32(d, e->tag);
    gfnt_woff_put32(d + 4, e->checksum);
    gfnt_woff_put32(d + 8, (uint32_t)e->out_offset);
    gfnt_woff_put32(d + 12, e->orig_length);
    if (e->orig_length == 0) {
      continue;
    }
    result = gfnt_reader_sub(&reader, e->offset, e->comp_length, &table);
    if (result == GFNT_OK) {
      result = gfnt_read_bytes(&table, e->comp_length, &bytes);
    }
    if (result != GFNT_OK) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, e->tag, e->offset,
          GFNT_GLYPH_NONE, "a WOFF table runs past the end of the file");
      goto fail;
    }
    if (e->comp_length == e->orig_length) {
      memcpy(sfnt + e->out_offset, bytes, e->orig_length);
    }
    else {
      result = gfnt_woff_inflate_table(bytes, e->comp_length, e->orig_length,
          sfnt + e->out_offset, e->tag, error);
      if (result != GFNT_OK) {
        goto fail;
      }
    }
  }
  result = gfnt_blob_create_memory(sfnt, total, GFNT_BLOB_COPY, limits,
      allocator, out_blob, error);
  allocator->free_fn(allocator->ctx, sfnt);
  allocator->free_fn(allocator->ctx, entries);
  return result;

fail:
  if (sfnt) {
    allocator->free_fn(allocator->ctx, sfnt);
  }
  allocator->free_fn(allocator->ctx, entries);
  return result;
}
