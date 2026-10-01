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
 * `cmap`: the subtable directory, the preference order, and the lookups.
 *
 * documentation/design.md section 7.2. Formats 0, 4, 6 and 12 are here -
 * phase 0 needs 4 and 12, and 0 and 6 come with them because the Macintosh and
 * symbol subtables the preference order falls back to are usually one of those
 * two, so without them the order would name subtables nothing could read.
 * Formats 2, 8, 10, 13 and 14 are ::GFNT_ERR_UNSUPPORTED until phase 2, which
 * is a different answer from ::GFNT_ERR_CORRUPT on purpose.
 *
 * Nothing here allocates: a lookup reads the subtable through the checked
 * reader on demand, so asking one question of a 30 MB CJK font costs one
 * question. The segment searches are linear rather than binary, because a
 * binary search needs the subtable to be sorted and sortedness is a property of
 * the file - which is to say, a property an attacker chooses.
 *
 * Reference: OpenType Specification 1.9, "cmap - Character to Glyph Index
 * Mapping Table"; the Apple TrueType Reference Manual, "The 'cmap' table".
 */

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/macros.h>
#include "../bitmap/bitmap.h"
#include "../tables/tables.h"

/** Bytes in a `cmap` header: version and numTables. */
#define GFNT_CMAP_HEADER_BYTES 4

/** Bytes in one encoding record: platform, encoding, offset. */
#define GFNT_CMAP_RECORD_BYTES 8

/** The `cmap` tag, spelled once. */
#define GFNT_CMAP_TAG GFNT_TAG('c', 'm', 'a', 'p')

/**
 * Whether this library can look a codepoint up in a subtable of this format.
 */
static bool gfnt_cmap_format_supported(uint16_t format) {
  return format == 0 || format == 4 || format == 6 || format == 12;
}

/**
 * A reader over the whole `cmap` table, and the number of encoding records.
 */
static GFNT_Result gfnt_cmap_open(const GFNT_Face * face, GFNT_Reader * out,
    size_t * out_count, GFNT_Error * error) {
  uint16_t version = 0;
  uint16_t count = 0;
  GFNT_Result result;

  result = gfnt_face_table_reader(face, GFNT_CMAP_TAG, out, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_u16(out, &version) != GFNT_OK
      || gfnt_read_u16(out, &count) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  // The version is 0 and has been since 1991; fonts that write something else
  // still carry a readable record list, so it is not checked.
  (void)version;
  if (out_count) {
    *out_count = count;
  }
  return GFNT_OK;
}

/**
 * One encoding record, with the subtable's format read from the subtable.
 */
static GFNT_Result gfnt_cmap_record(const GFNT_Face * face, size_t index,
    GFNT_CmapSubtable * out_subtable, GFNT_Error * error) {
  GFNT_Reader cmap;
  size_t count = 0;
  size_t at;
  uint32_t offset = 0;
  GFNT_Result result;

  result = gfnt_cmap_open(face, &cmap, &count, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (index >= count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_CMAP_TAG, 2,
        GFNT_GLYPH_NONE, "the cmap has no subtable at that index");
  }

  at = GFNT_CMAP_HEADER_BYTES + index * GFNT_CMAP_RECORD_BYTES;
  *out_subtable = (GFNT_CmapSubtable) {0};
  if (gfnt_reader_u16_at(&cmap, at, &out_subtable->platform_id) != GFNT_OK
      || gfnt_reader_u16_at(&cmap, at + 2, &out_subtable->encoding_id)
          != GFNT_OK
      || gfnt_reader_u32_at(&cmap, at + 4, &offset) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }

  out_subtable->offset = offset;
  // The format comes from the subtable rather than from the encoding: the two
  // are independent, and fonts pair them freely.
  if (gfnt_reader_u16_at(&cmap, offset, &out_subtable->format) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_CMAP_TAG, at + 4,
        GFNT_GLYPH_NONE, "a cmap subtable offset leaves the table");
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_cmap_count(const GFNT_Face * face, size_t * out_count,
    GFNT_Error * error) {
  GFNT_Reader cmap;
  size_t count = 0;
  GFNT_Result result;

  if (!face || !out_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_CMAP_TAG, 0,
        GFNT_GLYPH_NONE, "no face, or nowhere to put the count");
  }
  result = gfnt_cmap_open(face, &cmap, &count, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_count = count;
  return GFNT_OK;
}

GFNT_Result gfnt_face_cmap_at(const GFNT_Face * face, size_t index,
    GFNT_CmapSubtable * out_subtable, GFNT_Error * error) {
  if (!face || !out_subtable) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_CMAP_TAG, 0,
        GFNT_GLYPH_NONE, "no face, or nowhere to put the subtable");
  }
  return gfnt_cmap_record(face, index, out_subtable, error);
}

