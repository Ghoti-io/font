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
 * `gvar`: how far each point of a TrueType glyph moves at a location in the
 * design space.
 *
 * documentation/design.md section 7.7. The table stores, per glyph, a list of
 * **tuples**: a region of the design space (a peak, and optionally the start and
 * end of a ramp up to it) and the deltas to apply to some of the glyph's points
 * when the location is inside it, scaled by how far inside. Deltas of every
 * tuple that applies are summed.
 *
 * **This file computes deltas and moves nothing.** It is handed a glyph's points
 * as numbers and hands back one pair of deltas per point; `glyf.c` is what
 * adds them to an outline. The split is what keeps this a tier 0 file with no
 * knowledge of outlines, and what lets a second consumer - the advance widths,
 * which `gvar` also carries as four *phantom points* after the glyph's own - ask
 * for the same arithmetic without a second copy of it.
 *
 * Reference: OpenType Specification 1.9, "gvar - Glyph Variations Table" and "The
 * 'gvar' table - tuple variation stores" in the Font Variations Overview.
 */

#ifndef GHOTI_IO_GFNT_GVAR_H
#define GHOTI_IO_GFNT_GVAR_H

#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The tag this file reads: `gvar`. */
#define GFNT_TAG_GVAR GFNT_TAG('g', 'v', 'a', 'r')

/**
 * @brief How many fractional bits a delta is carried to while it is computed.
 *
 * Twenty-four, and not the sixteen of a `Fixed`, because a delta is a whole number
 * of font units that can run into the thousands and a scalar's rounding error is
 * multiplied by it: at sixteen bits a 3,000-unit delta is uncertain by a tenth of
 * a unit, which is six 64ths and more than the precision an outline is held to.
 * At twenty-four the same error is a four-hundredth of one. The widest the
 * arithmetic gets is 2^58 in an interpolation's product, which fits.
 */
#define GFNT_GVAR_FRACTION_BITS 24

/** @brief Bytes in the `gvar` header before its offset array. */
#define GFNT_GVAR_HEADER_BYTES 20u

/**
 * @brief How many points follow a glyph's own, whose deltas `gvar` also carries.
 *
 * The left and right side bearing points and the top and bottom ones. A point
 * number in a tuple counts them: a glyph of N points has numbers 0 to N + 3.
 */
#define GFNT_GVAR_PHANTOM_POINTS 4u

/**
 * A parsed `gvar` header: where everything is, and nothing read from past it.
 *
 * No allocation, so no release: the per-glyph data is read in place when a glyph
 * asks, which is what keeps loading a face with 30,000 glyphs from costing a
 * list of 30,000 ranges.
 */
typedef struct GFNT_Gvar {
  size_t axis_count;             ///< Equal to `fvar`'s, checked at parse.
  size_t shared_tuple_count;
  size_t shared_tuples_offset;   ///< From the start of the table.
  size_t glyph_count;            ///< What `gvar` says, which a glyph is checked against.
  bool long_offsets;             ///< Whether the offset array is 32-bit.
  size_t data_offset;            ///< Where per-glyph data begins, from the table.
} GFNT_Gvar;

/** The `gvar` parser, for ::gfnt_table_cached(). */
GFNT_Result gfnt_gvar_parse(const GFNT_Face * face, void * out, void * context,
    GFNT_Error * error);

/** The memoised `gvar`. */
GFNT_Result gfnt_face_gvar(const GFNT_Face * face, const GFNT_Gvar ** out_gvar,
    GFNT_Error * error);

/**
 * One glyph's points, as `gvar` needs to see them.
 *
 * `x` and `y` are the **unvaried** positions in font units and are consulted only
 * to interpolate: a point a tuple does not name takes a delta inferred from its
 * contour's neighbours that it *does* name (IUP), and that inference is a
 * function of where the points are. A composite glyph's "points" are its
 * components, which have no contours and infer nothing, so it passes none.
 */
typedef struct GFNT_GvarPoints {
  size_t count;                  ///< The glyph's own points, phantoms excluded.
  const int32_t * x;             ///< Unvaried x, font units; NULL for a composite.
  const int32_t * y;             ///< Unvaried y, font units; NULL for a composite.
  /**
   * One past the last point of each contour, in order, counting from 0 - or NULL
   * for a composite. The contours partition `0 .. count`.
   */
  const size_t * contour_ends;
  size_t contour_count;
} GFNT_GvarPoints;

/**
 * Every delta one glyph has at one location.
 *
 * @param face The face, whose `gvar` is consulted and whose allocator is used.
 * @param glyph The glyph.
 * @param coordinates The location: normalised 2.14 coordinates in `fvar` order,
 *   or NULL for all zeros. Axes past @p coordinate_count are zero.
 * @param coordinate_count How many were given.
 * @param points The glyph's points.
 * @param out_x Receives `points->count + ::GFNT_GVAR_PHANTOM_POINTS` deltas in font
 *   units with ::GFNT_GVAR_FRACTION_BITS fractional bits. **Written in full on
 *   success**: a point no tuple moves is zero rather than left alone.
 * @param out_y Likewise.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_CORRUPT for data that contradicts itself, which
 *   condemns **this glyph** and not the table (M11); ::GFNT_ERR_OOM; or what
 *   reading the table returned.
 */
GFNT_Result gfnt_gvar_glyph_deltas(const GFNT_Face * face, uint32_t glyph,
    const GFNT_F2Dot14 * coordinates, size_t coordinate_count,
    const GFNT_GvarPoints * points, int64_t * out_x, int64_t * out_y,
    GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_GVAR_H
