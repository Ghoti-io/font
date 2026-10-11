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
 * `sbix`: Apple's color bitmap strikes.
 *
 * documentation/design.md section 7.5. A table of strikes, each a pixel size and an
 * offset per glyph to a record of an origin, a four-letter type and the picture. The
 * strike list is small and is read from the table each time rather than memoized:
 * a count and one offset per strike.
 *
 * Reference: Apple's TrueType Reference Manual, "sbix". Everything beyond what that
 * says - that a chain of `dupe` records is followed to eight hops and no further,
 * that one strike answers for every glyph - was found by experiment against
 * `hb-shape` (`tools/oracle/color_extents_diff.py`) and is stated where it is used.
 */

#ifndef GHOTI_IO_GFNT_SBIX_H
#define GHOTI_IO_GFNT_SBIX_H

#include <ghoti.io/font/bitmap.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The tag this file reads: `sbix`. */
#define GFNT_TAG_SBIX GFNT_TAG('s', 'b', 'i', 'x')

/** @brief The only version Apple defines. */
#define GFNT_SBIX_VERSION 1u

/** @brief Hops through `dupe` records a glyph may take to reach its picture. */
#define GFNT_SBIX_MAX_DUPE_HOPS 8u

/** Whether the face has an `sbix`. */
bool gfnt_face_has_sbix(const GFNT_Face * face);

/** The number of strikes; ::GFNT_ERR_CORRUPT or ::GFNT_ERR_UNSUPPORTED for a bad table. */
GFNT_Result gfnt_sbix_strike_count(const GFNT_Face * face, size_t * out_count,
    GFNT_Error * error);

/** Strike @p index, as ::gfnt_face_strike_at() reports it. */
GFNT_Result gfnt_sbix_strike_at(const GFNT_Face * face, size_t index,
    GFNT_Strike * out_strike, GFNT_Error * error);

/** The strike a size picks, by the rule of ::gfnt_strike_better(); 0 if none. */
size_t gfnt_sbix_strike_for(const GFNT_Face * face, uint32_t requested);

/**
 * One glyph of one strike, following `dupe` records to the picture.
 *
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED for a glyph the strike does not carry
 *   ("does not carry" is in the message) and for a record type this library does
 *   not read; ::GFNT_ERR_CORRUPT for a record the table gets wrong.
 */
GFNT_Result gfnt_sbix_find(const GFNT_Face * face, size_t strike, uint32_t glyph,
    GFNT_GlyphImage * out, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_SBIX_H
