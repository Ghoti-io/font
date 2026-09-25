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
 * The `post` table's header fields.
 *
 * documentation/design.md section 7.2: the header fields, and the glyph names.
 * Format 2.0 spells the first 258 names as indices into the standard Macintosh
 * glyph order, and that list is a 258-entry vector - section 14's rule is that
 * a vector comes from an oracle and is never written from memory, so it is
 * generated into post_names.h by tools/vectors/make_vectors.py and checked by
 * `make check-vectors` and testVectors.
 *
 * Reference: OpenType Specification 1.9, "post - PostScript Table".
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/macros.h>
#include <stdio.h>
#include <string.h>
#include "post_names.h"
#include "tables.h"

GFNT_Result gfnt_post_parse(const GFNT_Face * face, void * out,
    GFNT_Error * error) {
  static const GFNT_Tag tag = GFNT_TAG('p', 'o', 's', 't');
  GFNT_Post * post = out;
  GFNT_Reader reader;
  GFNT_Result result;

  result = gfnt_face_table_reader(face, tag, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }

  *post = (GFNT_Post) {0};
  if (gfnt_read_fixed(&reader, &post->version) != GFNT_OK
      || gfnt_read_fixed(&reader, &post->italic_angle) != GFNT_OK
      || gfnt_read_s16(&reader, &post->underline_position) != GFNT_OK
      || gfnt_read_s16(&reader, &post->underline_thickness) != GFNT_OK
      || gfnt_read_u32(&reader, &post->is_fixed_pitch) != GFNT_OK
      || gfnt_read_u32(&reader, &post->min_mem_type42) != GFNT_OK
      || gfnt_read_u32(&reader, &post->max_mem_type42) != GFNT_OK
      || gfnt_read_u32(&reader, &post->min_mem_type1) != GFNT_OK
      || gfnt_read_u32(&reader, &post->max_mem_type1) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }

  // Every version's header is these same 32 bytes, and what follows them
  // differs: 1.0 nothing, 2.0 the name index and the names, 2.5 a deprecated
  // offset array, 3.0 nothing. The version is reported rather than checked,
  // because the header is readable whatever follows it and a caller asking for
  // italicAngle should not be refused by a version it never mentioned.
  return GFNT_OK;
}

GFNT_Result gfnt_post_dump(const GFNT_Post * post, FILE * out) {
  if (!post || !out) {
    return GFNT_ERR_INVALID;
  }
  if (fprintf(out, "post: version 0x%08X, italicAngle 0x%08X, isFixedPitch %u\n",
          (unsigned)post->version, (unsigned)post->italic_angle,
          (unsigned)post->is_fixed_pitch)
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out, "post: underlinePosition %d, underlineThickness %d\n",
          post->underline_position, post->underline_thickness)
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out, "post: minMemType42 %u, maxMemType42 %u, minMemType1 %u, "
                   "maxMemType1 %u\n",
          (unsigned)post->min_mem_type42, (unsigned)post->max_mem_type42,
          (unsigned)post->min_mem_type1, (unsigned)post->max_mem_type1)
      < 0) {
    return GFNT_ERR_IO;
  }
  return GFNT_OK;
}

////////////////////////////////////////////////////////////////////////
// Glyph names
////////////////////////////////////////////////////////////////////////
//
// documentation/design.md section 7.2. Format 2.0 names glyph `g` by
// `glyphNameIndex[g]`: below GFNT_POST_STANDARD_NAME_COUNT it indexes the
// standard Macintosh glyph order, and at or above it names the
// `index - GFNT_POST_STANDARD_NAME_COUNT`th Pascal string that follows the
// index array. That 258-entry order is generated from the reference into
// post_names.h and is never written from memory (section 14).
//
// The other versions are answers, not silence:
//
//   1.0  the font *is* the standard order, so a name comes from the vector
//   2.5  a deprecated offset array; refused rather than guessed
//   3.0  the font states it has no names; GFNT_ERR_UNSUPPORTED, because
//        "this font has none" is not "this library cannot"
//   4.0  CID-keyed, and a different question entirely

