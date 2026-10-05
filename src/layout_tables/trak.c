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
 * Apple's `trak` table: tracking, the space added between every pair of glyphs
 * for a size, in HarfBuzz's way.
 *
 * Only version 1.0 is read, and only the track whose value is exactly zero. Its
 * values are given for a few sizes; the one for the point size is found by a
 * straight line through the two sizes either side of it, or through the first
 * two or the last two when it lies outside. Every glyph is made wider by that
 * much, rounded, and moved along by half of it, the horizontal table being used
 * for horizontal text and the vertical one for vertical.
 */

#include <math.h>
#include <string.h>
#include "layout.h"
#include "../sfnt/sfnt.h"

#define GFNT_TAG_trak GFNT_TAG('t', 'r', 'a', 'k')

bool gfnt_trak_present(const GFNT_Face * face) {
  GFNT_Reader table;
  bool bad = false;

  if (!gfnt_face_has_table(face, GFNT_TAG_trak)
      || gfnt_face_table_reader(face, GFNT_TAG_trak, &table, NULL) != GFNT_OK) {
    return false;
  }
  table.error = NULL;
  return gfnt_lr_u32(&table, 0, &bad) == 0x00010000u && !bad;
}

static float trak_size(const GFNT_Reader * r, size_t at, bool * bad) {
  return (float)(int32_t)gfnt_lr_u32(r, at, bad) / 65536.0f;
}

/** The tracking for @p ptem from the track data at @p data, or 0. */
static float trak_tracking(const GFNT_Reader * r, size_t data, float ptem,
    bool * bad) {
  uint32_t tracks = gfnt_lr_u16(r, data, bad);
  uint32_t sizes = gfnt_lr_u16(r, data + 2, bad);
  size_t size_table = gfnt_lr_u32(r, data + 4, bad);
  size_t entry = 0;
  size_t values;
  uint32_t i;
  uint32_t at;
  float s0;
  float s1;
  float t;
  float v0;
  float v1;

  for (i = 0; i < tracks && !*bad; i++) {
    size_t e = data + 8 + 8 * (size_t)i;

    if (gfnt_lr_u32(r, e, bad) == 0) {
      entry = e;
      break;
    }
  }
  if (*bad || !entry || !sizes) {
    return 0;
  }
  values = gfnt_lr_u16(r, entry + 6, bad);
  if (sizes == 1) {
    return (float)(int16_t)gfnt_lr_u16(r, values, bad);
  }
  for (at = 0; at < sizes; at++) {
    if (trak_size(r, size_table + 4 * (size_t)at, bad) >= ptem) {
      break;
    }
  }
  if (at == 0) {
    at = 1;
  }
  if (at > sizes - 1) {
    at = sizes - 1;
  }
  s0 = trak_size(r, size_table + 4 * (size_t)(at - 1), bad);
  s1 = trak_size(r, size_table + 4 * (size_t)at, bad);
  t = s0 == s1 ? 0.0f : (ptem - s0) / (s1 - s0);
  v0 = (float)(int16_t)gfnt_lr_u16(r, values + 2 * (size_t)(at - 1), bad);
  v1 = (float)(int16_t)gfnt_lr_u16(r, values + 2 * (size_t)at, bad);
  return t * v1 + (1.0f - t) * v0;
}

GFNT_Result gfnt_trak_apply(const GFNT_Face * face, GFNT_LBuffer * b,
    float ptem, bool vertical, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_Result result;
  bool bad = false;
  size_t data;
  float tracking;
  int32_t advance;
  int32_t offset;
  size_t i;

  result = gfnt_face_table_reader(face, GFNT_TAG_trak, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  table.error = NULL;
  data = gfnt_lr_u16(&table, vertical ? 8 : 6, &bad);
  if (bad || !data) {
    return GFNT_OK;
  }
  tracking = trak_tracking(&table, data, ptem, &bad);
  if (bad) {
    return GFNT_OK;
  }
  advance = (int32_t)floorf(tracking + 0.5f);
  offset = advance / 2;
  for (i = 0; i < b->len; i++) {
    if (vertical) {
      b->pos[i].y_advance += advance;
      b->pos[i].y_offset += offset;
    }
    else {
      b->pos[i].x_advance += advance;
      b->pos[i].x_offset += offset;
    }
  }
  return GFNT_OK;
}
