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
 * `HVAR` and `MVAR`: how a glyph's horizontal metrics, and a font's line metrics,
 * change at a location in the design space.
 *
 * documentation/design.md section 7.7. Both keep their deltas in an item
 * variation store (`ivs.h`); `HVAR` finds a glyph's row through delta-set index
 * maps, and `MVAR` through a list of four-letter tags, one per metric.
 *
 * What is returned is a **delta** and is never the metric itself: adding it to
 * the default's value, and rounding, is the caller's, which is what lets a
 * caller without the metric's table (a face with no `hmtx`) not be asked for one.
 *
 * There is no `VVAR`: this library reads no vertical metrics (`vhea`, `vmtx`), so
 * there is nothing for it to vary.
 *
 * Reference: OpenType Specification 1.9, "HVAR - Horizontal Metrics Variations
 * Table" and "MVAR - Metrics Variations Table".
 */

#ifndef GHOTI_IO_GFNT_METVAR_H
#define GHOTI_IO_GFNT_METVAR_H

#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief `HVAR`. */
#define GFNT_TAG_HVAR GFNT_TAG('H', 'V', 'A', 'R')
/** @brief `MVAR`. */
#define GFNT_TAG_MVAR GFNT_TAG('M', 'V', 'A', 'R')
/** @brief `VVAR`. */
#define GFNT_TAG_VVAR GFNT_TAG('V', 'V', 'A', 'R')

/** @brief Which of `HVAR`'s three mappings a lookup is for. */
typedef enum GFNT_HvarField {
  GFNT_HVAR_ADVANCE,            ///< The advance width.
  GFNT_HVAR_LEFT_BEARING,       ///< The left side bearing.
} GFNT_HvarField;

/**
 * One glyph's delta from `HVAR`.
 *
 * @param face The face; it must have `HVAR`.
 * @param glyph The glyph.
 * @param field Which delta.
 * @param coordinates The location, normalised 2.14 in `fvar` order, or NULL.
 * @param coordinate_count How many were given.
 * @param out_delta Receives the delta in font units with ::GFNT_GVAR_FRACTION_BITS
 *   fractional bits. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED if the face has no `HVAR`, or the
 *   table states no mapping for the left side bearing - which is *not* a zero
 *   delta, since the bearing then follows the outline and not the table;
 *   ::GFNT_ERR_CORRUPT; ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_hvar_delta(const GFNT_Face * face, uint32_t glyph,
    GFNT_HvarField field, const GFNT_F2Dot14 * coordinates,
    size_t coordinate_count, int64_t * out_delta, GFNT_Error * error);

/** @brief Which of `VVAR`'s mappings a lookup is for. */
typedef enum GFNT_VvarField {
  GFNT_VVAR_ADVANCE,   ///< The advance height.
  GFNT_VVAR_ORIGIN,    ///< The vertical origin (`VORG`'s y).
  GFNT_VVAR_TSB,       ///< The top side bearing; no mapping means no delta.
} GFNT_VvarField;

/**
 * One glyph's delta from `VVAR`.
 *
 * The advance falls back to the glyph number as its row when the table has no
 * mapping for it; the origin with no mapping has **no delta** and succeeds with
 * zero, as HarfBuzz reads it.
 *
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED if the face has no `VVAR`;
 *   ::GFNT_ERR_CORRUPT; ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_vvar_delta(const GFNT_Face * face, uint32_t glyph,
    GFNT_VvarField field, const GFNT_F2Dot14 * coordinates,
    size_t coordinate_count, int64_t * out_delta, GFNT_Error * error);

/**
 * One metric's delta from `MVAR`.
 *
 * A face with no `MVAR`, or one that does not list @p tag, **has no change in that
 * metric**, which is the specification's statement and not a refusal: the delta
 * is zero and the call succeeds.
 *
 * @param face The face.
 * @param tag The metric's tag, such as `hasc`.
 * @param coordinates The location, normalised 2.14 in `fvar` order, or NULL.
 * @param coordinate_count How many were given.
 * @param out_delta Receives the delta as ::gfnt_hvar_delta() does.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_CORRUPT or ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_mvar_delta(const GFNT_Face * face, GFNT_Tag tag,
    const GFNT_F2Dot14 * coordinates, size_t coordinate_count,
    int64_t * out_delta, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_METVAR_H
