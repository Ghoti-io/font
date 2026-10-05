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
 * @ref GFNT_Outline "GFNT_Outline": a glyph as a path, and what can be done
 * to one.
 *
 * documentation/design.md sections 4.1, 7.3 and 8.1. This is tier 1: the
 * reader below it knows nothing about paths, and nothing above it is needed to
 * use one.
 *
 * **An outline holds the font's own points, not a path derived from them.**
 * `glyf` allows a contour to begin off-curve and leaves the on-curve point
 * between two consecutive off-curve points implicit, and both of those are
 * resolved by ::gfnt_outline_decompose() rather than at parse time. The
 * reason is that they are two different things to be right about: the points
 * and flags can be compared against the table byte for byte, and the path can
 * be compared against a reference pen's idea of the same glyph. Materialising
 * the implicit points at parse time would leave the second comparison as the
 * only one possible, and it is the weaker of the two.
 *
 * Coordinates are 26.6 throughout, in one of two spaces, and
 * ::gfnt_outline_space() says which. No `float` appears anywhere between a
 * font's bytes and a pixel's coverage, which is section 1's determinism
 * promise; section 14.4 is the gate that keeps it.
 */

#ifndef GHOTI_IO_GFNT_OUTLINE_H
#define GHOTI_IO_GFNT_OUTLINE_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A glyph's contours, as the font draws them.
 *
 * Created by ::gfnt_outline_create() or filled in by
 * ::gfnt_face_glyph_outline(), and released with ::gfnt_outline_destroy().
 * Used from one thread at a time (design.md section 15.3).
 */
typedef struct GFNT_Outline GFNT_Outline;

/**
 * @brief Which space an outline's coordinates are in.
 *
 * Both are 26.6, which is exactly why the distinction has to be carried: the
 * numbers are indistinguishable and the mistake - rasterising font units, or
 * measuring pixels as units - produces a glyph a thousand pixels tall rather
 * than an error.
 */
typedef enum {
  /** Font units times 64. A point at x=500 in a 1000-unit em reads 32000. */
  GFNT_OUTLINE_UNITS = 0,
  /** Pixels in 26.6, after ::gfnt_outline_scale(). */
  GFNT_OUTLINE_PIXELS,
  GFNT_OUTLINE_SPACE_COUNT ///< Closes the enum for the string table's test.
} GFNT_OutlineSpace;

/**
 * @brief Name a coordinate space, for dumps and diagnostics.
 *
 * @param space The space.
 * @return A static string, never NULL.
 */
GFNT_API const char * gfnt_outline_space_string(GFNT_OutlineSpace space);

/**
 * @brief What one point of an outline is.
 *
 * A quadratic control point comes from `glyf`, a cubic from a charstring or a
 * `COLR` v1 transform. Both arms exist now because the tag is stored per point
 * and a path that mixed them would otherwise be unrepresentable - not because
 * anything reads cubics yet.
 */
typedef enum {
  GFNT_POINT_ON = 0, ///< On the curve.
  GFNT_POINT_QUAD,   ///< Quadratic (conic) control point.
  GFNT_POINT_CUBIC,  ///< Cubic control point.
  GFNT_POINT_TAG_COUNT ///< Closes the enum.
} GFNT_PointTag;

/**
 * @brief Name a point tag.
 *
 * @param tag The tag.
 * @return A static string, never NULL.
 */
GFNT_API const char * gfnt_point_tag_string(GFNT_PointTag tag);

/**
 * @brief One coordinate pair, 26.6, y-up (design.md section 5.5).
 */
typedef struct GFNT_Point {
  GFNT_F26Dot6 x; ///< Horizontal, positive rightwards.
  GFNT_F26Dot6 y; ///< Vertical, positive **upwards**.
} GFNT_Point;

/**
 * @brief An axis-aligned box, 26.6, y-up.
 *
 * An empty box is `x_min > x_max`, which is what ::gfnt_outline_bounds()
 * reports for an outline with no points - distinguishable from a box of zero
 * area at the origin, which a glyph can legitimately have.
 */
typedef struct GFNT_Box {
  GFNT_F26Dot6 x_min; ///< Left edge.
  GFNT_F26Dot6 y_min; ///< Bottom edge.
  GFNT_F26Dot6 x_max; ///< Right edge.
  GFNT_F26Dot6 y_max; ///< Top edge.
} GFNT_Box;

/**
 * @brief Whether a box holds nothing.
 *
 * @param box The box, or NULL.
 * @return true for NULL or for a box whose minimum exceeds its maximum.
 */
GFNT_API bool gfnt_box_is_empty(const GFNT_Box * box);

