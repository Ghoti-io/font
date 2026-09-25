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
 * What a glyph can be, and which strike answers a pixel size.
 *
 * documentation/design.md sections 5.3 and 5.4, and decision 3: bitmap fonts
 * are first-class **from phase 0's API**, not a later feature. Five different
 * things can come back for one glyph index, a caller that cannot ask which
 * cannot composite it, and a strike list that arrives in phase 3 is a break
 * across the whole API. So the enums and the strike accessors are here now.
 *
 * What is **not** here is any glyph data: the bitmap parsers arrive in phase
 * 1b and outlines in phase 1. The accessors below are honest about that in the
 * one way that matters - a face carrying `EBLC`, `CBLC` or `sbix` reports
 * ::GFNT_ERR_UNSUPPORTED rather than "no strikes", because a library that
 * answered zero would be telling a caller a bitmap font has no bitmaps.
 */

#ifndef GHOTI_IO_GFNT_GLYPH_H
#define GHOTI_IO_GFNT_GLYPH_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What a glyph turned out to be.
 *
 * The tag of the union design.md section 5.4 describes. A caller that cannot
 * ask which of these it has cannot composite the result, which is why the kind
 * is part of the API before any of the data is.
 */
typedef enum {
  GFNT_GLYPH_OUTLINE,     ///< A path, quadratic (`glyf`) or cubic (CFF).
  GFNT_GLYPH_BITMAP_MONO, ///< A 1-bit strike, with its own metrics.
  GFNT_GLYPH_BITMAP_GRAY, ///< A 2-, 4- or 8-bit strike.
  GFNT_GLYPH_BITMAP_PNG,  ///< `CBDT` or `sbix` bytes, needing a decoder.
  GFNT_GLYPH_COLR_LAYERS, ///< A `COLR` v0 layer list or a v1 paint graph.
  GFNT_GLYPH_SVG,         ///< Absent (section 16); the arm completes the enum.
  GFNT_GLYPH_KIND_COUNT   ///< Closes the enum, so a test can check the strings.
} GFNT_GlyphKind;

/**
 * @brief Name a glyph kind, for diagnostics and dumps.
 *
 * @param kind The kind.
 * @return A static string, never NULL.
 */
GFNT_API const char * gfnt_glyph_kind_string(GFNT_GlyphKind kind);

/**
 * @brief Which strike answers a pixel size, when a face has any.
 *
 * Zero is the policy that cannot surprise a caller who never heard of strikes:
 * it ignores them entirely. M9 is the mistake the other three exist to let a
 * caller avoid - a bitmap strike scaled as though it were an outline, or an
 * outline ignored because a strike existed, is how 13-pixel text ends up blurry
 * next to crisp 12-pixel text.
 */
typedef enum {
  /** Ignore strikes; ::GFNT_ERR_UNSUPPORTED if the face has no outlines. */
  GFNT_STRIKE_OUTLINES_ONLY = 0,
  /** A strike at exactly this ppem, else outlines. */
  GFNT_STRIKE_EXACT,
  /** The nearest strike, used unscaled, else outlines. */
  GFNT_STRIKE_NEAREST,
  /** The nearest strike, scaled if it has to be; the last resort. */
  GFNT_STRIKE_PREFER_STRIKE
} GFNT_StrikePolicy;

/**
 * @brief One pixel size at which a face carries bitmaps.
 */
typedef struct GFNT_Strike {
  size_t index;        ///< Which strike of the face this is.
  uint32_t ppem_x;     ///< Horizontal pixels per em.
  uint32_t ppem_y;     ///< Vertical pixels per em.
  uint8_t bit_depth;   ///< 1, 2, 4, 8, or 32 for colour.
  GFNT_GlyphKind kind; ///< What glyphs from this strike come back as.
} GFNT_Strike;

/**
 * @brief Whether the face carries outline glyph data this library reads.
 *
 * `glyf` with `loca`, or `CFF `. A face whose outlines are in a format section
 * 16 defers - `CFF2` - reports false, because what a caller needs to know is
 * whether asking for an outline can succeed.
 *
 * @param face The face, or NULL.
 * @return true if outlines are there to be read.
 */
GFNT_API bool gfnt_face_has_outlines(const GFNT_Face * face);

/**
 * @brief How many strikes the face carries.
 *
 * @param face The face.
 * @param out_count Receives the count. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK with a count of 0 for a face with no bitmap tables at all,
 *   ::GFNT_ERR_INVALID, or ::GFNT_ERR_UNSUPPORTED for a face whose strikes are
 *   in a table this library does not parse yet - which is not the same answer
 *   as zero, and must not be.
 */
GFNT_API GFNT_Result gfnt_face_strike_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/**
 * @brief The nth strike.
 *
 * @param face The face.
 * @param index Which one, from 0.
 * @param out_strike Receives it. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID for an index the face does not have,
 *   or ::GFNT_ERR_UNSUPPORTED.
 */
GFNT_API GFNT_Result gfnt_face_strike_at(const GFNT_Face * face, size_t index,
    GFNT_Strike * out_strike, GFNT_Error * error);

/**
 * @brief Decide what answers a pixel size: a strike, or the outlines.
 *
 * Every size-dependent call reports which strike answered, or that the answer
 * came from outlines (design.md section 5.3), because a caller that cannot find
 * out why 14-pixel text looks different from 12-pixel text has a bug report it
 * cannot act on.
 *
 * @param face The face.
 * @param ppem The pixel size wanted. Zero is a caller error.
 * @param policy Which way to resolve it; zero ignores strikes.
 * @param out_strike Receives the strike that answered, when one did. May be
 *   NULL.
 * @param out_from_outlines Receives true when the answer is "scale the
 *   outlines". May be NULL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, or ::GFNT_ERR_UNSUPPORTED when
 *   nothing in the face can answer at that size.
 */
GFNT_API GFNT_Result gfnt_face_select_strike(const GFNT_Face * face,
    uint32_t ppem, GFNT_StrikePolicy policy, GFNT_Strike * out_strike,
    bool * out_from_outlines, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_GLYPH_H
