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
 * The ligature carets of `GDEF`: where the cursor goes between the components of
 * a ligature.
 */

#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/shape.h>
#include <string.h>
#include "layout.h"
#include "../core/fixed.h"

/** One `CaretValue`, in font units, or false when it says nothing. */
static bool gfnt_caret_value(GFNT_LApply * c, const GFNT_Face * face,
    size_t caret, uint32_t glyph, int vertical, int32_t * out) {
  uint32_t format = gfnt_lu16(c, caret);

  if (format == 1 || format == 3) {
    int64_t value = gfnt_ls16(c, caret + 2);

    if (format == 3) {
      uint32_t device = gfnt_lu16(c, caret + 4);

      value += gfnt_round_shift_mode(
          gfnt_gpos_device_delta(c, gfnt_l_rel(caret, device)), 16,
          c->variation ? c->variation->delta_rounding : 0);
    }
    *out = (int32_t)value;
    return true;
  }
  if (format == 2) {
    GFNT_Outline * outline = NULL;
    GFNT_Point point;
    bool found = false;

    if (gfnt_face_glyph_outline(face, glyph, c->variation, NULL, &outline, NULL)
            == GFNT_OK) {
      found = gfnt_outline_point_at(outline, gfnt_lu16(c, caret + 2), &point,
                  NULL) == GFNT_OK;
      if (found) {
        // 26.6 to font units, nearest.
        int32_t v = vertical ? point.y : point.x;

        *out = (v + (v < 0 ? -32 : 32)) / 64;
      }
    }
    gfnt_outline_destroy(outline);
    return found;
  }
  return false;
}

size_t gfnt_face_ligature_carets(const GFNT_Face * face, uint32_t glyph,
    int vertical, const GFNT_Variation * variation, uint32_t ppem, int32_t * out,
    size_t capacity) {
  GFNT_Gdef gdef;
  GFNT_LayoutTable lt;
  GFNT_LApply c;
  size_t list;
  size_t entry;
  int32_t index;
  size_t count;
  size_t found = 0;

  if (!face) {
    return 0;
  }
  gfnt_gdef_open(face, &gdef);
  if (!gdef.lig_carets) {
    return 0;
  }
  memset(&lt, 0, sizeof lt);
  lt.table = gdef.table;
  memset(&c, 0, sizeof c);
  c.face = face;
  c.lt = &lt;
  c.gdef = &gdef;
  c.variation = variation;
  c.ppem = ppem;
  list = gdef.lig_carets;
  index = gfnt_l_coverage(&c, gfnt_l_rel(list, gfnt_lu16(&c, list)), glyph);
  if (index < 0 || (uint32_t)index >= gfnt_lu16(&c, list + 2)) {
    return 0;
  }
  entry = gfnt_l_rel(list, gfnt_lu16(&c, list + 4 + 2 * (size_t)index));
  if (!entry) {
    return 0;
  }
  count = gfnt_lu16(&c, entry);
  for (size_t i = 0; i < count; i++) {
    int32_t value = 0;
    size_t caret = gfnt_l_rel(entry, gfnt_lu16(&c, entry + 2 + 2 * i));

    if (!caret || !gfnt_caret_value(&c, face, caret, glyph, vertical, &value)) {
      continue;
    }
    if (out && found < capacity) {
      out[found] = value;
    }
    found++;
  }
  return found;
}
