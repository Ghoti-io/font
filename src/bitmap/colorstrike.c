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
 * Color bitmap strikes: a glyph that is a picture. `CBDT` here.
 *
 * documentation/design.md section 7.5. `CBLC` is `EBLC`'s layout, so its strike
 * list and the index that finds a glyph's bytes are `eblc.c`'s and `ebdt.c`'s,
 * parameterized by table; what is written here is what comes after the index: the
 * three PNG image formats (17 with small metrics, 18 with big ones, 19 with the
 * metrics in the index), and the decode.
 *
 * **One glyph at a time.** `EBDT` strikes are parsed whole because a mono glyph
 * costs a few bytes. A color strike is hundreds of kilobytes of PNG per hundred
 * glyphs, and decoding the 3,000 pictures of Noto Color Emoji to answer for one
 * is not a cost anyone asked for; so the index subtables are read per call and
 * the one glyph's bytes are borrowed from the face.
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/bitmap.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <string.h>
#include "../sfnt/sfnt.h"
#include "colorstrike.h"
#include "eblc.h"

/** `CBDT`'s version: 3.0 in the files that exist, 2.0 from the first writers. */
#define GFNT_CBDT_VERSION_2 0x00020000u
#define GFNT_CBDT_VERSION_3 0x00030000u

/** Bytes in a `SmallGlyphMetrics`, and in a `BigGlyphMetrics`. */
#define GFNT_CBDT_SMALL_METRICS_BYTES 5u
#define GFNT_CBDT_BIG_METRICS_BYTES 8u

/**
 * One `CBDT` glyph: find it through the index, read its metrics, borrow its PNG.
 *
 * @param strike The `CBLC` strike.
 */
