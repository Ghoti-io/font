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
 * The checked reader: every read of a font's bytes, and nothing else.
 *
 * documentation/design.md section 6. Three rules hold throughout, and each is
 * a defect class this library is choosing not to have:
 *
 * - **Every read is bounds-checked before it happens**, against the extent the
 *   reader was given, which is one table - so a table cannot read into its
 *   neighbour even when its own directory entry says it may. Every offset sum
 *   goes through cutil's safemath, because a length near SIZE_MAX that wraps
 *   would pass a naive comparison.
 * - **Every multi-byte value is assembled with explicit shifts.** No cast and
 *   dereference (which is undefined on an unaligned address and host-endian
 *   even when it works), no `memcpy` into a scalar, no `ntohs`, no byte-order
 *   conditional. The big-endian, 32-bit, strict-alignment container runs this
 *   file unchanged, and that is the only way to know it is right.
 * - **Every signed value is converted arithmetically**, not by casting a
 *   larger unsigned type down. Converting 0xFFFF to an int16_t by cast is
 *   implementation-defined before C23; subtracting 65536 is defined and gives
 *   the two's-complement answer the specification means, on every platform.
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/macros.h>
#include "reader.h"

/**
 * Record a diagnostic at @p offset and return the failure.
 */
static GFNT_Result gfnt_reader_fail(const GFNT_Reader * reader, size_t offset,
    const char * message) {
  return gfnt_error_set(reader->error, GFNT_ERR_CORRUPT, reader->table, offset,
      GFNT_GLYPH_NONE, message);
}

/**
 * Whether [offset, offset + count) lies inside the extent.
 *
 * The sum is computed with safemath rather than compared as `offset + count >
 * length`, which is the bug that lets a count near SIZE_MAX through.
 */
static bool gfnt_reader_fits(const GFNT_Reader * reader, size_t offset,
    size_t count) {
  size_t end;

  if (!gcu_safe_add_size(offset, count, &end)) {
    return false;
  }
  return end <= reader->length;
}

GFNT_Result gfnt_reader_init(GFNT_Reader * reader, const uint8_t * base,
    size_t length, GFNT_Tag table, GFNT_Error * error) {
  if (!reader || (!base && length > 0)) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, table, 0, GFNT_GLYPH_NONE,
        "no reader, or no bytes for a non-empty extent");
  }

  *reader = (GFNT_Reader) {
    .base = length > 0 ? base : NULL,
    .length = length,
    .cursor = 0,
    .table = table,
    .error = error,
  };
  return GFNT_OK;
}

GFNT_Result gfnt_reader_init_blob(GFNT_Reader * reader, const GFNT_Blob * blob,
    GFNT_Tag table, GFNT_Error * error) {
  if (!blob) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, table, 0, GFNT_GLYPH_NONE,
        "no blob");
  }
  return gfnt_reader_init(reader, gfnt_blob_data(blob), gfnt_blob_size(blob),
      table, error);
}

GFNT_Result gfnt_reader_sub(const GFNT_Reader * parent, size_t offset,
    size_t length, GFNT_Reader * out_child) {
  size_t available;

  if (!parent || !out_child) {
    return GFNT_ERR_INVALID;
  }
  if (offset > parent->length) {
    return gfnt_reader_fail(parent, offset,
        "a subtable offset points past the end of its table");
  }
  available = parent->length - offset;
  if (length == GFNT_READER_REST) {
    length = available;
  }
  else if (length > available) {
    return gfnt_reader_fail(parent, offset,
        "a subtable's length runs past the end of its table");
  }

  *out_child = (GFNT_Reader) {
    .base = length > 0 ? parent->base + offset : NULL,
    .length = length,
    .cursor = 0,
    .table = parent->table,
    .error = parent->error,
  };
  return GFNT_OK;
}

GFNT_Result gfnt_reader_seek(GFNT_Reader * reader, size_t offset) {
  if (!reader) {
    return GFNT_ERR_INVALID;
  }
  if (offset > reader->length) {
    return gfnt_reader_fail(reader, offset, "seek past the end of the table");
  }
  reader->cursor = offset;
  return GFNT_OK;
}