/**
 * @brief Where ::gfnt_outline_decompose() sends a path.
 *
 * Every callback returns a ::GFNT_Result, and the first failure stops the walk
 * and is what ::gfnt_outline_decompose() returns: a sink that is filling a
 * buffer has to be able to say it ran out. `move_to` and `close` are required;
 * a NULL `cubic_to` makes a cubic segment ::GFNT_ERR_UNSUPPORTED, which is
 * what a quadratic-only consumer wants to hear rather than silently losing the
 * curve.
 */
typedef struct GFNT_OutlineSink {
  /** Begin a contour at @p to. */
  GFNT_Result (*move_to)(void * user, GFNT_Point to);
  /** A straight segment to @p to. */
  GFNT_Result (*line_to)(void * user, GFNT_Point to);
  /** A quadratic from the current point through @p control to @p to. */
  GFNT_Result (*quad_to)(void * user, GFNT_Point control, GFNT_Point to);
  /** A cubic from the current point through @p c1 and @p c2 to @p to. */
  GFNT_Result (*cubic_to)(void * user, GFNT_Point c1, GFNT_Point c2,
      GFNT_Point to);
  /** Close the current contour back to where `move_to` put it. */
  GFNT_Result (*close)(void * user);
} GFNT_OutlineSink;

/**
 * @brief Create an empty outline.
 *
 * @param allocator Where its arrays come from, or NULL for the default. The
 *   outline keeps the pointer, so the allocator must outlive it.
 * @param out_outline Receives the outline. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID or ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_outline_create(const GFNT_Allocator * allocator,
    GFNT_Outline ** out_outline, GFNT_Error * error);

/**
 * @brief Release an outline.
 *
 * @param outline The outline, or NULL.
 */
GFNT_API void gfnt_outline_destroy(GFNT_Outline * outline);

/**
 * @brief Forget every point and contour, keeping the allocation.
 *
 * For a caller rendering a run of glyphs through one outline rather than
 * allocating per glyph.
 *
 * @param outline The outline, or NULL.
 */
GFNT_API void gfnt_outline_clear(GFNT_Outline * outline);

/**
 * @brief This glyph's outline, in font units.
 *
 * Composites are resolved: the returned outline is the union of the
 * components' contours, each transformed as its flags say, to the depth
 * `max_composite_depth` allows. A glyph with no contours at all - `space`,
 * usually - succeeds with an outline of zero points, because "this glyph draws
 * nothing" is an answer and not a failure.
 *
 * A composite that reaches itself, through any number of other glyphs, is
 * ::GFNT_ERR_CORRUPT named as a cycle rather than left to exhaust
 * ::GFNT_Limits::max_composite_depth, which is then what refuses an *acyclic*
 * chain nested deeper than the caller allows and reports ::GFNT_ERR_LIMIT. The
 * distinction is the one a caller can act on: a limit is answered by raising the
 * budget, and for a circular font no budget is ever enough. A glyph used twice in
 * one composite is not a cycle and is not refused, which is what every font with
 * a doubled diacritic needs.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param variation Normalised coordinates - ::gfnt_face_normalize() makes them
 *   from the ones a person types - or NULL for the default instance. A `glyf`
 *   face's points move by what its `gvar` says, a composite's component offsets
 *   move, and a component is itself drawn at the same location; a point a tuple
 *   does not name takes a delta inferred from its neighbours in the same contour.
 *   See "Variation" below for what is refused.
 *
 * **Variation.** An outline at a location is the glyph as stored plus its deltas,
 * with the fraction kept: a delta is a whole number of font units times a scalar
 * between zero and one, and 26.6 holds the product to a 64th where rounding it to
 * a font unit would draw a different shape at nearly every point. FreeType rounds
 * a *composite's component offsets* to whole units and this library and fontTools
 * do not, so the three agree on a simple glyph to a 64th and on a composite to
 * half a unit per level of nesting at worst (`tools/oracle/var_diff.py`).
 *
 * - Fewer coordinates than axes leave the rest at their default; more is
 *   ::GFNT_ERR_INVALID, as is a variation of coordinates promised and not given.
 * - **A variation that moves nothing - every coordinate zero - is the default
 *   instance** and is answered as one, for any face.
 * - Anything else needs `glyf` outlines and a `gvar`, or `CFF2` outlines, whose
 *   `blend` operators move them. A face with `CFF ` or Type 1 outlines is
 *   ::GFNT_ERR_UNSUPPORTED (they have no blend operators), and so is a `glyf` face
 *   with an `fvar` and no `gvar` - for **every** glyph, an empty one included, so
 *   that the answer does not depend on which glyph a caller tried first.
 * - A `gvar` that contradicts itself condemns **the glyph** it contradicts
 *   (::GFNT_ERR_CORRUPT, M11) and not the face: the next glyph and the default
 *   instance still answer. A tuple that does not apply at the location is not
 *   read at all, so damage in it is not seen from there.
 * - ::gfnt_face_glyph_stated_box() is the box the file states and does not vary:
 *   it is the default instance's. The outline's own bounds are the ones at the
 *   location.
 * - The **phantom points** - the four after a glyph's own, which carry advance
 *   and bearing deltas - are not applied to anything here. Metrics at a location
 *   are a different accessor's business (design.md section 7.7).
 * @param allocator Allocator for the outline, or NULL for the default.
 * @param out_outline Receives a new outline the caller destroys. Written only
 *   on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a glyph past `numGlyphs`;
 *   ::GFNT_ERR_UNSUPPORTED for a face whose outlines this library does not read
 *   yet, which is not the same as a face with none; ::GFNT_ERR_CORRUPT;
 *   ::GFNT_ERR_LIMIT; or ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_face_glyph_outline(const GFNT_Face * face,
    uint32_t glyph, const GFNT_Variation * variation,
    const GFNT_Allocator * allocator, GFNT_Outline ** out_outline,
    GFNT_Error * error);

/**
 * @brief How many points the outline holds.
 *
 * @param outline The outline, or NULL.
 * @return The count, or 0 for NULL.
 */
