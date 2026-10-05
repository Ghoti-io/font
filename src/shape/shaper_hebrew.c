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
 * The Hebrew shaper: HarfBuzz's `hb-ot-shape-complex-hebrew.cc`.
 *
 * Two things set Hebrew apart. A font made before OpenType layout expects the
 * *presentation forms* - a letter with its dagesh already joined, at U+FB30 - so a
 * letter and a point are composed into one where Unicode's own composition says no,
 * as long as the font has no `mark` feature to place the point itself. And a font's
 * `GPOS` is believed only if it names the Hebrew script: one that offers positioning
 * under `DFLT` or `latn` alone was not made for Hebrew, and its marks are placed by
 * the shaper instead.
 */

#include <ghoti.io/unicode/norm.h>
#include "plan.h"

/** The letters with a dagesh form, U+05D0 to U+05EA; 0 where there is none. */
static const uint16_t gfnt_dagesh_forms[0x05EA - 0x05D0 + 1] = {
  0xFB30, 0xFB31, 0xFB32, 0xFB33, 0xFB34, 0xFB35, 0xFB36, 0x0000,
  0xFB38, 0xFB39, 0xFB3A, 0xFB3B, 0xFB3C, 0x0000, 0xFB3E, 0x0000,
  0xFB40, 0xFB41, 0x0000, 0xFB43, 0xFB44, 0x0000, 0xFB46, 0xFB47,
  0xFB48, 0xFB49, 0xFB4A,
};

static bool gfnt_hebrew_compose(uint32_t a, uint32_t b, uint32_t * ab,
    void * ctx) {
  const GFNT_ShapeCtx * shape = ctx;
  bool found;

  *ab = guni_compose(a, b);
  found = *ab != 0;
  if (found || (shape && shape->plan->has_mark_feature)) {
    return found;
  }
  switch (b) {
    case 0x05B4:  // HIRIQ
      if (a == 0x05D9) {  // YOD
        *ab = 0xFB1D;
        found = true;
      }
      break;
    case 0x05B7:  // PATAH
      if (a == 0x05F2) {  // YIDDISH YOD YOD
        *ab = 0xFB1F;
        found = true;
      }
      else if (a == 0x05D0) {  // ALEF
        *ab = 0xFB2E;
        found = true;
      }
      break;
    case 0x05B8:  // QAMATS
      if (a == 0x05D0) {
        *ab = 0xFB2F;
        found = true;
      }
      break;
    case 0x05B9:  // HOLAM
      if (a == 0x05D5) {  // VAV
        *ab = 0xFB4B;
        found = true;
      }
      break;
    case 0x05BC:  // DAGESH
      if (a >= 0x05D0 && a <= 0x05EA) {
        *ab = gfnt_dagesh_forms[a - 0x05D0];
        found = *ab != 0;
      }
      else if (a == 0xFB2A) {  // SHIN WITH SHIN DOT
        *ab = 0xFB2C;
        found = true;
      }
      else if (a == 0xFB2B) {  // SHIN WITH SIN DOT
        *ab = 0xFB2D;
        found = true;
      }
      break;
    case 0x05BF:  // RAFE
      if (a == 0x05D1) {
        *ab = 0xFB4C;
        found = true;
      }
      else if (a == 0x05DB) {
        *ab = 0xFB4D;
        found = true;
      }
      else if (a == 0x05E4) {
        *ab = 0xFB4E;
        found = true;
      }
      break;
    case 0x05C1:  // SHIN DOT
      if (a == 0x05E9) {  // SHIN
        *ab = 0xFB2A;
        found = true;
      }
      else if (a == 0xFB49) {  // SHIN WITH DAGESH
        *ab = 0xFB2C;
        found = true;
      }
      break;
    case 0x05C2:  // SIN DOT
      if (a == 0x05E9) {
        *ab = 0xFB2B;
        found = true;
      }
      else if (a == 0xFB49) {
        *ab = 0xFB2D;
        found = true;
      }
      break;
    default:
      break;
  }
  return found;
}

static const GFNT_NormHooks gfnt_hebrew_hooks = {
  .compose = gfnt_hebrew_compose,
};

const GFNT_Shaper gfnt_shaper_hebrew = {
  .name = "hebrew",
  .normalization = GFNT_NORM_COMPOSED_DIACRITICS,
  .normalization_hooks = &gfnt_hebrew_hooks,
  .zero_width_marks = 2,
  .fallback_position = true,
  .gpos_tag = GFNT_TAG('h', 'e', 'b', 'r'),
};
