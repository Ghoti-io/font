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
 * `MATH`: the constants, glyph metrics, size variants and glyph assemblies a formula
 * typesetter needs from a math font.
 *
 * Nothing is parsed ahead of a call; a lookup reads the header and the one record it is asked
 * for, so none of this allocates. Every value is in font units (a percentage constant is the
 * percentage). A value that carries a `Device` table is adjusted by it when the table is a
 * `VariationIndex` and a variation is given; a hinting device table (for a pixel size) is
 * ignored, as the value record's own number is the unhinted design value.
 *
 * A glyph the table says nothing about is not an error: the lookups report `present` false or a
 * count of zero. What the table leaves to the caller (a missing italics correction is zero, a
 * missing top-accent attachment is half the advance, and which kern applies at a height) is
 * not decided here; the kern lookups hand over the table's own heights and values.
 *
 * Reference: OpenType Specification 1.9, "MATH - The Mathematical Typesetting Table".
 */

#ifndef GHOTI_IO_GFNT_MATH_H
#define GHOTI_IO_GFNT_MATH_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The `MathConstants` entries, in the order the table stores them. */
typedef enum GFNT_MathConstant {
  GFNT_MATH_SCRIPT_PERCENT_SCALE_DOWN = 0,
  GFNT_MATH_SCRIPT_SCRIPT_PERCENT_SCALE_DOWN = 1,
  GFNT_MATH_DELIMITED_SUB_FORMULA_MIN_HEIGHT = 2,
  GFNT_MATH_DISPLAY_OPERATOR_MIN_HEIGHT = 3,
  GFNT_MATH_MATH_LEADING = 4,
  GFNT_MATH_AXIS_HEIGHT = 5,
  GFNT_MATH_ACCENT_BASE_HEIGHT = 6,
  GFNT_MATH_FLATTENED_ACCENT_BASE_HEIGHT = 7,
  GFNT_MATH_SUBSCRIPT_SHIFT_DOWN = 8,
  GFNT_MATH_SUBSCRIPT_TOP_MAX = 9,
  GFNT_MATH_SUBSCRIPT_BASELINE_DROP_MIN = 10,
  GFNT_MATH_SUPERSCRIPT_SHIFT_UP = 11,
  GFNT_MATH_SUPERSCRIPT_SHIFT_UP_CRAMPED = 12,
  GFNT_MATH_SUPERSCRIPT_BOTTOM_MIN = 13,
  GFNT_MATH_SUPERSCRIPT_BASELINE_DROP_MAX = 14,
  GFNT_MATH_SUB_SUPERSCRIPT_GAP_MIN = 15,
  GFNT_MATH_SUPERSCRIPT_BOTTOM_MAX_WITH_SUBSCRIPT = 16,
  GFNT_MATH_SPACE_AFTER_SCRIPT = 17,
  GFNT_MATH_UPPER_LIMIT_GAP_MIN = 18,
  GFNT_MATH_UPPER_LIMIT_BASELINE_RISE_MIN = 19,
  GFNT_MATH_LOWER_LIMIT_GAP_MIN = 20,
  GFNT_MATH_LOWER_LIMIT_BASELINE_DROP_MIN = 21,
  GFNT_MATH_STACK_TOP_SHIFT_UP = 22,
  GFNT_MATH_STACK_TOP_DISPLAY_STYLE_SHIFT_UP = 23,
  GFNT_MATH_STACK_BOTTOM_SHIFT_DOWN = 24,
  GFNT_MATH_STACK_BOTTOM_DISPLAY_STYLE_SHIFT_DOWN = 25,
  GFNT_MATH_STACK_GAP_MIN = 26,
  GFNT_MATH_STACK_DISPLAY_STYLE_GAP_MIN = 27,
  GFNT_MATH_STRETCH_STACK_TOP_SHIFT_UP = 28,
  GFNT_MATH_STRETCH_STACK_BOTTOM_SHIFT_DOWN = 29,
  GFNT_MATH_STRETCH_STACK_GAP_ABOVE_MIN = 30,
  GFNT_MATH_STRETCH_STACK_GAP_BELOW_MIN = 31,
  GFNT_MATH_FRACTION_NUMERATOR_SHIFT_UP = 32,
  GFNT_MATH_FRACTION_NUMERATOR_DISPLAY_STYLE_SHIFT_UP = 33,
  GFNT_MATH_FRACTION_DENOMINATOR_SHIFT_DOWN = 34,
  GFNT_MATH_FRACTION_DENOMINATOR_DISPLAY_STYLE_SHIFT_DOWN = 35,
  GFNT_MATH_FRACTION_NUMERATOR_GAP_MIN = 36,
  GFNT_MATH_FRACTION_NUM_DISPLAY_STYLE_GAP_MIN = 37,
  GFNT_MATH_FRACTION_RULE_THICKNESS = 38,
  GFNT_MATH_FRACTION_DENOMINATOR_GAP_MIN = 39,
  GFNT_MATH_FRACTION_DENOM_DISPLAY_STYLE_GAP_MIN = 40,
  GFNT_MATH_SKEWED_FRACTION_HORIZONTAL_GAP = 41,
  GFNT_MATH_SKEWED_FRACTION_VERTICAL_GAP = 42,
  GFNT_MATH_OVERBAR_VERTICAL_GAP = 43,
  GFNT_MATH_OVERBAR_RULE_THICKNESS = 44,
  GFNT_MATH_OVERBAR_EXTRA_ASCENDER = 45,
  GFNT_MATH_UNDERBAR_VERTICAL_GAP = 46,
  GFNT_MATH_UNDERBAR_RULE_THICKNESS = 47,
  GFNT_MATH_UNDERBAR_EXTRA_DESCENDER = 48,
  GFNT_MATH_RADICAL_VERTICAL_GAP = 49,
  GFNT_MATH_RADICAL_DISPLAY_STYLE_VERTICAL_GAP = 50,
  GFNT_MATH_RADICAL_RULE_THICKNESS = 51,
  GFNT_MATH_RADICAL_EXTRA_ASCENDER = 52,
  GFNT_MATH_RADICAL_KERN_BEFORE_DEGREE = 53,
  GFNT_MATH_RADICAL_KERN_AFTER_DEGREE = 54,
  GFNT_MATH_RADICAL_DEGREE_BOTTOM_RAISE_PERCENT = 55,
  GFNT_MATH_CONSTANT_COUNT = 56
} GFNT_MathConstant;

