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
 * A growable byte buffer, for the containers that build bytes rather than
 * pointing at them.
 *
 * Most of this library never needs one: a table parser hands out pointers into
 * the blob, because the bytes are already there in the shape the caller wants.
 * Three things are not like that, and each has to assemble an arena of its own -
 * a Type 1 program whose private half is encrypted, a bitmap container whose rows
 * come in four bit and byte orders, and (when it lands) WOFF, whose tables are
 * compressed.
 *
 * It is one file because it was two: this is Type 1's buffer, lifted here when
 * the bitmap containers needed the same four functions. A doubling that grows by
 * a different rule in two places is two reallocation paths for `tools/coverage.sh`
 * to report, and the second copy is the one no test reaches.
 */

#ifndef GHOTI_IO_GFNT_BUFFER_H
#define GHOTI_IO_GFNT_BUFFER_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bytes being built, with their own capacity.
 *
 * Zero-initialise it, set `allocator`, and free it with ::gfnt_buffer_free()
 * unless ::gfnt_buffer_release() has taken the bytes.
 */
typedef struct GFNT_Buffer {
  uint8_t * data;                   ///< The bytes, or NULL while empty.
  size_t length;                    ///< How many are used.
  size_t capacity;                  ///< How many are allocated.
  const GFNT_Allocator * allocator; ///< Where they came from. Required.
} GFNT_Buffer;

/** Start a buffer on @p allocator, owning nothing yet. */
void gfnt_buffer_init(GFNT_Buffer * buffer, const GFNT_Allocator * allocator);

/** Release the bytes and leave the buffer empty; a second call is harmless. */
void gfnt_buffer_free(GFNT_Buffer * buffer);

/**
 * Hand the bytes to the caller and leave the buffer empty.
 *
 * For the usual ending, where the arena outlives the buffer that built it.
 *
 * @param buffer The buffer.
 * @param out_data Receives the bytes, which the caller now frees.
 * @param out_length Receives the length, or NULL.
 */
void gfnt_buffer_release(GFNT_Buffer * buffer, uint8_t ** out_data,
    size_t * out_length);

/**
 * Make room for @p extra more bytes, doubling.
 *
 * @return false only on overflow or a refused allocation.
 */
bool gfnt_buffer_grow(GFNT_Buffer * buffer, size_t extra);

/** Append @p length bytes. A length of zero succeeds and does nothing. */
bool gfnt_buffer_add(GFNT_Buffer * buffer, const uint8_t * bytes,
    size_t length);

/** Append one byte. */
bool gfnt_buffer_byte(GFNT_Buffer * buffer, uint8_t byte);

/**
 * Append @p count zero bytes.
 *
 * What a blank row of a bitmap is, and what pads a short one. Written as its own
 * function because the alternative - appending a byte at a time - is where a
 * 16-pixel row of a 4,000-glyph font stops being free.
 */
bool gfnt_buffer_zeros(GFNT_Buffer * buffer, size_t count);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_BUFFER_H
