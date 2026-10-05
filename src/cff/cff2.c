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
 * The `CFF2` container. See `cff2.h` for what differs from a CFF.
 */

#include <ghoti.io/font/macros.h>
#include <string.h>
#include "../core/fixed.h"
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"
#include "cff2.h"

// Top DICT operators this library reads. `vstore` is the one a CFF does not have.
#define GFNT_CFF2_OP_CHARSTRINGS 17
#define GFNT_CFF2_OP_VSTORE 24
#define GFNT_CFF2_OP_FONT_MATRIX 0x0c07
#define GFNT_CFF2_OP_FDARRAY 0x0c24
#define GFNT_CFF2_OP_FDSELECT 0x0c25
// Font DICT and Private DICT operators.
#define GFNT_CFF2_OP_PRIVATE 18
#define GFNT_CFF2_OP_SUBRS 19
#define GFNT_CFF2_OP_VSINDEX 22

/** What the Top DICT walk fills in. */
typedef struct GFNT_Cff2Top {
  GFNT_Cff2 * cff2;
  size_t charstrings;   ///< Where `CharStrings` is; 0 when not stated.
  size_t fdarray;       ///< Where `FDArray` is; 0 when not stated.
  size_t vstore;        ///< Where `vstore` is; 0 when not stated.
} GFNT_Cff2Top;

static GFNT_Result gfnt_cff2_top_visit(void * user, const GFNT_CffDictOp * entry,
    GFNT_Error * error) {
  GFNT_Cff2Top * top = (GFNT_Cff2Top *)user;
  GFNT_Cff2 * cff2 = top->cff2;

  switch (entry->op) {
    case GFNT_CFF2_OP_CHARSTRINGS:
      return gfnt_cff_offset_operand(entry, GFNT_TAG_CFF2, 0, cff2->length,
          &top->charstrings, error);
    case GFNT_CFF2_OP_FDARRAY:
      return gfnt_cff_offset_operand(entry, GFNT_TAG_CFF2, 0, cff2->length,
          &top->fdarray, error);
    case GFNT_CFF2_OP_FDSELECT:
      return gfnt_cff_offset_operand(entry, GFNT_TAG_CFF2, 0, cff2->length,
          &cff2->fdselect, error);
    case GFNT_CFF2_OP_VSTORE:
      return gfnt_cff_offset_operand(entry, GFNT_TAG_CFF2, 0, cff2->length,
          &top->vstore, error);
    case GFNT_CFF2_OP_FONT_MATRIX:
      if (entry->count < 6) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF2, 0,
            GFNT_GLYPH_NONE, "a FontMatrix with fewer than six numbers");
      }
      for (size_t i = 0; i < 6; ++i) {
        cff2->font_matrix[i] = gfnt_saturate32(entry->values[i]);
      }
      cff2->font_matrix_stated = true;
      return GFNT_OK;
    default:
      // The `blend` operator, and everything else a Top DICT may carry, is
      // metadata that nothing here reads.
      return GFNT_OK;
  }
}

