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
 * What the shapers that work a syllable at a time have in common: finding the font's
 * dotted circle for a syllable that has no base, and clearing what a pause between
 * stages has no business leaving behind.
 */

#include <ghoti.io/font/cmap.h>
#include <string.h>
#include "plan.h"
#include "uprops.h"

bool gfnt_syllabic_insert_dotted_circles(GFNT_ShapeCtx * ctx,
    unsigned broken_type, unsigned dotted_category, int repha_category,
    int dotted_position) {
  GFNT_LBuffer * buf = ctx->buf;
  uint32_t glyph = 0;
  size_t i;
  uint8_t last_syllable = 0;
  bool any = false;

  for (i = 0; i < buf->len; i++) {
    if ((buf->info[i].syllable & 0x0F) == broken_type) {
      any = true;
      break;
    }
  }
  if (!any) {
    return true;
  }
  if (gfnt_face_glyph_for_codepoint(ctx->face, 0x25CC, &glyph, NULL)
      != GFNT_OK || !glyph) {
    return true;
  }
  for (i = 0; i < buf->len; i++) {
    uint8_t syllable = buf->info[i].syllable;

    if (last_syllable != syllable && (syllable & 0x0F) == broken_type) {
      GFNT_LInfo circle;
      size_t at = i;

      last_syllable = syllable;
      memset(&circle, 0, sizeof circle);
      circle.unicode = 0x25CC;
      gfnt_u_set_props(&circle);
      circle.glyph = glyph;
      circle.cluster = buf->info[i].cluster;
      circle.mask = buf->info[i].mask;
      circle.syllable = syllable;
      circle.category = (uint8_t)dotted_category;
      if (dotted_position >= 0) {
        circle.position = (uint8_t)dotted_position;
      }
      // After a repha, if the syllable starts with one.
      if (repha_category >= 0) {
        while (at < buf->len && buf->info[at].syllable == last_syllable
            && buf->info[at].category == repha_category) {
          at++;
        }
      }
      if (!gfnt_lbuf_insert(buf, at, &circle)) {
        ctx->oom = true;
        return false;
      }
      // The circle is now at `at`; carry on past it and the syllable's first glyphs.
      i = at;
    }
  }
  return true;
}

void gfnt_syllabic_clear_substitution_flags(GFNT_ShapeCtx * ctx) {
  size_t i;

  for (i = 0; i < ctx->buf->len; i++) {
    ctx->buf->info[i].props &= (uint16_t)~GFNT_PROP_SUBSTITUTED;
  }
}

void gfnt_syllabic_clear_syllables(GFNT_ShapeCtx * ctx) {
  size_t i;

  for (i = 0; i < ctx->buf->len; i++) {
    ctx->buf->info[i].syllable = 0;
  }
}
