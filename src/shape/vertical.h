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
 * What vertical text needs of a font that horizontal text does not: the height a
 * glyph takes (`vmtx`, with `vhea` saying how many entries it has) and the place
 * in the glyph that sits on the pen (`VORG`, or `vmtx`'s top side bearing, or a
 * guess from the font's line). These are HarfBuzz's answers, fallbacks and all.
 */

#ifndef GHOTI_IO_GFNT_VERTICAL_H
#define GHOTI_IO_GFNT_VERTICAL_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * A glyph's advance down the line: negative, because y points up. From `vmtx`;
 * for a font with none, the height of the font's line.
 *
 * @return false if a table the answer needs cannot be read.
 */
bool gfnt_vertical_advance(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, int32_t * out_advance);

/**
 * The point of a glyph that sits on the pen in vertical text: half its horizontal
 * advance across, and `VORG`'s height, or the top of its box less its top side
 * bearing, or a guess from the line.
 */
bool gfnt_vertical_origin(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, int32_t * out_x, int32_t * out_y);

#endif
