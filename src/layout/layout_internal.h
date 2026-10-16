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
 * The shaped paragraph as layout and hit testing see it. Internal.
 */

#ifndef GHOTI_IO_GFNT_LAYOUT_INTERNAL_H
#define GHOTI_IO_GFNT_LAYOUT_INTERNAL_H

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/layout.h>
#include <ghoti.io/unicode/break.h>

/** One shaped glyph, already in 26.6 pixels. */
typedef struct {
  uint32_t glyph;
  uint32_t cluster;   // byte offset
  size_t unit;        // owning paragraph unit, or unused for synthetic glyphs
  GFNT_F26Dot6 advance, dx, dy;
  bool expand;        // a justification opportunity: the space this stands for
  bool tab;           // a tab: its advance is the distance to the next stop
  bool has_ink;
  GFNT_F26Dot6 ix0, iy0, ix1, iy1;   // the outline's box in pixels from the pen, y up
} PGlyph;

/** What a unit's glyphs were shaped with. */
typedef struct {
  const GFNT_Face * face;
  size_t span;
  size_t face_index;
  uint8_t level;
  uint8_t orient;                      // a GFNT_Orientation: how its glyphs sit
  GFNT_F26Dot6 ascent, descent, gap;   // positive, 26.6
  GFNT_F26Dot6 inline_ascent, inline_descent, half_leading, shift; // line box relative to baseline
  GFNT_F26Dot6 x_height; // parent baseline to x-height, from OS/2 or half ascent fallback
  GFNT_F26Dot6 line_height;
  GFNT_VerticalAlign vertical_align;
  const GFNT_InlineObject * object; // non-NULL for the synthetic U+FFFC run
} PRun;

/** The glyphs of one cluster. */
typedef struct {
  size_t cp_start, cp_end;     // the code points it stands for
  size_t run;                  // index into the paragraph's runs
  size_t g_lo, g_hi;           // its glyphs in the paragraph's glyph array
  GFNT_F26Dot6 width, base_width, ruby_added;
  GFNT_F26Dot6 autospace_after; // CSS inter-script spacing before the next unit, if on this line
  bool space;                  // hangs at the end of a line
  bool opportunity;            // a line may break after it
  bool emergency;              // an overflow-wrap grapheme break may follow it
  bool hyphen;                 // ... and then a hyphen is added to the line
  bool hard;                   // a line must break after it
  bool tab;                    // one tab character
  uint32_t kashida;            // 1 + the kashida point in the gap after this unit, or 0
  bool object;                 // one synthetic glyph stands for a caller-owned object
} Unit;

/** A measured annotation; its text is shaped in a child paragraph at creation. */
typedef struct {
  GFNT_Annotation descriptor;
  char * text;
  GFNT_Paragraph * shaped;
  GFNT_Paragraph * alternate_shaped;
  size_t cp_start, cp_end;
  size_t unit_start, unit_end;
  GFNT_F26Dot6 advance, ascent, descent, break_extra;
  GFNT_F26Dot6 alternate_advance, alternate_ascent, alternate_descent;
} PAnnotation;

/** Where a word can be stretched: the units it is shaped as, and what a tatweel there measures. */
typedef struct {
  size_t unit;                 // the unit after the place (the stroke goes before it)
  size_t win_lo, win_hi;       // the units shaped with the tatweels
  size_t lo, hi;               // the units whose glyphs the stroke replaces
  GFNT_F26Dot6 adv1;           // the advance of one tatweel, to guess how many fit
} KPoint;

/** The most tatweels in a stroke, how far (in units) either way the probe looks, and the most
 * code points it shapes. */
#define KASHIDA_COPIES_MAX 128
#define KASHIDA_REACH 8
#define KASHIDA_PROBE 256

struct GFNT_Paragraph {
  const GFNT_Allocator * allocator;
  GFNT_ParagraphOptions options;
  size_t length;               // code points
  size_t * cp_byte;            // length + 1 byte offsets
  uint32_t * cps;              // the code points
  GFNT_TextStyle * styles;     // one per span, copied
  GFNT_F26Dot6 * ideographic_advance; // first shaped Han advance per style span, for punctuation
  GFNT_ShapeFeature ** owned_features; // transformed copies when codepoint counts change
  size_t span_count;
  uint8_t para_level;
  GFNT_WritingMode mode;
  bool vertical;               // lines are columns
  uint32_t ell[8];             // the truncation mark
  size_t ell_count;
  GFNT_FontProvider provider;
  GFNT_F26Dot6 tab_width;      // 0 when the text has no tab
  PRun * runs;
  size_t run_count;
  PGlyph * glyphs;
  size_t glyph_count;
  Unit * units;
  size_t unit_count;
  PAnnotation * annotations;
  size_t annotation_count;
  GFNT_F26Dot6 * hyphen_width; // per span: the hyphen's width, or 0 when no point is in the span
  uint32_t * hyphen_cp;        // per span: the character it is set with
  size_t hyphen_glyphs;        // the most glyphs any span's hyphen shapes to
  KPoint * kpoints;            // for ::GFNT_JUSTIFY_KASHIDA
  size_t kcount;
  GFNT_F26Dot6 kmin_adv;       // the shortest tatweel among the points
  PRun fallback_metrics;       // the first span's first face, for an empty text
};

