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
 * The rounding and saturation rules, in one place because more than one
 * translation unit rounds.
 *
 * documentation/design.md section 5.2 states one rule - form the product in
 * `int64_t`, round half away from zero, saturate rather than wrap - and the
 * determinism claim is that every platform agrees about it. Two copies of that
 * rule is how the outline transform and the metric scale come to disagree in
 * the last bit, which is a half-pixel seam between a composite's components
 * and no error anywhere. `static inline` rather than exported: these are three
 * arithmetic expressions, not API.
 *
 * Negative cases are written as division and remainder rather than as shifts:
 * `>>` on a negative value is implementation-defined in C.
 */

#ifndef GHOTI_IO_GFNT_FIXED_H
#define GHOTI_IO_GFNT_FIXED_H

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/core.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Clamp a 64-bit intermediate into an int32_t.
 */
static inline int32_t gfnt_saturate32(int64_t value) {
  if (value > INT32_MAX) {
    return INT32_MAX;
  }
  if (value < INT32_MIN) {
    return INT32_MIN;
  }
  return (int32_t)value;
}

/**
 * The largest magnitude a value may have and still survive gfnt_round_shift().
 *
 * Rounding adds half a unit before shifting, so a value within half a unit of
 * INT64_MAX would overflow while doing it. Anything this large saturates to
 * INT32_MAX afterwards regardless, so clamping here costs no precision that
 * survives the shift.
 */
#define GFNT_FIXED_CLAMP64 (INT64_MAX / 4)

/**
 * Clamp a 64-bit intermediate to a magnitude gfnt_round_shift() accepts.
 */
static inline int64_t gfnt_clamp64(int64_t value) {
  if (value > GFNT_FIXED_CLAMP64) {
    return GFNT_FIXED_CLAMP64;
  }
  if (value < -GFNT_FIXED_CLAMP64) {
    return -GFNT_FIXED_CLAMP64;
  }
  return value;
}

/**
 * Add two 64-bit intermediates, saturating at ::GFNT_FIXED_CLAMP64.
 *
 * Each of this library's products is bounded by 2^62 - both factors are
 * `int32_t` - so it is their *sum* that can leave the type, which is what a
 * two-term matrix row computes.
 */
static inline int64_t gfnt_add_clamp64(int64_t a, int64_t b) {
  return gfnt_clamp64(gfnt_clamp64(a) + gfnt_clamp64(b));
}

/**
 * Divide by 2^shift, rounding half away from zero.
 *
 * @param product The value, whose magnitude must be at most
 *   ::GFNT_FIXED_CLAMP64.
 * @param shift How many bits, at least 1.
 */
static inline int64_t gfnt_round_shift(int64_t product, int shift) {
  int64_t half = (int64_t)1 << (shift - 1);

  if (product >= 0) {
    return (product + half) >> shift;
  }
  return -((-product + half) >> shift);
}

/**
 * Divide, rounding half away from zero, with both signs handled explicitly.
 */
static inline int64_t gfnt_round_div(int64_t numerator, int64_t denominator) {
  uint64_t n = (uint64_t)(numerator < 0 ? -numerator : numerator);
  uint64_t d = (uint64_t)(denominator < 0 ? -denominator : denominator);
  uint64_t quotient = (n + d / 2) / d;
  bool negative = (numerator < 0) != (denominator < 0);

  return negative ? -(int64_t)quotient : (int64_t)quotient;
}

/**
 * Divide, rounding towards negative infinity.
 *
 * What a coverage cell's row and column want: a pixel boundary is a floor, and
 * rounding a negative coordinate towards zero puts two different y values in
 * the same scanline on one side of the origin and not the other.
 */
static inline int64_t gfnt_floor_div(int64_t numerator, int64_t denominator) {
  int64_t quotient = numerator / denominator;

  if (numerator % denominator != 0
      && ((numerator < 0) != (denominator < 0))) {
    quotient -= 1;
  }
  return quotient;
}

/**
 * Divide, rounding towards positive infinity.
 */
static inline int64_t gfnt_ceil_div(int64_t numerator, int64_t denominator) {
  int64_t quotient = numerator / denominator;

  if (numerator % denominator != 0
      && ((numerator < 0) == (denominator < 0))) {
    quotient += 1;
  }
  return quotient;
}

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_FIXED_H
