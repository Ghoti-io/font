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
 * PCF: X11's compiled bitmap font, which is a typed table of contents.
 *
 * documentation/design.md sections 7.1 and 7.5. This is the container the
 * multi-entry synthetic directory was written for: a PCF file is a header, a list
 * of `(type, format, size, offset)` entries, and then those extents - so each
 * becomes a directory entry, and every table parse below reads through a reader
 * that spans its own table and nothing else.
 *
 * **PCF carries its byte order in the file**, because it was compiled on the
 * machine that would serve it, and that is the thing about this format:
 *
 *  - The header's integers and every table's repeated `format` word are **always**
 *    least-significant-byte first. That is not a choice the file makes; it is how
 *    X writes those particular numbers.
 *  - Everything else in a table is in the order that table's own `format` says,
 *    so one file can hold tables in both orders and real ones do not, and a reader
 *    that assumed one would work on half the fonts in the world.
 *  - A bitmap's **bits** have an order of their own, its **bytes** another, its
 *    scan unit is one, two or four bytes and its rows are padded to one, two, four
 *    or eight. Those four are independent, and `bitmap.c` normalises them in one
 *    place for all four containers.
 *
 * One honest limitation, stated because a caller will otherwise assume otherwise:
 * the `BDF_ENCODINGS` table maps **positions in the font's own charset**, which
 * the XLFD name's `CHARSET_REGISTRY` and `CHARSET_ENCODING` properties name. For
 * an `ISO10646-1` font those positions are Unicode codepoints and the map this
 * library builds is a codepoint map; for an `ISO8859-5` font they are not, and
 * turning them into codepoints needs a charset table this library does not carry
 * (the same gap Type 1's `/Encoding` has without the Adobe Glyph List).
 *
 * Reference: *X11 Portable Compiled Format*, as documented in
 * `libXfont`'s `src/bitmap/pcfread.c` and `bdfToPcf`; the accelerator and metrics
 * layouts as `pcf.h` defines them.
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/macros.h>
#include <string.h>
#include "../sfnt/sfnt.h"
#include "bitmap.h"

/** PCF's magic: a byte, then `fcp`. Not a tag, which is why it is four bytes. */
#define GFNT_PCF_MAGIC0 0x01u
#define GFNT_PCF_MAGIC1 'f'
#define GFNT_PCF_MAGIC2 'c'
#define GFNT_PCF_MAGIC3 'p'

/** The table types PCF defines, as the table of contents states them. */
#define GFNT_PCF_PROPERTIES 0x00000001u
#define GFNT_PCF_ACCELERATORS 0x00000002u
#define GFNT_PCF_METRICS 0x00000004u
#define GFNT_PCF_BITMAPS 0x00000008u
#define GFNT_PCF_INK_METRICS 0x00000010u
#define GFNT_PCF_BDF_ENCODINGS 0x00000020u
#define GFNT_PCF_SWIDTHS 0x00000040u
#define GFNT_PCF_GLYPH_NAMES 0x00000080u
#define GFNT_PCF_BDF_ACCELERATORS 0x00000100u

/** The format word's low byte: how the bits of a bitmap are laid out. */
#define GFNT_PCF_GLYPH_PAD_MASK 0x03u
#define GFNT_PCF_BYTE_MASK 0x04u
#define GFNT_PCF_BIT_MASK 0x08u
#define GFNT_PCF_SCAN_UNIT_MASK 0x30u
/** The format word's high bytes: which of three shapes the table is in. */
#define GFNT_PCF_FORMAT_MASK 0xFFFFFF00u
#define GFNT_PCF_DEFAULT_FORMAT 0x00000000u
#define GFNT_PCF_INKBOUNDS 0x00000200u
#define GFNT_PCF_ACCEL_W_INKBOUNDS 0x00000100u
#define GFNT_PCF_COMPRESSED_METRICS 0x00000100u

/** Bytes of the file header before the table of contents. */
#define GFNT_PCF_HEADER 8u
/** Bytes of one table-of-contents entry: four little-endian 32-bit numbers. */
#define GFNT_PCF_TOC_ENTRY 16u
/** What a compressed metric subtracts from each of its five bytes. */
#define GFNT_PCF_METRIC_BIAS 0x80u

/** The tag for the header and table of contents themselves. */
#define GFNT_TAG_PCF_TOC GFNT_TAG('T', 'O', 'C', ' ')

/** One glyph's metrics, as either metrics table spells them. */
typedef struct GFNT_PcfMetric {
  int32_t left;    ///< Left side bearing.
  int32_t right;   ///< Right side bearing: the box's right edge from the pen.
  int32_t width;   ///< The advance.
  int32_t ascent;  ///< Rows above the baseline.
  int32_t descent; ///< Rows below it, positive in the file.
} GFNT_PcfMetric;

/** Which of PCF's tables this library reads, and the tag each one gets. */
static const struct { uint32_t type; GFNT_Tag tag; } gfnt_pcf_tags[] = {
  {GFNT_PCF_PROPERTIES, GFNT_TAG_PCF_PROPERTIES},
  {GFNT_PCF_ACCELERATORS, GFNT_TAG_PCF_ACCELERATORS},
  {GFNT_PCF_METRICS, GFNT_TAG_PCF_METRICS},
  {GFNT_PCF_BITMAPS, GFNT_TAG_PCF_BITMAPS},
  {GFNT_PCF_INK_METRICS, GFNT_TAG_PCF_INK_METRICS},
  {GFNT_PCF_BDF_ENCODINGS, GFNT_TAG_PCF_ENCODINGS},
  {GFNT_PCF_SWIDTHS, GFNT_TAG_PCF_SWIDTHS},
  {GFNT_PCF_GLYPH_NAMES, GFNT_TAG_PCF_GLYPH_NAMES},
  {GFNT_PCF_BDF_ACCELERATORS, GFNT_TAG_PCF_BDF_ACCELERATORS},
};

bool gfnt_pcf_looks_like(const GFNT_Reader * blob) {
  static const uint8_t magic[4] = {
    GFNT_PCF_MAGIC0, GFNT_PCF_MAGIC1, GFNT_PCF_MAGIC2, GFNT_PCF_MAGIC3
  };

  for (size_t i = 0; i < sizeof magic; ++i) {
    uint8_t byte = 0;

    if (gfnt_reader_u8_at(blob, i, &byte) != GFNT_OK || byte != magic[i]) {
      return false;
    }
  }
  return true;
}

/** The tag a PCF type gets, or 0 for a type this library does not read. */
static GFNT_Tag gfnt_pcf_tag(uint32_t type) {
  for (size_t i = 0; i < GFNT_ARRAY_SIZE(gfnt_pcf_tags); ++i) {
    if (gfnt_pcf_tags[i].type == type) {
      return gfnt_pcf_tags[i].tag;
    }
  }
  return 0;
}

GFNT_Result gfnt_pcf_directory(GFNT_Face * face, GFNT_Error * error) {
  GFNT_Reader reader;
  GFNT_SfntTable * entries;
  uint32_t claimed = 0;
  size_t count = 1;
  size_t toc_bytes;
  GFNT_Result result;

  result = gfnt_reader_init_blob(&reader, face->bytes, GFNT_FLAVOUR_PCF, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_reader_seek(&reader, 4) != GFNT_OK
      || gfnt_read_u32_order(&reader, GFNT_ORDER_LSB_FIRST, &claimed)
          != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_FLAVOUR_PCF, 4,
        GFNT_GLYPH_NONE, "a PCF header with no table count");
  }
  // The count bounds an array in the file, so the array has to be there before
  // the number means anything - the same guard a collection header gets, and for
  // the same reason.
  if (!gcu_safe_mul_size(claimed, GFNT_PCF_TOC_ENTRY, &toc_bytes)
      || !gfnt_reader_has(&reader, toc_bytes)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_FLAVOUR_PCF, 4,
        GFNT_GLYPH_NONE,
        "a PCF claiming more tables than its own file can hold");
  }
  // One entry for the header and the table of contents, then one per table this
  // library reads. The header gets one because the parse has to read the formats
  // again, and reading them through the reader is the rule (design.md section 6)
  // rather than a preference.
  if (claimed + 1u > face->limits.max_tables) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_FLAVOUR_PCF, 4,
        GFNT_GLYPH_NONE, "more tables than GFNT_Limits::max_tables");
  }
  entries = face->allocator->calloc_fn(face->allocator->ctx, claimed + 1u,
      sizeof *entries);
  if (!entries) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_FLAVOUR_PCF, 0,
        GFNT_GLYPH_NONE, "allocating a PCF's synthetic directory");
  }
  entries[0] = (GFNT_SfntTable) {
    .tag = GFNT_TAG_PCF_TOC,
    .offset = 0,
    .length = GFNT_PCF_HEADER + toc_bytes,
  };

  for (uint32_t i = 0; i < claimed; ++i) {
    uint32_t type = 0;
    uint32_t format = 0;
    uint32_t size = 0;
    uint32_t offset = 0;
    GFNT_Tag tag;

    if (gfnt_read_u32_order(&reader, GFNT_ORDER_LSB_FIRST, &type) != GFNT_OK
        || gfnt_read_u32_order(&reader, GFNT_ORDER_LSB_FIRST, &format)
            != GFNT_OK
        || gfnt_read_u32_order(&reader, GFNT_ORDER_LSB_FIRST, &size) != GFNT_OK
        || gfnt_read_u32_order(&reader, GFNT_ORDER_LSB_FIRST, &offset)
            != GFNT_OK) {
      face->allocator->free_fn(face->allocator->ctx, entries);
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_FLAVOUR_PCF,
          GFNT_PCF_HEADER + (size_t)i * GFNT_PCF_TOC_ENTRY, GFNT_GLYPH_NONE,
          "a PCF table-of-contents entry cut short");
    }
    tag = gfnt_pcf_tag(type);
    if (tag == 0) {
      // A type this library does not read gets no entry, and that is not an
      // error: PCF's types are a set X may add to, and an unknown one is a table
      // nothing here asks for. It is left out rather than given an invented tag,
      // so that gfnt_face_table_count() is the number of tables that can be read.
      continue;
    }
    entries[count] = (GFNT_SfntTable) {
      .tag = tag,
      .offset = offset,
      .length = size,
    };
    ++count;
  }

  result = gfnt_sfnt_table_directory(face, GFNT_FLAVOUR_PCF, entries, count,
      error);
  face->allocator->free_fn(face->allocator->ctx, entries);
  return result;
}

