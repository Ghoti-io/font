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
 * **What it does not do, stated rather than approximated:** kashida (a justified line
 * widens its spaces and its ideograph gaps, which is all Chromium and Pango do to Arabic as well),
 * hyphenation data (a caller supplies the dictionary, ::GFNT_Hyphenator), and full CSS Ruby
 * behavior. Ruby and emphasis annotations are supported for shaped over/under tracks; see
 * ::GFNT_Annotation for the supported subset.
 * Per-line constraints use ::gfnt_paragraph_layout_flow(); ::GFNT_Exclusions supplies a built-in
 * callback for one-sided shapes. A caller may describe an atomic U+FFFC inline
 * object with ::GFNT_InlineObject; the library places its synthetic glyph and
 * returns its caller identity, but never paints it.
 * Newline characters (LF, CR, VT, FF, U+0085, U+2028, U+2029) end a line;
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

/** @brief The source of an inline run's line height. Values are nonnegative. */
typedef enum GFNT_LineHeightKind {
  GFNT_LINE_HEIGHT_NORMAL = 0, ///< Use the face's current metrics path.
  GFNT_LINE_HEIGHT_MULTIPLE,   ///< 16.16 multiple of style size (1.0 = 0x10000).
  GFNT_LINE_HEIGHT_LENGTH      ///< 26.6 pixel line height.
} GFNT_LineHeightKind;

/** @brief A tagged line-height value; a zeroed value is normal. */
typedef struct GFNT_LineHeight {
  GFNT_LineHeightKind kind;
  int32_t value; ///< 16.16 for MULTIPLE; 26.6 pixels for LENGTH; ignored for NORMAL.
} GFNT_LineHeight;

/** @brief Baseline alignment of an inline run; zero initialization means BASELINE. */
typedef enum GFNT_VerticalAlign {
  GFNT_VERTICAL_ALIGN_BASELINE = 0,
  GFNT_VERTICAL_ALIGN_SUB,
  GFNT_VERTICAL_ALIGN_SUPER,
  GFNT_VERTICAL_ALIGN_LENGTH,      ///< Signed 26.6 pixels; positive raises in horizontal text.
  GFNT_VERTICAL_ALIGN_PERCENTAGE,  ///< Signed 16.16 of this style's computed line-height.
  GFNT_VERTICAL_ALIGN_TEXT_TOP,
  GFNT_VERTICAL_ALIGN_TEXT_BOTTOM,
  GFNT_VERTICAL_ALIGN_MIDDLE,
  GFNT_VERTICAL_ALIGN_TOP,
  GFNT_VERTICAL_ALIGN_BOTTOM
} GFNT_VerticalAlign;

/**
 * @brief Metrics and caller identity for an atomic U+FFFC inline object, in 26.6 pixels.
 *
 * The descriptor and @p user remain owned by the caller and must outlive the paragraph and its
 * layouts. A true break flag adds a UAX #14 opportunity at that edge; it does not force a break.
 */
typedef struct GFNT_InlineObject {
  GFNT_F26Dot6 advance; ///< Length along the line; zero is allowed.
  GFNT_F26Dot6 ascent;  ///< Distance above the baseline (or half-thickness in vertical text).
  GFNT_F26Dot6 descent; ///< Distance below the baseline (or half-thickness in vertical text).
  GFNT_VerticalAlign align; ///< Baseline alignment, using the same choices as text styles.
  int32_t align_value; ///< Signed 26.6 length or signed 16.16 percentage for align.
  bool break_before; ///< Permit a line break immediately before this object.
  bool break_after;  ///< Permit a line break immediately after this object.
  void * user; ///< Caller-owned object identity, returned on its line run.
} GFNT_InlineObject;

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
  GFNT_LineHeight line_height; ///< Zero is NORMAL; other kinds use nonnegative 16.16 or 26.6 values.
  GFNT_VerticalAlign vertical_align; ///< Zero is BASELINE; selectors with values use the field below.
  int32_t vertical_align_value; ///< Signed 26.6 length or signed 16.16 percentage; ignored otherwise.
  /** Optional object descriptor. Non-NULL only for a span of exactly one U+FFFC. */
  const GFNT_InlineObject * inline_object;
  /** Extra space after U+0020 word separators, in 26.6 pixels (negative tightens it). */
  GFNT_F26Dot6 word_spacing;
} GFNT_TextStyle;