GFNT_API size_t gfnt_outline_point_count(const GFNT_Outline * outline);

/**
 * @brief How many contours the outline holds.
 *
 * @param outline The outline, or NULL.
 * @return The count, or 0 for NULL.
 */
GFNT_API size_t gfnt_outline_contour_count(const GFNT_Outline * outline);

/**
 * @brief Which space the coordinates are in.
 *
 * @param outline The outline, or NULL.
 * @return The space, or ::GFNT_OUTLINE_UNITS for NULL.
 */
GFNT_API GFNT_OutlineSpace gfnt_outline_space(const GFNT_Outline * outline);

/**
 * @brief One point and its tag.
 *
 * @param outline The outline.
 * @param index Which point, from 0.
 * @param out_point Receives the coordinates, or NULL.
 * @param out_tag Receives the tag, or NULL.
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID for NULL or an index the outline
 *   does not have.
 */
GFNT_API GFNT_Result gfnt_outline_point_at(const GFNT_Outline * outline,
    size_t index, GFNT_Point * out_point, GFNT_PointTag * out_tag);

/**
 * @brief Which points make up one contour.
 *
 * @param outline The outline.
 * @param contour Which contour, from 0.
 * @param out_first Receives the first point's index, or NULL.
 * @param out_count Receives how many points it has, or NULL.
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID.
 */
GFNT_API GFNT_Result gfnt_outline_contour_at(const GFNT_Outline * outline,
    size_t contour, size_t * out_first, size_t * out_count);

/**
 * @brief Add a point to the outline's last contour.
 *
 * For a caller building an outline of its own - a test, or a producer this
 * library does not have. The point joins the contour opened by
 * ::gfnt_outline_begin_contour().
 *
 * @param outline The outline.
 * @param point Where.
 * @param tag What kind of point.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID if no contour is open,
 *   ::GFNT_ERR_LIMIT or ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_outline_add_point(GFNT_Outline * outline,
    GFNT_Point point, GFNT_PointTag tag, GFNT_Error * error);

/**
 * @brief Start a new contour.
 *
 * @param outline The outline.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_LIMIT or ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_outline_begin_contour(GFNT_Outline * outline,
    GFNT_Error * error);

/**
 * @brief The smallest box containing the whole path.
 *
 * The **tight** box, not the box of the control points: a quadratic's
 * extremum is solved for exactly in integer arithmetic, and a cubic's by
 * subdivision to within one 26.6 unit. A `glyf` glyph's stated `xMin`/`yMax`
 * are not consulted, so that they can be compared against this rather than
 * trusted as it.
 *
 * @param outline The outline.
 * @param out_box Receives the box; empty for an outline with no points.
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID.
 */
GFNT_API GFNT_Result gfnt_outline_bounds(const GFNT_Outline * outline,
    GFNT_Box * out_box);

/**
 * @brief The box containing every point, control points included.
 *
 * Cheaper than ::gfnt_outline_bounds() and never smaller. What an atlas
 * allocator wants when it would rather over-reserve than walk curves.
 *
 * @param outline The outline.
 * @param out_box Receives the box; empty for an outline with no points.
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID.
 */
GFNT_API GFNT_Result gfnt_outline_control_box(const GFNT_Outline * outline,
    GFNT_Box * out_box);

