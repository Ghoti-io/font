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
 * @ref GFNT_Face "GFNT_Face": loading one font out of a blob, and what a loaded face will
 * answer before any table has been parsed.
 *
 * documentation/design.md sections 5.3, 7.1 and 7.8. A face load reads the
 * offset table and the directory and stops; the tables are parsed on first
 * use, so opening a collection to ask one question costs one question.
 *
 * Reference: OpenType Specification 1.9, "Organization of an OpenType Font";
 * the Apple TrueType Reference Manual, "TrueType Collections".
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/macros.h>
#include "sfnt.h"

/** Bytes in a TTC header before the offset array: tag, version, numFonts. */
#define GFNT_TTC_HEADER_BYTES 12

/**
 * The limits to use: the caller's, or the defaults.
 */
static void gfnt_face_limits(const GFNT_Limits * limits, GFNT_Limits * out) {
  if (limits) {
    *out = *limits;
    return;
  }
  gfnt_limits_default(out);
}

/**
 * Read a `ttcf` header and report how many faces it lists and where they are.
 *
 * @param reader A reader over the whole blob, positioned anywhere.
 * @param out_count Receives numFonts.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_UNSUPPORTED for a TTC version this library
 *   does not read, or ::GFNT_ERR_CORRUPT.
 */
static GFNT_Result gfnt_ttc_header(GFNT_Reader * reader, size_t * out_count,
    GFNT_Error * error) {
  uint16_t major = 0;
  uint16_t minor = 0;
  uint32_t count = 0;
  size_t bytes_needed;

  if (gfnt_reader_seek(reader, 4) != GFNT_OK
      || gfnt_read_u16(reader, &major) != GFNT_OK
      || gfnt_read_u16(reader, &minor) != GFNT_OK
      || gfnt_read_u32(reader, &count) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_FLAVOUR_COLLECTION, 4,
        GFNT_GLYPH_NONE, "the collection header ends early");
  }
  // Versions 1.0 and 2.0 differ only in the DSIG fields after the offset
  // array, which this library skips, so both read identically here.
  if (major != 1 && major != 2) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_FLAVOUR_COLLECTION,
        4, GFNT_GLYPH_NONE, "a collection version this library does not read");
  }
  (void)minor;

  // numFonts is a count from the file, so the offset array it claims has to be
  // there before the number means anything. Without this a 16-byte file can
  // claim four billion faces.
  if (!gcu_safe_mul_size(count, 4, &bytes_needed)
      || !gfnt_reader_has(reader, bytes_needed)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_FLAVOUR_COLLECTION, 8,
        GFNT_GLYPH_NONE,
        "the collection claims more faces than its header can hold");
  }

  *out_count = count;
  return GFNT_OK;
}

/**
 * Whether a blob that is not an sfnt is one of the containers that has no
 * directory, and which.
 *
 * Ordered by how strongly each format identifies itself, strongest first, so
 * that a weak magic never gets to claim a file some other reader would have
 * recognised outright. A bare CFF is last and will stay last: its header is
 * four bytes, two of which are a version number.
 *
 * @param blob A reader over the whole blob.
 * @param out_flavour Receives the flavour, on a match.
 * @param out_tag Receives the tag its one synthetic entry should carry.
 * @return Whether anything matched.
 */
static bool gfnt_face_bare_container(const GFNT_Reader * blob,
    GFNT_Tag * out_flavour, GFNT_Tag * out_tag) {
  if (gfnt_cff_header_plausible(blob)) {
    *out_flavour = GFNT_FLAVOUR_BARE_CFF;
    *out_tag = GFNT_TAG_CFF;
    return true;
  }
  return false;
}

