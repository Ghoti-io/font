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
 * Rasterisation, and @ref GFNT_Coverage "GFNT_Coverage": an outline turned into
 * 8-bit coverage.
 *
 * documentation/design.md section 8. The coverage is computed by `shape-cpu`, the
 * family's reference rasteriser, to which this library's scan converter moved
 * (AD-1, AD-12): a cell-based exact-area rasteriser in the lineage of libart and
 * FreeType's `smooth` module, where each edge deposits signed `cover` and `area`
 * into the cells it crosses and a sweep per scanline accumulates winding coverage
 * across cells and applies the fill rule. That handles a self-overlapping contour
 * - a CJK composite, a badly drawn font - the way every reference implementation
 * does, and it computes area rather than sampling it. This library builds the
 * device path from the outline (flipping y, since an outline is y-up and a
 * device is y-down) and derives a ::GFNT_Coverage from what `shape-cpu` returns.
 *
 * **Coverage is linear alpha.** Blending it in the right colour space is the
 * compositor's business (section 8.2); ::gfnt_coverage_apply_table() will apply
 * a table, and the table is not made here because only the compositor knows
 * what space it is compositing in.
 *
 * **No `float` appears between a font's bytes and a pixel's coverage.** That is
 * section 1's determinism promise - the same font at the same size gives
 * byte-identical pixels on every platform - and `make check-golden` is what keeps
 * it, by rendering the committed fixtures here and on three big-endian targets
 * and requiring the same hashes. What that gate sees is byte order, alignment and
 * word size; that no `float` is here at all is kept by there being no
 * floating-point type in these headers and by `-Wfloat-conversion` on every
 * translation unit, because two IEEE-754 targets would agree about a float.
 *
 * What is deliberately absent (section 8.4): no LCD filtering, no stem
 * darkening, no dropout control, no embolden or oblique synthesis. Hinting is
 * not absent (section 8.5): it is on by default, so a TrueType outline is fitted
 * to the pixel grid by the font's own instructions unless `hint_mode` is
 * `GFNT_HINT_MODE_NONE` or the font's `gasp` table asks for no grid-fitting at
 * the size. What is absent is an automatic hinter (`GFNT_HINT_MODE_AUTO` is
 * reserved and refused) and any hinting of CFF, Type 1 or bitmap faces.
 */

#ifndef GHOTI_IO_GFNT_RASTER_H
#define GHOTI_IO_GFNT_RASTER_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/bitmap.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/hint.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/shape-geometry/fill.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Which points a path encloses.
 *
 * This is `shape-geometry`'s fill rule under its old name (AD-4): the type is
 * the same, the values are pinned (0 nonzero, the default; 1 even-odd; a
 * further rule is appended after them), and a caller that moves between this
 * library and `shape-cpu` converts nothing. `GFNT_FILL_NONZERO` is a point
 * inside where the winding number is not zero - the default, and what every
 * font format means; `GFNT_FILL_EVEN_ODD`, where it is odd;
 * `GFNT_FILL_RULE_COUNT` closes the enumeration.
 */
typedef GSHGEOM_Fill_Rule GFNT_FillRule;
#define GFNT_FILL_NONZERO GSHGEOM_FILL_NONZERO
#define GFNT_FILL_EVEN_ODD GSHGEOM_FILL_EVEN_ODD
#define GFNT_FILL_RULE_COUNT GSHGEOM_FILL_RULE_COUNT

/**
 * @brief Name a fill rule.
 *
 * @param rule The rule.
 * @return A static string, never NULL.
 */
GFNT_API const char * gfnt_fill_rule_string(GFNT_FillRule rule);

