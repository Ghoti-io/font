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
 * A variable font's design space: its axes, its named instances, and the
 * mapping from a user's coordinates to the normalised ones every accessor takes.
 *
 * documentation/design.md section 7.7. `fvar` states the axes and the named
 * instances; `avar` bends the normalised scale so that a design's intended
 * weights sit where its designer put them. Neither moves an outline - `gvar`
 * does, and takes the normalised coordinates this file produces.
 *
 * **There are two coordinate systems and they are not interchangeable.**
 *
 *   * **User coordinates** are what a person types: weight 700, optical size 14.
 *     They are in the axis's own units, as 16.16, and `fvar` states each axis's
 *     range in them.
 *   * **Normalised coordinates** are ::GFNT_Variation's: -1 to 1 in 2.14, where 0
 *     is the axis's default. Every accessor that a variation can change takes
 *     these.
 *
 * ::gfnt_face_normalize() is the only way between them, and it is exposed
 * because getting it wrong moves every glyph of the font by a plausible amount
 * with no error anywhere: a caller who passes user coordinates where normalised
 * ones are expected asks for weight 700 and is clamped to the heaviest the font
 * has.
 *
 * Reference: OpenType Specification 1.9, "fvar - Font Variations Table",
 * "avar - Axis Variations Table", and "OpenType Font Variations Overview" for
 * the normalisation.
 */

#ifndef GHOTI_IO_GFNT_VARIATION_H
#define GHOTI_IO_GFNT_VARIATION_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief ::GFNT_Axis::flags bit 0: the axis is not to be shown to a user. */
#define GFNT_AXIS_HIDDEN 0x0001u

/**
 * @brief ::GFNT_NamedInstance::postscript_name_id when the record states none.
 *
 * `postScriptNameID` is an optional field, and 0xFFFF is the specification's own
 * "no name" value for it.
 */
#define GFNT_INSTANCE_NO_POSTSCRIPT_NAME 0xFFFFu

/**
 * @brief One axis of variation.
 *
 * Values are **user coordinates** in 16.16, exactly as `fvar` stores them, and
 * `min <= def <= max` always: a face whose axis says otherwise is refused as
 * ::GFNT_ERR_CORRUPT rather than normalised by a rule the font did not state.
 */
typedef struct GFNT_Axis {
  GFNT_Tag tag;            ///< `wght`, `wdth`, `opsz`, or a foundry's own.
  GFNT_F16Dot16 min;       ///< The least value the axis takes.
  GFNT_F16Dot16 def;       ///< The value the default outlines are drawn at.
  GFNT_F16Dot16 max;       ///< The most.
  uint16_t flags;          ///< ::GFNT_AXIS_HIDDEN, and bits this library ignores.
  uint16_t name_id;        ///< A `name` ID for the axis, as `fvar` states it.
} GFNT_Axis;

/**
 * @brief One named instance: a point in the design space with a name.
 *
 * `coordinates` is **borrowed from the face** and valid until it is freed. It
 * holds ::gfnt_face_axis_count() user coordinates in `fvar`'s axis order.
 */
typedef struct GFNT_NamedInstance {
  const GFNT_F16Dot16 * coordinates; ///< One per axis, user coordinates.
  size_t coordinate_count;           ///< The face's axis count.
  uint16_t name_id;                  ///< The instance's `subfamilyNameID`.
  uint16_t flags;                    ///< Reserved by the format; reported as stored.
  /** A `name` ID, or ::GFNT_INSTANCE_NO_POSTSCRIPT_NAME. */
  uint16_t postscript_name_id;
} GFNT_NamedInstance;

/**
 * @brief Whether the face carries an `fvar`.
 *
 * Says nothing about whether this library can *move* its outlines: a `glyf` face
 * moves through `gvar`, a CFF2 face through its blend operators, and a face with
 * an `fvar` and neither has a design space and outlines that do not move in it, so
 * a variation is refused for it by name.
 *
 * @param face The face, or NULL.
 * @return true if the table directory has an `fvar`.
 */
