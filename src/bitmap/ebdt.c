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
 * **And a third kind: image formats 8 and 9 have no rows at all**, but a list of
 * other glyphs of the same strike, each OR-ed into the composite's own box at a
 * signed pixel offset. Those offsets are the *only* thing that positions a
 * component - not its own bearings - and y runs down from the top row.
 *
 * The specification does not say any of that. `EbdtComponent`'s fields are
 * documented as "Position of component left" and "Position of component top",
 * which names neither the space nor the direction nor what happens to a component
 * that does not fit, and all three change the pixels. What settles it is
 * FreeType's `src/sfnt/ttsbit.c`: `tt_sbit_decoder_load_compound()` recurses with
 * `x_pos + dx, y_pos + dy`, the loaders compute `line += y_pos * pitch +
 * (x_pos >> 3)` and `|=` into it, a component past the canvas is refused, and the
 * component's own bearings are saved and restored around the recursion so they
 * never place it. That is the de facto reference for this format and it is cited
 * here because no second reader of these pixels exists at all: nothing in Debian
 * has a composite, and fontTools parses the component list without composing an
 * image.
 *
 * One thing here is deliberately *more* than FreeType does: a cycle is walked for
 * and named. FreeType has only `recurse_count > 100`, which answers a depth limit
 * for a glyph that reaches itself - true, and useless to a caller who can act on a
 * limit by raising it.
 *
 * Reference: OpenType Specification 1.9, "EBDT - Embedded Bitmap Data Table";
 * FreeType 2.13.3, `src/sfnt/ttsbit.c`, for the composite placement the
 * specification leaves unstated.
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
    case 8:
      // SmallGlyphMetrics and then **a pad byte**, which is the whole difference
      // between formats 8 and 9's headers and the one thing a reader of this
      // format gets wrong: skip it and `numComponents` is read from the high half
      // of the count, so every component id is garbage and the glyph refuses for
      // a reason that says nothing about the real mistake.
      if (gfnt_read_u8(reader, &out->height) != GFNT_OK
          || gfnt_read_u8(reader, &out->width) != GFNT_OK
          || gfnt_read_s8(reader, &out->bearing_x) != GFNT_OK
          || gfnt_read_s8(reader, &out->bearing_y) != GFNT_OK
          || gfnt_read_u8(reader, &out->advance) != GFNT_OK
          || gfnt_reader_skip(reader, 1) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT,
            gfnt_reader_tell(reader), GFNT_GLYPH_NONE,
            "an EBDT composite too short for its metrics and pad byte");
      }
      *out_consumed = GFNT_EBDT_SMALL_METRICS_BYTES + 1u;
      return GFNT_OK;
    case 6:
    case 7:
    case 9:
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
 * The index format's whole job.
 *
 * **Two equal consecutive offsets mean the strike does not carry the glyph**, in
 * the three formats that store offsets: 1, 3 and 4. This library read them as a
 * glyph that is present and has no pixels, and that was wrong. Konatu.ttf is
 * where it showed: 13,249 of its 15,572 glyphs are zero-length in every one of
 * its fourteen strikes, so this library reported each strike as carrying 15,570
 * glyphs where fontTools and FreeType both say 2,323. Two independent references
 * agreeing against one reading is as close to settled as this format gets, and
 * the specification's wording for format 1 - the difference between consecutive
 * offsets is the data size, and there is no data when it is zero - is the third
 * voice. FreeType spells it as `image_start == image_end`, commented "missing
 * glyph", in the format 1 and format 3 arms of `tt_sbit_decoder_load_image`.
 *
 * It also makes the two ways this format can say "nothing here" agree. A sparse
 * format says it by leaving the glyph out of its list, and that already reported
 * absent; an offset format says it with a zero difference, and that reported
 * present. One predicate at the tail of this function now answers both, rather
 * than three callers each deciding what a zero length meant.
 *
 * **Formats 2 and 5 are deliberately not included.** Their length is a constant
 * the subtable states, so a zero there is not a per-glyph statement about one
 * glyph but a strike-wide one, and it is coherent with constant metrics of 0x0 -
 * every glyph empty. FreeType draws the same line, checking for equal offsets
 * only in the arms that read an offset array. A difference between the formats is
 * the thing being modelled, so the condition is per format rather than on the
 * length alone.
 *
 * @return ::GFNT_OK with @p out_found false when this subtable has no bitmap for
 *   @p glyph - either because a sparse list omits it, or because an offset array
 *   gives it no bytes.
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
  if (end == start && gfnt_eblc_index_has_offsets(subtable->index_format)) {
    // No bytes, from a format that states its sizes as differences: the strike
    // does not carry this glyph. See the contract above for why this is not
    // "present with no pixels", and why 2 and 5 are not here.
    *out_found = false;
    return GFNT_OK;
  }
  if (!gcu_safe_add_size(subtable->image_data_offset, start, out_offset)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, 0,
        GFNT_GLYPH_NONE, "an EBDT offset that does not fit a size_t");
  }
  *out_length = (size_t)(end - start);
  *out_found = true;
  return GFNT_OK;
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