/**
 * @brief Apply a 2x2 matrix and a translation, in place.
 *
 * Row-major: `x' = xx*x + xy*y` and `y' = yx*x + yy*y`. Stated because `glyf`'s
 * own component matrix is stored column-major, so the four numbers in a font
 * file are not these four numbers in this order, and a reader that passes them
 * straight through shears every sheared component the wrong way while
 * reporting success.
 *
 * @param outline The outline.
 * @param xx Row 0 column 0, in 16.16.
 * @param xy Row 0 column 1, in 16.16.
 * @param yx Row 1 column 0, in 16.16.
 * @param yy Row 1 column 1, in 16.16.
 * @param dx Added to every x afterwards, in 26.6.
 * @param dy Added to every y afterwards, in 26.6.
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID.
 */
GFNT_API GFNT_Result gfnt_outline_transform(GFNT_Outline * outline,
    GFNT_F16Dot16 xx, GFNT_F16Dot16 xy, GFNT_F16Dot16 yx, GFNT_F16Dot16 yy,
    GFNT_F26Dot6 dx, GFNT_F26Dot6 dy);

/**
 * @brief Move every point, in place.
 *
 * @param outline The outline.
 * @param dx Added to every x, in 26.6.
 * @param dy Added to every y, in 26.6.
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID.
 */
GFNT_API GFNT_Result gfnt_outline_translate(GFNT_Outline * outline,
    GFNT_F26Dot6 dx, GFNT_F26Dot6 dy);

/**
 * @brief Scale font units to pixels, in place, and say so.
 *
 * The only way an outline's space becomes ::GFNT_OUTLINE_PIXELS, so that a
 * rasteriser can refuse an outline nobody scaled.
 *
 * @param outline The outline, which must be in ::GFNT_OUTLINE_UNITS.
 * @param scale Pixels per font unit, in 16.16, from
 *   ::gfnt_scale_for_ppem().
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID for NULL or an outline already in
 *   pixels.
 */
GFNT_API GFNT_Result gfnt_outline_scale(GFNT_Outline * outline,
    GFNT_F16Dot16 scale, GFNT_Error * error);

/**
 * @brief Walk the outline as a path.
 *
 * Where `glyf`'s two implicit constructions are resolved, and the only place
 * they are: a contour whose first point is off-curve starts at the midpoint
 * between it and the contour's last point - or at the last point itself when
 * that one is on-curve - and an on-curve point is synthesised midway between
 * any two consecutive off-curve points. A contour of nothing but off-curve
 * points is a circle of implied midpoints, which is legal and which fonts do
 * contain.
 *
 * Every contour is closed, whether or not the font's last point returns to the
 * first: a contour is a region, and an unclosed one is not a thing `glyf` can
 * express.
 *
 * @param outline The outline.
 * @param sink The callbacks, whose `move_to` and `close` must be non-NULL.
 * @param user Passed to every callback.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID; ::GFNT_ERR_UNSUPPORTED for a segment
 *   the sink has no callback for; or whatever a callback returned.
 */
GFNT_API GFNT_Result gfnt_outline_decompose(const GFNT_Outline * outline,
    const GFNT_OutlineSink * sink, void * user, GFNT_Error * error);

/**
 * @brief Write the outline's points and contours to a stream.
 *
 * The differential's input: `tools/oracle/glyf_diff.py` compares this against
 * fontTools' `glyf` table, point for point and flag for flag. An outline with
 * no points says so on one line rather than printing nothing, because a
 * differential reads nothing as agreement.
 *
 * @param outline The outline.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_outline_dump(const GFNT_Outline * outline,
    FILE * out);

/**
 * @brief Write the outline's **path** to a stream, one segment per line.
 *
 * ::gfnt_outline_dump() is the points as the font stores them; this is what
 * ::gfnt_outline_decompose() makes of them, which is the half a reference pen
 * can be compared against.
 *
 * @param outline The outline.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_outline_path_dump(const GFNT_Outline * outline,
    FILE * out);

/**
 * @brief Whether this glyph is a composite in the font's own terms.
 *
 * ::gfnt_face_glyph_outline() resolves composites, so nothing about the
 * resulting outline says whether the font drew it or assembled it. The
 * differential needs to know, and so does anything reporting on a font.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param out_composite Receives true for a composite glyph.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED for a face
 *   with no `glyf`, or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_glyph_is_composite(const GFNT_Face * face,
    uint32_t glyph, bool * out_composite, GFNT_Error * error);

/**
 * @brief The bounding box the `glyf` entry states for this glyph.
 *
 * What the font claims, in font units, which is a different fact from what its
 * points draw (::gfnt_outline_bounds()) and is stored separately in the file.
 * A composite's stated box is its own, not its components'.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param out_box Receives the box, empty for a glyph with no contours.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED or
 *   ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_glyph_stated_box(const GFNT_Face * face,
    uint32_t glyph, GFNT_Box * out_box, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_OUTLINE_H
