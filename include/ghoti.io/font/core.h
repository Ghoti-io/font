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
 * Core types, result codes, fixed-point arithmetic, diagnostics, limits and
 * the version for the Ghoti.io Font library.
 *
 * The fixed-point types and their arithmetic are the mechanism behind this
 * library's determinism claim: see documentation/design.md section 5.2. No
 * core type is a `float`, and every product goes through a 64-bit
 * intermediate with a stated rounding rule, so the same font at the same size
 * produces the same numbers on every platform and at every optimisation
 * level.
 */

#ifndef GHOTI_IO_GFNT_CORE_H
#define GHOTI_IO_GFNT_CORE_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/macros.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result code for font library operations.
 *
 * The fixed vocabulary of CONVENTIONS.md section 5. Zero is success and
 * GFNT_RESULT_COUNT closes the enum so that a test can check the string table
 * is complete.
 *
 * documentation/design.md section 5.6 says what each means *here*, and the
 * distinctions are load-bearing: ::GFNT_ERR_FORMAT is "not a font",
 * ::GFNT_ERR_UNSUPPORTED is "a font, with a feature this library does not
 * implement", and ::GFNT_ERR_CORRUPT is "a font, with bytes that violate its
 * own specification". Collapsing them would make "this font is broken" and
 * "we do not do CFF2" one answer.
 */
typedef enum {
  GFNT_OK = 0,          ///< Operation succeeded.
  GFNT_ERR_IO,          ///< I/O error (read/write/seek failed).
  GFNT_ERR_FORMAT,      ///< Not a format this library recognises.
  GFNT_ERR_UNSUPPORTED, ///< This format, but a feature not implemented.
  GFNT_ERR_LIMIT,       ///< A GFNT_Limits field was exceeded.
  GFNT_ERR_CORRUPT,     ///< This format, but the bytes are wrong.
  GFNT_ERR_OOM,         ///< The allocator returned NULL.
  GFNT_ERR_INVALID,     ///< A caller-supplied argument is wrong.
  GFNT_ERR_INTERNAL,    ///< The library's own invariant failed.
  GFNT_RESULT_COUNT
} GFNT_Result;

/**
 * @brief Convert a result code to a human-readable string.
 *
 * The returned string is statically allocated and must not be freed.
 *
 * @param result The result code.
 * @return A description of the result code, never NULL.
 */
GFNT_API const char * gfnt_result_string(GFNT_Result result);

/**
 * @brief A four-character table tag, as every sfnt-derived format spells one.
 *
 * Held in the order the bytes appear in the file, so that GFNT_TAG() and a
 * big-endian read of four bytes produce the same number on every platform.
 */
typedef uint32_t GFNT_Tag;

/**
 * @brief Build a ::GFNT_Tag from its four characters.
 *
 * `GFNT_TAG('c', 'm', 'a', 'p')` is the `cmap` table's tag. Note that several
 * tags are padded with spaces in the file - `CFF `, `OS/2` is not - so the
 * fourth character matters.
 */
#define GFNT_TAG(a, b, c, d)                                                   \
  ((GFNT_Tag)(((uint32_t)(unsigned char)(a) << 24)                             \
      | ((uint32_t)(unsigned char)(b) << 16)                                   \
      | ((uint32_t)(unsigned char)(c) << 8)                                    \
      | (uint32_t)(unsigned char)(d)))

/**
 * @brief Spell a tag as five bytes: four characters and a NUL.
 *
 * For diagnostics, so a byte a terminal would not survive is not written to
 * one: any byte outside printable ASCII becomes `.`. A tag is therefore not
 * recoverable from its string, which is deliberate - the number is the
 * identity and this is the label.
 *
 * @param tag The tag.
 * @param out Receives five bytes: four characters and a terminating NUL.
 *   NULL is ignored.
 * @return @p out, so that this can be used inside a printf argument list,
 *   or NULL if @p out was NULL.
 */
GFNT_API char * gfnt_tag_string(GFNT_Tag tag, char * out);

/**
 * @brief Glyph index meaning "no glyph", for a diagnostic that names no glyph.
 *
 * Glyph 0 is `.notdef` and is a real glyph (documentation/design.md section
 * 5.4), so zero cannot serve as the absent value. sfnt glyph indices are 16
 * bits, so this cannot collide with one.
 */
#define GFNT_GLYPH_NONE 0xFFFFFFFFu

