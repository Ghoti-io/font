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
 * `cvt ` and `cvar`: the control values a TrueType program reads, and how they
 * change at a location in the design space.
 *
 * documentation/design.md section 7.7. A TrueType font's hinting program refers
 * to a table of numbers in font units - stem widths, alignment heights - and a
 * variable font's `cvar` moves them with the same kind of tuples `gvar` moves
 * points with. This library has **no hinting interpreter**, so nothing here is
 * used to draw anything; it is the data, read exactly, for a caller that has an
 * interpreter of its own or wants to inspect the font.
 *
 * Values are whole font units. A control value at a location is the default plus
 * the sum of every applicable tuple's delta, scaled and rounded **once**, a tie going
 * as ::GFNT_Variation.delta_rounding says (half up by default).
 *
 * Reference: OpenType Specification 1.9, "cvt - Control Value Table" and "cvar -
 * CVT Variations Table".
 */

#ifndef GHOTI_IO_GFNT_CVT_H
#define GHOTI_IO_GFNT_CVT_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How many control values the face has.
 *
 * @param face The face.
 * @param out_count Receives the count, **0 for a face with no `cvt `**. Written
 *   only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK or ::GFNT_ERR_INVALID.
 */
GFNT_API GFNT_Result gfnt_face_cvt_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/**
 * @brief Every control value, at a location.
 *
 * With no variation, or one that moves nothing, these are the `cvt ` table's own
 * values and `cvar` is not read. A face with no `cvar` has values that do not vary,
 * which is an answer and not a refusal.
 *
 * @param face The face.
 * @param variation The location (normalised 2.14), or NULL for the default.
 * @param out_values Receives ::gfnt_face_cvt_count() values in font units. Written
 *   only on success.
 * @param capacity How many @p out_values holds; at least the count is needed.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for no face, a variation with more
 *   coordinates than axes, or too small an output; ::GFNT_ERR_UNSUPPORTED for a
 *   `cvar` version other than 1.x; ::GFNT_ERR_CORRUPT for data that contradicts
 *   itself; ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_face_cvt_values(const GFNT_Face * face,
    const GFNT_Variation * variation, int32_t * out_values, size_t capacity,
    GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_CVT_H