/** @brief A stretch of the text and how it is set. The spans tile the text. */
typedef struct GFNT_TextSpan {
  size_t length;        ///< Its length in bytes; the spans' lengths sum to the text's.
  GFNT_TextStyle style; ///< How it is set.
} GFNT_TextSpan;

/** @brief Kind of annotation attached to a source-text range. */
typedef enum GFNT_AnnotationKind {
  GFNT_ANNOTATION_RUBY = 0,
  GFNT_ANNOTATION_EMPHASIS
} GFNT_AnnotationKind;

/** @brief Side of the base range on which an annotation is placed. */
typedef enum GFNT_AnnotationPosition {
  GFNT_ANNOTATION_OVER = 0,
  GFNT_ANNOTATION_UNDER,
  GFNT_ANNOTATION_INTER_CHARACTER
} GFNT_AnnotationPosition;

/** @brief How annotation advance is aligned to the base units. */
typedef enum GFNT_AnnotationAlign {
  GFNT_ANNOTATION_ALIGN_START = 0,
  GFNT_ANNOTATION_ALIGN_CENTER,
  GFNT_ANNOTATION_ALIGN_SPACE_BETWEEN,
  GFNT_ANNOTATION_ALIGN_SPACE_AROUND
} GFNT_AnnotationAlign;

/** @brief Ruby annotation overhang policy at an unannotated line neighbor. */
typedef enum GFNT_RubyOverhang {
  GFNT_RUBY_OVERHANG_NONE = 0,
  GFNT_RUBY_OVERHANG_AUTO
} GFNT_RubyOverhang;

/** @brief Mark shape used when an emphasis annotation has no literal text. */
typedef enum GFNT_EmphasisMarkKind {
  GFNT_EMPHASIS_MARK_DEFAULT = 0, ///< Filled dot for Latin; filled sesame for CJK.
  GFNT_EMPHASIS_MARK_DOT,
  GFNT_EMPHASIS_MARK_CIRCLE,
  GFNT_EMPHASIS_MARK_DOUBLE_CIRCLE,
  GFNT_EMPHASIS_MARK_TRIANGLE,
  GFNT_EMPHASIS_MARK_SESAME,
  GFNT_EMPHASIS_MARK_STRING
} GFNT_EmphasisMarkKind;

/**
 * @brief Ruby text or emphasis marks attached to a byte range in the paragraph.
 *
 * Ranges use UTF-8 byte offsets and must end on character boundaries. Siblings must be disjoint
 * or have the same base range; children must be strictly contained in their parent's range.
 * A non-empty UTF-8 @p text and its explicit byte length are required for ruby and STRING emphasis marks;
 * preset emphasis marks use a zero length and NULL text. Text and child descriptors are copied
 * during paragraph creation. The style's faces and feature data follow the same lifetime rules
 * as a text span. Annotation run glyph clusters and byte ranges are relative to annotation text;
 * GFNT_LineAnnotation maps them back to the source base range.
 */
typedef struct GFNT_Annotation {
  size_t base_start;
  size_t base_end;
  GFNT_AnnotationKind kind;
  GFNT_AnnotationPosition position; ///< INTER_CHARACTER requires vertical writing mode.
  GFNT_TextStyle style;
  const char * text; ///< UTF-8 ruby text or the mark for each eligible emphasis cluster.
  size_t text_length; ///< Explicit UTF-8 byte length; required for ruby and STRING marks.
  GFNT_AnnotationAlign align;
  bool merge;         ///< Merge compatible adjacent ruby siblings (same style/placement, no children).
  bool group_break;   ///< Make the ruby base indivisible; false leaves its normal break points.
  GFNT_RubyOverhang overhang; ///< Whether ruby may hang over plain neighbors; zero disables it.
  const struct GFNT_Annotation * children;
  size_t child_count;
  GFNT_EmphasisMarkKind mark_kind;
  bool filled; ///< Open circle/triangle/sesame when false; ignored for DOT and STRING.
} GFNT_Annotation;

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

