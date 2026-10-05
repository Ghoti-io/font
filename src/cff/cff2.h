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
 * The `CFF2` container: a Compact Font Format with the parts a variable OpenType
 * font does not need removed and a variation store added.
 *
 * documentation/design.md section 7.7. Internal, and free of `outline.h` for the
 * reason `cff.h` is: this describes bytes, and what draws a glyph is
 * `cff_glyph.c`. It reuses `cff.h`'s INDEX and DICT readers and differs from a
 * CFF in these ways, each of which a reader that treated the table as a CFF would
 * get wrong without an error:
 *
 *   * The header is five bytes - `major`, `minor`, `headerSize` and a two-byte
 *     `topDictLength` - and the Top DICT *is* the next `topDictLength` bytes, not
 *     an element of an INDEX. There is no Name INDEX, no String INDEX, no
 *     charset and no encoding: the glyph names are `post`'s, and the glyph order
 *     is the font's.
 *   * Every INDEX counts its elements in four bytes.
 *   * Every glyph has a Font DICT, so `FDArray` is required, and `FDSelect` -
 *     formats 0, 3 and **4**, which CFF does not have - is what chooses between
 *     them; without it every glyph is in the first.
 *   * A Private DICT has no widths. A CFF2 charstring states none, and the
 *     advance is `hmtx`'s.
 *   * `vstore` is an item variation store behind a two-byte length. Its regions
 *     are what a `blend` scales its deltas by, and a Private DICT's `vsindex`
 *     chooses which of the store's ItemVariationData a glyph's blends use unless
 *     the charstring says otherwise.
 *
 * Everything is an offset from the start of the table, never a pointer, because
 * ::GFNT_Cff2 is memoised into the face by value.
 *
 * Reference: OpenType Specification 1.9, "CFF2 - Compact Font Format
 * (CFF) Version 2".
 */

#ifndef GHOTI_IO_GFNT_CFF2_H
#define GHOTI_IO_GFNT_CFF2_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cff.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes in the fixed part of a CFF2 header. */
#define GFNT_CFF2_HEADER_BYTES 5u

/** One CFF2 font's structure. Memoised per face, pointer-free. */
typedef struct GFNT_Cff2 {
  size_t length;               ///< The table's own length.
  uint8_t minor;               ///< Format minor version, not acted on.
  GFNT_CffIndex gsubrs;        ///< Global subroutines; count 0 when there are none.
  GFNT_CffIndex charstrings;   ///< One element per glyph.
  GFNT_CffIndex fdarray;       ///< The Font DICTs; at least one.
  size_t fdselect;             ///< Glyph to Font DICT; 0 when every glyph is in the first.
  size_t vstore;               ///< The item variation store itself; 0 when there is none.
  size_t vstore_length;        ///< Its length in bytes.
  GFNT_F16Dot16 font_matrix[6];///< `FontMatrix`, 16.16, as ::GFNT_Cff has it.
  bool font_matrix_stated;     ///< Whether the Top DICT carried one.
} GFNT_Cff2;

/**
 * What one glyph's Font DICT and Private DICT say that the interpreter needs.
 */
typedef struct GFNT_Cff2Font {
  GFNT_CffIndex subrs;         ///< Local subroutines; count 0 when there are none.
  uint32_t vsindex;            ///< Which ItemVariationData blends use by default.
} GFNT_Cff2Font;

/**
 * Parse the face's `CFF2` table, for ::gfnt_table_cached().
 *
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED for a major version other than 2;
 *   ::GFNT_ERR_CORRUPT; or ::GFNT_ERR_LIMIT.
 */
GFNT_Result gfnt_cff2_parse(const GFNT_Face * face, void * out, void * context,
    GFNT_Error * error);

/** The face's parsed CFF2, parsing it on first use. */
GFNT_Result gfnt_face_cff2(const GFNT_Face * face, const GFNT_Cff2 ** out_cff2,
    GFNT_Error * error);

/**
 * The Font DICT's Private DICT that governs one glyph, as far as a charstring
 * needs it.
 *
 * Parsed per call, like ::gfnt_cff_private_for_glyph(): a Private DICT is a few
 * dozen bytes and there may be 255 of them.
 *
 * @param face The face.
 * @param cff2 Its parsed CFF2.
 * @param glyph The glyph index.
 * @param out_font Receives the subroutines and the default `vsindex`.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_CORRUPT or ::GFNT_ERR_UNSUPPORTED.
 */
GFNT_Result gfnt_cff2_font_for_glyph(const GFNT_Face * face,
    const GFNT_Cff2 * cff2, uint32_t glyph, GFNT_Cff2Font * out_font,
    GFNT_Error * error);

/**
 * How many glyphs the `CharStrings` INDEX implies, for the numGlyphs minimum (M12).
 *
 * @return true when the face has a readable `CFF2` whose count this is.
 */
bool gfnt_cff2_glyph_bound(const GFNT_Face * face, size_t * out_bound);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_CFF2_H