GFNT_Result gfnt_face_count(const GFNT_Blob * blob, const GFNT_Limits * limits,
    size_t * out_count, GFNT_Error * error) {
  GFNT_Reader reader;
  GFNT_Tag tag = 0;
  GFNT_Limits effective;
  GFNT_Result result;
  size_t count = 0;

  gfnt_error_clear(error);
  if (!out_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "nowhere to put the count");
  }
  gfnt_face_limits(limits, &effective);

  result = gfnt_reader_init_blob(&reader, blob, 0, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_tag(&reader, &tag) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_FORMAT, 0, 0, GFNT_GLYPH_NONE,
        "too short to be a font");
  }

  if (tag == GFNT_FLAVOUR_COLLECTION) {
    result = gfnt_ttc_header(&reader, &count, error);
    if (result != GFNT_OK) {
      return result;
    }
    *out_count = count;
    return GFNT_OK;
  }

  // An ordinary sfnt is one face. Whether this library can read its outlines
  // is a question for the flavour, which the directory parse answers; whether
  // it is a font at all is this one.
  if (tag != GFNT_FLAVOUR_TRUETYPE && tag != GFNT_FLAVOUR_CFF
      && tag != GFNT_FLAVOUR_APPLE_TRUE && tag != GFNT_FLAVOUR_APPLE_TYPE1) {
    GFNT_Tag flavour = 0;
    GFNT_Tag entry = 0;

    // A container with no directory is one face too. This counts without
    // parsing the font program, which is the same bargain an sfnt gets: the
    // count says a face is there and the load says whether it can be read.
    if (gfnt_face_bare_container(&reader, &flavour, &entry)) {
      *out_count = 1;
      return GFNT_OK;
    }
    return gfnt_error_set(error, GFNT_ERR_FORMAT, 0, 0, GFNT_GLYPH_NONE,
        "not an sfnt version this library recognises, a collection, or a bare "
        "CFF");
  }
  *out_count = 1;
  return GFNT_OK;
}

/**
 * Give the face its table directory: the one the file carries, or a synthetic
 * one for a container that has no directory at all.
 *
 * @param face The face. Its blob, allocator, limits and directory_offset must
 *   already be set, and its lock must exist - a bare container's font program is
 *   parsed here, and that writes a memo.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return What the directory parse or the container's own parse returned.
 */
static GFNT_Result gfnt_face_build_directory(GFNT_Face * face,
    GFNT_Error * error) {
  GFNT_Reader blob_reader;
  GFNT_Tag flavour = 0;
  GFNT_Tag entry = 0;
  const GFNT_Cff * cff = NULL;
  GFNT_Result result = gfnt_sfnt_parse_directory(face, error);

  // ERR_FORMAT is the directory parse saying "this is not an sfnt", which is
  // the only result that hands the blob on. A corrupt sfnt is not offered to
  // another reader: it identified itself.
  if (result != GFNT_ERR_FORMAT) {
    return result;
  }
  // Only a whole file can be another container, and that is enforced by where
  // the probe reads rather than by a test of directory_offset here: the reader
  // below starts at byte 0 of the blob, so a face inside a collection is
  // offered its container's first bytes, which say `ttcf` and match nothing. A
  // guard on directory_offset would be unreachable - there is no blob that both
  // opens with `ttcf` and opens with another container's magic.
  result = gfnt_reader_init_blob(&blob_reader, face->blob, 0, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (!gfnt_face_bare_container(&blob_reader, &flavour, &entry)) {
    return gfnt_error_set(error, GFNT_ERR_FORMAT, 0, 0, GFNT_GLYPH_NONE,
        "not an sfnt, a collection, or any container this library recognises");
  }
  result = gfnt_sfnt_single_table_directory(face, flavour, entry, error);
  if (result != GFNT_OK) {
    return result;
  }

  // The font program is parsed now rather than on first use, which is the one
  // place this library departs from "loading a face parses the directory and
  // nothing else" (design.md section 7.8) - and it departs because for this
  // container the table *is* the identification. Four header bytes are not
  // evidence, so a blob is not a bare CFF until its whole font program has
  // parsed, and a face is not handed back for one that has not.
  //
  // The parse's own diagnostic is propagated rather than flattened to
  // ERR_FORMAT: "a DICT byte the format reserves" says far more about the file
  // than "not a font" does, and a caller who wanted the cheap question has
  // ::gfnt_face_count().
  return gfnt_face_cff(face, &cff, error);
}

GFNT_Result gfnt_face_load(const GFNT_Blob * blob, size_t index,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Face ** out_face, GFNT_Error * error) {
  GFNT_Reader reader;
  GFNT_Tag tag = 0;
  GFNT_Limits effective;
  GFNT_Face * face;
  size_t directory_offset = 0;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!out_face) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "nowhere to put the face");
  }
  gfnt_face_limits(limits, &effective);

  result = gfnt_reader_init_blob(&reader, blob, 0, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_tag(&reader, &tag) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_FORMAT, 0, 0, GFNT_GLYPH_NONE,
        "too short to be a font");
  }

  if (tag == GFNT_FLAVOUR_COLLECTION) {
    size_t count = 0;
    uint32_t offset = 0;

    result = gfnt_ttc_header(&reader, &count, error);
    if (result != GFNT_OK) {
      return result;
    }
    if (index >= count) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_FLAVOUR_COLLECTION,
          GFNT_TTC_HEADER_BYTES, GFNT_GLYPH_NONE,
          "the collection has no face at that index");
    }
    result = gfnt_reader_u32_at(&reader,
        GFNT_TTC_HEADER_BYTES + index * 4, &offset);
    if (result != GFNT_OK) {
      return result;
    }
    directory_offset = offset;
  }
  else if (index != 0) {
    // Asking for face 3 of a font that is not a collection is a caller error,
    // not a corrupt font.
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "this font is not a collection and has only face 0");
  }

  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  face = allocator->calloc_fn(allocator->ctx, 1, sizeof *face);
  if (!face) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the face");
  }
  face->blob = blob;
  face->allocator = allocator;
  face->limits = effective;
  face->index = index;
  face->directory_offset = directory_offset;

  if (GCU_MUTEX_CREATE(face->lock) != 0) {
    gfnt_face_free(face);
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
        "the platform refused a mutex for the face's table cache");
  }
  face->lock_ready = true;

  result = gfnt_face_build_directory(face, error);
  if (result != GFNT_OK) {
    gfnt_face_free(face);
    return result;
  }

  *out_face = face;
  return GFNT_OK;
}

