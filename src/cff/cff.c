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
 * The CFF container: the header, the INDEX structures, the DICTs, the charset,
 * the encoding, and `FDSelect`.
 *
 * documentation/design.md section 7.4. What this file does *not* do is draw
 * anything: it finds bytes. `charstring.c` interprets them and `cff_glyph.c`
 * joins the two.
 *
 * Two things about a CFF are unlike every table this library read before phase
 * 2, and both are places a reader goes wrong quietly:
 *
 *   * **An INDEX's offsets are one-based from the byte before its data.** Not
 *     from the table, not from the INDEX, and not zero-based. An off-by-one here
 *     shifts every element by one byte, which for a charstring means reading its
 *     last operand as its first operator.
 *   * **A Private DICT's `Subrs` offset is relative to the Private DICT**, while
 *     every other offset in the font is relative to the table. A reader that
 *     treats them alike finds a plausible INDEX somewhere else in the font and
 *     draws nonsense from it.
 *
 * Reference: Adobe *The Compact Font Format Specification* (technical note
 * #5176), sections 5 (INDEX), 4 (DICT), 13 (charsets), 12 (encodings) and 19
 * (FDSelect).
 */

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/metrics.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "../core/fixed.h"
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"
#include "cff.h"
#include "cff_strings.h"

/** Top DICT operators this library reads. One-byte. */
#define GFNT_CFF_OP_CHARSET 15
#define GFNT_CFF_OP_ENCODING 16
#define GFNT_CFF_OP_CHARSTRINGS 17
#define GFNT_CFF_OP_PRIVATE 18
/** Private DICT operators this library reads. One-byte. */
#define GFNT_CFF_OP_SUBRS 19
#define GFNT_CFF_OP_DEFAULT_WIDTH_X 20
#define GFNT_CFF_OP_NOMINAL_WIDTH_X 21

/** Two-byte operators, spelled as 0x0c00 | the second byte. */
#define GFNT_CFF_OP_CHARSTRING_TYPE 0x0c06
#define GFNT_CFF_OP_FONT_MATRIX 0x0c07
#define GFNT_CFF_OP_ROS 0x0c1e
#define GFNT_CFF_OP_FDARRAY 0x0c24
#define GFNT_CFF_OP_FDSELECT 0x0c25

/** The escape byte that introduces a two-byte DICT operator. */
#define GFNT_CFF_DICT_ESCAPE 12

/** One DICT operator and the operands that preceded it. */
typedef struct GFNT_CffDictOp {
  uint16_t op;                                  ///< 0-21, or 0x0c00 | n.
  size_t count;                                 ///< Operands given.
  int64_t values[GFNT_CFF_DICT_MAX_OPERANDS];   ///< 16.16.
} GFNT_CffDictOp;

/** What ::gfnt_cff_dict_walk() calls for each operator. */
typedef GFNT_Result (*GFNT_CffDictVisit)(void * user,
    const GFNT_CffDictOp * entry, GFNT_Error * error);

GFNT_Result gfnt_cff_index_parse(GFNT_Reader * reader,
    GFNT_CffIndex * out_index, GFNT_Error * error) {
  uint16_t count = 0;
  uint8_t off_size = 0;
  size_t offsets;
  size_t data_base;
  size_t last = 0;
  size_t index;
  GFNT_Result result;

  if (!reader || !out_index) {
    return GFNT_ERR_INVALID;
  }
  memset(out_index, 0, sizeof *out_index);
  result = gfnt_read_u16(reader, &count);
  if (result != GFNT_OK) {
    return result;
  }
  if (count == 0) {
    // An empty INDEX is its count and nothing else - no offset size, no
    // sentinel offset. A reader that always read three bytes would take the
    // next structure's first byte for an offset size.
    out_index->end = gfnt_reader_tell(reader);
    return GFNT_OK;
  }
  result = gfnt_read_u8(reader, &off_size);
  if (result != GFNT_OK) {
    return result;
  }
  if (off_size < 1 || off_size > 4) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF,
        gfnt_reader_tell(reader) - 1, GFNT_GLYPH_NONE,
        "an INDEX whose offsets are neither one, two, three nor four bytes");
  }
  offsets = gfnt_reader_tell(reader);
  // count + 1 offsets: the extra one is where the data ends.
  if (!gfnt_reader_has(reader, ((size_t)count + 1) * off_size)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, offsets,
        GFNT_GLYPH_NONE,
        "an INDEX whose offset array does not fit in the table");
  }
  data_base = offsets + ((size_t)count + 1) * off_size - 1;

  // The last offset says where the data ends, which is what makes the whole
  // INDEX's extent knowable without walking every element.
  for (index = 0; index < off_size; index++) {
    uint8_t byte = 0;

    result = gfnt_reader_u8_at(reader,
        offsets + (size_t)count * off_size + index, &byte);
    if (result != GFNT_OK) {
      return result;
    }
    last = (last << 8) | byte;
  }
  if (last < 1) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, offsets,
        GFNT_GLYPH_NONE,
        "an INDEX whose final offset is below one, which the one-based "
        "numbering makes impossible");
  }
  out_index->count = count;
  out_index->off_size = off_size;
  out_index->offsets = offsets;
  out_index->data_base = data_base;
  out_index->end = data_base + last;
  if (out_index->end > reader->length || out_index->end < data_base) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, offsets,
        GFNT_GLYPH_NONE, "an INDEX whose data runs past the end of the table");
  }
  return gfnt_reader_seek(reader, out_index->end);
}

