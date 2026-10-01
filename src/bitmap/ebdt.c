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
 * `EBDT`: the pixels an `EBLC` strike points at.
 *
 * documentation/design.md section 7.5. `eblc.c` reads which glyphs a strike
 * covers and in what shape; this reads the glyphs.
 *
 * **Two axes, and they are independent.** The *index* format says how to find a
 * glyph's bytes - four-byte offsets, a constant size, two-byte offsets, or a
 * sparse list of glyph ids - and the *image* format says what those bytes are:
 * which metrics precede the rows, and whether the rows are byte-aligned or
 * bit-aligned. A reader that conflated them would work on the one pairing that is
 * 97.8% of the real population and fail on the rest.
 *
 * The bit-aligned formats are widened to byte-aligned rows by
 * ::gfnt_bitmap_widen_rows() and then go through the same
 * ::gfnt_bitmap_build_glyph() every other container uses - and the byte-aligned
 * ones go straight there, because `EBDT` is MSB-first with no scan unit, which is
 * exactly that function's identity case.
 *
 * Reference: OpenType Specification 1.9, "EBDT - Embedded Bitmap Data Table".
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/macros.h>
#include <string.h>
#include "../tables/tables.h"
#include "eblc.h"

/** Bytes in a `SmallGlyphMetrics`. */
#define GFNT_EBDT_SMALL_METRICS_BYTES 5u

/** The one `EBDT` version the specification defines. */
#define GFNT_EBDT_VERSION 0x00020000u

/** Whether an image format's rows start each on a byte boundary. */
static bool gfnt_ebdt_byte_aligned(uint16_t image_format) {
  // 1 and 6 are byte-aligned; 2, 5 and 7 are bit-aligned. This single bit is the
  // only difference between image formats 6 and 7, and between 1 and 2.
  return image_format == 1 || image_format == 6;
}

/**
 * Read the metrics an image format puts in front of its rows.
 *
 * Format 5 has none - its metrics are the index subtable's - and the caller has
 * already checked that it is indexed by a format that states them.
 *
 * @param out_consumed How many bytes the metrics took, so the caller knows where
 *   the rows start.
 */
static GFNT_Result gfnt_ebdt_read_metrics(GFNT_Reader * reader,
    uint16_t image_format, GFNT_EblcMetrics * out, size_t * out_consumed,
    GFNT_Error * error) {
  switch (image_format) {
    case 1:
    case 2:
      // SmallGlyphMetrics: the horizontal five and nothing else.
      if (gfnt_read_u8(reader, &out->height) != GFNT_OK
          || gfnt_read_u8(reader, &out->width) != GFNT_OK
          || gfnt_read_s8(reader, &out->bearing_x) != GFNT_OK
          || gfnt_read_s8(reader, &out->bearing_y) != GFNT_OK
          || gfnt_read_u8(reader, &out->advance) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT,
            gfnt_reader_tell(reader), GFNT_GLYPH_NONE,
            "an EBDT glyph too short for its SmallGlyphMetrics");
      }
      *out_consumed = GFNT_EBDT_SMALL_METRICS_BYTES;
      return GFNT_OK;
    case 6:
    case 7:
      if (gfnt_read_u8(reader, &out->height) != GFNT_OK
          || gfnt_read_u8(reader, &out->width) != GFNT_OK
          || gfnt_read_s8(reader, &out->bearing_x) != GFNT_OK
          || gfnt_read_s8(reader, &out->bearing_y) != GFNT_OK
          || gfnt_read_u8(reader, &out->advance) != GFNT_OK
          || gfnt_reader_skip(reader, 3) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT,
            gfnt_reader_tell(reader), GFNT_GLYPH_NONE,
            "an EBDT glyph too short for its BigGlyphMetrics");
      }
      *out_consumed = 8u;
      return GFNT_OK;
    default:
      // Format 5, the only one left: its metrics are the index's, and the caller
      // supplied them.
      *out_consumed = 0;
      return GFNT_OK;
  }
}

/**
 * Where one glyph's data is, and how long it is.
 *
 * The index format's whole job. A length of zero means the strike lists the glyph
 * and carries no bitmap for it, which the specification states for formats 1 and 3
 * - consecutive equal offsets - and which is a glyph with no pixels rather than a
 * glyph the strike does not have.
 *
 * @return ::GFNT_OK with @p out_found false when this subtable does not cover
 *   @p glyph at all, which only the sparse formats can say.
 */
