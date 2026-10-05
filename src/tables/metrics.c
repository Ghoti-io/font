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
 * What a text stack asks a face for: the table accessors, the glyph count, one
 * glyph's advance, and how tall a line is.
 *
 * documentation/design.md section 7.2. Three rules are implemented here rather
 * than described:
 *
 * - **numGlyphs is the minimum** across every table that indexes glyphs (M12),
 *   not what `maxp` says. Fonts exist where `hmtx` is shorter than `maxp`
 *   claims, and the parser that indexed past the shorter one is the defect.
 * - **`hmtx`'s last advance repeats** for every glyph past
 *   `numberOfHMetrics`. A monospaced font with 3,000 glyphs stores one
 *   advance, and a parser that indexes the array directly reads whatever
 *   follows it.
 * - **Descent is negative** whichever table answered (M18), and which table
 *   answers is a named policy with the font's own request as its zero (M4).
 *
 * Reference: OpenType Specification 1.9, "maxp", "hmtx", "hhea" and "OS/2";
 * CSS Inline Layout Module Level 3 for what a consumer does with the result.
 */

#include <ghoti.io/font/macros.h>
#include "../bitmap/bitmap.h"
#include "../core/fixed.h"
#include "../var/var.h"
#include "tables.h"

GFNT_Result gfnt_face_head(const GFNT_Face * face, const GFNT_Head ** out_head,
    GFNT_Error * error) {
  GFNT_Face * owner = (GFNT_Face *)face;
  GFNT_Head scratch;
  GFNT_Result result;

  if (!face || !out_head) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the table");
  }
  result = gfnt_table_cached(face, &owner->head_state, &owner->head, &scratch,
      sizeof scratch, gfnt_head_parse, NULL, NULL, error);
  if (result == GFNT_OK) {
    *out_head = &owner->head;
  }
  return result;
}

GFNT_Result gfnt_face_hhea(const GFNT_Face * face, const GFNT_Hhea ** out_hhea,
    GFNT_Error * error) {
  GFNT_Face * owner = (GFNT_Face *)face;
  GFNT_Hhea scratch;
  GFNT_Result result;

  if (!face || !out_hhea) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the table");
  }
  result = gfnt_table_cached(face, &owner->hhea_state, &owner->hhea, &scratch,
      sizeof scratch, gfnt_hhea_parse, NULL, NULL, error);
  if (result == GFNT_OK) {
    *out_hhea = &owner->hhea;
  }
  return result;
}

GFNT_Result gfnt_face_os2(const GFNT_Face * face, const GFNT_Os2 ** out_os2,
    GFNT_Error * error) {
  GFNT_Face * owner = (GFNT_Face *)face;
  GFNT_Os2 scratch;
  GFNT_Result result;

  if (!face || !out_os2) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the table");
  }
  result = gfnt_table_cached(face, &owner->os2_state, &owner->os2, &scratch,
      sizeof scratch, gfnt_os2_parse, NULL, NULL, error);
  if (result == GFNT_OK) {
    *out_os2 = &owner->os2;
  }
  return result;
}

GFNT_Result gfnt_face_post(const GFNT_Face * face, const GFNT_Post ** out_post,
    GFNT_Error * error) {
  GFNT_Face * owner = (GFNT_Face *)face;
  GFNT_Post scratch;
  GFNT_Result result;

  if (!face || !out_post) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the table");
  }
  result = gfnt_table_cached(face, &owner->post_state, &owner->post, &scratch,
      sizeof scratch, gfnt_post_parse, NULL, NULL, error);
  if (result == GFNT_OK) {
    *out_post = &owner->post;
  }
  return result;
}

