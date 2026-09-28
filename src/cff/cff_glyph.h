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
 * Where the CFF container meets the charstring interpreter: one glyph's
 * program, its subroutines, and the outline it draws.
 *
 * Tier 1, unlike `cff.h`, because this is the half that produces a
 * ::GFNT_Outline. Keeping the two apart is what lets `sfnt.h` hold a parsed CFF
 * in a face without tier 0 ever including `outline.h`.
 */

#ifndef GHOTI_IO_GFNT_CFF_GLYPH_H
#define GHOTI_IO_GFNT_CFF_GLYPH_H

#include <ghoti.io/font/charstring.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/outline.h>
#include "cff.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Draw one CFF glyph into @p outline.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param outline Receives the contours, appended.
 * @param out_metrics Receives what the charstring said about its advance, or
 *   NULL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED for a face with no `CFF `, or one
 *   whose `FontMatrix` disagrees with `head.unitsPerEm`; ::GFNT_ERR_INVALID for
 *   a glyph the font does not have; ::GFNT_ERR_CORRUPT; ::GFNT_ERR_LIMIT; or
 *   ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_cff_load(const GFNT_Face * face, uint32_t glyph,
    GFNT_Outline * outline, GFNT_CharstringMetrics * out_metrics,
    GFNT_Error * error);

/**
 * Whether this glyph's charstring assembles an accented character.
 *
 * CFF has no composite glyph in `glyf`'s sense: the nearest thing is `seac`, or
 * `endchar` with four operands, which draws two other glyphs. Answering it means
 * **running the program**, because whether the operator is reached depends on
 * the subroutines the program calls - so this costs what drawing the glyph
 * costs, and is a diagnostic rather than something to call per glyph in a
 * rendering loop.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param out_composite Receives true for an accented character.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return As ::gfnt_cff_load().
 */
GFNT_Result gfnt_cff_is_composite(const GFNT_Face * face, uint32_t glyph,
    bool * out_composite, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_CFF_GLYPH_H