static GFNT_Result gfnt_ebdt_locate(const GFNT_Face * face,
    const GFNT_EblcSubtable * subtable, uint32_t glyph, size_t * out_offset,
    size_t * out_length, bool * out_found, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_Result result;
  size_t position = (size_t)(glyph - subtable->first_glyph);
  uint32_t start = 0;
  uint32_t end = 0;

  *out_found = false;
  result = gfnt_face_table_reader(face, GFNT_TAG_EBLC, &table, error);
  if (result != GFNT_OK) {
    return result;
  }

  switch (subtable->index_format) {
    case 1: {
      // Offset32 sbitOffsets[last - first + 2]: glyph n is the bytes between
      // entry n and entry n+1, so every entry but the last belongs to two glyphs.
      size_t at = subtable->body_offset + position * 4u;

      if (gfnt_reader_u32_at(&table, at, &start) != GFNT_OK
          || gfnt_reader_u32_at(&table, at + 4u, &end) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, at,
            GFNT_GLYPH_NONE, "an EBLC format 1 offset past the end of the table");
      }
      break;
    }
    case 2: {
      // One size for every glyph, so the offset is arithmetic and there is no
      // array to read at all.
      size_t scaled;

      if (!gcu_safe_mul_size(position, subtable->image_size, &scaled)
          || scaled > UINT32_MAX) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, 0,
            GFNT_GLYPH_NONE, "an EBLC format 2 glyph offset that does not fit");
      }
      start = (uint32_t)scaled;
      end = start + subtable->image_size;
      break;
    }
    case 3: {
      // Offset16 rather than Offset32, and otherwise format 1. The specification
      // calls it obsolete and fontTools writes it for small strikes; nothing in
      // the population uses it, so this arm's only evidence is a fixture.
      size_t at = subtable->body_offset + position * 2u;
      uint16_t low = 0;
      uint16_t high = 0;

      if (gfnt_reader_u16_at(&table, at, &low) != GFNT_OK
          || gfnt_reader_u16_at(&table, at + 2u, &high) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, at,
            GFNT_GLYPH_NONE, "an EBLC format 3 offset past the end of the table");
      }
      start = low;
      end = high;
      break;
    }
    case 4: {
      // A sorted list of (glyphID, Offset16) pairs with a sentinel, so a glyph in
      // the subtable's *range* may still not be in the subtable. Bisected rather
      // than scanned: a strike of 27,000 glyphs in one sparse subtable is a
      // shape the population has.
      size_t low = 0;
      size_t high = subtable->sparse_count;

      while (low < high) {
        size_t middle = low + (high - low) / 2u;
        size_t at = subtable->body_offset + 4u + middle * 4u;
        uint16_t id = 0;

        if (gfnt_reader_u16_at(&table, at, &id) != GFNT_OK) {
          return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, at,
              GFNT_GLYPH_NONE,
              "an EBLC format 4 glyph list past the end of the table");
        }
        if (id < glyph) {
          low = middle + 1u;
        }
        else if (id > glyph) {
          high = middle;
        }
        else {
          uint16_t here = 0;
          uint16_t next = 0;

          if (gfnt_reader_u16_at(&table, at + 2u, &here) != GFNT_OK
              || gfnt_reader_u16_at(&table, at + 6u, &next) != GFNT_OK) {
            return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, at,
                GFNT_GLYPH_NONE,
                "an EBLC format 4 pair whose offset or successor is missing");
          }
          start = here;
          end = next;
          low = high = middle;
          *out_found = true;
          break;
        }
      }
      if (!*out_found) {
        // In range and not in the list. The one answer a non-sparse format cannot
        // give, and the reason `present` is a field.
        return GFNT_OK;
      }
      break;
    }
    default: {
      // Format 5: constant metrics *and* a sparse glyph list, so the position in
      // the list is what multiplies the size - not the glyph's distance from
      // firstGlyphIndex, which is the mistake this arm exists to not make.
      size_t low = 0;
      size_t high = subtable->sparse_count;

      while (low < high) {
        size_t middle = low + (high - low) / 2u;
        size_t at = subtable->body_offset + 4u + GFNT_EBLC_METRICS_BYTES
            + 4u + middle * 2u;
        uint16_t id = 0;

        if (gfnt_reader_u16_at(&table, at, &id) != GFNT_OK) {
          return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, at,
              GFNT_GLYPH_NONE,
              "an EBLC format 5 glyph list past the end of the table");
        }
        if (id < glyph) {
          low = middle + 1u;
        }
        else if (id > glyph) {
          high = middle;
        }
        else {
          size_t scaled;

          if (!gcu_safe_mul_size(middle, subtable->image_size, &scaled)
              || scaled > UINT32_MAX) {
            return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, at,
                GFNT_GLYPH_NONE,
                "an EBLC format 5 glyph offset that does not fit");
          }
          start = (uint32_t)scaled;
          end = start + subtable->image_size;
          *out_found = true;
          break;
        }
      }
      if (!*out_found) {
        return GFNT_OK;
      }
      break;
    }
  }

  if (end < start) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, 0,
        GFNT_GLYPH_NONE, "an EBLC glyph whose offsets run backwards");
  }
  if (!gcu_safe_add_size(subtable->image_data_offset, start, out_offset)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, 0,
        GFNT_GLYPH_NONE, "an EBDT offset that does not fit a size_t");
  }
  *out_length = (size_t)(end - start);
  *out_found = true;
  return GFNT_OK;
}