GFNT_API bool gfnt_face_is_variable(const GFNT_Face * face);

/**
 * @brief How many axes the face varies along.
 *
 * @param face The face.
 * @param out_count Receives the count; 0 for a face with no `fvar`. Written
 *   only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED for an `fvar`
 *   version this library does not read, ::GFNT_ERR_LIMIT past
 *   ::GFNT_Limits::max_axes, or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_axis_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/**
 * @brief The nth axis, in `fvar` order.
 *
 * @param face The face.
 * @param index Which one, from 0. This is also the index of its coordinate in a
 *   ::GFNT_Variation.
 * @param out_axis Receives it. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID for an index the face does not have, or
 *   what reading the table returned.
 */
GFNT_API GFNT_Result gfnt_face_axis_at(const GFNT_Face * face, size_t index,
    GFNT_Axis * out_axis, GFNT_Error * error);

/**
 * @brief How many named instances the face has.
 *
 * @param face The face.
 * @param out_count Receives the count; 0 for a face with no `fvar`. Written
 *   only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return As ::gfnt_face_axis_count().
 */
GFNT_API GFNT_Result gfnt_face_instance_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/**
 * @brief The nth named instance.
 *
 * @param face The face.
 * @param index Which one, from 0.
 * @param out_instance Receives it. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID for an index the face does not have, or
 *   what reading the table returned.
 */
GFNT_API GFNT_Result gfnt_face_instance_at(const GFNT_Face * face, size_t index,
    GFNT_NamedInstance * out_instance, GFNT_Error * error);

/**
 * @brief User coordinates to the normalised ones a ::GFNT_Variation carries.
 *
 * Each coordinate is clamped to its axis's range, mapped to -1..1 around the
 * axis default - linearly on either side of it, which is why the two halves of
 * an axis can have different slopes - and then bent by the axis's `avar` segment
 * map when the face has one.
 *
 * The arithmetic is 16.16 with half-away-from-zero rounding at each division,
 * and the result is rounded to 2.14. That is FreeType's arithmetic, deliberately:
 * a rounding rule the specification leaves open and every other reader agrees on
 * is the one to share, because two readers that differ in the last bit of a
 * coordinate differ in the last bit of every point it moves.
 *
 * @param face The face.
 * @param user_coordinates One 16.16 value per axis, in `fvar` order, or NULL when
 *   @p count is 0.
 * @param count How many were given, at most the axis count. **Axes past @p count
 *   take their default**, so a caller that sets only weight on a two-axis face
 *   leaves the other where the designer put it.
 * @param out_coordinates Receives one 2.14 value per axis - the axis count, not
 *   @p count - so that the array is ready to be a ::GFNT_Variation. Written only
 *   on success.
 * @param capacity How many values @p out_coordinates holds. At least the axis
 *   count is needed.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID for more coordinates than axes or too
 *   small an output, ::GFNT_ERR_UNSUPPORTED for a face with no `fvar` or an
 *   `avar` version 2, or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_normalize(const GFNT_Face * face,
    const GFNT_F16Dot16 * user_coordinates, size_t count,
    GFNT_F2Dot14 * out_coordinates, size_t capacity, GFNT_Error * error);

/**
 * @brief Print a face's `fvar`, and its `avar` when it has one.
 *
 * One line per axis, per instance and per segment map, in the table's own
 * integers - 16.16 for user coordinates and 2.14 for the map - so that the
 * oracle can compare them to the reference without a conversion that is a
 * second place to be wrong. `ttx_diff.py` is what reads this.
 *
 * @param face The face.
 * @param out Where to print.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_IO, or what reading the
 *   table returned - which for a face with no `fvar` is
 *   ::GFNT_ERR_UNSUPPORTED, and nothing is printed.
 */
GFNT_API GFNT_Result gfnt_face_variation_dump(const GFNT_Face * face, FILE * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_VARIATION_H