/**
 * Read one table's `format` word and check it against the table of contents.
 *
 * Every PCF table repeats the format the header gave it, always
 * least-significant-byte first. The two have to agree: the format decides how
 * every number after it is read, so a file whose two copies disagree is one where
 * a reader would have to choose, and choosing is how two readers draw one font
 * differently.
 *
 * @param face The face.
 * @param tag Which table.
 * @param reader Receives a reader over the table, positioned after the format.
 * @param out_format Receives the format word.
 * @param out_order Receives the byte order it names.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_UNSUPPORTED when the face has no such table, or
 *   ::GFNT_ERR_CORRUPT.
 */
static GFNT_Result gfnt_pcf_table(const GFNT_Face * face, GFNT_Tag tag,
    GFNT_Reader * reader, uint32_t * out_format, GFNT_ByteOrder * out_order,
    GFNT_Error * error) {
  GFNT_Reader toc;
  uint32_t format = 0;
  uint32_t claimed = 0;
  GFNT_Result result = gfnt_face_table_reader(face, tag, reader, error);

  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_u32_order(reader, GFNT_ORDER_LSB_FIRST, &format) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0, GFNT_GLYPH_NONE,
        "a PCF table too short to hold its own format word");
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_PCF_TOC, &toc, error);
  if (result != GFNT_OK) {
    return result;
  }
  for (size_t at = GFNT_PCF_HEADER; at + GFNT_PCF_TOC_ENTRY <= toc.length;
      at += GFNT_PCF_TOC_ENTRY) {
    uint32_t type = 0;

    if (gfnt_reader_u32_order_at(&toc, GFNT_ORDER_LSB_FIRST, at, &type)
            != GFNT_OK
        || gfnt_pcf_tag(type) != tag) {
      continue;
    }
    if (gfnt_reader_u32_order_at(&toc, GFNT_ORDER_LSB_FIRST, at + 4, &claimed)
        != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, at + 4,
          GFNT_GLYPH_NONE, "a PCF table-of-contents entry cut short");
    }
    if (claimed != format) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0, GFNT_GLYPH_NONE,
          "a PCF table whose format word disagrees with its directory entry");
    }
    break;
  }
  if ((format & GFNT_PCF_FORMAT_MASK) != GFNT_PCF_DEFAULT_FORMAT
      && (format & GFNT_PCF_FORMAT_MASK) != GFNT_PCF_INKBOUNDS
      && (format & GFNT_PCF_FORMAT_MASK) != GFNT_PCF_ACCEL_W_INKBOUNDS) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, 0, GFNT_GLYPH_NONE,
        "a PCF table in a format this library does not read");
  }
  *out_format = format;
  *out_order = (format & GFNT_PCF_BYTE_MASK) ? GFNT_ORDER_MSB_FIRST
      : GFNT_ORDER_LSB_FIRST;
  return GFNT_OK;
}

