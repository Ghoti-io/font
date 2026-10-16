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
 * A glyph's program, by face.
 */

#include <ghoti.io/font/hint.h>
#include <ghoti.io/font/macros.h>
#include "../glyf/glyf.h"
#include "../sfnt/sfnt.h"

GFNT_Result gfnt_face_glyph_program(const GFNT_Face * face, uint32_t glyph,
    const uint8_t ** out_bytes, size_t * out_length, GFNT_Error * error) {
  gfnt_error_clear(error);
  if (!face || !out_bytes || !out_length) {
    return GFNT_ERR_INVALID;
  }
  if (gfnt_sfnt_producer(face) != GFNT_PRODUCER_GLYF) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, glyph,
        "only a glyf face has TrueType instructions");
  }
  return gfnt_glyf_program(face, glyph, out_bytes, out_length, error);
}