/** @brief How the last line, or a line ended by a hard break, is aligned. */
typedef enum GFNT_AlignLast {
  GFNT_ALIGN_LAST_AUTO = 0, ///< Follow align; justified text starts unless justify_last is set.
  GFNT_ALIGN_LAST_START,
  GFNT_ALIGN_LAST_END,
  GFNT_ALIGN_LAST_LEFT,
  GFNT_ALIGN_LAST_RIGHT,
  GFNT_ALIGN_LAST_CENTER,
  GFNT_ALIGN_LAST_JUSTIFY
} GFNT_AlignLast;

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
  /**
   * Arabic-script words are lengthened inside, by whole tatweels (U+0640) drawn where the word is
   * stretched, and the rest of the slack goes to the spaces. One place per word is chosen by the
   * connection-priority scheme of Internet Explorer 6 (an author's own tatweel, after a seen or
   * sad, before a final letter by class, ...), from Unicode's joining data; ligatures a font
   * must draw whole (lam-alef) are never pulled apart. The word is reshaped with the tatweels in
   * it, so the glyphs are the ones the font makes of the longer word (Amiri's own long tatweel
   * glyphs, and the letters' forms beside them), and the paragraph's glyphs for those letters are
   * replaced. A face with no tatweel, a pair the font ligates and a place whose letters cannot
   * be reproduced fall to the next best place. A stroke is whole tatweels, so a word with room
   * for less than one is left to the spaces and a line with no spaces may fall short by less
   * than one. How the slack is divided is ::GFNT_ParagraphOptions::space_share. Horizontal text
   * only; a vertical paragraph is justified as by ::GFNT_JUSTIFY_INTER_WORD. Chromium's patch for
   * `text-justify: kashida` is the model, and `make check-oracle-kashida` holds this to its
   * published screenshots.
   */
  GFNT_JUSTIFY_KASHIDA,
  GFNT_JUSTIFY_NONE              ///< Not at all: the line is set as if aligned to its start.
} GFNT_Justify;

/** @brief How eagerly a line breaks, after CSS `line-break`. */
typedef enum GFNT_LineBreak {
  GFNT_LINE_BREAK_NORMAL = 0, ///< UAX #14 with the conventional tailoring.
  GFNT_LINE_BREAK_STRICT,     ///< Also keeps small kana and the like with what precedes.
  GFNT_LINE_BREAK_LOOSE,      ///< Allows breaks before small kana and the like.
  GFNT_LINE_BREAK_ANYWHERE    ///< Breaks between any two characters.
} GFNT_LineBreak;

/** @brief Emergency word wrapping, after CSS `overflow-wrap`. */
typedef enum GFNT_OverflowWrap {
  GFNT_OVERFLOW_WRAP_NORMAL = 0, ///< Only the ordinary line-break and hyphenation opportunities.
  GFNT_OVERFLOW_WRAP_ANYWHERE,   ///< Emergency grapheme breaks reduce min-content width.
  GFNT_OVERFLOW_WRAP_BREAK_WORD  ///< Emergency grapheme breaks do not reduce min-content width.
} GFNT_OverflowWrap;

/** @brief How words may break, after CSS `word-break`. */
typedef enum GFNT_WordBreak {
  GFNT_WORD_BREAK_NORMAL = 0,
  GFNT_WORD_BREAK_BREAK_ALL, ///< Emergency breaks between typographic units are ordinary breaks.
  GFNT_WORD_BREAK_KEEP_ALL,  ///< Suppress implicit breaks between adjacent CJK letter units.
  GFNT_WORD_BREAK_BREAK_WORD ///< Legacy CSS alias for normal plus overflow-wrap:anywhere.
} GFNT_WordBreak;

