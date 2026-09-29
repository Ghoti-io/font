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
 * The sfnt container: the offset table, the table directory, and the `ttcf`
 * collection header.
 *
 * documentation/design.md section 7.1. Four things are deliberate here:
 *
 * - `searchRange`, `entrySelector` and `rangeShift` are **derived, never
 *   read**. They are redundant with numTables, fonts in the wild get them
 *   wrong, and a parser that trusts them can be steered by them.
 * - Every table's offset and length are validated against the blob at load,
 *   so a reader derived from a directory entry spans that table and no more.
 * - **Overlapping tables are permitted.** Fonts in the wild share bytes
 *   between tables deliberately, and refusing them would refuse fonts every
 *   other implementation reads.
 * - **Checksums are reported, never enforced**, for the same reason.
 *
 * Reference: OpenType Specification 1.9, "Organization of an OpenType Font";
 * the Apple TrueType Reference Manual, "The Font File" and "TrueType
 * Collections".
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/macros.h>
#include "sfnt.h"

/** Bytes in one directory entry: tag, checksum, offset, length. */
#define GFNT_SFNT_ENTRY_BYTES 16

/** Bytes in an offset table: version, numTables, and the three derived. */
#define GFNT_SFNT_OFFSET_TABLE_BYTES 12

/** Offset of `checkSumAdjustment` within `head`. */
#define GFNT_HEAD_ADJUSTMENT_OFFSET 8

/**
 * Whether a tag is an sfnt version this library reads.
 */
static bool gfnt_sfnt_is_flavour(GFNT_Tag tag) {
  return tag == GFNT_FLAVOUR_TRUETYPE || tag == GFNT_FLAVOUR_CFF
      || tag == GFNT_FLAVOUR_APPLE_TRUE || tag == GFNT_FLAVOUR_APPLE_TYPE1;
}

void gfnt_face_adopt_bytes(GFNT_Face * face, GFNT_Blob * derived) {
  GFNT_Blob * previous = face->owned;

  face->bytes = derived;
  face->owned = derived;
  // After, not before: the new bytes were built by reading the old ones, so the
  // old ones have to outlive the derivation that consumed them.
  gfnt_blob_destroy(previous);
}

GFNT_Result gfnt_sfnt_table_directory(GFNT_Face * face, GFNT_Tag flavour,
    const GFNT_SfntTable * entries, size_t count, GFNT_Error * error) {
  GFNT_Reader blob_reader;
  GFNT_SfntTable * tables;
  GFNT_Result result;

  if (count == 0) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, flavour, 0, GFNT_GLYPH_NONE,
        "a synthetic directory with no entries in it");
  }
  if (count > face->limits.max_tables) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, flavour, 0, GFNT_GLYPH_NONE,
        "more tables than GFNT_Limits::max_tables");
  }
  result = gfnt_reader_init_blob(&blob_reader, face->bytes, 0, error);
  if (result != GFNT_OK) {
    return result;
  }
  // Every entry is checked against the blob here rather than by the container
  // that built it, so that a synthetic directory is validated by the same rule a
  // file's own directory is: an entry that runs past the bytes is refused before
  // anything derives a reader from it.
  for (size_t i = 0; i < count; ++i) {
    size_t end;

    if (!gcu_safe_add_size(entries[i].offset, entries[i].length, &end)
        || end > blob_reader.length) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, entries[i].tag,
          entries[i].offset, GFNT_GLYPH_NONE,
          "a table that ends past the end of the file");
    }
  }
  tables = face->allocator->calloc_fn(face->allocator->ctx, count,
      sizeof *tables);
  if (!tables) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the synthetic table directory");
  }
  for (size_t i = 0; i < count; ++i) {
    tables[i] = entries[i];
  }
  face->flavour = flavour;
  face->tables = tables;
  face->table_count = count;
  return GFNT_OK;
}

