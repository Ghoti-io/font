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
 * What a PostScript `FontMatrix` says about the em, in one place because two
 * formats state one.
 *
 * documentation/design.md section 7.4. A CFF Top DICT and a Type 1 font program
 * both carry a `FontMatrix`, both default it to 1/1000, and both mean the same
 * thing by it: the map from the charstring's own coordinate space to the em.
 * This library reports charstring coordinates, which is what every reference pen
 * reports, so the matrix is not applied - it is *read*, to learn how many of
 * those units make an em and to refuse a font where that question has no answer.
 *
 * Two copies of that rule is how a CFF and a Type 1 font of the same design come
 * to be scaled differently by one library.
 */

#ifndef GHOTI_IO_GFNT_MATRIX_H
#define GHOTI_IO_GFNT_MATRIX_H

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/core.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Whether a `FontMatrix` and an em stated elsewhere describe the same em.
 *
 * For every font anybody ships the two agree - a 1000-unit font says 0.001 - and
 * a font where they disagree would need the matrix applied, which would put this
 * library and every reference in different spaces while agreeing about nothing.
 * So the caller refuses such a font by name.
 *
 * The comparison is deliberately loose in the last bits: `0.001` is 65 or 66 in
 * 16.16 depending on how it was written and neither is 1/1000 exactly, so an
 * exact test would refuse every font in the world.
 *
 * @param matrix The six 16.16 values, in PostScript order.
 * @param stated Whether the font stated a matrix at all. An unstated one is the
 *   format's default and agrees with anything by definition.
 * @param upem The em another part of the font claims.
 * @return Whether to go on reading.
 */
bool gfnt_matrix_agrees_with_em(const GFNT_F16Dot16 * matrix, bool stated,
    size_t upem);

/**
 * How many charstring units a `FontMatrix` puts in an em.
 *
 * Where the em comes from when nothing else states one - a bare CFF or a Type 1
 * font program, neither of which has a `head`.
 *
 * @param matrix The six 16.16 values, in PostScript order.
 * @param stated Whether the font stated a matrix at all.
 * @param out_upem Receives the em.
 * @return true when the matrix states an em this library can report. false for
 *   one that does not reduce to a single number - a skew, different scales per
 *   axis, or a reciprocal 16.16 cannot hold exactly - which the caller must
 *   refuse rather than guess at, because drawing in the wrong em silently is the
 *   failure this returns false to prevent.
 */
bool gfnt_matrix_units_per_em(const GFNT_F16Dot16 * matrix, bool stated,
    size_t * out_upem);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_MATRIX_H
