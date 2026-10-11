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
 * A glyph that is pixels rather than a path.
 *
 * documentation/design.md sections 5.4 and 7.5. `glyph.h` says what a glyph can
 * be and which strike answers a size; this is the ::GFNT_GLYPH_BITMAP_MONO arm's
 * data, and the accessor that returns it.
 *
 * Four standalone containers are read (section 7.1) - PCF, BDF, PSF 1 and 2, and
 * GNU Unifont's `.hex` - and each produces a face with **one strike and no
 * outlines**.
 *
 * Inside an sfnt there can be several. `EBLC`/`EBDT` is read (and `CBLC`/`CBDT`, whose
 * pictures are ::gfnt_face_glyph_image() and ::gfnt_face_glyph_color_bitmap()): a face with embedded
 * strikes reports how many it has, at what sizes, and where each puts its
 * baseline - which is what ::gfnt_face_select_strike() needs and the first thing in
 * this library that has to **choose** between strikes rather than confirm the only
 * one - and its glyphs come back through this same accessor.
 *
 * Two things differ for such a face, and both are about a strike being sparse over
 * a glyph count that came from `maxp` rather than from the strike itself:
 *
 *  - **A glyph a strike does not carry is ::GFNT_ERR_UNSUPPORTED**, not an invalid
 *    index. The index is fine and another strike may well answer for it.
 *  - **A glyph whose data the font gets wrong is ::GFNT_ERR_CORRUPT, and only that
 *    glyph** (M11). Every other glyph of the strike still answers, as a face with
 *    one bad `loca` entry still answers for every other glyph.
 *
 * `bloc`, `CBDT` and `sbix` are not read at all and refuse at the strike list,
 * which is a different answer again - `glyph.h` exists to keep them apart. So are
 * `EBDT`'s grey depths: a grey strike is *listed*, with its depth, and its glyph
 * data declined, because every row here is one bit per pixel.
 *
 * `EBDT`'s **composite** image formats 8 and 9 *are* read, and nothing about them
 * reaches this interface: a composite is a list of other glyphs of the same
 * strike, each OR-ed in at a signed pixel offset, and what comes back is the
 * composed pixels in the box the composite states. A caller cannot tell one from
 * a glyph that stored its rows directly, which is the point. Two of them are
 * refusals rather than compositions, and both are per glyph (M11): a component
 * the strike does not carry, and a component that falls outside the composite's
 * own box, are ::GFNT_ERR_CORRUPT; a composite that reaches itself, through any
 * number of other glyphs, is ::GFNT_ERR_CORRUPT named as a cycle rather than left
 * to exhaust ::GFNT_Limits::max_composite_depth, which is what refuses an acyclic
 * chain nested deeper than the caller allows and reports ::GFNT_ERR_LIMIT.
 *
 * Two things are normalised on the way out, and both are deliberate:
 *
 *  - **Rows run top to bottom and bits run most-significant-first**, one row
 *    starting on a byte boundary. Every one of these formats stores rows that
 *    way, but PCF also stores the *bits* either way round, in scan units of one,
 *    two or four bytes, with rows padded to one, two, four or eight - a
 *    cross-product a caller would have to implement to use the bytes at all. So
 *    the container does it once, where one differential can check it.
 *  - **Every measurement is in pixels**, and there is no em. A bitmap strike is
 *    not an outline scaled down: it was drawn at a size, and asking for its
 *    `unitsPerEm` has no answer (M9). ::gfnt_face_units_per_em() therefore
 *    refuses on a face like this rather than inventing 1000, and the numbers
 *    below are the pixels the font states.
 */

#ifndef GHOTI_IO_GFNT_BITMAP_H
#define GHOTI_IO_GFNT_BITMAP_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One glyph's pixels, and the metrics that position them.
 *
 * `bits` is borrowed from the face and lives as long as the face does; nothing
 * here needs freeing. A glyph with no pixels at all - a space - is `width` or
 * `height` of zero with a `bits` of NULL and an `advance` that still matters.
 *
 * The y convention is section 5.5's, which is the whole library's: y is up, the
 * origin is on the baseline at the pen, and so `bearing_y` is the **top** row's
 * upper edge above the baseline and is positive for a glyph that rises above it.
 * A descender's bottom row is at `bearing_y - height`, which is negative, and
 * that subtraction is the one place a caller can get this wrong.
 */