void gfnt_face_free(GFNT_Face * face) {
  const GFNT_Allocator * allocator;

  if (!face) {
    return;
  }
  allocator = face->allocator;
  if (face->lock_ready) {
    GCU_MUTEX_DESTROY(face->lock);
  }
  allocator->free_fn(allocator->ctx, face->tables);
  // The blob is the caller's, and every other face of a collection is still
  // using it.
  allocator->free_fn(allocator->ctx, face);
}

size_t gfnt_face_index(const GFNT_Face * face) {
  return face ? face->index : 0;
}

GFNT_Tag gfnt_face_flavour(const GFNT_Face * face) {
  return face ? face->flavour : 0;
}

size_t gfnt_face_table_count(const GFNT_Face * face) {
  return face ? face->table_count : 0;
}

GFNT_Result gfnt_face_table_tag_at(const GFNT_Face * face, size_t index,
    GFNT_Tag * out_tag) {
  if (!face || !out_tag || index >= face->table_count) {
    return GFNT_ERR_INVALID;
  }
  *out_tag = face->tables[index].tag;
  return GFNT_OK;
}

bool gfnt_face_has_table(const GFNT_Face * face, GFNT_Tag tag) {
  return gfnt_sfnt_find(face, tag) != NULL;
}

GFNT_Result gfnt_face_table_range(const GFNT_Face * face, GFNT_Tag tag,
    size_t * out_offset, size_t * out_length) {
  const GFNT_SfntTable * entry;

  if (!face) {
    return GFNT_ERR_INVALID;
  }
  entry = gfnt_sfnt_find(face, tag);
  if (!entry) {
    return GFNT_ERR_UNSUPPORTED;
  }
  if (out_offset) {
    *out_offset = entry->offset;
  }
  if (out_length) {
    *out_length = entry->length;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_table_checksum(const GFNT_Face * face, GFNT_Tag tag,
    uint32_t * out_stored, uint32_t * out_computed) {
  const GFNT_SfntTable * entry;
  uint32_t computed = 0;
  GFNT_Result result;

  if (!face) {
    return GFNT_ERR_INVALID;
  }
  entry = gfnt_sfnt_find(face, tag);
  if (!entry) {
    return GFNT_ERR_UNSUPPORTED;
  }

  if (out_computed) {
    result = gfnt_sfnt_checksum(face, entry, &computed);
    if (result != GFNT_OK) {
      return result;
    }
    *out_computed = computed;
  }
  if (out_stored) {
    *out_stored = entry->checksum;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_dump(const GFNT_Face * face, FILE * out) {
  char tag[5];

  if (!face || !out) {
    return GFNT_ERR_INVALID;
  }

  if (fprintf(out, "face: index %zu, flavour '%s' (0x%08X), %zu tables\n",
          face->index, gfnt_tag_string(face->flavour, tag),
          (unsigned)face->flavour, face->table_count)
      < 0) {
    return GFNT_ERR_IO;
  }
  for (size_t i = 0; i < face->table_count; ++i) {
    const GFNT_SfntTable * entry = &face->tables[i];

    if (fprintf(out,
            "table '%s': offset %zu, length %zu, checksum 0x%08X\n",
            gfnt_tag_string(entry->tag, tag), entry->offset, entry->length,
            (unsigned)entry->checksum)
        < 0) {
      return GFNT_ERR_IO;
    }
  }
  return GFNT_OK;
}
