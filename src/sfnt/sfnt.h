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
 * Internal shape of a face: the directory it parsed, and how the rest of the
 * library gets a reader over one of its tables.
 *
 * documentation/design.md sections 5.3, 7.1 and 7.8. Internal because the
 * struct is this library's to change; a consumer reads a face through
 * face.h's accessors.
 *
 * Reference: OpenType Specification 1.9, "Organization of an OpenType Font"
 * (the offset table, the table directory, and TTC headers); the Apple
 * TrueType Reference Manual, "The Font File".
 */

#ifndef GHOTI_IO_GFNT_SFNT_H
#define GHOTI_IO_GFNT_SFNT_H

#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include "../reader/reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One entry of the table directory, as the file gives it.
 *
 * The offset and length are from the start of the **blob**, validated against
 * it at load; a reader derived from them therefore spans exactly this table.
 */
typedef struct GFNT_SfntTable {
  GFNT_Tag tag;     ///< Four characters, in file order.
  uint32_t checksum; ///< What the directory claims. Never enforced.
  size_t offset;    ///< From the start of the blob.
  size_t length;    ///< As the directory gives it.
} GFNT_SfntTable;

/**
 * One font from a blob.
 *
 * `blob` is borrowed - the caller keeps it alive, and every face of a
 * collection shares one. `limits` is a copy, so that a caller cannot change
 * the caps a face was loaded under by mutating the struct it passed.
 */
struct GFNT_Face {
  const GFNT_Blob * blob;           ///< Borrowed; must outlive the face.
  const GFNT_Allocator * allocator; ///< Where this face came from.
  GFNT_Limits limits;               ///< A copy of the caller's caps.
  size_t index;                     ///< Which face of the collection.
  GFNT_Tag flavour;                 ///< The sfnt version.
  size_t directory_offset;          ///< Where this face's offset table is.
  GFNT_SfntTable * tables;          ///< Directory entries, in file order.
  size_t table_count;               ///< How many.
};

/**
 * Read the offset table and directory of the face at @p directory_offset.
 *
 * Fills in `flavour`, `tables` and `table_count`; allocates the table array
 * with the face's allocator.
 *
 * @param face The face to fill in. Its blob, allocator, limits and
 *   directory_offset must already be set.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_FORMAT, ::GFNT_ERR_CORRUPT, ::GFNT_ERR_LIMIT
 *   or ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_sfnt_parse_directory(GFNT_Face * face, GFNT_Error * error);

/**
 * Find a directory entry by tag.
 *
 * @param face The face.
 * @param tag The tag.
 * @return The entry, or NULL if the face has no such table.
 */
const GFNT_SfntTable * gfnt_sfnt_find(const GFNT_Face * face, GFNT_Tag tag);

/**
 * Set up a reader over one of the face's tables.
 *
 * The only way the parsers reach a table's bytes, so that every table read is
 * bounded by that table's own extent.
 *
 * @param face The face.
 * @param tag The table.
 * @param out_reader Receives the reader. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, or ::GFNT_ERR_UNSUPPORTED if the
 *   face has no such table - which is not a corrupt font, because required
 *   tables are per operation (design.md section 7.8).
 */
GFNT_Result gfnt_face_table_reader(const GFNT_Face * face, GFNT_Tag tag,
    GFNT_Reader * out_reader, GFNT_Error * error);

/**
 * The checksum of a table's bytes, as the specification computes one.
 *
 * @param face The face.
 * @param entry The directory entry.
 * @param out_checksum Receives the checksum.
 * @return ::GFNT_OK, or ::GFNT_ERR_CORRUPT if the bytes are not there.
 */
GFNT_Result gfnt_sfnt_checksum(const GFNT_Face * face,
    const GFNT_SfntTable * entry, uint32_t * out_checksum);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_SFNT_H