/**
 * The preference order of design.md section 7.2, as a rank: lower is better.
 *
 * Full Unicode first, then the Basic Multilingual Plane, then the two
 * single-byte encodings that are all some fonts have.
 */
static int gfnt_cmap_rank(uint16_t platform, uint16_t encoding) {
  static const struct {
    uint16_t platform;
    uint16_t encoding;
  } order[] = {
    {GFNT_PLATFORM_WINDOWS, 10},  // UCS-4
    {GFNT_PLATFORM_UNICODE, 6},   // full repertoire
    {GFNT_PLATFORM_UNICODE, 4},   // UCS-4 (deprecated spelling)
    {GFNT_PLATFORM_WINDOWS, 1},   // BMP
    {GFNT_PLATFORM_UNICODE, 3},   // BMP
    {GFNT_PLATFORM_WINDOWS, 0},   // symbol, with its 0xF0xx mapping
    {GFNT_PLATFORM_MACINTOSH, 0}, // Macintosh Roman
  };

  for (size_t i = 0; i < GFNT_ARRAY_SIZE(order); ++i) {
    if (order[i].platform == platform && order[i].encoding == encoding) {
      return (int)i;
    }
  }
  return -1;
}

/**
 * Choose the subtable to use, skipping formats this library cannot read.
 */
static GFNT_Result gfnt_cmap_best_parse(const GFNT_Face * face, void * out,
    GFNT_Error * error) {
  GFNT_CmapSubtable * best = out;
  size_t count = 0;
  int best_rank = -1;
  GFNT_Result result;

  result = gfnt_face_cmap_count(face, &count, error);
  if (result != GFNT_OK) {
    return result;
  }

  *best = (GFNT_CmapSubtable) {0};
  for (size_t i = 0; i < count; ++i) {
    GFNT_CmapSubtable candidate;
    int rank;

    // A subtable this library cannot read - a broken offset, an unsupported
    // format - is passed over rather than failing the whole choice: the next
    // preference is often the one every other implementation uses anyway.
    if (gfnt_cmap_record(face, i, &candidate, NULL) != GFNT_OK) {
      continue;
    }
    if (!gfnt_cmap_format_supported(candidate.format)) {
      continue;
    }
    rank = gfnt_cmap_rank(candidate.platform_id, candidate.encoding_id);
    if (rank < 0) {
      continue;
    }
    if (best_rank < 0 || rank < best_rank) {
      best_rank = rank;
      *best = candidate;
    }
  }

  if (best_rank < 0) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_CMAP_TAG, 0,
        GFNT_GLYPH_NONE,
        "the cmap has no subtable in an encoding and format this library "
        "reads");
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_cmap_best(const GFNT_Face * face,
    GFNT_CmapSubtable * out_subtable, GFNT_Error * error) {
  GFNT_Face * owner = (GFNT_Face *)face;
  GFNT_CmapSubtable scratch;
  GFNT_Result result;

  if (!face || !out_subtable) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_CMAP_TAG, 0,
        GFNT_GLYPH_NONE, "no face, or nowhere to put the subtable");
  }
  // Memoised: every codepoint lookup needs this, and it costs a walk of the
  // whole record list.
  result = gfnt_table_cached(face, &owner->cmap_best_state, &owner->cmap_best,
      &scratch, sizeof scratch, gfnt_cmap_best_parse, NULL, error);
  if (result == GFNT_OK) {
    *out_subtable = owner->cmap_best;
  }
  return result;
}

/**
 * Format 0: a 256-byte array of glyph indices, one per byte value.
 */