int32_t gfnt_phrase_japanese_boundary_score(const uint32_t * cps, size_t cp_count,
    size_t line_start, size_t boundary, size_t fit_end, GFNT_F26Dot6 line_width);

/* ----- Shared by the layout files (moved out of layout.c) ----- */

#define ALLOC(p, n) (allocator->calloc_fn(allocator->ctx, (n) ? (n) : 1, sizeof *(p)))

#define PROVIDER_MAX 8
#define ELL_GLYPHS 16

typedef enum TextSpacingPunctuation {
  TEXT_SPACING_NONE,
  TEXT_SPACING_OPEN,
  TEXT_SPACING_CLOSE,
  TEXT_SPACING_MIDDLE
} TextSpacingPunctuation;

/** What drawing a stroke of some tatweels at a point makes of the glyphs around it. */
typedef struct {
  size_t g_lo, g_hi;           // the paragraph's glyphs the new ones replace
  size_t count;                // how many new ones
  size_t first;                // where they are in the layout's pool
  GFNT_F26Dot6 growth;         // their advance less that of the glyphs they replace
  GFNT_F26Dot6 adv1;           // the advance a tatweel adds: for guessing how many fit
} KReshape;

/* ------------------------------------------------------------ vertical orientation */

#define ORI_NONE 0
#define ORI_UPRIGHT 1
#define ORI_SIDEWAYS 2

/** A slice of a line: units of one run, at one level. */
typedef struct {
  const PRun * pr;
  const PGlyph * src;
  uint8_t level;
  bool hang;
  bool ellipsis;
  bool hyphen;
  size_t g_lo, g_hi;
  size_t b_lo, b_hi;   // the bytes of text it stands for
  GFNT_F26Dot6 advance;
} Slice;

/**
 * Cut the units into lines. Fills @p starts with where each line begins and @p hyphened with
 * whether it ends at a hyphenation point, so that a hyphen is added. Returns how many lines.
 *
 * @p cand_at and @p cand_fit are scratch for the places a line could break so far, each as many
 * as there are units.
 */
typedef struct {
  size_t end;
  bool hyphen;
} BreakResult;

/** Marks a glyph a stroke's new glyphs replace, other than the first of them. */
#define KASHIDA_SKIP 0xFFFFFFFFu

/** Per-line measurements and inputs shared by placement and alignment. */
typedef struct {
  size_t unit_start, unit_end, hang_from;
  GFNT_F26Dot6 start_offset, available_width, top_y, thickness;
  GFNT_F26Dot6 natural, ascent, descent, gap;
  bool invalid;
} LayoutLineState;

typedef struct {
  const GFNT_Paragraph * paragraph;
  Slice * slices;
  size_t * order;
  GFNT_LineRun * runs;
  size_t * run_used;
  GFNT_PlacedGlyph * placed;
  size_t * glyph_used;
  uint8_t * gjf;
  uint32_t * gk;
  KReshape * kres;
  PGlyph * spool;
  GFNT_F26Dot6 * tabadv;
  GFNT_F26Dot6 * ruby_reclaim;
  GFNT_F26Dot6 * spacing_trim;
  GFNT_F26Dot6 extra, extra_rem, cursor, lead;
  size_t unit_start, unit_end;
  size_t expanded;
  bool vertical, seen_text, ink_set;
  bool failed;
  GFNT_F26Dot6 line_ascent, line_descent;
  bool inline_mode;
  GFNT_F26Dot6 ink_l, ink_t, ink_r, ink_b;
} PlacementState;

typedef struct {
  const GFNT_Paragraph * paragraph;
  const GFNT_Allocator * allocator;
  uint8_t * gjf;
  uint32_t * gk;
  KReshape * kres;
  PGlyph ** spool;
  size_t * spool_len, * spool_cap;
} JustificationState;