/** One uncompressed metric: six 16-bit numbers, of which five are read. */
static GFNT_Result gfnt_pcf_metric(GFNT_Reader * reader, GFNT_ByteOrder order,
    GFNT_PcfMetric * out) {
  int16_t left = 0;
  int16_t right = 0;
  int16_t width = 0;
  int16_t ascent = 0;
  int16_t descent = 0;
  uint16_t attributes = 0;

  if (gfnt_read_s16_order(reader, order, &left) != GFNT_OK
      || gfnt_read_s16_order(reader, order, &right) != GFNT_OK
      || gfnt_read_s16_order(reader, order, &width) != GFNT_OK
      || gfnt_read_s16_order(reader, order, &ascent) != GFNT_OK
      || gfnt_read_s16_order(reader, order, &descent) != GFNT_OK
      || gfnt_read_u16_order(reader, order, &attributes) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  // `attributes` is read and dropped: X puts a BDF font's own attribute word
  // here and no reader acts on it. Reading it is not optional, because it is what
  // makes a metric twelve bytes.
  (void)attributes;
  out->left = left;
  out->right = right;
  out->width = width;
  out->ascent = ascent;
  out->descent = descent;
  return GFNT_OK;
}

/** One compressed metric: five bytes, each biased by 0x80. */
static GFNT_Result gfnt_pcf_metric_compressed(GFNT_Reader * reader,
    GFNT_PcfMetric * out) {
  uint8_t bytes[5] = {0, 0, 0, 0, 0};

  for (size_t i = 0; i < sizeof bytes; ++i) {
    if (gfnt_read_u8(reader, &bytes[i]) != GFNT_OK) {
      return GFNT_ERR_CORRUPT;
    }
  }
  out->left = (int32_t)bytes[0] - GFNT_PCF_METRIC_BIAS;
  out->right = (int32_t)bytes[1] - GFNT_PCF_METRIC_BIAS;
  out->width = (int32_t)bytes[2] - GFNT_PCF_METRIC_BIAS;
  out->ascent = (int32_t)bytes[3] - GFNT_PCF_METRIC_BIAS;
  out->descent = (int32_t)bytes[4] - GFNT_PCF_METRIC_BIAS;
  return GFNT_OK;
}

/**
 * Read the metrics table: how many glyphs, and each one's box and advance.
 *
 * @param out_metrics Receives an array the caller frees, or NULL when the table
 *   is absent.
 * @param out_count Receives how many.
 */
static GFNT_Result gfnt_pcf_metrics(const GFNT_Face * face, GFNT_Tag tag,
    GFNT_PcfMetric ** out_metrics, size_t * out_count, GFNT_Error * error) {
  GFNT_Reader reader;
  GFNT_ByteOrder order = GFNT_ORDER_LSB_FIRST;
  GFNT_PcfMetric * metrics;
  uint32_t format = 0;
  size_t count;
  GFNT_Result result = gfnt_pcf_table(face, tag, &reader, &format, &order,
      error);

  if (result != GFNT_OK) {
    return result;
  }
  // Compressed metrics are a *different table shape*, not a different encoding of
  // the same one: the count is sixteen bits rather than thirty-two, so a reader
  // that only switched the per-entry decode would read the count from the wrong
  // two bytes and then be wrong about everything.
  if ((format & GFNT_PCF_FORMAT_MASK) == GFNT_PCF_COMPRESSED_METRICS) {
    uint16_t small = 0;

    if (gfnt_read_u16_order(&reader, order, &small) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 4, GFNT_GLYPH_NONE,
          "a compressed PCF metrics table with no count");
    }
    count = small;
  }
  else {
    uint32_t large = 0;

    if (gfnt_read_u32_order(&reader, order, &large) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 4, GFNT_GLYPH_NONE,
          "a PCF metrics table with no count");
    }
    count = large;
  }
  if (count > face->limits.max_glyphs) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, tag, 4, GFNT_GLYPH_NONE,
        "more glyphs than GFNT_Limits::max_glyphs");
  }
  if (count == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 4, GFNT_GLYPH_NONE,
        "a PCF metrics table with no glyphs in it");
  }
  metrics = face->allocator->calloc_fn(face->allocator->ctx, count,
      sizeof *metrics);
  if (!metrics) {
    return gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE,
        "allocating a PCF's metrics");
  }
  for (size_t i = 0; i < count; ++i) {
    GFNT_Result one;

    if ((format & GFNT_PCF_FORMAT_MASK) == GFNT_PCF_COMPRESSED_METRICS) {
      one = gfnt_pcf_metric_compressed(&reader, &metrics[i]);
    }
    else {
      one = gfnt_pcf_metric(&reader, order, &metrics[i]);
    }
    if (one != GFNT_OK) {
      face->allocator->free_fn(face->allocator->ctx, metrics);
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag,
          gfnt_reader_tell(&reader), (uint32_t)i,
          "a PCF metrics table that ends before the glyph it promised");
    }
  }
  *out_metrics = metrics;
  *out_count = count;
  return GFNT_OK;
}