static GFNT_Result gfnt_cmap_lookup_0(GFNT_Reader * subtable,
    uint32_t codepoint, uint32_t * out_glyph) {
  uint8_t glyph = 0;

  if (codepoint > 0xFF) {
    *out_glyph = 0;
    return GFNT_OK;
  }
  if (gfnt_reader_u8_at(subtable, 6 + codepoint, &glyph) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  *out_glyph = glyph;
  return GFNT_OK;
}

/**
 * Format 6: a trimmed array covering firstCode to firstCode + entryCount.
 */
static GFNT_Result gfnt_cmap_lookup_6(GFNT_Reader * subtable,
    uint32_t codepoint, uint32_t * out_glyph) {
  uint16_t first = 0;
  uint16_t entries = 0;
  uint16_t glyph = 0;

  if (gfnt_reader_u16_at(subtable, 6, &first) != GFNT_OK
      || gfnt_reader_u16_at(subtable, 8, &entries) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  if (codepoint < first || codepoint >= (uint32_t)first + entries) {
    *out_glyph = 0;
    return GFNT_OK;
  }
  if (gfnt_reader_u16_at(subtable, 10 + (codepoint - first) * 2, &glyph)
      != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  *out_glyph = glyph;
  return GFNT_OK;
}

/**
 * Format 4: segments, with the arithmetic M10 is about.
 *
 * Four parallel arrays of segCount entries each - endCode, startCode, idDelta,
 * idRangeOffset - after a header whose searchRange, entrySelector and
 * rangeShift are derived and therefore skipped.
 */
static GFNT_Result gfnt_cmap_lookup_4(GFNT_Reader * subtable,
    uint32_t codepoint, uint32_t * out_glyph, GFNT_Error * error) {
  uint16_t seg_count_x2 = 0;
  size_t seg_count;
  size_t end_codes = 14;
  size_t start_codes;
  size_t id_deltas;
  size_t id_range_offsets;

  if (codepoint > 0xFFFF) {
    // The format cannot express anything above the BMP; that is not corrupt.
    *out_glyph = 0;
    return GFNT_OK;
  }
  if (gfnt_reader_u16_at(subtable, 6, &seg_count_x2) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  if ((seg_count_x2 & 1u) != 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_CMAP_TAG, 6,
        GFNT_GLYPH_NONE, "cmap format 4 segCountX2 is odd");
  }
  seg_count = seg_count_x2 / 2;
  start_codes = end_codes + seg_count_x2 + 2; // the reservedPad between them
  id_deltas = start_codes + seg_count_x2;
  id_range_offsets = id_deltas + seg_count_x2;

  for (size_t i = 0; i < seg_count; ++i) {
    uint16_t end = 0;
    uint16_t start = 0;
    int16_t delta = 0;
    uint16_t range_offset = 0;
    uint16_t glyph = 0;

    if (gfnt_reader_u16_at(subtable, end_codes + i * 2, &end) != GFNT_OK) {
      return GFNT_ERR_CORRUPT;
    }
    if (codepoint > end) {
      continue;
    }
    if (gfnt_reader_u16_at(subtable, start_codes + i * 2, &start) != GFNT_OK
        || gfnt_reader_s16_at(subtable, id_deltas + i * 2, &delta) != GFNT_OK
        || gfnt_reader_u16_at(subtable, id_range_offsets + i * 2,
               &range_offset)
            != GFNT_OK) {
      return GFNT_ERR_CORRUPT;
    }
    if (codepoint < start) {
      // Inside no segment: the codepoint falls in the gap this segment's
      // start leaves, and the segments are in order, so nothing later can
      // hold it either.
      *out_glyph = 0;
      return GFNT_OK;
    }

    if (range_offset == 0) {
      // The whole segment is one arithmetic run. The modulus is the format's
      // own: idDelta wraps deliberately, which is how a segment maps 0xF000
      // upwards to low glyph indices.
      *out_glyph = ((uint32_t)codepoint + (uint32_t)(int32_t)delta) & 0xFFFFu;
      return GFNT_OK;
    }

    // M10, the arithmetic every implementation gets wrong at least once: the
    // offset is from the address of *this* idRangeOffset entry, not from the
    // start of the subtable and not from the start of the array. Computed in
    // size_t, so the 16-bit overflow that version of this bug relies on cannot
    // happen, and bounds-checked by the reader like everything else.
    {
      size_t at = id_range_offsets + i * 2 + (size_t)range_offset
          + ((size_t)codepoint - (size_t)start) * 2;

      if (gfnt_reader_u16_at(subtable, at, &glyph) != GFNT_OK) {
        return GFNT_ERR_CORRUPT;
      }
    }
    // A zero in the glyph array means unmapped, and idDelta is *not* applied
    // to it: adding the delta would turn "no glyph" into a real one.
    *out_glyph = glyph == 0
        ? 0u
        : (((uint32_t)glyph + (uint32_t)(int32_t)delta) & 0xFFFFu);
    return GFNT_OK;
  }

  *out_glyph = 0;
  return GFNT_OK;
}

/**
 * Format 12: groups of (startCharCode, endCharCode, startGlyphID), 32 bits
 * each, so the whole of Unicode fits.
 */
static GFNT_Result gfnt_cmap_lookup_12(GFNT_Reader * subtable,
    uint32_t codepoint, uint32_t * out_glyph, GFNT_Error * error) {
  uint32_t groups = 0;

  if (gfnt_reader_u32_at(subtable, 12, &groups) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }

  for (uint32_t i = 0; i < groups; ++i) {
    size_t at = 16 + (size_t)i * 12;
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t start_glyph = 0;

    if (gfnt_reader_u32_at(subtable, at, &start) != GFNT_OK
        || gfnt_reader_u32_at(subtable, at + 4, &end) != GFNT_OK
        || gfnt_reader_u32_at(subtable, at + 8, &start_glyph) != GFNT_OK) {
      return GFNT_ERR_CORRUPT;
    }
    if (end < start) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_CMAP_TAG, at,
          GFNT_GLYPH_NONE, "a cmap format 12 group ends before it starts");
    }
    if (codepoint < start) {
      break;
    }
    if (codepoint <= end) {
      // start_glyph + (codepoint - start) is computed in 64 bits: both come
      // from the file, and a group claiming a start glyph near UINT32_MAX must
      // not wrap into a low glyph index.
      uint64_t glyph = (uint64_t)start_glyph + (codepoint - start);

      if (glyph > 0xFFFFFFFFull) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_CMAP_TAG, at + 8,
            GFNT_GLYPH_NONE,
            "a cmap format 12 group maps past the largest glyph index");
      }
      *out_glyph = (uint32_t)glyph;
      return GFNT_OK;
    }
  }

  *out_glyph = 0;
  return GFNT_OK;
}

