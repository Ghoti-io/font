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
 * Fallback mark positioning. See fallback.h.
 */

#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <string.h>
#include "../tables/tables.h"
#include "fallback.h"

// Positional combining classes (UAX #44).
#define GFNT_CCC_ATTACHED_BELOW_LEFT 200
#define GFNT_CCC_ATTACHED_BELOW 202
#define GFNT_CCC_ATTACHED_ABOVE 214
#define GFNT_CCC_ATTACHED_ABOVE_RIGHT 216
#define GFNT_CCC_BELOW_LEFT 218
#define GFNT_CCC_BELOW 220
#define GFNT_CCC_BELOW_RIGHT 222
#define GFNT_CCC_ABOVE_LEFT 228
#define GFNT_CCC_ABOVE 230
#define GFNT_CCC_ABOVE_RIGHT 232
#define GFNT_CCC_DOUBLE_BELOW 233
#define GFNT_CCC_DOUBLE_ABOVE 234

/**
 * The box of a glyph, in whole font units: HarfBuzz's `get_glyph_extents()`.
 *
 * For a `glyf` glyph at the default location it is the header's vertical extent
 * and width, with the left bearing the `hmtx` states and not the header's own
 * `xMin` (they differ when a font keeps the glyph away from its origin). At a
 * location, and for a charstring, it is the box of the control points.
 */
bool gfnt_glyph_extents(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, GFNT_Extents * out) {
  GFNT_Box box;
  GFNT_Outline * outline = NULL;
  GFNT_Result result;
  bool varied = variation && variation->count;
  int32_t lsb = 0;
  bool header = !varied && gfnt_face_has_table(face, GFNT_TAG('g', 'l', 'y', 'f'));

  memset(out, 0, sizeof *out);
  if (header) {
    if (gfnt_face_glyph_stated_box(face, glyph, &box, NULL) != GFNT_OK
        || gfnt_face_glyph_side_bearing(face, glyph, variation, &lsb, NULL)
            != GFNT_OK) {
      return false;
    }
  }
  else {
    // A font with `fvar` and glyf outlines but no `gvar` has nothing that moves its
    // outlines, and HarfBuzz measures them as they are.
    if (gfnt_face_glyph_outline(face, glyph, variation, NULL, &outline, NULL)
            != GFNT_OK
        && (!varied
            || gfnt_face_glyph_outline(face, glyph, NULL, NULL, &outline, NULL)
                != GFNT_OK)) {
      return false;
    }
    result = gfnt_outline_control_box(outline, &box);
    gfnt_outline_destroy(outline);
    if (result != GFNT_OK) {
      return false;
    }
  }
  if (!gfnt_box_is_empty(&box)) {
    // The box is font units times 64. Whole font units, the minimum rounded
    // down and the maximum up.
    // A box that a location has moved is rounded to the nearest unit, a tie going
    // down (HarfBuzz measures it in floats, where a tie is rarely one).
    int32_t up = varied ? 31 : 63;
    int32_t down = varied ? 31 : 0;
    int32_t x_min = (int32_t)((box.x_min + down) >> 6);
    int32_t y_min = (int32_t)((box.y_min + down) >> 6);
    int32_t x_max = (int32_t)((box.x_max + up) >> 6);
    int32_t y_max = (int32_t)((box.y_max + up) >> 6);

    out->x_bearing = header ? lsb : x_min;
    out->y_bearing = y_max;
    out->width = x_max - x_min;
    out->height = y_min - y_max;
  }
  return true;
}

/**
 * A mark's class as a place to put it. The modified combining class gives every
 * script's marks their own number so that they sort; this turns the ones that
 * are not positional back into the positional class they stand for.
 */
static uint8_t gfnt_recategorize(uint8_t klass) {
  if (klass >= 200) {
    return klass;
  }
  switch (klass) {
    case 22:  // sheva
    case 15:  // hataf segol
    case 16:  // hataf patah
    case 17:  // hataf qamats
    case 23:  // hiriq
    case 18:  // tsere
    case 19:  // segol
    case 20:  // patah
    case 21:  // qamats
    case 24:  // qubuts
    case 25:  // meteg
      return GFNT_CCC_BELOW;
    case 13:  // rafe
      return GFNT_CCC_ATTACHED_ABOVE;
    case 10:  // shin dot
      return GFNT_CCC_ABOVE_RIGHT;
    case 11:  // sin dot
    case 14:  // holam
      return GFNT_CCC_ABOVE_LEFT;
    case 26:  // point varika
      return GFNT_CCC_ABOVE;
    case 12:  // dagesh
      return klass;
    // Arabic and Syriac.
    case 28:  // fathatan
    case 29:  // dammatan
    case 31:  // fatha
    case 32:  // damma
    case 27:  // shadda
    case 34:  // sukun
    case 35:  // superscript alef
    case 36:  // Syriac superscript alaph
      return GFNT_CCC_ABOVE;
    case 30:  // kasratan
    case 33:  // kasra
      return GFNT_CCC_BELOW;
    // Thai.
    case 3:
      return GFNT_CCC_BELOW_RIGHT;
    case 107:
      return GFNT_CCC_ABOVE_RIGHT;
    // Lao.
    case 118:
      return GFNT_CCC_BELOW;
    case 122:
      return GFNT_CCC_ABOVE;
    // Tibetan.
    case 129:
      return GFNT_CCC_BELOW;
    case 132:
      return GFNT_CCC_ABOVE;
    case 131:
      return GFNT_CCC_BELOW;
    default:
      return klass;
  }
}

