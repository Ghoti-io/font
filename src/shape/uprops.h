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
 * What the shaper asks of a code point, answered from `unicode` and settled into
 * the glyph record once, so that normalisation, clustering, joining and the
 * lookups' skipping all read one answer.
 *
 * The rules are HarfBuzz's `hb_set_unicode_props()`: a mark (Mn, Mc, Me) is a
 * *continuation* of the grapheme before it, and so is a ZWJ, an emoji modifier,
 * a tag character and a half-width katakana voiced mark; a default-ignorable is
 * *hidden* from the lookups' skipping when it is one a context must be able to
 * see (CGJ, a Mongolian free variation selector, a tag).
 */

#ifndef GHOTI_IO_GFNT_UPROPS_H
#define GHOTI_IO_GFNT_UPROPS_H

#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stdint.h>
#include "../layout_tables/layout.h"

/**
 * How a space the font has no glyph for is drawn: HarfBuzz's `space_t`. The
 * em fractions are their own divisors, which is how they are sized.
 */
enum {
  GFNT_SPACE_NONE = 0,
  GFNT_SPACE_EM = 1,
  GFNT_SPACE_EM_2 = 2,
  GFNT_SPACE_EM_3 = 3,
  GFNT_SPACE_EM_4 = 4,
  GFNT_SPACE_EM_5 = 5,
  GFNT_SPACE_EM_6 = 6,
  GFNT_SPACE_EM_16 = 16,
  GFNT_SPACE_4_EM_18 = 17,     ///< Four eighteenths of an em.
  GFNT_SPACE = 18,             ///< The font's own space, unchanged.
  GFNT_SPACE_FIGURE = 19,      ///< As wide as a digit.
  GFNT_SPACE_PUNCTUATION = 20, ///< As wide as a full stop or comma.
  GFNT_SPACE_NARROW = 21       ///< Half the space's own width.
};

/** Whether a code point is default-ignorable, as HarfBuzz's table has it. */
bool gfnt_u_default_ignorable(uint32_t u);

/** The modified combining class: ccc, rearranged so that marks sort as HarfBuzz does. */
uint8_t gfnt_u_modified_ccc(uint32_t u);

/**
 * How a space character is drawn when the font lacks it. Not part of
 * ::gfnt_u_set_props(): a glyph carries it only once it has been drawn that way.
 */
uint8_t gfnt_u_space_type(uint32_t u);

/**
 * Fill every property of @p info from `info->unicode`.
 *
 * Leaves the glyph, cluster, mask and ligature fields alone.
 */
void gfnt_u_set_props(GFNT_LInfo * info);

#endif
