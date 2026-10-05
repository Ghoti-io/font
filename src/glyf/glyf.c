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
 * `loca` and `glyf`: simple glyphs, composite glyphs, and every composite flag.
 *
 * documentation/design.md section 7.3. The flag stream with `REPEAT` and the
 * same-or-positive bits, contours that begin off-curve (resolved in
 * `outline.c`, not here), and the composite transforms including the
 * Apple/Microsoft scaled-offset difference.
 *
 * Instructions are skipped by their stated length and never run: there is no
 * interpreter and design.md section 8.5 says why.
 */

#include <ghoti.io/font/macros.h>
#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "../core/chain.h"
#include "../core/fixed.h"
#include "../outline/outline.h"
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "../var/gvar.h"
#include "glyf.h"

/** The tag of the table holding the glyph descriptions. */
#define GFNT_TAG_GLYF GFNT_TAG('g', 'l', 'y', 'f')
/** The tag of the table holding their offsets. */
#define GFNT_TAG_LOCA GFNT_TAG('l', 'o', 'c', 'a')

/** Bytes before `endPtsOfContours`: numberOfContours and the four bounds. */
#define GFNT_GLYF_HEADER_BYTES 10u

// Simple glyph flags, OpenType 1.9 table "Simple Glyph Flags".
#define GFNT_GLYF_ON_CURVE 0x01u
#define GFNT_GLYF_X_SHORT 0x02u
#define GFNT_GLYF_Y_SHORT 0x04u
#define GFNT_GLYF_REPEAT 0x08u
#define GFNT_GLYF_SAME_X 0x10u
#define GFNT_GLYF_SAME_Y 0x20u
#define GFNT_GLYF_OVERLAP_SIMPLE 0x40u
/**
 * Flag bit 0x80: reserved in OpenType 1.9, and the cubic control-point flag in
 * the proposed `glyf` version 1. A reader that ignores it draws a cubic control
 * point as a quadratic one, which is a different shape and no error at all, so
 * it is refused by name. `head.glyphDataFormat` is the other half of the same
 * refusal and is checked where the table is opened rather than per glyph.
 */
#define GFNT_GLYF_CUBIC 0x80u

// Composite glyph flags, OpenType 1.9 table "Component Glyph Flags".
#define GFNT_GLYF_ARGS_ARE_WORDS 0x0001u
#define GFNT_GLYF_ARGS_ARE_XY 0x0002u
#define GFNT_GLYF_ROUND_XY_TO_GRID 0x0004u
#define GFNT_GLYF_HAVE_SCALE 0x0008u
#define GFNT_GLYF_MORE_COMPONENTS 0x0020u
#define GFNT_GLYF_HAVE_XY_SCALE 0x0040u
#define GFNT_GLYF_HAVE_TWO_BY_TWO 0x0080u
#define GFNT_GLYF_HAVE_INSTRUCTIONS 0x0100u
#define GFNT_GLYF_USE_MY_METRICS 0x0200u
#define GFNT_GLYF_OVERLAP_COMPOUND 0x0400u
#define GFNT_GLYF_SCALED_OFFSET 0x0800u
#define GFNT_GLYF_UNSCALED_OFFSET 0x1000u