void gfnt_fallback_recategorize_marks(GFNT_LBuffer * buf) {
  size_t i;

  for (i = 0; i < buf->len; i++) {
    GFNT_LInfo * info = &buf->info[i];

    if (info->gc == 17 /* GUNI_GC_NONSPACING_MARK */) {
      info->mcc = gfnt_recategorize(info->mcc);
    }
  }
}

/** Place one mark against the box of what it sits on, and grow the box by it. */
static void gfnt_position_mark(const GFNT_Face * face, GFNT_LBuffer * buf,
    const GFNT_Variation * variation, uint32_t upem, GFNT_Extents * base,
    size_t i, uint8_t klass, int ltr) {
  GFNT_Extents mark;
  GFNT_LPos * pos = &buf->pos[i];
  int32_t y_gap = (int32_t)(upem / 16);

  if (!gfnt_glyph_extents(face, buf->info[i].glyph, variation, &mark)) {
    return;
  }
  pos->x_offset = 0;
  pos->y_offset = 0;

  // Left and right marks are not positioned.
  switch (klass) {
    case GFNT_CCC_DOUBLE_BELOW:
    case GFNT_CCC_DOUBLE_ABOVE:
      // Only in horizontal text: vertical text centres the mark like any other.
      if (ltr > 0) {
        pos->x_offset += base->x_bearing + base->width - mark.width / 2
            - mark.x_bearing;
        break;
      }
      if (ltr < 0) {
        pos->x_offset += base->x_bearing - mark.width / 2 - mark.x_bearing;
        break;
      }
      pos->x_offset += base->x_bearing + (base->width - mark.width) / 2
          - mark.x_bearing;
      break;
    case GFNT_CCC_ATTACHED_BELOW_LEFT:
    case GFNT_CCC_BELOW_LEFT:
    case GFNT_CCC_ABOVE_LEFT:
      pos->x_offset += base->x_bearing - mark.x_bearing;
      break;
    case GFNT_CCC_ATTACHED_ABOVE_RIGHT:
    case GFNT_CCC_BELOW_RIGHT:
    case GFNT_CCC_ABOVE_RIGHT:
      pos->x_offset += base->x_bearing + base->width - mark.width
          - mark.x_bearing;
      break;
    default:
      // Centre it.
      pos->x_offset += base->x_bearing + (base->width - mark.width) / 2
          - mark.x_bearing;
      break;
  }

  switch (klass) {
    case GFNT_CCC_DOUBLE_BELOW:
    case GFNT_CCC_BELOW_LEFT:
    case GFNT_CCC_BELOW:
    case GFNT_CCC_BELOW_RIGHT:
      base->height -= y_gap;
      // fall through
    case GFNT_CCC_ATTACHED_BELOW_LEFT:
    case GFNT_CCC_ATTACHED_BELOW:
      pos->y_offset = base->y_bearing + base->height - mark.y_bearing;
      // Never shift a mark below the base up.
      if ((y_gap > 0) == (pos->y_offset > 0)) {
        base->height -= pos->y_offset;
        pos->y_offset = 0;
      }
      base->height += mark.height;
      break;
    case GFNT_CCC_DOUBLE_ABOVE:
    case GFNT_CCC_ABOVE_LEFT:
    case GFNT_CCC_ABOVE:
    case GFNT_CCC_ABOVE_RIGHT:
      base->y_bearing += y_gap;
      base->height -= y_gap;
      // fall through
    case GFNT_CCC_ATTACHED_ABOVE:
    case GFNT_CCC_ATTACHED_ABOVE_RIGHT:
      pos->y_offset = base->y_bearing - (mark.y_bearing + mark.height);
      // Do not shift a mark above the base down too far.
      if ((y_gap > 0) != (pos->y_offset > 0)) {
        int32_t correction = -pos->y_offset / 2;

        base->y_bearing += correction;
        base->height -= correction;
        pos->y_offset += correction;
      }
      // The box now reaches as high as the mark does.
      base->y_bearing -= mark.height;
      base->height += mark.height;
      break;
    default:
      break;
  }
}

static void gfnt_zero_mark_advances(GFNT_LBuffer * buf, size_t start,
    size_t end, bool adjust) {
  size_t i;

  for (i = start; i < end; i++) {
    if (buf->info[i].gc == 17 /* GUNI_GC_NONSPACING_MARK */) {
      if (adjust) {
        buf->pos[i].x_offset -= buf->pos[i].x_advance;
        buf->pos[i].y_offset -= buf->pos[i].y_advance;
      }
      buf->pos[i].x_advance = 0;
      buf->pos[i].y_advance = 0;
    }
  }
}

