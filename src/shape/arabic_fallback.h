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
 * Arabic shaping for a font that has no joining features: HarfBuzz's
 * `hb-ot-shape-complex-arabic-fallback.cc`.
 *
 * A font from before OpenType Arabic maps the *presentation forms* - U+FE91
 * initial BEH, U+FEFB isolated LAM ALEF - and expects the shaper to choose among
 * them. The choice is the same one `init`, `medi` and `fina` make, so this builds
 * the lookups those features would have held, as a real `GSUB` table in memory,
 * from `unicode`'s compatibility decompositions and the font's own `cmap`, and runs
 * them through the same engine as the font's own.
 */

#ifndef GHOTI_IO_GFNT_ARABIC_FALLBACK_H
#define GHOTI_IO_GFNT_ARABIC_FALLBACK_H

#include <ghoti.io/font/macros.h>
#include "plan.h"

/** The lookups, in order: isol, fina, init, medi, the letter ligatures (`rlig`) and the mark ligatures (also `rlig`'s). */
#define GFNT_AR_FALLBACK_LOOKUPS 6

/**
 * Apply the fallback to the run.
 *
 * @param masks The plan's mask for each of isol, fina, init, medi, rlig (twice), in
 *   that order. A zero mask skips its lookup.
 */
void gfnt_arabic_fallback_shape(GFNT_ShapeCtx * ctx,
    const uint32_t masks[GFNT_AR_FALLBACK_LOOKUPS]);

#endif