GFNT_Result gfnt_face_units_per_em(const GFNT_Face * face,
    uint16_t * out_units, GFNT_Error * error) {
  const GFNT_Head * head = NULL;
  GFNT_Result result;

  if (!out_units) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "nowhere to put the em size");
  }
  result = gfnt_face_head(face, &head, error);
  if (result == GFNT_ERR_UNSUPPORTED) {
    // No `head`, so no directory carried an em: the font program states its own.
    // A container whose tables are synthetic (a bare CFF, and Type 1 when it
    // arrives) reaches the em this way and through nothing else, which is why
    // this is here rather than in the CFF module - every caller that scales a
    // glyph asks this one function.
    const GFNT_Cff * cff = NULL;
    const GFNT_Type1 * type1 = NULL;
    size_t upem = 0;

    // A bitmap strike has no em, and this is the one accessor where saying so
    // matters: 1000 would be a number every scaling caller would then use, and
    // M9 - a strike scaled as though it were an outline - is exactly what comes
    // of that. The strike's own ppem, ascent and descent are in pixels and are
    // reached through ::gfnt_face_strike_at().
    if (gfnt_face_is_bitmap(face)) {
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, face->flavour, 0,
          GFNT_GLYPH_NONE,
          "a bitmap font, which was drawn at a pixel size and has no em to "
          "scale from; ask the strike for its ppem");
    }
    if (gfnt_sfnt_producer(face) == GFNT_PRODUCER_TYPE1) {
      if (gfnt_face_type1(face, &type1, NULL) == GFNT_OK
          && gfnt_type1_units_per_em(type1, &upem)) {
        gfnt_error_clear(error);
        *out_units = (uint16_t)upem;
        return GFNT_OK;
      }
      return result;
    }
    if (gfnt_face_cff(face, &cff, NULL) == GFNT_OK
        && gfnt_cff_units_per_em(cff, &upem)) {
      gfnt_error_clear(error);
      *out_units = (uint16_t)upem;
      return GFNT_OK;
    }
    return result;
  }
  if (result != GFNT_OK) {
    return result;
  }
  *out_units = head->units_per_em;
  return GFNT_OK;
}

/**
 * How many glyphs `hmtx` can actually answer for, given what `hhea` claims.
 *
 * `hmtx` is numberOfHMetrics long entries of four bytes, then one short entry
 * of two bytes per remaining glyph. Inverting that gives the largest glyph
 * index the table covers, which is the bound M12 wants.
 *
 * @return true if a bound was computed; false if `hmtx` or `hhea` is missing
 *   or says nothing useful, in which case it constrains nothing.
 */
static bool gfnt_hmtx_glyph_bound(const GFNT_Face * face, size_t * out_bound) {
  const GFNT_Hhea * hhea = NULL;
  size_t length = 0;
  size_t metrics;

  if (gfnt_face_hhea(face, &hhea, NULL) != GFNT_OK) {
    return false;
  }
  if (gfnt_face_table_range(face, GFNT_TAG('h', 'm', 't', 'x'), NULL, &length)
      != GFNT_OK) {
    return false;
  }
  metrics = hhea->number_of_h_metrics;
  if (metrics == 0) {
    // hhea claiming no horizontal metrics at all is a defect in hhea, and the
    // advance accessor reports it as one. It must not be allowed to bound the
    // glyph count to zero, which would make every other table unreadable too.
    return false;
  }

  if (length >= metrics * 4) {
    *out_bound = metrics + (length - metrics * 4) / 2;
  }
  else {
    // The table is shorter than hhea's own claim: only the complete long
    // entries are there.
    *out_bound = length / 4;
  }
  return true;
}

/**
 * Parse `maxp` for numGlyphs, then take the minimum with every other table
 * that indexes glyphs.
 */