static void gfnt_position_around_base(const GFNT_Face * face,
    GFNT_LBuffer * buf, const GFNT_Variation * variation, uint32_t upem,
    size_t base, size_t end, bool adjust, bool forward, bool horizontal_ltr,
    bool vertical) {
  GFNT_LInfo * info = buf->info;
  GFNT_Extents base_extents;
  GFNT_Extents component;
  GFNT_Extents cluster;
  int32_t x_offset = 0;
  int32_t y_offset = 0;
  int32_t advance = 0;
  uint32_t lig_id;
  int32_t num_components;
  int32_t last_component = -1;
  uint8_t last_class = 255;
  size_t i;

  if (!gfnt_glyph_extents(face, info[base].glyph, variation, &base_extents)) {
    // No box to place against: the marks are zeroed and left where they fall.
    gfnt_zero_mark_advances(buf, base + 1, end, adjust);
    return;
  }
  base_extents.y_bearing += buf->pos[base].y_offset;
  // The advance stands in for the width: better generally, and it works for a
  // glyph with no ink.
  (void)gfnt_shape_advance(face, info[base].glyph, variation, &advance);
  base_extents.x_bearing = 0;
  base_extents.width = advance;

  lig_id = gfnt_l_lig_id(&info[base]);
  num_components = (int32_t)gfnt_l_lig_num_comps(&info[base]);
  if (forward) {
    x_offset -= buf->pos[base].x_advance;
    y_offset -= buf->pos[base].y_advance;
  }
  component = base_extents;
  cluster = base_extents;
  for (i = base + 1; i < end; i++) {
    if (info[i].mcc) {
      uint8_t klass;

      if (num_components > 1) {
        uint32_t this_lig_id = gfnt_l_lig_id(&info[i]);
        int32_t this_component = (int32_t)gfnt_l_lig_comp(&info[i]) - 1;

        // Conditions for attaching to the last component.
        if (!lig_id || lig_id != this_lig_id
            || this_component >= num_components) {
          this_component = num_components - 1;
        }
        if (last_component != this_component) {
          last_component = this_component;
          last_class = 255;
          component = base_extents;
          if (horizontal_ltr) {
            component.x_bearing += (this_component * component.width)
                / num_components;
          }
          else {
            component.x_bearing += ((num_components - 1 - this_component)
                * component.width) / num_components;
          }
          component.width /= num_components;
        }
      }
      klass = info[i].mcc;
      if (last_class != klass) {
        last_class = klass;
        cluster = component;
      }
      gfnt_position_mark(face, buf, variation, upem, &cluster, i, klass,
          vertical ? 0 : (forward && horizontal_ltr) ? 1 : -1);
      buf->pos[i].x_advance = 0;
      buf->pos[i].y_advance = 0;
      buf->pos[i].x_offset += x_offset;
      buf->pos[i].y_offset += y_offset;
    }
    else if (forward) {
      x_offset -= buf->pos[i].x_advance;
      y_offset -= buf->pos[i].y_advance;
    }
    else {
      x_offset += buf->pos[i].x_advance;
      y_offset += buf->pos[i].y_advance;
    }
  }
}

static void gfnt_position_cluster(const GFNT_Face * face, GFNT_LBuffer * buf,
    const GFNT_Variation * variation, uint32_t upem, size_t start, size_t end,
    bool adjust, bool forward, bool horizontal_ltr, bool vertical) {
  size_t i;

  if (end - start < 2) {
    return;
  }
  for (i = start; i < end; i++) {
    if (!(buf->info[i].flags & GFNT_GF_MARK)) {
      size_t j;

      for (j = i + 1; j < end; j++) {
        if (!(buf->info[j].flags & GFNT_GF_MARK)) {
          break;
        }
      }
      gfnt_position_around_base(face, buf, variation, upem, i, j, adjust,
          forward, horizontal_ltr, vertical);
      i = j - 1;
    }
  }
}

void gfnt_fallback_mark_position(const GFNT_Face * face, GFNT_LBuffer * buf,
    const GFNT_Variation * variation, bool adjust_offsets_when_zeroing,
    bool forward, bool horizontal_ltr, bool vertical) {
  uint16_t upem = 0;
  size_t start = 0;
  size_t i;

  (void)gfnt_face_units_per_em(face, &upem, NULL);
  gfnt_fallback_recategorize_marks(buf);
  for (i = 1; i < buf->len; i++) {
    if (!(buf->info[i].flags & GFNT_GF_MARK)) {
      gfnt_position_cluster(face, buf, variation, upem, start, i,
          adjust_offsets_when_zeroing, forward, horizontal_ltr, vertical);
      start = i;
    }
  }
  gfnt_position_cluster(face, buf, variation, upem, start, buf->len,
      adjust_offsets_when_zeroing, forward, horizontal_ltr, vertical);
}

GFNT_Result gfnt_shape_advance(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, int32_t * advance) {
  GFNT_Result result = gfnt_face_glyph_advance(face, glyph, variation, advance,
      NULL);

  if (result == GFNT_ERR_UNSUPPORTED && variation) {
    result = gfnt_face_glyph_advance(face, glyph, NULL, advance, NULL);
  }
  return result;
}
