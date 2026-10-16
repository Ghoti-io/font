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
 * The fixed-point arithmetic and the rounding of the TrueType virtual machine.
 *
 * Everything is integer, in 64 bits where a product needs it, and every result is
 * a defined 32-bit value on every platform: a sum that overflows wraps as the
 * machine's 32-bit words do, and no shift is of a negative number.
 */

#include "vm.h"

#define GFNT_HINT_FINE 10 /* extra fractional bits in the rounding grid */

static int32_t gfnt_hint_narrow(int64_t value) {
  return (int32_t)(uint32_t)((uint64_t)value & 0xFFFFFFFFu);
}

int32_t gfnt_hint_add(int32_t a, int32_t b) {
  return gfnt_hint_narrow((int64_t)a + b);
}

int32_t gfnt_hint_sub(int32_t a, int32_t b) {
  return gfnt_hint_narrow((int64_t)a - b);
}

/* |a| * |b| and the sign of the product, so that rounding is symmetric. */
static uint64_t gfnt_hint_abs_product(int32_t a, int32_t b, int * negative) {
  int64_t x = a;
  int64_t y = b;

  *negative = (x < 0) != (y < 0);
  x = x < 0 ? -x : x;
  y = y < 0 ? -y : y;
  return (uint64_t)x * (uint64_t)y;
}

int32_t gfnt_hint_muldiv(int32_t a, int32_t b, int32_t c) {
  int negative;
  uint64_t product;
  uint64_t divisor;
  uint64_t quotient;

  if (c == 0) {
    return 0;
  }
  product = gfnt_hint_abs_product(a, b, &negative);
  if (c < 0) {
    negative = !negative;
  }
  divisor = (uint64_t)(c < 0 ? -(int64_t)c : (int64_t)c);
  quotient = (product + divisor / 2u) / divisor;
  return gfnt_hint_narrow(negative ? -(int64_t)quotient : (int64_t)quotient);
}

int32_t gfnt_hint_muldiv_trunc(int32_t a, int32_t b, int32_t c) {
  int negative;
  uint64_t product;
  uint64_t divisor;
  uint64_t quotient;

  if (c == 0) {
    return 0;
  }
  product = gfnt_hint_abs_product(a, b, &negative);
  if (c < 0) {
    negative = !negative;
  }
  divisor = (uint64_t)(c < 0 ? -(int64_t)c : (int64_t)c);
  quotient = product / divisor;
  return gfnt_hint_narrow(negative ? -(int64_t)quotient : (int64_t)quotient);
}

int32_t gfnt_hint_mul_fix(int32_t a, int32_t b) {
  return gfnt_hint_muldiv(a, b, 0x10000);
}

int32_t gfnt_hint_div_fix(int32_t a, int32_t b) {
  return gfnt_hint_muldiv(a, 0x10000, b);
}

void gfnt_hint_gstate_default(GFNT_HintGState * gs) {
  gs->auto_flip = true;
  gs->cvt_cut_in = 68;      /* 17/16 pixel */
  gs->delta_base = 9;
  gs->delta_shift = 3;
  gs->loop = 1;
  gs->min_distance = 64;
  gs->sw_cut_in = 0;
  gs->sw_value = 0;
  gs->scan_control = 0;
  gs->scan_type = 0;
  gs->pv[0] = 0x4000;
  gs->pv[1] = 0;
  gs->fv[0] = 0x4000;
  gs->fv[1] = 0;
  gs->dpv[0] = 0x4000;
  gs->dpv[1] = 0;
  for (int i = 0; i < 3; ++i) {
    gs->rp[i] = 0;
    gs->zp[i] = 1;
  }
  gfnt_hint_set_round(gs, GFNT_HINT_ROUND_GRID);
}

/* Period, phase and threshold of the six plain modes, in 26.6 units. */
void gfnt_hint_set_round(GFNT_HintGState * gs, GFNT_HintRound mode) {
  int32_t period = 64;
  int32_t phase = 0;
  int32_t threshold = 32;

  switch (mode) {
    case GFNT_HINT_ROUND_HALF:   phase = 32; break;
    case GFNT_HINT_ROUND_DOUBLE: period = 32; threshold = 16; break;
    case GFNT_HINT_ROUND_DOWN:   threshold = 0; break;
    case GFNT_HINT_ROUND_UP:     threshold = 63; break;
    case GFNT_HINT_ROUND_OFF:
    case GFNT_HINT_ROUND_GRID:
    case GFNT_HINT_ROUND_SUPER:
    case GFNT_HINT_ROUND_SUPER45:
      break;
  }
  gs->round = mode;
  gs->period = period * (1 << GFNT_HINT_FINE);
  gs->phase = phase * (1 << GFNT_HINT_FINE);
  gs->threshold = threshold * (1 << GFNT_HINT_FINE);
}

void gfnt_hint_set_super_round(GFNT_HintGState * gs, int32_t operand,
    bool at_45_degrees) {
  /* Only the low eight bits mean anything. */
  uint32_t bits = (uint32_t)operand & 0xFFu;
  int64_t period;
  int64_t phase;
  int64_t threshold;
  const int64_t one = (int64_t)1 << GFNT_HINT_FINE;

  if (at_45_degrees) {
    /* sqrt(2)/2 of a pixel, in the grid's fine units: 64 * 1024 * 0.70710678. */
    period = 46341;
    period = (period * ((bits >> 6) == 0 ? 1 : (bits >> 6) == 1 ? 2 : 4)) / 2;
  }
  else {
    static const int64_t periods[4] = {32, 64, 128, 64};

    period = periods[bits >> 6] * one;
  }
  phase = ((bits >> 4) & 3u) * period / 4;
  if ((bits & 15u) == 0) {
    threshold = period - 1;
  }
  else {
    threshold = ((int64_t)(bits & 15u) - 4) * period / 8;
  }
  gs->round = at_45_degrees ? GFNT_HINT_ROUND_SUPER45 : GFNT_HINT_ROUND_SUPER;
  gs->period = (int32_t)period;
  gs->phase = (int32_t)phase;
  gs->threshold = (int32_t)threshold;
}

/* Floor division of a signed 64-bit numerator by a positive divisor. */
static int64_t gfnt_hint_floor_div(int64_t numerator, int64_t divisor) {
  int64_t quotient = numerator / divisor;

  if (numerator % divisor < 0) {
    --quotient;
  }
  return quotient;
}

int32_t gfnt_hint_round(const GFNT_HintGState * gs, int32_t value) {
  int64_t n;
  int64_t v;
  int negative;

  if (gs->round == GFNT_HINT_ROUND_OFF) {
    return value;
  }
  negative = value < 0;
  n = (int64_t)value * (1 << GFNT_HINT_FINE);
  if (negative) {
    n = -n;
  }
  v = gfnt_hint_floor_div(n - gs->phase + gs->threshold, gs->period)
      * gs->period + gs->phase;
  if (v < 0) {
    v = gs->phase;
  }
  /* Back to 26.6; the grids with a whole-unit period lose nothing. */
  v = (v + (1 << (GFNT_HINT_FINE - 1))) >> GFNT_HINT_FINE;
  return gfnt_hint_narrow(negative ? -v : v);
}
