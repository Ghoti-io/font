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
 * Fixed-point arithmetic: 26.6, 16.16 and 2.14, with a stated rounding rule
 * and no floating point anywhere.
 *
 * documentation/design.md section 5.2. Every product is formed in `int64_t`
 * and rounded half away from zero, which is FreeType's `FT_MulFix` rule;
 * every result that will not fit an `int32_t` saturates rather than wrapping,
 * because the inputs come from a font file and signed overflow is undefined
 * behaviour.
 *
 * The negative cases are written as division and remainder rather than as
 * shifts: `>>` on a negative value is implementation-defined in C, and this
 * library's whole determinism claim is that two platforms agree.
 */

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/core.h>
#include <stdbool.h>

/**
 * Clamp a 64-bit intermediate into an int32_t.
 */
static int32_t gfnt_saturate32(int64_t value) {
  if (value > INT32_MAX) {
    return INT32_MAX;
  }
  if (value < INT32_MIN) {
    return INT32_MIN;
  }
  return (int32_t)value;
}

/**
 * Divide by 2^shift, rounding half away from zero.
 *
 * The callers' products are bounded by 2^62, so negating one is safe.
 */
static int64_t gfnt_round_shift(int64_t product, int shift) {
  int64_t half = (int64_t)1 << (shift - 1);

  if (product >= 0) {
    return (product + half) >> shift;
  }
  return -((-product + half) >> shift);
}

/**
 * Divide, rounding half away from zero, with both signs handled explicitly.
 */
static int64_t gfnt_round_div(int64_t numerator, int64_t denominator) {
  uint64_t n = (uint64_t)(numerator < 0 ? -numerator : numerator);
  uint64_t d = (uint64_t)(denominator < 0 ? -denominator : denominator);
  uint64_t quotient = (n + d / 2) / d;
  bool negative = (numerator < 0) != (denominator < 0);

  return negative ? -(int64_t)quotient : (int64_t)quotient;
}

GFNT_F16Dot16 gfnt_f16dot16_mul(GFNT_F16Dot16 a, GFNT_F16Dot16 b) {
  return gfnt_saturate32(gfnt_round_shift((int64_t)a * (int64_t)b, 16));
}

GFNT_F16Dot16 gfnt_f16dot16_div(GFNT_F16Dot16 a, GFNT_F16Dot16 b) {
  if (b == 0) {
    // A font whose metrics divide by zero gets a bounded number, not a trap.
    if (a > 0) {
      return INT32_MAX;
    }
    return a < 0 ? INT32_MIN : 0;
  }
  return gfnt_saturate32(gfnt_round_div((int64_t)a * GFNT_F16DOT16_ONE, b));
}

GFNT_F16Dot16 gfnt_f2dot14_to_f16dot16(GFNT_F2Dot14 value) {
  // 1/16384 to 1/65536 is exactly four times as many units; no rounding.
  return (GFNT_F16Dot16)((int32_t)value * 4);
}

GFNT_F16Dot16 gfnt_scale_for_ppem(uint16_t units_per_em, uint32_t ppem) {
  if (units_per_em == 0) {
    return 0;
  }
  return gfnt_saturate32(gfnt_round_div(
      (int64_t)ppem * GFNT_F16DOT16_ONE, (int64_t)units_per_em));
}

GFNT_F26Dot6 gfnt_units_to_pixels(int32_t units, GFNT_F16Dot16 scale) {
  // units * scale is pixels in 16.16; 26.6 pixels is that divided by 1024.
  return gfnt_saturate32(
      gfnt_round_shift((int64_t)units * (int64_t)scale, 10));
}

int32_t gfnt_f26dot6_round(GFNT_F26Dot6 value) {
  return (int32_t)gfnt_round_div((int64_t)value, GFNT_F26DOT6_ONE);
}

int32_t gfnt_f26dot6_floor(GFNT_F26Dot6 value) {
  int64_t quotient = (int64_t)value / GFNT_F26DOT6_ONE;

  if (value % GFNT_F26DOT6_ONE != 0 && value < 0) {
    quotient -= 1;
  }
  return (int32_t)quotient;
}

int32_t gfnt_f26dot6_ceil(GFNT_F26Dot6 value) {
  int64_t quotient = (int64_t)value / GFNT_F26DOT6_ONE;

  if (value % GFNT_F26DOT6_ONE != 0 && value > 0) {
    quotient += 1;
  }
  return (int32_t)quotient;
}
