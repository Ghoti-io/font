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
 * A font file that arrives compressed: the gzip wrapper, undone.
 *
 * documentation/design.md section 7.1. A PCF ships as `.pcf.gz` almost
 * everywhere - all 234 of them in the oracle image do - and a PSF often as
 * `.psf.gz`, because X and the Linux console both read them that way. So gzip is
 * not a feature of those formats but the shape they are found in, and a reader
 * that cannot undo it reads almost no PCF that exists.
 *
 * **This is a wrapper and not a container.** Inflating a `.pcf.gz` produces a
 * PCF, and everything after that is the ordinary probe: the gzip layer decides
 * nothing about what the font is. So it happens once, in ::gfnt_face_load(),
 * before the flavour is looked at, and the rest of the library never learns that
 * the file was compressed.
 *
 * The bytes are **derived**, in exactly the sense `GFNT_Face::bytes` and
 * `GFNT_Face::owned` already mean for a Type 1 program - and the two can stack: a
 * `.pfb.gz` is inflated here and then deciphered there, each layer freeing the one
 * it consumed once it has read it. ::gfnt_face_adopt_bytes() is what makes that
 * safe.
 *
 * WOFF wants `compress` too and will not come through here: its tables are
 * compressed one at a time, with zlib rather than gzip, and reconstructing the
 * sfnt is a container's work rather than a wrapper's.
 */

#ifndef GHOTI_IO_GFNT_GZIP_H
#define GHOTI_IO_GFNT_GZIP_H

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/macros.h>
#include "../reader/reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Whether these bytes open with gzip's magic.
 *
 * RFC 1952's first two bytes, `0x1F 0x8B`, and the third - the compression
 * method - which the specification defines exactly one value of. Three bytes
 * rather than two because two is weak enough to matter here: this probe runs
 * before every other, on every file, and a font whose first two bytes happened to
 * collide would be handed to an inflater instead of to its own reader.
 *
 * No font format this library reads begins with `0x1F`: an sfnt starts with a
 * version or a tag, a PCF with `0x01`, a PSF with `0x36` or `0x72`, a Type 1 with
 * `0x80` or `%`, a BDF with `S`, and a `.hex` with a hexadecimal digit.
 *
 * @param blob A reader over the whole blob.
 * @return Whether it looks like a gzip member.
 */
bool gfnt_gzip_looks_like(const GFNT_Reader * blob);

/**
 * Inflate a gzip member into a blob of its own.
 *
 * The ceiling is ::GFNT_Limits::max_blob_bytes - the same cap a font file read
 * from disk is held to, because what comes out of here *is* the font file as far
 * as everything above is concerned. A member that would inflate past it is
 * ::GFNT_ERR_LIMIT and not an allocation: `compress` checks every enlargement
 * against the limit before making it, so a decompression bomb is refused by the
 * documented number rather than by exhausting memory.
 *
 * Concatenated members are accepted, because `gzip` produces them and
 * `cat a.gz b.gz` is a legal gzip file. A font made of two concatenated members is
 * not a thing anyone ships, but refusing one would be this library inventing a
 * rule RFC 1952 does not have.
 *
 * @param in The compressed bytes. Borrowed; nothing is kept.
 * @param limits The caps to inflate under. Required.
 * @param allocator Where the blob and its bytes come from, or NULL for the
 *   default.
 * @param out_blob Receives the inflated blob, which the caller owns and destroys
 *   with ::gfnt_blob_destroy(). Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_CORRUPT for bytes that are not a gzip member or
 *   whose CRC or length disagrees with the data; ::GFNT_ERR_LIMIT for one that
 *   inflates past the cap; ::GFNT_ERR_OOM; or ::GFNT_ERR_INTERNAL if the
 *   compression library refuses for a reason that is neither.
 */
GFNT_Result gfnt_gzip_inflate(const GFNT_Blob * in, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob,
    GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_GZIP_H
