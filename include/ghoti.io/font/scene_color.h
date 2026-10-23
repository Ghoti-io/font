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
 * glyph (hinted where the run asks and the route allows). The rules are
 * numbered L1 to L13 here and in documentation/design.md section 8.8, which
 * quotes the OpenType sentence beside each.
 *
 * - Version 0: each layer, bottom first, is a fill of its glyph with its
 *   palette entry; `0xFFFF` is the foreground. No group: CPAL and `COLR`
 *   version 0 state no space.
 * - Version 1 is, in order, one isolated source-over group in the blend space
 *   COLRv1 composites in (L8; ::GFNT_COLOR_SPACE_STANDARD, linear light, unless
 *   ::GFNT_ColorRun::space asks for the encoded one), around one clip of the
 *   base glyph's `ClipList` box where it has one (L5; so does a `PaintColrGlyph`
 *   whose target has a box, under the transforms at that node), around the graph: `PaintColrLayers` lowers its
 *   layers in order; a `PaintGlyph` whose child reaches a solid or a gradient
 *   through transforms is a fill of the glyph (the transforms above it its map,
 *   the ones between it and a gradient the gradient's, L4), and over anything
 *   else a clip of the glyph around its child; `PaintSolid` is the palette
 *   paint, its alpha F2.14 clamped to `[0, 1]`; the three gradients are gradient
 *   resources with their color lines (L2), interpolated premultiplied in linear
 *   light unless the run asks for the encoded space (L3); a solid or gradient
 *   no `PaintGlyph` bounds directly fills the plane under the clips in force
 *   (L6); `PaintColrGlyph` is the other glyph's graph; the transforms are the
 *   design-map ops of the shapes beneath them; `PaintComposite` is an isolated
 *   group of the backdrop, then a group of the operator around the source:
 *   modes 0 to 12 are the Porter-Duff operators, 13 to 27 the blend modes, any
 *   other value clear (L7).
 * - Every `Var*` number is read at the instance the face resource was interned
 *   at (L1); there is no second location, so the outline and the paint cannot
 *   disagree about where the font is.
 *
 * **What is refused, per glyph and with a reason** (::GFNT_ColorRefusal): a solid
 * or gradient no `PaintGlyph` bounds in a glyph that states no clip box (the
 * lowering never guesses an extent the font did not state), a `PaintColrGlyph`
 * naming a glyph with no version 1 record, and a graph past the walk's depth or
 * node bound (a glyph that names itself, which OpenType forbids; the bound
 * itself is unchanged, so a hostile font still cannot make the call allocate
 * without bound). A refusal is a property of the glyph, and it must reach the
 * render report, where an application that draws the list will look for it
 * (shape-scene's placeholder item, PH1 to PH6). What the call records for it (L12, L13) is
 * ::GFNT_ColorRun::refusal_policy (P5):
 *
 * - ::GFNT_COLOR_ON_REFUSAL_PLACEHOLDER (0, the default): a placeholder item
 *   (source `COLR`, the refusal as its code, ::GFNT_ColorRefusal, `approximated`
 *   0) and nothing else. The call returns ::GFNT_OK; `GFNT_ColorLowering::outcome`
 *   says a placeholder is in the list and `refusal` why. Every sink reports it
 *   `REFUSED`, and a strict render fails on it.
 * - ::GFNT_COLOR_ON_REFUSAL_FALLBACK (1, opt in): the placeholder marked
 *   approximated and, right after it, one fill of the base glyph's own outline in
 *   the foreground paint, so the glyph draws as a monochrome one and the report
 *   says `APPROXIMATED` (which never fails a render). A base glyph with no
 *   outline has nothing to draw in its place, so it gets a `REFUSED` placeholder
 *   and no fill: the report never says a glyph is drawn when nothing is.
 * - ::GFNT_COLOR_ON_REFUSAL_NONE (2): the call returns ::GFNT_ERR_UNSUPPORTED
 *   and the reason, and records nothing, for an application that supplies its
 *   own substitute (a bitmap strike, for one).
 *
 * The placeholder's box (PH11) is, in this order, the glyph's `ClipList` box at
 * the face resource's instance; else the glyph's color extents (the box layout
 * places marks by); else the base glyph's outline control box; else none. It is
 * in the font's design units and placed at the run's origin and size, so an
 * application that wants a missing-glyph box or a hatch draws it from the item.
 *
 * These are refusals of the glyph. The failures of the call stay errors, with
 * the list as it was: ::GFNT_ERR_OOM, a list or store limit (::GFNT_ERR_LIMIT),
 * ::GFNT_ERR_CORRUPT and ::GFNT_ERR_INVALID. A hostile font therefore cannot
 * make a corrupt table a quiet gap.
 *
 * **All or nothing.** The list is read for its count first and truncated back
 * to it on a refusal or any failure, so it holds the whole glyph, or the
 * placeholder (and its stand-in) for it, never part of a graph.
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

/**
 * @brief Why a glyph's color description was not lowered (L10 of color-paint story P4, R4 of P5).
 *
 * The lowering refuses only what no scene list can say: a paint no clip bounds and the font states no clip box for, a
 * `PaintColrGlyph` naming a glyph with no version 1 record, and a graph past the walk's depth or node bound (which
 * OpenType forbids). The values are numbered from 1 and are the codes `COLR` has in shape-scene's placeholder registry
 * (`GSHSCENE_PLACEHOLDER_COLR_*`, report.h).
 */
typedef enum GFNT_ColorRefusal {
  GFNT_COLOR_LOWERED = 0,             ///< Not refused.
  GFNT_COLOR_REFUSED_UNBOUNDED = 1,   ///< A solid or gradient no glyph bounds, in a glyph with no clip box.
  GFNT_COLOR_REFUSED_NO_RECORD = 2,   ///< A `PaintColrGlyph` naming a glyph with no version 1 record.
  GFNT_COLOR_REFUSED_WALK_LIMIT = 3,  ///< A paint graph past the walk's depth or node bound (a glyph that names itself).
  GFNT_COLOR_REFUSAL_COUNT            ///< Closes the enum.
} GFNT_ColorRefusal;

/**
 * @brief What a refused glyph is given (color-paint story P5; see the file comment). The values are pinned.
 */
typedef enum GFNT_ColorRefusalPolicy {
  GFNT_COLOR_ON_REFUSAL_PLACEHOLDER = 0, ///< The default: a placeholder item; ::GFNT_OK.
  GFNT_COLOR_ON_REFUSAL_FALLBACK = 1,    ///< An approximated placeholder and a fill of the base outline in the foreground; ::GFNT_OK.
  GFNT_COLOR_ON_REFUSAL_NONE = 2,        ///< Nothing recorded; ::GFNT_ERR_UNSUPPORTED and the reason.
  GFNT_COLOR_ON_REFUSAL_COUNT            ///< Closes the enum.
} GFNT_ColorRefusalPolicy;

/** @brief What a lowering put in the list for a glyph. The values are pinned. */
typedef enum GFNT_ColorOutcome {
  GFNT_COLOR_OUTCOME_LOWERED = 0,     ///< The glyph's whole description (or nothing: it has none; see `source`).
  GFNT_COLOR_OUTCOME_PLACEHOLDER = 1, ///< A placeholder item for a refusal (`refusal` says why).
  GFNT_COLOR_OUTCOME_FALLBACK = 2     ///< An approximated placeholder and the monochrome outline's fill.
} GFNT_ColorOutcome;

/**
 * @brief The space a version 1 glyph's gradients are interpolated in and its layers, composites and blends are
 *   composited in (L3 and L8).
 */
typedef enum GFNT_ColorSpace {
  /**
   * The standard's: OpenType requires gradient colors interpolated premultiplied in linear light and compositing and
   * blending done on linear-light values (CPAL, "Interpolation of colors"; COLR, "PaintComposite"). The default.
   */
  GFNT_COLOR_SPACE_STANDARD = 0,
  /**
   * The encoded values: sRGB with its transfer function, gradients interpolated unpremultiplied. This is what the
   * shipping renderers (Skia, and the browsers on it) draw, and what the Skia comparison runs in.
   */
  GFNT_COLOR_SPACE_ENCODED = 1
} GFNT_ColorSpace;

/** @brief Where and how a color glyph is drawn: every number fixed point, `v / 2^bits`. */
typedef struct GFNT_ColorRun {
  int32_t size;          ///< The em size in list units; greater than zero.
  int32_t x;             ///< The glyph's origin in list space.
  int32_t y;             ///< Likewise; down is positive.
  uint32_t bits;         ///< The fraction bits of the three, 0 to 16.
  uint32_t hint_mode;    ///< A `GSHSCENE_Hint_Mode` for every shape's run.
  uint32_t hint_version; ///< Passed to the provider unread.
  int32_t layer;         ///< The declared layer of every item recorded.
  uint32_t space;        ///< A ::GFNT_ColorSpace (zero, the standard's, is the default).
  uint32_t refusal_policy; ///< A ::GFNT_ColorRefusalPolicy (zero, a placeholder, is the default).
} GFNT_ColorRun;

/** @brief What a lowering did. */
typedef struct GFNT_ColorLowering {
  uint32_t source;         ///< A ::GFNT_ColorSource.
  uint32_t refusal;        ///< A ::GFNT_ColorRefusal.
  uint32_t paint_format;   ///< The refused paint's `GFNT_PaintFormat` (version 1), or 0.
  uint32_t composite_mode; ///< A refused blend mode, as `COLR` numbers it; else 0.
  size_t items;            ///< The items recorded: 0 when refused under ::GFNT_COLOR_ON_REFUSAL_NONE, else the placeholder (and its fill).
  uint32_t outcome;        ///< A ::GFNT_ColorOutcome: what is in the list (P5).
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
 * @return ::GFNT_OK (lowered, or no color description: see `out->source`; or a
 *   refusal answered by a placeholder or a fallback: see `out->outcome` and
 *   `out->refusal`); ::GFNT_ERR_UNSUPPORTED for a refusal under
 *   ::GFNT_COLOR_ON_REFUSAL_NONE (`out->refusal`), nothing recorded;
 *   ::GFNT_ERR_INVALID (NULL, a face not bound by this library, a closed list or
 *   one of another store, a run outside its rules, a policy past the enum);
 *   ::GFNT_ERR_LIMIT (a list or store limit); ::GFNT_ERR_CORRUPT;
 *   ::GFNT_ERR_OOM. On every failure the list is as it was.
 */
GFNT_API GFNT_Result gfnt_scene_record_color_glyph(const GSHSCENE_Resource * face,
    GSHSCENE_Store * store, GSHSCENE_List * list, uint32_t glyph,
    const GFNT_ColorRun * run, GFNT_ColorLowering * out, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_SCENE_COLOR_H
