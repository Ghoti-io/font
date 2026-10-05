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
 * Shaping: code points in, positioned glyphs out, through `GSUB` and `GPOS`.
 *
 * documentation/design.md section 9. This is the OpenType Layout engine and the
 * *default* shaper's pipeline around it: map the code points through `cmap`, run
 * the `GSUB` lookups of the default features, take advances from the metric
 * tables, run the `GPOS` lookups, then do what the specification leaves to the
 * shaper (zero the advance of a mark, hide a default-ignorable). The order lookups
 * run in, the way a context lookup recurses, and which glyph a mark attaches to
 * are HarfBuzz's, held to it by `tools/oracle/hb_diff.py`.
 *
 * **What it does not do, stated rather than approximated:**
 *
 *   * **A few characters sorted differently.** Arabic and the scripts that join
 *     like it (Syriac, Mongolian, N'Ko, Mandaic and the rest) are shaped with
 *     HarfBuzz's joining state machine and, for a font with no joining features,
 *     from the Unicode presentation forms; Hebrew has its own composition of
 *     letters and points; Thai and Lao move their SARA AM; Hangul jamo become the
 *     syllable the font has; the Indic scripts, Khmer, Myanmar and the Universal
 *     Shaping Engine's scripts cut a run into syllables, reorder them and select
 *     the forms the font's features make. In nine scripts a handful of
 *     characters are sorted into a different category than HarfBuzz's table
 *     gives them; `hb_diff.py` lists them.
 *   * **No bidirectional reordering.** A run is one direction: left to right,
 *     right to left, top to bottom or bottom to top. Vertical text is shaped as
 *     HarfBuzz shapes it for a font that is not variable; at a location in the
 *     design space a vertical run is refused.
 *   * **No device table for a pixel size.** Positions are in font units; a
 *     `VariationIndex` device table is read when a location is given.
 *
 * Reference: OpenType Specification 1.9, "GSUB", "GPOS", "GDEF" and "OpenType
 * Layout Common Table Formats".
 */

#ifndef GHOTI_IO_GFNT_SHAPE_H
#define GHOTI_IO_GFNT_SHAPE_H

#include <ghoti.io/font/allocator.h>
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

/** @brief The end of a feature's range, meaning "to the end of the run". */
#define GFNT_SHAPE_END ((size_t)-1)

/** @brief Which way a run reads. */
typedef enum GFNT_Direction {
  GFNT_DIRECTION_LTR = 0, ///< Left to right: the glyphs come back in text order.
  GFNT_DIRECTION_RTL,     ///< Right to left: they come back in visual order.
  /**
   * Top to bottom: the glyphs come back in text order, and each one moves the pen
   * down by `y_advance`, which is negative because y points up. The `vert`
   * feature is on in place of the horizontal ones, advances come from `vmtx` and
   * every offset puts the glyph's vertical origin, from `VORG` or `vmtx`, where
   * the pen is.
   */
  GFNT_DIRECTION_TTB,
  GFNT_DIRECTION_BTT      ///< Bottom to top: as top to bottom, in visual order.
} GFNT_Direction;

/**
 * @brief One feature the caller turns on or off, for the whole run or a stretch of it.
 *
 * Without any of these the default features apply. A feature with @p value 0 is
 * off, with 1 on, and with more than 1 it is the *alternate* a `salt`- or
 * `aalt`-style lookup picks.
 */
typedef struct GFNT_ShapeFeature {
  GFNT_Tag tag;    ///< The feature, as GFNT_TAG('l','i','g','a').
  uint32_t value;  ///< 0 off, 1 on, or the alternate number.
  size_t start;    ///< The first code point it applies to.
  size_t end;      ///< One past the last, or ::GFNT_SHAPE_END.
} GFNT_ShapeFeature;

/** @brief What a run is shaped as. A zeroed one is valid. */
typedef struct GFNT_ShapeOptions {
  GFNT_Tag script;    ///< The OpenType script tag ('latn'), or 0 for the default.
  GFNT_Tag language;  ///< The OpenType language system tag ('TRK '), or 0.
  GFNT_Direction direction;           ///< The direction the run is set in; zero is left to right.
  const GFNT_ShapeFeature * features; ///< Or NULL.
  size_t feature_count;               ///< How many entries @p features has.
  const GFNT_Variation * variation;   ///< The location in the design space, or NULL.
} GFNT_ShapeOptions;

/**
 * @brief One glyph of a shaped run. Distances are in font units.
 *
 * The pen is moved by @p x_advance after the glyph is drawn at its offset; the
 * offset does not move the pen.
 */
typedef struct GFNT_ShapedGlyph {
  uint32_t glyph;     ///< The glyph ID in the face.
  uint32_t cluster;   ///< The index of the first code point this glyph stands for.
  int32_t x_advance;  ///< How far the pen moves horizontally after this glyph.
  int32_t y_advance;  ///< How far the pen moves vertically after this glyph.
  int32_t x_offset;   ///< Horizontal displacement of the drawn glyph from the pen.
  int32_t y_offset;   ///< Vertical displacement of the drawn glyph from the pen.
} GFNT_ShapedGlyph;

/**
 * @brief The result of shaping a run. Free with ::gfnt_shaped_run_free().
 *
 * Clusters are non-decreasing in text order (so, for a right-to-left run, they
 * run backwards through the array) and a ligature takes the smallest cluster of
 * its components.
 */
typedef struct GFNT_ShapedRun {
  GFNT_ShapedGlyph * glyphs;  ///< The glyphs, in the order described above.
  size_t count;               ///< How many entries @p glyphs has.
  const GFNT_Allocator * allocator; ///< What @p glyphs was allocated with.
  GFNT_Tag script;    ///< The script the font's tables were searched with, or 0.
  GFNT_Tag language;  ///< The language system, or 0 for the script's default.
  size_t gsub_lookups; ///< How many `GSUB` lookups ran.
  size_t gpos_lookups; ///< How many `GPOS` lookups ran.
} GFNT_ShapedRun;

/**
 * @brief Shape a run of code points.
 *
 * @param face The face.
 * @param codepoints The text, as Unicode code points.
 * @param count How many. 0 gives an empty run.
 * @param options What to shape it as, or NULL for left to right Latin with the
 *   default features.
 * @param allocator Where the run's glyphs come from, or NULL for the default. The
 *   run keeps the pointer.
 * @param out_run Receives the run; its previous contents are not freed. Written
 *   only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a NULL argument or a feature range
 *   that is backwards; ::GFNT_ERR_UNSUPPORTED if the face has no `cmap` or
 *   `hmtx`; ::GFNT_ERR_CORRUPT, naming the table and the offset, for a layout
 *   table that reads past itself; ::GFNT_ERR_LIMIT for a run longer than the
 *   engine shapes; ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_face_shape(const GFNT_Face * face,
    const uint32_t * codepoints, size_t count, const GFNT_ShapeOptions * options,
    const GFNT_Allocator * allocator, GFNT_ShapedRun * out_run,
    GFNT_Error * error);

/** @brief Release a run's glyphs. A zeroed run, or one already freed, is harmless. */
GFNT_API void gfnt_shaped_run_free(GFNT_ShapedRun * run);

/**
 * @brief Print a face's layout tables: scripts, language systems, features and the
 * type of every lookup.
 *
 * @param face The face.
 * @param table `GSUB` or `GPOS`.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_UNSUPPORTED for a face without the table, or
 *   ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_layout_dump(const GFNT_Face * face, GFNT_Tag table,
    FILE * out);

/**
 * @brief The script a run of text is written in, as an OpenType tag.
 *
 * The first character that belongs to a script of its own, which is how
 * HarfBuzz guesses it: a digit or a space (Common) and a combining mark
 * (Inherited) say nothing. It is what ::gfnt_face_shape() uses when the options
 * name no script.
 *
 * @param codepoints The text.
 * @param count How many.
 * @return The tag (`GFNT_TAG('a','r','a','b')`), or 0 if the text has none.
 */
GFNT_API GFNT_Tag gfnt_shape_script_of(const uint32_t * codepoints,
    size_t count);

/**
 * @brief The direction a script is written in.
 *
 * @param script An OpenType script tag, or 0.
 * @return ::GFNT_DIRECTION_RTL for Arabic, Hebrew, Syriac and the other
 *   right-to-left scripts, ::GFNT_DIRECTION_LTR for everything else.
 */
GFNT_API GFNT_Direction gfnt_shape_script_direction(GFNT_Tag script);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_SHAPE_H