/** @brief Whether preserved text may soft-wrap, after the supported CSS `white-space` values. */
typedef enum GFNT_WhiteSpace {
  GFNT_WHITE_SPACE_PRE_WRAP = 0, ///< Preserve spaces and allow soft wrapping; the existing default.
  GFNT_WHITE_SPACE_PRE,            ///< Preserve spaces and breaks, but prohibit soft wrapping.
  GFNT_WHITE_SPACE_BREAK_SPACES,   ///< Preserve spaces, include trailing spaces, and break after each.
  GFNT_WHITE_SPACE_NORMAL,         ///< Collapse whitespace and allow soft wrapping.
  GFNT_WHITE_SPACE_NOWRAP,         ///< Collapse whitespace and prohibit soft wrapping.
  GFNT_WHITE_SPACE_PRE_LINE        ///< Collapse spaces; preserve segment breaks.
} GFNT_WhiteSpace;

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
  /**
   * For ::GFNT_JUSTIFY_KASHIDA, the part of the slack that goes to the spaces, as 16.16 from 0
   * (all of it to the strokes: the default) to 0x10000 (all of it to the spaces, which is
   * ::GFNT_JUSTIFY_INTER_WORD). CSS `text-kashida-space` is one minus this. What the strokes
   * cannot take (no word with a place on the line, or a stroke at its longest) goes to the spaces,
   * and what the spaces cannot take (none on the line) to the strokes.
   */
  GFNT_F16Dot16 space_share;
  /**
   * Justify the last line of the paragraph, and a line that ends at a newline, as the others
   * (CSS `text-align-last: justify`). Only with ::GFNT_ALIGN_JUSTIFY.
   */
  bool justify_last;
  GFNT_WritingMode writing_mode;   ///< Which way lines run and stack.
  /** How characters sit in a vertical mode; ignored for ::GFNT_WRITING_HORIZONTAL_TB. */
  GFNT_TextOrientation text_orientation;
  /** Include the first span's inline box as a paragraph strut on every line; off by default. */
  bool strut;
  /** Indent the first line in 26.6 pixels; with hanging_indent, indent every later line instead. */
  GFNT_F26Dot6 text_indent;
  bool hanging_indent;
  /** Measure tab stops from the paragraph origin instead of each line's start. */
  bool tabs_from_container;
  const GFNT_Annotation * annotations; ///< Optional nested ruby and emphasis descriptors.
  size_t annotation_count;
  GFNT_AlignLast align_last; ///< CSS `text-align-last`; zero preserves the default `auto` behavior.
  GFNT_OverflowWrap overflow_wrap; ///< CSS `overflow-wrap`; zero preserves `normal`.
  GFNT_WordBreak word_break; ///< CSS `word-break`; zero preserves `normal`.
  GFNT_WhiteSpace white_space; ///< `pre-wrap` by default; supports CSS `pre`, `break-spaces`,
                               ///< `normal`, `nowrap`, and `pre-line` behavior.
} GFNT_ParagraphOptions;

/** @brief The one inline band available to a line at a given block position. */
typedef struct GFNT_Band {
  GFNT_F26Dot6 start;    ///< Start along the line (x in horizontal text, y in vertical text; may be negative).
  GFNT_F26Dot6 width;    ///< Available length along the line; zero means no room at this position.
  GFNT_F26Dot6 next_top; ///< Next block position with room, or <= top when there is none.
} GFNT_Band;

/** @brief Callback that returns the intersection band over a line's block-axis interval. */
typedef GFNT_Result (*GFNT_LineBandFn)(void * ctx, GFNT_F26Dot6 top,
    GFNT_F26Dot6 height, GFNT_Band * out);

/** @brief Per-line constraint callback and context. */
typedef struct GFNT_LineConstraint {
  void * ctx;
  GFNT_LineBandFn band;
} GFNT_LineConstraint;

