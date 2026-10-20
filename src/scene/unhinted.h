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
 * The one unhinted rounding (story 14b, D1), shared by the provider
 * (src/scene/scene.c) and ::gfnt_face_render_glyph() (src/raster/raster.c), so
 * that the direct call and a one-glyph scene draw the same pixels (AD-1).
 *
 * An outline in design units (26.6 of units, y up) is walked by
 * ::gfnt_outline_decompose() as it stands, so `glyf`'s implied midpoints are
 * taken in design units, and every point the walk emits is rounded once:
 * `(fx + gshgeom_scale_26_6(x, S, 0), fy + gshgeom_scale_26_6(y, S, 1))`, which
 * is `floor((s p S + 2^15) / 2^16)` (half up, in y down) plus the origin's
 * fraction. documentation/design.md section 8.6.
 */

#ifndef GHOTI_IO_GFNT_SCENE_UNHINTED_H
#define GHOTI_IO_GFNT_SCENE_UNHINTED_H

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>
#include <ghoti.io/shape-geometry/device.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where the walk sends its device points: y down, 26.6 pixels. */
typedef struct GFNT_DeviceSink {
  GFNT_Result (*move_to)(void * user, GSHGEOM_Device_Point to);
  GFNT_Result (*line_to)(void * user, GSHGEOM_Device_Point to);
  GFNT_Result (*quad_to)(void * user, GSHGEOM_Device_Point control,
      GSHGEOM_Device_Point to);
  GFNT_Result (*cubic_to)(void * user, GSHGEOM_Device_Point c1,
      GSHGEOM_Device_Point c2, GSHGEOM_Device_Point to);
  GFNT_Result (*close)(void * user);
} GFNT_DeviceSink;

/**
 * Walk @p units (in ::GFNT_OUTLINE_UNITS) with D1's rounding at the 16.16
 * @p scale, each point moved by the fraction @p fx, @p fy (y down).
 *
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for an outline in pixels;
 *   ::GFNT_ERR_LIMIT for a point whose result does not fit 26.6; or what the
 *   sink or the walk returned.
 */
GFNT_Result gfnt_unhinted_walk(const GFNT_Outline * units, int32_t scale,
    int32_t fx, int32_t fy, const GFNT_DeviceSink * sink, void * user,
    GFNT_Error * error);

/** Map a `shape-geometry` failure to this library's, with a message. */
GFNT_Result gfnt_unhinted_geometry_error(GSHGEOM_Result result,
    GFNT_Error * error);

/**
 * The walk into a device path: the provider's unhinted fast tier.
 */
GFNT_Result gfnt_unhinted_device_path(const GFNT_Outline * units,
    int32_t scale, int32_t fx, int32_t fy, GSHGEOM_Device_Path * out,
    GFNT_Error * error);

/**
 * The walk at fraction zero as an outline in ::GFNT_OUTLINE_PIXELS, y up, every
 * implied point made explicit, so that ::gfnt_raster_outline() walks exactly
 * the segments the device path holds: the direct renderer's unhinted outline.
 *
 * @param out Receives a new outline (from @p allocator, or the default) the
 *   caller destroys. Written only on success.
 */
GFNT_Result gfnt_unhinted_pixels(const GFNT_Outline * units, int32_t scale,
    const GFNT_Allocator * allocator, GFNT_Outline ** out, GFNT_Error * error);

/**
 * ::gfnt_face_render_glyph()'s unhinted case: the glyph's outline at the
 * options' variation, rounded by D1 at ::gfnt_scale_for_ppem()'s scale, and
 * rasterised under the face's limits.
 */
GFNT_Result gfnt_unhinted_render(const GFNT_Face * face, uint32_t glyph,
    uint16_t upem, uint32_t ppem, const GFNT_RasterOptions * options,
    const GFNT_Allocator * allocator, GFNT_Coverage * out_coverage,
    GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_SCENE_UNHINTED_H
