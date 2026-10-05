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
 * `STAT`: what a place in the design space is called.
 *
 * documentation/design.md section 7.7. `fvar` says where a named instance is;
 * `STAT` says what each *value* on each axis is called - "Bold" for weight 700,
 * "Condensed" for width 75 - which is how a font menu names a location that is not
 * one of `fvar`'s instances, and how a location is named at all in a font with no
 * instances.
 *
 * The table is read in place at each call, so none of this allocates and a
 * returned name ID is a `name` ID for ::gfnt_face_name().
 *
 * **The axes here are `STAT`'s own**, indexed from the start of its design axis
 * list, which need not be `fvar`'s order, and may include axes `fvar` lacks (a
 * static font's italic axis). ::gfnt_face_stat_match() is where the two are joined,
 * by tag.
 *
 * Reference: OpenType Specification 1.9, "STAT - Style Attributes Table".
 */

#ifndef GHOTI_IO_GFNT_STAT_H
#define GHOTI_IO_GFNT_STAT_H

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

/** @brief ::GFNT_StatValue::flags bit 0: a font of the same family is older. */
#define GFNT_STAT_OLDER_SIBLING 0x0001u
/** @brief ::GFNT_StatValue::flags bit 1: the name may be left out of a full name. */
#define GFNT_STAT_ELIDABLE 0x0002u

/** @brief One design axis of `STAT`. */
typedef struct GFNT_StatAxis {
  GFNT_Tag tag;            ///< `wght`, `wdth`, `ital`, or a foundry's own.
  uint16_t name_id;        ///< The axis's name, a `name` ID.
  uint16_t ordering;       ///< Where a name for this axis goes in a full name.
} GFNT_StatAxis;

/** @brief The four shapes an axis value takes. */
typedef enum GFNT_StatFormat {
  GFNT_STAT_VALUE = 1,     ///< One value on one axis.
  GFNT_STAT_RANGE = 2,     ///< A nominal value, standing for a range.
  GFNT_STAT_LINKED = 3,    ///< One value, linked to another (Regular to Bold).
  GFNT_STAT_MULTI = 4,     ///< A value on each of several axes at once.
} GFNT_StatFormat;

/**
 * @brief One axis value: a name, and the place it names.
 *
 * Values are 16.16 user coordinates. Which fields mean anything depends on
 * ::GFNT_StatValue::format, and the others are zero:
 *
 *   * ::GFNT_STAT_VALUE - `axis_index` and `value`.
 *   * ::GFNT_STAT_RANGE - `axis_index`, `value` (the nominal), `range_min`,
 *     `range_max`.
 *   * ::GFNT_STAT_LINKED - `axis_index`, `value` and `linked_value`.
 *   * ::GFNT_STAT_MULTI - `pair_count`, with the pairs from
 *     ::gfnt_face_stat_value_pair().
 */
typedef struct GFNT_StatValue {
  GFNT_StatFormat format;
  uint16_t flags;          ///< ::GFNT_STAT_OLDER_SIBLING and ::GFNT_STAT_ELIDABLE.
  uint16_t name_id;        ///< What the value is called, a `name` ID.
  uint16_t axis_index;     ///< Into the design axes; not for ::GFNT_STAT_MULTI.
  GFNT_F16Dot16 value;
  GFNT_F16Dot16 range_min;
  GFNT_F16Dot16 range_max;
  GFNT_F16Dot16 linked_value;
  size_t pair_count;       ///< ::GFNT_STAT_MULTI only.
} GFNT_StatValue;

/**
 * @brief Whether the face has a `STAT`.
 *
 * @param face The face, or NULL.
 * @return true if the table directory has one.
 */
GFNT_API bool gfnt_face_has_stat(const GFNT_Face * face);

/**
 * @brief How many design axes `STAT` lists.
 *
 * @param face The face.
 * @param out_count Receives it. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED for a face with no
 *   `STAT` or a version other than 1.x, or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_stat_axis_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/** @brief The nth design axis. Results as ::gfnt_face_stat_axis_count(). */
GFNT_API GFNT_Result gfnt_face_stat_axis_at(const GFNT_Face * face, size_t index,
    GFNT_StatAxis * out_axis, GFNT_Error * error);

/** @brief How many axis values `STAT` has. Results as ::gfnt_face_stat_axis_count(). */
GFNT_API GFNT_Result gfnt_face_stat_value_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/**
 * @brief The nth axis value.
 *
 * ::GFNT_ERR_CORRUPT for a value whose format is not 1 to 4, one that runs past
 * the table, or one that names a design axis `STAT` does not list.
 */
GFNT_API GFNT_Result gfnt_face_stat_value_at(const GFNT_Face * face, size_t index,
    GFNT_StatValue * out_value, GFNT_Error * error);

/**
 * @brief One (axis, value) pair of a ::GFNT_STAT_MULTI value.
 *
 * @param face The face.
 * @param value_index Which axis value, from 0.
 * @param pair_index Which of its pairs, below ::GFNT_StatValue::pair_count.
 * @param out_axis_index Receives the design axis. Written only on success.
 * @param out_value Receives the 16.16 coordinate. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID for an index it does not have or a value
 *   that is not format 4, or what reading the table returned.
 */
GFNT_API GFNT_Result gfnt_face_stat_value_pair(const GFNT_Face * face,
    size_t value_index, size_t pair_index, uint16_t * out_axis_index,
    GFNT_F16Dot16 * out_value, GFNT_Error * error);

/**
 * @brief The `name` ID for a name that applies when every value is elided.
 *
 * @param face The face.
 * @param out_name_id Receives it. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED for a face with no `STAT` or a table of
 *   version 1.0, which has no such field; or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_stat_elided_fallback(const GFNT_Face * face,
    uint16_t * out_name_id, GFNT_Error * error);

/**
 * @brief Which axis values name a location.
 *
 * A location is **user** coordinates, as for ::gfnt_face_normalize(), and axes past
 * @p count take the face's default. `STAT`'s axes are joined to `fvar`'s by tag;
 * a `STAT` axis the face has no `fvar` axis for is not matched, because there is
 * no coordinate to match it against. A value matches when
 *
 *   * ::GFNT_STAT_VALUE and ::GFNT_STAT_LINKED: its axis is at exactly `value`;
 *   * ::GFNT_STAT_RANGE: its axis is within `range_min` to `range_max`, both ends
 *     included;
 *   * ::GFNT_STAT_MULTI: **every** pair's axis is at exactly its value.
 *
 * Indices come back in table order, which is the specification's order for
 * choosing among them; ordering the names for display is `STAT`'s axis
 * `ordering`, and is the caller's.
 *
 * @param face The face.
 * @param user_coordinates One 16.16 value per axis, or NULL when @p count is 0.
 * @param count How many were given, at most the `fvar` axis count.
 * @param out_values Receives the matching value indices, or NULL to count only.
 * @param capacity How many @p out_values holds.
 * @param out_count Receives how many matched, which may exceed @p capacity.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED for a face with no
 *   `STAT` or no `fvar`, or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_stat_match(const GFNT_Face * face,
    const GFNT_F16Dot16 * user_coordinates, size_t count, size_t * out_values,
    size_t capacity, size_t * out_count, GFNT_Error * error);

/**
 * @brief Print a face's `STAT`: one line per design axis and per axis value.
 *
 * @param face The face.
 * @param out The stream.
 * @return ::GFNT_OK, or what reading the table returned.
 */
GFNT_API GFNT_Result gfnt_face_stat_dump(const GFNT_Face * face, FILE * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_STAT_H