/** @brief Which per-glyph value a lookup reads. */
typedef enum GFNT_MathGlyphValue {
  GFNT_MATH_ITALICS_CORRECTION,       ///< `MathItalicsCorrectionInfo`.
  GFNT_MATH_TOP_ACCENT_ATTACHMENT     ///< `MathTopAccentAttachment`.
} GFNT_MathGlyphValue;

/** @brief A corner of a glyph, in the order `MathKernInfoRecord` stores them. */
typedef enum GFNT_MathKernCorner {
  GFNT_MATH_KERN_TOP_RIGHT,
  GFNT_MATH_KERN_TOP_LEFT,
  GFNT_MATH_KERN_BOTTOM_RIGHT,
  GFNT_MATH_KERN_BOTTOM_LEFT
} GFNT_MathKernCorner;

/** @brief A larger form of a glyph. */
typedef struct GFNT_MathVariant {
  uint32_t glyph;     ///< The variant glyph.
  uint32_t advance;   ///< Its size along the direction it grows, in font units.
} GFNT_MathVariant;

/** @brief ::GFNT_MathPart::flags bit 0: the part may be repeated. */
#define GFNT_MATH_PART_EXTENDER 0x0001u

/** @brief One part of a glyph assembly, in the order the table lists them (start to end). */
typedef struct GFNT_MathPart {
  uint32_t glyph;
  uint32_t start_connector_length;
  uint32_t end_connector_length;
  uint32_t full_advance;
  uint16_t flags;     ///< ::GFNT_MATH_PART_EXTENDER, and bits the format reserves.
} GFNT_MathPart;

