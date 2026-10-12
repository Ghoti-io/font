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
 * **Vertical text** follows CSS Writing Modes (::GFNT_WritingMode, ::GFNT_TextOrientation): the
 * coordinates stay physical, lines are columns, and the width given to
 * ::gfnt_paragraph_layout() is the room *along* the lines, a height. A run says how its glyphs
 * sit (::GFNT_Orientation); a turned run's glyphs are drawn turned about their origin.
 *
 * **What it does not do, stated rather than approximated:** kashida justification
 * (a justified line widens its spaces and its ideograph gaps only), hyphenation data (a caller supplies the dictionary, ::GFNT_Hyphenator), ruby, and anything that must know about a non-text box on its
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

/** @brief Which way lines run and stack, after CSS `writing-mode`. */
typedef enum GFNT_WritingMode {
  GFNT_WRITING_HORIZONTAL_TB = 0, ///< Lines run left to right (or right to left), stack downward.
  GFNT_WRITING_VERTICAL_RL,       ///< Lines run downward and stack from the right to the left.
  GFNT_WRITING_VERTICAL_LR,       ///< Lines run downward and stack from the left to the right.
  /** As ::GFNT_WRITING_VERTICAL_RL with every character turned a quarter clockwise. */
  GFNT_WRITING_SIDEWAYS_RL,
  /**
   * Lines run upward (text begins at the bottom edge) and stack from the left to the right, every
   * character turned a quarter counterclockwise: ::GFNT_WRITING_SIDEWAYS_RL turned half a turn.
   */
  GFNT_WRITING_SIDEWAYS_LR
} GFNT_WritingMode;

/** @brief How characters sit in vertical text, after CSS `text-orientation`. */
typedef enum GFNT_TextOrientation {
  /**
   * Each character by UAX #50 `Vertical_Orientation`: upright for the ideographs and
   * kana and their punctuation, sideways for the rest (Latin, digits, Hebrew, Arabic...). A
   * character that has a vertical form in the font (`vert`) is upright if the font has it.
   */
  GFNT_ORIENT_MIXED = 0,
  GFNT_ORIENT_UPRIGHT,  ///< Every character upright, Latin included, on vertical advances.
  GFNT_ORIENT_SIDEWAYS, ///< Every character sideways, as ::GFNT_WRITING_SIDEWAYS_RL.
  /**
   * As ::GFNT_ORIENT_MIXED, but as Pango's "natural" gravity sets text where the two differ
   * (measured over the whole Basic Multilingual Plane, `tools/oracle/orientation_survey.py`):
   * a character UAX #50 calls transformed-rotated (`Tr`: the CJK brackets, fullwidth
   * parentheses, colon, semicolon and the like) is turned even when the face has a vertical
   * form for it, where CSS sets it upright if the face has one; U+2B97, U+2E50 and U+2E51 are
   * turned where UAX #50 sets them upright; a few emoji-like symbols (U+261D, U+2795 to
   * U+2797, U+27B0, U+27BF) are set the way the character before them is, and U+0D4E is
   * upright. Elsewhere the two agree.
   */
  GFNT_ORIENT_MIXED_PANGO
} GFNT_TextOrientation;

/** @brief How a run's glyphs sit relative to the page. */
typedef enum GFNT_Orientation {
  GFNT_ORIENT_RUN_HORIZONTAL = 0, ///< Drawn as they are, along a horizontal line.
  GFNT_ORIENT_RUN_UPRIGHT,        ///< Drawn as they are, along a vertical line (vertical advances).
  GFNT_ORIENT_RUN_CW,             ///< Drawn turned a quarter clockwise about their origin.
  GFNT_ORIENT_RUN_CCW             ///< Drawn turned a quarter counterclockwise about their origin.
} GFNT_Orientation;

/** @brief A rectangle in the paragraph's coordinates, 26.6 pixels, y down. */
typedef struct GFNT_Rect {
  GFNT_F26Dot6 x;       ///< The left edge.
  GFNT_F26Dot6 y;       ///< The top edge.
  GFNT_F26Dot6 width;   ///< The width.
  GFNT_F26Dot6 height;  ///< The height.
} GFNT_Rect;

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
  /**
   * Extra space after every character, in 26.6 pixels (negative tightens it).
   * It is added to the advance of the last glyph of each cluster, in reading
   * order. Ligatures are not switched off, as CSS does.
   */
  GFNT_F26Dot6 letter_spacing;
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
  GFNT_ALIGN_CENTER,
  /**
   * Both edges: the opportunities of ::GFNT_ParagraphOptions::justify on a line grow so it fills
   * the room. A line that ends a paragraph or a newline, a truncated line, a line with no
   * opportunity on it and any line of an unbounded layout is aligned to the start instead.
   */
  GFNT_ALIGN_JUSTIFY
} GFNT_Align;

