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
 *   * **Scripts with a shaper of their own.** Arabic and the scripts that join
 *     like it (Syriac, Mongolian, N'Ko, Mandaic and the rest) are shaped with
 *     HarfBuzz's joining state machine and, for a font with no joining features,
 *     from the Unicode presentation forms; Hebrew has its own composition of
 *     letters and points; Thai and Lao move their SARA AM; Hangul jamo become the
 *     syllable the font has; the Indic scripts, Khmer, Myanmar and the Universal
 *     Shaping Engine's scripts cut a run into syllables, reorder them and select
 *     the forms the font's features make.
 *   * **One direction per run.** ::gfnt_face_shape() and ::gfnt_faces_shape()
 *     take a run that is left to right, right to left, top to bottom or bottom
 *     to top; ::gfnt_faces_shape_bidi() splits a mixed paragraph into such runs.
 *     Vertical text is shaped as HarfBuzz shapes it, including at a location in
 *     the design space: the origin follows `gvar`'s top phantom point or `VVAR`'s
 *     origin delta, and the advance `VVAR` or `gvar`.
 *   * **Apple's tables in part.** A font with `morx` (or the older `mort`) is
 *     substituted by it and not by `GSUB`, as HarfBuzz does, and a version 2
 *     `kerx` kerns and attaches by its subtables (formats 0, 1, 2, 4 and 6) in
 *     place of `GPOS` and `kern`; `trak` tracks by the point size when one is
 *     given. The rest of AAT is not read.
 *   * **A device table is read for a pixel size only when `ppem` is given.**
 *     Positions are in font units; a hinting `Device` table's pixels are scaled
 *     back to font units, and a `VariationIndex` one is read when a location is
 *     given.
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
  float point_size;                   ///< The size in points, or 0 for none: Apple's `trak` tracks by it.
  uint32_t ppem;                      ///< The size in pixels per em, or 0 for none: a hinting `Device` table is read for it.
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
 * @brief One stretch of a text, shaped with one face of a fallback list.
 *
 * The clusters of @p run are indices into the whole text, not into the stretch.
 */
typedef struct GFNT_FaceRun {
  size_t face;     ///< The index into the list of faces that shaped this stretch.
  size_t start;    ///< The first code point of the stretch.
  size_t length;   ///< How many code points it has.
  GFNT_ShapedRun run; ///< The glyphs, positioned as ::gfnt_face_shape() does.
} GFNT_FaceRun;

/**
 * @brief A text shaped with a list of faces. Free with ::gfnt_face_runs_free().
 *
 * The stretches are in visual order: for a right-to-left or bottom-to-top run the
 * last stretch of the text comes first, so drawing them one after another, each
 * where the last one's pen stopped, draws the text.
 */
typedef struct GFNT_FaceRuns {
  GFNT_FaceRun * runs; ///< The stretches.
  size_t count;        ///< How many.
  const GFNT_Allocator * allocator; ///< What @p runs was allocated with.
} GFNT_FaceRuns;

/**
 * @brief A script's baseline from the `BASE` table, such as `ideo` or `romn`.
 *
 * The script is looked up by its own tag and then as `DFLT`. A reference-point
 * coordinate (format 2) is read as its stated value, as for a `GPOS` anchor.
 *
 * @param face The face.
 * @param baseline The baseline's tag, as GFNT_TAG('i','d','e','o').
 * @param vertical Non-zero for the vertical axis' baselines, 0 for the horizontal's.
 * @param script The OpenType script tag.
 * @param variation The location in the design space, or NULL.
 * @param ppem The size in pixels per em for a hinting device table, or 0.
 * @param out Receives the coordinate in font units. Written only on success.
 * @return true when the table gives that baseline for that script.
 */
GFNT_API bool gfnt_face_baseline(const GFNT_Face * face, GFNT_Tag baseline,
    int vertical, GFNT_Tag script, const GFNT_Variation * variation,
    uint32_t ppem, int32_t * out);

/**
 * @brief The caret positions `GDEF` gives inside a ligature glyph.
 *
 * A text editor puts the cursor between the components of a ligature at these
 * positions. A caret given by a coordinate, or by a coordinate and a device
 * table, is read as such; one given by a contour point is the point's coordinate
 * in the glyph's outline, and is left out when the face has no outline to read.
 * A hinting device table is applied when @p ppem is not 0, and a variation index
 * when @p variation is given.
 *
 * @param face The face.
 * @param glyph The ligature glyph.
 * @param vertical Non-zero for the y coordinates of vertical text, 0 for the x.
 * @param variation The location in the design space, or NULL.
 * @param ppem The size in pixels per em, or 0.
 * @param out Receives the carets in font units, left to right as the font lists
 *   them, or is NULL to count only.
 * @param capacity How many entries @p out holds.
 * @return How many carets the glyph has, which may be more than @p capacity;
 *   0 for a face with no `GDEF`, no caret list, or no entry for the glyph.
 */
GFNT_API size_t gfnt_face_ligature_carets(const GFNT_Face * face, uint32_t glyph,
    int vertical, const GFNT_Variation * variation, uint32_t ppem, int32_t * out,
    size_t capacity);

/**
 * @brief Shape a text with the first face of a list that has each character.
 *
 * Font fallback. Each cluster (a character and the marks that follow it) goes to
 * the first face whose `cmap` maps all of it. When none does, the base character
 * goes to the first face that has it and each mark the base's face lacks goes to
 * the first face that has the mark, so a stack of combining marks over a letter
 * is drawn with whichever fonts hold the marks. A character no face has is shaped
 * by the first face and comes out as its `.notdef`.
 *
 * Consecutive characters of one face are shaped together, so a face's own
 * substitution and positioning run across them. They do not run across a change
 * of face: a ligature or an Arabic join cannot span two fonts. A mark set apart
 * from its base this way is given no advance and is centred over the base's
 * glyph, from the extents of the two, and stacked on the ink the cluster has so far:
 * a mark that hangs below the baseline under it, any other over it, so marks
 * from fallback faces form chains above and below the base. A mark that was
 * shaped with its base keeps what the font's own anchors give it. This is done
 * for a horizontal run; a vertical one is left where its own face puts it.
 *
 * @param faces The faces, most preferred first.
 * @param face_count How many. At least one.
 * @param codepoints The text.
 * @param count How many code points.
 * @param options As for ::gfnt_face_shape(). When it names no script, the text's
 *   own is used for every stretch, so they agree. A feature's range is cut to
 *   each stretch. A variation location applies to every face.
 * @param allocator Or NULL for the default. The result keeps the pointer.
 * @param out Receives the stretches; written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return As ::gfnt_face_shape(); ::GFNT_ERR_INVALID also for no faces or a NULL face.
 */
GFNT_API GFNT_Result gfnt_faces_shape(const GFNT_Face * const * faces,
    size_t face_count, const uint32_t * codepoints, size_t count,
    const GFNT_ShapeOptions * options, const GFNT_Allocator * allocator,
    GFNT_FaceRuns * out, GFNT_Error * error);

/** @brief The direction of a paragraph's base level, for ::gfnt_faces_shape_bidi(). */
typedef enum GFNT_BidiDirection {
  GFNT_BIDI_LTR = 0,  ///< Paragraph level 0.
  GFNT_BIDI_RTL,      ///< Paragraph level 1.
  GFNT_BIDI_AUTO      ///< The first strong character decides (UAX #9, P2 and P3).
} GFNT_BidiDirection;

/**
 * @brief Shape a paragraph that mixes left-to-right and right-to-left text.
 *
 * Runs the Unicode Bidirectional Algorithm (UAX #9) over the text, shapes each
 * run of one embedding level as ::gfnt_faces_shape() does, in its own direction
 * (an odd level reads right to left), and returns the stretches in visual order
 * for one line: drawing them one after another, each where the last one's pen
 * stopped, draws the paragraph. @p options->direction is not read, except that a
 * vertical one shapes the text as a single run with no reordering. The clusters
 * and starts are indices into the whole text.
 *
 * The text is one paragraph on one line: the caller splits paragraphs and
 * breaks lines (the levels of a line are what a line break would reorder). A
 * ligature or a join does not span two level runs, and a character's mirror image
 * is chosen as in ::gfnt_faces_shape() for a right-to-left run.
 *
 * @param paragraph The paragraph's base direction.
 * @return As ::gfnt_faces_shape(); ::GFNT_ERR_LIMIT for a text the algorithm will
 *   not take, and ::GFNT_ERR_INVALID for a direction that is not one of the three.
 */
GFNT_API GFNT_Result gfnt_faces_shape_bidi(const GFNT_Face * const * faces,
    size_t face_count, const uint32_t * codepoints, size_t count,
    GFNT_BidiDirection paragraph, const GFNT_ShapeOptions * options,
    const GFNT_Allocator * allocator, GFNT_FaceRuns * out, GFNT_Error * error);

/** @brief Release the stretches and their glyphs. A zeroed one is harmless. */
GFNT_API void gfnt_face_runs_free(GFNT_FaceRuns * runs);

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
