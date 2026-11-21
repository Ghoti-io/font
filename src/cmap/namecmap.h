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
 * The characters of a face that has glyph names and no `cmap`: a Type 1 font, a bare `CFF`,
 * a TrueType font whose `cmap` is gone.
 *
 * Every other implementation reads such a face the same way, and this follows them: a
 * glyph whose name the Adobe Glyph List gives as one character (`A`, `Aacute`, `uni20AC`)
 * is that character's glyph. Where several glyphs claim one character the plain name wins
 * over a variant (`a` over `a.sc`), and among equals the lower glyph. A name that stands
 * for several characters (a base and its mark) or none (`g23`, `.notdef`) answers for no
 * character, since a glyph that is a sequence is not any one character's glyph.
 *
 * The map is built once per face, on the first lookup, and kept (src/sfnt/sfnt.h).
 */

#ifndef GHOTI_IO_GFNT_NAMECMAP_H
#define GHOTI_IO_GFNT_NAMECMAP_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/allocator.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The characters a face's glyph names give, sorted by codepoint. */
typedef struct GFNT_NameCmap {
  uint32_t * codepoints; ///< Ascending, unique.
  uint32_t * glyphs;     ///< The glyph for each.
  size_t count;          ///< How many.
} GFNT_NameCmap;

/** For ::gfnt_table_cached(): build the map from the face's glyph names. */
GFNT_Result gfnt_namecmap_parse(const GFNT_Face * face, void * out, void * context,
    GFNT_Error * error);

/** Release what a parse allocated. */
void gfnt_namecmap_release(const GFNT_Allocator * allocator, void * table);

/**
 * The glyph a face's names give a codepoint, or 0.
 *
 * @return ::GFNT_OK (with glyph 0 for a codepoint no name gives);
 *   ::GFNT_ERR_UNSUPPORTED when the face has no glyph names to read.
 */
GFNT_Result gfnt_namecmap_lookup(const GFNT_Face * face, uint32_t codepoint,
    uint32_t * out_glyph, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif
