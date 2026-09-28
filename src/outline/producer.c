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
 * Which producer answers for a glyph: `glyf`, or a charstring.
 *
 * documentation/design.md sections 7.3 and 7.4. This file exists because
 * ::gfnt_face_glyph_outline() is one question with two implementations, and
 * putting the choice in either implementation would make that one include the
 * other. Until phase 2 the choice was inside `glyf.c`, where the `CFF ` arm was
 * a sentence saying the charstrings arrive later.
 *
 * Which format a face carries is ::gfnt_sfnt_producer(), in `sfnt.c`, because
 * ::gfnt_face_has_outlines() is tier 0 and has to answer the same question
 * without including `outline.h` - and two answers to it would be two answers.
 */

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/outline.h>
#include <stdbool.h>
#include "../cff/cff_glyph.h"
#include "../glyf/glyf.h"
#include "../sfnt/sfnt.h"
#include "outline.h"

/** The tag of the table holding TrueType glyph descriptions. */
#define GFNT_TAG_GLYF GFNT_TAG('g', 'l', 'y', 'f')
/** The tag of the table holding their offsets. */
#define GFNT_TAG_LOCA GFNT_TAG('l', 'o', 'c', 'a')

/** The refusal a face with no outlines this library reads gets, by name. */
static GFNT_Result gfnt_producer_refuse(const GFNT_Face * face, uint32_t glyph,
    GFNT_Error * error) {
  if (gfnt_sfnt_find(face, GFNT_TAG_CFF2)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF2, 0, glyph,
        "this face's outlines are CFF2 charstrings, which are a different "
        "format in a different table and are not read (design.md section 16)");
  }
  if (gfnt_sfnt_find(face, GFNT_TAG_GLYF) || gfnt_sfnt_find(face, GFNT_TAG_LOCA)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_GLYF, 0, glyph,
        "this face has one of glyf and loca and not the other, so neither "
        "indexes a glyph");
  }
  return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, glyph,
      "this face has no outlines at all");
}

GFNT_Result gfnt_face_glyph_outline(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, const GFNT_Allocator * allocator,
    GFNT_Outline ** out_outline, GFNT_Error * error) {
  GFNT_Outline * outline = NULL;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_outline) {
    return GFNT_ERR_INVALID;
  }
  if (variation && variation->count != 0) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, glyph,
        "this library does not read gvar yet, so it cannot honour variation "
        "coordinates rather than quietly ignoring them");
  }
  if (gfnt_sfnt_producer(face) == GFNT_PRODUCER_NONE) {
    return gfnt_producer_refuse(face, glyph, error);
  }
  result = gfnt_outline_create(allocator ? allocator : face->allocator,
      &outline, error);
  if (result != GFNT_OK) {
    return result;
  }
  gfnt_outline_set_limits(outline, &face->limits);
  if (gfnt_sfnt_producer(face) == GFNT_PRODUCER_GLYF) {
    result = gfnt_glyf_load(face, glyph, 0, outline, error);
  }
  else {
    result = gfnt_cff_load(face, glyph, outline, NULL, error);
  }
  if (result != GFNT_OK) {
    gfnt_outline_destroy(outline);
    return result;
  }
  *out_outline = outline;
  return GFNT_OK;
}

GFNT_Result gfnt_face_glyph_is_composite(const GFNT_Face * face, uint32_t glyph,
    bool * out_composite, GFNT_Error * error) {
  gfnt_error_clear(error);
  if (!face || !out_composite) {
    return GFNT_ERR_INVALID;
  }
  switch (gfnt_sfnt_producer(face)) {
    case GFNT_PRODUCER_GLYF:
      return gfnt_glyf_is_composite(face, glyph, out_composite, error);
    case GFNT_PRODUCER_CFF:
      // A CFF glyph is "composite" when its charstring assembles an accented
      // character out of two others. That is the same *fact* a `glyf` composite
      // reports - this glyph is drawn out of other glyphs - reached by a
      // different construction, which is why one predicate answers for both.
      return gfnt_cff_is_composite(face, glyph, out_composite, error);
    case GFNT_PRODUCER_NONE:
      break;
  }
  return gfnt_producer_refuse(face, glyph, error);
}

GFNT_Result gfnt_face_glyph_stated_box(const GFNT_Face * face, uint32_t glyph,
    GFNT_Box * out_box, GFNT_Error * error) {
  gfnt_error_clear(error);
  if (!face || !out_box) {
    return GFNT_ERR_INVALID;
  }
  switch (gfnt_sfnt_producer(face)) {
    case GFNT_PRODUCER_GLYF:
      return gfnt_glyf_stated_box(face, glyph, out_box, error);
    case GFNT_PRODUCER_CFF:
      // There is no per-glyph box in a CFF at all: the Top DICT's `FontBBox`
      // covers the whole font, and a per-glyph one would have to be computed
      // from the charstring - which is ::gfnt_outline_bounds(), a different
      // fact. Answering the font's box here would be answering a question
      // nobody asked with a number that looks like the one they did.
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF, 0, glyph,
          "a CFF glyph states no bounding box of its own; the Top DICT's "
          "FontBBox is the whole font's, and this glyph's own bounds come from "
          "gfnt_outline_bounds()");
    case GFNT_PRODUCER_NONE:
      break;
  }
  return gfnt_producer_refuse(face, glyph, error);
}