GFNT_Result gfnt_cmap_lookup(const GFNT_Face * face,
    const GFNT_CmapSubtable * subtable, uint32_t codepoint,
    uint32_t * out_glyph, GFNT_Error * error) {
  GFNT_Reader cmap;
  GFNT_Reader reader;
  GFNT_Result result;

  if (!face || !subtable || !out_glyph) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_CMAP_TAG, 0,
        GFNT_GLYPH_NONE, "no face, no subtable, or nowhere to put the glyph");
  }

  result = gfnt_cmap_open(face, &cmap, NULL, error);
  if (result != GFNT_OK) {
    return result;
  }
  // The subtable's own extent is not known from the record - the length is
  // inside the subtable and is a uint16 in some formats and a uint32 in others
  // - so the reader spans from the subtable's offset to the end of the cmap.
  // Every read inside it is still bounded by the table, which is what matters:
  // a subtable cannot reach into the next table this way.
  result = gfnt_reader_sub(&cmap, subtable->offset, GFNT_READER_REST, &reader);
  if (result != GFNT_OK) {
    return result;
  }

  switch (subtable->format) {
    case 0:
      return gfnt_cmap_lookup_0(&reader, codepoint, out_glyph);
    case 4:
      return gfnt_cmap_lookup_4(&reader, codepoint, out_glyph, error);
    case 6:
      return gfnt_cmap_lookup_6(&reader, codepoint, out_glyph);
    case 12:
      return gfnt_cmap_lookup_12(&reader, codepoint, out_glyph, error);
    default:
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_CMAP_TAG,
          subtable->offset, GFNT_GLYPH_NONE,
          "a cmap subtable format this library does not read yet");
  }
}

