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
 * What a `FontMatrix` says about the em. See matrix.h.
 */

#include "matrix.h"
#include "fixed.h"

bool gfnt_matrix_agrees_with_em(const GFNT_F16Dot16 * matrix, bool stated,
    size_t upem) {
  int64_t scale;
  int64_t implied;

  if (!stated) {
    return true;
  }
  if (!matrix) {
    return false;
  }
  if (matrix[1] != 0 || matrix[2] != 0 || matrix[4] != 0 || matrix[5] != 0) {
    return false;
  }
  scale = matrix[0];
  if (scale != matrix[3] || scale <= 0 || upem == 0) {
    return false;
  }
  implied = scale * (int64_t)upem;
  // One 16.16 unit per em unit of slack, which is what the decimal-to-binary
  // conversion of 1/upem can cost and no more.
  return implied >= GFNT_F16DOT16_ONE - (int64_t)upem
      && implied <= GFNT_F16DOT16_ONE + (int64_t)upem;
}

bool gfnt_matrix_units_per_em(const GFNT_F16Dot16 * matrix, bool stated,
    size_t * out_upem) {
  int64_t scale;
  size_t candidate;

  if (!out_upem) {
    return false;
  }
  // The format's own default, and exact: a font that states no FontMatrix is a
  // 1000-unit font by definition, which is what most of them are and what every
  // one of them is authored in.
  if (!stated) {
    *out_upem = 1000;
    return true;
  }
  if (!matrix) {
    return false;
  }
  if (gfnt_matrix_agrees_with_em(matrix, stated, 1000)) {
    *out_upem = 1000;
    return true;
  }
  // What is left has to be inverted, and 1/upem is a 16.16 approximation - so
  // inverting recovers the em only where the division is exact. It is for every
  // power of two (1/2048 is 32 exactly) and it is not for 1000: 0.001 is 65.536,
  // which a font writes as 65 or 66, and those invert to 1008 and 993. The check
  // above is what covers that, and it has to come first for that reason.
  scale = matrix[0];
  if (scale <= 0 || GFNT_F16DOT16_ONE % scale != 0) {
    return false;
  }
  candidate = (size_t)(GFNT_F16DOT16_ONE / scale);
  // The same bound `head` puts on an em, because this is the same quantity and a
  // caller cannot tell which part of the font it came from.
  if (candidate < 16 || candidate > 16384) {
    return false;
  }
  // And the matrix has to agree with the em just derived from it, which is not
  // circular: the test also requires the two diagonals to match and the
  // off-diagonals to be zero, so a matrix that skews or scales the axes
  // differently is refused rather than reduced to one number.
  if (!gfnt_matrix_agrees_with_em(matrix, stated, candidate)) {
    return false;
  }
  *out_upem = candidate;
  return true;
}
