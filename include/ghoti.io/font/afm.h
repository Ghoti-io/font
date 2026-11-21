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
 * Adobe Font Metrics files (`.afm`): the metrics a Type 1 font is distributed with, and the
 * form in which the metrics of the fourteen standard PDF fonts are published.
 *
 * An AFM file is not a font and is not a ::GFNT_Face: it has no outlines, no tables and no
 * glyph ids, only names, codes, widths, boxes and kerning pairs. So this is a standalone
 * object read from bytes, which a caller who wants a Type 1 font's metrics from its sidecar
 * (or the widths of a standard font with no font file) holds beside a face or without one.
 *
 * What is read is the Adobe Font Metrics File Format Specification 4.1's horizontal-writing
 * data: the header's global keys, every character's code, widths, name, box and ligatures,
 * and the pair kerning (`KP`, `KPX`, `KPH`, `KPY`). Vertical writing (`StartDirection 1`),
 * track kerning, composite characters and the other global keys are accepted and skipped:
 * a file that has them is read, and they are not offered. Like `write.h` this is not part
 * of the `font.h` umbrella.
 */

#ifndef GHOTI_IO_GFNT_AFM_H
#define GHOTI_IO_GFNT_AFM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A parsed AFM file. Owned by the caller; released with ::gfnt_afm_free(). */
typedef struct GFNT_Afm GFNT_Afm;

/**
 * The header's global keys that are read. A string the file did not give is NULL.
 *
 * This library holds no floating-point number in its interface, so every number of an AFM
 * file is given in thousandths: `278` is 278000 and `-12.5` is -12500. Three decimal places are
 * kept, and a value with more is rounded half away from zero; a value whose thousandths do not
 * fit an `int32_t` (past two million units) is refused as corrupt.
 */
typedef struct GFNT_AfmInfo {
  const char * font_name;       ///< `FontName`.
  const char * full_name;       ///< `FullName`.
  const char * family_name;     ///< `FamilyName`.
  const char * weight;          ///< `Weight`.
  const char * version;         ///< `Version`.
  const char * notice;          ///< `Notice`.
  const char * encoding_scheme; ///< `EncodingScheme` (`AdobeStandardEncoding`, `FontSpecific`, ...).
  const char * character_set;   ///< `CharacterSet`.
  int32_t italic_angle_milli;   ///< `ItalicAngle`, in thousandths of a degree; 0 when not given.
  bool is_fixed_pitch;          ///< `IsFixedPitch`.
  int32_t underline_position_milli; ///< `UnderlinePosition`; 0 when not given.
  int32_t underline_thickness_milli; ///< `UnderlineThickness`; 0 when not given.
  int32_t bbox_milli[4];        ///< `FontBBox`: llx, lly, urx, ury; zeros when not given.
  bool has_bbox;                ///< Whether `FontBBox` was given.
  int32_t cap_height_milli;     ///< `CapHeight`; 0 when not given.
  int32_t x_height_milli;       ///< `XHeight`.
  int32_t ascender_milli;       ///< `Ascender`.
  int32_t descender_milli;      ///< `Descender`.
  int32_t std_hw_milli;         ///< `StdHW`.
  int32_t std_vw_milli;         ///< `StdVW`.
} GFNT_AfmInfo;

/** One character's metrics, in thousandths as ::GFNT_AfmInfo says. The strings belong to the ::GFNT_Afm. */
typedef struct GFNT_AfmGlyph {
  int32_t code;           ///< The character code, or -1 for an unencoded glyph.
  int32_t width_x_milli;  ///< The advance along x (`WX`, `W0X`, or the x of `W`/`W0`).
  int32_t width_y_milli;  ///< The advance along y (`WY`, `W0Y`, or the y of `W`/`W0`); usually 0.
  const char * name;      ///< The glyph name (`N`); NULL if the line named none.
  int32_t bbox_milli[4];  ///< The box (`B`): llx, lly, urx, ury; zeros when not given.
  bool has_bbox;          ///< Whether `B` was given.
  const char * ligatures; ///< `L` pairs as "successor ligature" separated by spaces, or NULL.
} GFNT_AfmGlyph;

/**
 * @brief Read an AFM file.
 *
 * The text is copied, so @p data need not outlive the result. A line ends at LF, CR or CRLF.
 * Numbers are read without the C library's `strtod`, which follows the locale: `278.5` is
 * the same number under every `LC_NUMERIC`. A key this library does not read is skipped.
 *
 * @param data The file's bytes.
 * @param size How many.
 * @param limits Caps (`max_glyphs`, `max_line_length`, `max_blob_bytes`), or NULL for the defaults.
 * @param allocator Where the result comes from, or NULL for the default.
 * @param out_afm Receives the AFM. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for NULL arguments; ::GFNT_ERR_CORRUPT for a file
 *   that does not start with `StartFontMetrics`, a number that is not one, a section whose
 *   count disagrees with its lines or that is not closed, or no `EndFontMetrics`;
 *   ::GFNT_ERR_LIMIT; ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_afm_parse(const void * data, size_t size, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Afm ** out_afm, GFNT_Error * error);

/** @brief Release an AFM. NULL is ignored. */
GFNT_API void gfnt_afm_free(GFNT_Afm * afm);

/** @brief The header's global keys. The strings belong to @p afm. NULL gives a zeroed struct. */
GFNT_API GFNT_AfmInfo gfnt_afm_info(const GFNT_Afm * afm);

/** @brief How many characters the file lists. */
GFNT_API size_t gfnt_afm_glyph_count(const GFNT_Afm * afm);

/**
 * @brief The @p index'th character, in file order.
 *
 * @return ::GFNT_OK or ::GFNT_ERR_INVALID.
 */
GFNT_API GFNT_Result gfnt_afm_glyph_at(const GFNT_Afm * afm, size_t index, GFNT_AfmGlyph * out);

/**
 * @brief The character with this name.
 *
 * @param afm The AFM.
 * @param name The glyph name.
 * @param out Receives the character. Written only when the answer is true.
 * @return Whether the file lists one. If it lists the name twice, the first.
 */
GFNT_API bool gfnt_afm_glyph_for_name(const GFNT_Afm * afm, const char * name, GFNT_AfmGlyph * out);

/**
 * @brief The character at this code.
 *
 * @return Whether the file lists one. An unencoded glyph (code -1) is never found this way.
 */
GFNT_API bool gfnt_afm_glyph_for_code(const GFNT_Afm * afm, int32_t code, GFNT_AfmGlyph * out);

/**
 * @brief The kerning between two named glyphs, along x.
 *
 * @param afm The AFM.
 * @param first The left glyph's name.
 * @param second The right glyph's name.
 * @param out_kern_milli Receives the amount in thousandths (the file's `KPX`, or the x of a `KP`). Written only
 *   when the answer is true.
 * @return Whether the file has a pair for them.
 */
GFNT_API bool gfnt_afm_kern(const GFNT_Afm * afm, const char * first, const char * second,
    int32_t * out_kern_milli);

/** @brief How many kerning pairs the file lists. */
GFNT_API size_t gfnt_afm_kern_count(const GFNT_Afm * afm);

#ifdef __cplusplus
}
#endif

#endif
