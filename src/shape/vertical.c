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
 * See vertical.h. Written to HarfBuzz's behaviour: the `vmtx` table is believed
 * only with a `vhea` to say how long it is, a glyph past the end of the font has
 * an advance of one em, and the origin of a glyph with no `VORG` entry is its top
 * from `vmtx`, or centred in the line if there is no `vmtx`.
 */

#include <ghoti.io/font/metrics.h>
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "../core/fixed.h"
#include "../glyf/glyf.h"
#include "../var/metvar.h"
#include "../var/gvar.h"
#include "fallback.h"
#include "vertical.h"

#define VMTX GFNT_TAG('v', 'm', 't', 'x')
#define VHEA GFNT_TAG('v', 'h', 'e', 'a')
#define VORG GFNT_TAG('V', 'O', 'R', 'G')

/** The number of entries `vmtx` has with an advance, and the reader of `vmtx`. */
static bool vertical_vmtx(const GFNT_Face * face, GFNT_Reader * reader,
    size_t * out_advances) {
  GFNT_Reader vhea;
  uint16_t count = 0;

  if (!gfnt_face_has_table(face, VMTX) || !gfnt_face_has_table(face, VHEA)) {
    return false;
  }
  if (gfnt_face_table_reader(face, VHEA, &vhea, NULL) != GFNT_OK
      || gfnt_reader_u16_at(&vhea, 34, &count) != GFNT_OK
      || gfnt_face_table_reader(face, VMTX, reader, NULL) != GFNT_OK) {
    return false;
  }
  *out_advances = count;
  return count != 0;
}

/** The font's line, ascent less descent, which stands in for a missing `vmtx`. */
static int32_t vertical_line(const GFNT_Face * face,
    const GFNT_Variation * variation, int32_t * out_ascent) {
  GFNT_LineMetrics m;
  uint16_t upem = 1000;

  if (gfnt_face_line_metrics(face, GFNT_LINE_METRICS_FONT, variation, &m, NULL)
      != GFNT_OK) {
    (void)gfnt_face_units_per_em(face, &upem, NULL);
    // HarfBuzz's guess when a font has no line metrics: 80 and 20 per cent.
    m.ascent = (int32_t)(upem * 4 / 5);
    m.descent = -(int32_t)(upem - upem * 4 / 5);
  }
  *out_ascent = m.ascent;
  return m.ascent - m.descent;
}

bool gfnt_vertical_advance(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, int32_t * out_advance) {
  GFNT_Reader vmtx;
  size_t advances;
  uint16_t advance = 0;

  if (!vertical_vmtx(face, &vmtx, &advances)) {
    int32_t ascent;

    *out_advance = -vertical_line(face, variation, &ascent);
    return true;
  }
  // A glyph past the metrics the table has - its long metrics and the bearings
  // after them - has no advance at all.
  if (vmtx.length >= advances * 4
      && glyph >= advances + (vmtx.length - advances * 4) / 2) {
    *out_advance = 0;
    return true;
  }
  if (gfnt_reader_u16_at(&vmtx, (glyph < advances ? glyph : advances - 1) * 4,
          &advance) != GFNT_OK) {
    return false;
  }
  *out_advance = -(int32_t)advance;
  if (variation && variation->count) {
    int64_t delta = 0;
    bool have = false;

    if (gfnt_face_has_table(face, GFNT_TAG_VVAR)) {
      have = gfnt_vvar_delta(face, glyph, GFNT_VVAR_ADVANCE, variation->coords,
          variation->count, &delta, NULL) == GFNT_OK;
    }
    else if (gfnt_face_has_table(face, GFNT_TAG('g', 'l', 'y', 'f'))
        && gfnt_face_has_table(face, GFNT_TAG('g', 'v', 'a', 'r'))) {
      int64_t left = 0;
      int64_t right = 0;
      int64_t top = 0;
      int64_t bottom = 0;

      have = gfnt_glyf_phantom_deltas_all(face, glyph, variation, &left, &right,
          &top, &bottom, NULL) == GFNT_OK;
      // The advance is the drop from the top phantom point to the bottom one.
      delta = top - bottom;
    }
    if (have) {
      *out_advance = gfnt_saturate32(-(int64_t)advance
          - gfnt_round_shift_mode(gfnt_clamp64(delta), GFNT_GVAR_FRACTION_BITS,
              variation->delta_rounding));
    }
  }
  return true;
}

