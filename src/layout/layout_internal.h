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
} PRun;

/** The glyphs of one cluster. */
typedef struct {
  size_t cp_start, cp_end;     // the code points it stands for
  size_t run;                  // index into the paragraph's runs
  size_t g_lo, g_hi;           // its glyphs in the paragraph's glyph array
  GFNT_F26Dot6 width;
  bool space;                  // hangs at the end of a line
  bool opportunity;            // a line may break after it
  bool hyphen;                 // ... and then a hyphen is added to the line
  bool hard;                   // a line must break after it
  bool tab;                    // one tab character
} Unit;

struct GFNT_Paragraph {
  const GFNT_Allocator * allocator;
  GFNT_ParagraphOptions options;
  size_t length;               // code points
  size_t * cp_byte;            // length + 1 byte offsets
  uint32_t * cps;              // the code points
  GFNT_TextStyle * styles;     // one per span, copied
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
  GFNT_F26Dot6 * hyphen_width; // per span: the hyphen's width, or 0 when no point is in the span
  uint32_t * hyphen_cp;        // per span: the character it is set with
  PRun fallback_metrics;       // the first span's first face, for an empty text
};

#endif // GHOTI_IO_GFNT_LAYOUT_INTERNAL_H