/** One offset out of an INDEX's offset array. */
static GFNT_Result gfnt_cff_offset_at(const GFNT_Reader * table,
    const GFNT_CffIndex * index, uint32_t which, size_t * out_offset) {
  size_t value = 0;
  size_t byte_index;

  for (byte_index = 0; byte_index < index->off_size; byte_index++) {
    uint8_t byte = 0;
    GFNT_Result result = gfnt_reader_u8_at(table,
        index->offsets + (size_t)which * index->off_size + byte_index, &byte);

    if (result != GFNT_OK) {
      return result;
    }
    value = (value << 8) | byte;
  }
  *out_offset = value;
  return GFNT_OK;
}

GFNT_Result gfnt_cff_index_at(const GFNT_Reader * table,
    const GFNT_CffIndex * index, uint32_t element, GFNT_Reader * out_reader,
    GFNT_Error * error) {
  size_t start = 0;
  size_t end = 0;
  GFNT_Result result;

  if (!table || !index || !out_reader) {
    return GFNT_ERR_INVALID;
  }
  if (element >= index->count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_CFF, 0,
        GFNT_GLYPH_NONE, "an INDEX element this INDEX does not have");
  }
  result = gfnt_cff_offset_at(table, index, element, &start);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_cff_offset_at(table, index, element + 1, &end);
  if (result != GFNT_OK) {
    return result;
  }
  if (start < 1 || end < start) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF,
        index->offsets, GFNT_GLYPH_NONE,
        "an INDEX element whose offsets run backwards");
  }
  return gfnt_reader_sub(table, index->data_base + start, end - start,
      out_reader);
}

/**
 * A DICT real number, as a 16.16, computed on integers.
 *
 * Nibble-encoded decimal: digits, a point, an exponent and a terminator. The
 * arithmetic is deliberately integer - a `strtod` here would put a `float` in
 * the path between a font's bytes and a pixel, which design.md section 1
 * forbids, and would read a comma locale's decimal point wrongly besides.
 */
static GFNT_Result gfnt_cff_real(GFNT_Reader * reader, int64_t * out_value,
    GFNT_Error * error) {
  int64_t mantissa = 0;
  int digits = 0;
  int fraction_digits = 0;
  int exponent = 0;
  int exponent_sign = 1;
  bool in_exponent = false;
  bool negative = false;
  bool saw_point = false;
  bool done = false;
  int64_t value;
  int scale;

  while (!done) {
    uint8_t byte = 0;
    int half;
    GFNT_Result result = gfnt_read_u8(reader, &byte);

    if (result != GFNT_OK) {
      return result;
    }
    for (half = 0; half < 2 && !done; half++) {
      uint8_t nibble = half == 0 ? (byte >> 4) : (byte & 0x0F);

      if (nibble <= 9) {
        if (in_exponent) {
          if (exponent < 1000) {
            exponent = exponent * 10 + nibble;
          }
        }
        else if (digits < 18) {
          mantissa = mantissa * 10 + nibble;
          digits += 1;
          if (saw_point) {
            fraction_digits += 1;
          }
        }
        else if (!saw_point) {
          // Past the precision a 16.16 can carry: keep the magnitude by
          // counting the digit as an exponent rather than dropping it.
          exponent += 1;
        }
        continue;
      }
      switch (nibble) {
        case 0x0A: saw_point = true; break;
        case 0x0B: in_exponent = true; break;
        case 0x0C: in_exponent = true; exponent_sign = -1; break;
        case 0x0E: negative = true; break;
        case 0x0F: done = true; break;
        default:
          return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF,
              gfnt_reader_tell(reader) - 1, GFNT_GLYPH_NONE,
              "a real number using the nibble the format reserves");
      }
    }
  }

  scale = exponent_sign * exponent - fraction_digits;
  value = mantissa;
  if (scale >= 0) {
    int step;

    for (step = 0; step < scale && step < 19; step++) {
      value = gfnt_clamp64(value * 10);
    }
    value = gfnt_clamp64(value << 16);
  }
  else {
    int64_t divisor = 1;
    int step;

    for (step = 0; step < -scale && step < 19; step++) {
      divisor *= 10;
    }
    value = gfnt_round_div(gfnt_clamp64(value << 16), divisor);
  }
  *out_value = negative ? -value : value;
  return GFNT_OK;
}

/**
 * Walk a DICT, calling @p visit once per operator.
 *
 * Operands come first and the operator last, which is the opposite of every
 * other structure in an sfnt and is why this is a walk with a callback rather
 * than a struct-filling parse: the parser does not know what it is reading
 * until after it has read it.
 */
