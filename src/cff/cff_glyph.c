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
 * One CFF glyph: which program draws it, which subroutines it may call, and
 * which Private DICT's widths apply to it.
 *
 * documentation/design.md section 7.4. Everything interesting here is a
 * *binding*: the interpreter knows nothing about CFF, so this file is what turns
 * "subroutine 42" into bytes and "Standard Encoding code 65" into a glyph.
 */

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/metrics.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "../charstring/charstring.h"
#include "../core/fixed.h"
#include "../outline/outline.h"
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "cff_glyph.h"
#include "cff_strings.h"

/** What one INDEX's element accessor needs: the table, and which INDEX. */
typedef struct GFNT_CffSubrs {
  GFNT_Reader table;      ///< A reader over the whole `CFF ` table.
  GFNT_CffIndex index;    ///< The INDEX the subroutines are in.
} GFNT_CffSubrs;

/** What the `seac` resolver needs, which is the whole font. */
typedef struct GFNT_CffSeac {
  const GFNT_Face * face; ///< The face.
  const GFNT_Cff * cff;   ///< Its parsed CFF.
  GFNT_Reader table;      ///< A reader over the `CFF ` table.
} GFNT_CffSeac;

/** Borrow one INDEX element's bytes, for ::GFNT_CharstringSubrs::at. */
static GFNT_Result gfnt_cff_element_bytes(const GFNT_Reader * table,
    const GFNT_CffIndex * index, uint32_t element, const uint8_t ** out_bytes,
    size_t * out_length) {
  GFNT_Reader reader;
  size_t length;
  GFNT_Result result = gfnt_cff_index_at(table, index, element, &reader, NULL);

  if (result != GFNT_OK) {
    return result;
  }
  length = gfnt_reader_remaining(&reader);
  *out_length = length;
  *out_bytes = NULL;
  if (length == 0) {
    // A zero-length charstring draws nothing, which `space` in a subsetted font
    // legitimately is. gfnt_read_bytes writes NULL for a zero-length request,
    // and the interpreter accepts that pair.
    return GFNT_OK;
  }
  return gfnt_read_bytes(&reader, length, out_bytes);
}

static GFNT_Result gfnt_cff_subr_at(void * user, uint32_t index,
    const uint8_t ** out_bytes, size_t * out_length) {
  GFNT_CffSubrs * subrs = (GFNT_CffSubrs *)user;

  return gfnt_cff_element_bytes(&subrs->table, &subrs->index, index, out_bytes,
      out_length);
}

/**
 * The charstring of the glyph a Standard Encoding code names.
 *
 * Two lookups, and the first is the one that is easy to get wrong: the code is
 * **not** an index into the font's own encoding. It names a glyph *name* through
 * the Standard Encoding, and that name is what the charset is searched for. A
 * re-encoded font - which is most of the interesting ones - maps the same code
 * to something else entirely.
 */
static GFNT_Result gfnt_cff_standard_code(void * user, uint8_t code,
    const uint8_t ** out_bytes, size_t * out_length) {
  GFNT_CffSeac * seac = (GFNT_CffSeac *)user;
  uint32_t glyph = 0;
  uint32_t sid = gfnt_cff_standard_encoding[code];
  GFNT_Result result;

  if (sid == 0) {
    return GFNT_ERR_CORRUPT;
  }
  result = gfnt_cff_glyph_for_sid(seac->face, seac->cff, sid, &glyph, NULL);
  if (result != GFNT_OK) {
    return result;
  }
  return gfnt_cff_element_bytes(&seac->table, &seac->cff->charstrings, glyph,
      out_bytes, out_length);
}

/**
 * Whether the `FontMatrix` and `head.unitsPerEm` describe the same em.
 *
 * A charstring's coordinates are in the font's own charstring space, and the
 * `FontMatrix` is what maps that space to the em. For every font anybody ships
 * the two agree - a 1000-unit CFF says 0.001 - and this library reports the
 * charstring's own coordinates, which is also what every reference pen reports.
 *
 * A font where they disagree would need the matrix applied, and applying it
 * would put this library and the reference in different spaces while agreeing
 * about nothing. So it is refused by name. The comparison is deliberately loose
 * in the last bits: `0.001` becomes 65 or 66 in 16.16 depending on how it was
 * written, and neither is 1/1000 exactly, so an exact test would refuse every
 * font in the world.
 */
static bool gfnt_cff_matrix_agrees(const GFNT_Cff * cff, size_t upem) {
  int64_t scale = cff->font_matrix[0];
  int64_t implied;

  if (!cff->font_matrix_stated) {
    return true;
  }
  if (cff->font_matrix[1] != 0 || cff->font_matrix[2] != 0
      || cff->font_matrix[4] != 0 || cff->font_matrix[5] != 0) {
    return false;
  }
  if (scale != cff->font_matrix[3] || scale <= 0 || upem == 0) {
    return false;
  }
  implied = scale * (int64_t)upem;
  // One 16.16 unit per em unit of slack, which is what the decimal-to-binary
  // conversion of 1/upem can cost and no more.
  return implied >= GFNT_F16DOT16_ONE - (int64_t)upem
      && implied <= GFNT_F16DOT16_ONE + (int64_t)upem;
}