GFNT_Result gfnt_sfnt_single_table_directory(GFNT_Face * face,
    GFNT_Tag flavour, GFNT_Tag tag, GFNT_Error * error) {
  GFNT_Reader blob_reader;
  GFNT_SfntTable entry;
  GFNT_Result result;

  result = gfnt_reader_init_blob(&blob_reader, face->bytes, 0, error);
  if (result != GFNT_OK) {
    return result;
  }
  // The checksum a directory would have carried is left at zero, which is what
  // gfnt_face_table_checksum() reports as the claim. There is nothing to claim:
  // no directory said anything about these bytes, and computing the checksum
  // and storing it as the claim would manufacture an agreement that was never
  // made - a check that can only ever pass.
  entry = (GFNT_SfntTable) {
    .tag = tag,
    .checksum = 0,
    .offset = 0,
    .length = blob_reader.length,
  };
  return gfnt_sfnt_table_directory(face, flavour, &entry, 1, error);
}

GFNT_Result gfnt_sfnt_parse_directory(GFNT_Face * face, GFNT_Error * error) {
  GFNT_Reader blob_reader;
  GFNT_Reader reader;
  GFNT_Tag flavour = 0;
  uint16_t table_count = 0;
  GFNT_SfntTable * tables;
  size_t blob_size;
  GFNT_Result result;

  // `bytes` and not `blob`: a file that arrived compressed has been inflated by
  // now, and the directory this reads is the inflated one's.
  result = gfnt_reader_init_blob(&blob_reader, face->bytes, 0, error);
  if (result != GFNT_OK) {
    return result;
  }
  blob_size = blob_reader.length;

  result = gfnt_reader_sub(&blob_reader, face->directory_offset,
      GFNT_READER_REST, &reader);
  if (result != GFNT_OK) {
    return result;
  }

  if (gfnt_read_tag(&reader, &flavour) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_FORMAT, 0, face->directory_offset,
        GFNT_GLYPH_NONE, "too short to hold an sfnt offset table");
  }
  if (!gfnt_sfnt_is_flavour(flavour)) {
    // Not a corrupt font: a file that is not an sfnt at all is ERR_FORMAT, and
    // the PCF, BDF and Type 1 readers get their turn at it.
    return gfnt_error_set(error, GFNT_ERR_FORMAT, 0, face->directory_offset,
        GFNT_GLYPH_NONE, "not an sfnt version this library recognises");
  }
  if (gfnt_read_u16(&reader, &table_count) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_FORMAT, 0, face->directory_offset,
        GFNT_GLYPH_NONE, "too short to hold an sfnt offset table");
  }
  if (table_count > face->limits.max_tables) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, face->directory_offset,
        GFNT_GLYPH_NONE, "more tables than GFNT_Limits::max_tables");
  }

  // searchRange, entrySelector and rangeShift: derived from numTables, so they
  // are skipped rather than read. A font that gets them wrong is common; a
  // parser that believes them is steerable.
  result = gfnt_reader_seek(&reader, GFNT_SFNT_OFFSET_TABLE_BYTES);
  if (result != GFNT_OK) {
    return result;
  }

  tables = face->allocator->calloc_fn(face->allocator->ctx,
      table_count > 0 ? table_count : 1, sizeof *tables);
  if (!tables) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the table directory");
  }

  for (size_t i = 0; i < table_count; ++i) {
    GFNT_Tag tag = 0;
    uint32_t checksum = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    size_t end;

    if (gfnt_read_tag(&reader, &tag) != GFNT_OK
        || gfnt_read_u32(&reader, &checksum) != GFNT_OK
        || gfnt_read_u32(&reader, &offset) != GFNT_OK
        || gfnt_read_u32(&reader, &length) != GFNT_OK) {
      face->allocator->free_fn(face->allocator->ctx, tables);
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, 0,
          face->directory_offset + GFNT_SFNT_OFFSET_TABLE_BYTES
              + i * GFNT_SFNT_ENTRY_BYTES,
          GFNT_GLYPH_NONE, "the table directory ends mid-entry");
    }

    // Against the blob, not against this face's own extent: a collection's
    // faces point at tables all over the file, and often at each other's.
    if (!gcu_safe_add_size(offset, length, &end) || end > blob_size) {
      face->allocator->free_fn(face->allocator->ctx, tables);
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, offset,
          GFNT_GLYPH_NONE, "a table lies partly or wholly outside the font");
    }

    tables[i] = (GFNT_SfntTable) {
      .tag = tag,
      .checksum = checksum,
      .offset = offset,
      .length = length,
    };
  }

  face->flavour = flavour;
  face->tables = tables;
  face->table_count = table_count;
  return GFNT_OK;
}

