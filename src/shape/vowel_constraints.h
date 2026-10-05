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
 * Vowel sequences that are spelled with one character, which HarfBuzz sets off
 * with a dotted circle.
 */

#ifndef GHOTI_IO_GFNT_VOWEL_CONSTRAINTS_H
#define GHOTI_IO_GFNT_VOWEL_CONSTRAINTS_H

#include <ghoti.io/font/macros.h>
#include "plan.h"

/**
 * Put a dotted circle between two characters of a run that a script has a single
 * character for the pair of: an independent vowel followed by the dependent sign
 * that adds up to another independent vowel, and the like. The circle takes the
 * cluster of the character it comes before.
 */
void gfnt_vowel_constraints(GFNT_ShapeCtx * ctx, GFNT_LInfo ** chars,
    size_t * len, size_t * capacity);

#endif // GHOTI_IO_GFNT_VOWEL_CONSTRAINTS_H