/** Set up everything one glyph's run needs. */
static GFNT_Result gfnt_cff_context(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Cff ** out_cff, GFNT_CffSubrs * local, GFNT_CffSubrs * global,
    GFNT_CffSeac * seac, GFNT_CharstringContext * context,
    GFNT_CharstringType * out_type, GFNT_Error * error) {
  const GFNT_Cff * cff = NULL;
  const GFNT_Head * head = NULL;
  GFNT_CffPrivate priv;
  size_t glyphs = 0;
  GFNT_Result result;

  result = gfnt_face_cff(face, &cff, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_face_num_glyphs(face, &glyphs, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (glyph >= glyphs) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_CFF, 0, glyph,
        "this glyph index is past the face's glyph count");
  }
  result = gfnt_face_head(face, &head, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (!gfnt_cff_matrix_agrees(cff, head->units_per_em)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF, 0, glyph,
        "a CFF FontMatrix that is not head.unitsPerEm's own scale, so this "
        "font's charstring coordinates are in a space this library would have "
        "to transform them out of rather than report");
  }
  result = gfnt_cff_private_for_glyph(face, cff, glyph, &priv, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_CFF, &local->table, error);
  if (result != GFNT_OK) {
    return result;
  }
  global->table = local->table;
  seac->table = local->table;
  local->index = priv.subrs;
  global->index = cff->gsubrs;
  seac->face = face;
  seac->cff = cff;

  memset(context, 0, sizeof *context);
  context->local.at = gfnt_cff_subr_at;
  context->local.user = local;
  context->local.count = priv.subrs.count;
  context->global.at = gfnt_cff_subr_at;
  context->global.user = global;
  context->global.count = cff->gsubrs.count;
  context->standard_code = gfnt_cff_standard_code;
  context->standard_user = seac;
  context->nominal_width = priv.nominal_width;
  context->default_width = priv.default_width;
  context->limits = &face->limits;

  // CharstringType 1 means Type 1 programs inside a CFF container - rare, and
  // the format allows it, so the language comes from the DICT rather than from
  // the table's name.
  *out_type = cff->charstring_type == 1
      ? GFNT_CHARSTRING_TYPE1 : GFNT_CHARSTRING_TYPE2;
  *out_cff = cff;
  return GFNT_OK;
}

GFNT_Result gfnt_cff_load(const GFNT_Face * face, uint32_t glyph,
    GFNT_Outline * outline, GFNT_CharstringMetrics * out_metrics,
    GFNT_Error * error) {
  const GFNT_Cff * cff = NULL;
  GFNT_CffSubrs local;
  GFNT_CffSubrs global;
  GFNT_CffSeac seac;
  GFNT_CharstringContext context;
  GFNT_CharstringType type = GFNT_CHARSTRING_TYPE2;
  const uint8_t * bytes = NULL;
  size_t length = 0;
  GFNT_Result result;

  if (!face || !outline) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_cff_context(face, glyph, &cff, &local, &global, &seac, &context,
      &type, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_cff_element_bytes(&local.table, &cff->charstrings, glyph,
      &bytes, &length);
  if (result != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, glyph,
        "a CharStrings entry whose bytes are not in the table");
  }
  return gfnt_charstring_run(type, bytes, length, &context, outline,
      out_metrics, error);
}

GFNT_Result gfnt_cff_is_composite(const GFNT_Face * face, uint32_t glyph,
    bool * out_composite, GFNT_Error * error) {
  GFNT_Outline * outline = NULL;
  GFNT_CharstringMetrics metrics;
  GFNT_Result result;

  if (!face || !out_composite) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_outline_create(face->allocator, &outline, error);
  if (result != GFNT_OK) {
    return result;
  }
  gfnt_outline_set_limits(outline, &face->limits);
  memset(&metrics, 0, sizeof metrics);
  result = gfnt_cff_load(face, glyph, outline, &metrics, error);
  gfnt_outline_destroy(outline);
  if (result != GFNT_OK) {
    return result;
  }
  *out_composite = metrics.seac;
  return GFNT_OK;
}

GFNT_Result gfnt_face_glyph_charstring(const GFNT_Face * face, uint32_t glyph,
    GFNT_CharstringType * out_type, const uint8_t ** out_bytes,
    size_t * out_length, GFNT_Error * error) {
  const GFNT_Cff * cff = NULL;
  GFNT_Reader table;
  size_t glyphs = 0;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_bytes || !out_length) {
    return GFNT_ERR_INVALID;
  }
  if (gfnt_sfnt_producer(face) != GFNT_PRODUCER_CFF) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, glyph,
        "this face's glyphs are not charstrings");
  }
  result = gfnt_face_cff(face, &cff, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_face_num_glyphs(face, &glyphs, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (glyph >= glyphs) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_CFF, 0, glyph,
        "this glyph index is past the face's glyph count");
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_CFF, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_cff_element_bytes(&table, &cff->charstrings, glyph, out_bytes,
      out_length);
  if (result != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, glyph,
        "a CharStrings entry whose bytes are not in the table");
  }
  if (out_type) {
    *out_type = cff->charstring_type == 1
        ? GFNT_CHARSTRING_TYPE1 : GFNT_CHARSTRING_TYPE2;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_glyph_charstring_metrics(const GFNT_Face * face,
    uint32_t glyph, GFNT_CharstringMetrics * out_metrics, GFNT_Error * error) {
  GFNT_Outline * outline = NULL;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_metrics) {
    return GFNT_ERR_INVALID;
  }
  if (gfnt_sfnt_producer(face) != GFNT_PRODUCER_CFF) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, glyph,
        "this face's glyphs are not charstrings, so nothing about them states "
        "an advance; gfnt_face_glyph_advance() reads the metric table");
  }
  result = gfnt_outline_create(face->allocator, &outline, error);
  if (result != GFNT_OK) {
    return result;
  }
  gfnt_outline_set_limits(outline, &face->limits);
  memset(out_metrics, 0, sizeof *out_metrics);
  result = gfnt_cff_load(face, glyph, outline, out_metrics, error);
  gfnt_outline_destroy(outline);
  return result;
}