GFNT_Result gfnt_cbdt_find(const GFNT_Face * face,
    const GFNT_EblcStrike * strike, uint32_t glyph, GFNT_CbdtGlyph * found_glyph,
    GFNT_Error * error) {
  GFNT_GlyphImage * out = &found_glyph->image;
  GFNT_EblcSubtable * subtables = NULL;
  const GFNT_EblcSubtable * subtable;
  size_t subtable_count = 0;
  size_t offset = 0;
  size_t length = 0;
  bool found = false;
  GFNT_Reader data;
  GFNT_Reader at;
  GFNT_EblcMetrics metrics;
  uint32_t version = 0;
  uint32_t png_length = 0;
  const uint8_t * bytes = NULL;
  size_t consumed;
  GFNT_Result result;

  if (strike->bit_depth != 32) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CBDT, 0, glyph,
        "a CBLC strike that is not 32 bits deep, which this library does not "
        "read as color");
  }
  if (glyph > UINT16_MAX || glyph < strike->start_glyph
      || glyph > strike->end_glyph) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CBDT, 0, glyph,
        "a glyph this strike does not carry");
  }
  result = gfnt_eblc_strike_index(face, strike, &subtables, &subtable_count,
      error);
  if (result != GFNT_OK) {
    return result;
  }
  subtable = gfnt_eblc_subtable_for(subtables, subtable_count, glyph);
  if (!subtable) {
    result = gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CBDT, 0, glyph,
        "a glyph this strike does not carry");
    goto done;
  }
  result = gfnt_eblc_locate_glyph(face, subtable, glyph, &offset, &length,
      &found, error);
  if (result != GFNT_OK) {
    goto done;
  }
  if (!found) {
    result = gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CBDT, 0, glyph,
        "a glyph this strike does not carry");
    goto done;
  }
  if (subtable->image_format < 17 || subtable->image_format > 19) {
    result = gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CBDT, offset,
        glyph, "a CBDT image format that is not a PNG - this library reads 17, "
        "18 and 19");
    goto done;
  }

  result = gfnt_face_table_reader(face, GFNT_TAG_CBDT, &data, error);
  if (result != GFNT_OK) {
    goto done;
  }
  if (gfnt_reader_u32_at(&data, 0, &version) != GFNT_OK) {
    result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CBDT, 0, glyph,
        "a CBDT shorter than its own header");
    goto done;
  }
  if (version != GFNT_CBDT_VERSION_2 && version != GFNT_CBDT_VERSION_3) {
    result = gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CBDT, 0, glyph,
        "a CBDT version this library does not read");
    goto done;
  }

  at = data;
  if (gfnt_reader_seek(&at, offset) != GFNT_OK) {
    result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CBDT, offset, glyph,
        "a CBDT glyph offset past the end of the table");
    goto done;
  }
  memset(&metrics, 0, sizeof metrics);
  switch (subtable->image_format) {
    case 17:
      consumed = GFNT_CBDT_SMALL_METRICS_BYTES;
      if (gfnt_read_u8(&at, &metrics.height) != GFNT_OK
          || gfnt_read_u8(&at, &metrics.width) != GFNT_OK
          || gfnt_read_s8(&at, &metrics.bearing_x) != GFNT_OK
          || gfnt_read_s8(&at, &metrics.bearing_y) != GFNT_OK
          || gfnt_read_u8(&at, &metrics.advance) != GFNT_OK) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CBDT, offset,
            glyph, "a CBDT glyph too short for its SmallGlyphMetrics");
        goto done;
      }
      break;
    case 18:
      consumed = GFNT_CBDT_BIG_METRICS_BYTES;
      if (gfnt_read_u8(&at, &metrics.height) != GFNT_OK
          || gfnt_read_u8(&at, &metrics.width) != GFNT_OK
          || gfnt_read_s8(&at, &metrics.bearing_x) != GFNT_OK
          || gfnt_read_s8(&at, &metrics.bearing_y) != GFNT_OK
          || gfnt_read_u8(&at, &metrics.advance) != GFNT_OK
          || gfnt_reader_skip(&at, 3) != GFNT_OK) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CBDT, offset,
            glyph, "a CBDT glyph too short for its BigGlyphMetrics");
        goto done;
      }
      break;
    default:
      // Format 19: the index states the metrics for every glyph it covers, and
      // the data is a length and a PNG. Index formats 2 and 5 are the only ones
      // that do, which the index parse has already required.
      consumed = 0;
      metrics = subtable->metrics;
      break;
  }
  if (gfnt_read_u32(&at, &png_length) != GFNT_OK) {
    result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CBDT, offset, glyph,
        "a CBDT glyph with no dataLen");
    goto done;
  }
  // The index says how many bytes the glyph owns; the PNG must sit inside them.
  // A dataLen that reaches past would borrow the next glyph's bytes as this one's
  // picture, which is the shape of mistake that decodes into someone else's face.
  if (length != 0 && (size_t)png_length > length - consumed - 4u) {
    result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CBDT, offset, glyph,
        "a CBDT glyph whose dataLen is longer than the index gives it");
    goto done;
  }
  if (gfnt_read_bytes(&at, png_length, &bytes) != GFNT_OK || png_length == 0) {
    result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CBDT, offset, glyph,
        "a CBDT picture that runs past the end of the table");
    goto done;
  }

  found_glyph->index_format = subtable->index_format;
  found_glyph->image_format = subtable->image_format;
  *out = (GFNT_GlyphImage) {
    .format = GFNT_IMAGE_PNG,
    .data = bytes,
    .length = png_length,
    .width = metrics.width,
    .height = metrics.height,
    .bearing_x = metrics.bearing_x,
    .bearing_y = metrics.bearing_y,
    .advance = metrics.advance,
    .strike = strike->strike,
  };
  result = GFNT_OK;

done:
  face->allocator->free_fn(face->allocator->ctx, subtables);
  return result;
}

/* The rule is HarfBuzz's, found by experiment: bitmap_extents.c has the findings. */
size_t gfnt_bitmap_strike_for(const GFNT_Eblc * cblc, uint32_t requested) {
  size_t best = 0;
  uint32_t best_ppem;

  if (requested == 0) {
    requested = UINT32_MAX;
  }
  best_ppem = cblc->strikes[0].strike.ppem_x > cblc->strikes[0].strike.ppem_y
      ? cblc->strikes[0].strike.ppem_x : cblc->strikes[0].strike.ppem_y;
  for (size_t i = 1; i < cblc->strike_count; ++i) {
    const GFNT_Strike * strike = &cblc->strikes[i].strike;
    uint32_t ppem = strike->ppem_x > strike->ppem_y ? strike->ppem_x
                                                    : strike->ppem_y;

    if ((requested <= ppem && ppem < best_ppem)
        || (requested > best_ppem && ppem > best_ppem)) {
      best = i;
      best_ppem = ppem;
    }
  }
  return best;
}