/** Where format 2.0's `numberOfGlyphs` sits: straight after the 32-byte header. */
#define GFNT_POST_HEADER_BYTES 32u

/** The longest a Pascal string can be, so a reverse scan needs no allocation. */
#define GFNT_POST_NAME_MAX 255u

/**
 * One glyph's name, counted when @p out is NULL and written when it is not.
 *
 * One function for both passes deliberately: a size pass and a fill pass that
 * walk different predicates is the defect this suite has met most often. @p
 * capacity is ignored when @p out is NULL.
 */
static GFNT_Result gfnt_post_name_at(const GFNT_Face * face, uint32_t glyph,
    char * out, size_t capacity, size_t * out_length, GFNT_Error * error) {
  static const GFNT_Tag tag = GFNT_TAG('p', 'o', 's', 't');
  const GFNT_Post * post = NULL;
  GFNT_Reader reader;
  uint16_t count = 0;
  uint16_t index = 0;
  size_t at;
  GFNT_Result result;

  result = gfnt_face_post(face, &post, error);
  if (result != GFNT_OK) {
    return result;
  }

  if (post->version == 0x00010000u) {
    // Format 1.0 *is* the standard order: there is no index array and no
    // string storage, and a font claiming it must have exactly those glyphs.
    if (glyph >= GFNT_POST_STANDARD_NAME_COUNT) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, tag, 0, glyph,
          "a post format 1.0 font has only the 258 standard glyphs");
    }
    const char * name = gfnt_post_standard_names[glyph];
    const size_t length = strlen(name);

    if (out) {
      if (length + 1 > capacity) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, tag, 0, glyph,
            "the buffer is too small for this glyph's name");
      }
      memcpy(out, name, length + 1);
    }
    *out_length = length;
    return GFNT_OK;
  }
  if (post->version != 0x00020000u) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, 0, glyph,
        post->version == 0x00030000u
            ? "this font states it has no glyph names (post format 3.0)"
            : "a post version whose glyph names this library does not read");
  }

  result = gfnt_face_table_reader(face, tag, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_reader_u16_at(&reader, GFNT_POST_HEADER_BYTES, &count) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, GFNT_POST_HEADER_BYTES,
        glyph, "a post format 2.0 table with no glyph count");
  }
  if (glyph >= count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, tag, GFNT_POST_HEADER_BYTES,
        glyph, "this glyph is past the post table's own glyph count");
  }

  // Two bytes per index, from the address of the array rather than from a
  // remembered offset: every sum that reaches a file goes through the checked
  // arithmetic (section 6).
  if (!gcu_safe_add_size(GFNT_POST_HEADER_BYTES + 2u, (size_t)glyph * 2u, &at)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0, glyph,
        "the glyph name index array's offset overflowed");
  }
  if (gfnt_reader_u16_at(&reader, at, &index) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, at, glyph,
        "a post format 2.0 index array shorter than its own glyph count");
  }

  if (index < GFNT_POST_STANDARD_NAME_COUNT) {
    const char * name = gfnt_post_standard_names[index];
    const size_t length = strlen(name);

    if (out) {
      if (length + 1 > capacity) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, tag, at, glyph,
            "the buffer is too small for this glyph's name");
      }
      memcpy(out, name, length + 1);
    }
    *out_length = length;
    return GFNT_OK;
  }

  // The Pascal strings start after the index array and are walked from there:
  // the format gives no offset table, so the nth string is found by stepping
  // over the n - 1 before it. Each step is bounded by the reader.
  if (!gcu_safe_add_size(GFNT_POST_HEADER_BYTES + 2u, (size_t)count * 2u, &at)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0, glyph,
        "the string area's offset overflowed");
  }
  for (uint32_t skip = index - GFNT_POST_STANDARD_NAME_COUNT;; --skip) {
    uint8_t length = 0;
    size_t next;

    if (gfnt_reader_u8_at(&reader, at, &length) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, at, glyph,
          "a post format 2.0 glyph name index past the last stored name");
    }
    if (!gcu_safe_add_size(at, (size_t)length + 1u, &next)) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, at, glyph,
          "a stored glyph name's length overflowed the table");
    }
    if (skip == 0) {
      if (out) {
        if ((size_t)length + 1u > capacity) {
          return gfnt_error_set(error, GFNT_ERR_INVALID, tag, at, glyph,
              "the buffer is too small for this glyph's name");
        }
        for (uint8_t i = 0; i < length; ++i) {
          uint8_t byte = 0;

          if (gfnt_reader_u8_at(&reader, at + 1u + i, &byte) != GFNT_OK) {
            return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, at + 1u + i,
                glyph, "a stored glyph name that ends past the table");
          }
          out[i] = (char)byte;
        }
        out[length] = '\0';
      }
      else {
        // Still bounded even when only counting, so that a length query and a
        // read cannot disagree about whether the name is there at all.
        uint8_t last = 0;

        if (length > 0
            && gfnt_reader_u8_at(&reader, next - 1u, &last) != GFNT_OK) {
          return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, next - 1u, glyph,
              "a stored glyph name that ends past the table");
        }
      }
      *out_length = length;
      return GFNT_OK;
    }
    at = next;
  }
}