/** Bytes one row of a @p width-pixel glyph takes, padded to @p pad bytes. */
static size_t gfnt_pcf_row_bytes(uint32_t width, unsigned pad) {
  size_t bytes = ((size_t)width + 7u) / 8u;

  return ((bytes + pad - 1u) / pad) * pad;
}

/**
 * The properties table, for the strings and the pixel size.
 *
 * Every property is a name, a flag saying whether its value is a string, and the
 * value - either an offset into the table's own string block or a number.
 */
static GFNT_Result gfnt_pcf_properties(const GFNT_Face * face,
    GFNT_BitmapBuild * build, int32_t * out_pixel_size, GFNT_Error * error) {
  GFNT_Reader reader;
  GFNT_ByteOrder order = GFNT_ORDER_LSB_FIRST;
  GFNT_BitmapFont * font = gfnt_bitmap_build_font(build);
  uint32_t format = 0;
  uint32_t count = 0;
  size_t strings_at;
  uint32_t strings_size = 0;
  size_t table_at;
  GFNT_Result result = gfnt_pcf_table(face, GFNT_TAG_PCF_PROPERTIES, &reader,
      &format, &order, error);

  if (result == GFNT_ERR_UNSUPPORTED) {
    return GFNT_OK;
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_u32_order(&reader, order, &count) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_PROPERTIES, 4,
        GFNT_GLYPH_NONE, "a PCF properties table with no count");
  }
  if (count > face->limits.max_name_records) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_PCF_PROPERTIES, 4,
        GFNT_GLYPH_NONE, "more properties than GFNT_Limits::max_name_records");
  }
  table_at = gfnt_reader_tell(&reader);
  // Nine bytes per property, then padding to a four-byte boundary, then the
  // string block's own length. The padding is the trap: it is computed from the
  // *count* rather than from the position, and a reader that rounded the offset
  // instead would be right for three counts in four.
  strings_at = table_at + (size_t)count * 9u;
  strings_at += (count % 4u) ? (4u - (count % 4u)) : 0u;
  if (gfnt_reader_seek(&reader, strings_at) != GFNT_OK
      || gfnt_read_u32_order(&reader, order, &strings_size) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_PROPERTIES,
        strings_at, GFNT_GLYPH_NONE,
        "a PCF properties table that ends before its strings");
  }
  strings_at += 4;

  for (uint32_t i = 0; i < count; ++i) {
    uint32_t name_offset = 0;
    uint8_t is_string = 0;
    uint32_t value = 0;
    const char * name;
    size_t name_length = 0;
    size_t * into = NULL;

    if (gfnt_reader_seek(&reader, table_at + (size_t)i * 9u) != GFNT_OK
        || gfnt_read_u32_order(&reader, order, &name_offset) != GFNT_OK
        || gfnt_read_u8(&reader, &is_string) != GFNT_OK
        || gfnt_read_u32_order(&reader, order, &value) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_PROPERTIES,
          table_at, GFNT_GLYPH_NONE, "a PCF property cut short");
    }
    if (name_offset >= strings_size) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_PROPERTIES,
          table_at + (size_t)i * 9u, GFNT_GLYPH_NONE,
          "a PCF property whose name is outside the table's strings");
    }
    {
      const uint8_t * bytes = NULL;
      size_t available = strings_size - name_offset;

      if (gfnt_reader_seek(&reader, strings_at + name_offset) != GFNT_OK
          || gfnt_read_bytes(&reader, available, &bytes) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_PROPERTIES,
            strings_at, GFNT_GLYPH_NONE,
            "a PCF properties table whose strings end early");
      }
      name = (const char *)bytes;
      while (name_length < available && bytes[name_length] != 0) {
        ++name_length;
      }
    }

    if (!is_string) {
      if (name_length == 10 && memcmp(name, "PIXEL_SIZE", 10) == 0) {
        *out_pixel_size = (int32_t)value;
      }
      else if (name_length == 12 && memcmp(name, "DEFAULT_CHAR", 12) == 0) {
        font->has_default_char = true;
        font->default_char = value;
      }
      continue;
    }
    if (name_length == 11 && memcmp(name, "FAMILY_NAME", 11) == 0) {
      into = &font->family;
    }
    else if (name_length == 9 && memcmp(name, "COPYRIGHT", 9) == 0) {
      into = &font->copyright;
    }
    else if (name_length == 11 && memcmp(name, "WEIGHT_NAME", 11) == 0) {
      into = &font->weight;
    }
    else if (name_length == 9 && memcmp(name, "FONT_NAME", 9) == 0) {
      into = &font->full_name;
    }
    if (!into) {
      continue;
    }
    if (value >= strings_size) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_PROPERTIES,
          table_at + (size_t)i * 9u, GFNT_GLYPH_NONE,
          "a PCF property whose value is outside the table's strings");
    }
    {
      const uint8_t * bytes = NULL;
      size_t available = strings_size - value;
      size_t length = 0;

      if (gfnt_reader_seek(&reader, strings_at + value) != GFNT_OK
          || gfnt_read_bytes(&reader, available, &bytes) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_PROPERTIES,
            strings_at, GFNT_GLYPH_NONE,
            "a PCF properties table whose strings end early");
      }
      while (length < available && bytes[length] != 0) {
        ++length;
      }
      result = gfnt_bitmap_build_string(build, (const char *)bytes, length,
          into, error);
      if (result != GFNT_OK) {
        return result;
      }
    }
  }
  return GFNT_OK;
}

