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
 * What a glyph is called, whichever table says so.
 *
 * documentation/design.md sections 7.2 and 7.4. There are two answers and they
 * live in different tables: a TrueType font names its glyphs in `post`, and a
 * CFF font names them in its **charset**, by SID into the standard strings or
 * its own. Before phase 2 this whole file was three functions in `post.c`,
 * which was right while `post` was the only answer.
 *
 * **The charset wins for a CFF face.** The OpenType specification says such a
 * font carries `post` version 3 - a header stating it has no names - and every
 * reference implementation takes its glyph order from the charset. A CFF font
 * that carries names in both is malformed, and preferring `post` there would
 * disagree with every other reader about what its glyphs are called.
 */

#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/macros.h>
#include <stdio.h>
#include <string.h>
#include "../cff/cff.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"
#include "../type1/type1.h"

/**
 * One glyph's name, counted when @p out is NULL and written when it is not.
 *
 * Both sources have the same two-pass contract, so choosing between them is a
 * choice of function and nothing else.
 *
 * A CFF face falls back to `post` only when the charset refuses *this glyph* -
 * a subset whose charset stops short, say - rather than when the CFF is
 * unreadable, because a corrupt CFF is news the caller should get.
 */
static GFNT_Result gfnt_glyph_name_at(const GFNT_Face * face, uint32_t glyph,
    char * out, size_t capacity, size_t * out_length, GFNT_Error * error) {
  // A Type 1 face has no `post` to fall back to and needs none: its glyphs are
  // *named* rather than numbered, so `/CharStrings` is not one source of names
  // among several - it is the only thing that says which glyph is which.
  if (gfnt_sfnt_producer(face) == GFNT_PRODUCER_TYPE1) {
    return gfnt_type1_name_at(face, glyph, out, capacity, out_length, error);
  }
  if (gfnt_sfnt_producer(face) == GFNT_PRODUCER_CFF) {
    GFNT_Result result = gfnt_cff_name_at(face, glyph, out, capacity,
        out_length, error);

    if (result != GFNT_ERR_INVALID) {
      return result;
    }
  }
  return gfnt_post_name_at(face, glyph, out, capacity, out_length, error);
}

GFNT_Result gfnt_face_glyph_name(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Allocator * allocator, char ** out_name, size_t * out_length,
    GFNT_Error * error) {
  size_t needed = 0;
  size_t written = 0;
  char * name;
  GFNT_Result result;

  if (!face || !out_name) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, glyph,
        "no face, or nowhere to put the name");
  }

  result = gfnt_glyph_name_at(face, glyph, NULL, 0, &needed, error);
  if (result != GFNT_OK) {
    return result;
  }

  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  name = allocator->malloc_fn(allocator->ctx, needed + 1);
  if (!name) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, glyph,
        "allocating the glyph name");
  }

  result = gfnt_glyph_name_at(face, glyph, name, needed + 1, &written, error);
  if (result != GFNT_OK) {
    allocator->free_fn(allocator->ctx, name);
    return result;
  }
  if (written != needed) {
    allocator->free_fn(allocator->ctx, name);
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, glyph,
        "the glyph name's size pass and fill pass disagreed");
  }

  *out_name = name;
  if (out_length) {
    *out_length = written;
  }
  return GFNT_OK;
}

void gfnt_glyph_name_free(const GFNT_Allocator * allocator, char * name) {
  if (!name) {
    return;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  allocator->free_fn(allocator->ctx, name);
}

GFNT_Result gfnt_face_glyph_for_name(const GFNT_Face * face, const char * name,
    uint32_t * out_glyph, GFNT_Error * error) {
  char buffer[GFNT_GLYPH_NAME_MAX + 1u];
  size_t glyphs = 0;
  GFNT_Result result;

  if (!face || !name || !out_glyph) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, no name, or nowhere to put the glyph");
  }

  result = gfnt_face_num_glyphs(face, &glyphs, error);
  if (result != GFNT_OK) {
    return result;
  }

  // A linear scan, and deliberately not an index: this is a text-extraction
  // path asked once per distinct name, and a map would have to be built,
  // memoised and invalidated for a question most callers never ask. The stack
  // buffer is the longest name either format stores, so the scan allocates
  // nothing.
  for (size_t glyph = 0; glyph < glyphs; ++glyph) {
    size_t length = 0;

    result = gfnt_glyph_name_at(face, (uint32_t)glyph, buffer, sizeof(buffer),
        &length, NULL);
    if (result == GFNT_ERR_UNSUPPORTED) {
      // Neither table has names at all. Reported once rather than per glyph.
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0,
          GFNT_GLYPH_NONE, "this font has no glyph names to search");
    }
    if (result != GFNT_OK) {
      continue;
    }
    if (strcmp(buffer, name) == 0) {
      *out_glyph = (uint32_t)glyph;
      return GFNT_OK;
    }
  }

  return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
      "no glyph in this font has that name");
}

GFNT_Result gfnt_face_glyph_names_dump(const GFNT_Face * face, FILE * out) {
  char buffer[GFNT_GLYPH_NAME_MAX + 1u];
  size_t glyphs = 0;

  if (!face || !out) {
    return GFNT_ERR_INVALID;
  }
  if (gfnt_face_num_glyphs(face, &glyphs, NULL) != GFNT_OK) {
    return GFNT_ERR_UNSUPPORTED;
  }

  for (size_t glyph = 0; glyph < glyphs; ++glyph) {
    size_t length = 0;
    const GFNT_Result result = gfnt_glyph_name_at(face, (uint32_t)glyph, buffer,
        sizeof(buffer), &length, NULL);

    if (result == GFNT_ERR_UNSUPPORTED) {
      // Said once, and said rather than left out: a dump that silently omits a
      // table lets a differential read "no names" as agreement.
      if (fprintf(out, "glyph names: none in this font\n") < 0) {
        return GFNT_ERR_IO;
      }
      return GFNT_OK;
    }
    if (result != GFNT_OK) {
      if (fprintf(out, "glyph name %zu: unreadable\n", glyph) < 0) {
        return GFNT_ERR_IO;
      }
      continue;
    }
    if (fprintf(out, "glyph name %zu: '%s'\n", glyph, buffer) < 0) {
      return GFNT_ERR_IO;
    }
  }
  return GFNT_OK;
}