/**
 * What a glyph read needs beyond its own bytes.
 *
 * Only the composites need any of it, and they need all of it: a component is
 * found through the strike's index exactly as a top-level glyph is, so painting
 * one means having the index, the table and the face that bounds them.
 */
typedef struct GFNT_EbdtRead {
  const GFNT_Face * face;
  const GFNT_EblcStrike * strike;
  const GFNT_EblcSubtable * subtables;
  size_t subtable_count;
  const GFNT_Reader * data;     ///< `EBDT` from its start, including the version.
  GFNT_BitmapBuild * build;
} GFNT_EbdtRead;

/**
 * The chain of glyphs currently being painted, innermost last.
 *
 * A linked list of stack frames rather than an array, because the depth a caller
 * allows is ::GFNT_Limits::max_composite_depth and a fixed array would either cap
 * that a second time or need allocating. Each level's node lives in its own
 * frame, so the walk costs nothing and there is nothing to free.
 *
 * It exists to **name a cycle rather than hit the depth cap with it**. The depth
 * budget alone would refuse `A -> B -> A` after sixteen levels and report a limit,
 * which is a true sentence about a font that is simply wrong - and the same thing
 * `glyf`'s composite reader does for an indirect cycle today, where it names only
 * the direct one. Here both are named.
 */
typedef struct GFNT_EbdtChain {
  const struct GFNT_EbdtChain * parent;
  uint32_t glyph;
} GFNT_EbdtChain;

/** Whether this glyph is already being painted, at any level. */
static bool gfnt_ebdt_chain_has(const GFNT_EbdtChain * chain, uint32_t glyph) {
  for (; chain; chain = chain->parent) {
    if (chain->glyph == glyph) {
      return true;
    }
  }
  return false;
}

/** Whether an image format's data is a list of other glyphs rather than rows. */
static bool gfnt_ebdt_is_composite(uint16_t image_format) {
  // 8 carries SmallGlyphMetrics and a pad byte, 9 BigGlyphMetrics and no pad;
  // past the metrics both are a uint16 count and four bytes per component.
  return image_format == 8 || image_format == 9;
}

/**
 * One leaf glyph's rows, byte-aligned and MSB-first whatever the format stored.
 *
 * Split out of ::gfnt_ebdt_read_glyph() because a composite's component needs
 * exactly this and then a blit, where a top-level glyph needs exactly this and
 * then the builder. Two copies of it would be two places for the
 * bit-aligned widening to be wrong.
 *
 * @param out_rows Receives them. **Borrowed**: from the blob for a byte-aligned
 *   format and from the builder's scratch row buffer for a bit-aligned one, so
 *   the pointer lasts only until the next call for another glyph. Every caller
 *   consumes it before asking again, and a caller that did not would see one
 *   component's pixels where another's belong.
 */