GFNT_Result gfnt_face_glyph_name(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Allocator * allocator, char ** out_name, size_t * out_length,
    GFNT_Error * error) {
  static const GFNT_Tag tag = GFNT_TAG('p', 'o', 's', 't');
  size_t needed = 0;
  size_t written = 0;
  char * name;
  GFNT_Result result;

  if (!face || !out_name) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, tag, 0, glyph,
        "no face, or nowhere to put the name");
  }

  result = gfnt_post_name_at(face, glyph, NULL, 0, &needed, error);
  if (result != GFNT_OK) {
    return result;
  }

  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  name = allocator->malloc_fn(allocator->ctx, needed + 1);
  if (!name) {
    return gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, glyph,
        "allocating the glyph name");
  }

  result = gfnt_post_name_at(face, glyph, name, needed + 1, &written, error);
  if (result != GFNT_OK) {
    allocator->free_fn(allocator->ctx, name);
    return result;
  }
  if (written != needed) {
    allocator->free_fn(allocator->ctx, name);
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, tag, 0, glyph,
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
  static const GFNT_Tag tag = GFNT_TAG('p', 'o', 's', 't');
  char buffer[GFNT_POST_NAME_MAX + 1u];
  size_t glyphs = 0;
  GFNT_Result result;

  if (!face || !name || !out_glyph) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, tag, 0, GFNT_GLYPH_NONE,
        "no face, no name, or nowhere to put the glyph");
  }

  result = gfnt_face_num_glyphs(face, &glyphs, error);
  if (result != GFNT_OK) {
    return result;
  }

  // A linear scan, and deliberately not an index: this is a text-extraction
  // path asked once per distinct name, and a map would have to be built,
  // memoised and invalidated for a question most callers never ask. The stack
  // buffer is the format's own maximum, so the scan allocates nothing.
  for (size_t glyph = 0; glyph < glyphs; ++glyph) {
    size_t length = 0;

    result = gfnt_post_name_at(face, (uint32_t)glyph, buffer, sizeof(buffer),
        &length, NULL);
    if (result == GFNT_ERR_UNSUPPORTED) {
      // The table has no names at all. Reported once rather than per glyph.
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, 0,
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

  return gfnt_error_set(error, GFNT_ERR_INVALID, tag, 0, GFNT_GLYPH_NONE,
      "no glyph in this font has that name");
}

GFNT_Result gfnt_face_glyph_names_dump(const GFNT_Face * face, FILE * out) {
  char buffer[GFNT_POST_NAME_MAX + 1u];
  size_t glyphs = 0;

  if (!face || !out) {
    return GFNT_ERR_INVALID;
  }
  if (gfnt_face_num_glyphs(face, &glyphs, NULL) != GFNT_OK) {
    return GFNT_ERR_UNSUPPORTED;
  }

  for (size_t glyph = 0; glyph < glyphs; ++glyph) {
    size_t length = 0;
    const GFNT_Result result = gfnt_post_name_at(face, (uint32_t)glyph, buffer,
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