GFNT_Result gfnt_loca_range(const GFNT_Face * face, uint32_t glyph,
    size_t * out_offset, size_t * out_length, GFNT_Error * error) {
  const GFNT_Head * head = NULL;
  const GFNT_SfntTable * glyf_entry = NULL;
  GFNT_Reader loca;
  size_t glyphs = 0;
  size_t start = 0;
  size_t end = 0;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_offset || !out_length) {
    return GFNT_ERR_INVALID;
  }
  glyf_entry = gfnt_sfnt_find(face, GFNT_TAG_GLYF);
  if (!glyf_entry || !gfnt_sfnt_find(face, GFNT_TAG_LOCA)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, glyph,
        "this face has no glyf and loca pair, so it has no TrueType outlines");
  }
  result = gfnt_face_num_glyphs(face, &glyphs, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (glyph >= glyphs) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_LOCA, 0, glyph,
        "this glyph index is past the face's glyph count");
  }
  result = gfnt_face_head(face, &head, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_LOCA, &loca, error);
  if (result != GFNT_OK) {
    return result;
  }
  loca.error = error;

  if (head->glyph_data_format != 0) {
    // The font states a glyph format this library does not read. Said here,
    // once, rather than per glyph: it is a property of the font.
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED,
        GFNT_TAG('h', 'e', 'a', 'd'), 52, glyph,
        "this font declares a glyphDataFormat other than 0, which is the "
        "cubic glyf extension this library does not read");
  }
  if (head->index_to_loc_format == 0) {
    uint16_t low = 0;
    uint16_t high = 0;

    // The short format stores the offset divided by two, which is why an odd
    // glyph offset is unrepresentable in it and why a font with one uses the
    // long format.
    result = gfnt_reader_u16_at(&loca, (size_t)glyph * 2u, &low);
    if (result != GFNT_OK) {
      return result;
    }
    result = gfnt_reader_u16_at(&loca, ((size_t)glyph + 1u) * 2u, &high);
    if (result != GFNT_OK) {
      return result;
    }
    start = (size_t)low * 2u;
    end = (size_t)high * 2u;
  }
  else if (head->index_to_loc_format == 1) {
    uint32_t low = 0;
    uint32_t high = 0;

    result = gfnt_reader_u32_at(&loca, (size_t)glyph * 4u, &low);
    if (result != GFNT_OK) {
      return result;
    }
    result = gfnt_reader_u32_at(&loca, ((size_t)glyph + 1u) * 4u, &high);
    if (result != GFNT_OK) {
      return result;
    }
    start = low;
    end = high;
  }
  else {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG('h', 'e', 'a', 'd'),
        50, glyph, "head's indexToLocFormat is neither 0 nor 1");
  }

  // M11: a backwards or overrunning entry condemns this glyph, not the font.
  if (end < start) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_LOCA, 0, glyph,
        "this glyph's loca entry runs backwards");
  }
  if (end > glyf_entry->length) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_LOCA, 0, glyph,
        "this glyph's loca entry runs past the end of glyf");
  }
  *out_offset = start;
  *out_length = end - start;
  return GFNT_OK;
}

/**
 * A reader over one glyph's description, or the news that it has none.
 */
