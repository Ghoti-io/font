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
 * This library as a glyph path provider of `shape-scene` (story 14b of the shape
 * family's first milestone; AD-5, CAP-10): a ::GFNT_Face bound to a scene's face
 * resource, so that any sink draws its glyphs through the seam
 * (`<ghoti.io/shape-scene/glyph_provider.h>`) without including a header of this
 * library. documentation/design.md section 8.7.
 *
 * **Make one ::GFNT_SceneFont per face** (::gfnt_scene_font_create), then intern
 * a face resource per instance with ::gfnt_scene_intern_face. The scene font
 * hashes the font's bytes once, for the resource's `font_key`, and reads the
 * names a writer needs to reference the font (R1). Each face resource it binds
 * holds a reference on it, so ::gfnt_scene_font_destroy may come before the
 * scene is done; the ::GFNT_Face, and the blob under it, must outlive the scene
 * font and every face resource bound to it.
 *
 * **The ops** (all of them integers; any may be called from any thread at once,
 * since a face is shareable read-only, design.md section 15.3):
 *
 * - `face_info`: the upem, the axis count, the flags, the largest ppem the face
 *   hints (its `max_ppem`, at most the seam's), the glyph count and the family
 *   (name 16, else 1) and PostScript (name 6) names.
 *   ::GSHSCENE_FACE_HINTING is set for a face with TrueType outlines and a
 *   program (an `fpgm`, a `prep`, or glyph instructions per `maxp`);
 *   ::GSHSCENE_FACE_COLOR for a face with `COLR` layers.
 * - `glyph_outline`: ::gfnt_face_glyph_outline() at the instance, walked by
 *   ::gfnt_outline_decompose() **in design units** (so `glyf`'s implied
 *   midpoints are taken in 26.6 design units, exactly for whole units) and given
 *   to the path through `shape-geometry`'s fixed-point builder with 6 fraction
 *   bits, y up. Size-independent: this is what a writer, and a GPU sink keeping
 *   signed-distance or MSDF atlases, takes (R2).
 * - `glyph_device_path`: the fast tier. **Hinted** where the run asks for
 *   hinting, the face's `gasp` grid-fits at the ppem, and the glyph's program
 *   runs (status `HINTED` or `WITH_ERRORS`): the hinter's points, each moved by
 *   the request's origin fraction, y down. **Unhinted** otherwise (mode `NONE`, a
 *   face with no TrueType outlines, a size `gasp` does not fit, a glyph with no
 *   program, a program that failed): the one rounding (D1), the same walk as
 *   `glyph_outline` with each point `(x, y)` becoming
 *   `(fx + gshgeom_scale_26_6(x, S, 0), fy + gshgeom_scale_26_6(y, S, 1))` for the
 *   request's scale `S`. Under `FONT_STRICT` a program that fails is
 *   `GSHSCENE_ERR_REFUSED`. The advance is the hinter's where hinted, else
 *   `hmtx`'s (at the instance) scaled by the same rule, or 0 where the face
 *   states none.
 * - `glyph_box`: the box of the outline's points, control points included, in
 *   26.6 design units (::gfnt_outline_control_box()).
 * - `face_data`: the bytes of the blob the face was loaded from.
 *
 * A glyph this library cannot give fails its op with the seam's spelling of the
 * failure (`CORRUPT`, `LIMIT`, `OOM`, `UNSUPPORTED`, `INVALID` for a glyph past
 * the face), and the sink refuses that glyph alone.
 *
 * **The hint contexts** (D8). Hinting at a size runs the font's `fpgm` and `prep`
 * once; the context that holds the result is kept, per face resource, in a
 * least-recently-used cache of ::GFNT_SCENE_HINT_CONTEXTS entries keyed by
 * `(ppem, mode, version)` (the instance is the face resource's). A context in use
 * by a call is never dropped; when every entry is in use the call makes one for
 * itself and frees it after. The cache has a cutil mutex, and the face
 * resource's last release frees it.
 *
 * **`gfnt_face_render_glyph()` draws through the same code**, so that its pixels
 * are a one-glyph scene's (AD-1): its unhinted cases are the walk above, and its
 * hinted ones the hinter's points.
 *
 * No floating type appears in this header or in any header it includes (AD-5,
 * `make check-float-free`).
 */

#ifndef GHOTI_IO_GFNT_SCENE_H
#define GHOTI_IO_GFNT_SCENE_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/shape-scene/glyph_provider.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How many hint contexts a face resource keeps (D8): its LRU's capacity. */
#define GFNT_SCENE_HINT_CONTEXTS 8u

/**
 * @brief A face prepared to be bound to scenes.
 *
 * Reference counted: ::gfnt_scene_font_destroy() drops the creator's reference
 * and each face resource bound to it holds one.
 */
typedef struct GFNT_SceneFont GFNT_SceneFont;

/**
 * @brief What a scene font's provider has been asked, summed over every face
 *   resource bound to it.
 *
 * The counts only grow; a test takes differences.
 */
typedef struct GFNT_SceneStats {
  size_t info_calls;       ///< `face_info`.
  size_t general_calls;    ///< `glyph_outline`: the general route.
  size_t fast_calls;       ///< `glyph_device_path`: the fast tier.
  size_t box_calls;        ///< `glyph_box`.
  size_t contexts_made;    ///< Hint contexts made (cached or not).
  size_t contexts_dropped; ///< Contexts the LRU dropped to make room.
  size_t contexts_live;    ///< Contexts the caches hold now.
  size_t bindings_live;    ///< Face resources bound and not yet released.
} GFNT_SceneStats;

/**
 * @brief Prepare @p face to be bound to scenes.
 *
 * Hashes the bytes the face was loaded from (FNV-1a 64, never zero: the face
 * resources' `font_key`) and reads its names.
 *
 * @param face The face; it must outlive the scene font and every face resource
 *   bound to it.
 * @param out_font Receives the scene font. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID (NULL); ::GFNT_ERR_UNSUPPORTED for a face
 *   whose `unitsPerEm` is outside the seam's `[16, 16384]`; ::GFNT_ERR_CORRUPT;
 *   ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_scene_font_create(const GFNT_Face * face,
    GFNT_SceneFont ** out_font, GFNT_Error * error);

/**
 * @brief Drop the creator's reference. The face resources bound to the font
 *   keep it alive. NULL is ignored.
 */
GFNT_API void gfnt_scene_font_destroy(GFNT_SceneFont * font);

/**
 * @brief Intern a face resource of the font at an instance, bound to this
 *   library's provider.
 *
 * Its content is the font's key, the face's index in its collection and the
 * instance's coordinates. An intern of content the store already holds returns
 * that face (and its binding, whichever scene font made it: equal keys are equal
 * bytes).
 *
 * @param font The scene font.
 * @param store The scene's store.
 * @param variation Normalised coordinates, or NULL for the default instance. At
 *   most the face's axis count; its `delta_rounding` must be the default (the
 *   resource's content has no place for another).
 * @param out_face Receives the face resource, with a reference the caller
 *   releases (`gshscene_resource_release`). Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID (NULL, more coordinates than axes, a
 *   coordinate outside `[-16384, 16384]`, a count without coordinates);
 *   ::GFNT_ERR_UNSUPPORTED (a delta rounding other than the default);
 *   ::GFNT_ERR_LIMIT (the store's limits); ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_scene_intern_face(GFNT_SceneFont * font,
    GSHSCENE_Store * store, const GFNT_Variation * variation,
    const GSHSCENE_Resource ** out_face, GFNT_Error * error);

/**
 * @brief The font's key: the `font_key` of every face resource it binds.
 *
 * @return The key, or 0 for NULL.
 */
GFNT_API uint64_t gfnt_scene_font_key(const GFNT_SceneFont * font);

/**
 * @brief The provider's ops, for a caller that binds a face itself.
 *
 * The context the ops take is a binding this library made; a caller that is not
 * this library has no use for them but to compare.
 */
GFNT_API const GSHSCENE_Glyph_Provider * gfnt_scene_provider(void);

/**
 * @brief What the font's provider has been asked (see ::GFNT_SceneStats).
 *
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID for NULL.
 */
GFNT_API GFNT_Result gfnt_scene_font_stats(const GFNT_SceneFont * font,
    GFNT_SceneStats * out_stats);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_SCENE_H