GFNT_Result gfnt_reader_skip(GFNT_Reader * reader, size_t count) {
  size_t target;

  if (!reader) {
    return GFNT_ERR_INVALID;
  }
  if (!gcu_safe_add_size(reader->cursor, count, &target)
      || target > reader->length) {
    return gfnt_reader_fail(reader, reader->cursor,
        "skip past the end of the table");
  }
  reader->cursor = target;
  return GFNT_OK;
}

size_t gfnt_reader_tell(const GFNT_Reader * reader) {
  return reader ? reader->cursor : 0;
}

size_t gfnt_reader_remaining(const GFNT_Reader * reader) {
  if (!reader) {
    return 0;
  }
  return reader->length - reader->cursor;
}

bool gfnt_reader_has(const GFNT_Reader * reader, size_t count) {
  if (!reader) {
    return false;
  }
  return gfnt_reader_fits(reader, reader->cursor, count);
}

/**
 * Assemble @p count big-endian bytes at @p offset into a 64-bit value.
 *
 * The single place a multi-byte quantity is built. Everything else in this
 * file is a range check and a signed conversion.
 */
static GFNT_Result gfnt_reader_raw_at(const GFNT_Reader * reader,
    size_t offset, size_t count, uint64_t * out_value) {
  uint64_t value = 0;

  if (!gfnt_reader_fits(reader, offset, count)) {
    return gfnt_reader_fail(reader, offset, "read past the end of the table");
  }
  for (size_t i = 0; i < count; ++i) {
    value = (value << 8) | (uint64_t)reader->base[offset + i];
  }
  *out_value = value;
  return GFNT_OK;
}

/**
 * Read @p count big-endian bytes at the cursor and advance past them.
 */
static GFNT_Result gfnt_reader_raw(GFNT_Reader * reader, size_t count,
    uint64_t * out_value) {
  GFNT_Result result = gfnt_reader_raw_at(reader, reader->cursor, count,
      out_value);

  if (result == GFNT_OK) {
    reader->cursor += count;
  }
  return result;
}

/**
 * Reinterpret the low @p bits of a value as two's complement, arithmetically.
 */
static int64_t gfnt_reader_signed(uint64_t value, unsigned bits) {
  uint64_t sign_bit = (uint64_t)1 << (bits - 1);
  uint64_t magnitude;

  if (value < sign_bit) {
    return (int64_t)value;
  }

  // The answer is value - 2^bits. Computing it as a magnitude first keeps
  // every intermediate inside int64_t's range, including the one case a
  // straightforward negation cannot express: -2^63, whose magnitude is not a
  // representable int64_t at all. Hence the -1 split.
  magnitude = (bits == 64) ? (~value + 1u) : ((sign_bit << 1) - value);
  return -(int64_t)(magnitude - 1) - 1;
}

GFNT_Result gfnt_read_u8(GFNT_Reader * reader, uint8_t * out_value) {
  uint64_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_reader_raw(reader, 1, &value);
  if (result == GFNT_OK) {
    *out_value = (uint8_t)value;
  }
  return result;
}

GFNT_Result gfnt_read_u16(GFNT_Reader * reader, uint16_t * out_value) {
  uint64_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_reader_raw(reader, 2, &value);
  if (result == GFNT_OK) {
    *out_value = (uint16_t)value;
  }
  return result;
}

GFNT_Result gfnt_read_u24(GFNT_Reader * reader, uint32_t * out_value) {
  uint64_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_reader_raw(reader, 3, &value);
  if (result == GFNT_OK) {
    *out_value = (uint32_t)value;
  }
  return result;
}

GFNT_Result gfnt_read_u32(GFNT_Reader * reader, uint32_t * out_value) {
  uint64_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_reader_raw(reader, 4, &value);
  if (result == GFNT_OK) {
    *out_value = (uint32_t)value;
  }
  return result;
}