static GFNT_Result gfnt_cff_dict_walk(GFNT_Reader * dict,
    GFNT_CffDictVisit visit, void * user, GFNT_Error * error) {
  GFNT_CffDictOp entry;
  GFNT_Result result;

  entry.count = 0;
  while (gfnt_reader_remaining(dict) > 0) {
    uint8_t b0 = 0;
    int64_t value = 0;

    result = gfnt_read_u8(dict, &b0);
    if (result != GFNT_OK) {
      return result;
    }
    if (b0 == 30) {
      result = gfnt_cff_real(dict, &value, error);
      if (result != GFNT_OK) {
        return result;
      }
    }
    else if (b0 == 28) {
      int16_t narrow = 0;

      result = gfnt_read_s16(dict, &narrow);
      if (result != GFNT_OK) {
        return result;
      }
      value = (int64_t)narrow << 16;
    }
    else if (b0 == 29) {
      int32_t wide = 0;

      result = gfnt_read_s32(dict, &wide);
      if (result != GFNT_OK) {
        return result;
      }
      value = (int64_t)wide << 16;
    }
    else if (b0 >= 32 && b0 <= 246) {
      value = ((int64_t)b0 - 139) << 16;
    }
    else if (b0 >= 247 && b0 <= 250) {
      uint8_t b1 = 0;

      result = gfnt_read_u8(dict, &b1);
      if (result != GFNT_OK) {
        return result;
      }
      value = (((int64_t)b0 - 247) * 256 + b1 + 108) << 16;
    }
    else if (b0 >= 251 && b0 <= 254) {
      uint8_t b1 = 0;

      result = gfnt_read_u8(dict, &b1);
      if (result != GFNT_OK) {
        return result;
      }
      value = (-((int64_t)b0 - 251) * 256 - b1 - 108) << 16;
    }
    else if (b0 == 31 || b0 == 255) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF,
          gfnt_reader_tell(dict) - 1, GFNT_GLYPH_NONE,
          "a DICT byte the format reserves");
    }
    else {
      // An operator. Everything pushed since the last one belongs to it.
      entry.op = b0;
      if (b0 == GFNT_CFF_DICT_ESCAPE) {
        uint8_t b1 = 0;

        result = gfnt_read_u8(dict, &b1);
        if (result != GFNT_OK) {
          return result;
        }
        entry.op = (uint16_t)0x0c00 | b1;
      }
      result = visit(user, &entry, error);
      if (result != GFNT_OK) {
        return result;
      }
      entry.count = 0;
      continue;
    }
    if (entry.count >= GFNT_CFF_DICT_MAX_OPERANDS) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF,
          gfnt_reader_tell(dict), GFNT_GLYPH_NONE,
          "a DICT operator with more operands than the format allows");
    }
    entry.values[entry.count++] = value;
  }
  if (entry.count > 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF,
        gfnt_reader_tell(dict), GFNT_GLYPH_NONE,
        "a DICT ending in operands with no operator to take them");
  }
  return GFNT_OK;
}

/** What the Top DICT walk is filling in. */
typedef struct GFNT_CffTopState {
  GFNT_Cff * cff;        ///< The font being described.
  size_t private_size;   ///< The Private DICT's length, from operator 18.
  size_t private_offset; ///< Where it is.
  bool private_stated;   ///< Whether operator 18 appeared at all.
} GFNT_CffTopState;

/** An operand as an offset, refusing a negative or absurd one. */
static GFNT_Result gfnt_cff_offset_operand(const GFNT_CffDictOp * entry,
    size_t which, size_t limit, size_t * out_offset, GFNT_Error * error) {
  int64_t value;

  if (which >= entry->count) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0,
        GFNT_GLYPH_NONE, "a DICT operator with fewer operands than it takes");
  }
  value = entry->values[which] >> 16;
  if (value < 0 || (uint64_t)value > (uint64_t)limit) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0,
        GFNT_GLYPH_NONE, "a DICT offset outside the table it points into");
  }
  *out_offset = (size_t)value;
  return GFNT_OK;
}

static GFNT_Result gfnt_cff_top_visit(void * user,
    const GFNT_CffDictOp * entry, GFNT_Error * error) {
  GFNT_CffTopState * state = (GFNT_CffTopState *)user;
  GFNT_Cff * cff = state->cff;
  size_t offset = 0;
  GFNT_Result result;

  switch (entry->op) {
    case GFNT_CFF_OP_CHARSET:
      result = gfnt_cff_offset_operand(entry, 0, cff->length, &offset, error);
      if (result != GFNT_OK) {
        return result;
      }
      // 0, 1 and 2 are not offsets: they name the three predefined charsets,
      // and a font using one carries no charset bytes at all.
      if (offset <= 2) {
        cff->charset_predefined = true;
        cff->charset_id = (uint32_t)offset;
      }
      else {
        cff->charset = offset;
      }
      return GFNT_OK;
    case GFNT_CFF_OP_ENCODING:
      result = gfnt_cff_offset_operand(entry, 0, cff->length, &offset, error);
      if (result != GFNT_OK) {
        return result;
      }
      if (offset <= 1) {
        cff->encoding_predefined = true;
        cff->encoding_id = (uint32_t)offset;
      }
      else {
        cff->encoding = offset;
      }
      return GFNT_OK;
    case GFNT_CFF_OP_CHARSTRINGS:
      return gfnt_cff_offset_operand(entry, 0, cff->length,
          &state->cff->charstrings.offsets, error);
    case GFNT_CFF_OP_PRIVATE:
      if (entry->count < 2) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0,
            GFNT_GLYPH_NONE,
            "a Private DICT entry without both its size and its offset");
      }
      result = gfnt_cff_offset_operand(entry, 0, cff->length,
          &state->private_size, error);
      if (result != GFNT_OK) {
        return result;
      }
      result = gfnt_cff_offset_operand(entry, 1, cff->length,
          &state->private_offset, error);
      if (result != GFNT_OK) {
        return result;
      }
      state->private_stated = true;
      return GFNT_OK;
    case GFNT_CFF_OP_CHARSTRING_TYPE: {
      int64_t value;

      if (entry->count < 1) {
        return GFNT_OK;
      }
      value = entry->values[0] >> 16;
      if (value != 1 && value != 2) {
        return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF, 0,
            GFNT_GLYPH_NONE,
            "a CharstringType that is neither 1 nor 2, so this font's glyph "
            "programs are in a language nobody has specified");
      }
      cff->charstring_type = (uint32_t)value;
      return GFNT_OK;
    }
    case GFNT_CFF_OP_FONT_MATRIX: {
      size_t index;

      if (entry->count < 6) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0,
            GFNT_GLYPH_NONE, "a FontMatrix with fewer than six numbers");
      }
      for (index = 0; index < 6; index++) {
        cff->font_matrix[index] = gfnt_saturate32(entry->values[index]);
      }
      cff->font_matrix_stated = true;
      return GFNT_OK;
    }
    case GFNT_CFF_OP_ROS:
      // The presence of a Registry-Ordering-Supplement is what makes a font
      // CID-keyed, which changes where its Private DICTs are and what its
      // charset means. It is not a string this library reads.
      cff->is_cid = true;
      return GFNT_OK;
    case GFNT_CFF_OP_FDARRAY:
      return gfnt_cff_offset_operand(entry, 0, cff->length,
          &cff->fdarray.offsets, error);
    case GFNT_CFF_OP_FDSELECT:
      return gfnt_cff_offset_operand(entry, 0, cff->length, &cff->fdselect,
          error);
    default:
      // Every other operator is metadata: names, the bounding box, the hinting
      // parameters, the CID counts. Recording them would be recording constants
      // nothing reads, which section 2 of design.md names as its own mistake.
      return GFNT_OK;
  }
}