static GFNT_Result gfnt_glyph_count_parse(const GFNT_Face * face, void * out,
    void * context,
    GFNT_Error * error) {
  // There is one of this table per face, so there is nothing to select; the
  // parameter is here because every memo parser shares one signature.
  (void)context;
  static const GFNT_Tag tag = GFNT_TAG('m', 'a', 'x', 'p');
  GFNT_GlyphCount * count = out;
  GFNT_Reader reader;
  GFNT_F16Dot16 version = 0;
  uint16_t num_glyphs = 0;
  size_t bound = 0;
  GFNT_Result result;

  result = gfnt_face_table_reader(face, tag, &reader, error);
  if (result == GFNT_ERR_UNSUPPORTED) {
    // No `maxp`: a container with no table directory of its own has nothing to
    // carry a count, and the one table that indexes glyphs *is* the count
    // rather than a minimum it could disagree with. `maxp` is left at zero
    // because zero is what it claimed - nothing - and writing the count there
    // would report an agreement between two sources when there was one source.
    // No cap is applied here, because there is nowhere for an uncapped count to
    // come from: gfnt_cff_parse() refuses a `CharStrings` INDEX with more
    // elements than GFNT_Limits::max_glyphs, so a bound that arrives here has
    // already been through it. A second check would be a line no input can
    // reach - and for a bare CFF, whose font program is parsed at load, the
    // refusal happens before there is a face to ask.
    if (gfnt_bitmap_glyph_bound(face, &bound)
        || gfnt_type1_glyph_bound(face, &bound)
        || gfnt_cff_glyph_bound(face, &bound)) {
      gfnt_error_clear(error);
      *count = (GFNT_GlyphCount) {
        .count = bound,
        .maxp = 0,
        .disagreement = false,
      };
      return GFNT_OK;
    }
    return result;
  }
  if (result != GFNT_OK) {
    return result;
  }
  // numGlyphs sits immediately after the version in both defined versions -
  // 0.5 for a CFF font and 1.0 for a TrueType one - and the fields that follow
  // it in 1.0 are advisory maxima nothing here reads. Recording them would be
  // recording constants no code consults.
  if (gfnt_read_fixed(&reader, &version) != GFNT_OK
      || gfnt_read_u16(&reader, &num_glyphs) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  if (num_glyphs > face->limits.max_glyphs) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, tag, 4, GFNT_GLYPH_NONE,
        "more glyphs than GFNT_Limits::max_glyphs");
  }

  *count = (GFNT_GlyphCount) {
    .count = num_glyphs,
    .maxp = num_glyphs,
    .disagreement = false,
  };

  // `hmtx` and CFF's `CharStrings` are the two that can disagree. `loca` is
  // deliberately **not** here: a `loca` entry that runs backwards or past `glyf`
  // condemns that one glyph and leaves the rest of the font answering (M11),
  // whereas a glyph past the end of `CharStrings` has no charstring at all -
  // there is nothing to condemn, so the count is what shrinks.
  if (gfnt_hmtx_glyph_bound(face, &bound) && bound < count->count) {
    count->count = bound;
    count->disagreement = true;
  }
  if (gfnt_cff_glyph_bound(face, &bound) && bound < count->count) {
    count->count = bound;
    count->disagreement = true;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_num_glyphs(const GFNT_Face * face, size_t * out_count,
    GFNT_Error * error) {
  GFNT_Face * owner = (GFNT_Face *)face;
  GFNT_GlyphCount scratch;
  GFNT_Result result;

  if (!face || !out_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the count");
  }
  result = gfnt_table_cached(face, &owner->glyph_count_state,
      &owner->glyph_count, &scratch, sizeof scratch, gfnt_glyph_count_parse,
      NULL, NULL, error);
  if (result == GFNT_OK) {
    *out_count = owner->glyph_count.count;
  }
  return result;
}

bool gfnt_face_num_glyphs_disagreement(const GFNT_Face * face,
    size_t * out_maxp) {
  size_t count = 0;

  if (!face) {
    return false;
  }
  if (gfnt_face_num_glyphs(face, &count, NULL) != GFNT_OK) {
    return false;
  }
  if (out_maxp) {
    *out_maxp = face->glyph_count.maxp;
  }
  return face->glyph_count.disagreement;
}

/**
 * One glyph's `hmtx` entry: its advance, its left side bearing, or both.
 */
static GFNT_Result gfnt_hmtx_metrics(const GFNT_Face * face, uint32_t glyph,
    uint16_t * out_advance, int16_t * out_bearing, GFNT_Error * error) {
  static const GFNT_Tag tag = GFNT_TAG('h', 'm', 't', 'x');
  const GFNT_Hhea * hhea = NULL;
  GFNT_Reader reader;
  size_t count = 0;
  size_t metrics;
  GFNT_Result result;

  result = gfnt_face_num_glyphs(face, &count, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (glyph >= count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, tag, 0, glyph,
        "no such glyph in this face");
  }
  result = gfnt_face_hhea(face, &hhea, error);
  if (result != GFNT_OK) {
    return result;
  }
  metrics = hhea->number_of_h_metrics;
  if (metrics == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG('h', 'h', 'e', 'a'),
        34, glyph, "hhea says the font has no horizontal metrics at all");
  }
  result = gfnt_face_table_reader(face, tag, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }

  if (out_advance) {
    // Past the end of the long entries, the last advance repeats: that is the
    // format's own compression for monospaced and CJK fonts, not a shortfall.
    size_t index = glyph < metrics ? glyph : metrics - 1;

    result = gfnt_reader_u16_at(&reader, index * 4, out_advance);
    if (result != GFNT_OK) {
      return result;
    }
  }
  if (out_bearing) {
    size_t offset = glyph < metrics
        ? glyph * 4 + 2
        : metrics * 4 + (glyph - metrics) * 2;

    result = gfnt_reader_s16_at(&reader, offset, out_bearing);
    if (result != GFNT_OK) {
      return result;
    }
  }
  return GFNT_OK;
}