/** Storage and bounded scratch for one complete paragraph layout operation. */
typedef struct {
  const GFNT_Allocator * allocator;
  size_t * starts, * order, * cand_at;
  GFNT_F26Dot6 * band_starts, * band_widths, * line_tops;
  Slice * slices;
  uint8_t * slice_levels, * gjf;
  GFNT_F26Dot6 * ew, * tabadv, * cand_fit;
  GFNT_F26Dot6 * ruby_reclaim, * spacing_trim;
  uint32_t * gk;
  KReshape * kres;
  PGlyph * spool;
  size_t spool_len, spool_cap;
  bool * hyphened;
  GFNT_Line * lines;
  uint8_t * storage;
  GFNT_LineRun * runs;
  GFNT_PlacedGlyph * placed;
  size_t line_count, total_lines, runs_bytes, total_bytes, stroke_room;
  size_t run_used, glyph_used;
  bool more, any_truncated;
  GFNT_F26Dot6 y, widest;
  GFNT_Rect ink;
} LayoutState;

/* textrules.c */
bool gfnt_lay_is_newline(uint32_t cp);
bool gfnt_lay_css_segment_break(uint32_t cp);
bool gfnt_lay_css_collapsible_space(uint32_t cp);
bool gfnt_lay_paragraph_hard_break(const GFNT_Paragraph * p, uint32_t cp);
bool gfnt_lay_word_break_letter(uint32_t cp, GUNI_LineBreakTailoring tailoring);
bool gfnt_lay_keep_all_pair(uint32_t left, uint32_t right, GUNI_LineBreakTailoring tailoring);
bool gfnt_lay_hangs(uint32_t cp);
bool gfnt_lay_is_ideographic(uint32_t cp);
uint32_t gfnt_lay_autospace_base_cp(const GFNT_Paragraph * p, const Unit * unit);
GFNT_F26Dot6 gfnt_lay_autospace_boundary(const GFNT_Paragraph * p,
    const Unit * left, const Unit * right);
bool gfnt_lay_text_spacing_ideograph(uint32_t cp);
TextSpacingPunctuation gfnt_lay_text_spacing_punctuation(const GFNT_Paragraph * p,
    const Unit * unit);
int64_t gfnt_lay_text_spacing_half_ic(const GFNT_Paragraph * p, size_t index);
bool gfnt_lay_text_spacing_open_at_start(const GFNT_Paragraph * p, size_t start);
bool gfnt_lay_text_spacing_trim_open_pair(const GFNT_Paragraph * p, size_t left, size_t right);
bool gfnt_lay_text_spacing_trim_close_pair(const GFNT_Paragraph * p, size_t left, size_t right);

/* break.c */
int64_t gfnt_lay_eff_width_at(const GFNT_Paragraph * p, const Unit * u,
    int64_t pos, int64_t origin);
int64_t gfnt_lay_eff_width(const GFNT_Paragraph * p, const Unit * u, int64_t pos);
int32_t gfnt_lay_phrase_score_at(const GFNT_Paragraph * p, size_t line_start,
    size_t boundary, size_t fit_end, GFNT_F26Dot6 line_width);
BreakResult gfnt_lay_break_one_line(const GFNT_Paragraph * p, size_t start,
    GFNT_F26Dot6 width, GFNT_F26Dot6 tab_origin, size_t * cand_at,
    GFNT_F26Dot6 * cand_fit);
bool gfnt_lay_indent_band(const GFNT_Paragraph * p, size_t line, GFNT_Band * band);
size_t gfnt_lay_break_lines_balanced(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    size_t * starts, bool * hyphened, size_t * cand_at, GFNT_F26Dot6 * cand_fit,
    GFNT_F26Dot6 * band_starts, GFNT_F26Dot6 * band_widths);

/* paragraph.c */
GFNT_Result gfnt_lay_glyph_ink(PGlyph * pg, const GFNT_Face * face,
    const GFNT_TextStyle * style, GFNT_F16Dot16 scale,
    const GFNT_Allocator * allocator);
bool gfnt_lay_face_maps(const GFNT_Face * face, uint32_t cp);
void gfnt_lay_scale_metrics(const GFNT_Face * face, const GFNT_TextStyle * style,
    PRun * run);
bool gfnt_lay_set_inline_metrics(PRun * run, const GFNT_TextStyle * style);

/* annotation.c */
bool gfnt_lay_annotation_tree_valid(const GFNT_Annotation * items, size_t count,
    size_t text_length, size_t parent_start, size_t parent_end, size_t depth);
bool gfnt_lay_annotation_byte_cp(const GFNT_Paragraph * p, size_t byte, size_t * cp);
GFNT_Result gfnt_lay_create_annotations(GFNT_Paragraph * p, const GFNT_Annotation * items,
    size_t count, GFNT_Error * error);
GFNT_F26Dot6 gfnt_lay_ruby_unit_share(const PAnnotation * annotation, size_t unit,
    GFNT_F26Dot6 extra);
GFNT_F26Dot6 gfnt_lay_ruby_right_refund(const GFNT_Paragraph * p, size_t line_start,
    size_t neighbor, const PAnnotation ** group_out, size_t * group_start);