static GFNT_Result gfnt_glyf_reader(const GFNT_Face * face, uint32_t glyph,
    GFNT_Reader * out_reader, bool * out_empty, GFNT_Error * error) {
  GFNT_Reader glyf;
  size_t offset = 0;
  size_t length = 0;
  GFNT_Result result;

  result = gfnt_loca_range(face, glyph, &offset, &length, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_empty = length == 0;
  if (length == 0) {
    return GFNT_OK;
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_GLYF, &glyf, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_reader_sub(&glyf, offset, length, out_reader);
  if (result != GFNT_OK) {
    return result;
  }
  out_reader->error = error;
  return GFNT_OK;
}

/**
 * Read the flag stream, expanding REPEAT.
 *
 * @param reader Positioned at the first flag byte.
 * @param flags Receives one byte per point.
 * @param count How many points the contours imply.
 */
static GFNT_Result gfnt_glyf_flags(GFNT_Reader * reader, uint8_t * flags,
    size_t count, uint32_t glyph, GFNT_Error * error) {
  size_t filled = 0;

  while (filled < count) {
    uint8_t flag = 0;
    GFNT_Result result = gfnt_read_u8(reader, &flag);

    if (result != GFNT_OK) {
      return result;
    }
    flags[filled] = flag;
    filled += 1;
    if ((flag & GFNT_GLYF_REPEAT) == 0) {
      continue;
    }
    {
      uint8_t repeats = 0;

      result = gfnt_read_u8(reader, &repeats);
      if (result != GFNT_OK) {
        return result;
      }
      if ((size_t)repeats > count - filled) {
        // A repeat count that would describe more points than the contours
        // say there are is the font contradicting itself, and filling only
        // as many as fit would be this library inventing an interpretation.
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GLYF,
            gfnt_reader_tell(reader), glyph,
            "a repeated flag describes more points than the contours have");
      }
      for (uint8_t i = 0; i < repeats; ++i) {
        flags[filled] = flag;
        filled += 1;
      }
    }
  }
  return GFNT_OK;
}

/**
 * Read one coordinate stream, accumulating its deltas.
 *
 * The x and y streams differ only in which flag bits select them, so they
 * share this: two copies is how one of them comes to read the wrong bit.
 *
 * @param short_bit The flag bit saying the delta is one byte.
 * @param same_bit Its partner: the delta's sign when short, and "no change"
 *   when not.
 */
static GFNT_Result gfnt_glyf_coordinates(GFNT_Reader * reader,
    const uint8_t * flags, GFNT_Point * points, size_t count, bool horizontal,
    uint8_t short_bit, uint8_t same_bit) {
  int32_t value = 0;

  for (size_t i = 0; i < count; ++i) {
    int32_t delta = 0;
    GFNT_Result result;

    if (flags[i] & short_bit) {
      uint8_t magnitude = 0;

      result = gfnt_read_u8(reader, &magnitude);
      if (result != GFNT_OK) {
        return result;
      }
      delta = (flags[i] & same_bit) ? (int32_t)magnitude
                                    : -(int32_t)magnitude;
    }
    else if ((flags[i] & same_bit) == 0) {
      int16_t wide = 0;

      result = gfnt_read_s16(reader, &wide);
      if (result != GFNT_OK) {
        return result;
      }
      delta = wide;
    }
    // else: the same-bit without the short-bit means this coordinate repeats
    // the last one, so the delta stays zero and nothing is read.

    // The running total is kept in int32 and saturated into 26.6 on the way
    // out. A font whose deltas sum past the type is nonsense either way, and
    // signed overflow would be undefined behaviour rather than nonsense.
    value = gfnt_saturate32((int64_t)value + (int64_t)delta);
    if (horizontal) {
      points[i].x = gfnt_saturate32((int64_t)value * GFNT_F26DOT6_ONE);
    }
    else {
      points[i].y = gfnt_saturate32((int64_t)value * GFNT_F26DOT6_ONE);
    }
  }
  return GFNT_OK;
}

/**
 * A `gvar` delta to 26.6: the bits past the sixth go, rounded half away from zero.
 *
 * One rule for the outline's points and a composite's offsets, so that a component
 * placed by an offset and a point moved by a delta of the same size land in the
 * same place.
 */
static int64_t gfnt_glyf_delta_to_26dot6(int64_t delta) {
  return gfnt_round_shift(gfnt_clamp64(delta), GFNT_GVAR_FRACTION_BITS - 6);
}

/**
 * Move a simple glyph's points by what `gvar` says they do at @p variation.
 *
 * Done after the points are read and before anything else sees them, so that a
 * composite built from this glyph places the *varied* outline. The unvaried
 * positions are what `gvar` interpolates from, and they are read back out of the
 * outline here - whole font units, because nothing has yet been added to them -
 * rather than kept from the read, which would be a second array to fall out of
 * step with the first.
 */
static GFNT_Result gfnt_glyf_vary_simple(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, GFNT_Outline * outline, size_t base,
    size_t point_count, size_t first_contour, size_t contour_count,
    GFNT_Error * error) {
  const GFNT_Allocator * allocator = face->allocator;
  GFNT_GvarPoints points;
  int32_t * original = NULL;
  int64_t * delta = NULL;
  size_t * ends = NULL;
  GFNT_Result result;

  if (point_count == 0) {
    return GFNT_OK;
  }
  original = allocator->calloc_fn(allocator->ctx, point_count * 2u,
      sizeof *original);
  delta = allocator->calloc_fn(allocator->ctx,
      (point_count + GFNT_GVAR_PHANTOM_POINTS) * 2u, sizeof *delta);
  ends = allocator->calloc_fn(allocator->ctx, contour_count ? contour_count : 1u,
      sizeof *ends);
  if (!original || !delta || !ends) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_GVAR, 0, glyph,
        "no memory for a glyph's variation deltas");
    goto done;
  }
  for (size_t i = 0; i < point_count; ++i) {
    original[i] = outline->points[base + i].x / GFNT_F26DOT6_ONE;
    original[point_count + i] = outline->points[base + i].y / GFNT_F26DOT6_ONE;
  }
  for (size_t c = 0; c < contour_count; ++c) {
    ends[c] = outline->contours[first_contour + c] - base;
  }
  points.count = point_count;
  points.x = original;
  points.y = original + point_count;
  points.contour_ends = ends;
  points.contour_count = contour_count;
  result = gfnt_gvar_glyph_deltas(face, glyph, variation->coords,
      variation->count, &points, delta,
      delta + point_count + GFNT_GVAR_PHANTOM_POINTS, error);
  if (result != GFNT_OK) {
    goto done;
  }
  for (size_t i = 0; i < point_count; ++i) {
    outline->points[base + i].x = gfnt_saturate32((int64_t)outline->points[base
        + i].x + gfnt_glyf_delta_to_26dot6(delta[i]));
    outline->points[base + i].y = gfnt_saturate32((int64_t)outline->points[base
        + i].y + gfnt_glyf_delta_to_26dot6(
            delta[point_count + GFNT_GVAR_PHANTOM_POINTS + i]));
  }
done:
  allocator->free_fn(allocator->ctx, original);
  allocator->free_fn(allocator->ctx, delta);
  allocator->free_fn(allocator->ctx, ends);
  return result;
}

/**
 * A simple glyph: contours, flags, and the two coordinate streams.
 */