/** What the Private DICT walk is filling in. */
typedef struct GFNT_CffPrivateState {
  GFNT_CffPrivate * priv; ///< The dictionary being filled in.
  size_t dict_at;         ///< Where the Private DICT starts, for `Subrs`.
  size_t limit;           ///< The table's length.
  size_t subrs_offset;    ///< `Subrs`, relative to `dict_at`. 0 when absent.
} GFNT_CffPrivateState;

static GFNT_Result gfnt_cff_private_visit(void * user,
    const GFNT_CffDictOp * entry, GFNT_Error * error) {
  GFNT_CffPrivateState * state = (GFNT_CffPrivateState *)user;

  switch (entry->op) {
    case GFNT_CFF_OP_SUBRS:
      // Relative to the Private DICT, uniquely in this format. See the file
      // comment: every other offset in a CFF is from the table's start.
      return gfnt_cff_offset_operand(entry, 0, state->limit,
          &state->subrs_offset, error);
    case GFNT_CFF_OP_DEFAULT_WIDTH_X:
      if (entry->count >= 1) {
        state->priv->default_width = gfnt_saturate32(entry->values[0]);
      }
      return GFNT_OK;
    case GFNT_CFF_OP_NOMINAL_WIDTH_X:
      if (entry->count >= 1) {
        state->priv->nominal_width = gfnt_saturate32(entry->values[0]);
      }
      return GFNT_OK;
    default:
      // The blue values, the stem widths, the language group: hinting
      // parameters, and there is no hinter (design.md section 8.5).
      return GFNT_OK;
  }
}