/**
 * @brief 8-bit linear coverage for one glyph.
 *
 * Row 0 is the **top** row, which is the convention every compositor wants, and
 * `top` is that row's upper edge in glyph space, where y is up (design.md
 * section 5.5). So the pixel at `(column, row)` covers glyph-space
 * `x` in `[left + column, left + column + 1)` and `y` in
 * `(top - row - 1, top - row]`. The flip between the two lives here and
 * nowhere else.
 *
 * The struct is open rather than opaque because a compositor walks rows, and a
 * per-pixel accessor call in that loop is a cost paid for nothing. `data`
 * belongs to the coverage: it is freed by ::gfnt_coverage_destroy() and must
 * not be freed by a caller.
 *
 * An empty glyph gives a coverage of zero width and height with `data` NULL,
 * which is a thing to composite rather than an error.
 */
typedef struct GFNT_Coverage {
  uint8_t * data;              ///< `height * stride` bytes, or NULL if empty.
  size_t stride;               ///< Bytes per row; at least `width`.
  uint32_t width;              ///< Pixels across.
  uint32_t height;             ///< Rows.
  int32_t left;                ///< Integer x of the leftmost column's left edge.
  int32_t top;                 ///< Integer y of the top row's upper edge, y-up.
  GFNT_F26Dot6 origin_x;       ///< The sub-pixel x this was rendered at.
  GFNT_F26Dot6 origin_y;       ///< The sub-pixel y this was rendered at.
  GFNT_FillRule fill;          ///< The rule that was applied.
  const GFNT_Allocator * allocator; ///< Where `data` came from.
} GFNT_Coverage;

/**
 * The revision of the flattener: the rule that turns a curve into the polyline
 * the scan converter fills, and so what every rendering's pixels depend on.
 *
 * - Revision 1: midpoints floored to 26.6 at every level, flat at four
 *   tolerances. Its deviation from the curve grew with the depth, to 13 units on
 *   a curve spanning the int32 range, though the tolerance said 4.
 * - Revision 2: subdivision carried at eight more fractional bits, a vertex
 *   rounded to 26.6 once when it is emitted, flat at `4 * (tolerance - 1)`
 *   units. Within the tolerance for every curve and every accepted tolerance.
 *
 * A change to the rule is a new revision, and every committed golden rendering
 * is rebaselined in the same change: the golden file's header names the revision
 * it was made under, and a test requires it to equal this. The revision is not
 * part of ::gfnt_coverage_hash(), which hashes pixels. There is no way to ask
 * for an earlier revision. The rule is `shape-geometry`'s, and this is its
 * `GSHGEOM_FLATTEN_REVISION`: raster.c does not compile if the two differ.
 */
#define GFNT_RASTER_FLATTEN_REVISION 2

/**
 * The smallest tolerance ::GFNT_RasterOptions::tolerance accepts, in 26.6 units
 * (two units, a 32nd of a pixel). See that field. It is `shape-geometry`'s
 * `GSHGEOM_FLATTEN_MIN_TOLERANCE`; raster.c does not compile if the two differ.
 */
#define GFNT_RASTER_MIN_TOLERANCE 2

/**
 * @brief How to rasterise.
 *
 * All-zero is the sensible default: non-zero winding, no sub-pixel offset, the
 * default flattening tolerance, and the font's own hinting. A caller that memsets one and sets nothing
 * gets what it expects.
 */
