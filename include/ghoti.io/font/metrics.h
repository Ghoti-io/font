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
 * The metric tables - `head`, `maxp`, `hhea`, `hmtx`, `OS/2`, `post` - and the
 * numbers a text stack asks a face for.
 *
 * documentation/design.md section 7.2, with two conventions that hold across
 * the whole API and are stated once here:
 *
 * - **Font units, y-up, ascent positive, descent negative** (section 5.5, M18).
 *   Every table spells the sign differently - `hhea`'s descender is negative,
 *   `OS/2`'s `usWinDescent` is positive, CFF's `FontBBox` is y-up - and every
 *   one of them is converted on the way out, so that a caller adding an ascent
 *   to a descent never has to ask which table answered.
 * - **Which ascent** is a named policy with a documented default, not whichever
 *   table the first platform happened to read (M4). ::GFNT_LineMetricsPolicy
 *   is that enum and zero is the font's own request.
 *
 * Every accessor takes a `const GFNT_Variation *`; NULL means the default
 * instance. A variation that moves nothing - every coordinate zero - is the
 * default instance too and is answered as one. At any other location what moves a
 * metric is `HVAR` (an advance, a left bearing), `MVAR` (a line's extent) or, in a
 * font with no `HVAR`, `gvar`'s phantom points (an advance); **where none of them
 * says, the call is refused as ::GFNT_ERR_UNSUPPORTED** and not answered with the
 * default's number, which would lay text out at weight 900 with the advances of
 * weight 400 and report success. A font with an `MVAR` that does not list a line
 * metric has no change in it, which is the specification's statement and not a
 * refusal. More coordinates than the face has axes is ::GFNT_ERR_INVALID (section
 * 7.7).
 */

#ifndef GHOTI_IO_GFNT_METRICS_H
#define GHOTI_IO_GFNT_METRICS_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The `head` table: the em square, the bounding box, and the two
 * formats a glyph reader needs.
 *
 * Reference: OpenType Specification 1.9, "head - Font Header Table".
 */
typedef struct GFNT_Head {
  uint16_t major_version;         ///< 1 for every font that exists.
  uint16_t minor_version;         ///< 0.
  GFNT_F16Dot16 font_revision;    ///< The foundry's own version number.
  uint32_t checksum_adjustment;   ///< The whole font's checksum, as claimed.
  uint16_t flags;                 ///< Baseline-at-zero and its neighbours.
  uint16_t units_per_em;          ///< The em square. 16 to 16384.
  int64_t created;                ///< Seconds since 1904-01-01, signed.
  int64_t modified;               ///< Seconds since 1904-01-01, signed.
  int16_t x_min;                  ///< Bounding box over all glyphs, y-up.
  int16_t y_min;                  ///< Bounding box over all glyphs, y-up.
  int16_t x_max;                  ///< Bounding box over all glyphs, y-up.
  int16_t y_max;                  ///< Bounding box over all glyphs, y-up.
  uint16_t mac_style;             ///< Bold, italic and the rest, as claimed.
  uint16_t lowest_rec_ppem;       ///< Advisory; this library ignores it.
  int16_t font_direction_hint;    ///< Deprecated; kept because the file has it.
  int16_t index_to_loc_format;    ///< 0 for short `loca`, 1 for long.
  int16_t glyph_data_format;      ///< 0 for every font that exists.
} GFNT_Head;

/**
 * @brief The `hhea` table: horizontal line metrics and the `hmtx` count.
 *
 * `descender` is as the file gives it, which is already negative in this table.
 *
 * Reference: OpenType Specification 1.9, "hhea - Horizontal Header Table".
 */
typedef struct GFNT_Hhea {
  uint16_t major_version;        ///< 1.
  uint16_t minor_version;        ///< 0.
  int16_t ascender;              ///< Positive, font units.
  int16_t descender;             ///< Negative, font units.
  int16_t line_gap;              ///< Positive, font units.
  uint16_t advance_width_max;    ///< The widest glyph's advance.
  int16_t min_left_side_bearing; ///< Over all glyphs with contours.
  int16_t min_right_side_bearing;///< Over all glyphs with contours.
  int16_t x_max_extent;          ///< max(lsb + (xMax - xMin)).
  int16_t caret_slope_rise;      ///< 1 for an upright font.
  int16_t caret_slope_run;       ///< 0 for an upright font.
  int16_t caret_offset;          ///< Font units.
  int16_t metric_data_format;    ///< 0.
  uint16_t number_of_h_metrics;  ///< `hmtx` entries; may be short of numGlyphs.
} GFNT_Hhea;

/**
 * @brief The `OS/2` table, versions 0 to 5.
 *
 * Fields a version does not carry are zero, and @p version says which those
 * are: 1 adds the code-page ranges, 2 adds `sxHeight`, `sCapHeight` and the
 * three that follow, 5 adds the optical point sizes.
 *
 * `fs_type` is **reported and never enforced** (documentation/design.md section
 * 17.9): a library that refuses to render a font because of an embedding bit
 * breaks every legitimate viewer, and one that embeds a font into a document
 * without telling the caller breaks the law for them. The consumer that embeds
 * decides.
 *
 * Reference: OpenType Specification 1.9, "OS/2 - OS/2 and Windows Metrics
 * Table".
 */
typedef struct GFNT_Os2 {
  uint16_t version;             ///< 0 to 5.
  int16_t x_avg_char_width;     ///< Font units.
  uint16_t weight_class;        ///< 1 to 1000; 400 is regular.
  uint16_t width_class;         ///< 1 to 9; 5 is medium.
  uint16_t fs_type;             ///< Embedding permissions. Reported only.
  int16_t subscript_x_size;     ///< Font units.
  int16_t subscript_y_size;     ///< Font units.
  int16_t subscript_x_offset;   ///< Font units.
  int16_t subscript_y_offset;   ///< Font units.
  int16_t superscript_x_size;   ///< Font units.
  int16_t superscript_y_size;   ///< Font units.
  int16_t superscript_x_offset; ///< Font units.
  int16_t superscript_y_offset; ///< Font units.
  int16_t strikeout_size;       ///< Font units.
  int16_t strikeout_position;   ///< Font units, y-up.
  int16_t family_class;         ///< IBM family class and subclass.
  uint8_t panose[10];           ///< PANOSE classification, as claimed.
  uint32_t unicode_range[4];    ///< Which blocks the font claims to cover.
  char vendor_id[5];            ///< Four characters and a NUL.
  uint16_t fs_selection;        ///< Bit 7 is USE_TYPO_METRICS.
  uint16_t first_char_index;    ///< Lowest codepoint the `cmap` maps.
  uint16_t last_char_index;     ///< Highest codepoint the `cmap` maps.
  int16_t typo_ascender;        ///< Positive, font units.
  int16_t typo_descender;       ///< Negative, font units.
  int16_t typo_line_gap;        ///< Positive, font units.
  uint16_t win_ascent;          ///< Positive, font units.
  uint16_t win_descent;         ///< Positive **as the file gives it**.
  uint32_t code_page_range[2];  ///< Version 1 and later; else zero.
  int16_t x_height;             ///< Version 2 and later; else zero.
  int16_t cap_height;           ///< Version 2 and later; else zero.
  uint16_t default_char;        ///< Version 2 and later; else zero.
  uint16_t break_char;          ///< Version 2 and later; else zero.
  uint16_t max_context;         ///< Version 2 and later; else zero.
  uint16_t lower_optical_size;  ///< Version 5; else zero.
  uint16_t upper_optical_size;  ///< Version 5; else zero.
} GFNT_Os2;

/** @brief `OS/2` `fsSelection` bit 7: use the typographic metrics. */
#define GFNT_OS2_USE_TYPO_METRICS 0x0080u

/**
 * @brief The `post` table's header fields.
 *
 * Glyph names are not here yet: format 2.0's names are indices into the
 * standard Macintosh order for the first 258 of them, and that table is a
 * vector this library will take from an oracle rather than write from memory
 * (documentation/design.md section 14). @p version says what a font carries.
 *
 * Reference: OpenType Specification 1.9, "post - PostScript Table".
 */
typedef struct GFNT_Post {
  GFNT_F16Dot16 version;        ///< 0x00010000, 0x00020000, 0x00030000.
  GFNT_F16Dot16 italic_angle;   ///< Degrees counter-clockwise from vertical.
  int16_t underline_position;   ///< Font units, y-up.
  int16_t underline_thickness;  ///< Font units.
  uint32_t is_fixed_pitch;      ///< Non-zero if the font is monospaced.
  uint32_t min_mem_type42;      ///< Advisory, as claimed.
  uint32_t max_mem_type42;      ///< Advisory, as claimed.
  uint32_t min_mem_type1;       ///< Advisory, as claimed.
  uint32_t max_mem_type1;       ///< Advisory, as claimed.
} GFNT_Post;

/**
 * @brief Which table answers "how tall is a line".
 *
 * The mistake this enum exists to prevent is M4: Windows read
 * `usWinAscent`/`usWinDescent`, macOS read `hhea`, browsers settled on a
 * compromise, and the same font is a different height in each. There is no
 * right answer, so the answer is named.
 */
typedef enum {
  /**
   * The font's own request, which is what zero should mean: the typographic
   * metrics when `OS/2` `fsSelection` bit 7 is set, `hhea` otherwise, and
   * `OS/2`'s window metrics if there is no `hhea`.
   */
  GFNT_LINE_METRICS_FONT = 0,
  GFNT_LINE_METRICS_TYPO, ///< `OS/2` sTypoAscender and its neighbours.
  GFNT_LINE_METRICS_HHEA, ///< `hhea` ascender, descender and lineGap.
  GFNT_LINE_METRICS_WIN   ///< `OS/2` usWinAscent and usWinDescent.
} GFNT_LineMetricsPolicy;

/**
 * @brief One line's vertical extent, in font units, y-up.
 *
 * @p ascent is positive, @p descent is **negative**, and @p line_gap is
 * positive, whichever table they came from. @p source is which table answered,
 * because a fallback nobody can audit is a bug report waiting to happen.
 */
typedef struct GFNT_LineMetrics {
  int32_t ascent;                  ///< Positive, font units.
  int32_t descent;                 ///< Negative, font units.
  int32_t line_gap;                ///< Positive, font units.
  GFNT_LineMetricsPolicy source;   ///< Which table these came from.
} GFNT_LineMetrics;

/**
 * @brief The `head` table, parsed once and memoised.
 *
 * @param face The face.
 * @param out_head Receives a pointer into the face, valid while the face is.
 *   Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED if the font
 *   has no `head`, or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_head(const GFNT_Face * face,
    const GFNT_Head ** out_head, GFNT_Error * error);

/**
 * @brief The `hhea` table, parsed once and memoised.
 *
 * @param face The face.
 * @param out_hhea Receives a pointer into the face. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED or
 *   ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_hhea(const GFNT_Face * face,
    const GFNT_Hhea ** out_hhea, GFNT_Error * error);

/**
 * @brief The `OS/2` table, parsed once and memoised.
 *
 * @param face The face.
 * @param out_os2 Receives a pointer into the face. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED or
 *   ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_os2(const GFNT_Face * face,
    const GFNT_Os2 ** out_os2, GFNT_Error * error);

/**
 * @brief The `post` table's header fields, parsed once and memoised.
 *
 * @param face The face.
 * @param out_post Receives a pointer into the face. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED or
 *   ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_post(const GFNT_Face * face,
    const GFNT_Post ** out_post, GFNT_Error * error);

/**
 * @brief The em square this face's coordinates are in.
 *
 * @param face The face.
 * @param out_units Receives `unitsPerEm`. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED if the font
 *   has no `head`, or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_units_per_em(const GFNT_Face * face,
    uint16_t * out_units, GFNT_Error * error);

/**
 * @brief How many glyphs this face has.
 *
 * **The minimum across every table that indexes glyphs** (M12): `maxp` says
 * one number, and `hmtx`, `loca` and CFF's `CharStrings` each imply another.
 * Fonts exist where they disagree, and the parser that read past the shortest
 * of them is the defect this rule prevents. A disagreement is reported through
 * ::gfnt_face_num_glyphs_disagreement() rather than being an error, because
 * the font is usable either way.
 *
 * @param face The face.
 * @param out_count Receives the count. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED if the font
 *   has no `maxp`, or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_num_glyphs(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/**
 * @brief Whether the tables disagreed about how many glyphs there are.
 *
 * @param face The face.
 * @param out_maxp Receives what `maxp` claimed, or NULL.
 * @return true if some table implied fewer glyphs than `maxp` claimed, in
 *   which case ::gfnt_face_num_glyphs() reported the smaller number.
 */
GFNT_API bool gfnt_face_num_glyphs_disagreement(const GFNT_Face * face,
    size_t * out_maxp);

/**
 * @brief A glyph's horizontal advance, in font units.
 *
 * `hmtx` may carry fewer entries than the face has glyphs, in which case
 * **the last advance repeats** for every glyph after it - a monospaced font
 * with 3,000 glyphs stores one advance, and a parser that indexes past the
 * array is reading whatever follows it.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param variation The instance, or NULL for the default. At a location other
 *   than the default the advance is `HVAR`'s delta for the glyph added to
 *   `hmtx`'s - or, in a font with no `HVAR`, the difference of the first two
 *   phantom points `gvar` carries - rounded once, a tie going as
 *   ::GFNT_Variation.delta_rounding says (half up by default). A
 *   composite glyph's own row is used, whatever its `USE_MY_METRICS` component
 *   says.
 * @param out_advance Receives the advance in font units. Written only on
 *   success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID for a glyph the face does not have or
 *   a variation with more coordinates than the face has axes,
 *   ::GFNT_ERR_UNSUPPORTED if the font has no `hmtx` or `hhea` - or, at a
 *   location, is a `glyf` font with neither `HVAR` nor `gvar` to say how the
 *   advance moves (a CFF2 font without an `HVAR` says it does not) - or
 *   ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_glyph_advance(const GFNT_Face * face,
    uint32_t glyph, const GFNT_Variation * variation, int32_t * out_advance,
    GFNT_Error * error);

/**
 * @brief A glyph's left side bearing, in font units.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param variation The instance, or NULL for the default. At a location other
 *   than the default the bearing is `hmtx`'s plus `HVAR`'s **left-bearing
 *   mapping**; a font whose `HVAR` has none (which is most of them) is refused,
 *   because its bearing then follows the varied outline and not any table.
 * @param out_bearing Receives the bearing in font units. Written only on
 *   success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED (at a location,
 *   for a font with no bearing mapping) or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_glyph_side_bearing(const GFNT_Face * face,
    uint32_t glyph, const GFNT_Variation * variation, int32_t * out_bearing,
    GFNT_Error * error);

/**
 * @brief One line's vertical extent under a named policy.
 *
 * @param face The face.
 * @param policy Which table to believe; ::GFNT_LINE_METRICS_FONT for the
 *   font's own request.
 * @param variation The instance, or NULL for the default. At a location the
 *   `MVAR` delta of each metric the policy chose is added; a font with no `MVAR`,
 *   or one that does not list a metric, has no change in it, which is a fact
 *   about the font and not a refusal.
 * @param out_metrics Receives the metrics, with @p source naming the table
 *   that answered. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED if the font
 *   has neither of the tables the policy needs, or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_line_metrics(const GFNT_Face * face,
    GFNT_LineMetricsPolicy policy, const GFNT_Variation * variation,
    GFNT_LineMetrics * out_metrics, GFNT_Error * error);

/**
 * @brief Write a `head` table's fields to a stream, one per line.
 *
 * @param head The table.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_head_dump(const GFNT_Head * head, FILE * out);

/**
 * @brief Write an `hhea` table's fields to a stream, one per line.
 *
 * @param hhea The table.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_hhea_dump(const GFNT_Hhea * hhea, FILE * out);

/**
 * @brief Write an `OS/2` table's fields to a stream, one per line.
 *
 * @param os2 The table.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_os2_dump(const GFNT_Os2 * os2, FILE * out);

/**
 * @brief Write a `post` table's fields to a stream, one per line.
 *
 * @param post The table.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_post_dump(const GFNT_Post * post, FILE * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_METRICS_H