static GFNT_Result gfnt_ebdt_leaf_rows(GFNT_BitmapBuild * build,
    uint16_t image_format, GFNT_Reader * at, size_t offset,
    size_t rows_available, const GFNT_EblcMetrics * metrics,
    const uint8_t ** out_rows, size_t * out_stride, GFNT_Error * error) {
  size_t stride = ((size_t)metrics->width + 7u) / 8u;
  const uint8_t * rows = NULL;
  size_t wanted;

  if (!gcu_safe_mul_size(stride, metrics->height, &wanted)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
        GFNT_GLYPH_NONE, "an EBDT glyph whose rows do not fit a size_t");
  }
  *out_stride = stride;

  if (gfnt_ebdt_byte_aligned(image_format)) {
    if (wanted > rows_available
        || gfnt_read_bytes(at, wanted, &rows) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
          GFNT_GLYPH_NONE, "an EBDT glyph with fewer rows than its box");
    }
    *out_rows = rows;
    return GFNT_OK;
  }

  {
    uint8_t * widened = NULL;
    GFNT_Result result;

    if (gfnt_read_bytes(at, rows_available, &rows) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
          GFNT_GLYPH_NONE, "an EBDT glyph shorter than the length it states");
    }
    result = gfnt_bitmap_build_rows(build, wanted, &widened, error);
    if (result != GFNT_OK) {
      return result;
    }
    if (!gfnt_bitmap_widen_rows(widened, rows, rows_available, metrics->width,
            metrics->height)) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
          GFNT_GLYPH_NONE,
          "an EBDT bit-aligned glyph with fewer bits than its box");
    }
    *out_rows = widened;
    return GFNT_OK;
  }
}

/**
 * OR one component's pixels into a composite's canvas at (@p x, @p y).
 *
 * @p y counts **down from the top row**, which is the convention `EBDT`'s rows
 * are stored in and the one ::GFNT_BitmapGlyph hands out, so a component's
 * `yOffset` needs no sign change. The component's own bearings do not come into
 * it at all: the specification positions a component by these two offsets alone,
 * and `strike-composite.ttf` gives its three leaves three *different* bearings so
 * that a reader which used them would draw a different glyph.
 *
 * Bit by bit rather than byte by byte with a shift. The shifted form is where the
 * off-by-one in a blit lives, and this format has no population at all to make
 * the speed worth it - `uming.ttc` and `mona.ttf` between them have no composite.
 *
 * @return false when the component does not fit, which is a composite whose own
 *   box contradicts its components rather than something to clip.
 */
static bool gfnt_ebdt_blit(uint8_t * canvas, size_t canvas_stride,
    uint32_t canvas_width, uint32_t canvas_height, const uint8_t * rows,
    size_t row_stride, uint32_t width, uint32_t height, int32_t x, int32_t y) {
  if (x < 0 || y < 0
      || (int64_t)x + width > (int64_t)canvas_width
      || (int64_t)y + height > (int64_t)canvas_height) {
    return false;
  }
  for (uint32_t row = 0; row < height; ++row) {
    const uint8_t * source = rows + (size_t)row * row_stride;
    uint8_t * target = canvas + ((size_t)y + row) * canvas_stride;

    for (uint32_t column = 0; column < width; ++column) {
      if (source[column >> 3] & (uint8_t)(0x80u >> (column & 7u))) {
        size_t at = (size_t)x + column;

        target[at >> 3] |= (uint8_t)(0x80u >> (at & 7u));
      }
    }
  }
  return true;
}

/**
 * Paint @p glyph into @p canvas at (@p x, @p y), recursing for a composite.
 *
 * One canvas for the whole tree, which is how FreeType does it and the only way
 * that is correct: a nested composite contributes nothing but its components'
 * offsets added to its own, so there is no intermediate image to hold. Its own
 * metrics are read and discarded, exactly as `tt_sbit_decoder_load_compound()`
 * saves and restores the composite's.
 */