/** Parse one Private DICT, given where the Top DICT said it is. */
static GFNT_Result gfnt_cff_private_parse(const GFNT_Reader * table,
    size_t offset, size_t size, GFNT_CffPrivate * out_private,
    GFNT_Error * error) {
  GFNT_CffPrivateState state;
  GFNT_Reader dict;
  GFNT_Result result;

  memset(out_private, 0, sizeof *out_private);
  state.priv = out_private;
  state.dict_at = offset;
  state.limit = table->length;
  state.subrs_offset = 0;

  result = gfnt_reader_sub(table, offset, size, &dict);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_cff_dict_walk(&dict, gfnt_cff_private_visit, &state, error);
  if (result != GFNT_OK) {
    return result;
  }
  out_private->present = true;
  if (state.subrs_offset > 0) {
    GFNT_Reader subrs;

    result = gfnt_reader_sub(table, offset + state.subrs_offset,
        GFNT_READER_REST, &subrs);
    if (result != GFNT_OK) {
      return result;
    }
    result = gfnt_cff_index_parse(&subrs, &out_private->subrs, error);
    if (result != GFNT_OK) {
      return result;
    }
    // The sub-reader started at the Subrs INDEX, so its offsets are relative to
    // that; the rest of this file works in table coordinates.
    out_private->subrs.offsets += offset + state.subrs_offset;
    out_private->subrs.data_base += offset + state.subrs_offset;
    out_private->subrs.end += offset + state.subrs_offset;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_cff_parse(const GFNT_Face * face, void * out,
    GFNT_Error * error) {
  GFNT_Cff * cff = (GFNT_Cff *)out;
  GFNT_CffTopState state;
  GFNT_Reader table;
  GFNT_Reader top;
  GFNT_Reader charstrings;
  GFNT_Result result;

  memset(cff, 0, sizeof *cff);
  cff->charstring_type = 2;
  result = gfnt_face_table_reader(face, GFNT_TAG_CFF, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  cff->length = table.length;

  result = gfnt_read_u8(&table, &cff->major);
  if (result == GFNT_OK) {
    result = gfnt_read_u8(&table, &cff->minor);
  }
  if (result == GFNT_OK) {
    result = gfnt_read_u8(&table, &cff->header_size);
  }
  if (result == GFNT_OK) {
    result = gfnt_read_u8(&table, &cff->offset_size);
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (cff->major != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF, 0,
        GFNT_GLYPH_NONE,
        "a CFF whose major version is not 1; CFF2 is a different format in a "
        "different table and is not read at all (design.md section 16)");
  }
  if (cff->header_size < 4) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 2,
        GFNT_GLYPH_NONE,
        "a CFF header shorter than the four bytes it always has");
  }
  // The header's stated size is what the Name INDEX follows, not the four bytes
  // just read: the format allows a longer header and says to skip by this.
  result = gfnt_reader_seek(&table, cff->header_size);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_cff_index_parse(&table, &cff->names, error);
  if (result == GFNT_OK) {
    result = gfnt_cff_index_parse(&table, &cff->top_dicts, error);
  }
  if (result == GFNT_OK) {
    result = gfnt_cff_index_parse(&table, &cff->strings, error);
  }
  if (result == GFNT_OK) {
    result = gfnt_cff_index_parse(&table, &cff->gsubrs, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (cff->top_dicts.count < 1) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0,
        GFNT_GLYPH_NONE, "a CFF with no Top DICT, so it describes no font");
  }

  // One font per table is what an sfnt-wrapped CFF holds. A FontSet with more
  // is legal CFF and is refused by name rather than silently read as its first
  // font, because which font a caller meant is a question this API cannot ask.
  if (cff->top_dicts.count > 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF, 0,
        GFNT_GLYPH_NONE,
        "a CFF FontSet holding more than one font, which an sfnt-wrapped CFF "
        "never is and which this library has no way to let a caller choose "
        "between");
  }

  memset(&state, 0, sizeof state);
  state.cff = cff;
  result = gfnt_cff_index_at(&table, &cff->top_dicts, 0, &top, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_cff_dict_walk(&top, gfnt_cff_top_visit, &state, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (cff->charstrings.offsets == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0,
        GFNT_GLYPH_NONE,
        "a CFF Top DICT with no CharStrings offset, so the font has no glyphs "
        "to find");
  }
  // The Top DICT walk recorded the offset in `charstrings.offsets`; parse the
  // INDEX there and let it overwrite that field with the real one.
  result = gfnt_reader_sub(&table, cff->charstrings.offsets, GFNT_READER_REST,
      &charstrings);
  if (result != GFNT_OK) {
    return result;
  }
  {
    size_t base = cff->charstrings.offsets;

    result = gfnt_cff_index_parse(&charstrings, &cff->charstrings, error);
    if (result != GFNT_OK) {
      return result;
    }
    cff->charstrings.offsets += base;
    cff->charstrings.data_base += base;
    cff->charstrings.end += base;
  }
  if (cff->charstrings.count > face->limits.max_glyphs) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_CFF, 0,
        GFNT_GLYPH_NONE,
        "more charstrings than GFNT_Limits::max_glyphs");
  }

  if (cff->fdarray.offsets != 0) {
    GFNT_Reader fdarray;
    size_t base = cff->fdarray.offsets;

    result = gfnt_reader_sub(&table, base, GFNT_READER_REST, &fdarray);
    if (result == GFNT_OK) {
      result = gfnt_cff_index_parse(&fdarray, &cff->fdarray, error);
    }
    if (result != GFNT_OK) {
      return result;
    }
    cff->fdarray.offsets += base;
    cff->fdarray.data_base += base;
    cff->fdarray.end += base;
  }
  if (cff->is_cid && cff->fdarray.count == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0,
        GFNT_GLYPH_NONE,
        "a CID-keyed CFF with no FDArray, so no glyph has a Private DICT");
  }
  if (state.private_stated) {
    result = gfnt_cff_private_parse(&table, state.private_offset,
        state.private_size, &cff->priv, error);
    if (result != GFNT_OK) {
      return result;
    }
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_cff(const GFNT_Face * face, const GFNT_Cff ** out_cff,
    GFNT_Error * error) {
  GFNT_Face * owner = (GFNT_Face *)face;
  GFNT_Cff scratch;
  GFNT_Result result;

  if (!face || !out_cff) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_table_cached(face, &owner->cff_state, &owner->cff, &scratch,
      sizeof scratch, gfnt_cff_parse, error);
  if (result == GFNT_OK) {
    *out_cff = &owner->cff;
  }
  return result;
}

bool gfnt_cff_glyph_bound(const GFNT_Face * face, size_t * out_bound) {
  const GFNT_Cff * cff = NULL;

  if (!gfnt_sfnt_find(face, GFNT_TAG_CFF)) {
    return false;
  }
  if (gfnt_face_cff(face, &cff, NULL) != GFNT_OK) {
    return false;
  }
  *out_bound = cff->charstrings.count;
  return true;
}

/**
 * Walk the charset, answering one of the two questions it can be asked.
 *
 * One walk for both because they are one traversal read in two directions - a
 * glyph's SID, and the glyph a SID belongs to - and two copies of the format's
 * three arms would be two places for the range arithmetic to be wrong.
 *
 * @param by_glyph true to find @p key's SID, false to find the glyph whose SID
 *   is @p key.
 */
static GFNT_Result gfnt_cff_charset_lookup(const GFNT_Face * face,
    const GFNT_Cff * cff, bool by_glyph, uint32_t key, uint32_t * out_value,
    GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_Reader charset;
  uint8_t format = 0;
  uint32_t glyph = 1;
  GFNT_Result result;

  // Glyph 0 is `.notdef` and SID 0 is its name, in every charset and in none of
  // their bytes: the format simply does not store the first entry.
  if (by_glyph && key == 0) {
    *out_value = 0;
    return GFNT_OK;
  }
  if (!by_glyph && key == 0) {
    *out_value = 0;
    return GFNT_OK;
  }
  if (key >= cff->charstrings.count && by_glyph) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_CFF, 0, key,
        "a glyph index past the CharStrings INDEX");
  }

  if (cff->charset_predefined || cff->charset == 0) {
    uint32_t id = cff->charset_predefined ? cff->charset_id : 0;
    const uint16_t * table_of_sids = NULL;
    size_t count = 0;

    // A font with no charset at all is ISOAdobe by default, which is why the
    // absent case and the explicit 0 case are one branch.
    if (id == 1) {
      table_of_sids = gfnt_cff_expert_charset;
      count = GFNT_CFF_EXPERT_CHARSET_COUNT;
    }
    else if (id == 2) {
      table_of_sids = gfnt_cff_expert_subset_charset;
      count = GFNT_CFF_EXPERT_SUBSET_CHARSET_COUNT;
    }
    if (!table_of_sids) {
      // ISOAdobe: the first 229 SIDs in order, so a glyph's name is its index.
      if (by_glyph) {
        if (key >= GFNT_CFF_ISO_ADOBE_COUNT) {
          return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_CFF, 0, key,
              "a glyph past the ISOAdobe charset, which names only its first "
              "229 and is what a font with no charset of its own uses");
        }
        *out_value = key;
        return GFNT_OK;
      }
      if (key >= GFNT_CFF_ISO_ADOBE_COUNT
          || key >= cff->charstrings.count) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_CFF, 0,
            GFNT_GLYPH_NONE, "no glyph of this font has that name");
      }
      *out_value = key;
      return GFNT_OK;
    }
    if (by_glyph) {
      if (key >= count) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_CFF, 0, key,
            "a glyph past the predefined charset this font uses");
      }
      *out_value = table_of_sids[key];
      return GFNT_OK;
    }
    for (glyph = 0; glyph < count; glyph++) {
      if (table_of_sids[glyph] == key) {
        *out_value = glyph;
        return GFNT_OK;
      }
    }
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_CFF, 0,
        GFNT_GLYPH_NONE, "no glyph of this font has that name");
  }

  result = gfnt_face_table_reader(face, GFNT_TAG_CFF, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_reader_sub(&table, cff->charset, GFNT_READER_REST, &charset);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_read_u8(&charset, &format);
  if (result != GFNT_OK) {
    return result;
  }
  switch (format) {
    case 0:
      while (glyph < cff->charstrings.count) {
        uint16_t sid = 0;

        result = gfnt_read_u16(&charset, &sid);
        if (result != GFNT_OK) {
          return result;
        }
        if (by_glyph ? (glyph == key) : (sid == key)) {
          *out_value = by_glyph ? sid : glyph;
          return GFNT_OK;
        }
        glyph += 1;
      }
      break;
    case 1:
    case 2:
      while (glyph < cff->charstrings.count) {
        uint16_t first = 0;
        uint32_t left = 0;
        uint32_t step;

        result = gfnt_read_u16(&charset, &first);
        if (result != GFNT_OK) {
          return result;
        }
        if (format == 1) {
          uint8_t narrow = 0;

          result = gfnt_read_u8(&charset, &narrow);
          left = narrow;
        }
        else {
          uint16_t wide = 0;

          result = gfnt_read_u16(&charset, &wide);
          left = wide;
        }
        if (result != GFNT_OK) {
          return result;
        }
        for (step = 0; step <= left && glyph < cff->charstrings.count;
            step++, glyph++) {
          uint32_t sid = (uint32_t)first + step;

          if (by_glyph ? (glyph == key) : (sid == key)) {
            *out_value = by_glyph ? sid : glyph;
            return GFNT_OK;
          }
        }
      }
      break;
    default:
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF,
          cff->charset, GFNT_GLYPH_NONE,
          "a charset format the Compact Font Format does not define");
  }
  if (by_glyph) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_CFF, cff->charset,
        key, "a glyph the charset stops short of naming");
  }
  return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_CFF, cff->charset,
      GFNT_GLYPH_NONE, "no glyph of this font has that name");
}

