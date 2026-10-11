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
 * The box HarfBuzz gives a glyph of a color bitmap font.
 *
 * Found by experiment against `hb-shape --show-extents`
 * (`tools/oracle/color_extents_diff.py` holds it there):
 *
 *  - **The picture's box wins over the outline's and over a COLR paint graph's.**
 *    A glyph with a real `glyf` outline and a bitmap is measured by the bitmap, and
 *    `sbix` comes before `CBDT`.
 *  - **One strike answers for every glyph.** It is chosen by size alone, not per
 *    glyph, and a glyph the chosen strike does not carry is not measured by another
 *    strike that has it; it falls through to the colour paint graph or the outline. The size is the larger of a strike's two ppems; the
 *    strike is the smallest one at least as large as the size asked for, else the
 *    largest, and the first of equals. With no size asked for - which is how a
 *    shaper is run - that is the largest.
 *  - **The box is the picture's metrics scaled from the strike's pixels to the
 *    em**, each axis by its own ppem, rounded to the nearest unit with a half going
 *    up. Height is negative, as everywhere in this box.
 *  - **`sbix` is measured the same way**, with a strike's one ppem for both axes, the
 *    box's bottom edge the record's origin offset (so the top is that plus the PNG's
 *    height), and the size read from the PNG's header: a JPEG, a TIFF, a truncated PNG
 *    and a `dupe` chain of more than eight hops have no box.
 *  - **Only some cells of the table are read.** Index subtable formats 1 and 3, and
 *    image formats 17 and 18. HarfBuzz reads the glyphs of every other cell (index
 *    formats 2, 4 and 5, image format 19) as having no box, and so does this: the
 *    reader (::gfnt_face_glyph_image()) returns those pictures, and the shaper
 *    measures as the shaper it is held to does.
 */

#include <ghoti.io/font/metrics.h>
#include <stdint.h>
#include <string.h>
#include "../bitmap/colorstrike.h"
#include "../bitmap/sbix.h"
#include "fallback.h"

/** floor(a / b) for b > 0. */
static int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;

  return (a % b != 0 && a < 0) ? q - 1 : q;
}

/** @p v pixels at @p ppem, in a font of @p upem units to the em: nearest, halves up. */
static int32_t to_units(int32_t v, uint32_t upem, uint32_t ppem) {
  return (int32_t)floor_div((int64_t)v * 2 * upem + ppem, (int64_t)ppem * 2);
}

/** `sbix`: the PNG's own size and the record's origin, over the strike's one ppem. */
static bool gfnt_sbix_extents(const GFNT_Face * face, uint32_t glyph,
    uint32_t requested_ppem, uint16_t upem, GFNT_Extents * out) {
  size_t strike = gfnt_sbix_strike_for(face, requested_ppem);
  GFNT_GlyphImage image;

  // Only a PNG is measured, and only one whose header states a size: a JPEG or a
  // TIFF has no box here, and neither has a truncated PNG.
  if (gfnt_sbix_find(face, strike, glyph, &image, NULL) != GFNT_OK
      || image.format != GFNT_IMAGE_PNG || image.width == 0 || image.height == 0
      || image.strike.ppem_x == 0) {
    return false;
  }
  out->x_bearing = to_units(image.bearing_x, upem, image.strike.ppem_x);
  out->y_bearing = to_units(image.bearing_y, upem, image.strike.ppem_x);
  out->width = to_units((int32_t)image.width, upem, image.strike.ppem_x);
  out->height = to_units(-(int32_t)image.height, upem, image.strike.ppem_x);
  return true;
}

bool gfnt_bitmap_extents(const GFNT_Face * face, uint32_t glyph,
    uint32_t requested_ppem, GFNT_Extents * out) {
  const GFNT_Eblc * cblc = NULL;
  const GFNT_EblcStrike * strike;
  GFNT_CbdtGlyph found;
  uint16_t upem = 0;

  if (gfnt_face_has_sbix(face)
      && gfnt_face_units_per_em(face, &upem, NULL) == GFNT_OK
      && gfnt_sbix_extents(face, glyph, requested_ppem, upem, out)) {
    return true;
  }
  if (!gfnt_face_has_cblc(face) || gfnt_face_cblc(face, &cblc, NULL) != GFNT_OK
      || cblc->strike_count == 0
      || gfnt_face_units_per_em(face, &upem, NULL) != GFNT_OK) {
    return false;
  }
  strike = &cblc->strikes[gfnt_bitmap_strike_for(cblc, requested_ppem)];
  if (gfnt_cbdt_find(face, strike, glyph, &found, NULL) != GFNT_OK
      || (found.index_format != 1 && found.index_format != 3)
      || (found.image_format != 17 && found.image_format != 18)
      || strike->strike.ppem_x == 0 || strike->strike.ppem_y == 0) {
    return false;
  }
  out->x_bearing = to_units(found.image.bearing_x, upem, strike->strike.ppem_x);
  out->y_bearing = to_units(found.image.bearing_y, upem, strike->strike.ppem_y);
  out->width = to_units((int32_t)found.image.width, upem, strike->strike.ppem_x);
  // The sign goes in before the rounding: a half rounds up, so -687.5 is -687.
  out->height = to_units(-(int32_t)found.image.height, upem, strike->strike.ppem_y);
  return true;
}