/**
 * @brief Where the slack of a justified line goes, after CSS `text-justify`.
 *
 * Every opportunity gets the same share. The last character of a line has none after it, and a
 * line that ends a paragraph or a newline, a truncated line and any line of an unbounded layout
 * are not justified.
 */
typedef enum GFNT_Justify {
  /**
   * Spaces, and the gap after every CJK ideograph, kana, bopomofo, CJK symbol or punctuation, or
   * full- or half-width form (not Hangul, which is justified by its spaces, and not the em dash
   * or ellipsis), and before one that follows something other than a space. A space is one
   * opportunity, whatever stands beside it. This is what Chromium does, and for text with no
   * ideographs it is the same as ::GFNT_JUSTIFY_INTER_WORD.
   */
  GFNT_JUSTIFY_AUTO = 0,
  GFNT_JUSTIFY_INTER_WORD,       ///< Spaces only: a gap in a script written without them stays closed.
  GFNT_JUSTIFY_INTER_CHARACTER,  ///< After every character (every cluster), spaces included.
  GFNT_JUSTIFY_NONE              ///< Not at all: the line is set as if aligned to its start.
} GFNT_Justify;

/** @brief How eagerly a line breaks, after CSS `line-break`. */
typedef enum GFNT_LineBreak {
  GFNT_LINE_BREAK_NORMAL = 0, ///< UAX #14 with the conventional tailoring.
  GFNT_LINE_BREAK_STRICT,     ///< Also keeps small kana and the like with what precedes.
  GFNT_LINE_BREAK_LOOSE,      ///< Allows breaks before small kana and the like.
  GFNT_LINE_BREAK_ANYWHERE    ///< Breaks between any two characters.
} GFNT_LineBreak;

/** @brief What hyphenation a paragraph does, after CSS `hyphens`. */
typedef enum GFNT_Hyphens {
  /**
   * Only where the text says: a soft hyphen (U+00AD) is a place a line may break, and a line
   * that does break there shows a hyphen at its end. Anywhere else a word is not split.
   */
  GFNT_HYPHENS_MANUAL = 0,
  GFNT_HYPHENS_NONE,  ///< A soft hyphen is not a place to break either; it is invisible.
  /** Soft hyphens, and the places ::GFNT_Hyphenator names in the words it is asked about. */
  GFNT_HYPHENS_AUTO,
  /**
   * As ::GFNT_HYPHENS_MANUAL, but a soft hyphen after an ideograph is a place to break (with no
   * hyphen shown), as Pango has it. With ::GFNT_HYPHENS_MANUAL, Chromium's way, a soft hyphen
   * after an ideograph is no place to break and is invisible: the break falls where it would
   * have without it.
   */
  GFNT_HYPHENS_MANUAL_PANGO
} GFNT_Hyphens;

/**
 * @brief Where a word may be hyphenated. The library has no hyphenation data (patterns are a
 * dictionary per language, each with its own licence); a caller supplies the dictionary.
 *
 * Asked once for every word of two or more letters that holds no soft hyphen (a word is a stretch
 * of letters and combining marks within one span), when the paragraph is made, and only for
 * ::GFNT_HYPHENS_AUTO. A point it names is a place a line may break, with a hyphen added at the
 * end of the line it breaks.
 */
typedef struct GFNT_Hyphenator {
  void * ctx; ///< Passed to the callback untouched.
  /**
   * @brief Mark where a hyphen may stand in a word.
   *
   * @param word The word, UTF-8, not terminated.
   * @param length Its length in bytes.
   * @param language The span's language tag, or 0.
   * @param allowed @p length + 1 bytes, all zero: set `allowed[k]` to 1 where a hyphen may stand
   *   before the byte at offset @p k. Entries 0 and @p length, and any that is not on a
   *   character boundary, are ignored.
   * @return ::GFNT_OK, or an error, which ends ::gfnt_paragraph_create() with that error.
   */
  GFNT_Result (*points)(void * ctx, const char * word, size_t length, GFNT_Tag language,
      uint8_t * allowed);
} GFNT_Hyphenator;

