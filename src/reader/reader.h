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

/**
 * Which end of a multi-byte number a file puts first.
 *
 * Every sfnt-family format is big-endian and says so in its specification, so
 * the readers above have no order parameter and must not grow one. Two formats
 * this library reads are not sfnt: PSF is a Linux console format and is
 * little-endian throughout, and **PCF carries its byte order in the file** -
 * each table's format word says which end its numbers start at, because the
 * format was compiled on the machine that would serve it.
 *
 * So the order is a value, passed in, and never a compile-time decision.
 * `check-reader` refuses `__BYTE_ORDER__` and every `htons` spelling for that
 * reason: a parse whose result depends on the host is one the big-endian cross
 * container cannot check, and both of these numbers are assembled with shifts
 * either way.
 */
typedef enum {
  GFNT_ORDER_MSB_FIRST = 0, ///< Most significant byte first, as sfnt has it.
  GFNT_ORDER_LSB_FIRST      ///< Least significant first: PCF's choice, PSF's.
} GFNT_ByteOrder;

/** Read a `uint16` in @p order and advance. */
GFNT_Result gfnt_read_u16_order(GFNT_Reader * reader, GFNT_ByteOrder order,
    uint16_t * out_value);
/** Read a `uint32` in @p order and advance. */
GFNT_Result gfnt_read_u32_order(GFNT_Reader * reader, GFNT_ByteOrder order,
    uint32_t * out_value);
/** Read an `int16` in @p order and advance. */
GFNT_Result gfnt_read_s16_order(GFNT_Reader * reader, GFNT_ByteOrder order,
    int16_t * out_value);
/** Read an `int32` in @p order and advance. */
GFNT_Result gfnt_read_s32_order(GFNT_Reader * reader, GFNT_ByteOrder order,
    int32_t * out_value);
/** Read a `uint32` in @p order at an absolute offset, leaving the cursor. */
GFNT_Result gfnt_reader_u32_order_at(const GFNT_Reader * reader,
    GFNT_ByteOrder order, size_t offset, uint32_t * out_value);

/**
 * A cursor that hands out one line at a time.
 *
 * documentation/design.md section 5.1: there is no `GFNT_Stream` here, because a
 * font is random-access by construction - but two of the formats are text, and
 * BDF and `.hex` are read line by line. This is that, over an ordinary
 * ::GFNT_Reader, so a line-oriented parse is bounded by its table's extent like
 * every other and `check-reader` has nothing to say about it.
 *
 * A line is the bytes before the terminator, which is LF, CRLF, or a lone CR -
 * all three, because a BDF written on a Mac in 1994 is still a BDF. The
 * terminator is not included and the last line needs none.
 */
typedef struct GFNT_Lines {
  GFNT_Reader reader;  ///< The extent; its cursor is the read position.
  size_t max_length;   ///< ::GFNT_Limits::max_line_length, never zero.
  size_t number;       ///< 1-based number of the line last handed out.
  size_t offset;       ///< Where that line started, for diagnostics.
} GFNT_Lines;

/**
 * Set up a line cursor over @p reader's remaining bytes.
 *
 * @param lines The cursor to initialise.
 * @param reader The extent to read. Copied; the original is left alone.
 * @param max_length The cap on one line, from the face's limits.
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID.
 */
GFNT_Result gfnt_lines_init(GFNT_Lines * lines, const GFNT_Reader * reader,
    size_t max_length);

/**
 * The next line, or the news that there are no more.
 *
 * @param lines The cursor.
 * @param out_bytes Receives a pointer to the line's bytes, NULL for an empty
 *   line. Borrowed from the extent.
 * @param out_length Receives its length, excluding the terminator.
 * @param out_more Receives false when the extent is exhausted, and then nothing
 *   else is written.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_CORRUPT, or
 *   ::GFNT_ERR_LIMIT for a line longer than @p max_length - which is a limit
 *   and not corruption, because a 4,097-byte line is a legal BDF comment.
 */
GFNT_Result gfnt_lines_next(GFNT_Lines * lines, const uint8_t ** out_bytes,
    size_t * out_length, bool * out_more, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_READER_H
