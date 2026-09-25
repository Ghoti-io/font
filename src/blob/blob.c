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
 * @ref GFNT_Blob: bytes, length, ownership.
 *
 * documentation/design.md section 5.1. Three sources - copied memory,
 * borrowed memory, and a file read whole - plus a mapping the caller has to
 * ask for by name, because a mapping is a SIGBUS waiting for someone else to
 * truncate the file (M23).
 */

#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/mmap.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/blob.h>
#include <string.h>

/**
 * A blob's bytes and how they were come by.
 *
 * `data` is what a reader gets; the rest is teardown. `owned` is non-NULL
 * exactly when this blob must free the bytes, which distinguishes a copy from
 * a borrow without a second flag to keep in step.
 */
struct GFNT_Blob {
  const uint8_t * data;                ///< The bytes, or NULL when empty.
  size_t size;                         ///< How many.
  const GFNT_Allocator * allocator;    ///< Where this blob came from.
  void * owned;                        ///< Bytes to free, or NULL if borrowed.
  bool mapped;                         ///< Whether `map` holds a mapping.
  GCU_Mapped_File map;                 ///< The mapping, when `mapped`.
};

/**
 * The limits to use: the caller's, or the defaults.
 */
static void gfnt_blob_limits(const GFNT_Limits * limits, GFNT_Limits * out) {
  if (limits) {
    *out = *limits;
    return;
  }
  gfnt_limits_default(out);
}

/**
 * Allocate an empty blob carrying its allocator.
 */
static GFNT_Blob * gfnt_blob_alloc(const GFNT_Allocator * allocator) {
  GFNT_Blob * blob = allocator->calloc_fn(allocator->ctx, 1, sizeof *blob);

  if (!blob) {
    return NULL;
  }
  blob->allocator = allocator;
  return blob;
}

GFNT_Result gfnt_blob_create_memory(const void * data, size_t size,
    GFNT_BlobOwnership ownership, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob,
    GFNT_Error * error) {
  GFNT_Limits effective;
  GFNT_Blob * blob;

  gfnt_error_clear(error);
  if (!out_blob || (!data && size > 0)) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no bytes, or nowhere to put the blob");
  }
  if (ownership != GFNT_BLOB_COPY && ownership != GFNT_BLOB_BORROWED) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "ownership is neither GFNT_BLOB_COPY nor GFNT_BLOB_BORROWED");
  }

  gfnt_blob_limits(limits, &effective);
  if (size > effective.max_blob_bytes) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "font is larger than GFNT_Limits::max_blob_bytes");
  }

  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  blob = gfnt_blob_alloc(allocator);
  if (!blob) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the blob");
  }

  if (ownership == GFNT_BLOB_COPY && size > 0) {
    void * copy = allocator->malloc_fn(allocator->ctx, size);

    if (!copy) {
      allocator->free_fn(allocator->ctx, blob);
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "copying the font's bytes");
    }
    memcpy(copy, data, size);
    blob->owned = copy;
    blob->data = copy;
  }
  else {
    blob->data = size > 0 ? (const uint8_t *)data : NULL;
  }
  blob->size = size;

  *out_blob = blob;
  return GFNT_OK;
}

GFNT_Result gfnt_blob_create_file(const char * path,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Blob ** out_blob, GFNT_Error * error) {
  GFNT_Limits effective;
  GFNT_Blob * blob;
  void * data = NULL;
  size_t size = 0;
  GCU_File_Result read;

  gfnt_error_clear(error);
  if (!path || !out_blob) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no path, or nowhere to put the blob");
  }

  gfnt_blob_limits(limits, &effective);
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }

  // The limit goes to the read rather than being checked afterwards: a file
  // above it must not be read at all, since reading it is the cost the limit
  // exists to refuse.
  read = gcu_file_read(path, effective.max_blob_bytes, allocator, &data, &size);
  if (read != GCU_FILE_OK) {
    switch (read) {
      case GCU_FILE_ERR_LIMIT:
        return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
            "font is larger than GFNT_Limits::max_blob_bytes");
      case GCU_FILE_ERR_OOM:
        return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
            "reading the font file");
      case GCU_FILE_ERR_INVALID:
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
            "the path is not one this platform can open");
      default:
        return gfnt_error_set(error, GFNT_ERR_IO, 0, 0, GFNT_GLYPH_NONE,
            gcu_file_result_string(read));
    }
  }

  blob = gfnt_blob_alloc(allocator);
  if (!blob) {
    gcu_file_free(allocator, data);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the blob");
  }
  blob->owned = data;
  blob->data = size > 0 ? (const uint8_t *)data : NULL;
  blob->size = size;

  *out_blob = blob;
  return GFNT_OK;
}

GFNT_Result gfnt_blob_create_mmap(const char * path,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Blob ** out_blob, GFNT_Error * error) {
  GFNT_Limits effective;
  GFNT_Blob * blob;
  GCU_Mapped_File map;

  gfnt_error_clear(error);
  if (!path || !out_blob) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no path, or nowhere to put the blob");
  }

  gfnt_blob_limits(limits, &effective);
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }

  if (gcu_mmap_open(&map, path, false) != 0) {
    return gfnt_error_set(error, GFNT_ERR_IO, 0, 0, GFNT_GLYPH_NONE,
        "mapping the font file");
  }
  if (map.size > effective.max_blob_bytes) {
    gcu_mmap_close(&map);
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "font is larger than GFNT_Limits::max_blob_bytes");
  }

  blob = gfnt_blob_alloc(allocator);
  if (!blob) {
    gcu_mmap_close(&map);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the blob");
  }
  blob->map = map;
  blob->mapped = true;
  blob->data = map.size > 0 ? (const uint8_t *)map.data : NULL;
  blob->size = map.size;

  *out_blob = blob;
  return GFNT_OK;
}

void gfnt_blob_destroy(GFNT_Blob * blob) {
  const GFNT_Allocator * allocator;

  if (!blob) {
    return;
  }
  allocator = blob->allocator;

  if (blob->mapped) {
    gcu_mmap_close(&blob->map);
  }
  else if (blob->owned) {
    // Bytes from gcu_file_read() and bytes from the allocator's malloc are
    // both released through the allocator; gcu_file_free() is that call.
    gcu_file_free(allocator, blob->owned);
  }
  allocator->free_fn(allocator->ctx, blob);
}

const uint8_t * gfnt_blob_data(const GFNT_Blob * blob) {
  return blob ? blob->data : NULL;
}

size_t gfnt_blob_size(const GFNT_Blob * blob) {
  return blob ? blob->size : 0;
}

bool gfnt_blob_is_mapped(const GFNT_Blob * blob) {
  return blob ? blob->mapped : false;
}

GFNT_Result gfnt_blob_dump(const GFNT_Blob * blob, FILE * out) {
  const char * source;

  if (!blob || !out) {
    return GFNT_ERR_INVALID;
  }

  if (blob->mapped) {
    source = "mapped";
  }
  else if (blob->owned) {
    source = "owned";
  }
  else {
    source = "borrowed";
  }

  if (fprintf(out, "blob: %zu bytes, %s\n", blob->size, source) < 0) {
    return GFNT_ERR_IO;
  }
  return GFNT_OK;
}
