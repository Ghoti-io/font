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
 * Apple's lookup tables, the glyph-keyed table `morx` and `kerx` share.
 */

#include "layout.h"
#include "../sfnt/sfnt.h"

static uint32_t value_at(const GFNT_Reader * r, size_t at, unsigned size,
    bool * bad) {
  return size == 4 ? gfnt_lr_u32(r, at, bad) : gfnt_lr_u16(r, at, bad);
}

bool gfnt_aat_lookup(const GFNT_Reader * r, size_t lt, size_t num_glyphs,
    uint32_t glyph, unsigned size, uint32_t * out, bool * bad) {
  uint16_t format = gfnt_lr_u16(r, lt, bad);

  if (*bad) {
    return false;
  }
  switch (format) {
    case 0:
      if (glyph >= num_glyphs) {
        return false;
      }
      *out = value_at(r, lt + 2 + size * (size_t)glyph, size, bad);
      return !*bad;
    case 2:
    case 4:
    case 6: {
      size_t lo = 0;
      size_t unit = gfnt_lr_u16(r, lt + 2, bad);
      size_t count = gfnt_lr_u16(r, lt + 4, bad);

      if (unit < 4 || *bad) {
        return false;
      }
      while (lo < count) {
        size_t mid = lo + (count - lo) / 2;
        size_t at = lt + 12 + mid * unit;

        if (format == 6) {
          uint32_t g = gfnt_lr_u16(r, at, bad);

          if (glyph < g) {
            count = mid;
          }
          else if (glyph > g) {
            lo = mid + 1;
          }
          else {
            *out = value_at(r, at + 2, size, bad);
            return !*bad;
          }
        }
        else {
          uint32_t last = gfnt_lr_u16(r, at, bad);
          uint32_t first = gfnt_lr_u16(r, at + 2, bad);

          if (glyph < first) {
            count = mid;
          }
          else if (glyph > last) {
            lo = mid + 1;
          }
          else if (format == 2) {
            *out = value_at(r, at + 4, size, bad);
            return !*bad;
          }
          else {
            *out = value_at(r, lt + gfnt_lr_u16(r, at + 4, bad)
                + size * (size_t)(glyph - first), size, bad);
            return !*bad;
          }
        }
        if (*bad) {
          return false;
        }
      }
      return false;
    }
    case 8: {
      uint32_t first = gfnt_lr_u16(r, lt + 2, bad);
      uint32_t n = gfnt_lr_u16(r, lt + 4, bad);

      if (glyph < first || glyph - first >= n) {
        return false;
      }
      *out = value_at(r, lt + 6 + size * (size_t)(glyph - first), size, bad);
      return !*bad;
    }
    // Format 10 is not read: HarfBuzz does not either.
    default:
      return false;
  }
}