static GFNT_Result gfnt_glyf_simple(const GFNT_Face * face, GFNT_Reader * reader,
    int16_t contours, uint32_t glyph, const GFNT_Variation * variation,
    GFNT_Outline * outline, GFNT_Error * error) {
  size_t contour_count = (size_t)contours;
  size_t point_count = 0;
  size_t base = gfnt_outline_point_count(outline);
  size_t first_contour = gfnt_outline_contour_count(outline);
  uint16_t instruction_length = 0;
  uint8_t * flags = NULL;
  GFNT_Result result;

  // The caps on points and contours are enforced in gfnt_outline_reserve(),
  // which every path here goes through with the total before reading a
  // coordinate. There were duplicate checks at the top of this function; they
  // were removed because a defect planted in them could not be seen - the
  // second check refused the same input, so the first one's removal changed
  // nothing (`notes`: paired defences shadow each other).
  result = gfnt_reader_seek(reader, GFNT_GLYF_HEADER_BYTES);
  if (result != GFNT_OK) {
    return result;
  }

  // The contour ends come first and the last of them gives the point count, so
  // the ends are read into the outline's own contour array as they arrive and
  // the point count is known before a single coordinate is read.
  result = gfnt_outline_reserve(outline, 0, contour_count, error);
  if (result != GFNT_OK) {
    return result;
  }
  for (size_t i = 0; i < contour_count; ++i) {
    uint16_t last = 0;

    result = gfnt_read_u16(reader, &last);
    if (result != GFNT_OK) {
      return result;
    }
    if (i > 0 && (size_t)last + 1u < point_count) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GLYF,
          gfnt_reader_tell(reader), glyph,
          "endPtsOfContours does not increase, so a contour has no points");
    }
    point_count = (size_t)last + 1u;
    result = gfnt_outline_begin_contour(outline, error);
    if (result != GFNT_OK) {
      return result;
    }
    // begin_contour ends the contour at the point count as it stands, which is
    // `base` until the coordinates are read. The end this contour actually has
    // is known now, so it is written now and not looked up twice.
    outline->contours[first_contour + i] = base + point_count;
  }
  result = gfnt_read_u16(reader, &instruction_length);
  if (result != GFNT_OK) {
    return result;
  }
  // Skipped by length, never run: there is no interpreter (section 8.5).
  result = gfnt_reader_skip(reader, instruction_length);
  if (result != GFNT_OK) {
    return result;
  }

  result = gfnt_outline_reserve(outline, point_count, 0, error);
  if (result != GFNT_OK) {
    return result;
  }
  flags = face->allocator->calloc_fn(face->allocator->ctx, point_count ?
      point_count : 1u, sizeof *flags);
  if (!flags) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_GLYF, 0, glyph,
        "no memory for a glyph's flag stream");
  }
  result = gfnt_glyf_flags(reader, flags, point_count, glyph, error);
  if (result == GFNT_OK) {
    result = gfnt_glyf_coordinates(reader, flags, outline->points + base,
        point_count, true, GFNT_GLYF_X_SHORT, GFNT_GLYF_SAME_X);
  }
  if (result == GFNT_OK) {
    result = gfnt_glyf_coordinates(reader, flags, outline->points + base,
        point_count, false, GFNT_GLYF_Y_SHORT, GFNT_GLYF_SAME_Y);
  }
  if (result == GFNT_OK) {
    for (size_t i = 0; i < point_count; ++i) {
      if (flags[i] & GFNT_GLYF_CUBIC) {
        // Refused rather than read as a quadratic, which would draw a
        // different shape and report success.
        result = gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_GLYF, 0,
            glyph,
            "this glyph has a cubic control point, which is glyf version 1 "
            "and not a format this library reads");
        break;
      }
      outline->tags[base + i] = (flags[i] & GFNT_GLYF_ON_CURVE)
          ? (uint8_t)GFNT_POINT_ON
          : (uint8_t)GFNT_POINT_QUAD;
    }
  }
  if (result == GFNT_OK) {
    outline->point_count = base + point_count;
  }
  face->allocator->free_fn(face->allocator->ctx, flags);
  if (result == GFNT_OK && variation) {
    result = gfnt_glyf_vary_simple(face, glyph, variation, outline, base,
        point_count, first_contour, contour_count, error);
  }
  return result;
}

/**
 * One component of a composite, as its flags describe it.
 */
