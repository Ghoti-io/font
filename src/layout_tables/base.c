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
 * `BASE`: the baselines of a script, such as the ideographic or hanging one, as a
 * coordinate in font units.
 */

#include <string.h>
#include <ghoti.io/font/shape.h>
#include "layout.h"
#include "../core/fixed.h"
#include "../sfnt/sfnt.h"

/** One `BaseCoord`, in font units. A reference point (format 2) is the stated coordinate. */
static int32_t gfnt_base_coord(GFNT_LApply * c, size_t coord) {
  int64_t value = gfnt_ls16(c, coord + 2);

  if (gfnt_lu16(c, coord) == 3) {
    value += gfnt_round_shift_mode(gfnt_gpos_device_delta(c,
        gfnt_l_rel(coord, gfnt_lu16(c, coord + 4))), 16,
        c->variation ? c->variation->delta_rounding : 0);
  }
  return (int32_t)value;
}

/** The `BaseScript` for @p script in an axis, or 0. */
static size_t gfnt_base_script(GFNT_LApply * c, size_t axis, GFNT_Tag script) {
  size_t list = gfnt_l_rel(axis, gfnt_lu16(c, axis + 2));
  uint32_t count;

  if (!list) {
    return 0;
  }
  count = gfnt_lu16(c, list);
  for (uint32_t i = 0; i < count; i++) {
    size_t record = list + 2 + 6 * (size_t)i;

    if (gfnt_lu32(c, record) == script) {
      return gfnt_l_rel(list, gfnt_lu16(c, record + 4));
    }
  }
  return 0;
}

bool gfnt_face_baseline(const GFNT_Face * face, GFNT_Tag baseline, int vertical,
    GFNT_Tag script, const GFNT_Variation * variation, uint32_t ppem,
    int32_t * out) {
  GFNT_Gdef base;
  GFNT_LayoutTable lt;
  GFNT_LApply c;
  uint16_t minor = 0;
  uint16_t major = 0;
  bool bad = false;
  size_t axis;
  size_t tags;
  size_t values;
  size_t script_at;
  uint32_t count;

  if (!face || !out || !gfnt_face_has_table(face, GFNT_TAG('B', 'A', 'S', 'E'))) {
    return false;
  }
  memset(&base, 0, sizeof base);
  if (gfnt_face_table_reader(face, GFNT_TAG('B', 'A', 'S', 'E'), &base.table, NULL)
      != GFNT_OK) {
    return false;
  }
  base.table.error = NULL;
  major = gfnt_lr_u16(&base.table, 0, &bad);
  minor = gfnt_lr_u16(&base.table, 2, &bad);
  if (bad || major != 1) {
    return false;
  }
  if (minor >= 1) {
    base.var_store = gfnt_lr_u32(&base.table, 8, &bad);
  }
  memset(&lt, 0, sizeof lt);
  lt.table = base.table;
  memset(&c, 0, sizeof c);
  c.face = face;
  c.lt = &lt;
  c.gdef = &base;
  c.variation = variation;
  c.ppem = ppem;
  axis = gfnt_l_rel(0, gfnt_lu16(&c, vertical ? 6 : 4));
  if (!axis) {
    return false;
  }
  tags = gfnt_l_rel(axis, gfnt_lu16(&c, axis));
  script_at = gfnt_base_script(&c, axis, script);
  if (!script_at) {
    script_at = gfnt_base_script(&c, axis, GFNT_TAG('D', 'F', 'L', 'T'));
  }
  if (!tags || !script_at) {
    return false;
  }
  values = gfnt_l_rel(script_at, gfnt_lu16(&c, script_at));
  if (!values) {
    return false;
  }
  count = gfnt_lu16(&c, tags);
  for (uint32_t i = 0; i < count && i < gfnt_lu16(&c, values + 2); i++) {
    if (gfnt_lu32(&c, tags + 2 + 4 * (size_t)i) == baseline) {
      size_t coord = gfnt_l_rel(values, gfnt_lu16(&c, values + 4 + 2 * (size_t)i));

      if (!coord) {
        return false;
      }
      *out = gfnt_base_coord(&c, coord);
      return !c.fault.bad;
    }
  }
  return false;
}
