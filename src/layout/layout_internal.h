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
  bool phrase_preferred;       // preferred linguistic boundary for word-break:auto-phrase
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

GFNT_Result gfnt_phrase_japanese_boundaries(const uint32_t * cps, size_t cp_count,
    const GFNT_Allocator * allocator, bool ** out);

#endif // GHOTI_IO_GFNT_LAYOUT_INTERNAL_H