/**
 * Refuse a variation that would move a metric, since none of them can.
 *
 * What moves an advance at a location is `HVAR`, or the phantom points `gvar`
 * carries when a font has none, and what moves a line's extent is `MVAR`. None of
 * the three is read. Answering the default's number for a location it is not at
 * would be the quiet wrong answer this parameter was added to prevent: text laid
 * out at weight 900 with the advances of weight 400, with no error anywhere. The
 * outline at the same location *is* honoured (`gvar`), so a caller who draws and
 * measures at one location gets a shape that moved and a width that would not
 * have - which is why this is a refusal and not a quiet default.
 *
 * A variation that moves nothing is the default instance and is answered as one.
 */
static GFNT_Result gfnt_metrics_refuse_variation(const GFNT_Face * face,
    uint32_t glyph, const GFNT_Variation * variation, GFNT_Error * error) {
  bool moved = false;
  GFNT_Result result = gfnt_variation_moves(face, glyph, variation, &moved,
      error);

  if (result != GFNT_OK || !moved) {
    return result;
  }
  return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, glyph,
      "this library reads no HVAR, MVAR or gvar phantom points, so a metric at "
      "a location other than the default would be the default's, quietly");
}

GFNT_Result gfnt_face_glyph_advance(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, int32_t * out_advance,
    GFNT_Error * error) {
  uint16_t advance = 0;
  GFNT_Result result;

  if (!face || !out_advance) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, glyph,
        "no face, or nowhere to put the advance");
  }
  // A bitmap strike's advance and side bearing are **pixels**, and this function's
  // unit is font units. Returning the number anyway would be the kind of quiet
  // unit error that shows up as text at four times the size it should be, so the
  // refusal names where the pixels are instead.
  if (gfnt_face_is_bitmap(face)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, face->flavour, 0, glyph,
        "a bitmap font, whose metrics are pixels rather than font units - "
        "gfnt_face_glyph_bitmap() carries each glyph's own");
  }
  result = gfnt_metrics_refuse_variation(face, glyph, variation, error);
  if (result != GFNT_OK) {
    return result;
  }

  result = gfnt_hmtx_metrics(face, glyph, &advance, NULL, error);
  if (result == GFNT_ERR_UNSUPPORTED
      && gfnt_sfnt_producer(face) == GFNT_PRODUCER_TYPE1) {
    // No `hmtx`, and there never is one: a Type 1 font states each glyph's
    // advance in that glyph's own `hsbw`, which is the first thing its charstring
    // does. So the charstring is the metric table here, and running it is the
    // only way to read it.
    GFNT_CharstringMetrics metrics;

    result = gfnt_type1_metrics(face, glyph, &metrics, error);
    if (result != GFNT_OK) {
      return result;
    }
    gfnt_error_clear(error);
    // 16.16 to whole font units, rounded the one way this library rounds.
    *out_advance = (int32_t)gfnt_round_shift(metrics.width, 16);
    return GFNT_OK;
  }
  if (result != GFNT_OK) {
    return result;
  }
  *out_advance = (int32_t)advance;
  return GFNT_OK;
}

GFNT_Result gfnt_face_glyph_side_bearing(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, int32_t * out_bearing,
    GFNT_Error * error) {
  int16_t bearing = 0;
  GFNT_Result result;

  if (!face || !out_bearing) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, glyph,
        "no face, or nowhere to put the side bearing");
  }
  // A bitmap strike's advance and side bearing are **pixels**, and this function's
  // unit is font units. Returning the number anyway would be the kind of quiet
  // unit error that shows up as text at four times the size it should be, so the
  // refusal names where the pixels are instead.
  if (gfnt_face_is_bitmap(face)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, face->flavour, 0, glyph,
        "a bitmap font, whose metrics are pixels rather than font units - "
        "gfnt_face_glyph_bitmap() carries each glyph's own");
  }
  result = gfnt_metrics_refuse_variation(face, glyph, variation, error);
  if (result != GFNT_OK) {
    return result;
  }

  result = gfnt_hmtx_metrics(face, glyph, NULL, &bearing, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_bearing = bearing;
  return GFNT_OK;
}