/** Parse the INDEX at @p offset into @p out, rebasing it to table coordinates. */
static GFNT_Result gfnt_cff2_index_at(const GFNT_Reader * table, size_t offset,
    GFNT_CffIndex * out, GFNT_Error * error) {
  GFNT_Reader reader;
  GFNT_Result result = gfnt_reader_sub(table, offset, GFNT_READER_REST, &reader);

  if (result == GFNT_OK) {
    result = gfnt_cff_index_parse_ex(&reader, true, GFNT_TAG_CFF2, out, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  // The sub-reader began at the INDEX, so its offsets are relative to that; the
  // rest of this file works in table coordinates.
  out->offsets += offset;
  out->data_base += offset;
  out->end += offset;
  return GFNT_OK;
}

GFNT_Result gfnt_cff2_parse(const GFNT_Face * face, void * out, void * context,
    GFNT_Error * error) {
  (void)context;
  GFNT_Cff2 * cff2 = (GFNT_Cff2 *)out;
  GFNT_Cff2Top top;
  GFNT_Reader table;
  GFNT_Reader dict;
  uint8_t major = 0;
  uint8_t header_size = 0;
  uint16_t top_length = 0;
  GFNT_Result result;

  memset(cff2, 0, sizeof *cff2);
  memset(&top, 0, sizeof top);
  top.cff2 = cff2;
  result = gfnt_face_table_reader(face, GFNT_TAG_CFF2, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  cff2->length = table.length;
  table.error = error;
  if (table.length < GFNT_CFF2_HEADER_BYTES
      || gfnt_reader_u8_at(&table, 0, &major) != GFNT_OK
      || gfnt_reader_u8_at(&table, 1, &cff2->minor) != GFNT_OK
      || gfnt_reader_u8_at(&table, 2, &header_size) != GFNT_OK
      || gfnt_reader_u16_at(&table, 3, &top_length) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF2, 0,
        GFNT_GLYPH_NONE, "a CFF2 shorter than its own header");
  }
  if (major != 2) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF2, 0,
        GFNT_GLYPH_NONE, "a CFF2 whose major version is not 2");
  }
  if (header_size < GFNT_CFF2_HEADER_BYTES) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF2, 2,
        GFNT_GLYPH_NONE, "a CFF2 header shorter than the five bytes it always has");
  }
  result = gfnt_reader_sub(&table, header_size, top_length, &dict);
  if (result != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF2, 3,
        GFNT_GLYPH_NONE, "a CFF2 whose Top DICT runs past the table");
  }
  result = gfnt_cff_dict_walk(&dict, GFNT_TAG_CFF2, GFNT_CFF2_DICT_MAX_OPERANDS,
      gfnt_cff2_top_visit, &top, error);
  if (result != GFNT_OK) {
    return result;
  }
  // The global subroutines follow the Top DICT directly, with no offset to say so.
  result = gfnt_cff2_index_at(&table, (size_t)header_size + top_length,
      &cff2->gsubrs, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (top.charstrings == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF2, 0,
        GFNT_GLYPH_NONE,
        "a CFF2 Top DICT with no CharStrings offset, so the font has no glyphs "
        "to find");
  }
  if (top.fdarray == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF2, 0,
        GFNT_GLYPH_NONE,
        "a CFF2 Top DICT with no FDArray, though every CFF2 glyph has a Font DICT");
  }
  result = gfnt_cff2_index_at(&table, top.charstrings, &cff2->charstrings, error);
  if (result == GFNT_OK) {
    result = gfnt_cff2_index_at(&table, top.fdarray, &cff2->fdarray, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (cff2->charstrings.count > face->limits.max_glyphs) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_CFF2, 0,
        GFNT_GLYPH_NONE, "more charstrings than GFNT_Limits::max_glyphs");
  }
  if (cff2->fdarray.count < 1) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF2, top.fdarray,
        GFNT_GLYPH_NONE, "a CFF2 FDArray with no Font DICT");
  }
  if (top.vstore != 0) {
    uint16_t length = 0;

    if (gfnt_reader_u16_at(&table, top.vstore, &length) != GFNT_OK
        || (size_t)length > table.length - top.vstore - 2u) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF2, top.vstore,
          GFNT_GLYPH_NONE, "a CFF2 variation store that runs past the table");
    }
    cff2->vstore = top.vstore + 2u;
    cff2->vstore_length = length;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_cff2(const GFNT_Face * face, const GFNT_Cff2 ** out_cff2,
    GFNT_Error * error) {
  GFNT_Face * owner = (GFNT_Face *)face;
  GFNT_Cff2 scratch;
  GFNT_Result result;

  if (!face || !out_cff2) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_table_cached(face, &owner->cff2_state, &owner->cff2, &scratch,
      sizeof scratch, gfnt_cff2_parse, NULL, NULL, error);
  if (result == GFNT_OK) {
    *out_cff2 = &owner->cff2;
  }
  return result;
}

bool gfnt_cff2_glyph_bound(const GFNT_Face * face, size_t * out_bound) {
  const GFNT_Cff2 * cff2 = NULL;

  if (!gfnt_sfnt_find(face, GFNT_TAG_CFF2)) {
    return false;
  }
  if (gfnt_face_cff2(face, &cff2, NULL) != GFNT_OK) {
    return false;
  }
  *out_bound = cff2->charstrings.count;
  return true;
}