typedef struct GFNT_BitmapGlyph {
  uint32_t width;       ///< Pixels across. Zero for a glyph with no pixels.
  uint32_t height;      ///< Rows. Zero for a glyph with no pixels.
  size_t stride;        ///< Bytes per row: `(width + 7) / 8` for 1-bit.
  int32_t bearing_x;    ///< Left side bearing in pixels; may be negative.
  int32_t bearing_y;    ///< Top row's edge above the baseline, y-up.
  int32_t advance;      ///< Pen movement in pixels; may differ from `width`.
  uint8_t bit_depth;    ///< Bits per pixel. 1 for every container read today.
  /**
   * The rows, top first, MSB-first within each byte, or NULL for no pixels.
   *
   * Borrowed from the face. `height * stride` bytes; the padding bits at the
   * end of a row are zero, because a container that left them set would make two
   * faces of one design compare unequal.
   */
  const uint8_t * bits;
  GFNT_Strike strike;   ///< Which strike answered, as section 5.3 requires.
} GFNT_BitmapGlyph;

/**
 * @brief This glyph's pixels, from a strike.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param strike Which strike, from 0. A face with one strike takes 0.
 * @param out_glyph Receives the glyph. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for no face, a glyph past the count, or
 *   a strike the face does not have; ::GFNT_ERR_UNSUPPORTED for a face with no
 *   bitmaps this library reads, for a strike whose depth or image format it
 *   declines, and for a glyph **this strike does not carry** - three different
 *   sentences, because a caller can act on each differently;
 *   ::GFNT_ERR_CORRUPT for a glyph whose bitmap the font gets wrong, which is per
 *   glyph (M11) and leaves the rest of the strike answering.
 */
GFNT_API GFNT_Result gfnt_face_glyph_bitmap(const GFNT_Face * face,
    uint32_t glyph, size_t strike, GFNT_BitmapGlyph * out_glyph,
    GFNT_Error * error);

/**
 * @brief One pixel of a bitmap glyph.
 *
 * Reading the rows directly is the fast path and this is the readable one; a
 * caller compositing a run should walk `bits` itself.
 *
 * @param glyph The glyph.
 * @param x Column, from the left.
 * @param y Row, from the **top**, which is how the rows are stored.
 * @return 0 or 255 for a 1-bit glyph, so that the value composites like
 *   coverage; 0 for a coordinate outside the bitmap, which is what a caller
 *   drawing a box around a glyph reads and is not an error.
 */
GFNT_API uint8_t gfnt_bitmap_pixel(const GFNT_BitmapGlyph * glyph, uint32_t x,
    uint32_t y);

/**
 * @brief Write a bitmap glyph as text, for the differentials and the golden gate.
 *
 * One header line, then one line per row with `#` for a set pixel and `.` for a
 * clear one. The same shape ::gfnt_coverage_dump_art() writes, so that a bitmap
 * strike and a rasterised outline can be read side by side.
 *
 * @param glyph The glyph.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_bitmap_dump(const GFNT_BitmapGlyph * glyph,
    FILE * out);

/**
 * @brief Which codepoints a bitmap container maps to a glyph, and how many.
 *
 * A standalone bitmap font has no `cmap`: PCF carries a `BDF_ENCODINGS` table,
 * BDF an `ENCODING` per character, PSF an optional Unicode table where one glyph
 * can answer several codepoints and a sequence, and `.hex` is keyed by codepoint
 * outright. ::gfnt_face_glyph_for_codepoint() answers from whichever of those the
 * face has, so a caller needs none of this; it is here for enumeration, which a
 * `cmap` offers and these formats otherwise would not.
 *
 * @param face The face.
 * @param out_count Receives how many (codepoint, glyph) pairs the face states.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID; ::GFNT_ERR_UNSUPPORTED for a face whose
 *   mapping is a `cmap`, which `cmap.h` enumerates, or for a bitmap font that
 *   states no mapping at all - PSF without its Unicode table, whose glyph
 *   indices are console positions and not characters.
 */