GFNT_Result gfnt_face_glyph_image(const GFNT_Face * face, uint32_t glyph,
    size_t strike, GFNT_GlyphImage * out_image, GFNT_Error * error) {
  size_t glyphs = 0;
  size_t count = 0;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_image) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the image");
  }
  result = gfnt_face_num_glyphs(face, &glyphs, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (glyph >= glyphs) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, glyph,
        "a glyph index past the end of the face");
  }
  result = gfnt_face_strike_count(face, &count, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (strike >= count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, glyph,
        "a strike index the face does not have");
  }
  // The strike list is one table's: EBLC's when the face has one, else CBLC's
  // (glyph.c's order), so the index means the same thing here as there.
  if (!gfnt_face_has_eblc(face) && gfnt_face_has_cblc(face)) {
    const GFNT_Eblc * cblc = NULL;
    GFNT_CbdtGlyph found;

    result = gfnt_face_cblc(face, &cblc, error);
    if (result != GFNT_OK) {
      return result;
    }
    result = gfnt_cbdt_find(face, &cblc->strikes[strike], glyph, &found, error);
    if (result != GFNT_OK) {
      return result;
    }
    *out_image = found.image;
    return GFNT_OK;
  }
  return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, glyph,
      "the strike is not a color strike");
}

/**
 * Decode a picture to straight-alpha RGBA8 in memory the face's allocator owns.
 */