typedef struct GFNT_Component {
  uint16_t flags;      ///< The component's own flag word.
  uint16_t glyph;      ///< Which glyph it places.
  int32_t arg1;        ///< x offset, or a point index in the composite.
  int32_t arg2;        ///< y offset, or a point index in the component.
  /**
   * The 2x2, widened from whichever of the three forms was stored and held
   * **row-major**: x' = xx*x + xy*y, y' = yx*x + yy*y.
   *
   * The file is column-major, and the two are transposes of each other. The
   * mapping happens once, where the four values are read, and is spelled out
   * there; a shear read transposed leans the wrong way and reports success,
   * and only a `WE_HAVE_A_TWO_BY_TWO` component can tell the difference - the
   * other two encodings are diagonal, where a transpose is invisible.
   */
  GFNT_F16Dot16 xx;
  GFNT_F16Dot16 xy;
  GFNT_F16Dot16 yx;
  GFNT_F16Dot16 yy;
} GFNT_Component;

/**
 * Read one component record, leaving the reader on the next.
 */
static GFNT_Result gfnt_glyf_component(GFNT_Reader * reader, uint32_t glyph,
    GFNT_Component * out, GFNT_Error * error) {
  uint16_t flags = 0;
  uint16_t index = 0;
  GFNT_Result result = gfnt_read_u16(reader, &flags);

  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_read_u16(reader, &index);
  if (result != GFNT_OK) {
    return result;
  }
  out->flags = flags;
  out->glyph = index;
  out->xx = GFNT_F16DOT16_ONE;
  out->xy = 0;
  out->yx = 0;
  out->yy = GFNT_F16DOT16_ONE;

  if (flags & GFNT_GLYF_ARGS_ARE_WORDS) {
    // Signed when they are offsets, unsigned when they are point indices, and
    // the flag that decides which is a different one. Read as signed and
    // reinterpreted below, because a point index large enough to look negative
    // is past any outline this library will accept anyway.
    int16_t a = 0;
    int16_t b = 0;

    result = gfnt_read_s16(reader, &a);
    if (result != GFNT_OK) {
      return result;
    }
    result = gfnt_read_s16(reader, &b);
    if (result != GFNT_OK) {
      return result;
    }
    out->arg1 = a;
    out->arg2 = b;
  }
  else if (flags & GFNT_GLYF_ARGS_ARE_XY) {
    int8_t a = 0;
    int8_t b = 0;

    result = gfnt_read_s8(reader, &a);
    if (result != GFNT_OK) {
      return result;
    }
    result = gfnt_read_s8(reader, &b);
    if (result != GFNT_OK) {
      return result;
    }
    out->arg1 = a;
    out->arg2 = b;
  }
  else {
    // Byte-sized point indices are unsigned; byte-sized offsets are signed.
    // This is the one place the two readings of the same two bytes differ, and
    // getting it wrong moves a component by up to 256 units without an error.
    uint8_t a = 0;
    uint8_t b = 0;

    result = gfnt_read_u8(reader, &a);
    if (result != GFNT_OK) {
      return result;
    }
    result = gfnt_read_u8(reader, &b);
    if (result != GFNT_OK) {
      return result;
    }
    out->arg1 = a;
    out->arg2 = b;
  }

  if (flags & GFNT_GLYF_HAVE_SCALE) {
    GFNT_F2Dot14 scale = 0;

    result = gfnt_read_f2dot14(reader, &scale);
    if (result != GFNT_OK) {
      return result;
    }
    out->xx = gfnt_f2dot14_to_f16dot16(scale);
    out->yy = out->xx;
  }
  else if (flags & GFNT_GLYF_HAVE_XY_SCALE) {
    GFNT_F2Dot14 x_scale = 0;
    GFNT_F2Dot14 y_scale = 0;

    result = gfnt_read_f2dot14(reader, &x_scale);
    if (result != GFNT_OK) {
      return result;
    }
    result = gfnt_read_f2dot14(reader, &y_scale);
    if (result != GFNT_OK) {
      return result;
    }
    out->xx = gfnt_f2dot14_to_f16dot16(x_scale);
    out->yy = gfnt_f2dot14_to_f16dot16(y_scale);
  }
  else if (flags & GFNT_GLYF_HAVE_TWO_BY_TWO) {
    GFNT_F2Dot14 values[4] = { 0, 0, 0, 0 };

    for (size_t i = 0; i < 4; ++i) {
      result = gfnt_read_f2dot14(reader, &values[i]);
      if (result != GFNT_OK) {
        return result;
      }
    }
    // The file's four values are xscale, scale01, scale10, yscale, and the
    // specification applies them as
    //
    //     x' = xscale*x + scale10*y
    //     y' = scale01*x + yscale*y
    //
    // which is column-major. This struct is row-major, so scale01 and scale10
    // cross over here - the one place they do. Reading them straight across
    // shears a component the wrong way with no error anywhere, and a diagonal
    // matrix cannot tell the two readings apart, so `comp-two-by-two` in
    // outline-composite.ttf is the fixture that can.
    out->xx = gfnt_f2dot14_to_f16dot16(values[0]);
    out->yx = gfnt_f2dot14_to_f16dot16(values[1]);
    out->xy = gfnt_f2dot14_to_f16dot16(values[2]);
    out->yy = gfnt_f2dot14_to_f16dot16(values[3]);
  }
  (void)glyph;
  (void)error;
  return GFNT_OK;
}