/** Read one glyph's metrics and rows, and hand them to the builder. */
static GFNT_Result gfnt_ebdt_read_glyph(const GFNT_EblcSubtable * subtable,
    const GFNT_Reader * data, size_t offset, size_t length,
    GFNT_BitmapBuild * build, GFNT_Error * error) {
  GFNT_Reader at = *data;
  GFNT_EblcMetrics metrics = subtable->metrics;
  GFNT_BitmapRecord record;
  size_t consumed = 0;
  size_t rows_available;
  size_t wanted;
  const uint8_t * rows = NULL;
  GFNT_Result result;

  if (length == 0) {
    // The strike lists the glyph and carries no bitmap for it. A space, and its
    // advance is still the index's business where the index states one.
    memset(&record, 0, sizeof record);
    record.advance = metrics.advance;
    return gfnt_bitmap_build_glyph(build, NULL, 0, GFNT_ORDER_MSB_FIRST,
        GFNT_ORDER_MSB_FIRST, 1, &record, NULL, 0, error);
  }

  if (gfnt_reader_seek(&at, offset) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
        GFNT_GLYPH_NONE, "an EBDT glyph that starts past the end of the table");
  }
  result = gfnt_ebdt_read_metrics(&at, subtable->image_format, &metrics,
      &consumed, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (consumed > length) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
        GFNT_GLYPH_NONE,
        "an EBDT glyph whose stated length does not reach past its metrics");
  }
  rows_available = length - consumed;

  memset(&record, 0, sizeof record);
  record.width = metrics.width;
  record.height = metrics.height;
  record.bearing_x = metrics.bearing_x;
  record.bearing_y = metrics.bearing_y;
  record.advance = metrics.advance;

  if (metrics.width == 0 || metrics.height == 0) {
    return gfnt_bitmap_build_glyph(build, NULL, 0, GFNT_ORDER_MSB_FIRST,
        GFNT_ORDER_MSB_FIRST, 1, &record, NULL, 0, error);
  }

  if (gfnt_ebdt_byte_aligned(subtable->image_format)) {
    size_t stride = ((size_t)metrics.width + 7u) / 8u;

    if (!gcu_safe_mul_size(stride, metrics.height, &wanted)) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
          GFNT_GLYPH_NONE, "an EBDT glyph whose rows do not fit a size_t");
    }
    if (wanted > rows_available
        || gfnt_read_bytes(&at, wanted, &rows) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
          GFNT_GLYPH_NONE, "an EBDT glyph with fewer rows than its box");
    }
    // MSB-first, no scan unit, rows already on byte boundaries: the builder's
    // identity case, which is why nothing here reimplements the normalisation.
    return gfnt_bitmap_build_glyph(build, rows, stride, GFNT_ORDER_MSB_FIRST,
        GFNT_ORDER_MSB_FIRST, 1, &record, NULL, 0, error);
  }

  // Bit-aligned: widen into the builder's own scratch row buffer, which is sized
  // for a whole glyph by gfnt_bitmap_build_rows().
  {
    uint8_t * widened = NULL;
    size_t stride = ((size_t)metrics.width + 7u) / 8u;

    if (!gcu_safe_mul_size(stride, metrics.height, &wanted)) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
          GFNT_GLYPH_NONE, "an EBDT glyph whose rows do not fit a size_t");
    }
    if (gfnt_read_bytes(&at, rows_available, &rows) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
          GFNT_GLYPH_NONE, "an EBDT glyph shorter than the length it states");
    }
    result = gfnt_bitmap_build_rows(build, wanted, &widened, error);
    if (result != GFNT_OK) {
      return result;
    }
    if (!gfnt_bitmap_widen_rows(widened, rows, rows_available, metrics.width,
            metrics.height)) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
          GFNT_GLYPH_NONE,
          "an EBDT bit-aligned glyph with fewer bits than its box");
    }
    return gfnt_bitmap_build_glyph(build, widened, stride,
        GFNT_ORDER_MSB_FIRST, GFNT_ORDER_MSB_FIRST, 1, &record, NULL, 0, error);
  }
}

