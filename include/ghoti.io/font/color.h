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
 * Color: `CPAL` palettes, `COLR` version 0 layers and `COLR` version 1 paint graphs.
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
 * **Version 1 is a graph, and this library does not draw it.** A base glyph's
 * paint is the root of a directed acyclic graph of solid and gradient fills,
 * glyph shapes to clip them to, affine transforms and Porter-Duff composites.
 * ::gfnt_face_color_paint_walk() visits it depth first, bounded in depth and in
 * nodes, and each node comes back as a ::GFNT_ColorPaint of plain numbers in the
 * file's own units; the caller's compositor turns them into pixels. Where a
 * paint is one of the variable forms its numbers have the item variation store's
 * deltas for the given location added, in the file's own units, and are not
 * clamped: an alpha past 1.0 is the compositor's to clamp.
 *
 * Reference: OpenType Specification 1.9, "COLR" and "CPAL".
 */

#ifndef GHOTI_IO_GFNT_COLOR_H
#define GHOTI_IO_GFNT_COLOR_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/variation.h>
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

/* ---------------------------------------------------------- COLR version 1 */

/** @brief A paint table in `COLR`, by its offset from the start of the table. 0 is none. */
typedef uint32_t GFNT_PaintRef;

/** @brief The paint formats, numbered as the specification numbers them. */
typedef enum GFNT_PaintFormat {
  GFNT_PAINT_COLR_LAYERS = 1,        ///< A run of paints from the layer list, composited source-over.
  GFNT_PAINT_SOLID = 2,              ///< One palette color with an alpha.
  GFNT_PAINT_VAR_SOLID = 3,          ///< As 2, alpha varying.
  GFNT_PAINT_LINEAR_GRADIENT = 4,    ///< A gradient along a line, with a rotation point.
  GFNT_PAINT_VAR_LINEAR_GRADIENT = 5,
  GFNT_PAINT_RADIAL_GRADIENT = 6,    ///< A gradient between two circles.
  GFNT_PAINT_VAR_RADIAL_GRADIENT = 7,
  GFNT_PAINT_SWEEP_GRADIENT = 8,     ///< A gradient swept around a center.
  GFNT_PAINT_VAR_SWEEP_GRADIENT = 9,
  GFNT_PAINT_GLYPH = 10,             ///< Fill a glyph's outline with the child paint.
  GFNT_PAINT_COLR_GLYPH = 11,        ///< Paint as another base glyph does.
  GFNT_PAINT_TRANSFORM = 12,         ///< The child under a 2x3 affine matrix.
  GFNT_PAINT_VAR_TRANSFORM = 13,
  GFNT_PAINT_TRANSLATE = 14,
  GFNT_PAINT_VAR_TRANSLATE = 15,
  GFNT_PAINT_SCALE = 16,
  GFNT_PAINT_VAR_SCALE = 17,
  GFNT_PAINT_SCALE_AROUND_CENTER = 18,
  GFNT_PAINT_VAR_SCALE_AROUND_CENTER = 19,
  GFNT_PAINT_SCALE_UNIFORM = 20,
  GFNT_PAINT_VAR_SCALE_UNIFORM = 21,
  GFNT_PAINT_SCALE_UNIFORM_AROUND_CENTER = 22,
  GFNT_PAINT_VAR_SCALE_UNIFORM_AROUND_CENTER = 23,
  GFNT_PAINT_ROTATE = 24,
  GFNT_PAINT_VAR_ROTATE = 25,
  GFNT_PAINT_ROTATE_AROUND_CENTER = 26,
  GFNT_PAINT_VAR_ROTATE_AROUND_CENTER = 27,
  GFNT_PAINT_SKEW = 28,
  GFNT_PAINT_VAR_SKEW = 29,
  GFNT_PAINT_SKEW_AROUND_CENTER = 30,
  GFNT_PAINT_VAR_SKEW_AROUND_CENTER = 31,
  GFNT_PAINT_COMPOSITE = 32          ///< The source composited onto the backdrop by a mode.
} GFNT_PaintFormat;