static GFNT_Result gfnt_color_decode(const GFNT_Face * face,
    const GFNT_GlyphImage * image, GFNT_ColorBitmap * out, GFNT_Error * error) {
  GIMG_Stream * stream = NULL;
  GIMG_Doc * doc = NULL;
  GIMG_Raster * raster = NULL;
  const GIMG_Pixel_Format * format;
  GIMG_Limits limits;
  GIMG_Load_Options load;
  GIMG_Decode_Options decode;
  const GIMG_Item * item;
  GFNT_Result result = GFNT_ERR_CORRUPT;
  size_t pixels;
  size_t bytes;
  uint32_t width;
  uint32_t height;
  uint8_t * rgba;
  bool gray;

  gimg_limits_default(&limits);
  // The cap that bounds a decoded picture is the same one that bounds a
  // rasterized glyph: both are a buffer of pixels this library hands a caller.
  limits.max_decoded_pixels = face->limits.max_raster_bytes / 4u;
  memset(&load, 0, sizeof load);
  load.limits = &limits;
  memset(&decode, 0, sizeof decode);
  decode.limits = &limits;

  if (gimg_stream_create_memory(image->data, image->length, &stream) != GIMG_OK) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the picture's stream");
  }
  switch (gimg_doc_load(stream, &load, NULL, &doc)) {
    case GIMG_OK:
      break;
    case GIMG_ERR_OOM:
      result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory to load the picture");
      goto done;
    case GIMG_ERR_LIMIT:
      result = gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
          "a picture larger than GFNT_Limits::max_raster_bytes allows");
      goto done;
    case GIMG_ERR_UNSUPPORTED:
      result = gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0,
          GFNT_GLYPH_NONE, "a picture encoding the image library does not read");
      goto done;
    default:
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, 0, 0, GFNT_GLYPH_NONE,
          "a glyph picture that does not decode");
      goto done;
  }
  item = gimg_doc_item(doc, 0);
  if (!item) {
    result = gfnt_error_set(error, GFNT_ERR_CORRUPT, 0, 0, GFNT_GLYPH_NONE,
        "a glyph picture with no image in it");
    goto done;
  }
  switch (gimg_item_decode(item, &decode, &raster)) {
    case GIMG_OK:
      break;
    case GIMG_ERR_OOM:
      result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory to decode the picture");
      goto done;
    case GIMG_ERR_LIMIT:
      result = gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
          "a picture larger than GFNT_Limits::max_raster_bytes allows");
      goto done;
    case GIMG_ERR_UNSUPPORTED:
      result = gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0,
          GFNT_GLYPH_NONE, "a picture the image library does not decode");
      goto done;
    default:
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, 0, 0, GFNT_GLYPH_NONE,
          "a glyph picture that does not decode");
      goto done;
  }

  format = gimg_raster_format(raster);
  if (!format || format->channel_type != GIMG_CHANNEL_UNORM
      || format->layout != GIMG_LAYOUT_INTERLEAVED
      || format->bits_per_channel[0] != 8
      || !((format->channel_model == GIMG_CHANNEL_RGBA
               && format->channel_count == 4)
          || (format->channel_model == GIMG_CHANNEL_GRAY
              && format->channel_count == 1))) {
    // image decodes a PNG to the form the file is in: RGBA8 for color and for
    // grey with alpha, GRAY8 for grey. A 16-bit file would have to be narrowed,
    // and narrowing is a rounding choice this library has no reference for, so it
    // is declined by name rather than guessed at.
    result = gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
        "a glyph picture that is not 8 bits per channel, grey or RGBA");
    goto done;
  }
  gray = format->channel_model == GIMG_CHANNEL_GRAY;

  width = gimg_raster_width(raster);
  height = gimg_raster_height(raster);
  if (!gcu_safe_mul_size((size_t)width, (size_t)height, &pixels)
      || !gcu_safe_mul_size(pixels, 4u, &bytes)
      || bytes > face->limits.max_raster_bytes) {
    result = gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "a picture larger than GFNT_Limits::max_raster_bytes allows");
    goto done;
  }
  rgba = face->allocator->malloc_fn(face->allocator->ctx, bytes ? bytes : 1u);
  if (!rgba) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the decoded picture");
    goto done;
  }
  {
    // The raster's rows may be padded to its own alignment; ours are not.
    const uint8_t * source = gimg_raster_pixels_const(raster);
    size_t source_stride = gimg_raster_stride_bytes(raster);

    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t * from = source + (size_t)y * source_stride;
      uint8_t * to = rgba + (size_t)y * width * 4u;

      if (!gray) {
        memcpy(to, from, (size_t)width * 4u);
        continue;
      }
      for (uint32_t x = 0; x < width; ++x) {
        to[4u * x] = to[4u * x + 1u] = to[4u * x + 2u] = from[x];
        to[4u * x + 3u] = 255u;
      }
    }
  }

  *out = (GFNT_ColorBitmap) {
    .width = width,
    .height = height,
    .stride = (size_t)width * 4u,
    .bearing_x = image->bearing_x,
    .bearing_y = image->bearing_y,
    .advance = image->advance,
    .rgba = rgba,
    .strike = image->strike,
    .allocator = face->allocator,
  };
  result = GFNT_OK;

done:
  if (raster) {
    gimg_raster_destroy(raster);
  }
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(stream);
  return result;
}

GFNT_Result gfnt_face_glyph_color_bitmap(const GFNT_Face * face, uint32_t glyph,
    size_t strike, GFNT_ColorBitmap * out_bitmap, GFNT_Error * error) {
  GFNT_GlyphImage image;
  GFNT_ColorBitmap bitmap;
  GFNT_Result result;

  if (!face || !out_bitmap) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the bitmap");
  }
  result = gfnt_face_glyph_image(face, glyph, strike, &image, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_color_decode(face, &image, &bitmap, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_bitmap = bitmap;
  return GFNT_OK;
}

void gfnt_color_bitmap_free(GFNT_ColorBitmap * bitmap) {
  if (!bitmap) {
    return;
  }
  if (bitmap->rgba && bitmap->allocator) {
    bitmap->allocator->free_fn(bitmap->allocator->ctx, bitmap->rgba);
  }
  memset(bitmap, 0, sizeof *bitmap);
}