/**
 * The offset a component is placed at, in 26.6.
 *
 * `SCALED_COMPONENT_OFFSET` and `UNSCALED_COMPONENT_OFFSET` are the
 * Apple/Microsoft disagreement about whether the offset is in the component's
 * space or the composite's. The default when neither flag is set is
 * **unscaled**, which is Microsoft's reading and what every shipping rasteriser
 * does; a font that wants Apple's says so with the flag.
 */
static void gfnt_glyf_offset(const GFNT_Component * component, int64_t delta_x,
    int64_t delta_y, GFNT_Point * out_offset) {
  GFNT_Point offset;

  // The variation's delta is added to the offset *as stored*, before the
  // component's own transform scales it when the flags say the offset is in the
  // component's space: `gvar` varies the numbers in the file and the flags decide
  // what those numbers mean.
  offset.x = gfnt_saturate32((int64_t)component->arg1 * GFNT_F26DOT6_ONE
      + gfnt_glyf_delta_to_26dot6(delta_x));
  offset.y = gfnt_saturate32((int64_t)component->arg2 * GFNT_F26DOT6_ONE
      + gfnt_glyf_delta_to_26dot6(delta_y));
  if ((component->flags & GFNT_GLYF_SCALED_OFFSET)
      && !(component->flags & GFNT_GLYF_UNSCALED_OFFSET)) {
    offset = gfnt_outline_apply_matrix(offset, component->xx, component->xy,
        component->yx, component->yy);
  }
  *out_offset = offset;
}

/**
 * What `gvar` says each component of a composite moves by at @p variation.
 *
 * A composite's "points" are its components - one per component record, in file
 * order, before the four phantom points - and what a delta to one of them moves is
 * its **offset**. The components are counted by reading the records once without
 * acting on them, because `gvar`'s "every point" shorthand and its point numbers
 * are both relative to a count the composite does not state anywhere.
 *
 * @param reader Positioned on the first component record. Not advanced.
 * @param out_deltas Receives `2 * (count + 4)` values, x then y, owned by
 *   the caller and freed with the allocator's `free_fn`; NULL when the glyph has
 *   no variation data to apply.
 * @param out_count Receives the number of components.
 */
static GFNT_Result gfnt_glyf_vary_composite(const GFNT_Face * face,
    uint32_t glyph, const GFNT_Variation * variation, const GFNT_Reader * reader,
    int64_t ** out_deltas, size_t * out_count, GFNT_Error * error) {
  const GFNT_Allocator * allocator = face->allocator;
  GFNT_Reader scan = *reader;
  GFNT_GvarPoints points;
  GFNT_Component component;
  size_t count = 0;
  size_t total;
  int64_t * deltas = NULL;
  GFNT_Result result;

  do {
    result = gfnt_glyf_component(&scan, glyph, &component, error);
    if (result != GFNT_OK) {
      return result;
    }
    count += 1;
  } while (component.flags & GFNT_GLYF_MORE_COMPONENTS);
  total = count + GFNT_GVAR_PHANTOM_POINTS;
  deltas = allocator->calloc_fn(allocator->ctx, total * 2u, sizeof *deltas);
  if (!deltas) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_GVAR, 0, glyph,
        "no memory for a composite's variation deltas");
  }
  memset(&points, 0, sizeof points);
  points.count = count;
  result = gfnt_gvar_glyph_deltas(face, glyph, variation->coords,
      variation->count, &points, deltas, deltas + total, error);
  if (result != GFNT_OK) {
    allocator->free_fn(allocator->ctx, deltas);
    return result;
  }
  *out_deltas = deltas;
  *out_count = count;
  return GFNT_OK;
}

static GFNT_Result gfnt_glyf_load_chained(const GFNT_Face * face,
    uint32_t glyph, size_t depth, const GFNT_GlyphChain * chain,
    const GFNT_Variation * variation, GFNT_Outline * outline,
    GFNT_Error * error);

/**
 * The components of a composite, each loaded, placed and appended.
 *
 * Separate from ::gfnt_glyf_load_chained() so that the one allocation a variation
 * needs - the components' deltas - has one place it is freed, rather than one per
 * `return` of a loop that has eight.
 *
 * @param deltas The components' deltas, x then y with the four phantom
 *   points between, or NULL for the default instance.
 * @param component_count How many components the composite has, when @p deltas is
 *   not NULL.
 */