/**
 * @brief One paint node, as numbers in the file's own units.
 *
 * Which fields mean what, by format (a value in the file's FWORD is in font
 * units; F2Dot14 is in 1/16384, an angle in 1/16384 of a half turn, a scale in
 * 1/16384; Fixed is 16.16):
 *
 *   * ::GFNT_PAINT_COLR_LAYERS: @p first_layer, @p layer_count - the layer list's
 *     entries from @p first_layer, bottom to top; see ::gfnt_face_color_layer_paint().
 *   * SOLID: @p palette_index (::GFNT_COLOR_FOREGROUND allowed), @p v[0] the alpha.
 *   * LINEAR_GRADIENT: @p color_line; v[0..5] the points x0 y0 x1 y1 x2 y2.
 *   * RADIAL_GRADIENT: @p color_line; v[0..5] x0 y0 r0 x1 y1 r1.
 *   * SWEEP_GRADIENT: @p color_line; v[0..3] center x, center y, start angle, end angle.
 *   * GLYPH: @p glyph and @p child. COLR_GLYPH: @p glyph.
 *   * TRANSFORM: @p child; v[0..5] xx yx xy yy dx dy, in Fixed.
 *   * TRANSLATE: v[0..1] dx dy. SCALE: sx sy. SCALE_AROUND_CENTER: sx sy cx cy.
 *     SCALE_UNIFORM: s. SCALE_UNIFORM_AROUND_CENTER: s cx cy. ROTATE: angle.
 *     ROTATE_AROUND_CENTER: angle cx cy. SKEW: x skew, y skew. SKEW_AROUND_CENTER:
 *     x skew, y skew, cx, cy. Each has @p child.
 *   * COMPOSITE: @p child the source, @p backdrop, @p composite_mode as the
 *     specification numbers it (0 clear through 27 hard luminosity).
 */
typedef struct GFNT_ColorPaint {
  GFNT_PaintFormat format;     ///< Which paint.
  bool variable;               ///< True for the Var forms; their @p v already include the deltas.
  GFNT_PaintRef child;         ///< The paint beneath, or the source of a composite; else 0.
  GFNT_PaintRef backdrop;      ///< A composite's backdrop; else 0.
  GFNT_PaintRef color_line;    ///< The gradient's color line; else 0.
  uint32_t glyph;              ///< PaintGlyph's or PaintColrGlyph's glyph.
  uint32_t first_layer;        ///< PaintColrLayers: first layer-list index.
  uint32_t layer_count;        ///< PaintColrLayers: how many.
  uint16_t palette_index;      ///< PaintSolid's color.
  uint8_t composite_mode;      ///< PaintComposite's mode.
  int32_t v[6];                ///< The numbers; see above.
} GFNT_ColorPaint;

/** @brief How a gradient continues past its first and last stops. */
typedef enum GFNT_ColorExtend {
  GFNT_COLOR_EXTEND_PAD = 0,
  GFNT_COLOR_EXTEND_REPEAT = 1,
  GFNT_COLOR_EXTEND_REFLECT = 2
} GFNT_ColorExtend;

/** @brief A gradient's color line. */
typedef struct GFNT_ColorLine {
  GFNT_ColorExtend extend;     ///< Beyond the ends.
  uint16_t stop_count;         ///< How many stops.
} GFNT_ColorLine;

/** @brief One stop of a color line. */
typedef struct GFNT_ColorStop {
  int32_t offset;              ///< F2Dot14: where on the line, 0 to 16384 and not clamped.
  uint16_t palette_index;      ///< The color.
  int32_t alpha;               ///< F2Dot14.
} GFNT_ColorStop;

/** @brief A glyph's clip box, in font units. */
typedef struct GFNT_ColorBox {
  int32_t x_min;
  int32_t y_min;
  int32_t x_max;
  int32_t y_max;
} GFNT_ColorBox;

