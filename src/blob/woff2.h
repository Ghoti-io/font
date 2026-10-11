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
 * WOFF 2, rebuilt as the sfnt (or collection) it wraps.
 *
 * W3C WOFF File Format 2.0. Like WOFF 1 it is a **wrapper**: the one difference
 * that matters is that the tables are not compressed one at a time. They are
 * concatenated and Brotli-compressed as a single stream, and three of them can be
 * stored in a *transformed* form that the decoder undoes: `glyf` (split into
 * separate streams for the contour counts, point flags, coordinate triplets,
 * composite data, bounding boxes and instructions) with its `loca` (dropped, and
 * rebuilt from the glyph lengths), and `hmtx` (whose left side bearings are
 * dropped when they equal the glyphs' `xMin`). The rebuild happens in the same
 * place as WOFF 1's, in ::gfnt_face_load(), and everything after it is the
 * ordinary probe.
 *
 * What the rebuild checks: the header's reserved field is zero; every extent lies
 * inside the file and the blocks do not overlap; the directory's variable-length
 * integers are well formed (no leading zeros, no overflow); the transform
 * versions are ones the format defines; the Brotli stream decompresses to exactly
 * the sum of the lengths the directory states; every stream a transformed `glyf`
 * reads is long enough; a composite glyph has its bounding box and an empty one
 * does not; the reconstructed `loca` fits its index format; and the sfnt that comes
 * out is no larger than the caller's file-size limit. The directory's
 * `totalSfntSize` and each `origLength` of `glyf` are reference values and are not
 * enforced, as the format says. The sfnt's table checksums and `head`'s
 * `checkSumAdjustment` are recomputed, and for a collection `checkSumAdjustment`
 * is left as the file stored it. The extended metadata and private data blocks are
 * range-checked and not read.
 */

#ifndef GHOTI_IO_GFNT_WOFF2_H
#define GHOTI_IO_GFNT_WOFF2_H

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/macros.h>
#include "../reader/reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The signature a WOFF 2 file opens with: `wOF2`. */
#define GFNT_WOFF2_SIGNATURE GFNT_TAG('w', 'O', 'F', '2')

/**
 * Whether these bytes open with the WOFF 2 signature.
 *
 * @param blob A reader over the whole blob.
 * @return Whether the first four bytes are `wOF2`.
 */
bool gfnt_woff2_looks_like(const GFNT_Reader * blob);

/**
 * How many faces a WOFF 2 file holds, from its directory alone.
 *
 * One for a single font; the collection directory's count for a `ttcf` flavour.
 * Nothing is decompressed.
 *
 * @param in The WOFF 2 file.
 * @param limits The caps.
 * @param out_count Receives the count. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK or ::GFNT_ERR_CORRUPT, ::GFNT_ERR_LIMIT.
 */
GFNT_Result gfnt_woff2_face_count(const GFNT_Blob * in, const GFNT_Limits * limits,
    size_t * out_count, GFNT_Error * error);

/**
 * Rebuild the sfnt, or the `ttcf` collection, a WOFF 2 file wraps.
 *
 * @param in The WOFF 2 file.
 * @param limits The caps: the rebuilt font may not exceed `max_blob_bytes`; the
 *   table count is held to `max_tables`, the glyph count to `max_glyphs`.
 * @param allocator Where the new blob comes from, or NULL for the default.
 * @param out_blob Receives the font. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_CORRUPT for a header, directory, stream or table
 *   that breaks the format; ::GFNT_ERR_LIMIT; ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_woff2_to_sfnt(const GFNT_Blob * in, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_WOFF2_H
