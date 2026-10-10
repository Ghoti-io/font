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
 * Paragraph layout: styled UTF-8 text in, lines of positioned glyphs out.
 *
 * documentation/design.md section 10. A paragraph is cut into items by style
 * span, by script and by bidirectional level, and each item is shaped once with
 * its span's fallback list of faces (::gfnt_faces_shape()). Laying it out at a
 * width then breaks the shaped clusters into lines at the opportunities UAX #14
 * gives, in logical order, puts each line's runs in visual order (UAX #9, rule
 * L2) and places them. The paragraph is not shaped again for a new width, which
 * is what makes resizing cheap.
 *
 * Positions are in 26.6 pixels, y down, with the origin at the top left of the
 * paragraph's box, and a glyph's pen position is the same number the painter
 * uses: measuring and painting are one code path.
 *
 * **What it does not do, stated rather than approximated:** vertical text
 * (a span's face runs horizontally), hyphenation, justification, tabs and
 * truncation, ruby, and anything that must know about a non-text box on its
 * line. Newline characters (LF, CR, VT, FF, U+0085, U+2028, U+2029) end a line;
 * the text is otherwise one bidirectional paragraph, so a caller with several
 * paragraphs lays out one object per paragraph.
 */

#ifndef GHOTI_IO_GFNT_LAYOUT_H
#define GHOTI_IO_GFNT_LAYOUT_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/shape.h>
#include <ghoti.io/font/variation.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A width that never wraps: the text breaks only at newline characters. */
#define GFNT_LAYOUT_UNBOUNDED INT32_MAX

/**
 * @brief How a stretch of text is set. Nothing here is owned: it all outlives the paragraph.
 *
 * A zeroed style is not valid: @p faces and @p size must be given.
 */
typedef struct GFNT_TextStyle {
  const GFNT_Face * const * faces; ///< The fallback list, most preferred first.
  size_t face_count;               ///< How many; at least one.
  GFNT_F26Dot6 size;               ///< Pixels per em, 26.6; more than zero.
  GFNT_Tag language;               ///< The OpenType language system tag, or 0.
  /** Features; a range counts code points from the start of the span. */
  const GFNT_ShapeFeature * features;
  size_t feature_count;            ///< How many entries @p features has.
  const GFNT_Variation * variation; ///< The location in the design space, or NULL.
  /** Whose line metrics a line takes from this span; zero is the font's own. */
  GFNT_LineMetricsPolicy line_metrics;
} GFNT_TextStyle;

/** @brief A stretch of the text and how it is set. The spans tile the text. */
typedef struct GFNT_TextSpan {
  size_t length;        ///< Its length in bytes; the spans' lengths sum to the text's.
  GFNT_TextStyle style; ///< How it is set.
} GFNT_TextSpan;

/** @brief Where a line's slack goes. START and END follow the paragraph's direction. */
typedef enum GFNT_Align {
  GFNT_ALIGN_START = 0, ///< The side the paragraph begins on.
  GFNT_ALIGN_END,       ///< The other side.
  GFNT_ALIGN_LEFT,
  GFNT_ALIGN_RIGHT,
  GFNT_ALIGN_CENTER
} GFNT_Align;

/** @brief How eagerly a line breaks, after CSS `line-break`. */
typedef enum GFNT_LineBreak {
  GFNT_LINE_BREAK_NORMAL = 0, ///< UAX #14 with the conventional tailoring.
  GFNT_LINE_BREAK_STRICT,     ///< Also keeps small kana and the like with what precedes.
  GFNT_LINE_BREAK_LOOSE,      ///< Allows breaks before small kana and the like.
  GFNT_LINE_BREAK_ANYWHERE    ///< Breaks between any two characters.
} GFNT_LineBreak;

/** @brief What a paragraph is, apart from its text and styles. A zeroed one is valid. */
typedef struct GFNT_ParagraphOptions {
  GFNT_BidiDirection direction; ///< The base direction, or ::GFNT_BIDI_AUTO.
  GFNT_Align align;             ///< Where the slack of a short line goes.
  GFNT_LineBreak line_break;    ///< How eagerly lines break.
} GFNT_ParagraphOptions;

/** @brief A shaped, itemised paragraph. Opaque; free with ::gfnt_paragraph_free(). */
typedef struct GFNT_Paragraph GFNT_Paragraph;

/**
 * @brief One glyph, placed.
 *
 * @p x and @p y are where to draw the glyph's origin; the pen is at @p pen_x
 * and moves @p advance after it (the difference is the shaper's offset).
 */
typedef struct GFNT_PlacedGlyph {
  uint32_t glyph;        ///< The glyph ID in the run's face.
  uint32_t cluster;      ///< The byte offset in the text of the first character it stands for.
  GFNT_F26Dot6 pen_x;    ///< The pen position before the glyph.
  GFNT_F26Dot6 x;        ///< The glyph origin's x.
  GFNT_F26Dot6 y;        ///< The glyph origin's y; the line's baseline when the font has no offset.
  GFNT_F26Dot6 advance;  ///< How far the pen moves; negative never, except for a kern.
} GFNT_PlacedGlyph;

/** @brief Glyphs of one face, one style and one direction, side by side on a line. */
typedef struct GFNT_LineRun {
  const GFNT_Face * face;       ///< The face that shaped the glyphs.
  size_t span;                  ///< The index of the span whose style it is.
  size_t face_index;            ///< The face's index in the span's list.
  bool fallback;                ///< True when it is not the span's first face (M8).
  uint8_t level;                ///< The bidirectional level; odd reads right to left.
  size_t start;                 ///< The first byte of the text it stands for.
  size_t end;                   ///< One past the last.
  const GFNT_PlacedGlyph * glyphs; ///< In visual order, left to right.
  size_t glyph_count;           ///< How many.
  GFNT_F26Dot6 x;               ///< The left edge of the run's advance, in the paragraph.
  GFNT_F26Dot6 advance;         ///< The run's width.
} GFNT_LineRun;

/** @brief One line of a laid-out paragraph. */
typedef struct GFNT_Line {
  size_t start;           ///< The first byte of the text on it.
  size_t end;             ///< One past the last, including a newline that ended it.
  const GFNT_LineRun * runs; ///< In visual order.
  size_t run_count;       ///< How many.
  GFNT_F26Dot6 x;         ///< The left edge of the line's text, after alignment.
  GFNT_F26Dot6 width;     ///< The text's width, not counting spaces that hang off the end.
  GFNT_F26Dot6 top;       ///< The top of the line box.
  GFNT_F26Dot6 baseline;  ///< The baseline's y.
  GFNT_F26Dot6 ascent;    ///< Above the baseline: the greatest of its runs' faces.
  GFNT_F26Dot6 descent;   ///< Below it, positive.
  GFNT_F26Dot6 height;    ///< The line box: ascent, descent and the greatest line gap.
} GFNT_Line;

/** @brief A paragraph laid out at one width. Free with ::gfnt_layout_free(). */
typedef struct GFNT_Layout {
  GFNT_Line * lines;     ///< The lines, top to bottom.
  size_t line_count;     ///< How many; at least one.
  GFNT_F26Dot6 width;    ///< The width laid out to, or the widest line when unbounded.
  GFNT_F26Dot6 height;   ///< The typographic box's height: the lines stacked.
  const GFNT_Allocator * allocator; ///< What the arrays came from.
  void * storage_;       ///< Internal: the runs and glyphs.
} GFNT_Layout;

/**
 * @brief Itemise and shape a paragraph.
 *
 * @param text UTF-8. An ill-formed sequence is one U+FFFD.
 * @param length Its length in bytes.
 * @param spans The spans, which tile the text; at least one, whatever the length.
 * @param span_count How many.
 * @param options Or NULL for a zeroed one.
 * @param allocator Or NULL for the default. The paragraph keeps the pointer.
 * @param out Receives the paragraph; written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a NULL argument, spans that do not tile
 *   the text or end inside a character, or a style with no face or no size; the
 *   errors of ::gfnt_faces_shape(); ::GFNT_ERR_LIMIT for a text longer than
 *   the bidirectional algorithm takes; ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_paragraph_create(const char * text, size_t length,
    const GFNT_TextSpan * spans, size_t span_count,
    const GFNT_ParagraphOptions * options, const GFNT_Allocator * allocator,
    GFNT_Paragraph ** out, GFNT_Error * error);

/** @brief Release a paragraph. NULL is harmless. */
GFNT_API void gfnt_paragraph_free(GFNT_Paragraph * paragraph);

/**
 * @brief Break the paragraph into lines no wider than a width, and place them.
 *
 * Lines break at UAX #14 opportunities, greedily. A line whose first unbreakable
 * stretch is wider than the width overflows rather than splitting a word.
 * Spaces at the end of a line hang: they are placed but not counted in the
 * line's width. The paragraph is not changed and can be laid out again.
 *
 * @param width The line width in 26.6 pixels, or ::GFNT_LAYOUT_UNBOUNDED.
 * @param out Receives the layout; written only on success.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_paragraph_layout(const GFNT_Paragraph * paragraph,
    GFNT_F26Dot6 width, GFNT_Layout * out, GFNT_Error * error);

/** @brief Release a layout. A zeroed one, or one already freed, is harmless. */
GFNT_API void gfnt_layout_free(GFNT_Layout * layout);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_LAYOUT_H
