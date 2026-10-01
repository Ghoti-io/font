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
 * documentation/design.md sections 5.3 and 5.4. No glyph data is read here: the
 * strike *list* comes from `EBLC` or from a standalone container's one strike,
 * and the pixels behind it are `bitmap.h`'s.
 *
 * The one thing this file refuses to do is answer "no strikes" for a face that
 * has them. A face carrying `bloc`, `CBLC` or `sbix` - or an `EBLC` with no
 * `EBDT` to go with it - gets ::GFNT_ERR_UNSUPPORTED, which says "this library
 * cannot enumerate them"; a count of zero would say "this bitmap font has no
 * bitmaps", and a caller cannot tell that from the truth.
 *
 * **The selection is here and the strikes come from elsewhere**, which is what
 * lets one policy serve a PCF's single strike and an `EBLC`'s six: this file
 * asks ::gfnt_face_strike_count() and ::gfnt_face_strike_at() like any caller
 * would, so there is one implementation of "nearest" and no format can have an
 * idea of its own about it.
 */

#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/macros.h>
#include "../bitmap/bitmap.h"
#include "../sfnt/sfnt.h"

/**
 * The tables a strike list can live in.
 *
 * `EBLC` is in the list and is also the one that is read: a face reaches
 * ::gfnt_face_strike_table() only after ::gfnt_face_has_eblc() has said no, so
 * an `EBLC` named here is one with no `EBDT` behind it - a strike list pointing
 * at glyph data the file does not contain, which is not strikes this library can
 * offer and must not be reported as none.
 */
static const GFNT_Tag gfnt_strike_tables[] = {
  GFNT_TAG('E', 'B', 'L', 'C'), // read, when an EBDT is there too
  GFNT_TAG('b', 'l', 'o', 'c'), // Apple's spelling of the same; not read
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
  // The question this predicate answers is *can asking for an outline succeed*,
  // and the one place that knows is the producer table: `glyf` with `loca`, or
  // `CFF `. Answering it here a second way is what once made `CFF ` report true
  // while `CFF2` reported false, each justified by a different reading of the
  // same sentence - and ::gfnt_face_strike_at() then told a caller with an OTTO
  // font it had outlines to scale, months before anything could draw one.
  //
  // `CFF ` reports true as of phase 2. `CFF2` still reports false, and now for
  // the only reason left: it is a format this library does not read at all
  // (design.md section 16).
  return gfnt_sfnt_producer(face) != GFNT_PRODUCER_NONE;
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
  // A standalone bitmap container is one strike, always: the file is a strike.
  // The parse has already happened - it is what made the face - so this costs a
  // memo lookup.
  if (gfnt_face_is_bitmap(face)) {
    const GFNT_BitmapFont * font = NULL;
    GFNT_Result result = gfnt_face_bitmap(face, &font, error);

    if (result != GFNT_OK) {
      return result;
    }
    *out_count = 1;
    return GFNT_OK;
  }
  // An sfnt's strikes are a list, and a refused EBLC is a refusal: a table that
  // contradicts itself must not read as a font with no bitmaps, which is the
  // same rule the unparsed tables below follow.
  if (gfnt_face_has_eblc(face)) {
    const GFNT_Eblc * eblc = NULL;
    GFNT_Result result = gfnt_face_eblc(face, &eblc, error);

    if (result != GFNT_OK) {
      return result;
    }
    *out_count = eblc->strike_count;
    return GFNT_OK;
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
  if (index < count && gfnt_face_is_bitmap(face)) {
    const GFNT_BitmapFont * font = NULL;

    result = gfnt_face_bitmap(face, &font, error);
    if (result != GFNT_OK) {
      return result;
    }
    *out_strike = font->strike;
    return GFNT_OK;
  }
  if (index < count && gfnt_face_has_eblc(face)) {
    const GFNT_Eblc * eblc = NULL;

    result = gfnt_face_eblc(face, &eblc, error);
    if (result != GFNT_OK) {
      return result;
    }
    *out_strike = eblc->strikes[index].strike;
    return GFNT_OK;
  }
  // For every other face the count is zero, so every index is out of range -
  // which is the same answer a real index past the end of a real strike list
  // gets.
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

  // Every policy but OUTLINES_ONLY is answered by a strike when the face has
  // any, and OUTLINES_ONLY has to fail on a face with no outlines - a caller who
  // asked for outlines and would be handed pixels is M9 with the policy
  // inverted.
  if (count > 0) {
    GFNT_Strike chosen;
    bool found = false;

    // The nearest strike, and a tie goes to the larger. A tie is real - 13
    // pixels between strikes at 12 and 14 - and the rule has to be *stated*
    // rather than left to the order the strikes happen to be listed in, because
    // a caller reporting "this text looks wrong at 13" needs the answer to be
    // the same twice. Larger, because the alternative throws pixels away.
    for (size_t i = 0; i < count; ++i) {
      GFNT_Strike strike;
      uint32_t distance;
      uint32_t best;

      result = gfnt_face_strike_at(face, i, &strike, error);
      if (result != GFNT_OK) {
        return result;
      }
      if (policy == GFNT_STRIKE_EXACT) {
        if (strike.ppem_y == ppem) {
          chosen = strike;
          found = true;
          break;
        }
        continue;
      }
      distance = strike.ppem_y > ppem ? strike.ppem_y - ppem
                                      : ppem - strike.ppem_y;
      best = !found ? 0
          : (chosen.ppem_y > ppem ? chosen.ppem_y - ppem
                                  : ppem - chosen.ppem_y);
      if (!found || distance < best
          || (distance == best && strike.ppem_y > chosen.ppem_y)) {
        chosen = strike;
        found = true;
      }
    }

    if (found) {
      if (out_strike) {
        *out_strike = chosen;
      }
      if (out_from_outlines) {
        *out_from_outlines = false;
      }
      return GFNT_OK;
    }
    // Only EXACT reaches here with strikes in the face: NEAREST and
    // PREFER_STRIKE always find one, because every strike is some distance from
    // every size. EXACT is the one policy that can refuse a strike a face has,
    // which is the whole point of it existing beside NEAREST - a font drawn at
    // sixteen pixels cannot answer for thirteen.
    if (gfnt_face_has_outlines(face)) {
      if (out_from_outlines) {
        *out_from_outlines = true;
      }
      return GFNT_OK;
    }
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
        "the face has no strike at exactly that pixel size, and no outlines "
        "to scale instead");
  }

  // With no strikes, every policy resolves the same way, and the only question
  // left is whether there are outlines to scale.
  if (!gfnt_face_has_outlines(face)) {
    // A bitmap face reaches here only under OUTLINES_ONLY, which skipped the
    // strike count above on purpose - an sfnt with an unparsed EBLC must still be
    // able to answer that policy from its outlines. So the refusal has to
    // distinguish the two: this face has a strike and was told to ignore it.
    if (gfnt_face_is_bitmap(face)) {
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, face->flavour, 0,
          GFNT_GLYPH_NONE,
          "a bitmap font asked for outlines only, and it has none - its strike "
          "answers under any other policy");
    }
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