typedef struct GFNT_RasterOptions {
  GFNT_FillRule fill;       ///< Zero is non-zero winding.
  /**
   * Sub-pixel offset applied to the outline before rasterising, in 26.6
   * pixels (design.md section 8.3). A GPU atlas that bins to quarters of a
   * pixel renders each bin at 0, 16, 32 and 48; vertical offsets work and are
   * usually unwanted.
   */
  GFNT_F26Dot6 origin_x;
  GFNT_F26Dot6 origin_y;   ///< The vertical offset, in 26.6 pixels; see origin_x.
  /**
   * How far the flattened path may stray from the curve, in 26.6 units. Zero
   * means the default, 4 units, a sixteenth of a pixel, which is below what
   * 8-bit coverage can express.
   *
   * **This is a guarantee.** For any outline, whatever its size, every point of
   * every quadratic and cubic lies within `tolerance` units of the polyline the
   * scan converter is given (measured before the polyline is turned into
   * coverage), and it is the same polyline on every target. The flattener is
   * `shape-geometry`'s (gshgeom_flatten_quadratic(), gshgeom_flatten_cubic()),
   * and the proof is beside it, in that library's src/flatten/flatten.c, and in
   * design.md section 8.1; the first revision of the
   * flattener (before ::GFNT_RASTER_FLATTEN_REVISION 2) did not keep it, and
   * strayed by up to 13 units on large curves.
   *
   * It holds for every value this field accepts: ::GFNT_RASTER_MIN_TOLERANCE or
   * more. Below that (1, a 64th of a pixel) the guarantee cannot be kept, so
   * the call is ::GFNT_ERR_INVALID rather than a rasterisation that quietly
   * promises less than it was asked for. A negative value is refused as well.
   */
  GFNT_F26Dot6 tolerance;
  /**
   * Whether ::gfnt_face_render_glyph() fits the outline to the pixel grid with
   * the font's own program. Zero is the font's own request; ::GFNT_HINT_MODE_NONE
   * renders the outline as drawn. ::gfnt_raster_outline() takes an outline that
   * is already in pixels and ignores this.
   */
  GFNT_HintMode hint_mode;
  GFNT_HintVersion hint_version;  ///< Which interpreter behaviour hinting runs under.
  /**
   * Where in a variable face's design space to render, or NULL for the default
   * instance. Used by ::gfnt_face_render_glyph(), which loads and hints the glyph at it;
   * ::gfnt_raster_outline() takes an outline already placed.
   */
  const GFNT_Variation * variation;
} GFNT_RasterOptions;

/**
 * @brief Rasterise an outline that is already in pixels.
 *
 * @param outline The outline, which must be in ::GFNT_OUTLINE_PIXELS - an
 *   outline in font units would render a thousand pixels tall, so it is
 *   ::GFNT_ERR_INVALID rather than a surprise.
 * @param options How to rasterise, or NULL for the defaults.
 * @param limits Caps to apply, or NULL for the defaults. `max_ppem` bounds the
 *   coverage's width and height and `max_raster_bytes` its total size.
 * @param allocator Allocator for the coverage, or NULL for the default.
 * @param out_coverage Receives the coverage. Written only on success; the
 *   caller releases it with ::gfnt_coverage_destroy().
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_LIMIT, ::GFNT_ERR_OOM, or
 *   ::GFNT_ERR_UNSUPPORTED for a path the walk cannot take.
 */
GFNT_API GFNT_Result gfnt_raster_outline(const GFNT_Outline * outline,
    const GFNT_RasterOptions * options, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Coverage * out_coverage,
    GFNT_Error * error);

/**
 * @brief Load, scale and rasterise one glyph.
 *
 * The whole path from a glyph index to pixels, so that a caller does not have
 * to know that scaling happens once and in one place (M5).
 *
 * Unless `options->hint_mode` says otherwise the glyph is hinted by the font's
 * own program. That prepares the face for the size on every call (it runs the
 * font's two programs), so a caller rendering many glyphs at one size should
 * make a ::GFNT_HintContext once and use ::gfnt_face_hinted_outline() with
 * ::gfnt_raster_outline().
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param ppem The pixel size. Zero is a caller error; above `max_ppem` is
 *   ::GFNT_ERR_LIMIT.
 * @param options How to rasterise, or NULL for the defaults.
 * @param allocator Allocator for the coverage, or NULL for the face's.
 * @param out_coverage Receives the coverage. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, or whatever loading the outline or rasterising it
 *   returned.
 */
GFNT_API GFNT_Result gfnt_face_render_glyph(const GFNT_Face * face,
    uint32_t glyph, uint32_t ppem, const GFNT_RasterOptions * options,
    const GFNT_Allocator * allocator, GFNT_Coverage * out_coverage,
    GFNT_Error * error);