GFNT_API GFNT_Result gfnt_face_bitmap_encoding_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/**
 * @brief The nth (codepoint, glyph) pair of a bitmap container's encoding.
 *
 * Sorted by codepoint, so a caller can page through them or bisect. A glyph
 * named by several codepoints appears once per codepoint.
 *
 * @param face The face.
 * @param index Which pair, from 0.
 * @param out_codepoint Receives the codepoint, or NULL.
 * @param out_glyph Receives the glyph index, or NULL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID or ::GFNT_ERR_UNSUPPORTED.
 */
GFNT_API GFNT_Result gfnt_face_bitmap_encoding_at(const GFNT_Face * face,
    size_t index, uint32_t * out_codepoint, uint32_t * out_glyph,
    GFNT_Error * error);

/**
 * @brief How an encoded glyph image is stored.
 *
 * `CBDT` holds PNG or, at a depth of 32, raw BGRA. `sbix` names its format per glyph, and the three it
 * defines for pictures are here; `dupe` (another glyph's image) is followed and
 * never surfaces, and `mask` is declined.
 */
typedef enum {
  GFNT_IMAGE_PNG = 0,   ///< A PNG file.
  GFNT_IMAGE_JPEG,      ///< A JPEG file, which `sbix` `jpg ` names.
  GFNT_IMAGE_TIFF,      ///< A TIFF file, which `sbix` `tiff` names.
  /**
   * Rows of premultiplied sRGB pixels, four bytes each in the order B, G, R, A, with
   * no padding: what `CBDT` stores for image formats 1, 2, 5, 6 and 7 in a strike of
   * 32 bits. `width` and `height` are the metrics', and `length` is `width * height * 4`.
   */
  GFNT_IMAGE_BGRA
} GFNT_ImageFormat;

/**
 * @brief One color glyph as the font stores it: an encoded picture and where it sits.
 *
 * Borrowed from the face, like ::GFNT_BitmapGlyph: `data` lives as long as the
 * face does and nothing here needs freeing. The picture is the font's own bytes,
 * untouched, so a caller with a decoder of its own (a GPU upload path, say) can
 * take them as they are; ::gfnt_face_glyph_color_bitmap() decodes them.
 *
 * Positions are in **pixels of the strike**, y up, from the pen on the baseline -
 * the same convention ::GFNT_BitmapGlyph states. `bearing_y` is the top edge.
 */
typedef struct GFNT_GlyphImage {
  GFNT_ImageFormat format; ///< What `data` is.
  const uint8_t * data;    ///< The file, borrowed from the face.
  size_t length;           ///< Bytes in `data`.
  /**
   * The picture's size as the **font** states it, or 0 when it states none.
   *
   * `CBDT` states it in every glyph's metrics. `sbix` does not, so for a PNG it
   * is read from the file's header and for the other two formats it is 0, since
   * learning it means decoding.
   */
  uint32_t width;
  uint32_t height;
  int32_t bearing_x;       ///< Left edge of the picture from the pen; may be negative.
  int32_t bearing_y;       ///< Top edge of the picture above the baseline; see `origin_y`.
  /**
   * The bottom edge of the picture above the baseline.
   *
   * `sbix` states this one (its origin offset) and not the top, so `bearing_y` is
   * `origin_y + height` and is only the top when `height` is not 0 - a JPEG or TIFF
   * does not say how tall it is until it is decoded. `CBDT` states the top, and
   * `origin_y` is `bearing_y - height`.
   */
  int32_t origin_y;
  /**
   * The pen movement in pixels, or -1 when the table states none.
   *
   * `CBDT` states one per glyph. `sbix` states none: a glyph's advance is the
   * `hmtx` one, in font units, which a caller scales by the strike.
   */
  int32_t advance;
  GFNT_Strike strike;      ///< Which strike answered.
} GFNT_GlyphImage;

/**
 * @brief This glyph's encoded picture, from a color strike.
 *
 * Reads `CBDT` (image formats 17, 18 and 19) and `sbix`, following an `sbix` `dupe` to
 * the picture it names (eight hops at most). Finding one glyph costs its strike's index
 * subtables and one glyph's bytes; nothing is decoded, and no
 * strike is read whole.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param strike Which strike, from 0; ::gfnt_face_strike_at() says which are color
 *   (::GFNT_GLYPH_BITMAP_PNG).
 * @param out_image Receives the picture. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for no face, a glyph past the count or a
 *   strike the face does not have; ::GFNT_ERR_UNSUPPORTED for a strike that is not
 *   a color one, for an image format this library declines, and for a glyph
 *   **this strike does not carry** (another strike may); ::GFNT_ERR_CORRUPT for a
 *   glyph whose bytes the font gets wrong, per glyph.
 */
