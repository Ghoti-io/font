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
 * Writing fonts: the sfnt and WOFF 1 serialisers, and the subsetter.
 *
 * Tier W (documentation/design.md sections 4 and 12). It reads through tier 0
 * and nothing above it, and it is not part of the `font.h` umbrella: a consumer
 * that only reads should not link a writer.
 *
 * Every blob this header produces is owned by the caller and released with
 * ::gfnt_blob_destroy(); the face that reads it back is loaded the ordinary way.
 */

#ifndef GHOTI_IO_GFNT_WRITE_H
#define GHOTI_IO_GFNT_WRITE_H

#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One table to be written: its tag and its bytes.
 *
 * The bytes are borrowed for the duration of the call. Their length need not be
 * a multiple of four - the serialiser pads, and the directory states the
 * unpadded length.
 */
typedef struct GFNT_WriteTable {
  GFNT_Tag tag;         ///< The table's tag.
  const uint8_t * data; ///< Its bytes; may be NULL only when @c length is zero.
  size_t length;        ///< How many.
} GFNT_WriteTable;

/**
 * @brief Serialise tables as an sfnt.
 *
 * Tables are written sorted by tag, each starting on a four-byte boundary and
 * padded with zeros, under an offset table whose `searchRange`,
 * `entrySelector` and `rangeShift` are derived from the count. Each directory
 * checksum is computed from the bytes, and when a `head` table of at least
 * twelve bytes is among them its `checkSumAdjustment` is computed last, over
 * the whole file, as the specification requires; whatever the caller put in
 * those four bytes is replaced.
 *
 * Nothing else about a table is looked at: this is a serialiser, not a
 * validator, and a font that is wrong going in is wrong coming out.
 *
 * @param flavour The sfnt version: `0x00010000` for TrueType outlines, `OTTO`
 *   for CFF.
 * @param tables The tables, in any order.
 * @param count How many. At least one, and no more than `limits->max_tables`.
 * @param limits Caps, or NULL for the defaults. The finished font may not exceed
 *   `max_blob_bytes`.
 * @param allocator Where the blob comes from, or NULL for the default.
 * @param out_blob Receives the font. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for no tables, a repeated tag or a
 *   table with bytes missing; ::GFNT_ERR_LIMIT; ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_write_sfnt(GFNT_Tag flavour,
    const GFNT_WriteTable * tables, size_t count, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob, GFNT_Error * error);

/**
 * @brief Serialise tables as a WOFF 1 file.
 *
 * The same sfnt ::gfnt_write_sfnt() builds, with each table compressed on its
 * own (zlib, through `compress`) and stored uncompressed whenever compression
 * does not make it smaller, as the format requires. No metadata or private
 * block is written. The font version in the header is `head.fontRevision`'s
 * integer and fraction halves, or zero without a `head`.
 *
 * @param flavour As for ::gfnt_write_sfnt().
 * @param tables As for ::gfnt_write_sfnt().
 * @param count As for ::gfnt_write_sfnt().
 * @param limits As for ::gfnt_write_sfnt().
 * @param allocator As for ::gfnt_write_sfnt().
 * @param out_blob Receives the WOFF file. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return As for ::gfnt_write_sfnt(), and ::GFNT_ERR_INTERNAL if the
 *   compression library fails for a reason other than memory.
 */
GFNT_API GFNT_Result gfnt_write_woff(GFNT_Tag flavour,
    const GFNT_WriteTable * tables, size_t count, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_WRITE_H