/**
 * @brief A bitmap strike's glyph, as coverage.
 *
 * So that a caller compositing a run does not need two code paths: a 1-bit
 * strike becomes the same ::GFNT_Coverage a rasterised outline does, 0 or 255 per
 * pixel, with `left` and `top` carrying the strike's bearings and the fill rule
 * reported as ::GFNT_FILL_NONZERO because no rule was applied.
 *
 * This is the whole of the bridge. **Nothing here scales**: a strike used at a
 * size it was not drawn for is M9, and the decision of whether to do that at all
 * belongs to ::gfnt_face_select_strike() and its policy, not to a converter.
 *
 * @param glyph The bitmap glyph.
 * @param allocator Allocator for the pixels, or NULL for the default.
 * @param out_coverage Receives it; the caller frees it with
 *   ::gfnt_coverage_destroy(). Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_LIMIT or ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_coverage_from_bitmap(const GFNT_BitmapGlyph * glyph,
    const GFNT_Allocator * allocator, GFNT_Coverage * out_coverage,
    GFNT_Error * error);

/**
 * @brief Release a coverage's pixels.
 *
 * Leaves the struct zeroed, so a second call is harmless.
 *
 * @param coverage The coverage, or NULL.
 */
GFNT_API void gfnt_coverage_destroy(GFNT_Coverage * coverage);

/**
 * @brief One pixel's coverage.
 *
 * For a test or a diagnostic; a compositor walks rows.
 *
 * @param coverage The coverage, or NULL.
 * @param column From 0.
 * @param row From 0, top first.
 * @return The coverage, or 0 for NULL or a pixel outside - a pixel outside the
 *   bitmap is a pixel the glyph does not cover, which is a coverage of zero.
 */
GFNT_API uint8_t gfnt_coverage_at(const GFNT_Coverage * coverage,
    uint32_t column, uint32_t row);

/**
 * @brief The sum of every pixel's coverage.
 *
 * 255 times the glyph's area in pixels, to within the rasteriser's rounding.
 * That makes it the cheapest invariant a test can assert about a rendering -
 * a shape's area does not depend on where it is drawn - and it is what catches
 * a rasteriser that drops a span or counts one twice.
 *
 * @param coverage The coverage, or NULL.
 * @return The sum, or 0 for NULL.
 */
GFNT_API uint64_t gfnt_coverage_total(const GFNT_Coverage * coverage);

/**
 * @brief Apply a 256-entry lookup table to every pixel, in place.
 *
 * The gamma helper section 8.2 asks for, less the table: coverage is linear
 * alpha, and what curve to put it through depends on the space the caller
 * composites in - which this library does not know and must not guess. A
 * caller with an sRGB destination and one with a linear destination want
 * different tables and the same loop.
 *
 * @param coverage The coverage.
 * @param table 256 bytes: the new value for each old value.
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID.
 */
GFNT_API GFNT_Result gfnt_coverage_apply_table(GFNT_Coverage * coverage,
    const uint8_t * table);

/**
 * @brief A hash of everything a rendering is.
 *
 * The dimensions, the origin, the fill rule and every pixel, through FNV-1a.
 * This is what `make check-golden` commits: a hash that differs between this
 * machine and the big-endian container is a host-dependent read or a `float`
 * that crept in, and section 14.4 is the only gate that sees it.
 *
 * The function is specified, not just implemented: FNV-1a over the
 * little-endian spelling of each field followed by the rows, so that the hash
 * of a rendering is the same number on every platform.
 *
 * @param coverage The coverage, or NULL.
 * @return The hash; the hash of an empty coverage is the offset basis mixed
 *   with its zero dimensions, not zero.
 */
GFNT_API uint64_t gfnt_coverage_hash(const GFNT_Coverage * coverage);

/**
 * @brief Write the coverage's shape, origin and hash to a stream.
 *
 * @param coverage The coverage.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_coverage_dump(const GFNT_Coverage * coverage,
    FILE * out);

/**
 * @brief Write the coverage as rows of text, one character per pixel.
 *
 * For a human looking at a glyph that came out wrong, and for a test whose
 * failure message should show the shape rather than a hash. Ten levels, from
 * `.` for zero to `@` for full.
 *
 * @param coverage The coverage.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_coverage_dump_art(const GFNT_Coverage * coverage,
    FILE * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_RASTER_H