/**
 * The typographic metrics, from `OS/2`.
 */
static void gfnt_line_metrics_typo(const GFNT_Os2 * os2,
    GFNT_LineMetrics * out) {
  *out = (GFNT_LineMetrics) {
    .ascent = os2->typo_ascender,
    // Used as the file gives it. sTypoDescender is specified negative, and a
    // font that stores it positive is reported that way rather than silently
    // flipped: flipping would hide a defect no other implementation hides.
    .descent = os2->typo_descender,
    .line_gap = os2->typo_line_gap,
    .source = GFNT_LINE_METRICS_TYPO,
  };
}

/**
 * The window metrics, from `OS/2`. usWinDescent is unsigned and positive in
 * the file; this API's descent is negative, so it is negated here (M18).
 */
static void gfnt_line_metrics_win(const GFNT_Os2 * os2,
    GFNT_LineMetrics * out) {
  *out = (GFNT_LineMetrics) {
    .ascent = (int32_t)os2->win_ascent,
    .descent = -(int32_t)os2->win_descent,
    // The window metrics carry no gap of their own: Windows took its external
    // leading from hhea. Reporting zero says that rather than borrowing a
    // number from a table the caller did not ask for.
    .line_gap = 0,
    .source = GFNT_LINE_METRICS_WIN,
  };
}

GFNT_Result gfnt_face_line_metrics(const GFNT_Face * face,
    GFNT_LineMetricsPolicy policy, const GFNT_Variation * variation,
    GFNT_LineMetrics * out_metrics, GFNT_Error * error) {
  const GFNT_Os2 * os2 = NULL;
  const GFNT_Hhea * hhea = NULL;
  GFNT_Result result;

  if (!face || !out_metrics) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the metrics");
  }
  result = gfnt_metrics_refuse_variation(face, GFNT_GLYPH_NONE, variation, error);
  if (result != GFNT_OK) {
    return result;
  }

  switch (policy) {
    case GFNT_LINE_METRICS_TYPO:
      result = gfnt_face_os2(face, &os2, error);
      if (result != GFNT_OK) {
        return result;
      }
      gfnt_line_metrics_typo(os2, out_metrics);
      return GFNT_OK;

    case GFNT_LINE_METRICS_WIN:
      result = gfnt_face_os2(face, &os2, error);
      if (result != GFNT_OK) {
        return result;
      }
      gfnt_line_metrics_win(os2, out_metrics);
      return GFNT_OK;

    case GFNT_LINE_METRICS_HHEA:
      result = gfnt_face_hhea(face, &hhea, error);
      if (result != GFNT_OK) {
        return result;
      }
      *out_metrics = (GFNT_LineMetrics) {
        .ascent = hhea->ascender,
        .descent = hhea->descender,
        .line_gap = hhea->line_gap,
        .source = GFNT_LINE_METRICS_HHEA,
      };
      return GFNT_OK;

    case GFNT_LINE_METRICS_FONT:
      // The font's own request, which is what zero means (design.md section
      // 10.3): fsSelection bit 7 is the font saying "use my typographic
      // metrics", and a font that does not set it wanted hhea's.
      if (gfnt_face_os2(face, &os2, NULL) == GFNT_OK
          && (os2->fs_selection & GFNT_OS2_USE_TYPO_METRICS) != 0) {
        gfnt_line_metrics_typo(os2, out_metrics);
        return GFNT_OK;
      }
      if (gfnt_face_hhea(face, &hhea, NULL) == GFNT_OK) {
        *out_metrics = (GFNT_LineMetrics) {
          .ascent = hhea->ascender,
          .descent = hhea->descender,
          .line_gap = hhea->line_gap,
          .source = GFNT_LINE_METRICS_HHEA,
        };
        return GFNT_OK;
      }
      if (os2) {
        gfnt_line_metrics_win(os2, out_metrics);
        return GFNT_OK;
      }
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
          "the font has neither hhea nor OS/2, so it states no line height");

    default:
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "not a line-metrics policy this library defines");
  }
}