GFNT_API GFNT_Result gfnt_face_glyph_image(const GFNT_Face * face,
    uint32_t glyph, size_t strike, GFNT_GlyphImage * out_image,
    GFNT_Error * error);

/**
 * @brief A color glyph decoded to pixels: four channels, straight alpha unless said.
 *
 * The depth is the picture's own: `bits_per_channel` is 8 for the usual 8-bit PNG and
 * for BGRA strikes, and 16 for a 16-bit PNG, whose samples are `uint16_t` in the
 * machine's byte order (so `stride` is `width * 8`). Nothing is narrowed on the way
 * out; ::gfnt_color_bitmap_to_8bit() does it when a caller wants 8 bits. Channels are
 * always R, G, B, A in that order, a grey picture repeated into the three colors.
 *
 * `premultiplied` is false for a PNG (straight alpha, as the file stores it, so that
 * nothing is lost to rounding) and true for the uncompressed BGRA strikes `CBDT`
 * defines, whose color channels are already multiplied by alpha. Rows run top first.
 * Unlike ::GFNT_BitmapGlyph this is **owned by the caller**: decoding makes the
 * pixels, and a face that cached every decoded glyph would hold a few hundred
 * kilobytes for each one a program ever drew.
 */
typedef struct GFNT_ColorBitmap {
  uint32_t width;          ///< Pixels across, from the decoded picture.
  uint32_t height;         ///< Rows.
  size_t stride;           ///< Bytes per row: `width * 4 * (bits_per_channel / 8)`.
  int32_t bearing_x;       ///< As ::GFNT_GlyphImage::bearing_x.
  int32_t bearing_y;       ///< As ::GFNT_GlyphImage::bearing_y.
  int32_t advance;         ///< As ::GFNT_GlyphImage::advance.
  uint8_t bits_per_channel; ///< 8 or 16.
  bool premultiplied;      ///< Whether the color channels are multiplied by alpha.
  uint8_t * rgba;          ///< `height * stride` bytes of R, G, B, A samples.
  GFNT_Strike strike;      ///< Which strike answered.
  const GFNT_Allocator * allocator; ///< What ::gfnt_color_bitmap_free() frees with.
} GFNT_ColorBitmap;

/**
 * @brief This glyph's picture, decoded.
 *
 * ::gfnt_face_glyph_image() and then a decode by `image`, so the errors are that
 * function's, plus ::GFNT_ERR_CORRUPT for a picture that does not decode,
 * ::GFNT_ERR_LIMIT for one that would take more than
 * ::GFNT_Limits::max_raster_bytes, and ::GFNT_ERR_OOM.
 *
 * @param out_bitmap Receives the glyph. Written only on success; release it with
 *   ::gfnt_color_bitmap_free().
 */
GFNT_API GFNT_Result gfnt_face_glyph_color_bitmap(const GFNT_Face * face,
    uint32_t glyph, size_t strike, GFNT_ColorBitmap * out_bitmap,
    GFNT_Error * error);

/**
 * @brief Narrow a 16-bit bitmap to 8 bits in place.
 *
 * Each sample becomes its high byte, which is what `image` does
 * (`gimg_ops_convert_bit_depth`) so the two libraries narrow alike; the result is
 * not rounded. A bitmap already 8 bits is left alone. `stride` shrinks to match.
 *
 * @param bitmap A bitmap from ::gfnt_face_glyph_color_bitmap().
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID for no bitmap.
 */
GFNT_API GFNT_Result gfnt_color_bitmap_to_8bit(GFNT_ColorBitmap * bitmap);

/** @brief Release a ::GFNT_ColorBitmap's pixels. Harmless on a zeroed one. */
GFNT_API void gfnt_color_bitmap_free(GFNT_ColorBitmap * bitmap);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_BITMAP_H