/* kashida_layout.c */
GFNT_Result gfnt_lay_kashida_fit(const GFNT_Paragraph * p, const KPoint * pt, GFNT_F26Dot6 share,
    PGlyph * out, KReshape * k, bool * fits, GFNT_Error * error);
GFNT_Result gfnt_lay_find_kashida(GFNT_Paragraph * p, GFNT_Error * error);

/* hyphenate.c */
GFNT_Result gfnt_lay_mark_hyphens(GFNT_Paragraph * p, const char * text, const size_t * span_cp,
    GFNT_Error * error);

/* orientation.c */
bool gfnt_lay_inherits_orientation(uint32_t cp);
uint8_t gfnt_lay_char_orientation(const GFNT_Paragraph * p, const GFNT_TextStyle * style,
    uint32_t cp);
uint8_t gfnt_lay_string_orientation(const GFNT_Paragraph * p, size_t span, uint32_t cp);
int gfnt_lay_hyphen_orientation(const GFNT_Paragraph * p);

/* transform.c */
GFNT_Result gfnt_lay_transform_case(const uint32_t * source, size_t source_length,
    const size_t * source_bytes, const GFNT_TextSpan * spans, size_t span_count,
    const size_t * source_span_cp, GFNT_WhiteSpace white_space, const GFNT_Allocator * allocator,
    uint32_t ** result_cps, size_t ** result_bytes, size_t * result_length,
    size_t ** result_span_cp, size_t ** result_source_to_layout);

/* annotation_line.c */
void gfnt_lay_free_line_annotation_tree(const GFNT_Allocator * allocator,
    GFNT_LineAnnotation * annotations, size_t count);
GFNT_Result gfnt_lay_shape_default_emphasis(const GFNT_Paragraph * p, const PAnnotation * source,
    uint32_t cp, PAnnotation * shaped, GFNT_Error * error);
GFNT_Result gfnt_lay_add_line_annotations(const GFNT_Paragraph * p, GFNT_Line * lines,
    size_t line_count, GFNT_Error * error);

/* line_measure.c */
GFNT_Result gfnt_lay_shape_string(const GFNT_Paragraph * p, size_t span,
    const uint32_t * text, size_t text_count,
    PGlyph * glyphs, size_t * count, GFNT_F26Dot6 * total, PRun * run, int force,
    GFNT_Error * error);
GFNT_Result gfnt_lay_shape_string_dir(const GFNT_Paragraph * p, size_t span,
    const uint32_t * text, size_t text_count,
    PGlyph * glyphs, size_t * count, GFNT_F26Dot6 * total, PRun * run, int force, int rtl,
    size_t cap, GFNT_Error * error);
bool gfnt_lay_inline_shift(const GFNT_Paragraph * p, const PRun * run,
    GFNT_F26Dot6 line_ascent, GFNT_F26Dot6 line_descent, GFNT_F26Dot6 * out);
LayoutLineState gfnt_lay_measure_line(const GFNT_Paragraph * p, size_t start, size_t end,
    GFNT_F26Dot6 width, GFNT_F26Dot6 top_y, GFNT_F26Dot6 tab_origin, GFNT_F26Dot6 * ew,
    GFNT_F26Dot6 * tabadv, GFNT_F26Dot6 * ruby_reclaim, GFNT_F26Dot6 * spacing_trim);

/* justify.c */
GFNT_Result gfnt_lay_justify_line(JustificationState * state, GFNT_Line * line,
    size_t ua, size_t ub, size_t hang_from, GFNT_F26Dot6 natural, GFNT_F26Dot6 width,
    size_t li, size_t total_lines, bool bounded, bool truncated,
    GFNT_F26Dot6 * extra, GFNT_F26Dot6 * extra_rem, GFNT_Error * error);

/* line_place.c */
void gfnt_lay_place_line(PlacementState * state, GFNT_Line * line, size_t slice_count);
GFNT_Rect gfnt_lay_align_lines(const GFNT_Paragraph * p, GFNT_Line * lines, size_t line_count,
    const GFNT_F26Dot6 * band_starts, const GFNT_F26Dot6 * band_widths,
    GFNT_F26Dot6 default_width, GFNT_F26Dot6 widest, GFNT_F26Dot6 block_extent,
    bool para_rtl, bool vertical);

/* flow_wrap.c */
void gfnt_lay_flow_wrap_optimize(const GFNT_Paragraph * p,
    const GFNT_LineConstraint * constraint, LayoutState * state);
void gfnt_lay_flow_avoid_short_last_line(const GFNT_Paragraph * p,
    const GFNT_LineConstraint * constraint, LayoutState * state);

/* columns.c */
bool gfnt_lay_layout_union_rect(GFNT_Rect * bounds, const GFNT_Rect * rect);

#endif // GHOTI_IO_GFNT_LAYOUT_INTERNAL_H
