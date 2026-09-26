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
 * What a `GFNT_Outline` is made of, and the operations only a producer needs.
 *
 * The public header hands out an opaque pointer with accessors, the way
 * `GFNT_Face` does, so that the point arrays can change shape without
 * breaking a consumer. The producers - `glyf` today, charstrings in phase 2 -
 * and the rasteriser read the arrays directly through this header, because a
 * per-point accessor call in the rasteriser's inner loop is not a cost worth
 * paying for encapsulation inside one library.
 */

#ifndef GHOTI_IO_GFNT_OUTLINE_INTERNAL_H
#define GHOTI_IO_GFNT_OUTLINE_INTERNAL_H

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/outline.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A glyph's contours.
 *
 * `points` and `tags` are parallel and `point_count` long; `contours` holds
 * one **end index, exclusive** per contour, which is `glyf`'s
 * `endPtsOfContours` plus one. Exclusive ends make an empty contour
 * representable and make the length of contour `i` a subtraction rather than a
 * special case at index zero.
 *
 * `limits` is a copy of whatever the producer was working under, so that
 * ::gfnt_outline_add_point() enforces the same caps whether the points came
 * from a font or from a caller building its own path.
 */
struct GFNT_Outline {
  const GFNT_Allocator * allocator; ///< Where the arrays came from.
  GFNT_Limits limits;               ///< Caps on points and contours.
  GFNT_OutlineSpace space;          ///< Units or pixels.

  GFNT_Point * points;              ///< point_count of them.
  uint8_t * tags;                   ///< One ::GFNT_PointTag per point.
  size_t point_count;               ///< Points in use.
  size_t point_capacity;            ///< Points allocated.

  size_t * contours;                ///< Exclusive end index per contour.
  size_t contour_count;             ///< Contours in use.
  size_t contour_capacity;          ///< Contours allocated.
};

/**
 * Make room for at least @p points more points and @p contours more contours.
 *
 * Growth is doubling with a floor, and the limits are checked against the
 * *requested* total rather than the rounded-up capacity: a cap on points is a
 * promise about the font, not about this function's arithmetic.
 *
 * @param outline The outline.
 * @param points How many more points will be added.
 * @param contours How many more contours will be added.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_LIMIT or ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_outline_reserve(GFNT_Outline * outline, size_t points,
    size_t contours, GFNT_Error * error);

/**
 * Set the limits a further ::gfnt_outline_add_point() is checked against.
 *
 * @param outline The outline.
 * @param limits The caps, or NULL for the defaults.
 */
void gfnt_outline_set_limits(GFNT_Outline * outline,
    const GFNT_Limits * limits);

/**
 * Append every contour of @p source to @p dest, transformed.
 *
 * How a composite glyph is assembled: each component is loaded into its own
 * outline and appended through here with the component's matrix and offset.
 * The transform is applied on the way in rather than to @p source, so that a
 * component outline can be reused for a second reference to the same glyph.
 *
 * @param dest Receives the contours.
 * @param source The contours to copy. An empty source is a no-op.
 * @param xx Row 0 column 0, 16.16.
 * @param xy Row 0 column 1, 16.16.
 * @param yx Row 1 column 0, 16.16.
 * @param yy Row 1 column 1, 16.16.
 * @param dx Added to every x afterwards, 26.6.
 * @param dy Added to every y afterwards, 26.6.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_LIMIT or ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_outline_append_transformed(GFNT_Outline * dest,
    const GFNT_Outline * source, GFNT_F16Dot16 xx, GFNT_F16Dot16 xy,
    GFNT_F16Dot16 yx, GFNT_F16Dot16 yy, GFNT_F26Dot6 dx, GFNT_F26Dot6 dy,
    GFNT_Error * error);

/**
 * Apply a 16.16 matrix to a 26.6 point.
 *
 * Shared by the transform, the append and the composite reader so that all
 * three round identically; a component placed by one rule and measured by
 * another is a half-pixel seam.
 *
 * @param point The point.
 * @param xx Row 0 column 0, 16.16.
 * @param xy Row 0 column 1, 16.16.
 * @param yx Row 1 column 0, 16.16.
 * @param yy Row 1 column 1, 16.16.
 * @return The transformed point, 26.6.
 */
GFNT_Point gfnt_outline_apply_matrix(GFNT_Point point, GFNT_F16Dot16 xx,
    GFNT_F16Dot16 xy, GFNT_F16Dot16 yx, GFNT_F16Dot16 yy);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_OUTLINE_INTERNAL_H
