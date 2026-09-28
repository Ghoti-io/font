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
 * A glyph that is pixels rather than a path.
 *
 * documentation/design.md sections 5.4 and 7.5. `glyph.h` says what a glyph can
 * be and which strike answers a size; this is the ::GFNT_GLYPH_BITMAP_MONO arm's
 * data, and the accessor that returns it.
 *
 * Four standalone containers are read (section 7.1) - PCF, BDF, PSF 1 and 2, and
 * GNU Unifont's `.hex` - and each produces a face with **one strike and no
 * outlines**. The strikes inside an sfnt (`EBDT`/`EBLC`, `CBDT`, `sbix`) are not
 * read yet and report ::GFNT_ERR_UNSUPPORTED rather than "no strikes", which is
 * the distinction `glyph.h` exists to keep.
 *
 * Two things are normalised on the way out, and both are deliberate:
 *
 *  - **Rows run top to bottom and bits run most-significant-first**, one row
 *    starting on a byte boundary. Every one of these formats stores rows that
 *    way, but PCF also stores the *bits* either way round, in scan units of one,
 *    two or four bytes, with rows padded to one, two, four or eight - a
 *    cross-product a caller would have to implement to use the bytes at all. So
 *    the container does it once, where one differential can check it.
 *  - **Every measurement is in pixels**, and there is no em. A bitmap strike is
 *    not an outline scaled down: it was drawn at a size, and asking for its
 *    `unitsPerEm` has no answer (M9). ::gfnt_face_units_per_em() therefore
 *    refuses on a face like this rather than inventing 1000, and the numbers
 *    below are the pixels the font states.
 */

#ifndef GHOTI_IO_GFNT_BITMAP_H
#define GHOTI_IO_GFNT_BITMAP_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One glyph's pixels, and the metrics that position them.
 *
 * `bits` is borrowed from the face and lives as long as the face does; nothing
 * here needs freeing. A glyph with no pixels at all - a space - is `width` or
 * `height` of zero with a `bits` of NULL and an `advance` that still matters.
 *
 * The y convention is section 5.5's, which is the whole library's: y is up, the
 * origin is on the baseline at the pen, and so `bearing_y` is the **top** row's
 * upper edge above the baseline and is positive for a glyph that rises above it.
 * A descender's bottom row is at `bearing_y - height`, which is negative, and
 * that subtraction is the one place a caller can get this wrong.
 */
typedef struct GFNT_BitmapGlyph {
  uint32_t width;       ///< Pixels across. Zero for a glyph with no pixels.
  uint32_t height;      ///< Rows. Zero for a glyph with no pixels.
  size_t stride;        ///< Bytes per row: `(width + 7) / 8` for 1-bit.
  int32_t bearing_x;    ///< Left side bearing in pixels; may be negative.
  int32_t bearing_y;    ///< Top row's edge above the baseline, y-up.
  int32_t advance;      ///< Pen movement in pixels; may differ from `width`.
  uint8_t bit_depth;    ///< Bits per pixel. 1 for every container read today.
  /**
   * The rows, top first, MSB-first within each byte, or NULL for no pixels.
   *
   * Borrowed from the face. `height * stride` bytes; the padding bits at the
   * end of a row are zero, because a container that left them set would make two
   * faces of one design compare unequal.
   */
  const uint8_t * bits;
  GFNT_Strike strike;   ///< Which strike answered, as section 5.3 requires.
} GFNT_BitmapGlyph;

/**
 * @brief This glyph's pixels, from a strike.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param strike Which strike, from 0. A face with one strike takes 0.
 * @param out_glyph Receives the glyph. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for no face, a glyph past the count, or
 *   a strike the face does not have; ::GFNT_ERR_UNSUPPORTED for a face with no
 *   bitmaps this library reads - including one whose strikes are in an sfnt
 *   table, which is not the same answer and says so; ::GFNT_ERR_CORRUPT for a
 *   glyph whose bitmap the container gets wrong, which is per glyph (M11).
 */
GFNT_API GFNT_Result gfnt_face_glyph_bitmap(const GFNT_Face * face,
    uint32_t glyph, size_t strike, GFNT_BitmapGlyph * out_glyph,
    GFNT_Error * error);

/**
 * @brief One pixel of a bitmap glyph.
 *
 * Reading the rows directly is the fast path and this is the readable one; a
 * caller compositing a run should walk `bits` itself.
 *
 * @param glyph The glyph.
 * @param x Column, from the left.
 * @param y Row, from the **top**, which is how the rows are stored.
 * @return 0 or 255 for a 1-bit glyph, so that the value composites like
 *   coverage; 0 for a coordinate outside the bitmap, which is what a caller
 *   drawing a box around a glyph reads and is not an error.
 */
GFNT_API uint8_t gfnt_bitmap_pixel(const GFNT_BitmapGlyph * glyph, uint32_t x,
    uint32_t y);

/**
 * @brief Write a bitmap glyph as text, for the differentials and the golden gate.
 *
 * One header line, then one line per row with `#` for a set pixel and `.` for a
 * clear one. The same shape ::gfnt_coverage_dump_art() writes, so that a bitmap
 * strike and a rasterised outline can be read side by side.
 *
 * @param glyph The glyph.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_bitmap_dump(const GFNT_BitmapGlyph * glyph,
    FILE * out);

/**
 * @brief Which codepoints a bitmap container maps to a glyph, and how many.
 *
 * A standalone bitmap font has no `cmap`: PCF carries a `BDF_ENCODINGS` table,
 * BDF an `ENCODING` per character, PSF an optional Unicode table where one glyph
 * can answer several codepoints and a sequence, and `.hex` is keyed by codepoint
 * outright. ::gfnt_face_glyph_for_codepoint() answers from whichever of those the
 * face has, so a caller needs none of this; it is here for enumeration, which a
 * `cmap` offers and these formats otherwise would not.
 *
 * @param face The face.
 * @param out_count Receives how many (codepoint, glyph) pairs the face states.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID; ::GFNT_ERR_UNSUPPORTED for a face whose
 *   mapping is a `cmap`, which `cmap.h` enumerates, or for a bitmap font that
 *   states no mapping at all - PSF without its Unicode table, whose glyph
 *   indices are console positions and not characters.
 */
GFNT_API GFNT_Result gfnt_face_bitmap_encoding_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/**
 * @brief The nth (codepoint, glyph) pair of a bitmap container's encoding.
 *
 * Sorted by codepoint, so a caller can page through them or bisect. A glyph
 * named by several codepoints appears once per codepoint.
 *
 * @param face The face.
 * @param index Which pair, from 0.
 * @param out_codepoint Receives the codepoint, or NULL.
 * @param out_glyph Receives the glyph index, or NULL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID or ::GFNT_ERR_UNSUPPORTED.
 */
GFNT_API GFNT_Result gfnt_face_bitmap_encoding_at(const GFNT_Face * face,
    size_t index, uint32_t * out_codepoint, uint32_t * out_glyph,
    GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_BITMAP_H