/** Which Font DICT a glyph belongs to, per `FDSelect` (formats 0, 3 and 4). */
static GFNT_Result gfnt_cff2_fd_for_glyph(const GFNT_Reader * table,
    const GFNT_Cff2 * cff2, uint32_t glyph, uint32_t * out_fd,
    GFNT_Error * error) {
  GFNT_Reader select;
  uint8_t format = 0;
  GFNT_Result result;

  if (cff2->fdselect == 0) {
    // Without a selector every glyph is in the first Font DICT.
    *out_fd = 0;
    return GFNT_OK;
  }
  result = gfnt_reader_sub(table, cff2->fdselect, GFNT_READER_REST, &select);
  if (result == GFNT_OK) {
    result = gfnt_read_u8(&select, &format);
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (format == 0) {
    uint8_t fd = 0;

    result = gfnt_reader_u8_at(&select, 1u + glyph, &fd);
    if (result != GFNT_OK) {
      return result;
    }
    *out_fd = fd;
    return GFNT_OK;
  }
  if (format == 3 || format == 4) {
    // Ranges of (first glyph, Font DICT) and then a sentinel first glyph: format 3
    // in 16-bit and 8-bit fields, format 4 in 32-bit and 16-bit ones.
    const bool wide = format == 4;
    const size_t record = wide ? 6u : 3u;
    const size_t header = wide ? 4u : 2u;
    uint32_t ranges = 0;
    uint32_t first = 0;

    if (wide) {
      result = gfnt_reader_u32_at(&select, 1, &ranges);
    }
    else {
      uint16_t narrow = 0;

      result = gfnt_reader_u16_at(&select, 1, &narrow);
      ranges = narrow;
    }
    for (size_t i = 0; result == GFNT_OK && i < ranges; ++i) {
      uint32_t fd = 0;
      uint32_t next = 0;
      size_t at = 1u + header + i * record;

      if (wide) {
        uint16_t narrow_fd = 0;

        result = gfnt_reader_u32_at(&select, at, &first);
        if (result == GFNT_OK) {
          result = gfnt_reader_u16_at(&select, at + 4u, &narrow_fd);
        }
        if (result == GFNT_OK) {
          result = gfnt_reader_u32_at(&select, at + record, &next);
        }
        fd = narrow_fd;
      }
      else {
        uint16_t narrow_first = 0;
        uint16_t narrow_next = 0;
        uint8_t narrow_fd = 0;

        result = gfnt_reader_u16_at(&select, at, &narrow_first);
        if (result == GFNT_OK) {
          result = gfnt_reader_u8_at(&select, at + 2u, &narrow_fd);
        }
        if (result == GFNT_OK) {
          result = gfnt_reader_u16_at(&select, at + record, &narrow_next);
        }
        first = narrow_first;
        next = narrow_next;
        fd = narrow_fd;
      }
      if (result != GFNT_OK) {
        break;
      }
      if (glyph >= first && glyph < next) {
        *out_fd = fd;
        return GFNT_OK;
      }
    }
    if (result != GFNT_OK) {
      return result;
    }
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF2, cff2->fdselect,
        glyph, "an FDSelect whose ranges do not cover this glyph");
  }
  return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF2,
      cff2->fdselect, GFNT_GLYPH_NONE,
      "an FDSelect format other than 0, 3 and 4");
}

/** A Font DICT's Private entry. */
typedef struct GFNT_Cff2FontDict {
  size_t size;
  size_t offset;
  bool stated;
  size_t limit;
} GFNT_Cff2FontDict;