const GFNT_SfntTable * gfnt_sfnt_find(const GFNT_Face * face, GFNT_Tag tag) {
  if (!face) {
    return NULL;
  }

  // A linear scan over at most max_tables entries. The directory is supposed
  // to be sorted by tag, but fonts exist whose directories are not, so a
  // binary search would answer "absent" for a table the font has.
  for (size_t i = 0; i < face->table_count; ++i) {
    if (face->tables[i].tag == tag) {
      return &face->tables[i];
    }
  }
  return NULL;
}

GFNT_Producer gfnt_sfnt_producer(const GFNT_Face * face) {
  if (!face) {
    return GFNT_PRODUCER_NONE;
  }
  // A Type 1 face is the whole file, so its one synthetic table settles this
  // before any directory question is asked.
  if (face->flavour == GFNT_FLAVOUR_TYPE1) {
    return GFNT_PRODUCER_TYPE1;
  }
  if (gfnt_sfnt_find(face, GFNT_TAG('g', 'l', 'y', 'f'))
      && gfnt_sfnt_find(face, GFNT_TAG('l', 'o', 'c', 'a'))) {
    return GFNT_PRODUCER_GLYF;
  }
  if (gfnt_sfnt_find(face, GFNT_TAG_CFF)) {
    return GFNT_PRODUCER_CFF;
  }
  return GFNT_PRODUCER_NONE;
}

GFNT_Result gfnt_face_table_reader(const GFNT_Face * face, GFNT_Tag tag,
    GFNT_Reader * out_reader, GFNT_Error * error) {
  const GFNT_SfntTable * entry;
  GFNT_Reader blob_reader;
  GFNT_Result result;

  if (!face || !out_reader) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, tag, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the reader");
  }
  entry = gfnt_sfnt_find(face, tag);
  if (!entry) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, 0, GFNT_GLYPH_NONE,
        "the font has no such table");
  }

  // `bytes` rather than `blob`: for a container whose font program had to be
  // decrypted or decompressed before anything could read it, the directory
  // describes the derived bytes and not the file's.
  result = gfnt_reader_init_blob(&blob_reader, face->bytes, tag, error);
  if (result != GFNT_OK) {
    return result;
  }
  return gfnt_reader_sub(&blob_reader, entry->offset, entry->length,
      out_reader);
}

GFNT_Result gfnt_sfnt_checksum(const GFNT_Face * face,
    const GFNT_SfntTable * entry, uint32_t * out_checksum) {
  GFNT_Reader reader;
  uint32_t sum = 0;
  size_t words;
  GFNT_Result result;

  if (!face || !entry || !out_checksum) {
    return GFNT_ERR_INVALID;
  }

  result = gfnt_face_table_reader(face, entry->tag, &reader, NULL);
  if (result != GFNT_OK) {
    return result;
  }

  // The checksum is the sum of the table's big-endian uint32s, with the table
  // padded to a multiple of four bytes with zeros - so the last word of an
  // unaligned table is assembled from what is there and nothing else.
  words = (entry->length + 3) / 4;
  for (size_t i = 0; i < words; ++i) {
    uint32_t word = 0;

    for (size_t byte = 0; byte < 4; ++byte) {
      size_t offset = i * 4 + byte;
      uint8_t value = 0;

      if (offset < entry->length) {
        if (gfnt_reader_u8_at(&reader, offset, &value) != GFNT_OK) {
          return GFNT_ERR_CORRUPT;
        }
        // head carries the checksum of the whole font, so its own entry is
        // computed with that field zeroed; otherwise the number would depend
        // on itself. The specification says so and every writer does it.
        if (entry->tag == GFNT_TAG('h', 'e', 'a', 'd')
            && offset >= GFNT_HEAD_ADJUSTMENT_OFFSET
            && offset < GFNT_HEAD_ADJUSTMENT_OFFSET + 4) {
          value = 0;
        }
      }
      word = (word << 8) | (uint32_t)value;
    }
    // Unsigned, so the wrap the specification's "sum" means is defined.
    sum += word;
  }

  *out_checksum = sum;
  return GFNT_OK;
}