/**
 * @brief A source of faces for characters a span's own faces lack.
 *
 * Asked once for each such character of an item, before it is shaped; a face it
 * returns joins the span's list for that item (after the span's own faces) and
 * the run's @p fallback flag is set. The faces must outlive the paragraph and its
 * layouts. Control characters and spaces are not asked about.
 */
typedef struct GFNT_FontProvider {
  void * ctx; ///< Passed to the callback untouched.
  /**
   * @brief A face that maps @p codepoint, or NULL for none.
   *
   * @param script The OpenType script tag of the item, or 0.
   * @param language The span's language tag, or 0.
   * @param style The span's style.
   */
  const GFNT_Face * (*font_for)(void * ctx, uint32_t codepoint, GFNT_Tag script,
      GFNT_Tag language, const GFNT_TextStyle * style);
} GFNT_FontProvider;

/** @brief What a paragraph is, apart from its text and styles. A zeroed one is valid. */
typedef struct GFNT_ParagraphOptions {
  GFNT_BidiDirection direction; ///< The base direction, or ::GFNT_BIDI_AUTO.
  GFNT_Align align;             ///< Where the slack of a short line goes.
  GFNT_LineBreak line_break;    ///< How eagerly lines break.
  /** The most lines to lay out, or 0 for all of them. Text past them is dropped. */
  size_t max_lines;
  /**
   * UTF-8 (at most 8 characters, copied) set at the end of a line that
   * @p max_lines cut short or that overflows its width, in the style of the text
   * it follows and with that span's first face. NULL puts no mark: the lines are
   * simply cut. It is read only when @p max_lines is not 0.
   */
  const char * ellipsis;
  /**
   * The distance between tab stops, from the start of each line, in 26.6
   * pixels; 0 for eight spaces of the first span's first face.
   */
  GFNT_F26Dot6 tab_width;
  GFNT_FontProvider font_provider; ///< Or zeroed for none.
  GFNT_Hyphens hyphens;            ///< What hyphenation is done.
  GFNT_Hyphenator hyphenator;      ///< Or zeroed for none; read only for ::GFNT_HYPHENS_AUTO.
  GFNT_Justify justify;            ///< Where a justified line's slack goes.
  GFNT_WritingMode writing_mode;   ///< Which way lines run and stack.
  /** How characters sit in a vertical mode; ignored for ::GFNT_WRITING_HORIZONTAL_TB. */
  GFNT_TextOrientation text_orientation;
} GFNT_ParagraphOptions;

/** @brief A shaped, itemised paragraph. Opaque; free with ::gfnt_paragraph_free(). */
typedef struct GFNT_Paragraph GFNT_Paragraph;

/**
 * @brief One glyph, placed.
 *
 * @p x and @p y are where to draw the glyph's origin (turned about it, for a turned run); the
 * pen is at (@p pen_x, @p pen_y) and moves @p advance after it along the line (the difference is
 * the shaper's offset and, in vertical text, the centring on the line's axis).
 */
typedef struct GFNT_PlacedGlyph {
  uint32_t glyph;        ///< The glyph ID in the run's face.
  uint32_t cluster;      ///< The byte offset in the text of the first character it stands for.
  GFNT_F26Dot6 pen_x;    ///< The pen position before the glyph: along a horizontal line, else on its axis.
  GFNT_F26Dot6 pen_y;    ///< The pen position's y: the baseline for a horizontal line, the place along a vertical one.
  GFNT_F26Dot6 x;        ///< The glyph origin's x.
  GFNT_F26Dot6 y;        ///< The glyph origin's y; the line's baseline when the font has no offset.
  GFNT_F26Dot6 advance;  ///< How far the pen moves along the line (right, or down); negative never, except a kern.
} GFNT_PlacedGlyph;