GFNT_Result gfnt_cff_glyph_sid(const GFNT_Face * face, const GFNT_Cff * cff,
    uint32_t glyph, uint32_t * out_sid, GFNT_Error * error) {
  if (!face || !cff || !out_sid) {
    return GFNT_ERR_INVALID;
  }
  return gfnt_cff_charset_lookup(face, cff, true, glyph, out_sid, error);
}

GFNT_Result gfnt_cff_glyph_for_sid(const GFNT_Face * face, const GFNT_Cff * cff,
    uint32_t sid, uint32_t * out_glyph, GFNT_Error * error) {
  if (!face || !cff || !out_glyph) {
    return GFNT_ERR_INVALID;
  }
  return gfnt_cff_charset_lookup(face, cff, false, sid, out_glyph, error);
}

GFNT_Result gfnt_cff_string(const GFNT_Face * face, const GFNT_Cff * cff,
    uint32_t sid, char * buffer, size_t size, size_t * out_length,
    GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_Reader element;
  const uint8_t * bytes = NULL;
  size_t length;
  GFNT_Result result;

  if (!face || !cff || !buffer || size == 0) {
    return GFNT_ERR_INVALID;
  }
  if (sid < GFNT_CFF_STANDARD_STRING_COUNT) {
    const char * name = gfnt_cff_standard_strings[sid];

    length = strlen(name);
    if (length + 1 > size) {
      return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_CFF, 0,
          GFNT_GLYPH_NONE, "a glyph name longer than the caller's buffer");
    }
    memcpy(buffer, name, length + 1);
    if (out_length) {
      *out_length = length;
    }
    return GFNT_OK;
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_CFF, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_cff_index_at(&table, &cff->strings,
      sid - GFNT_CFF_STANDARD_STRING_COUNT, &element, error);
  if (result != GFNT_OK) {
    return result;
  }
  length = gfnt_reader_remaining(&element);
  if (length + 1 > size) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_CFF, 0,
        GFNT_GLYPH_NONE, "a glyph name longer than the caller's buffer");
  }
  result = gfnt_read_bytes(&element, length, &bytes);
  if (result != GFNT_OK) {
    return result;
  }
  // A CFF string is not NUL-terminated and may hold any byte; it is copied and
  // terminated rather than pointed at, so that a caller can treat it as a C
  // string without the font deciding where it ends.
  if (length > 0) {
    memcpy(buffer, bytes, length);
  }
  buffer[length] = '\0';
  if (out_length) {
    *out_length = length;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_cff_glyph_for_code(const GFNT_Face * face,
    const GFNT_Cff * cff, uint8_t code, uint32_t * out_glyph,
    GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_Reader encoding;
  uint8_t format = 0;
  uint32_t glyph = 1;
  GFNT_Result result;

  if (!face || !cff || !out_glyph) {
    return GFNT_ERR_INVALID;
  }
  if (cff->encoding_predefined || cff->encoding == 0) {
    uint32_t id = cff->encoding_predefined ? cff->encoding_id : 0;

    if (id == 1) {
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF, 0,
          GFNT_GLYPH_NONE,
          "the predefined Expert encoding, whose table this library does not "
          "carry; its charset is read, so the font's glyphs are reachable by "
          "name");
    }
    // A font with no encoding is Standard-encoded by default, so the absent
    // case and the explicit 0 case are one branch.
    if (gfnt_cff_standard_encoding[code] == 0) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_CFF, 0,
          GFNT_GLYPH_NONE, "the Standard Encoding leaves this code unassigned");
    }
    return gfnt_cff_glyph_for_sid(face, cff, gfnt_cff_standard_encoding[code],
        out_glyph, error);
  }

  result = gfnt_face_table_reader(face, GFNT_TAG_CFF, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_reader_sub(&table, cff->encoding, GFNT_READER_REST, &encoding);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_read_u8(&encoding, &format);
  if (result != GFNT_OK) {
    return result;
  }
  if ((format & 0x7F) == 0) {
    uint8_t count = 0;
    uint8_t index;

    result = gfnt_read_u8(&encoding, &count);
    if (result != GFNT_OK) {
      return result;
    }
    for (index = 0; index < count; index++) {
      uint8_t stored = 0;

      result = gfnt_read_u8(&encoding, &stored);
      if (result != GFNT_OK) {
        return result;
      }
      if (stored == code) {
        *out_glyph = (uint32_t)index + 1;
        return GFNT_OK;
      }
    }
  }
  else if ((format & 0x7F) == 1) {
    uint8_t ranges = 0;
    uint8_t index;

    result = gfnt_read_u8(&encoding, &ranges);
    if (result != GFNT_OK) {
      return result;
    }
    for (index = 0; index < ranges; index++) {
      uint8_t first = 0;
      uint8_t left = 0;
      uint32_t step;

      result = gfnt_read_u8(&encoding, &first);
      if (result == GFNT_OK) {
        result = gfnt_read_u8(&encoding, &left);
      }
      if (result != GFNT_OK) {
        return result;
      }
      for (step = 0; step <= left; step++, glyph++) {
        if ((uint32_t)first + step == code) {
          *out_glyph = glyph;
          return GFNT_OK;
        }
      }
    }
  }
  else {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF,
        cff->encoding, GFNT_GLYPH_NONE,
        "an encoding format the Compact Font Format does not define");
  }

  // The high bit says a supplement list follows, which maps further codes to
  // glyphs **by name** rather than by position - so it has to be read after the
  // base format's own array has been walked past.
  if ((format & 0x80) != 0) {
    uint8_t supplements = 0;
    uint8_t index;

    result = gfnt_read_u8(&encoding, &supplements);
    if (result != GFNT_OK) {
      return result;
    }
    for (index = 0; index < supplements; index++) {
      uint8_t stored = 0;
      uint16_t sid = 0;

      result = gfnt_read_u8(&encoding, &stored);
      if (result == GFNT_OK) {
        result = gfnt_read_u16(&encoding, &sid);
      }
      if (result != GFNT_OK) {
        return result;
      }
      if (stored == code) {
        return gfnt_cff_glyph_for_sid(face, cff, sid, out_glyph, error);
      }
    }
  }
  return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_CFF, cff->encoding,
      GFNT_GLYPH_NONE, "this font's encoding leaves this code unassigned");
}