/**
 * The accelerators, for the font's ascent and descent.
 *
 * A PCF may carry two of these: `BDF_ACCELERATORS`, which describes the glyphs
 * the font actually has, and `ACCELERATORS`, which may describe a larger set. The
 * BDF one is preferred where both exist, which is what X itself does.
 */
static GFNT_Result gfnt_pcf_accelerators(const GFNT_Face * face,
    int32_t * out_ascent, int32_t * out_descent, bool * out_found,
    GFNT_Error * error) {
  static const GFNT_Tag order_of_preference[2] = {
    GFNT_TAG_PCF_BDF_ACCELERATORS, GFNT_TAG_PCF_ACCELERATORS
  };

  for (size_t i = 0; i < GFNT_ARRAY_SIZE(order_of_preference); ++i) {
    GFNT_Reader reader;
    GFNT_ByteOrder order = GFNT_ORDER_LSB_FIRST;
    uint32_t format = 0;
    int32_t ascent = 0;
    int32_t descent = 0;
    GFNT_Result result = gfnt_pcf_table(face, order_of_preference[i], &reader,
        &format, &order, error);

    if (result == GFNT_ERR_UNSUPPORTED) {
      continue;
    }
    if (result != GFNT_OK) {
      return result;
    }
    // Eight one-byte flags, then the two numbers this library wants. The flags
    // are skipped rather than read: each is a promise about the glyphs that a
    // reader which measures them does not need.
    if (gfnt_reader_skip(&reader, 8) != GFNT_OK
        || gfnt_read_s32_order(&reader, order, &ascent) != GFNT_OK
        || gfnt_read_s32_order(&reader, order, &descent) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, order_of_preference[i], 4,
          GFNT_GLYPH_NONE, "a PCF accelerator table cut short");
    }
    *out_ascent = ascent;
    *out_descent = descent;
    *out_found = true;
    return GFNT_OK;
  }
  return GFNT_OK;
}

