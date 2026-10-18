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
 * The curve flattener: a quadratic or cubic turned into a polyline whose every
 * vertex is a 26.6 point and which stays within the caller's tolerance of the
 * curve. Revision ::GFNT_RASTER_FLATTEN_REVISION.
 *
 * Internal. It is its own translation unit so that the property it exists to
 * guarantee - the polyline is within the tolerance of the curve - can be tested
 * against the curve itself rather than against the pixels it ends up as, and so
 * that the proof (flatten.c) sits beside the only code it is about.
 * documentation/design.md section 8.1.
 */

#ifndef GHOTI_IO_GFNT_FLATTEN_H
#define GHOTI_IO_GFNT_FLATTEN_H

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How many times a curve is halved, at most, before its chord is used as it is. */
#define GFNT_FLATTEN_DEPTH 20

/**
 * Receives one vertex of the polyline, in order.
 *
 * @param user The pointer given to the flattener.
 * @param to The vertex, 26.6. The polyline starts at the curve's first point,
 *   which is not emitted.
 * @param leaf Which piece of the curve this vertex ends: the piece covers the
 *   curve's parameter interval `[leaf / 2^level, (leaf + 1) / 2^level]`. A
 *   test uses this to evaluate the curve exactly where the segment replaces it.
 * @param level How many times the curve was halved to reach that piece.
 */
typedef void (*GFNT_FlattenEmit)(void * user, GFNT_Point to, uint32_t leaf,
    int level);

/**
 * Flatten a quadratic Bezier.
 *
 * @param tolerance How far the polyline may stray, in 26.6 units. At least
 *   ::GFNT_RASTER_MIN_TOLERANCE; a smaller value is raised to it, which is a
 *   guard and not a feature (the rasteriser refuses such an option before it
 *   gets here).
 * @param p0 First point. Any int32 coordinates are valid.
 * @param p1 The control point.
 * @param p2 Last point; the last vertex emitted.
 * @param emit Called once per vertex, in order.
 * @param user Handed to `emit`.
 * @return How many pieces the depth cap (::GFNT_FLATTEN_DEPTH) forced to a
 *   chord although they were not flat. flatten.c proves it is zero for every
 *   input; the value is returned so that a test can see that it is.
 */
int gfnt_flatten_quad(GFNT_F26Dot6 tolerance, GFNT_Point p0, GFNT_Point p1,
    GFNT_Point p2, GFNT_FlattenEmit emit, void * user);

/**
 * ::gfnt_flatten_quad with a smaller depth cap, `1 <= levels <= GFNT_FLATTEN_DEPTH`.
 * The proof shows the real cap is never reached, so nothing else can show that
 * a piece forced by it is counted, emitted and tiles the curve; a test uses a cap
 * a large curve does reach.
 */
int gfnt_flatten_quad_capped(GFNT_F26Dot6 tolerance, int levels, GFNT_Point p0,
    GFNT_Point p1, GFNT_Point p2, GFNT_FlattenEmit emit, void * user);

/** ::gfnt_flatten_cubic with a smaller depth cap; see ::gfnt_flatten_quad_capped. */
int gfnt_flatten_cubic_capped(GFNT_F26Dot6 tolerance, int levels, GFNT_Point p0,
    GFNT_Point p1, GFNT_Point p2, GFNT_Point p3, GFNT_FlattenEmit emit,
    void * user);

/** Flatten a cubic Bezier; see ::gfnt_flatten_quad. */
int gfnt_flatten_cubic(GFNT_F26Dot6 tolerance, GFNT_Point p0, GFNT_Point p1,
    GFNT_Point p2, GFNT_Point p3, GFNT_FlattenEmit emit, void * user);

#ifdef __cplusplus
}
#endif

#endif
