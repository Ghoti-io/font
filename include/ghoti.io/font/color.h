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
 * Color: `CPAL` palettes and `COLR` version 0 layers.
 *
 * documentation/design.md section 7.6. A color glyph is a stack of ordinary
 * outline glyphs, each painted in one color of a palette. `COLR` lists the
 * layers of a base glyph, bottom to top, each a glyph ID and a palette index;
 * `CPAL` holds the palettes those indexes choose from. This library returns the
 * description and never composites it: draw each layer's outline
 * (::gfnt_face_glyph_outline()) in the color the layer names.
 *
 * Both tables are read in place at each call, so nothing here allocates. Colors
 * are straight (not premultiplied) 8-bit sRGB with alpha, as `CPAL` stores them.
 *
 * Reference: OpenType Specification 1.9, "COLR" and "CPAL".
 */

#ifndef GHOTI_IO_GFNT_COLOR_H
#define GHOTI_IO_GFNT_COLOR_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A layer's palette index meaning "the text color", not a palette entry. */
#define GFNT_COLOR_FOREGROUND 0xFFFFu
/** @brief A palette's or an entry's label when it has none. */
#define GFNT_PALETTE_NO_LABEL 0xFFFFu

/** @brief One color: straight RGBA, 8 bits a channel. */
typedef struct GFNT_Color {
  uint8_t red;     ///< Red.
  uint8_t green;   ///< Green.
  uint8_t blue;    ///< Blue.
  uint8_t alpha;   ///< Alpha; 255 is opaque.
} GFNT_Color;

/** @brief ::GFNT_PaletteInfo::flags bit 0: the palette suits a light background. */
#define GFNT_PALETTE_USABLE_ON_LIGHT 0x0001u
/** @brief ::GFNT_PaletteInfo::flags bit 1: the palette suits a dark background. */
#define GFNT_PALETTE_USABLE_ON_DARK 0x0002u

/** @brief What a `CPAL` version 1 says about one palette. */
typedef struct GFNT_PaletteInfo {
  uint32_t flags;   ///< ::GFNT_PALETTE_USABLE_ON_LIGHT and _DARK; 0 for a version 0 table.
  uint16_t label;   ///< A `name` ID for the palette, or ::GFNT_PALETTE_NO_LABEL.
} GFNT_PaletteInfo;

/** @brief One layer of a `COLR` version 0 glyph. */
typedef struct GFNT_ColorLayer {
  uint32_t glyph;          ///< The glyph to draw, whose outline is the layer's shape.
  uint16_t palette_index;  ///< The color: an entry of the chosen palette, or ::GFNT_COLOR_FOREGROUND.
} GFNT_ColorLayer;

/** @brief Whether the face has a `CPAL` table. */
GFNT_API bool gfnt_face_has_palettes(const GFNT_Face * face);

/** @brief Whether the face has a `COLR` table. */
GFNT_API bool gfnt_face_has_color_layers(const GFNT_Face * face);

/**
 * @brief How many palettes the face has.
 *
 * @param out_count Receives the count. Written only on success.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED for a face with
 *   no `CPAL` or a version above 1, or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_palette_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/** @brief How many colors every palette holds (they all hold the same number). */
GFNT_API GFNT_Result gfnt_face_palette_entry_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/**
 * @brief One color of one palette.
 *
 * @param palette The palette, from 0.
 * @param entry The entry, from 0.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a NULL argument or an index out of
 *   range; ::GFNT_ERR_CORRUPT when the table's record array does not hold it.
 */
GFNT_API GFNT_Result gfnt_face_palette_color(const GFNT_Face * face,
    size_t palette, size_t entry, GFNT_Color * out_color, GFNT_Error * error);

/** @brief The type flags and label of a palette; zero flags and no label for `CPAL` 0. */
GFNT_API GFNT_Result gfnt_face_palette_info(const GFNT_Face * face,
    size_t palette, GFNT_PaletteInfo * out_info, GFNT_Error * error);

/**
 * @brief The `name` ID that labels a palette entry, such as "Skin tone".
 *
 * @param out_label Receives the ID, or ::GFNT_PALETTE_NO_LABEL.
 */
GFNT_API GFNT_Result gfnt_face_palette_entry_label(const GFNT_Face * face,
    size_t entry, uint16_t * out_label, GFNT_Error * error);

/**
 * @brief The `COLR` version, 0 or 1.
 *
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED for a face with
 *   no `COLR` or a version above 1, or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_color_version(const GFNT_Face * face,
    uint16_t * out_version, GFNT_Error * error);

/**
 * @brief How many version 0 layers a glyph has.
 *
 * A glyph with no base glyph record has none, which is not an error: most
 * glyphs of a color font are not color glyphs. A version 1 table's version 0
 * records are read the same way.
 */
GFNT_API GFNT_Result gfnt_face_color_layer_count(const GFNT_Face * face,
    uint32_t glyph, size_t * out_count, GFNT_Error * error);

/**
 * @brief One layer of a glyph, from the bottom (0) up.
 *
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for an index at or past the count;
 *   ::GFNT_ERR_CORRUPT when the layer record array does not hold the layer.
 */
GFNT_API GFNT_Result gfnt_face_color_layer(const GFNT_Face * face,
    uint32_t glyph, size_t index, GFNT_ColorLayer * out_layer,
    GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_COLOR_H
