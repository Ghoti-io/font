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
 * `loca` and `glyf`: where one glyph's bytes are, and what they draw.
 *
 * documentation/design.md section 7.3. Internal because the public question is
 * "what is this glyph's outline" (`outline.h`), not "where in `glyf` does it
 * live"; a caller who wants the bytes has `gfnt_face_table_range()`.
 */

#ifndef GHOTI_IO_GFNT_GLYF_H
#define GHOTI_IO_GFNT_GLYF_H

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/outline.h>
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Where glyph @p glyph's description is within `glyf`, per `loca`.
 *
 * A length of zero means the glyph draws nothing, which every font uses for
 * `space` and which is not an error (M11's other half: an empty glyph and a
 * broken one must not be one answer).
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param out_offset Receives the offset from the start of `glyf`.
 * @param out_length Receives how many bytes the description is.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a glyph past `numGlyphs`;
 *   ::GFNT_ERR_UNSUPPORTED for a face with no `glyf`/`loca`; or
 *   ::GFNT_ERR_CORRUPT when the entry runs backwards or past `glyf`, which
 *   is a fact about *that glyph* and not about the font (M11).
 */
GFNT_Result gfnt_loca_range(const GFNT_Face * face, uint32_t glyph,
    size_t * out_offset, size_t * out_length, GFNT_Error * error);

/**
 * Load one glyph's contours into @p outline, resolving composites.
 *
 * A composite that reaches itself, directly or through another glyph, is
 * ::GFNT_ERR_CORRUPT named as a cycle rather than left to exhaust the depth
 * budget; ::GFNT_ERR_LIMIT is then what an *acyclic* chain nested deeper than
 * ::GFNT_Limits::max_composite_depth gets, which a caller can act on. `ebdt.c`
 * answers the same two ways for the same question, and src/core/chain.h is the
 * walk both use. There is no depth parameter because every call from outside is
 * the top of a walk: see the note on ::gfnt_glyf_load() itself.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param outline Receives the contours, appended to whatever is there.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED,
 *   ::GFNT_ERR_CORRUPT, ::GFNT_ERR_LIMIT or ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_glyf_load(const GFNT_Face * face, uint32_t glyph,
    GFNT_Outline * outline, GFNT_Error * error);

/**
 * Whether this glyph's `glyf` description is a composite.
 *
 * The `glyf` half of ::gfnt_face_glyph_is_composite(); `outline/producer.c`
 * decides which producer answers.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param out_composite Receives true for a negative contour count.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED or
 *   ::GFNT_ERR_CORRUPT.
 */
GFNT_Result gfnt_glyf_is_composite(const GFNT_Face * face, uint32_t glyph,
    bool * out_composite, GFNT_Error * error);

/**
 * The bounding box this glyph's `glyf` entry states.
 *
 * The `glyf` half of ::gfnt_face_glyph_stated_box().
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param out_box Receives the box, empty for a glyph with no description.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED or
 *   ::GFNT_ERR_CORRUPT.
 */
GFNT_Result gfnt_glyf_stated_box(const GFNT_Face * face, uint32_t glyph,
    GFNT_Box * out_box, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_GLYF_H