/** The encodings table, which maps the font's own charset to glyphs. */
static GFNT_Result gfnt_pcf_encodings(const GFNT_Face * face,
    GFNT_BitmapBuild * build, size_t glyph_count, GFNT_Error * error) {
  GFNT_Reader reader;
  GFNT_ByteOrder order = GFNT_ORDER_LSB_FIRST;
  uint32_t format = 0;
  uint16_t min_low = 0;
  uint16_t max_low = 0;
  uint16_t min_high = 0;
  uint16_t max_high = 0;
  uint16_t default_char = 0;
  GFNT_Result result = gfnt_pcf_table(face, GFNT_TAG_PCF_ENCODINGS, &reader,
      &format, &order, error);

  if (result == GFNT_ERR_UNSUPPORTED) {
    return GFNT_OK;
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_u16_order(&reader, order, &min_low) != GFNT_OK
      || gfnt_read_u16_order(&reader, order, &max_low) != GFNT_OK
      || gfnt_read_u16_order(&reader, order, &min_high) != GFNT_OK
      || gfnt_read_u16_order(&reader, order, &max_high) != GFNT_OK
      || gfnt_read_u16_order(&reader, order, &default_char) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_ENCODINGS, 4,
        GFNT_GLYPH_NONE, "a PCF encodings table with no ranges");
  }
  if (max_low < min_low || max_high < min_high) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_ENCODINGS, 4,
        GFNT_GLYPH_NONE, "a PCF encodings range that runs backwards");
  }
  gfnt_bitmap_build_font(build)->states_encoding = true;
  if (default_char != 0xFFFFu) {
    gfnt_bitmap_build_font(build)->has_default_char = true;
    gfnt_bitmap_build_font(build)->default_char = default_char;
  }

  for (uint32_t high = min_high; high <= max_high; ++high) {
    for (uint32_t low = min_low; low <= max_low; ++low) {
      uint16_t glyph = 0;
      uint32_t code;

      if (gfnt_read_u16_order(&reader, order, &glyph) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_ENCODINGS,
            gfnt_reader_tell(&reader), GFNT_GLYPH_NONE,
            "a PCF encodings table that ends before the ranges it declared");
      }
      if (glyph == 0xFFFFu) {
        continue;
      }
      if (glyph >= glyph_count) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_ENCODINGS,
            gfnt_reader_tell(&reader), glyph,
            "a PCF encoding naming a glyph the font does not have");
      }
      // A single-byte font states no high byte at all, and then the code is the
      // low byte: a font whose range is 0x20..0xFF maps 0x41 and not 0x0041 in a
      // two-byte sense, and the two happen to agree only because the high byte is
      // zero. A two-byte font's code is the pair.
      code = (min_high == 0 && max_high == 0) ? low : ((high << 8) | low);
      result = gfnt_bitmap_build_map(build, code, glyph, error);
      if (result != GFNT_OK) {
        return result;
      }
    }
  }
  return GFNT_OK;
}

/** The glyph-names table: one offset per glyph into its own string block. */
static GFNT_Result gfnt_pcf_glyph_names(const GFNT_Face * face,
    size_t glyph_count, GFNT_Reader * out_strings, uint32_t ** out_offsets,
    bool * out_found, GFNT_Error * error) {
  GFNT_Reader reader;
  GFNT_ByteOrder order = GFNT_ORDER_LSB_FIRST;
  uint32_t * offsets;
  uint32_t format = 0;
  uint32_t count = 0;
  uint32_t strings_size = 0;
  GFNT_Result result = gfnt_pcf_table(face, GFNT_TAG_PCF_GLYPH_NAMES, &reader,
      &format, &order, error);

  if (result == GFNT_ERR_UNSUPPORTED) {
    return GFNT_OK;
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_u32_order(&reader, order, &count) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_GLYPH_NAMES, 4,
        GFNT_GLYPH_NONE, "a PCF glyph-names table with no count");
  }
  if (count != glyph_count) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_GLYPH_NAMES, 4,
        GFNT_GLYPH_NONE,
        "a PCF naming a different number of glyphs than it has");
  }
  offsets = face->allocator->calloc_fn(face->allocator->ctx, count,
      sizeof *offsets);
  if (!offsets) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_PCF_GLYPH_NAMES, 0,
        GFNT_GLYPH_NONE, "allocating a PCF's glyph-name offsets");
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (gfnt_read_u32_order(&reader, order, &offsets[i]) != GFNT_OK) {
      face->allocator->free_fn(face->allocator->ctx, offsets);
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_GLYPH_NAMES,
          gfnt_reader_tell(&reader), i,
          "a PCF glyph-names table that ends before its offsets do");
    }
  }
  if (gfnt_read_u32_order(&reader, order, &strings_size) != GFNT_OK) {
    face->allocator->free_fn(face->allocator->ctx, offsets);
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_GLYPH_NAMES,
        gfnt_reader_tell(&reader), GFNT_GLYPH_NONE,
        "a PCF glyph-names table with no string block");
  }
  result = gfnt_reader_sub(&reader, gfnt_reader_tell(&reader), strings_size,
      out_strings);
  if (result != GFNT_OK) {
    face->allocator->free_fn(face->allocator->ctx, offsets);
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_GLYPH_NAMES,
        gfnt_reader_tell(&reader), GFNT_GLYPH_NONE,
        "a PCF glyph-names table whose strings run past its end");
  }
  *out_offsets = offsets;
  *out_found = true;
  return GFNT_OK;
}