static GFNT_Result gfnt_glyf_composite(const GFNT_Face * face, uint32_t glyph,
    size_t depth, const GFNT_GlyphChain * here, const GFNT_Variation * variation,
    GFNT_Reader * reader_in, size_t composite_base, const int64_t * deltas,
    size_t component_count, GFNT_Outline * outline, GFNT_Error * error) {
  GFNT_Reader reader = *reader_in;
  size_t component_index = 0;
  GFNT_Result result;

  for (;;) {
    GFNT_Component component;
    GFNT_Outline * part = NULL;
    GFNT_Point offset;

    result = gfnt_glyf_component(&reader, glyph, &component, error);
    if (result != GFNT_OK) {
      return result;
    }
    if (gfnt_glyph_chain_has(here, component.glyph)) {
      // A glyph that reaches itself would recurse to the depth cap and report a
      // limit, which is a true statement about a font that is not deep but
      // circular - and one a caller acts on by raising a budget that will never
      // be enough. Named for what it is instead.
      //
      // Against the whole chain and not just this composite: `A -> B -> A` is a
      // cycle too, and was the case this reader used to walk to the cap while
      // `ebdt.c` named it. src/core/chain.h is now the one place either asks.
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GLYF,
          gfnt_reader_tell(&reader), glyph,
          "a composite glyph includes itself as a component, directly or "
          "through another glyph");
    }
    result = gfnt_outline_create(face->allocator, &part, error);
    if (result != GFNT_OK) {
      return result;
    }
    gfnt_outline_set_limits(part, &face->limits);
    result = gfnt_glyf_load_chained(face, component.glyph, depth + 1, here,
        variation, part, error);
    if (result != GFNT_OK) {
      gfnt_outline_destroy(part);
      return result;
    }

    if (component.flags & GFNT_GLYF_ARGS_ARE_XY) {
      // A component placed by matching points has no offset for a delta to
      // move, and its delta is ignored: the matched points have already moved,
      // being points of glyphs that were themselves varied.
      gfnt_glyf_offset(&component,
          deltas ? deltas[component_index] : 0,
          deltas ? deltas[component_index + GFNT_GVAR_PHANTOM_POINTS
              + component_count] : 0, &offset);
    }
    else {
      // Point matching: arg1 indexes a point of what has been assembled so
      // far, arg2 a point of this component, and the component moves so that
      // the two coincide. An index either array does not have is the font
      // contradicting itself; guessing zero would silently stack the
      // component on the origin.
      GFNT_Point anchor;
      GFNT_Point moving;
      GFNT_Point placed;

      if (gfnt_outline_point_at(outline,
              composite_base + (size_t)component.arg1, &anchor, NULL)
              != GFNT_OK
          || gfnt_outline_point_at(part, (size_t)component.arg2, &moving, NULL)
              != GFNT_OK) {
        gfnt_outline_destroy(part);
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GLYF,
            gfnt_reader_tell(&reader), glyph,
            "a component matches a point neither glyph has");
      }
      // The component's point is matched after its own transform, which is
      // what makes a scaled component line up with the anchor rather than
      // near it.
      placed = gfnt_outline_apply_matrix(moving, component.xx, component.xy,
          component.yx, component.yy);
      offset.x = gfnt_saturate32((int64_t)anchor.x - (int64_t)placed.x);
      offset.y = gfnt_saturate32((int64_t)anchor.y - (int64_t)placed.y);
    }

    result = gfnt_outline_append_transformed(outline, part, component.xx,
        component.xy, component.yx, component.yy, offset.x, offset.y, error);
    gfnt_outline_destroy(part);
    if (result != GFNT_OK) {
      return result;
    }
    component_index += 1;
    if ((component.flags & GFNT_GLYF_MORE_COMPONENTS) == 0) {
      break;
    }
  }
  return GFNT_OK;
}

/**
 * ::gfnt_glyf_load() with the recursion's state, which is what recurses.
 *
 * @p chain is every glyph this walk already has open, innermost last, and @p
 * depth is how many of them there are. Both, rather than deriving the depth from
 * the chain's length, because the chain is walked per *component* and the depth
 * is compared per *call*: counting frames would make the cheaper of the two
 * checks the more expensive one.
 *
 * @p variation is passed down unchanged, so that every component of a composite is
 * drawn at the same location as the glyph that places it.
 */
