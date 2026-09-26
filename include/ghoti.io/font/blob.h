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
 * @ref GFNT_Blob "GFNT_Blob": the bytes of a font, with their length and their ownership.
 *
 * documentation/design.md section 5.1. There is no `GFNT_Stream` in this
 * library and the departure is deliberate: a font is random-access by
 * construction - the table directory is a list of offsets into the file - so a
 * sequential stream would be seeked past on the first read. A blob and the
 * checked reader over it are the stream's replacement, and CONVENTIONS.md
 * section 13 records the departure.
 */

#ifndef GHOTI_IO_GFNT_BLOB_H
#define GHOTI_IO_GFNT_BLOB_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The bytes of a font file.
 *
 * Opaque: a blob's fields are how it was created, which no caller needs and
 * every caller would be tempted to read past. ::gfnt_blob_data() and
 * ::gfnt_blob_size() are the whole of the accessible surface, and the checked
 * reader (documentation/design.md section 6) is what the library itself uses.
 */
typedef struct GFNT_Blob GFNT_Blob;

/**
 * @brief Whether a blob over caller-supplied memory copies it or borrows it.
 *
 * Named in the call rather than implied, as `image` names GIMG_RASTER_BORROWED
 * (CONVENTIONS.md section 5): a library that borrows by default eventually
 * outlives something.
 */
typedef enum {
  /** Copy the caller's bytes; the blob is independent of them afterwards. */
  GFNT_BLOB_COPY = 0,
  /**
   * Borrow the caller's bytes. They must stay allocated and unchanged for the
   * blob's whole lifetime, and for the lifetime of every face loaded from it.
   * Zero is the copy, so a caller who never thought about it cannot pick this
   * by accident.
   */
  GFNT_BLOB_BORROWED
} GFNT_BlobOwnership;

/**
 * @brief Create a blob over bytes the caller supplies.
 *
 * @param data The bytes. May be NULL only when @p size is zero.
 * @param size How many bytes.
 * @param ownership ::GFNT_BLOB_COPY or ::GFNT_BLOB_BORROWED.
 * @param limits Caps to apply, or NULL for the defaults. A @p size above
 *   `max_blob_bytes` is refused with ::GFNT_ERR_LIMIT rather than truncated.
 * @param allocator Allocator for the blob, or NULL for the default.
 * @param out_blob Receives the blob, owned by the caller and released with
 *   ::gfnt_blob_destroy(). Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_LIMIT or
 *   ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_blob_create_memory(const void * data, size_t size,
    GFNT_BlobOwnership ownership, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob,
    GFNT_Error * error);

/**
 * @brief Create a blob by reading a file whole.
 *
 * This is the default way to open a font, and ::gfnt_blob_create_mmap() is
 * the exception rather than the other way round: see that call for why.
 *
 * @param path The file, UTF-8, handed to the platform's open as an opaque
 *   string.
 * @param limits Caps to apply, or NULL for the defaults. A file larger than
 *   `max_blob_bytes` is refused with ::GFNT_ERR_LIMIT and nothing is read.
 * @param allocator Allocator for the blob and its bytes, or NULL for the
 *   default.
 * @param out_blob Receives the blob. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_LIMIT, ::GFNT_ERR_OOM or
 *   ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_blob_create_file(const char * path,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Blob ** out_blob, GFNT_Error * error);

/**
 * @brief Create a blob over a memory-mapped file.
 *
 * **A mapped file that is truncated after mapping raises `SIGBUS` on the next
 * access, and C has no way to recover from it.** That is the whole reason
 * reading is the default (M23 in documentation/design.md section 2): a font
 * this library did not write, in a directory it does not control, can be
 * replaced under it. Map only a font you trust not to change - a large CJK
 * collection you ship, say - and read everything else.
 *
 * @param path The file, UTF-8.
 * @param limits Caps to apply, or NULL for the defaults. A file larger than
 *   `max_blob_bytes` is unmapped again and refused with ::GFNT_ERR_LIMIT.
 * @param allocator Allocator for the blob itself, or NULL for the default.
 *   The bytes are the mapping's and are not allocated.
 * @param out_blob Receives the blob. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_LIMIT, ::GFNT_ERR_OOM or
 *   ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_blob_create_mmap(const char * path,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Blob ** out_blob, GFNT_Error * error);

/**
 * @brief Release a blob.
 *
 * Frees the bytes if the blob copied or read them, and unmaps them if it
 * mapped them. Borrowed bytes are left alone: a library never frees what it
 * did not allocate.
 *
 * @param blob The blob, or NULL.
 */
GFNT_API void gfnt_blob_destroy(GFNT_Blob * blob);

/**
 * @brief The blob's bytes.
 *
 * @param blob The blob, or NULL.
 * @return The bytes, or NULL for a NULL or empty blob.
 */
GFNT_API const uint8_t * gfnt_blob_data(const GFNT_Blob * blob);

/**
 * @brief The blob's length in bytes.
 *
 * @param blob The blob, or NULL.
 * @return The length, or 0 for a NULL blob.
 */
GFNT_API size_t gfnt_blob_size(const GFNT_Blob * blob);

/**
 * @brief Whether the blob's bytes are a memory mapping.
 *
 * Exposed because it changes what a truncation of the underlying file does to
 * the process, which is a caller's decision to make and not this library's to
 * hide.
 *
 * @param blob The blob, or NULL.
 * @return true if the bytes came from ::gfnt_blob_create_mmap().
 */
GFNT_API bool gfnt_blob_is_mapped(const GFNT_Blob * blob);

/**
 * @brief Write a human-readable description of the blob to a stream.
 *
 * @param blob The blob.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID if either argument is NULL, or
 *   ::GFNT_ERR_IO if the stream refused a write.
 */
GFNT_API GFNT_Result gfnt_blob_dump(const GFNT_Blob * blob, FILE * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_BLOB_H
