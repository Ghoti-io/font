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
 * `cmap`: which glyph a codepoint maps to.
 *
 * documentation/design.md section 7.2. Two things about this table are worth
 * saying before the API:
 *
 * - **Format 4 is the most implemented and most misimplemented subtable there
 *   is** (M10). Its `idRangeOffset` is a byte offset from the address of the
 *   `idRangeOffset` entry itself, which invites arithmetic in the wrong base
 *   and in sixteen bits; here it is computed in `size_t` from that entry's own
 *   offset and bounds-checked by the reader like every other read.
 * - **A codepoint no subtable maps is not an error.** The answer is glyph 0,
 *   `.notdef`, which every font has and which this library will draw
 *   (section 5.4, M14). A caller that wants to know whether a codepoint was
 *   mapped tests the glyph against zero.
 *
 * Lookups read the table on demand rather than building an index, so asking
 * one question of a 30 MB CJK font allocates nothing.
 */

#ifndef GHOTI_IO_GFNT_CMAP_H
#define GHOTI_IO_GFNT_CMAP_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Platform ID 0: Unicode. */
#define GFNT_PLATFORM_UNICODE 0u
/** @brief Platform ID 1: Macintosh. */
#define GFNT_PLATFORM_MACINTOSH 1u
/** @brief Platform ID 3: Windows. */
#define GFNT_PLATFORM_WINDOWS 3u

/**
 * @brief One `cmap` subtable: who it is for, what shape it is, and where.
 *
 * @p format is read from the subtable itself rather than guessed from the
 * encoding, because the two are independent and fonts pair them freely.
 */
typedef struct GFNT_CmapSubtable {
  uint16_t platform_id; ///< ::GFNT_PLATFORM_WINDOWS and its neighbours.
  uint16_t encoding_id; ///< Meaning depends on the platform.
  uint16_t format;      ///< 0, 2, 4, 6, 8, 10, 12, 13 or 14.
  size_t offset;        ///< From the start of the `cmap` table.
} GFNT_CmapSubtable;

/**
 * @brief How many subtables the `cmap` lists.
 *
 * @param face The face.
 * @param out_count Receives the count. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED if the font
 *   has no `cmap`, or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_cmap_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/**
 * @brief The nth subtable, in the order the font lists them.
 *
 * @param face The face.
 * @param index Which one, from 0.
 * @param out_subtable Receives it. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED or
 *   ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_cmap_at(const GFNT_Face * face, size_t index,
    GFNT_CmapSubtable * out_subtable, GFNT_Error * error);

/**
 * @brief The subtable this library will use for codepoint lookups.
 *
 * The preference order is `(3,10)`, `(0,6)`, `(0,4)`, `(3,1)`, `(0,3)`, then
 * the Windows symbol subtable `(3,0)`, then Macintosh `(1,0)` - full Unicode
 * first, then the Basic Multilingual Plane, then the two single-byte
 * encodings that are all some fonts have.
 *
 * **A subtable whose format this library cannot read is passed over**, and the
 * next preference is used instead. That is a choice: the alternative is to
 * select a format 13 subtable and then refuse every lookup, which leaves a
 * caller with a font that other implementations map fine. Which subtable
 * answered is returned here, so the fallback is auditable rather than silent
 * (M8).
 *
 * @param face The face.
 * @param out_subtable Receives it. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED if the font
 *   has no `cmap` or no subtable this library can read, or
 *   ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_cmap_best(const GFNT_Face * face,
    GFNT_CmapSubtable * out_subtable, GFNT_Error * error);

/**
 * @brief The glyph a codepoint maps to, through the preferred subtable.
 *
 * @param face The face.
 * @param codepoint The Unicode codepoint. Values a subtable cannot express are
 *   simply unmapped rather than an error.
 * @param out_glyph Receives the glyph index, or 0 when the codepoint is
 *   unmapped. **Not checked against the face's glyph count**: the mapping is
 *   what the font says, and a glyph index past the end is refused by the
 *   accessor that would load it, which is the one place that check belongs.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK (mapped or not), ::GFNT_ERR_INVALID,
 *   ::GFNT_ERR_UNSUPPORTED or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_glyph_for_codepoint(const GFNT_Face * face,
    uint32_t codepoint, uint32_t * out_glyph, GFNT_Error * error);

/** @brief What a variation sequence maps to. */
typedef enum GFNT_UvsKind {
  GFNT_UVS_NONE = 0, ///< The font has no entry for the pair.
  GFNT_UVS_DEFAULT,  ///< The base character's own glyph is the one to use.
  GFNT_UVS_GLYPH     ///< A glyph of its own, returned.
} GFNT_UvsKind;

/**
 * @brief What a base character and a variation selector map to, through the
 *   font's format 14 subtable.
 *
 * @param face The face.
 * @param base The base character.
 * @param selector The variation selector (U+FE00 to U+FE0F, U+E0100 to
 *   U+E01EF, or a Mongolian one).
 * @param out_glyph Receives the glyph when @p out_kind is ::GFNT_UVS_GLYPH.
 * @param out_kind Receives which of the three answers it is.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK (including when the font has no format 14 subtable, which
 *   is ::GFNT_UVS_NONE), ::GFNT_ERR_INVALID or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_variation_glyph(const GFNT_Face * face,
    uint32_t base, uint32_t selector, uint32_t * out_glyph,
    GFNT_UvsKind * out_kind, GFNT_Error * error);

/**
 * @brief The glyph a codepoint maps to through one named subtable.
 *
 * For a caller that wants a particular encoding - a PDF writer reading a
 * symbol font's own `(3,0)` map, a tool comparing two subtables of one font.
 *
 * @param face The face.
 * @param subtable A subtable from ::gfnt_face_cmap_at() or
 *   ::gfnt_face_cmap_best().
 * @param codepoint The codepoint, in whatever the subtable's encoding is.
 * @param out_glyph Receives the glyph index, or 0 when unmapped.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED for a subtable
 *   format this library does not read yet, or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_cmap_lookup(const GFNT_Face * face,
    const GFNT_CmapSubtable * subtable, uint32_t codepoint,
    uint32_t * out_glyph, GFNT_Error * error);

/**
 * @brief Write the `cmap`'s subtable list to a stream, one per line.
 *
 * @param face The face.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED,
 *   ::GFNT_ERR_CORRUPT or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_face_cmap_dump(const GFNT_Face * face, FILE * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_CMAP_H