/** @brief Glyphs of one face, one style and one direction, side by side on a line. */
typedef struct GFNT_LineRun {
  const GFNT_Face * face;       ///< The face that shaped the glyphs.
  size_t span;                  ///< The index of the span whose style it is.
  size_t face_index;            ///< The face's index in the span's list.
  bool fallback;                ///< True when it is not the span's first face (M8).
  bool ellipsis;                ///< True for the truncation mark, which stands for no text.
  /**
   * True for the hyphen a line that breaks inside a word ends with, which stands for no text. It
   * is U+2010 if the span's first face has it and U+002D if not, set in that face.
   */
  bool hyphen;
  uint8_t level;                ///< The bidirectional level; odd reads right to left.
  GFNT_Orientation orientation; ///< How its glyphs sit; horizontal in a horizontal paragraph.
  size_t start;                 ///< The first byte of the text it stands for.
  size_t end;                   ///< One past the last.
  const GFNT_PlacedGlyph * glyphs; ///< In visual order along the line (see ::GFNT_Line::runs).
  size_t glyph_count;           ///< How many.
  /**
   * Where the run's advance begins along the line, in the paragraph: its left edge, or in
   * vertical text its top edge (the edge with the smaller coordinate, whichever way the line
   * reads).
   */
  GFNT_F26Dot6 pos;
  GFNT_F26Dot6 advance;         ///< The run's length along the line: its width, or its height.
} GFNT_LineRun;

/**
 * @brief One line of a laid-out paragraph: a row, or in vertical text a column.
 *
 * Positions are physical. "Along the line" is x for a row and y for a column; the line box is
 * the rectangle of the line's whole thickness, which is as long as the paragraph is.
 */
typedef struct GFNT_Line {
  size_t start;           ///< The first byte of the text on it.
  size_t end;             ///< One past the last, including a newline that ended it.
  /**
   * In visual order from the side the line begins on: left to right in a row, top to bottom in a
   * column (bottom to top for ::GFNT_WRITING_SIDEWAYS_LR, whose lines read upward).
   */
  const GFNT_LineRun * runs;
  size_t run_count;       ///< How many.
  /**
   * Where the line's text begins along the line (its left edge, or its top edge), after
   * alignment.
   */
  GFNT_F26Dot6 offset;
  GFNT_F26Dot6 extent;    ///< The text's length along the line, not counting spaces that hang off the end.
  GFNT_Rect box;          ///< The line box.
  /**
   * The baseline: the y of the alphabetic baseline of a row; the x of the central axis of a
   * column, on which its upright glyphs are centred.
   */
  GFNT_F26Dot6 baseline;
  /**
   * Above the baseline, the greatest of its runs' faces; in a column half the thickness, on
   * either side of the central axis.
   */
  GFNT_F26Dot6 ascent;
  GFNT_F26Dot6 descent;   ///< Below it, positive.
  bool truncated;         ///< True when text was cut from its end and a mark may follow.
  bool justified;         ///< True when its spaces were widened to fill the room.
  GFNT_Rect ink;          ///< The box of the glyphs' outlines; all zero for a line with no ink.
} GFNT_Line;