/** Which FD a glyph belongs to, per `FDSelect`. */
static GFNT_Result gfnt_cff_fd_for_glyph(const GFNT_Face * face,
    const GFNT_Cff * cff, uint32_t glyph, uint32_t * out_fd,
    GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_Reader select;
  uint8_t format = 0;
  GFNT_Result result;

  if (cff->fdselect == 0) {
    // Legal when there is exactly one Font DICT: with nothing to choose
    // between, the format lets the selector be left out.
    *out_fd = 0;
    return GFNT_OK;
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_CFF, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_reader_sub(&table, cff->fdselect, GFNT_READER_REST, &select);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_read_u8(&select, &format);
  if (result != GFNT_OK) {
    return result;
  }
  if (format == 0) {
    uint8_t fd = 0;

    result = gfnt_reader_u8_at(&select, 1 + glyph, &fd);
    if (result != GFNT_OK) {
      return result;
    }
    *out_fd = fd;
    return GFNT_OK;
  }
  if (format == 3) {
    uint16_t ranges = 0;
    uint16_t index;
    uint16_t first = 0;

    result = gfnt_read_u16(&select, &ranges);
    if (result == GFNT_OK) {
      result = gfnt_read_u16(&select, &first);
    }
    if (result != GFNT_OK) {
      return result;
    }
    for (index = 0; index < ranges; index++) {
      uint8_t fd = 0;
      uint16_t next = 0;

      result = gfnt_read_u8(&select, &fd);
      if (result == GFNT_OK) {
        // Each range gives its own first glyph and the *next* range's first
        // glyph is where it ends; the last one is followed by a sentinel.
        result = gfnt_read_u16(&select, &next);
      }
      if (result != GFNT_OK) {
        return result;
      }
      if (glyph >= first && glyph < next) {
        *out_fd = fd;
        return GFNT_OK;
      }
      first = next;
    }
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, cff->fdselect,
        glyph, "an FDSelect whose ranges do not cover this glyph");
  }
  return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF,
      cff->fdselect, GFNT_GLYPH_NONE,
      "an FDSelect format the Compact Font Format does not define");
}