GFNT_Result gfnt_read_s8(GFNT_Reader * reader, int8_t * out_value) {
  uint64_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_reader_raw(reader, 1, &value);
  if (result == GFNT_OK) {
    *out_value = (int8_t)gfnt_reader_signed(value, 8);
  }
  return result;
}

GFNT_Result gfnt_read_s16(GFNT_Reader * reader, int16_t * out_value) {
  uint64_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_reader_raw(reader, 2, &value);
  if (result == GFNT_OK) {
    *out_value = (int16_t)gfnt_reader_signed(value, 16);
  }
  return result;
}

GFNT_Result gfnt_read_s32(GFNT_Reader * reader, int32_t * out_value) {
  uint64_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_reader_raw(reader, 4, &value);
  if (result == GFNT_OK) {
    *out_value = (int32_t)gfnt_reader_signed(value, 32);
  }
  return result;
}

GFNT_Result gfnt_read_tag(GFNT_Reader * reader, GFNT_Tag * out_value) {
  uint32_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  // A tag is four bytes in file order, which is what a big-endian uint32 is:
  // GFNT_TAG() packs them the same way, so the two are comparable.
  result = gfnt_read_u32(reader, &value);
  if (result == GFNT_OK) {
    *out_value = (GFNT_Tag)value;
  }
  return result;
}

GFNT_Result gfnt_read_f2dot14(GFNT_Reader * reader, GFNT_F2Dot14 * out_value) {
  int16_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_read_s16(reader, &value);
  if (result == GFNT_OK) {
    *out_value = (GFNT_F2Dot14)value;
  }
  return result;
}

GFNT_Result gfnt_read_fixed(GFNT_Reader * reader, GFNT_F16Dot16 * out_value) {
  int32_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_read_s32(reader, &value);
  if (result == GFNT_OK) {
    *out_value = (GFNT_F16Dot16)value;
  }
  return result;
}

GFNT_Result gfnt_read_longdatetime(GFNT_Reader * reader, int64_t * out_value) {
  uint64_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_reader_raw(reader, 8, &value);
  if (result == GFNT_OK) {
    *out_value = gfnt_reader_signed(value, 64);
  }
  return result;
}

GFNT_Result gfnt_read_bytes(GFNT_Reader * reader, size_t count,
    const uint8_t ** out_bytes) {
  if (!reader || !out_bytes) {
    return GFNT_ERR_INVALID;
  }
  if (!gfnt_reader_fits(reader, reader->cursor, count)) {
    return gfnt_reader_fail(reader, reader->cursor,
        "a block of bytes runs past the end of the table");
  }
  *out_bytes = count > 0 ? reader->base + reader->cursor : NULL;
  reader->cursor += count;
  return GFNT_OK;
}

GFNT_Result gfnt_reader_u8_at(const GFNT_Reader * reader, size_t offset,
    uint8_t * out_value) {
  uint64_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_reader_raw_at(reader, offset, 1, &value);
  if (result == GFNT_OK) {
    *out_value = (uint8_t)value;
  }
  return result;
}

GFNT_Result gfnt_reader_u16_at(const GFNT_Reader * reader, size_t offset,
    uint16_t * out_value) {
  uint64_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_reader_raw_at(reader, offset, 2, &value);
  if (result == GFNT_OK) {
    *out_value = (uint16_t)value;
  }
  return result;
}

GFNT_Result gfnt_reader_u32_at(const GFNT_Reader * reader, size_t offset,
    uint32_t * out_value) {
  uint64_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_reader_raw_at(reader, offset, 4, &value);
  if (result == GFNT_OK) {
    *out_value = (uint32_t)value;
  }
  return result;
}

GFNT_Result gfnt_reader_s16_at(const GFNT_Reader * reader, size_t offset,
    int16_t * out_value) {
  uint64_t value;
  GFNT_Result result;

  if (!reader || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_reader_raw_at(reader, offset, 2, &value);
  if (result == GFNT_OK) {
    *out_value = (int16_t)gfnt_reader_signed(value, 16);
  }
  return result;
}