static GFNT_Result gfnt_cff2_font_visit(void * user, const GFNT_CffDictOp * entry,
    GFNT_Error * error) {
  GFNT_Cff2FontDict * state = (GFNT_Cff2FontDict *)user;
  GFNT_Result result;

  if (entry->op != GFNT_CFF2_OP_PRIVATE) {
    return GFNT_OK;
  }
  if (entry->count < 2) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF2, 0,
        GFNT_GLYPH_NONE,
        "a Font DICT whose Private entry lacks its size or its offset");
  }
  result = gfnt_cff_offset_operand(entry, GFNT_TAG_CFF2, 0, state->limit,
      &state->size, error);
  if (result == GFNT_OK) {
    result = gfnt_cff_offset_operand(entry, GFNT_TAG_CFF2, 1, state->limit,
        &state->offset, error);
  }
  state->stated = result == GFNT_OK;
  return result;
}

/** What a Private DICT says that a charstring needs. */
typedef struct GFNT_Cff2PrivateState {
  size_t limit;
  size_t subrs_offset;   ///< Relative to the Private DICT; 0 when absent.
  uint32_t vsindex;
} GFNT_Cff2PrivateState;

static GFNT_Result gfnt_cff2_private_visit(void * user,
    const GFNT_CffDictOp * entry, GFNT_Error * error) {
  GFNT_Cff2PrivateState * state = (GFNT_Cff2PrivateState *)user;

  switch (entry->op) {
    case GFNT_CFF2_OP_SUBRS:
      return gfnt_cff_offset_operand(entry, GFNT_TAG_CFF2, 0, state->limit,
          &state->subrs_offset, error);
    case GFNT_CFF2_OP_VSINDEX: {
      size_t index = 0;
      GFNT_Result result = gfnt_cff_offset_operand(entry, GFNT_TAG_CFF2, 0,
          0xFFFF, &index, error);

      state->vsindex = (uint32_t)index;
      return result;
    }
    default:
      // The blue values, the stem widths and `blend`, which can carry them: hinting
      // parameters, and there is no hinter.
      return GFNT_OK;
  }
}

GFNT_Result gfnt_cff2_font_for_glyph(const GFNT_Face * face,
    const GFNT_Cff2 * cff2, uint32_t glyph, GFNT_Cff2Font * out_font,
    GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_Reader font_dict;
  GFNT_Reader private_dict;
  GFNT_Cff2FontDict font;
  GFNT_Cff2PrivateState state;
  uint32_t fd = 0;
  GFNT_Result result;

  if (!face || !cff2 || !out_font) {
    return GFNT_ERR_INVALID;
  }
  memset(out_font, 0, sizeof *out_font);
  result = gfnt_face_table_reader(face, GFNT_TAG_CFF2, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_cff2_fd_for_glyph(&table, cff2, glyph, &fd, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (fd >= cff2->fdarray.count) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF2, cff2->fdselect,
        glyph, "an FDSelect naming a Font DICT the FDArray does not have");
  }
  result = gfnt_cff_index_at(&table, &cff2->fdarray, fd, &font_dict, error);
  if (result != GFNT_OK) {
    return result;
  }
  memset(&font, 0, sizeof font);
  font.limit = table.length;
  result = gfnt_cff_dict_walk(&font_dict, GFNT_TAG_CFF2,
      GFNT_CFF2_DICT_MAX_OPERANDS, gfnt_cff2_font_visit, &font, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (!font.stated) {
    // No Private DICT: no local subroutines and the default `vsindex` of zero.
    return GFNT_OK;
  }
  result = gfnt_reader_sub(&table, font.offset, font.size, &private_dict);
  if (result != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF2, font.offset,
        glyph, "a Private DICT that runs past the table");
  }
  memset(&state, 0, sizeof state);
  state.limit = table.length;
  result = gfnt_cff_dict_walk(&private_dict, GFNT_TAG_CFF2,
      GFNT_CFF2_DICT_MAX_OPERANDS, gfnt_cff2_private_visit, &state, error);
  if (result != GFNT_OK) {
    return result;
  }
  out_font->vsindex = state.vsindex;
  if (state.subrs_offset > 0) {
    // Relative to the Private DICT, as in a CFF.
    result = gfnt_cff2_index_at(&table, font.offset + state.subrs_offset,
        &out_font->subrs, error);
    if (result != GFNT_OK) {
      return result;
    }
  }
  return GFNT_OK;
}