/**
 * @brief Whether the face has a `MATH`.
 *
 * @param face The face, or NULL.
 * @return true if the table directory has one.
 */
GFNT_API bool gfnt_face_has_math(const GFNT_Face * face);

/**
 * @brief One entry of `MathConstants`.
 *
 * @param face The face.
 * @param variation Where in the design space, or NULL for the default.
 * @param constant Which.
 * @param out_value Receives the value. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED for a face with no `MATH` (or
 *   a version this library does not read), or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_math_constant(const GFNT_Face * face,
    const GFNT_Variation * variation, GFNT_MathConstant constant, int32_t * out_value,
    GFNT_Error * error);

/**
 * @brief A glyph's italics correction or top-accent attachment.
 *
 * @param out_present Receives whether the table lists the glyph. Written only on success.
 * @param out_value Receives the value when it is present, else zero. Written only on success.
 */
GFNT_API GFNT_Result gfnt_face_math_glyph_value(const GFNT_Face * face,
    const GFNT_Variation * variation, uint32_t glyph, GFNT_MathGlyphValue which,
    bool * out_present, int32_t * out_value, GFNT_Error * error);

/** @brief Whether the table lists the glyph in its extended-shape coverage. */
GFNT_API GFNT_Result gfnt_face_math_is_extended_shape(const GFNT_Face * face, uint32_t glyph,
    bool * out_extended, GFNT_Error * error);

/**
 * @brief How many correction heights a glyph's kern at one corner has.
 *
 * There is one more kern value than there are heights. A glyph or corner the table does not
 * list gives @p out_present false.
 */
GFNT_API GFNT_Result gfnt_face_math_kern_count(const GFNT_Face * face, uint32_t glyph,
    GFNT_MathKernCorner corner, bool * out_present, size_t * out_heights, GFNT_Error * error);

/** @brief The height at @p index (below the count of heights). */
GFNT_API GFNT_Result gfnt_face_math_kern_height_at(const GFNT_Face * face,
    const GFNT_Variation * variation, uint32_t glyph, GFNT_MathKernCorner corner, size_t index,
    int32_t * out_height, GFNT_Error * error);

/** @brief The kern value at @p index (up to and including the count of heights). */
GFNT_API GFNT_Result gfnt_face_math_kern_value_at(const GFNT_Face * face,
    const GFNT_Variation * variation, uint32_t glyph, GFNT_MathKernCorner corner, size_t index,
    int32_t * out_kern, GFNT_Error * error);

/** @brief `MinConnectorOverlap`: how far connected parts must overlap, in font units. */
GFNT_API GFNT_Result gfnt_face_math_min_connector_overlap(const GFNT_Face * face,
    uint32_t * out_overlap, GFNT_Error * error);

/** @brief How many larger forms a glyph has in a direction (vertical or horizontal); 0 if none. */
GFNT_API GFNT_Result gfnt_face_math_variant_count(const GFNT_Face * face, uint32_t glyph,
    bool vertical, size_t * out_count, GFNT_Error * error);

/** @brief The variant at @p index, in the table's order (smallest first). */
GFNT_API GFNT_Result gfnt_face_math_variant_at(const GFNT_Face * face, uint32_t glyph,
    bool vertical, size_t index, GFNT_MathVariant * out_variant, GFNT_Error * error);

/** @brief How many parts a glyph's assembly has in a direction; 0 if it has none. */
GFNT_API GFNT_Result gfnt_face_math_assembly_count(const GFNT_Face * face, uint32_t glyph,
    bool vertical, size_t * out_count, GFNT_Error * error);

/** @brief The italics correction of the assembled glyph. The assembly must exist. */
GFNT_API GFNT_Result gfnt_face_math_assembly_italics_correction(const GFNT_Face * face,
    const GFNT_Variation * variation, uint32_t glyph, bool vertical, int32_t * out_value,
    GFNT_Error * error);

/** @brief The part at @p index. */
GFNT_API GFNT_Result gfnt_face_math_assembly_part_at(const GFNT_Face * face, uint32_t glyph,
    bool vertical, size_t index, GFNT_MathPart * out_part, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_MATH_H