/** @brief A point in paragraph coordinates, 26.6 pixels, y down. */
typedef struct GFNT_FlowPoint {
  GFNT_F26Dot6 x;
  GFNT_F26Dot6 y;
} GFNT_FlowPoint;

/** @brief The geometry used by the built-in exclusion constraint. */
typedef enum GFNT_ExclusionShape {
  GFNT_EXCLUSION_RECTANGLE = 0,
  GFNT_EXCLUSION_CIRCLE,
  GFNT_EXCLUSION_POLYGON
} GFNT_ExclusionShape;

/** @brief Which side of an exclusion remains available to text. */
typedef enum GFNT_ExclusionSide {
  GFNT_EXCLUSION_TEXT_RIGHT = 0, ///< Exclusion occupies the left side of the band.
  GFNT_EXCLUSION_TEXT_LEFT       ///< Exclusion occupies the right side of the band.
} GFNT_ExclusionSide;

/** @brief One caller-owned exclusion; polygon points remain caller-owned. */
typedef struct GFNT_Exclusion {
  GFNT_ExclusionShape shape;
  GFNT_ExclusionSide side;
  GFNT_F26Dot6 margin; ///< Nonnegative expansion around the shape.
  union {
    GFNT_Rect rectangle;
    struct { GFNT_FlowPoint center; GFNT_F26Dot6 radius; } circle;
    struct { const GFNT_FlowPoint * points; size_t count; } polygon;
  } geometry;
} GFNT_Exclusion;

/**
 * @brief A caller-owned collection and containing inline interval for horizontal writing.
 *
 * The built-in exclusion callback interprets top/height as y coordinates and start/width as x
 * coordinates. Vertical writing modes should use a custom ::GFNT_LineBandFn that maps their
 * physical exclusion geometry to the paragraph's inline and block axes.
 */
typedef struct GFNT_Exclusions {
  const GFNT_Exclusion * items;
  size_t count;
  GFNT_F26Dot6 start;
  GFNT_F26Dot6 width;
} GFNT_Exclusions;

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
  void * object; ///< Caller identity for an inline object; NULL for text. Object pixels are not ink.
} GFNT_LineRun;

/** @brief One positioned annotation on a line, with ordinary runs for painting. */
typedef struct GFNT_LineAnnotation {
  size_t base_start; ///< Base byte range; for children, offsets are in the parent annotation text.
  size_t base_end;
  GFNT_Rect box; ///< Physical bounds of the ruby text or emphasis mark.
  const GFNT_LineRun * runs;
  size_t run_count;
  GFNT_AnnotationKind kind;
  GFNT_AnnotationPosition position;
  size_t annotation_index; ///< Index in the descriptor list for this paragraph (or parent track).
  const struct GFNT_LineAnnotation * children; ///< Nested annotation tracks, in descriptor order.
  size_t child_count;
} GFNT_LineAnnotation;

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
  const GFNT_LineAnnotation * annotations; ///< Ruby and emphasis tracks, in source order.
  size_t annotation_count;
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

/** @brief How a fixed set of columns is filled. */
typedef enum GFNT_ColumnFill {
  GFNT_COLUMN_FILL_BALANCE = 0, ///< Balance all columns as evenly as possible; CSS initial value.
  GFNT_COLUMN_FILL_AUTO         ///< Fill each column sequentially to its block extent.
} GFNT_ColumnFill;

/** @brief Policy for a line taller than an otherwise empty column. */
typedef enum GFNT_ColumnOverflow {
  GFNT_COLUMN_OVERFLOW_PROGRESS = 0, ///< Place the line and overflow so content always advances.
  GFNT_COLUMN_OVERFLOW_ERROR         ///< Return ::GFNT_ERR_LIMIT instead of overflowing the line.
} GFNT_ColumnOverflow;