/**
 * @brief Where a failure happened, beyond which failure it was.
 *
 * "Corrupt" is not a diagnostic (documentation/design.md section 5.6): the
 * table, the offset within that table, and the glyph if one was involved are
 * what make a malformed font actionable. Every parser takes an optional
 * `GFNT_Error *`; NULL means the caller does not want one, and the result code
 * is the same either way.
 *
 * The offset is within the **table**, not the file, because that is the extent
 * the reader validated against and the number a table dump can be compared
 * with. `message` is always a static string - a diagnostic that can fail to
 * allocate is a diagnostic that disappears when it is most wanted.
 */
typedef struct GFNT_Error {
  GFNT_Result result;   ///< The failure. ::GFNT_OK when nothing is recorded.
  GFNT_Tag table;       ///< Table it happened in, or 0 for none.
  size_t offset;        ///< Byte offset within that table.
  uint32_t glyph;       ///< Glyph involved, or ::GFNT_GLYPH_NONE.
  const char * message; ///< Static description, or NULL when nothing is
                        ///< recorded.
} GFNT_Error;

/**
 * @brief Clear a diagnostic to "nothing recorded".
 *
 * @param error The diagnostic, or NULL.
 */
GFNT_API void gfnt_error_clear(GFNT_Error * error);

/**
 * @brief Record a diagnostic and hand back its result code.
 *
 * Returns @p result so that a failing parser can both report and return in one
 * statement, which is what keeps the diagnostic from being forgotten at some
 * of the call sites and not others.
 *
 * @param error Receives the diagnostic. NULL is accepted and ignored.
 * @param result The failure.
 * @param table The table it happened in, or 0.
 * @param offset The byte offset within that table.
 * @param glyph The glyph involved, or ::GFNT_GLYPH_NONE.
 * @param message A static string. Not copied.
 * @return @p result.
 */
GFNT_API GFNT_Result gfnt_error_set(GFNT_Error * error, GFNT_Result result,
    GFNT_Tag table, size_t offset, uint32_t glyph, const char * message);

/**
 * @brief Write a diagnostic to a stream, one line, human-readable.
 *
 * @param error The diagnostic, or NULL.
 * @param out The stream. NULL is ignored.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID if either argument is NULL, or
 *   ::GFNT_ERR_IO if the stream refused a write.
 */
GFNT_API GFNT_Result gfnt_error_dump(const GFNT_Error * error, FILE * out);

/**
 * @brief Signed 26.6 fixed point: an `int32_t` in units of 1/64.
 *
 * Pixel-space coordinates and advances, and font-unit outlines multiplied by
 * 64 so that CFF and variation fractions survive.
 */
typedef int32_t GFNT_F26Dot6;

/**
 * @brief Signed 16.16 fixed point: an `int32_t` in units of 1/65536.
 *
 * Scale factors and transforms. This is the sfnt specification's `Fixed`.
 */
typedef int32_t GFNT_F16Dot16;

/**
 * @brief Signed 2.14 fixed point: an `int16_t` in units of 1/16384.
 *
 * Variation coordinates and composite glyph transforms, the format's own
 * type for both.
 */
typedef int16_t GFNT_F2Dot14;

/** @brief 1.0 in 26.6. */
#define GFNT_F26DOT6_ONE 64
/** @brief 1.0 in 16.16. */
#define GFNT_F16DOT16_ONE 65536
/** @brief 1.0 in 2.14. */
#define GFNT_F2DOT14_ONE 16384

/**
 * @brief Multiply two 16.16 values, rounding half away from zero.
 *
 * The product is formed in `int64_t` and rounded half away from zero, which is
 * FreeType's `FT_MulFix` rule and the one every table of expected values in
 * this library's tests is generated against. A result outside the range of an
 * `int32_t` saturates to ::INT32_MIN or ::INT32_MAX rather than wrapping: the
 * inputs that reach it come from a font file, wrapping is undefined behaviour
 * on the signed type, and a saturated coordinate is bounded nonsense where a
 * wrapped one is unbounded nonsense.
 *
 * @param a First factor.
 * @param b Second factor.
 * @return The product, in 16.16.
 */
GFNT_API GFNT_F16Dot16 gfnt_f16dot16_mul(GFNT_F16Dot16 a, GFNT_F16Dot16 b);

/**
 * @brief Divide one 16.16 value by another, rounding half away from zero.
 *
 * @param a Numerator.
 * @param b Denominator. Zero yields ::INT32_MAX or ::INT32_MIN by the sign of
 *   @p a, and 0 for a zero numerator, so that a font whose metrics divide by
 *   zero produces a bounded number rather than a trap.
 * @return The quotient, in 16.16.
 */