GFNT_Result gfnt_pcf_parse(const GFNT_Face * face, void * out_font,
    GFNT_Error * error) {
  GFNT_BitmapFont * font = out_font;
  GFNT_BitmapBuild * build = NULL;
  GFNT_PcfMetric * metrics = NULL;
  uint32_t * name_offsets = NULL;
  GFNT_Reader names_strings;
  GFNT_Reader bitmaps;
  GFNT_ByteOrder bitmap_order = GFNT_ORDER_LSB_FIRST;
  GFNT_ByteOrder bit_order = GFNT_ORDER_LSB_FIRST;
  GFNT_Strike * strike;
  size_t glyph_count = 0;
  size_t data_at;
  uint32_t format = 0;
  uint32_t claimed_glyphs = 0;
  uint32_t data_size = 0;
  unsigned pad;
  unsigned scan_unit;
  bool have_names = false;
  bool have_accelerators = false;
  int32_t ascent = 0;
  int32_t descent = 0;
  int32_t pixel_size = 0;
  GFNT_Result result;

  result = gfnt_pcf_metrics(face, GFNT_TAG_PCF_METRICS, &metrics, &glyph_count,
      error);
  if (result != GFNT_OK) {
    // A PCF with no metrics table has no glyphs that can be measured, and every
    // one of its bitmaps would be a box of unknown size. That is missing rather
    // than unsupported, so the diagnostic says which table.
    return result;
  }

  result = gfnt_pcf_table(face, GFNT_TAG_PCF_BITMAPS, &bitmaps, &format,
      &bitmap_order, error);
  if (result != GFNT_OK) {
    face->allocator->free_fn(face->allocator->ctx, metrics);
    return result;
  }
  bit_order = (format & GFNT_PCF_BIT_MASK) ? GFNT_ORDER_MSB_FIRST
      : GFNT_ORDER_LSB_FIRST;
  pad = 1u << (format & GFNT_PCF_GLYPH_PAD_MASK);
  scan_unit = 1u << ((format & GFNT_PCF_SCAN_UNIT_MASK) >> 4);
  if (scan_unit > 4) {
    face->allocator->free_fn(face->allocator->ctx, metrics);
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_BITMAPS, 0,
        GFNT_GLYPH_NONE, "a PCF scan unit of eight bytes, which X never wrote");
  }
  if (gfnt_read_u32_order(&bitmaps, bitmap_order, &claimed_glyphs) != GFNT_OK) {
    face->allocator->free_fn(face->allocator->ctx, metrics);
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_BITMAPS, 4,
        GFNT_GLYPH_NONE, "a PCF bitmaps table with no count");
  }
  if (claimed_glyphs != glyph_count) {
    face->allocator->free_fn(face->allocator->ctx, metrics);
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_BITMAPS, 4,
        GFNT_GLYPH_NONE,
        "a PCF whose bitmaps and metrics disagree about how many glyphs it has");
  }

  result = gfnt_bitmap_build_start(&build, face, GFNT_FLAVOUR_PCF, glyph_count,
      error);
  if (result != GFNT_OK) {
    face->allocator->free_fn(face->allocator->ctx, metrics);
    return result;
  }
  result = gfnt_pcf_glyph_names(face, glyph_count, &names_strings,
      &name_offsets, &have_names, error);
  if (result != GFNT_OK) {
    goto done;
  }
  result = gfnt_pcf_properties(face, build, &pixel_size, error);
  if (result != GFNT_OK) {
    goto done;
  }
  result = gfnt_pcf_accelerators(face, &ascent, &descent, &have_accelerators,
      error);
  if (result != GFNT_OK) {
    goto done;
  }

  // The four sizes the table states are the *whole bitmap block's* length under
  // each of the four paddings, and only the one this font used is the truth about
  // this file. Reading the wrong one is how a reader accepts a file whose data is
  // a quarter of the length it needs.
  {
    size_t offsets_at = gfnt_reader_tell(&bitmaps);
    size_t sizes_at;

    if (!gcu_safe_mul_size(glyph_count, 4, &sizes_at)
        || !gcu_safe_add_size(sizes_at, offsets_at, &sizes_at)
        || gfnt_reader_seek(&bitmaps, sizes_at) != GFNT_OK) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_BITMAPS,
          offsets_at, GFNT_GLYPH_NONE,
          "a PCF bitmaps table that ends before its offsets do");
      goto done;
    }
    for (unsigned i = 0; i < 4; ++i) {
      uint32_t size = 0;

      if (gfnt_read_u32_order(&bitmaps, bitmap_order, &size) != GFNT_OK) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_BITMAPS,
            sizes_at, GFNT_GLYPH_NONE,
            "a PCF bitmaps table with fewer than its four block sizes");
        goto done;
      }
      if (i == (format & GFNT_PCF_GLYPH_PAD_MASK)) {
        data_size = size;
      }
    }
    data_at = gfnt_reader_tell(&bitmaps);
    if (!gfnt_reader_has(&bitmaps, data_size)) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_BITMAPS,
          data_at, GFNT_GLYPH_NONE,
          "a PCF bitmaps table shorter than the block size it states");
      goto done;
    }

    for (size_t glyph = 0; glyph < glyph_count; ++glyph) {
      const GFNT_PcfMetric * metric = &metrics[glyph];
      const uint8_t * rows = NULL;
      GFNT_BitmapRecord record;
      uint32_t offset = 0;
      size_t row_bytes;
      size_t needed;
      int32_t width = metric->right - metric->left;
      int32_t height = metric->ascent + metric->descent;
      const char * name = NULL;
      size_t name_length = 0;

      if (gfnt_reader_u32_order_at(&bitmaps, bitmap_order,
              offsets_at + glyph * 4u, &offset) != GFNT_OK) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_BITMAPS,
            offsets_at, (uint32_t)glyph, "a PCF bitmap offset that is not there");
        goto done;
      }
      if (width < 0 || height < 0) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_METRICS, 0,
            (uint32_t)glyph,
            "a PCF glyph whose side bearings or ascent run backwards");
        goto done;
      }
      row_bytes = gfnt_pcf_row_bytes((uint32_t)width, pad);
      if (!gcu_safe_mul_size(row_bytes, (size_t)height, &needed)) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_BITMAPS, 0,
            (uint32_t)glyph, "a PCF glyph whose rows do not fit a size_t");
        goto done;
      }
      if (needed > 0) {
        size_t end;

        if (!gcu_safe_add_size(offset, needed, &end) || end > data_size) {
          result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_BITMAPS,
              data_at + offset, (uint32_t)glyph,
              "a PCF glyph whose rows run past the bitmap block");
          goto done;
        }
        if (gfnt_reader_seek(&bitmaps, data_at + offset) != GFNT_OK
            || gfnt_read_bytes(&bitmaps, needed, &rows) != GFNT_OK) {
          result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PCF_BITMAPS,
              data_at + offset, (uint32_t)glyph,
              "a PCF glyph whose rows are not in the file");
          goto done;
        }
      }
      if (have_names) {
        const uint8_t * bytes = NULL;
        size_t available;

        if (name_offsets[glyph] >= names_strings.length) {
          result = gfnt_error_set(error, GFNT_ERR_CORRUPT,
              GFNT_TAG_PCF_GLYPH_NAMES, 0, (uint32_t)glyph,
              "a PCF glyph name outside the table's strings");
          goto done;
        }
        available = names_strings.length - name_offsets[glyph];
        if (gfnt_reader_seek(&names_strings, name_offsets[glyph]) != GFNT_OK
            || gfnt_read_bytes(&names_strings, available, &bytes) != GFNT_OK) {
          result = gfnt_error_set(error, GFNT_ERR_CORRUPT,
              GFNT_TAG_PCF_GLYPH_NAMES, 0, (uint32_t)glyph,
              "a PCF glyph name that is not in the file");
          goto done;
        }
        while (name_length < available && bytes[name_length] != 0) {
          ++name_length;
        }
        name = (const char *)bytes;
      }

      record = (GFNT_BitmapRecord) {
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .bearing_x = metric->left,
        // PCF states the ascent of the box, which *is* the top edge above the
        // baseline, so this one needs no adjustment - unlike BDF's BBX, which
        // states the bottom.
        .bearing_y = metric->ascent,
        .advance = metric->width,
      };
      result = gfnt_bitmap_build_glyph(build, rows, row_bytes, bit_order,
          bitmap_order, scan_unit, &record, name, name_length, error);
      if (result != GFNT_OK) {
        goto done;
      }
    }
  }

  result = gfnt_pcf_encodings(face, build, glyph_count, error);
  if (result != GFNT_OK) {
    goto done;
  }

  strike = gfnt_bitmap_build_strike(build);
  strike->index = 0;
  // The accelerators state the baseline. Without them the metrics do: the tallest
  // ascent and the deepest descent any glyph states is the line the font drew on,
  // which is what bdftopcf computes for the accelerator table in the first place.
  if (have_accelerators) {
    strike->ascent = ascent;
    strike->descent = -descent;
  }
  else {
    for (size_t glyph = 0; glyph < glyph_count; ++glyph) {
      if (metrics[glyph].ascent > strike->ascent) {
        strike->ascent = metrics[glyph].ascent;
      }
      if (-metrics[glyph].descent < strike->descent) {
        strike->descent = -metrics[glyph].descent;
      }
    }
  }
  if (pixel_size > 0) {
    strike->ppem_y = (uint32_t)pixel_size;
  }
  else {
    int32_t height = strike->ascent - strike->descent;

    strike->ppem_y = height > 0 ? (uint32_t)height : 1u;
  }
  strike->ppem_x = strike->ppem_y;
  // states_encoding is set by the encodings table itself, so a PCF that ships
  // without one - which is legal, and is what a font reached only by glyph index
  // looks like - says it states no characters rather than claiming an empty map.
  gfnt_bitmap_build_font(build)->states_baseline = true;

done:
  face->allocator->free_fn(face->allocator->ctx, metrics);
  face->allocator->free_fn(face->allocator->ctx, name_offsets);
  gfnt_bitmap_build_finish(build, font, result == GFNT_OK);
  return result;
}
