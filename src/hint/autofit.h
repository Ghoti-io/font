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

#ifndef GHOTI_IO_GFNT_HINT_AUTOFIT_H
#define GHOTI_IO_GFNT_HINT_AUTOFIT_H

/**
 * @file
 *
 * The automatic hinter: fitting an outline with no hint data of its own to the pixel grid, in y
 * only, from the shapes of the face's own letters. See autofit.c for the method and for what
 * it is measured against.
 */

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/hint.h>
#include <ghoti.io/font/outline.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A context for one face at one size and location. */
typedef struct GFNT_Autofit GFNT_Autofit;

/**
 * Make a context: the face's blue zones and standard stem width, read from its own letters, and the
 * scale at a size.
 *
 * @param face An outline face: `glyf`, `CFF`, `CFF2` or Type 1.
 * @param ppem The pixel size.
 * @param variation The location of a variable face, or NULL.
 * @param xy Fit both axes (::GFNT_HINT_MODE_AUTO_XY) rather than y alone.
 * @param out Receives the context; free it with ::gfnt_autofit_destroy().
 * @param error Receives a diagnostic, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED for a face with no outlines; ::GFNT_ERR_INVALID;
 *   ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_autofit_create(const GFNT_Face * face, uint32_t ppem, const GFNT_Variation * variation, bool xy,
    GFNT_Autofit ** out, GFNT_Error * error);

/** Free a context. NULL is ignored. */
void gfnt_autofit_destroy(GFNT_Autofit * af);

/** Whether @p face has outlines this hinter can fit. */
bool gfnt_autofit_applies(const GFNT_Face * face);

/**
 * A glyph's outline in 26.6 pixels, fitted. The advance is the unhinted one rounded to a whole pixel.
 *
 * @param face The face the context was made from.
 * @param af The context.
 * @param glyph The glyph index.
 * @param allocator For the outline, or NULL for the face's.
 * @param out_outline Receives the outline, in ::GFNT_OUTLINE_PIXELS.
 * @param out_status Receives ::GFNT_HINT_STATUS_HINTED.
 * @param out_advance Receives the advance in 26.6 pixels, or NULL.
 * @param error Receives a diagnostic, or NULL.
 * @return ::GFNT_OK or the failure to read the glyph or to allocate.
 */
GFNT_Result gfnt_autofit_glyph_outline(const GFNT_Face * face, const GFNT_Autofit * af, uint32_t glyph,
    const GFNT_Allocator * allocator, GFNT_Outline ** out_outline, GFNT_HintStatus * out_status,
    int32_t * out_advance, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GFNT_HINT_AUTOFIT_H */