/**
 * @brief The root paint of a base glyph's graph.
 *
 * @param out_ref Receives the paint, or 0 when the glyph has no version 1 record
 *   (including every glyph of a version 0 table). Written only on success.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID; ::GFNT_ERR_UNSUPPORTED for a face with
 *   no `COLR`; ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_color_base_paint(const GFNT_Face * face,
    uint32_t glyph, GFNT_PaintRef * out_ref, GFNT_Error * error);

/** @brief How many paints the layer list holds; 0 when there is none. */
GFNT_API GFNT_Result gfnt_face_color_layer_list_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/** @brief The paint at an index of the layer list. */
GFNT_API GFNT_Result gfnt_face_color_layer_paint(const GFNT_Face * face,
    size_t index, GFNT_PaintRef * out_ref, GFNT_Error * error);

/**
 * @brief Read one paint node.
 *
 * @param variation The location, for the variable forms, or NULL for the default.
 *   A face with no item variation store reads them at their defaults.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a NULL argument or a ref of 0;
 *   ::GFNT_ERR_CORRUPT for a ref outside the table, a child offset of 0 where
 *   one is required, or a store that contradicts itself; ::GFNT_ERR_UNSUPPORTED
 *   for a paint format this library does not know.
 */
GFNT_API GFNT_Result gfnt_face_color_paint(const GFNT_Face * face,
    GFNT_PaintRef ref, const GFNT_Variation * variation,
    GFNT_ColorPaint * out_paint, GFNT_Error * error);

/** @brief The color line of a gradient paint. */
GFNT_API GFNT_Result gfnt_face_color_line(const GFNT_Face * face,
    const GFNT_ColorPaint * paint, GFNT_ColorLine * out_line, GFNT_Error * error);

/** @brief One stop of a gradient paint's color line, with variable deltas added. */
GFNT_API GFNT_Result gfnt_face_color_stop(const GFNT_Face * face,
    const GFNT_ColorPaint * paint, size_t index, const GFNT_Variation * variation,
    GFNT_ColorStop * out_stop, GFNT_Error * error);

/**
 * @brief A base glyph's clip box, if the clip list gives one.
 *
 * @param out_found Receives false when no clip covers the glyph.
 */
GFNT_API GFNT_Result gfnt_face_color_clip(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, GFNT_ColorBox * out_box, bool * out_found,
    GFNT_Error * error);

/** @brief What ::gfnt_face_color_paint_walk() calls. */
typedef struct GFNT_PaintVisitor {
  /**
   * Called on entering a node; @p depth is 0 at the root. A layers node's
   * children are the layer-list paints it names, and a COLR_GLYPH node's child is
   * the root paint of the glyph it names. Return false to stop the walk, which
   * then returns ::GFNT_ERR_INVALID.
   */
  bool (*enter)(void * user, const GFNT_ColorPaint * paint, size_t depth);
  /** Called on leaving a node, or NULL. */
  void (*leave)(void * user, const GFNT_ColorPaint * paint, size_t depth);
} GFNT_PaintVisitor;

/** @brief The most nodes ::gfnt_face_color_paint_walk() visits in one walk. */
#define GFNT_PAINT_WALK_NODES 65536u

/**
 * @brief Walk a base glyph's paint graph depth first.
 *
 * Bounded twice, because a graph shares nodes and can name itself: by depth
 * (`GFNT_Limits::max_paint_depth`, 64) and by ::GFNT_PAINT_WALK_NODES, so that a
 * graph that fans out is refused rather than walked for hours.
 *
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a glyph with no version 1 record or a
 *   visitor that stopped; ::GFNT_ERR_LIMIT when either bound is passed;
 *   the errors of ::gfnt_face_color_paint().
 */
GFNT_API GFNT_Result gfnt_face_color_paint_walk(const GFNT_Face * face,
    uint32_t glyph, const GFNT_Variation * variation,
    const GFNT_PaintVisitor * visitor, void * user, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_COLOR_H