GFNT_Result gfnt_face_glyph_for_codepoint(const GFNT_Face * face,
    uint32_t codepoint, uint32_t * out_glyph, GFNT_Error * error) {
  GFNT_CmapSubtable best;
  GFNT_Result result;

  if (!face || !out_glyph) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_CMAP_TAG, 0,
        GFNT_GLYPH_NONE, "no face, or nowhere to put the glyph");
  }
  // A standalone bitmap font has no `cmap` and still maps characters: PCF in its
  // BDF_ENCODINGS table, BDF in each character's ENCODING, PSF in its Unicode
  // table. So the question is answered here rather than left to fail as "this
  // font has no cmap", which would be true and useless.
  if (gfnt_face_is_bitmap(face)) {
    const GFNT_BitmapFont * font = NULL;

    result = gfnt_face_bitmap(face, &font, error);
    if (result != GFNT_OK) {
      return result;
    }
    result = gfnt_bitmap_glyph_for_codepoint(font, codepoint, out_glyph);
    if (result == GFNT_ERR_UNSUPPORTED) {
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, font->container, 0,
          GFNT_GLYPH_NONE,
          "a bitmap font that states no mapping from characters to glyphs - a "
          "PSF without its Unicode table indexes a console's cells, not "
          "characters");
    }
    if (result != GFNT_OK) {
      // Every cmap subtable answers an unmapped codepoint with glyph 0, and this
      // has to answer the same way: a caller looping over a string cannot be
      // asked to treat one container's misses as errors and another's as zero.
      *out_glyph = 0;
      gfnt_error_clear(error);
    }
    return GFNT_OK;
  }
  result = gfnt_face_cmap_best(face, &best, error);
  if (result != GFNT_OK) {
    return result;
  }

  result = gfnt_cmap_lookup(face, &best, codepoint, out_glyph, error);
  if (result != GFNT_OK) {
    return result;
  }

  // The Windows symbol encoding puts its glyphs in the private-use range
  // 0xF000..0xF0FF and expects a caller asking for 'A' to be answered from
  // 0xF041. A font with such a subtable and nothing better is a symbol font,
  // and this is how every other implementation reads one.
  if (*out_glyph == 0 && best.platform_id == GFNT_PLATFORM_WINDOWS
      && best.encoding_id == 0 && codepoint <= 0xFF) {
    return gfnt_cmap_lookup(face, &best, 0xF000u | codepoint, out_glyph, error);
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_cmap_dump(const GFNT_Face * face, FILE * out) {
  GFNT_CmapSubtable best;
  size_t count = 0;
  GFNT_Result result;

  if (!face || !out) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_face_cmap_count(face, &count, NULL);
  if (result != GFNT_OK) {
    return result;
  }
  if (fprintf(out, "cmap: %zu subtables\n", count) < 0) {
    return GFNT_ERR_IO;
  }

  for (size_t i = 0; i < count; ++i) {
    GFNT_CmapSubtable subtable;

    if (gfnt_cmap_record(face, i, &subtable, NULL) != GFNT_OK) {
      if (fprintf(out, "cmap subtable %zu: unreadable\n", i) < 0) {
        return GFNT_ERR_IO;
      }
      continue;
    }
    if (fprintf(out,
            "cmap subtable %zu: platform %u, encoding %u, format %u, "
            "offset %zu\n",
            i, subtable.platform_id, subtable.encoding_id, subtable.format,
            subtable.offset)
        < 0) {
      return GFNT_ERR_IO;
    }
  }

  if (gfnt_face_cmap_best(face, &best, NULL) == GFNT_OK) {
    if (fprintf(out, "cmap: using platform %u, encoding %u, format %u\n",
            best.platform_id, best.encoding_id, best.format)
        < 0) {
      return GFNT_ERR_IO;
    }
  }
  else if (fprintf(out, "cmap: no subtable this library reads\n") < 0) {
    return GFNT_ERR_IO;
  }
  return GFNT_OK;
}