/** @brief Constraints for laying a paragraph into a fixed set of equal columns. */
typedef struct GFNT_ColumnOptions {
  GFNT_F26Dot6 inline_size; ///< Width in horizontal writing; height in vertical writing.
  GFNT_F26Dot6 inline_gap; ///< Gap between columns, zero when omitted.
  GFNT_F26Dot6 block_extent; ///< Height in horizontal writing; width in vertical writing.
  size_t count; ///< Number of columns.
  GFNT_ColumnFill fill; ///< Zero selects balanced filling, the CSS initial value.
  GFNT_ColumnOverflow overflow; ///< Zero guarantees progress through over-tall lines.
  uint32_t orphans; ///< Minimum lines before a break; zero selects the CSS initial value, 2.
  uint32_t widows; ///< Minimum lines after a break; zero selects the CSS initial value, 2.
} GFNT_ColumnOptions;

/** @brief A paragraph placed into columns. Free with ::gfnt_column_layout_free(). */
typedef struct GFNT_ColumnLayout {
  GFNT_Layout layout; ///< All placed lines in reading order, in physical column positions.
  size_t column_count;
  /** `column_count + 1` indexes into layout.lines; horizontal RTL starts at the rightmost column. */
  const size_t * column_line_starts;
  size_t next_byte; ///< Resume offset, or the paragraph length when complete.
  bool complete; ///< True when all paragraph text fits in the requested columns.
  void * storage_; ///< Internal: column_line_starts allocation.
} GFNT_ColumnLayout;

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

/**
 * @brief Lay out a paragraph across a fixed set of equal columns.
 *
 * Lines are first broken at @p inline_size, then assigned to columns whose block extent is
 * @p block_extent and separated by @p inline_gap (zero by default). In vertical writing, line
 * boxes keep their block-axis progression and are aligned to the requested block extent.
 * BALANCE (the zero/default
 * value) minimizes the maximum column height when all text fits; AUTO fills columns sequentially.
 * `orphans` and `widows` default to two. If a line is taller than an empty column and would be
 * placed in the result, the default places it and overflows to guarantee progress; the ERROR
 * policy returns ::GFNT_ERR_LIMIT. The output layout uses physical coordinates and reports each
 * column's line range in `column_line_starts`; `next_byte` resumes incomplete output. A
 * `max_lines` paragraph is accepted if it is not truncated; a truncated layout is rejected
 * because it cannot provide a continuation offset.
 * @param out Receives the result; written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID for invalid options or truncated input,
 *   ::GFNT_ERR_LIMIT for an over-tall placed line under the ERROR policy, or
 *   ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_paragraph_layout_columns(const GFNT_Paragraph * paragraph,
    const GFNT_ColumnOptions * options, GFNT_ColumnLayout * out, GFNT_Error * error);

/** @brief Release a column layout; a zeroed one, or one already freed, is harmless. */
GFNT_API void gfnt_column_layout_free(GFNT_ColumnLayout * layout);

/**
 * @brief Lay out a paragraph against a callback-provided band for each line.
 *
 * The callback is called first with height 0, then with the measured line height. If the
 * intersection changes, the line is broken and measured again. A stable band is required within
 * 2 * unit_count + 16 callback attempts per layout. When the first unbreakable stretch does not
 * fit, a later next_top moves the line down; without a later position it overflows, matching
 * ::gfnt_paragraph_layout(). A zero-width band with no next_top also overflows one stretch to
 * ensure progress. Callback errors propagate. Negative starts are allowed; widths must be
 * nonnegative and the band end must fit in the 26.6 coordinate range.
 */
GFNT_API GFNT_Result gfnt_paragraph_layout_flow(const GFNT_Paragraph * paragraph,
    const GFNT_LineConstraint * constraint, GFNT_Layout * out, GFNT_Error * error);

/** @brief Callback for a ::GFNT_Exclusions value, usable in a line constraint. */
GFNT_API GFNT_Result gfnt_exclusions_band(void * ctx, GFNT_F26Dot6 top,
    GFNT_F26Dot6 height, GFNT_Band * out);

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
 * on the other. Rectangles that touch are merged. Each is as thick as its line;
 * when the selected byte range intersects an annotation, its rectangle includes
 * the annotation's contribution to the line box.
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
