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

/*
 * Usage: font-math <font> [face-index]
 *
 * Prints everything the face's MATH table says, one line per fact, in an order and a spelling
 * that tools/oracle/fonttools_math_read.py reproduces from fontTools:
 *
 *   C <n> <value>               a MathConstants entry
 *   O <overlap>                 MinConnectorOverlap
 *   I|T <glyph> <value>         italics correction, top-accent attachment
 *   X <glyph>                   extended shape
 *   K <glyph> <corner> <h...> | <k...>
 *   V|H <glyph> <glyph:advance>...          vertical/horizontal variants
 *   A|B <glyph> <italics> <glyph:start:end:full:flags>...   vertical/horizontal assembly
 */

#include <stdio.h>
#include <stdlib.h>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/math.h>
#include <ghoti.io/font/metrics.h>

#define CHECK(call) do { GFNT_Result r_ = (call); if (r_ != GFNT_OK) { \
    fprintf(stderr, "%s: %s\n", #call, gfnt_result_string(r_)); return 1; } } while (0)

int main(int argc, char ** argv) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  size_t glyphs = 0;
  size_t g;
  size_t k;
  size_t n;
  int c;
  int dir;
  int32_t value;
  uint32_t overlap;
  bool present;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <font> [face-index]\n", argv[0]);
    return 2;
  }
  gfnt_error_clear(&error);
  CHECK(gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error));
  CHECK(gfnt_face_load(blob, argc > 2 ? (size_t)strtoul(argv[2], NULL, 10) : 0, NULL, NULL, &face,
      &error));
  CHECK(gfnt_face_num_glyphs(face, &glyphs, &error));
  if (!gfnt_face_has_math(face)) {
    fprintf(stderr, "no MATH\n");
    return 3;
  }
  for (c = 0; c < GFNT_MATH_CONSTANT_COUNT; c++) {
    CHECK(gfnt_face_math_constant(face, NULL, (GFNT_MathConstant)c, &value, &error));
    printf("C %d %d\n", c, (int)value);
  }
  CHECK(gfnt_face_math_min_connector_overlap(face, &overlap, &error));
  printf("O %u\n", (unsigned)overlap);
  for (g = 0; g < glyphs; g++) {
    bool ext;

    for (c = 0; c < 2; c++) {
      CHECK(gfnt_face_math_glyph_value(face, NULL, (uint32_t)g, (GFNT_MathGlyphValue)c, &present,
          &value, &error));
      if (present) {
        printf("%c %zu %d\n", c ? 'T' : 'I', g, (int)value);
      }
    }
    CHECK(gfnt_face_math_is_extended_shape(face, (uint32_t)g, &ext, &error));
    if (ext) {
      printf("X %zu\n", g);
    }
    for (c = 0; c < 4; c++) {
      CHECK(gfnt_face_math_kern_count(face, (uint32_t)g, (GFNT_MathKernCorner)c, &present, &n,
          &error));
      if (!present) {
        continue;
      }
      printf("K %zu %d", g, c);
      for (k = 0; k < n; k++) {
        CHECK(gfnt_face_math_kern_height_at(face, NULL, (uint32_t)g, (GFNT_MathKernCorner)c, k,
            &value, &error));
        printf(" %d", (int)value);
      }
      printf(" |");
      for (k = 0; k <= n; k++) {
        CHECK(gfnt_face_math_kern_value_at(face, NULL, (uint32_t)g, (GFNT_MathKernCorner)c, k,
            &value, &error));
        printf(" %d", (int)value);
      }
      printf("\n");
    }
    for (dir = 0; dir < 2; dir++) {
      const bool vertical = dir == 0;

      CHECK(gfnt_face_math_variant_count(face, (uint32_t)g, vertical, &n, &error));
      if (n) {
        printf("%c %zu", vertical ? 'V' : 'H', g);
        for (k = 0; k < n; k++) {
          GFNT_MathVariant v;

          CHECK(gfnt_face_math_variant_at(face, (uint32_t)g, vertical, k, &v, &error));
          printf(" %u:%u", (unsigned)v.glyph, (unsigned)v.advance);
        }
        printf("\n");
      }
      CHECK(gfnt_face_math_assembly_count(face, (uint32_t)g, vertical, &n, &error));
      if (n) {
        CHECK(gfnt_face_math_assembly_italics_correction(face, NULL, (uint32_t)g, vertical, &value,
            &error));
        printf("%c %zu %d", vertical ? 'A' : 'B', g, (int)value);
        for (k = 0; k < n; k++) {
          GFNT_MathPart p;

          CHECK(gfnt_face_math_assembly_part_at(face, (uint32_t)g, vertical, k, &p, &error));
          printf(" %u:%u:%u:%u:%u", (unsigned)p.glyph, (unsigned)p.start_connector_length,
              (unsigned)p.end_connector_length, (unsigned)p.full_advance, (unsigned)p.flags);
        }
        printf("\n");
      }
    }
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
