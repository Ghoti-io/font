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
 * WOFF 1, rebuilt as the sfnt it wraps.
 *
 * W3C WOFF File Format 1.0. A WOFF file is an sfnt whose tables are compressed one
 * at a time (zlib, RFC 1950) and listed in a directory of its own, with a header
 * that says how large the whole font is once it is put back. So it is a
 * **wrapper** like gzip and not a container with a directory of its own to parse:
 * rebuilding the sfnt produces an ordinary font, and everything after that is the
 * ordinary probe. It happens once, in ::gfnt_face_load(), after any gzip layer is
 * undone and before the flavour is looked at, and the face owns the rebuilt bytes
 * the way it owns an inflated PCF.
 *
 * What the rebuild checks, because it is the only place that can: the header's
 * reserved field is zero; every table lies inside the file; a table's compressed
 * length never exceeds its original length (equal means stored as is); a
 * compressed table inflates to exactly its stated original length; and the sfnt
 * that comes out is no larger than the caller's file-size limit. The checksums the
 * directory carries go into the rebuilt directory untouched, and are not enforced,
 * as everywhere else in this library. The extended metadata and the private data
 * blocks are not read.
 */

#ifndef GHOTI_IO_GFNT_WOFF_H
#define GHOTI_IO_GFNT_WOFF_H

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/macros.h>
#include "../reader/reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The signature a WOFF 1 file opens with: `wOFF`. */
#define GFNT_WOFF_SIGNATURE GFNT_TAG('w', 'O', 'F', 'F')

/**
 * Whether these bytes open with the WOFF 1 signature.
 *
 * @param blob A reader over the whole blob.
 * @return Whether the first four bytes are `wOFF`.
 */
bool gfnt_woff_looks_like(const GFNT_Reader * blob);

/**
 * Rebuild the sfnt a WOFF 1 file wraps, into a blob of its own.
 *
 * @param in The WOFF file.
 * @param limits The caps: the rebuilt sfnt may not exceed `max_blob_bytes`, and
 *   the table count is held to `max_tables`.
 * @param allocator Where the new blob comes from, or NULL for the default.
 * @param out_blob Receives the sfnt. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_CORRUPT for a header, directory or table that
 *   breaks the format; ::GFNT_ERR_LIMIT for a font that would be too large or have
 *   too many tables; ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_woff_to_sfnt(const GFNT_Blob * in, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_WOFF_H
