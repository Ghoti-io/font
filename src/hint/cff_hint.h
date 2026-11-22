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
 * Hinting for a `CFF`, `CFF2` or Type 1 glyph: the stems and blue zones its font
 * declares, fitted to the pixel grid in the y direction only.
 *
 * The rules are those of the engine every renderer people compare with uses for these
 * formats, recovered by experiment (the oracle scripts under tools/oracle) and written
 * here in the 16.16 fixed-point arithmetic they reproduce to the last 1/64 pixel on the
 * corpus. Nothing is read from a font that the charstring interpreter did not already
 * read: the stems and hint masks (gfnt_face_glyph_hints()) and the Private dictionary's
 * blue values.
 */

#ifndef GHOTI_IO_GFNT_HINT_CFF_HINT_H
#define GHOTI_IO_GFNT_HINT_CFF_HINT_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/hint.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/outline.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A face prepared for charstring hinting at one size. Immutable once made. */
typedef struct GFNT_CffHint GFNT_CffHint;

/**
 * Prepare a charstring face for hinting at @p ppem.
 *
 * @param face A `CFF`, `CFF2` or Type 1 face.
 * @param ppem Pixels per em.
 * @param variation A CFF2 face's location, or NULL for the default instance.
 * @param out Receives the context; free it with ::gfnt_cffhint_destroy().
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED for any other kind of face; ::GFNT_ERR_INVALID;
 *   ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_cffhint_create(const GFNT_Face * face, uint32_t ppem,
    const GFNT_Variation * variation, GFNT_CffHint ** out, GFNT_Error * error);

/** Free a context. NULL is ignored. */
void gfnt_cffhint_destroy(GFNT_CffHint * hint);

/** Whether @p face is one this engine hints. */
bool gfnt_cffhint_applies(const GFNT_Face * face);

/**
 * A glyph's outline in 26.6 pixels, fitted by its stems and its font's blue zones.
 *
 * A glyph whose hints cannot be read (an accented `seac` glyph, a hint count over the
 * recording limit) is returned scaled and unfitted, with
 * ::GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM, as the TrueType path does for a glyph with no
 * program. The advance is the unhinted one rounded to a whole pixel.
 *
 * @param face The face the context was made from.
 * @param hint The context.
 * @param glyph The glyph index.
 * @param allocator For the outline, or NULL for the face's.
 * @param out_outline Receives the outline, in ::GFNT_OUTLINE_PIXELS.
 * @param out_status Receives what happened.
 * @param out_advance Receives the advance in 26.6 pixels, or NULL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK or the failure to read the glyph or to allocate.
 */
GFNT_Result gfnt_cffhint_glyph_outline(const GFNT_Face * face, const GFNT_CffHint * hint,
    uint32_t glyph, const GFNT_Allocator * allocator, GFNT_Outline ** out_outline,
    GFNT_HintStatus * out_status, int32_t * out_advance, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GFNT_HINT_CFF_HINT_H */