/** @brief A paragraph laid out at one width. Free with ::gfnt_layout_free(). */
typedef struct GFNT_Layout {
  GFNT_Line * lines;     ///< The lines in reading order: top to bottom, or by writing mode.
  size_t line_count;     ///< How many; at least one.
  GFNT_WritingMode writing_mode; ///< What the paragraph was made with.
  /**
   * The typographic box. For a row layout the width is the room laid out to (or the widest line
   * when unbounded) and the height the lines stacked. For a column layout the height is the room
   * (or the longest line) and the width the columns side by side.
   */
  GFNT_F26Dot6 width;
  GFNT_F26Dot6 height;
  bool truncated;        ///< True when text was dropped by the line limit or the width.
  /**
   * The ink box: the union of the lines' ink, from the glyphs' outline bounds at
   * their placed positions. It differs from the typographic box (width by
   * height) for nearly every text. All zero when nothing has ink.
   */
  GFNT_Rect ink;
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
 * Lines break at UAX #14 opportunities, greedily, and at the hyphenation points the paragraph
 * has (soft hyphens, and a ::GFNT_Hyphenator's), where the hyphen counts toward the width. A line
 * whose first unbreakable stretch is wider than the width overflows rather than splitting a word
 * where it has no point to split it at.
 * Spaces at the end of a line hang: they are placed but not counted in the
 * line's width. The paragraph is not changed and can be laid out again.
 *
 * @param width The room along the lines in 26.6 pixels (their width; their height in vertical
 *   text), or ::GFNT_LAYOUT_UNBOUNDED.
 * @param out Receives the layout; written only on success.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_paragraph_layout(const GFNT_Paragraph * paragraph,
    GFNT_F26Dot6 width, GFNT_Layout * out, GFNT_Error * error);

/** @brief Release a layout. A zeroed one, or one already freed, is harmless. */
GFNT_API void gfnt_layout_free(GFNT_Layout * layout);

/**
 * @brief The narrowest width the paragraph can be laid out to without a line
 *   being wider than the width: its widest unbreakable stretch.
 *
 * Spaces that would hang at a break are not counted. It is what CSS calls the
 * min-content width, and no line of any layout is wider than it unless the
 * line holds one such stretch.
 *
 * @param out Receives the width in 26.6 pixels. Written only on success.
 * @return ::GFNT_OK or ::GFNT_ERR_INVALID.
 */
GFNT_API GFNT_Result gfnt_paragraph_min_content(const GFNT_Paragraph * paragraph,
    GFNT_F26Dot6 * out);

/**
 * @brief The width of the widest line when only newline characters break: the
 *   max-content width. A layout to any narrower width is no wider than this.
 */
GFNT_API GFNT_Result gfnt_paragraph_max_content(const GFNT_Paragraph * paragraph,
    GFNT_F26Dot6 * out);

/**
 * @brief Which side of a boundary between two characters a caret belongs to.
 *
 * It matters where one logical offset has two visual places: at a soft line
 * break (the end of one line and the start of the next) and between a
 * left-to-right and a right-to-left run.
 */
typedef enum GFNT_Affinity {
  GFNT_AFFINITY_DOWNSTREAM = 0, ///< Attached to the character after the offset.
  GFNT_AFFINITY_UPSTREAM        ///< Attached to the character before it.
} GFNT_Affinity;

/** @brief What a point in a laid-out paragraph is on. */
typedef struct GFNT_Hit {
  size_t offset;          ///< A byte offset on a character boundary, in the text.
  size_t line;            ///< The line.
  GFNT_Affinity affinity; ///< The caret's side, so ::gfnt_layout_caret() returns the point.
} GFNT_Hit;

/**
 * @brief Where a caret is drawn: a bar across the line's thickness.
 *
 * In a row it is a vertical bar at @p x from @p y down @p length; in a column it is a horizontal
 * bar at @p y from @p x across @p length.
 */
typedef struct GFNT_Caret {
  size_t line;          ///< The line.
  GFNT_F26Dot6 x;       ///< The bar's x (its left end, for a horizontal bar).
  GFNT_F26Dot6 y;       ///< The bar's y (its top end, for a vertical bar).
  GFNT_F26Dot6 length;  ///< The line's thickness.
  bool horizontal;      ///< True in a column layout.
} GFNT_Caret;

/**
 * @brief The byte offset nearest a point.
 *
 * The line is the one whose box holds @p y (the first or last for a point above
 * or below them); in it the offset is the nearer edge of the character under
 * @p x, or the end of the line for a point past it. Characters inside a ligature
 * are told apart: the glyph is divided at the font's `GDEF` ligature carets when
 * it has one per component, and evenly when not. A newline is never a place to
 * land after.
 *
 * @param paragraph The paragraph @p layout was made from.
 * @param layout The layout.
 * @param out Receives the result. Written only on success.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_layout_hit_test(const GFNT_Paragraph * paragraph,
    const GFNT_Layout * layout, GFNT_F26Dot6 x, GFNT_F26Dot6 y, GFNT_Hit * out);

/**
 * @brief Where the caret for a byte offset is.
 *
 * @param offset A byte offset; one past the end gives the end of the text, and
 *   one inside a character the character's start.
 * @param affinity Which side of the offset the caret is attached to, where it
 *   has two places.
 */
GFNT_API GFNT_Result gfnt_layout_caret(const GFNT_Paragraph * paragraph,
    const GFNT_Layout * layout, size_t offset, GFNT_Affinity affinity,
    GFNT_Caret * out);

/**
 * @brief The rectangles that cover a range of the text, for a selection.
 *
 * One range gives several rectangles when it spans lines, and also when
 * right-to-left text puts part of it on one side of a left-to-right run and part
 * on the other. Rectangles that touch are merged. Each is as thick as its line.
 *
 * @param start The first byte of the range.
 * @param end One past the last; a range with @p end not after @p start is empty.
 * @param out Receives the rectangles, or is NULL to count only.
 * @param capacity How many @p out holds.
 * @param count Receives how many there are, which may be more than @p capacity.
 */
GFNT_API GFNT_Result gfnt_layout_range_rects(const GFNT_Paragraph * paragraph,
    const GFNT_Layout * layout, size_t start, size_t end, GFNT_Rect * out,
    size_t capacity, size_t * count);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_LAYOUT_H