/** `VORG`'s height for a glyph, or false if the font has no such table. */
static bool vertical_vorg(const GFNT_Face * face, uint32_t glyph, int32_t * out) {
  GFNT_Reader r;
  int16_t default_y = 0;
  uint16_t count = 0;
  size_t lo = 0;
  size_t hi;

  if (!gfnt_face_has_table(face, VORG)
      || gfnt_face_table_reader(face, VORG, &r, NULL) != GFNT_OK
      || gfnt_reader_s16_at(&r, 4, &default_y) != GFNT_OK
      || gfnt_reader_u16_at(&r, 6, &count) != GFNT_OK) {
    return false;
  }
  *out = default_y;
  hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    uint16_t index = 0;
    int16_t y = 0;

    if (gfnt_reader_u16_at(&r, 8 + mid * 4, &index) != GFNT_OK
        || gfnt_reader_s16_at(&r, 8 + mid * 4 + 2, &y) != GFNT_OK) {
      return true;
    }
    if (glyph < index) {
      hi = mid;
    }
    else if (glyph > index) {
      lo = mid + 1;
    }
    else {
      *out = y;
      return true;
    }
  }
  return true;
}

bool gfnt_vertical_origin(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, int32_t * out_x, int32_t * out_y) {
  int32_t advance = 0;
  GFNT_Extents extents;
  int32_t ascent = 0;
  int32_t line;

  if (gfnt_shape_advance(face, glyph, variation, &advance) != GFNT_OK) {
    advance = 0;
  }
  *out_x = advance / 2;
  if (vertical_vorg(face, glyph, out_y)) {
    int64_t delta = 0;

    if (variation && variation->count && gfnt_face_has_table(face, GFNT_TAG_VVAR)
        && gfnt_vvar_delta(face, glyph, GFNT_VVAR_ORIGIN, variation->coords,
               variation->count, &delta, NULL) == GFNT_OK) {
      *out_y = gfnt_saturate32((int64_t)*out_y
          + gfnt_round_shift_mode(gfnt_clamp64(delta), GFNT_GVAR_FRACTION_BITS,
              variation->delta_rounding));
    }
    return true;
  }
  // At a location a `glyf` font's origin is its top phantom point: the box's top
  // as the glyph states it (not as the location moves it) and what `gvar` adds.
  if (variation && variation->count
      && !gfnt_face_has_table(face, GFNT_TAG_VVAR)
      && gfnt_face_has_table(face, GFNT_TAG('g', 'l', 'y', 'f'))) {
    GFNT_Box box;
    int64_t left = 0;
    int64_t right = 0;
    int64_t top = 0;

    if (gfnt_glyf_stated_box(face, glyph, &box, NULL) == GFNT_OK
        // With no `gvar` nothing moves the top point.
        && (!gfnt_face_has_table(face, GFNT_TAG('g', 'v', 'a', 'r'))
            || gfnt_glyf_phantom_deltas_all(face, glyph, variation, &left, &right,
                   &top, NULL, NULL) == GFNT_OK)) {
      GFNT_Reader vmtx;
      size_t advances;
      int16_t tsb = 0;

      if (vertical_vmtx(face, &vmtx, &advances)) {
        size_t offset = glyph < advances ? glyph * 4 + 2
            : advances * 4 + (glyph - advances) * 2;

        if (gfnt_reader_s16_at(&vmtx, offset, &tsb) != GFNT_OK) {
          tsb = 0;
        }
      }
      *out_y = (int32_t)((gfnt_box_is_empty(&box) ? 0 : box.y_max / GFNT_F26DOT6_ONE) + tsb
          + ((top + (1 << (GFNT_GVAR_FRACTION_BITS - 1)))
              >> GFNT_GVAR_FRACTION_BITS));
      return true;
    }
  }
  line = vertical_line(face, variation, &ascent);
  if (gfnt_face_has_table(face, GFNT_TAG('g', 'l', 'y', 'f'))
      && gfnt_glyph_extents(face, glyph, variation, &extents)) {
    GFNT_Reader vmtx;
    size_t advances;

    if (vertical_vmtx(face, &vmtx, &advances)) {
      // The top of the box, less the top side bearing `vmtx` states.
      size_t offset = glyph < advances ? glyph * 4 + 2
          : advances * 4 + (glyph - advances) * 2;
      int16_t tsb = 0;

      if (gfnt_reader_s16_at(&vmtx, offset, &tsb) == GFNT_OK) {
        int64_t delta = 0;
        int32_t bearing = tsb;

        // At a location `VVAR` moves the top side bearing, rounded to whole units.
        if (variation && variation->count
            && gfnt_face_has_table(face, GFNT_TAG_VVAR)
            && gfnt_vvar_delta(face, glyph, GFNT_VVAR_TSB, variation->coords,
                   variation->count, &delta, NULL) == GFNT_OK) {
          bearing = gfnt_saturate32((int64_t)tsb
              + gfnt_round_shift_mode(gfnt_clamp64(delta),
                  GFNT_GVAR_FRACTION_BITS, variation->delta_rounding));
        }
        *out_y = extents.y_bearing + bearing;
        return true;
      }
    }
    // With no `vmtx`, the glyph is centred in the line.
    *out_y = extents.y_bearing + ((line + extents.height) >> 1);
    return true;
  }
  *out_y = ascent;
  return true;
}