static GFNT_Result gfnt_ebdt_paint(const GFNT_EbdtRead * read, uint32_t glyph,
    int32_t x, int32_t y, uint8_t * canvas, size_t canvas_stride,
    uint32_t canvas_width, uint32_t canvas_height,
    const GFNT_EbdtChain * chain, size_t depth, GFNT_Error * error) {
  const GFNT_EblcSubtable * subtable;
  GFNT_EbdtChain here;
  GFNT_Reader at = *read->data;
  GFNT_EblcMetrics metrics;
  size_t offset = 0;
  size_t length = 0;
  size_t consumed = 0;
  bool found = false;
  uint16_t components = 0;
  GFNT_Result result;

  if (depth > read->face->limits.max_composite_depth) {
    // A limit rather than a corruption, and the distinction is the point: the
    // cycle check above has already refused every font that reaches itself, so
    // what is left here is a font nesting legitimately deeper than this caller
    // allowed. ::GFNT_ERR_LIMIT is what `gfnt_glyf_load()` answers for the same
    // question, and a caller can act on it by raising the budget where a
    // "corrupt" they cannot act on would be a lie.
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_EBDT, 0, glyph,
        "EBDT composite glyphs nest deeper than max_composite_depth allows");
  }
  if (glyph < read->strike->start_glyph || glyph > read->strike->end_glyph) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, 0, glyph,
        "an EBDT composite whose component is outside the strike's glyph range");
  }
  subtable = gfnt_ebdt_subtable_for(read->subtables, read->subtable_count,
      glyph);
  if (!subtable) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, 0, glyph,
        "an EBDT composite whose component no index subtable covers");
  }
  result = gfnt_ebdt_locate(read->face, subtable, glyph, &offset, &length,
      &found, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (!found) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, 0, glyph,
        "an EBDT composite whose component this strike does not carry");
  }
  if (length == 0) {
    // Reachable only for a constant-size subtable that states a size of zero: an
    // offset format with no bytes for the glyph now comes back as `!found` and is
    // refused above, which is what FreeType does to the same component. A
    // constant-size zero is a strike-wide statement that every glyph is empty, so
    // a component drawn from one contributes nothing and that is not an error.
    return GFNT_OK;
  }
  if (gfnt_reader_seek(&at, offset) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset, glyph,
        "an EBDT component that starts past the end of the table");
  }
  metrics = subtable->metrics;
  result = gfnt_ebdt_read_metrics(&at, subtable->image_format, &metrics,
      &consumed, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (consumed > length) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset, glyph,
        "an EBDT glyph whose stated length does not reach past its metrics");
  }

  if (!gfnt_ebdt_is_composite(subtable->image_format)) {
    const uint8_t * rows = NULL;
    size_t stride = 0;

    if (metrics.width == 0 || metrics.height == 0) {
      return GFNT_OK;
    }
    result = gfnt_ebdt_leaf_rows(read->build, subtable->image_format, &at,
        offset, length - consumed, &metrics, &rows, &stride, error);
    if (result != GFNT_OK) {
      return result;
    }
    if (!gfnt_ebdt_blit(canvas, canvas_stride, canvas_width, canvas_height,
            rows, stride, metrics.width, metrics.height, x, y)) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
          glyph, "an EBDT composite whose component falls outside its own box");
    }
    return GFNT_OK;
  }

  // A composite, nested or not. Its own metrics are already read and are not
  // used: what it contributes is its components, at its own offsets plus theirs.
  if (gfnt_read_u16(&at, &components) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset, glyph,
        "an EBDT composite with no component count");
  }
  if ((size_t)components * 4u + consumed + 2u > length) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset, glyph,
        "an EBDT composite claiming more components than its length holds");
  }

  here.parent = chain;
  here.glyph = glyph;
  for (uint16_t index = 0; index < components; ++index) {
    uint16_t component = 0;
    int8_t dx = 0;
    int8_t dy = 0;

    if (gfnt_read_u16(&at, &component) != GFNT_OK
        || gfnt_read_s8(&at, &dx) != GFNT_OK
        || gfnt_read_s8(&at, &dy) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
          glyph, "an EBDT composite component past the end of the table");
    }
    if (gfnt_ebdt_chain_has(&here, component)) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
          glyph,
          "an EBDT composite that includes itself, directly or through another "
          "glyph");
    }
    result = gfnt_ebdt_paint(read, component, x + dx, y + dy, canvas,
        canvas_stride, canvas_width, canvas_height, &here, depth + 1, error);
    if (result != GFNT_OK) {
      return result;
    }
  }
  return GFNT_OK;
}

