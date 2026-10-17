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
 * The public hinting API: a context for a face at a size, a hinted outline, and
 * what the font's `gasp` says about whether to hint at a size.
 */

#include "vm.h"

#include <ghoti.io/font/hint.h>
#include "../sfnt/sfnt.h"

/* `gasp` range behaviour bits (OpenType 1.9, "gasp - Grid-fitting And
   Scan-conversion Procedure"). */
#define GFNT_GASP_GRIDFIT 0x0001u

bool gfnt_face_gasp_gridfit(const GFNT_Face * face, uint32_t ppem) {
  const uint8_t * data = NULL;
  size_t length = 0;
  uint32_t ranges;

  if (!face || gfnt_face_table_data(face, GFNT_TAG('g', 'a', 's', 'p'), &data, &length)
      != GFNT_OK || length < 4u) {
    return true;
  }
  ranges = ((uint32_t)data[2] << 8) | data[3];
  /* The ranges are in ascending order of their upper bound; the first that holds
     the size is the one. A table that ends before the size is a table with no
     opinion about it. */
  for (uint32_t i = 0; i < ranges && 4u + 4u * (size_t)(i + 1u) <= length; ++i) {
    const uint8_t * r = data + 4u + 4u * (size_t)i;
    uint32_t max_ppem = ((uint32_t)r[0] << 8) | r[1];
    uint32_t behaviour = ((uint32_t)r[2] << 8) | r[3];

    if (ppem <= max_ppem) {
      return (behaviour & GFNT_GASP_GRIDFIT) != 0;
    }
  }
  return true;
}

GFNT_Result gfnt_hint_context_create(const GFNT_Face * face, uint32_t ppem,
    GFNT_HintVersion version, GFNT_HintMode mode, const GFNT_Variation * variation,
    GFNT_HintContext ** out_context, GFNT_Error * error) {
  gfnt_error_clear(error);
  if (!face || !out_context) {
    return GFNT_ERR_INVALID;
  }
  if (ppem == 0) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, 0,
        "a pixel size of zero cannot be hinted for");
  }
  if ((size_t)ppem > face->limits.max_ppem) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, 0,
        "this pixel size is larger than max_ppem allows");
  }
  if (version != GFNT_HINT_VERSION_AUTO && version != GFNT_HINT_VERSION_35
      && version != GFNT_HINT_VERSION_40) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, 0, "not a hinting version");
  }
  if (mode != GFNT_HINT_MODE_FONT && mode != GFNT_HINT_MODE_FONT_STRICT) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, 0,
        "a context is made for the font's own program, tolerant or strict");
  }
  if (variation && variation->count && !variation->coords) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, 0, "a variation with a count and no coordinates");
  }
  return gfnt_hint_ctx_from_face_at(face, ppem, version == GFNT_HINT_VERSION_35 ? 35 : 40,
      true, mode == GFNT_HINT_MODE_FONT, NULL, variation, out_context, error);
}

void gfnt_hint_context_destroy(GFNT_HintContext * context) {
  gfnt_hint_ctx_destroy(context);
}

GFNT_Result gfnt_face_hinted_outline(const GFNT_Face * face,
    const GFNT_HintContext * context, uint32_t glyph, const GFNT_Allocator * allocator,
    GFNT_Outline ** out_outline, GFNT_HintStatus * out_status, GFNT_F26Dot6 * out_advance,
    GFNT_Error * error) {
  GFNT_HintStatus status = GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM;
  int32_t advance = 0;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !context || !out_outline) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_hint_glyph_outline_in(face, context, glyph, allocator, out_outline, &status, NULL,
      &advance, NULL, 0, NULL, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (out_status) *out_status = status;
  if (out_advance) *out_advance = advance;
  return GFNT_OK;
}
