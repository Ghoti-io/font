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
 * Color glyphs as a scene draws them (story 14c of the shape family's first
 * milestone): a `CPAL` palette as a `shape-scene` palette resource, and a
 * glyph's `COLR` lowered into a list the caller supplies, through
 * `shape-scene`'s float-free calls (`<ghoti.io/shape-scene/fixed.h>`).
 * documentation/design.md section 8.8.
 *
 * **The palette.** ::gfnt_scene_intern_palette interns one of the face's `CPAL`
 * palettes, its 8-bit straight sRGB entries as they are stored. The caller sets
 * it on the list (or on a version) that draws the glyphs; a theme is another
 * palette, never another lowering. An entry the palette in force lacks is
 * refused by every sink when it draws (`shape-scene`'s N14).
 *
 * **The lowering.** ::gfnt_scene_record_color_glyph records a glyph's color
 * description into a list: its `COLR` version 1 paint graph where the glyph has
 * one, else its version 0 layers. Every shape is a one-glyph run of the face
 * resource at the run's size and origin, so the provider draws it like any
 * glyph (hinted where the run asks and the route allows).
 *
 * - Version 0: each layer, bottom first, is a fill of its glyph with its
 *   palette entry; `0xFFFF` is the foreground.
 * - Version 1: `PaintColrLayers` lowers its layers in order; `PaintGlyph` over a
 *   solid (through any transforms between them, which do not move a solid) is a
 *   fill, and over anything else a clip push of the glyph around its child;
 *   `PaintSolid` is the palette paint, its alpha F2.14 clamped to `[0, 1]`;
 *   `PaintColrGlyph` is the other glyph's graph; the transforms (`PaintTransform`,
 *   `PaintTranslate`, the four scales, the two rotations, the two skews) are the
 *   design-map ops of the shapes beneath them; `PaintComposite` with a
 *   Porter-Duff mode (0 to 12, the scene's thirteen operators) is a group
 *   (isolated, source-over) of the backdrop, then a group of that operator around
 *   the source.
 *
 * **What is refused, per glyph and with a reason** (::GFNT_ColorRefusal), and
 * nothing recorded: the gradients (linear, radial, sweep; the scene has none
 * until the shape family's step 4), the blend modes (`PaintComposite` modes 13 to
 * 27; the spine's row on blend modes), the variable forms (`PaintVar*`), a solid
 * no `PaintGlyph` bounds directly (it would fill the plane), and a
 * `PaintColrGlyph` naming a glyph with no version 1 record. The call returns
 * ::GFNT_ERR_UNSUPPORTED and the reason; the caller decides what to draw instead
 * (the glyph's own outline, for one). A lowering is not a render, so a refusal
 * is the call's answer and not a report entry.
 *
 * **All or nothing.** The list is read for its count first and truncated back
 * to it on a refusal or any failure, so it holds the whole glyph or none of it.
 *
 * **Not applied:** the `ClipList`'s clip boxes. Everything lowered is bounded by
 * a glyph, so a box changes nothing a font that states a box covering its
 * glyphs draws; the spine's Deferred table has the row.
 *
 * No floating type appears in this header or in any header it includes (AD-5,
 * `make check-float-free`), nor in src/scene/colr.c.
 */

#ifndef GHOTI_IO_GFNT_SCENE_COLOR_H
#define GHOTI_IO_GFNT_SCENE_COLOR_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/scene.h>
#include <ghoti.io/shape-scene/fixed.h>
#include <ghoti.io/shape-scene/glyph_provider.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Which description a lowering used. */
typedef enum GFNT_ColorSource {
  GFNT_COLOR_SOURCE_NONE = 0, ///< The glyph has no color description; nothing recorded.
  GFNT_COLOR_SOURCE_V0 = 1,   ///< `COLR` version 0 layers.
  GFNT_COLOR_SOURCE_V1 = 2    ///< A `COLR` version 1 paint graph.
} GFNT_ColorSource;

/** @brief Why a glyph's color description was not lowered. */
typedef enum GFNT_ColorRefusal {
  GFNT_COLOR_LOWERED = 0,             ///< Not refused.
  GFNT_COLOR_REFUSED_GRADIENT = 1,    ///< A linear, radial or sweep gradient.
  GFNT_COLOR_REFUSED_BLEND_MODE = 2,  ///< A `PaintComposite` mode past `PLUS` (12): a blend mode.
  GFNT_COLOR_REFUSED_VARIABLE = 3,    ///< A variable paint (`PaintVar*`).
  GFNT_COLOR_REFUSED_UNBOUNDED = 4,   ///< A solid no `PaintGlyph` bounds directly.
  GFNT_COLOR_REFUSED_NO_RECORD = 5,   ///< A `PaintColrGlyph` naming a glyph with no version 1 record.
  GFNT_COLOR_REFUSAL_COUNT            ///< Closes the enum.
} GFNT_ColorRefusal;

/** @brief Where and how a color glyph is drawn: every number fixed point, `v / 2^bits`. */
typedef struct GFNT_ColorRun {
  int32_t size;          ///< The em size in list units; greater than zero.
  int32_t x;             ///< The glyph's origin in list space.
  int32_t y;             ///< Likewise; down is positive.
  uint32_t bits;         ///< The fraction bits of the three, 0 to 16.
  uint32_t hint_mode;    ///< A `GSHSCENE_Hint_Mode` for every shape's run.
  uint32_t hint_version; ///< Passed to the provider unread.
  int32_t layer;         ///< The declared layer of every item recorded.
} GFNT_ColorRun;

/** @brief What a lowering did. */
typedef struct GFNT_ColorLowering {
  uint32_t source;         ///< A ::GFNT_ColorSource.
  uint32_t refusal;        ///< A ::GFNT_ColorRefusal.
  uint32_t paint_format;   ///< The refused paint's `GFNT_PaintFormat` (version 1), or 0.
  uint32_t composite_mode; ///< A refused blend mode, as `COLR` numbers it; else 0.
  size_t items;            ///< The items recorded (0 when refused).
} GFNT_ColorLowering;

/**
 * @brief Intern one of a face's `CPAL` palettes as a palette resource.
 *
 * @param face A face resource bound to this library's provider
 *   (::gfnt_scene_intern_face).
 * @param store The store of the lists or the scene that will draw with it.
 * @param palette The palette, from 0.
 * @param out_palette Receives the palette, with a reference the caller releases
 *   (`gshscene_resource_release`). Written only on success.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID (NULL, a face not bound by this library,
 *   a palette past the count);
 *   ::GFNT_ERR_UNSUPPORTED (no `CPAL`); ::GFNT_ERR_CORRUPT; ::GFNT_ERR_LIMIT;
 *   ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_scene_intern_palette(const GSHSCENE_Resource * face,
    GSHSCENE_Store * store, size_t palette, const GSHSCENE_Resource ** out_palette,
    GFNT_Error * error);

/**
 * @brief Lower a glyph's color description into @p list (see the file comment).
 *
 * @param face A face resource bound to this library's provider.
 * @param store The store of the face and of @p list.
 * @param list An open list of @p store.
 * @param glyph The base glyph.
 * @param run Its size, origin, hinting and layer.
 * @param out Receives what was done; written on success and on a refusal.
 * @return ::GFNT_OK (lowered, or no color description: see `out->source`);
 *   ::GFNT_ERR_UNSUPPORTED for a refusal (`out->refusal`), nothing recorded;
 *   ::GFNT_ERR_INVALID (NULL, a face not bound by this library, a closed list or
 *   one of another store, a run outside its rules); ::GFNT_ERR_LIMIT (a graph
 *   past the walk's bounds, a list or store limit); ::GFNT_ERR_CORRUPT;
 *   ::GFNT_ERR_OOM. On every failure the list is as it was.
 */
GFNT_API GFNT_Result gfnt_scene_record_color_glyph(const GSHSCENE_Resource * face,
    GSHSCENE_Store * store, GSHSCENE_List * list, uint32_t glyph,
    const GFNT_ColorRun * run, GFNT_ColorLowering * out, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_SCENE_COLOR_H
