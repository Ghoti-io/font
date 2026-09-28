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
 * The CFF container: INDEXes, DICTs, the charset, the encoding, and where one
 * glyph's charstring is.
 *
 * documentation/design.md section 7.4. Internal, and deliberately **free of
 * `outline.h`**: this header describes bytes, and `src/sfnt/sfnt.h` includes it
 * to give a face its memo. What turns a charstring into a path is
 * `charstring.h` and `cff_glyph.c`, which are tier 1; this is not.
 *
 * Everything here is an offset from the start of the `CFF ` table, never a
 * pointer, because ::GFNT_Cff is memoised into the face by value and a face
 * that owned pointers would have to free them.
 *
 * Reference: Adobe *The Compact Font Format Specification*, version 1.0
 * (technical note #5176), and *The Type 2 Charstring Format* (#5177) for the
 * Private DICT entries the interpreter reads.
 */

#ifndef GHOTI_IO_GFNT_CFF_H
#define GHOTI_IO_GFNT_CFF_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../reader/reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The tag of the table holding a CFF font. */
#define GFNT_TAG_CFF GFNT_TAG('C', 'F', 'F', ' ')
/** The tag of the CFF2 table, which this library reads no glyph from. */
#define GFNT_TAG_CFF2 GFNT_TAG('C', 'F', 'F', '2')

/**
 * The most operands a DICT operator may be given.
 *
 * The specification's own limit, and the reason it is a constant here rather
 * than a growable array: an operator with more operands than this is a corrupt
 * DICT, so there is nothing to grow for.
 */
#define GFNT_CFF_DICT_MAX_OPERANDS 48

/**
 * Where an INDEX's elements are.
 *
 * An INDEX is a count, an offset size, `count + 1` offsets and then the data.
 * The offsets are **one-based from the byte before the data**, which is why
 * `data_base` is stored rather than the first element's position: element `i`
 * runs from `data_base + offset[i]` to `data_base + offset[i + 1]`.
 *
 * A count of zero is a two-byte INDEX with no offsets at all, which is legal
 * and which a font with no local subroutines uses.
 */
typedef struct GFNT_CffIndex {
  uint32_t count;    ///< Elements.
  uint8_t off_size;  ///< Bytes per offset, 1-4. Meaningless when empty.
  size_t offsets;    ///< Where the offset array starts, from the table's base.
  size_t data_base;  ///< What element offsets are relative to.
  size_t end;        ///< One past the INDEX's last byte.
} GFNT_CffIndex;

/**
 * What a Private DICT says that the interpreter needs.
 *
 * Two of the three are the width rules: a charstring states its advance as a
 * delta from `nominalWidthX`, and one that states nothing means
 * `defaultWidthX`. Getting that wrong gives every unstated glyph the same
 * wrong advance, which no outline comparison can see.
 */
typedef struct GFNT_CffPrivate {
  bool present;            ///< Whether a Private DICT was found at all.
  GFNT_F16Dot16 default_width; ///< `defaultWidthX`, 16.16 font units.
  GFNT_F16Dot16 nominal_width; ///< `nominalWidthX`, 16.16 font units.
  GFNT_CffIndex subrs;     ///< Local subroutines; count 0 when there are none.
} GFNT_CffPrivate;

/**
 * One CFF font's structure.
 *
 * Memoised per face. Pointer-free by construction (see the file comment).
 */
typedef struct GFNT_Cff {
  size_t length;            ///< The table's own length, for bounds in messages.
  uint8_t major;            ///< Format major version; 1 is what this reads.
  uint8_t minor;            ///< Format minor version, not acted on.
  uint8_t header_size;      ///< Where the Name INDEX starts.
  uint8_t offset_size;      ///< The header's absOffSize. Read, never used.

  GFNT_CffIndex names;      ///< Font names; one entry for a single font.
  GFNT_CffIndex top_dicts;  ///< Top DICTs, parallel to `names`.
  GFNT_CffIndex strings;    ///< Strings past the standard ones.
  GFNT_CffIndex gsubrs;     ///< Global subroutines.
  GFNT_CffIndex charstrings;///< One element per glyph. Its count is numGlyphs.

  uint32_t charstring_type; ///< 2, or 1 for Type 1 charstrings in a CFF.
  size_t charset;           ///< Offset of the charset, when it has one.
  uint32_t charset_id;      ///< 0, 1 or 2 when the charset is predefined.
  bool charset_predefined;  ///< Whether `charset_id` is the answer.
  size_t encoding;          ///< Offset of the encoding, when it has one.
  uint32_t encoding_id;     ///< 0 or 1 when the encoding is predefined.
  bool encoding_predefined; ///< Whether `encoding_id` is the answer.

  bool is_cid;              ///< Whether the Top DICT carried `ROS`.
  GFNT_CffIndex fdarray;    ///< CID only: one Private DICT owner per FD.
  size_t fdselect;          ///< CID only: glyph to FD. 0 when absent.

  GFNT_CffPrivate priv;     ///< The one Private DICT of a non-CID font.

  /**
   * `FontMatrix`, 16.16, in the order the file gives it: xx, xy, yx, yy and
   * the two translations.
   *
   * Read and **compared against `head.unitsPerEm`** rather than applied. A
   * charstring's coordinates are in the font's own charstring space, which for
   * every sane font is the em space `unitsPerEm` describes; a font whose matrix
   * says otherwise is refused by name, because drawing it at the wrong scale
   * while reporting success is the worse failure and applying it silently would
   * put this library and every reference pen in different spaces.
   */
  GFNT_F16Dot16 font_matrix[6];
  bool font_matrix_stated;  ///< Whether the Top DICT carried one.
} GFNT_Cff;

/**
 * Parse the face's `CFF ` table, for ::gfnt_table_cached().
 *
 * @param face The face.
 * @param out A ::GFNT_Cff to fill in.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED for a face with no `CFF `, a CFF
 *   whose major version is not 1, or a construction this library refuses by
 *   name; ::GFNT_ERR_CORRUPT; or ::GFNT_ERR_LIMIT.
 */
GFNT_Result gfnt_cff_parse(const GFNT_Face * face, void * out,
    GFNT_Error * error);

/**
 * The face's parsed CFF, parsing it on first use.
 *
 * @param face The face.
 * @param out_cff Receives a pointer into the face's memo, valid as long as the
 *   face is.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, or whatever ::gfnt_cff_parse() returned.
 */
GFNT_Result gfnt_face_cff(const GFNT_Face * face, const GFNT_Cff ** out_cff,
    GFNT_Error * error);

/**
 * How many glyphs the `CharStrings` INDEX implies, for the numGlyphs minimum.
 *
 * M12: `maxp` states a count and every table that indexes glyphs implies one.
 * A CFF glyph past `CharStrings` has no charstring to read at all - there is no
 * broken offset to condemn one glyph by, the way `loca` has - so the count
 * joins the minimum rather than being checked per glyph.
 *
 * @param face The face.
 * @param out_bound Receives the count.
 * @return true when the face has a readable `CFF ` whose count this is. A
 *   failure to parse is reported as false rather than as an error, because the
 *   caller is computing a glyph count and a corrupt CFF is the CFF reader's
 *   news to break, not the glyph count's.
 */
bool gfnt_cff_glyph_bound(const GFNT_Face * face, size_t * out_bound);

/**
 * Set up a reader over one element of an INDEX.
 *
 * @param table A reader over the whole `CFF ` table.
 * @param index The INDEX.
 * @param element Which element, from 0.
 * @param out_reader Receives a reader spanning exactly that element.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID for an element the INDEX does not
 *   have, or ::GFNT_ERR_CORRUPT when its offsets do not describe a range
 *   inside the table.
 */
GFNT_Result gfnt_cff_index_at(const GFNT_Reader * table,
    const GFNT_CffIndex * index, uint32_t element, GFNT_Reader * out_reader,
    GFNT_Error * error);

/**
 * Read an INDEX at the reader's cursor, leaving the cursor past it.
 *
 * @param reader The table reader, positioned at the INDEX.
 * @param out_index Receives it.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK or ::GFNT_ERR_CORRUPT.
 */
GFNT_Result gfnt_cff_index_parse(GFNT_Reader * reader,
    GFNT_CffIndex * out_index, GFNT_Error * error);

/**
 * The Private DICT that governs one glyph.
 *
 * For a non-CID font this is the font's single Private DICT and the glyph is
 * not consulted. For a CID-keyed font, `FDSelect` names which of the
 * `FDArray`'s dictionaries a glyph belongs to, and they differ in the local
 * subroutines *and* in the width defaults - so a reader that used the first
 * one for every glyph would draw most CID fonts nearly right.
 *
 * Parsed per call rather than memoised: an FD's Private DICT is a few dozen
 * bytes, there may be 255 of them, and the memo is a by-value struct that
 * cannot hold an array of them without owning memory.
 *
 * @param face The face.
 * @param cff Its parsed CFF.
 * @param glyph The glyph index.
 * @param out_private Receives the dictionary.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_CORRUPT or ::GFNT_ERR_UNSUPPORTED.
 */
GFNT_Result gfnt_cff_private_for_glyph(const GFNT_Face * face,
    const GFNT_Cff * cff, uint32_t glyph, GFNT_CffPrivate * out_private,
    GFNT_Error * error);

/**
 * The SID the charset gives a glyph.
 *
 * @param face The face.
 * @param cff Its parsed CFF.
 * @param glyph The glyph index.
 * @param out_sid Receives the SID.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID for a glyph the charset does not
 *   cover, ::GFNT_ERR_CORRUPT, or ::GFNT_ERR_UNSUPPORTED for a charset format
 *   this library does not read.
 */
GFNT_Result gfnt_cff_glyph_sid(const GFNT_Face * face, const GFNT_Cff * cff,
    uint32_t glyph, uint32_t * out_sid, GFNT_Error * error);

/**
 * The first glyph whose charset SID is @p sid.
 *
 * A linear scan of the charset, which is what `seac` needs and what a name
 * lookup needs; both are asked once per distinct name.
 *
 * @param face The face.
 * @param cff Its parsed CFF.
 * @param sid The SID.
 * @param out_glyph Receives the glyph index.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID when no glyph has that SID, or
 *   ::GFNT_ERR_CORRUPT.
 */
GFNT_Result gfnt_cff_glyph_for_sid(const GFNT_Face * face,
    const GFNT_Cff * cff, uint32_t sid, uint32_t * out_glyph,
    GFNT_Error * error);

/**
 * A SID's string, copied into a caller's buffer.
 *
 * A SID below ::GFNT_CFF_STANDARD_STRING_COUNT is one of the predefined
 * strings; at or above it, it indexes the font's own `String` INDEX.
 *
 * @param face The face.
 * @param cff Its parsed CFF.
 * @param sid The SID.
 * @param buffer Where the NUL-terminated name goes.
 * @param size How big @p buffer is.
 * @param out_length Receives the name's length excluding the NUL, or NULL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a SID past the font's strings;
 *   ::GFNT_ERR_LIMIT when the name does not fit; or ::GFNT_ERR_CORRUPT.
 */
GFNT_Result gfnt_cff_string(const GFNT_Face * face, const GFNT_Cff * cff,
    uint32_t sid, char * buffer, size_t size, size_t * out_length,
    GFNT_Error * error);

/**
 * The glyph a code maps to under the font's own encoding.
 *
 * Not what a `cmap` answers: a CFF inside an sfnt carries an encoding that
 * OpenType says to ignore, and this is for the containers where it is the only
 * mapping there is - bare CFF in a PDF, and Type 1.
 *
 * @param face The face.
 * @param cff Its parsed CFF.
 * @param code The code, 0-255.
 * @param out_glyph Receives the glyph index.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID when the code maps to nothing;
 *   ::GFNT_ERR_UNSUPPORTED for the Expert encoding, which is predefined and
 *   whose table this library does not carry; or ::GFNT_ERR_CORRUPT.
 */
GFNT_Result gfnt_cff_glyph_for_code(const GFNT_Face * face,
    const GFNT_Cff * cff, uint8_t code, uint32_t * out_glyph,
    GFNT_Error * error);

/**
 * One glyph's name from the charset, counted when @p out is NULL.
 *
 * The same contract as `post`'s ::gfnt_post_name_at(), so that `glyph/names.c`
 * can choose between them without knowing which it has. **For a CFF face the
 * charset is the authority**: the OpenType specification says such a font's
 * `post` should be version 3 and carry no names at all, and every reference -
 * fontTools included - takes its glyph order from the charset.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param out Where the NUL-terminated name goes, or NULL to only count.
 * @param capacity How big @p out is; ignored when it is NULL.
 * @param out_length Receives the length, excluding the NUL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED for a face with no `CFF `;
 *   ::GFNT_ERR_INVALID for a glyph the charset does not name; ::GFNT_ERR_LIMIT;
 *   or ::GFNT_ERR_CORRUPT.
 */
GFNT_Result gfnt_cff_name_at(const GFNT_Face * face, uint32_t glyph,
    char * out, size_t capacity, size_t * out_length, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_CFF_H