/** The Private entry of one Font DICT out of the `FDArray`. */
typedef struct GFNT_CffFdState {
  size_t size;   ///< The Private DICT's length.
  size_t offset; ///< Where it is.
  bool stated;   ///< Whether the Font DICT had one.
  size_t limit;  ///< The table's length.
} GFNT_CffFdState;

static GFNT_Result gfnt_cff_fd_visit(void * user, const GFNT_CffDictOp * entry,
    GFNT_Error * error) {
  GFNT_CffFdState * state = (GFNT_CffFdState *)user;
  GFNT_Result result;

  if (entry->op != GFNT_CFF_OP_PRIVATE) {
    return GFNT_OK;
  }
  if (entry->count < 2) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0,
        GFNT_GLYPH_NONE,
        "a Font DICT whose Private entry lacks its size or its offset");
  }
  result = gfnt_cff_offset_operand(entry, 0, state->limit, &state->size, error);
  if (result == GFNT_OK) {
    result = gfnt_cff_offset_operand(entry, 1, state->limit, &state->offset,
        error);
  }
  state->stated = result == GFNT_OK;
  return result;
}

GFNT_Result gfnt_cff_private_for_glyph(const GFNT_Face * face,
    const GFNT_Cff * cff, uint32_t glyph, GFNT_CffPrivate * out_private,
    GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_Reader font_dict;
  GFNT_CffFdState state;
  uint32_t fd = 0;
  GFNT_Result result;

  if (!face || !cff || !out_private) {
    return GFNT_ERR_INVALID;
  }
  if (cff->fdarray.count == 0) {
    // The ordinary case: one Private DICT for the whole font, already parsed.
    *out_private = cff->priv;
    return GFNT_OK;
  }
  result = gfnt_cff_fd_for_glyph(face, cff, glyph, &fd, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (fd >= cff->fdarray.count) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, cff->fdselect,
        glyph, "an FDSelect naming a Font DICT the FDArray does not have");
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_CFF, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_cff_index_at(&table, &cff->fdarray, fd, &font_dict, error);
  if (result != GFNT_OK) {
    return result;
  }
  memset(&state, 0, sizeof state);
  state.limit = table.length;
  result = gfnt_cff_dict_walk(&font_dict, gfnt_cff_fd_visit, &state, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (!state.stated) {
    // A Font DICT with no Private DICT: no local subroutines and no width
    // defaults, which is a font that draws but states nothing.
    memset(out_private, 0, sizeof *out_private);
    return GFNT_OK;
  }
  return gfnt_cff_private_parse(&table, state.offset, state.size, out_private,
      error);
}

GFNT_Result gfnt_cff_name_at(const GFNT_Face * face, uint32_t glyph,
    char * out, size_t capacity, size_t * out_length, GFNT_Error * error) {
  const GFNT_Cff * cff = NULL;
  char buffer[GFNT_GLYPH_NAME_MAX + 1u];
  uint32_t sid = 0;
  size_t length = 0;
  GFNT_Result result;

  if (!face || !out_length) {
    return GFNT_ERR_INVALID;
  }
  if (!gfnt_sfnt_find(face, GFNT_TAG_CFF)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, glyph,
        "this face has no CFF, so it has no charset to name glyphs by");
  }
  result = gfnt_face_cff(face, &cff, error);
  if (result != GFNT_OK) {
    return result;
  }
  // Glyph 0 is the exception, and not by convention: the format says glyph 0 is
  // `.notdef` in every CFF, so that name is a fact about the format rather than
  // a string this font's charset supplied, and every reference agrees about it.
  if (cff->is_cid && glyph != 0) {
    // A CID-keyed font's charset holds **CIDs**, not SIDs. They index nothing:
    // a CID is a number in a character collection the `ROS` names, and reading
    // one as a SID prints whichever standard string happens to sit at that
    // number - CID 11 came out as `asterisk`. fontTools invents `cid00011` for
    // such a glyph, which is a name for a thing that has none; this library says
    // it has none.
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF, 0, glyph,
        "a CID-keyed font names its glyphs by CID rather than by string, so "
        "this glyph has no name to read");
  }
  result = gfnt_cff_glyph_sid(face, cff, glyph, &sid, error);
  if (result != GFNT_OK) {
    return result;
  }
  // Read into a local buffer even for the counting pass, so that the two passes
  // cannot walk different predicates - the failure `post`'s two passes were
  // written as one function to avoid.
  result = gfnt_cff_string(face, cff, sid, buffer, sizeof buffer, &length,
      error);
  if (result != GFNT_OK) {
    return result;
  }
  if (out) {
    if (length + 1 > capacity) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_CFF, 0, glyph,
          "the buffer is too small for this glyph's name");
    }
    memcpy(out, buffer, length + 1);
  }
  *out_length = length;
  return GFNT_OK;
}
