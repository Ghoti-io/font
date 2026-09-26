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
 * @ref GFNT_Reader "GFNT_Reader": the one path by which this library reads a byte of a font.
 *
 * documentation/design.md section 6. Every read checks that it fits before it
 * happens and returns ::GFNT_ERR_CORRUPT with the offset if it does not, and
 * every multi-byte quantity is assembled with explicit shifts - no casts, no
 * `memcpy` into a scalar, no `ntohs`, no byte-order conditional - so that a
 * big-endian or strict-alignment platform runs the same code as this one.
 *
 * A reader spans **one table's extent**, not the file's. Table readers are
 * derived from the directory entry's offset and length, so a table cannot
 * reach into its neighbour by construction, and a sub-reader derived from a
 * parent cannot exceed the parent.
 *
 * This header is internal: it lives under `src/` and is not installed. The
 * reader is not part of the public API because a consumer parsing a font by
 * hand is not a use this library supports - it is the mechanism behind the
 * threat model, and `make check-reader` fails the build on any read of file
 * data that goes around it.
 *
 * Two fields go beyond design.md's `(base, length, cursor)`: the table tag and
 * the caller's diagnostic. Both are what make "returns ERR_CORRUPT with the
 * offset" possible at the point of failure rather than at the call site that
 * has forgotten which table it was reading.
 */

#ifndef GHOTI_IO_GFNT_READER_H
#define GHOTI_IO_GFNT_READER_H

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A bounds-checked cursor over one extent of a font file.
 *
 * Copying a reader is how a parser saves its position: the copy shares the
 * extent and carries its own cursor, which is what lets a table parser walk a
 * record list and re-read a field without seeking twice.
 */
typedef struct GFNT_Reader {
  const uint8_t * base; ///< First byte of the extent, or NULL when empty.
  size_t length;        ///< Bytes in the extent.
  size_t cursor;        ///< Read position, always <= length.
  GFNT_Tag table;       ///< The table this extent is, for diagnostics.
  GFNT_Error * error;   ///< Where a failure is recorded, or NULL.
} GFNT_Reader;

/**
 * Passed as a sub-reader's length to mean "from the offset to the end of the
 * parent", for the tables whose subtables carry no length of their own.
 */
#define GFNT_READER_REST ((size_t)-1)

/**
 * Set up a reader over an extent of memory.
 *
 * @param reader The reader to initialise.
 * @param base The first byte, or NULL only when @p length is zero.
 * @param length How many bytes.
 * @param table The tag to name in diagnostics, or 0 for none.
 * @param error Where reads record their diagnostics, or NULL.
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID.
 */
GFNT_Result gfnt_reader_init(GFNT_Reader * reader, const uint8_t * base,
    size_t length, GFNT_Tag table, GFNT_Error * error);

/**
 * Set up a reader over a whole blob.
 *
 * The only place the library turns a blob into bytes; `check-reader` fails the
 * build on a second one.
 *
 * @param reader The reader to initialise.
 * @param blob The blob.
 * @param table The tag to name in diagnostics, or 0 for none.
 * @param error Where reads record their diagnostics, or NULL.
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID.
 */
GFNT_Result gfnt_reader_init_blob(GFNT_Reader * reader, const GFNT_Blob * blob,
    GFNT_Tag table, GFNT_Error * error);

/**
 * Derive a reader over part of another.
 *
 * @param parent The reader to carve from. Its cursor is not consulted:
 *   subtable offsets are from the start of the table, which is what every
 *   sfnt-derived format means by an offset.
 * @param offset Where the child starts, from the parent's base.
 * @param length How long the child is, or ::GFNT_READER_REST for the
 *   remainder of the parent.
 * @param out_child Receives the child, which inherits the parent's table tag
 *   and diagnostic. Written only on success.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, or ::GFNT_ERR_CORRUPT if the child
 *   would leave the parent.
 */
GFNT_Result gfnt_reader_sub(const GFNT_Reader * parent, size_t offset,
    size_t length, GFNT_Reader * out_child);

/**
 * Move the cursor to an absolute offset within the extent.
 *
 * @param reader The reader.
 * @param offset The offset. The end of the extent is a legal position; past
 *   it is ::GFNT_ERR_CORRUPT.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, or ::GFNT_ERR_CORRUPT.
 */
GFNT_Result gfnt_reader_seek(GFNT_Reader * reader, size_t offset);

/**
 * Advance the cursor.
 *
 * @param reader The reader.
 * @param count How many bytes to skip.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, or ::GFNT_ERR_CORRUPT.
 */
