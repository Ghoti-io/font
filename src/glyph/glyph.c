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
 * The glyph-kind names, and which strike answers a pixel size.
 *
 * documentation/design.md sections 5.3 and 5.4. No glyph data is read here -
 * the outline producers arrive in phase 1 and the strike parsers in phase 1b -
 * but the shape of the question is decided now, because adding a strike list
 * to a released API is a break across all of it.
 *
 * The one thing this file refuses to do is answer "no strikes" for a face that
 * has them. A face carrying `EBLC`, `CBLC` or `sbix` gets
 * ::GFNT_ERR_UNSUPPORTED, which says "this library cannot enumerate them yet";
 * a count of zero would say "this bitmap font has no bitmaps", and a caller
 * cannot tell that from the truth.
 */

#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/macros.h>
#include "../sfnt/sfnt.h"

/** The tables a strike list can live in, none of which is parsed yet. */
static const GFNT_Tag gfnt_strike_tables[] = {
  GFNT_TAG('E', 'B', 'L', 'C'), // embedded monochrome and grey strikes
  GFNT_TAG('b', 'l', 'o', 'c'), // Apple's spelling of the same
  GFNT_TAG('C', 'B', 'L', 'C'), // colour strikes, PNG payloads
  GFNT_TAG('s', 'b', 'i', 'x'), // Apple's colour strikes
};

const char * gfnt_glyph_kind_string(GFNT_GlyphKind kind) {
  switch (kind) {
    case GFNT_GLYPH_OUTLINE:
      return "outline";
    case GFNT_GLYPH_BITMAP_MONO:
      return "1-bit bitmap";
    case GFNT_GLYPH_BITMAP_GRAY:
      return "grey bitmap";
    case GFNT_GLYPH_BITMAP_PNG:
      return "encoded image";
    case GFNT_GLYPH_COLR_LAYERS:
      return "colour layers";
    case GFNT_GLYPH_SVG:
      return "SVG document";
    case GFNT_GLYPH_KIND_COUNT:
    default:
      return "unknown glyph kind";
  }
}

bool gfnt_face_has_outlines(const GFNT_Face * face) {
  if (!face) {
    return false;
  }
  // glyf needs loca to be indexable at all, so a font with one and not the
  // other has no outlines this library can reach.
  if (gfnt_face_has_table(face, GFNT_TAG('g', 'l', 'y', 'f'))
      && gfnt_face_has_table(face, GFNT_TAG('l', 'o', 'c', 'a'))) {
    return true;
  }
  return gfnt_face_has_table(face, GFNT_TAG('C', 'F', 'F', ' '));
}

/**
 * The first strike table the face carries, or 0 if it carries none.
 */
static GFNT_Tag gfnt_face_strike_table(const GFNT_Face * face) {
  for (size_t i = 0; i < GFNT_ARRAY_SIZE(gfnt_strike_tables); ++i) {
    if (gfnt_face_has_table(face, gfnt_strike_tables[i])) {
      return gfnt_strike_tables[i];
    }
  }
  return 0;
}

GFNT_Result gfnt_face_strike_count(const GFNT_Face * face, size_t * out_count,
    GFNT_Error * error) {
  GFNT_Tag table;

  if (!face || !out_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the count");
  }

  table = gfnt_face_strike_table(face);
  if (table != 0) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, table, 0,
        GFNT_GLYPH_NONE,
        "the face has bitmap strikes in a table this library does not parse "
        "yet");
  }
  *out_count = 0;
  return GFNT_OK;
}

GFNT_Result gfnt_face_strike_at(const GFNT_Face * face, size_t index,
    GFNT_Strike * out_strike, GFNT_Error * error) {
  size_t count = 0;
  GFNT_Result result;

  if (!face || !out_strike) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the strike");
  }
  result = gfnt_face_strike_count(face, &count, error);
  if (result != GFNT_OK) {
    return result;
  }
  // count is zero until the strike parsers land, so every index is out of
  // range - which is the same answer this will give for a real index past the
  // end of a real strike list.
  (void)index;
  return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
      "the face has no strike at that index");
}

GFNT_Result gfnt_face_select_strike(const GFNT_Face * face, uint32_t ppem,
    GFNT_StrikePolicy policy, GFNT_Strike * out_strike,
    bool * out_from_outlines, GFNT_Error * error) {
  size_t count = 0;
  GFNT_Result result;

  if (!face) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face");
  }
  if (ppem == 0) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "a pixel size of zero");
  }
  if (policy != GFNT_STRIKE_OUTLINES_ONLY && policy != GFNT_STRIKE_EXACT
      && policy != GFNT_STRIKE_NEAREST && policy != GFNT_STRIKE_PREFER_STRIKE) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "not a strike policy this library defines");
  }
  if (ppem > face->limits.max_ppem) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "a pixel size above GFNT_Limits::max_ppem");
  }

  if (policy != GFNT_STRIKE_OUTLINES_ONLY) {
    // Asking the strike list is what turns an unparsed EBLC into an honest
    // refusal: a policy that wanted a strike must not be answered "outlines"
    // by a library that simply cannot see the strikes.
    result = gfnt_face_strike_count(face, &count, error);
    if (result != GFNT_OK) {
      return result;
    }
  }

  // With no strikes, every policy resolves the same way, and the only question
  // left is whether there are outlines to scale.
  if (!gfnt_face_has_outlines(face)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
        "the face has neither strikes nor outlines this library reads");
  }
  if (out_from_outlines) {
    *out_from_outlines = true;
  }
  if (out_strike) {
    *out_strike = (GFNT_Strike) {
      .index = 0,
      .ppem_x = ppem,
      .ppem_y = ppem,
      .bit_depth = 8,
      .kind = GFNT_GLYPH_OUTLINE,
    };
  }
  return GFNT_OK;
}