static GFNT_Result gfnt_glyf_load_chained(const GFNT_Face * face,
    uint32_t glyph, size_t depth, const GFNT_GlyphChain * chain,
    const GFNT_Variation * variation, GFNT_Outline * outline,
    GFNT_Error * error) {
  GFNT_Reader reader;
  GFNT_GlyphChain here;
  bool empty = false;
  int16_t contours = 0;
  size_t composite_base = 0;
  int64_t * component_deltas = NULL;
  size_t component_count = 0;
  GFNT_Result result;

  if (depth > face->limits.max_composite_depth) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_GLYF, 0, glyph,
        "composite glyphs nest deeper than max_composite_depth allows");
  }
  result = gfnt_glyf_reader(face, glyph, &reader, &empty, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (empty) {
    // A glyph with no description draws nothing, and that is an answer.
    return GFNT_OK;
  }
  result = gfnt_read_s16(&reader, &contours);
  if (result != GFNT_OK) {
    return result;
  }
  if (contours == 0) {
    // A description that exists and states zero contours. Fonts do this.
    return GFNT_OK;
  }
  if (contours > 0) {
    return gfnt_glyf_simple(face, &reader, contours, glyph, variation, outline,
        error);
  }

  // A composite: see gfnt_glyf_composite(). `composite_base` is where this call's
  // own contribution starts, because that function *appends*: a point index in a
  // component record counts from the start of the composite being assembled, not
  // from the start of whatever the outline already holds. Every caller today
  // hands over an outline that is empty - a nested composite gets a fresh one per
  // component - so the subtraction is zero in every font this library has met,
  // and it is here because the correctness of point matching should not rest on
  // that.
  composite_base = gfnt_outline_point_count(outline);
  here.parent = chain;
  here.glyph = glyph;
  result = gfnt_reader_seek(&reader, GFNT_GLYF_HEADER_BYTES);
  if (result != GFNT_OK) {
    return result;
  }
  if (variation) {
    result = gfnt_glyf_vary_composite(face, glyph, variation, &reader,
        &component_deltas, &component_count, error);
    if (result != GFNT_OK) {
      return result;
    }
  }
  result = gfnt_glyf_composite(face, glyph, depth, &here, variation, &reader,
      composite_base, component_deltas, component_count, outline, error);
  face->allocator->free_fn(face->allocator->ctx, component_deltas);
  return result;
}

GFNT_Result gfnt_glyf_load(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, GFNT_Outline * outline,
    GFNT_Error * error) {
  if (!face || !outline) {
    return GFNT_ERR_INVALID;
  }
  // Depth zero and an empty chain: this is the top of a walk by construction,
  // which is why there is no parameter for either. A caller that could claim to
  // be partway down one could claim a depth without the chain that goes with it,
  // and the cycle check would then be missing exactly the frames that prove the
  // cycle.
  return gfnt_glyf_load_chained(face, glyph, 0, NULL, variation, outline, error);
}

GFNT_Result gfnt_glyf_is_composite(const GFNT_Face * face, uint32_t glyph,
    bool * out_composite, GFNT_Error * error) {
  GFNT_Reader reader;
  bool empty = false;
  int16_t contours = 0;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_composite) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_glyf_reader(face, glyph, &reader, &empty, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (empty) {
    *out_composite = false;
    return GFNT_OK;
  }
  result = gfnt_read_s16(&reader, &contours);
  if (result != GFNT_OK) {
    return result;
  }
  *out_composite = contours < 0;
  return GFNT_OK;
}

GFNT_Result gfnt_glyf_stated_box(const GFNT_Face * face, uint32_t glyph,
    GFNT_Box * out_box, GFNT_Error * error) {
  GFNT_Reader reader;
  bool empty = false;
  int16_t values[5] = { 0, 0, 0, 0, 0 };
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_box) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_glyf_reader(face, glyph, &reader, &empty, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (empty) {
    // An empty box, which gfnt_box_is_empty() reports: a glyph with no
    // description states no bounds, and zeroes would be a claim it did not
    // make.
    out_box->x_min = 1;
    out_box->y_min = 1;
    out_box->x_max = 0;
    out_box->y_max = 0;
    return GFNT_OK;
  }
  for (size_t i = 0; i < 5; ++i) {
    result = gfnt_read_s16(&reader, &values[i]);
    if (result != GFNT_OK) {
      return result;
    }
  }
  // Font units times 64, the same space an outline's points are in, so that
  // the two can be compared without a conversion at the comparison site.
  out_box->x_min = (GFNT_F26Dot6)values[1] * GFNT_F26DOT6_ONE;
  out_box->y_min = (GFNT_F26Dot6)values[2] * GFNT_F26DOT6_ONE;
  out_box->x_max = (GFNT_F26Dot6)values[3] * GFNT_F26DOT6_ONE;
  out_box->y_max = (GFNT_F26Dot6)values[4] * GFNT_F26DOT6_ONE;
  return GFNT_OK;
}