GFNT_API GFNT_F16Dot16 gfnt_f16dot16_div(GFNT_F16Dot16 a, GFNT_F16Dot16 b);

/**
 * @brief Widen a 2.14 value to 16.16, exactly.
 *
 * @param value The 2.14 value.
 * @return The same number in 16.16.
 */
GFNT_API GFNT_F16Dot16 gfnt_f2dot14_to_f16dot16(GFNT_F2Dot14 value);

/**
 * @brief The scale that maps one font unit to pixels, in 16.16.
 *
 * @param units_per_em The face's `unitsPerEm`. Zero yields 0, because a face
 *   with no em cannot scale and the caller has a diagnostic for it already.
 * @param ppem The pixel size.
 * @return Pixels per font unit, in 16.16.
 */
GFNT_API GFNT_F16Dot16 gfnt_scale_for_ppem(uint16_t units_per_em,
    uint32_t ppem);

/**
 * @brief Scale a font-unit quantity to 26.6 pixels.
 *
 * The one place font units become pixels, so that measuring and painting
 * cannot round differently (documentation/design.md section 10.2, M5).
 *
 * @param units The quantity in font units.
 * @param scale Pixels per font unit, from ::gfnt_scale_for_ppem().
 * @return The quantity in 26.6 pixels, rounded half away from zero.
 */
GFNT_API GFNT_F26Dot6 gfnt_units_to_pixels(int32_t units,
    GFNT_F16Dot16 scale);

/**
 * @brief Round a 26.6 value to whole pixels, half away from zero.
 *
 * @param value The value.
 * @return The value in whole pixels.
 */
GFNT_API int32_t gfnt_f26dot6_round(GFNT_F26Dot6 value);

/**
 * @brief The largest whole pixel not greater than a 26.6 value.
 *
 * @param value The value.
 * @return The floor, in whole pixels.
 */
GFNT_API int32_t gfnt_f26dot6_floor(GFNT_F26Dot6 value);

/**
 * @brief The smallest whole pixel not less than a 26.6 value.
 *
 * @param value The value.
 * @return The ceiling, in whole pixels.
 */
GFNT_API int32_t gfnt_f26dot6_ceil(GFNT_F26Dot6 value);

/**
 * @brief Caps applied while reading a font, so that a hostile file
 * cannot make the library allocate, recurse or loop without bound. Every
 * parser takes one; NULL means the defaults. See documentation/design.md
 * section 15.2, and section 6.2 for the recursion budgets among them.
 */
typedef struct GFNT_Limits {
  size_t max_blob_bytes;      ///< Largest font file accepted. 256 MiB.
  size_t max_tables;          ///< sfnt table directory entries. 512.
  size_t max_glyphs;          ///< Glyphs per face; the format's own 65,535.
  size_t max_composite_depth; ///< Composite glyph nesting. 16.
  size_t max_outline_points;  ///< Points in one glyph's outline. 65,536.
  size_t max_contours;        ///< Contours in one glyph. 4,096.
  size_t max_ppem;            ///< Largest pixel size rasterised. 4,096.
  size_t max_raster_bytes;    ///< Coverage bytes for one glyph. 64 MiB.
  size_t max_strikes;         ///< Bitmap strikes per face. 256.
  size_t max_name_records;    ///< `name` table records. 4,096.
  size_t max_axes;            ///< Variation axes. 64.
  size_t max_lookup_depth;    ///< Nested GSUB/GPOS lookups. 6.
  size_t max_ops_per_glyph;   ///< Substitutions at one position. 64.
  size_t max_paint_depth;     ///< COLR v1 paint graph depth. 64.
  size_t max_run_bytes;       ///< Text handed to one shaping call. 16 MiB.
  size_t max_line_length;     ///< BDF and .hex line length. 4,096.
} GFNT_Limits;

/**
 * @brief Fill in the default limits.
 *
 * @param limits Structure to populate. NULL is ignored.
 */
GFNT_API void gfnt_limits_default(GFNT_Limits * limits);

/**
 * @brief This build's version, as the string the Makefile generated.
 *
 * "0.0.0", or "0.0.0-dev" for a build with an overridden BRANCH. The same
 * string is GFNT_VERSION_STRING at compile time; this is the one the linked
 * library reports, which is the one that matters when the two differ.
 *
 * @return A static string, never NULL.
 */
GFNT_API const char * gfnt_version_string(void);

/**
 * @brief This build's version, packed as GFNT_MAKE_VERSION() packs it.
 *
 * @return `(major << 16) | (minor << 8) | patch`.
 */
GFNT_API unsigned gfnt_version_number(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_CORE_H