/**
 * Read one glyph's metrics and pixels, and hand them to the builder.
 *
 * Three shapes, and the dispatch between them is the image format's: rows that
 * start on byte boundaries, rows that start at the next bit, and - formats 8 and
 * 9 - no rows at all but a list of other glyphs to draw.
 */
static GFNT_Result gfnt_ebdt_read_glyph(const GFNT_EbdtRead * read,
    const GFNT_EblcSubtable * subtable, uint32_t glyph, size_t offset,
    size_t length, GFNT_Error * error) {
  GFNT_Reader at = *read->data;
  GFNT_EblcMetrics metrics = subtable->metrics;
  GFNT_BitmapBuild * build = read->build;
  GFNT_BitmapRecord record;
  size_t consumed = 0;
  size_t rows_available;
  size_t stride;
  const uint8_t * rows = NULL;
  GFNT_Result result;

  if (length == 0) {
    // Reachable only for a constant-size subtable stating a size of zero; an
    // offset format's zero-length glyph is absent and never arrives here. The
    // advance is still the index's business, because the index is where a
    // constant-metrics subtable states one.
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

  if (gfnt_ebdt_is_composite(subtable->image_format)) {
    // The composite's **own** metrics give the canvas, which is what makes a
    // component falling outside it a contradiction rather than something to clip:
    // the font states the box and then states what goes in it.
    uint8_t * canvas = NULL;
    size_t wanted;

    stride = ((size_t)metrics.width + 7u) / 8u;
    if (!gcu_safe_mul_size(stride, metrics.height, &wanted)) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, offset,
          glyph, "an EBDT composite whose box does not fit a size_t");
    }
    result = gfnt_bitmap_build_canvas(build, wanted, &canvas, error);
    if (result != GFNT_OK) {
      return result;
    }
    // Painted from the glyph itself rather than from its component list, so that
    // a top-level composite and a nested one take the same path - and so that the
    // chain the cycle check walks starts where the recursion does.
    result = gfnt_ebdt_paint(read, glyph, 0, 0, canvas, stride, metrics.width,
        metrics.height, NULL, 0, error);
    if (result != GFNT_OK) {
      return result;
    }
    return gfnt_bitmap_build_glyph(build, canvas, stride, GFNT_ORDER_MSB_FIRST,
        GFNT_ORDER_MSB_FIRST, 1, &record, NULL, 0, error);
  }

  result = gfnt_ebdt_leaf_rows(build, subtable->image_format, &at, offset,
      rows_available, &metrics, &rows, &stride, error);
  if (result != GFNT_OK) {
    return result;
  }
  // MSB-first, no scan unit, rows on byte boundaries by the time they get here:
  // the builder's identity case, which is why nothing in this file reimplements
  // the normalisation.
  return gfnt_bitmap_build_glyph(build, rows, stride, GFNT_ORDER_MSB_FIRST,
      GFNT_ORDER_MSB_FIRST, 1, &record, NULL, 0, error);
}

GFNT_Result gfnt_ebdt_read_strike(const GFNT_Face * face,
    const GFNT_EblcStrike * strike, const GFNT_EblcSubtable * subtables,
    size_t subtable_count, GFNT_BitmapBuild * build, GFNT_Error * error) {
  GFNT_Reader data;
  GFNT_EbdtRead read;
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

  // Everything a composite needs to find its components, gathered once. A leaf
  // glyph uses only `data` and `build` of it, which is why this is one struct
  // rather than six more parameters on a function most glyphs take the short way
  // through.
  read.face = face;
  read.strike = strike;
  read.subtables = subtables;
  read.subtable_count = subtable_count;
  read.data = &data;
  read.build = build;

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
    result = gfnt_ebdt_read_glyph(&read, subtable, (uint32_t)glyph, offset,
        length, &glyph_error);
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
