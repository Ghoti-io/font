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
 * Positioning marks when the font does not say where they go: HarfBuzz's
 * `hb-ot-shape-fallback.cc`.
 *
 * A font with no `mark` feature leaves a combining mark where the pen is, a
 * whole advance away from the letter it belongs to. The fallback pulls it back
 * over the base and places it by the mark's combining class: above, below,
 * left or right of the base's box, the way a typesetter does by hand. It needs the
 * glyphs' boxes, so it is the one place in shaping that reads outlines.
 */

#ifndef GHOTI_IO_GFNT_FALLBACK_H
#define GHOTI_IO_GFNT_FALLBACK_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include "../layout_tables/layout.h"

/** A glyph's box: HarfBuzz's `hb_glyph_extents_t`, y up, height negative. */
typedef struct GFNT_Extents {
  int32_t x_bearing;
  int32_t y_bearing;
  int32_t width;
  int32_t height;
} GFNT_Extents;

/**
 * The box of a glyph, in whole font units: HarfBuzz's `get_glyph_extents()`.
 * @return false if the face cannot say.
 */
bool gfnt_glyph_extents(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, GFNT_Extents * out);

/**
 * Position the marks of every cluster of the run.
 *
 * @param adjust_offsets_when_zeroing Whether a mark whose advance is taken away
 *   keeps its place over the glyph before it, which it does when no `GPOS` ran.
 * @param forward Whether the run's own direction is left to right.
 * @param horizontal_ltr The script's native direction, for a ligature's components.
 */
void gfnt_fallback_mark_position(const GFNT_Face * face, GFNT_LBuffer * buf,
    const GFNT_Variation * variation, bool adjust_offsets_when_zeroing,
    bool forward, bool horizontal_ltr);

/** The combining classes a script's marks are positioned as. */
void gfnt_fallback_recategorize_marks(GFNT_LBuffer * buf);

#endif
