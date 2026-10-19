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
 * Vertical text: how each character sits.
 */

#include <ghoti.io/font/layout.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/unicode/bidi.h>
#include <ghoti.io/unicode/break.h>
#include <ghoti.io/unicode/case.h>
#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/norm.h>
#include <ghoti.io/unicode/script.h>
#include <ghoti.io/unicode/utf.h>
#include <stdlib.h>
#include <string.h>
#include "kashida.h"
#include "layout_internal.h"

/**
 * Whether the face list gives @p cp another glyph when it is set top to bottom (a `vert` or `vrt2`
 * form) than when it is set along a row.
 */
static bool has_vertical_form(const GFNT_Paragraph * p, const GFNT_TextStyle * style,
    uint32_t cp) {
  GFNT_Result r;
  uint32_t glyph[2] = {0, 0};
  uint32_t face[2] = {0, 0};

  for (int k = 0; k < 2; k++) {
    GFNT_ShapeOptions so;
    GFNT_FaceRuns fr;

    memset(&so, 0, sizeof so);
    memset(&fr, 0, sizeof fr);
    so.language = style->language;
    so.variation = style->variation;
    so.direction = k ? GFNT_DIRECTION_TTB : GFNT_DIRECTION_LTR;
    r = gfnt_faces_shape(style->faces, style->face_count, &cp, 1, &so, p->allocator, &fr, NULL);
    if (r != GFNT_OK) {
      return false;
    }
    if (fr.count && fr.runs[0].run.count) {
      glyph[k] = fr.runs[0].run.glyphs[0].glyph;
      face[k] = (uint32_t)fr.runs[0].face;
    }
    gfnt_face_runs_free(&fr);
  }
  return glyph[0] != glyph[1] && face[0] == face[1];
}

/** Whether a character takes the orientation of the character it follows. */
bool gfnt_lay_inherits_orientation(uint32_t cp) {
  if (cp == 0x200C || cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F)
      || (cp >= 0xE0100 && cp <= 0xE01EF)) {
    return true;
  }
  return (guni_general_category_mask(cp) & GUNI_GC_MASK_M) != 0;
}

/** How a character sits in vertical text, by the paragraph's orientation option and UAX #50. */
uint8_t gfnt_lay_char_orientation(const GFNT_Paragraph * p, const GFNT_TextStyle * style,
    uint32_t cp) {
  if (p->mode == GFNT_WRITING_SIDEWAYS_RL || p->mode == GFNT_WRITING_SIDEWAYS_LR
      || p->options.text_orientation == GFNT_ORIENT_SIDEWAYS) {
    return ORI_SIDEWAYS;
  }
  if (p->options.text_orientation == GFNT_ORIENT_UPRIGHT) {
    return ORI_UPRIGHT;
  }
  if (p->options.text_orientation == GFNT_ORIENT_MIXED_PANGO) {
    // Pango's natural gravity: UAX #50 with every transformed-rotated character turned whether
    // or not the face has a vertical form for it, three symbols turned that UAX #50 sets
    // upright, and one letter set upright that it turns.
    if (cp == 0x0D4E) {
      return ORI_UPRIGHT;
    }
    if (cp == 0x2B97 || cp == 0x2E50 || cp == 0x2E51
        || guni_vertical_orientation(cp) == GUNI_VO_TRANSFORMED_ROTATED) {
      return ORI_SIDEWAYS;
    }
  }
  switch (guni_vertical_orientation(cp)) {
    case GUNI_VO_UPRIGHT:
    case GUNI_VO_TRANSFORMED_UPRIGHT:
      return ORI_UPRIGHT;
    case GUNI_VO_TRANSFORMED_ROTATED:
      return has_vertical_form(p, style, cp) ? ORI_UPRIGHT : ORI_SIDEWAYS;
    default:
      return ORI_SIDEWAYS;
  }
}

uint8_t gfnt_lay_string_orientation(const GFNT_Paragraph * p, size_t span, uint32_t cp) {
  if (!p->vertical) {
    return GFNT_ORIENT_RUN_HORIZONTAL;
  }
  return gfnt_lay_char_orientation(p, &p->styles[span], cp) == ORI_UPRIGHT ? GFNT_ORIENT_RUN_UPRIGHT
                                                                    : GFNT_ORIENT_RUN_CW;
}

/**
 * How the hyphen a line ends in sits. In vertical text that reads CSS's way (the columns of
 * `vertical-rl` and `vertical-lr`, orientation mixed or upright) Chromium draws it upright, on
 * the vertical advance, however the letters before it sit; everywhere else it is set as the
 * character it is, which for a hyphen is sideways (and Pango's way, whatever the orientation).
 * -1 means that.
 */
int gfnt_lay_hyphen_orientation(const GFNT_Paragraph * p) {
  if ((p->mode == GFNT_WRITING_VERTICAL_RL || p->mode == GFNT_WRITING_VERTICAL_LR)
      && (p->options.text_orientation == GFNT_ORIENT_MIXED
          || p->options.text_orientation == GFNT_ORIENT_UPRIGHT)) {
    return GFNT_ORIENT_RUN_UPRIGHT;
  }
  return -1;
}