GFNT_Result gfnt_reader_skip(GFNT_Reader * reader, size_t count);

/**
 * The cursor's position.
 *
 * @param reader The reader.
 * @return The offset, or 0 for NULL.
 */
size_t gfnt_reader_tell(const GFNT_Reader * reader);

/**
 * How many bytes are left.
 *
 * @param reader The reader.
 * @return The remaining length, or 0 for NULL.
 */
size_t gfnt_reader_remaining(const GFNT_Reader * reader);

/**
 * Whether a read of @p count bytes would fit.
 *
 * For a parser that wants to size an array before allocating it. Records no
 * diagnostic: a caller asking whether something fits has not failed yet.
 *
 * @param reader The reader.
 * @param count How many bytes.
 * @return true if a read of that many bytes from the cursor would succeed.
 */
bool gfnt_reader_has(const GFNT_Reader * reader, size_t count);

/** Read one byte and advance. */
GFNT_Result gfnt_read_u8(GFNT_Reader * reader, uint8_t * out_value);
/** Read a big-endian `uint16` and advance. */
GFNT_Result gfnt_read_u16(GFNT_Reader * reader, uint16_t * out_value);
/** Read a big-endian `uint24` and advance; the CFF and `EBDT` offset size. */
GFNT_Result gfnt_read_u24(GFNT_Reader * reader, uint32_t * out_value);
/** Read a big-endian `uint32` and advance. */
GFNT_Result gfnt_read_u32(GFNT_Reader * reader, uint32_t * out_value);
/** Read a big-endian `int8` and advance. */
GFNT_Result gfnt_read_s8(GFNT_Reader * reader, int8_t * out_value);
/** Read a big-endian `int16` - `FWORD`, `SHORT` - and advance. */
GFNT_Result gfnt_read_s16(GFNT_Reader * reader, int16_t * out_value);
/** Read a big-endian `int32` and advance. */
GFNT_Result gfnt_read_s32(GFNT_Reader * reader, int32_t * out_value);
/** Read a four-character tag and advance. */
GFNT_Result gfnt_read_tag(GFNT_Reader * reader, GFNT_Tag * out_value);
/** Read an `F2DOT14` and advance. */
GFNT_Result gfnt_read_f2dot14(GFNT_Reader * reader, GFNT_F2Dot14 * out_value);
/** Read the sfnt `Fixed` (16.16) and advance. */
GFNT_Result gfnt_read_fixed(GFNT_Reader * reader, GFNT_F16Dot16 * out_value);
/**
 * Read a `LONGDATETIME` and advance.
 *
 * Seconds since 1904-01-01 00:00 UTC, signed, as `head` carries them. The
 * value is not range-checked: a font with a nonsense date is not a corrupt
 * font, and what a date means is the caller's business.
 */
GFNT_Result gfnt_read_longdatetime(GFNT_Reader * reader, int64_t * out_value);

/**
 * Borrow @p count bytes at the cursor and advance past them.
 *
 * The pointer is into the extent and lives as long as the blob does. This is
 * how a bitmap's rows and a charstring's bytes are reached; the bytes are
 * bounds-checked as a block, and whatever walks them must stay inside
 * @p count.
 *
 * @param reader The reader.
 * @param count How many bytes.
 * @param out_bytes Receives the pointer. Written only on success; NULL is
 *   written for a zero-length request.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, or ::GFNT_ERR_CORRUPT.
 */
GFNT_Result gfnt_read_bytes(GFNT_Reader * reader, size_t count,
    const uint8_t ** out_bytes);

/**
 * Read a byte at an absolute offset, leaving the cursor alone.
 *
 * The `_at` family is what an indexed array in a table wants - `cmap` format
 * 4's four parallel arrays, `loca`'s entries - where seeking per element would
 * be both slower and easier to get wrong.
 */
GFNT_Result gfnt_reader_u8_at(const GFNT_Reader * reader, size_t offset,
    uint8_t * out_value);
/** Read a big-endian `uint16` at an absolute offset. */
GFNT_Result gfnt_reader_u16_at(const GFNT_Reader * reader, size_t offset,
    uint16_t * out_value);
/** Read a big-endian `uint32` at an absolute offset. */
GFNT_Result gfnt_reader_u32_at(const GFNT_Reader * reader, size_t offset,
    uint32_t * out_value);
/** Read a big-endian `int16` at an absolute offset. */
GFNT_Result gfnt_reader_s16_at(const GFNT_Reader * reader, size_t offset,
    int16_t * out_value);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_READER_H
