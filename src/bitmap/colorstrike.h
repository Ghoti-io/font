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
 * What the color strike readers share with the code that measures a color
 * bitmap glyph (`src/shape/bitmap_extents.c`).
 */

#ifndef GHOTI_IO_GFNT_COLORSTRIKE_H
#define GHOTI_IO_GFNT_COLORSTRIKE_H

#include <ghoti.io/font/bitmap.h>
#include <ghoti.io/font/macros.h>
#include "eblc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A `CBDT` glyph, and the two formats that found it.
 *
 * The formats are here because the shaper's measure reads only some of them (see
 * `bitmap_extents.c`), and the reader does not.
 */
typedef struct GFNT_CbdtGlyph {
  GFNT_GlyphImage image;
  uint16_t index_format; ///< The `CBLC` index subtable format, 1 to 5.
  uint16_t image_format; ///< The `CBDT` image format, 17 to 19.
} GFNT_CbdtGlyph;

/**
 * Find one glyph of one `CBLC` strike.
 *
 * ::gfnt_face_glyph_image() for a caller that has the strike in hand and wants the
 * formats as well.
 */
GFNT_Result gfnt_cbdt_find(const GFNT_Face * face,
    const GFNT_EblcStrike * strike, uint32_t glyph, GFNT_CbdtGlyph * out,
    GFNT_Error * error);

/**
 * Which strike HarfBuzz would draw from at a size: see `bitmap_extents.c`.
 *
 * @param requested The size, or 0 for none (the largest strike answers).
 */
size_t gfnt_bitmap_strike_for(const GFNT_Eblc * cblc, uint32_t requested);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_COLORSTRIKE_H