/** The subtable covering @p glyph, or NULL. */
static const GFNT_EblcSubtable * gfnt_ebdt_subtable_for(
    const GFNT_EblcSubtable * subtables, size_t count, uint32_t glyph) {
  for (size_t i = 0; i < count; ++i) {
    if (glyph >= subtables[i].first_glyph && glyph <= subtables[i].last_glyph) {
      return &subtables[i];
    }
  }
  return NULL;
}

GFNT_Result gfnt_ebdt_read_strike(const GFNT_Face * face,
    const GFNT_EblcStrike * strike, const GFNT_EblcSubtable * subtables,
    size_t subtable_count, GFNT_BitmapBuild * build, GFNT_Error * error) {
  GFNT_Reader data;
  GFNT_Result result;
  GFNT_Error glyph_error;
  uint32_t version = 0;
  size_t glyphs = 0;
  size_t corrupt = 0;

  gfnt_error_clear(&glyph_error);

  result = gfnt_face_table_reader(face, GFNT_TAG_EBDT, &data, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_u32(&data, &version) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, 0,
        GFNT_GLYPH_NONE, "an EBDT shorter than its own header");
  }
  if (version != GFNT_EBDT_VERSION) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_EBDT, 0,
        GFNT_GLYPH_NONE, "an EBDT version this library does not read");
  }

  result = gfnt_face_num_glyphs(face, &glyphs, error);
  if (result != GFNT_OK) {
    return result;
  }

  // Every glyph of the face gets a record, present or not, so that a lookup is an
  // index rather than a search - the strike is sparse over the face and the
  // records are the face's glyph ids.
  for (size_t glyph = 0; glyph < glyphs; ++glyph) {
    const GFNT_EblcSubtable * subtable;
    size_t offset = 0;
    size_t length = 0;
    bool found = false;

    if (glyph > UINT16_MAX || glyph < strike->start_glyph
        || glyph > strike->end_glyph) {
      // Outside what the strike itself claims. `startGlyphIndex` and
      // `endGlyphIndex` are the strike's own bounds and the subtables sit inside
      // them; a glyph past 0xFFFF cannot be in either, because every glyph id in
      // this table is a uint16.
      result = gfnt_bitmap_build_absent(build, error);
      if (result != GFNT_OK) {
        return result;
      }
      continue;
    }
    subtable = gfnt_ebdt_subtable_for(subtables, subtable_count,
        (uint32_t)glyph);
    if (!subtable) {
      result = gfnt_bitmap_build_absent(build, error);
      if (result != GFNT_OK) {
        return result;
      }
      continue;
    }
    // M11 from here down: a glyph whose index entry or whose data contradicts
    // itself is refused *as that glyph*, and the strike keeps answering for the
    // rest. The first draft returned the failure and condemned the strike, which
    // made one bad offset in a 27,000-glyph strike lose all 27,000 - and which a
    // test caught only because it also made a *correct* glyph of the same strike
    // unreadable.
    //
    // ::GFNT_ERR_OOM is the exception and is returned: running out of memory is
    // not a fact about the font, and a parse that swallowed it would publish a
    // strike that silently lost glyphs.
    result = gfnt_ebdt_locate(face, subtable, (uint32_t)glyph, &offset, &length,
        &found, &glyph_error);
    if (result == GFNT_ERR_OOM) {
      *error = glyph_error;
      return result;
    }
    if (result != GFNT_OK) {
      ++corrupt;
      result = gfnt_bitmap_build_corrupt(build, glyph_error.message, error);
      if (result != GFNT_OK) {
        return result;
      }
      continue;
    }
    if (!found) {
      result = gfnt_bitmap_build_absent(build, error);
      if (result != GFNT_OK) {
        return result;
      }
      continue;
    }
    result = gfnt_ebdt_read_glyph(subtable, &data, offset, length, build,
        &glyph_error);
    if (result == GFNT_ERR_OOM || result == GFNT_ERR_LIMIT) {
      // A limit is the caller's ceiling rather than the font's mistake, so it is
      // reported rather than recorded against one glyph.
      *error = glyph_error;
      return result;
    }
    if (result != GFNT_OK) {
      ++corrupt;
      result = gfnt_bitmap_build_corrupt(build, glyph_error.message, error);
      if (result != GFNT_OK) {
        return result;
      }
    }
  }
  if (corrupt > 0 && error) {
    // The last glyph-level refusal, kept so that a caller parsing a strike and
    // getting OK can still find out that something in it was wrong. The strike is
    // usable; this says how much of it is not.
    *error = glyph_error;
  }
  return GFNT_OK;
}
