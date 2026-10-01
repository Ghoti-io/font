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
 * Print every glyph of a bitmap strike, pixel by pixel.
 *
 * Usage: font-bitmap <font> [stride] [first]
 *        font-bitmap --golden <label> <font>
 *
 * The driver `tools/oracle/bitmap_diff.py` compares against Pillow's own PCF and
 * BDF readers, and the one that makes the four-container identity check possible:
 * a PCF, a BDF, a PSF and a `.hex` of one design produce the same lines here, and
 * `diff` is then the whole test.
 *
 * So the output is the *normalised* glyph and nothing about the file it came from.
 * A strike's ppem, baseline and per-glyph metrics are printed because a container
 * either states them or does not, and which is which is the other half of what a
 * reader has to get right.
 *
 * `--golden` prints one line per glyph in `font-render`'s shape - the coverage's
 * box, its total and its hash - so that the golden-bitmap gate (section 14.4)
 * covers a strike as well as a rasterised outline. Every operation between a
 * file's bytes and a strike's pixels is an explicit shift, so the pixels *should*
 * be the same on a big-endian machine; that sentence is a claim, and this is what
 * makes it a measurement.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/raster.h>

/**
 * One glyph as a golden line: the coverage the strike becomes, hashed.
 */
static int golden_glyph(const GFNT_Face * face, const char * label,
    uint32_t glyph, size_t strike) {
  GFNT_BitmapGlyph bitmap;
  GFNT_Coverage coverage;
  GFNT_Error error;
  GFNT_Result result;

  gfnt_error_clear(&error);
  result = gfnt_face_glyph_bitmap(face, glyph, strike, &bitmap, &error);
  if (result != GFNT_OK) {
    printf("%s %u 0 0 0 refused %s\n", label, (unsigned)glyph,
        gfnt_result_string(result));
    return 0;
  }
  result = gfnt_coverage_from_bitmap(&bitmap, NULL, &coverage, &error);
  if (result != GFNT_OK) {
    printf("%s %u 0 0 0 refused %s\n", label, (unsigned)glyph,
        gfnt_result_string(result));
    return 0;
  }
  // The strike's own ppem in the size column, because a strike has exactly one
  // and it is the only size at which these pixels mean anything.
  printf("%s %u %u 0 0 %ux%u+%d+%d %llu %016llX\n", label, (unsigned)glyph,
      (unsigned)bitmap.strike.ppem_y, (unsigned)coverage.width,
      (unsigned)coverage.height, (int)coverage.left, (int)coverage.top,
      (unsigned long long)gfnt_coverage_total(&coverage),
      (unsigned long long)gfnt_coverage_hash(&coverage));
  gfnt_coverage_destroy(&coverage);
  return 0;
}

/**
 * Every glyph of a strike as golden lines.
 */
static int golden(const char * label, const char * path) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  size_t glyphs = 0;
  size_t strikes = 0;
  GFNT_Result result;

  gfnt_error_clear(&error);
  result = gfnt_blob_create_file(path, NULL, NULL, &blob, &error);
  if (result != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  result = gfnt_face_load(blob, 0, NULL, NULL, &face, &error);
  if (result != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  if (gfnt_face_num_glyphs(face, &glyphs, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }
  // **Every strike, not strike 0.** A standalone container has exactly one and
  // this loop ran once; an `EBLC` face has as many as the table lists, and
  // committing only the first would leave two thirds of strikes.ttf's pixels
  // unchecked on the big-endian targets this gate exists for. The ppem column
  // already distinguishes them, so the line format does not change.
  //
  // A face with no strikes at all still prints one line per glyph, refused, which
  // is what says the refusal itself reproduces.
  if (gfnt_face_strike_count(face, &strikes, &error) != GFNT_OK) {
    strikes = 0;
  }
  for (size_t strike = 0; strike < (strikes ? strikes : 1); ++strike) {
    for (size_t glyph = 0; glyph < glyphs; ++glyph) {
      if (golden_glyph(face, label, (uint32_t)glyph, strike) != 0) {
        gfnt_face_free(face);
        gfnt_blob_destroy(blob);
        return 1;
      }
    }
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}

int main(int argc, char ** argv) {
  const char * path;
  size_t stride = 1;
  size_t first = 0;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Strike strike;
  GFNT_Error error;
  GFNT_Result result;
  size_t glyphs = 0;
  size_t strikes = 0;
  size_t mappings = 0;
  char tag[5];

  if (argc < 2) {
    fprintf(stderr, "usage: %s <font> [stride] [first]\n"
                    "       %s --golden <label> <font>\n", argv[0], argv[0]);
    return 2;
  }
  if (strcmp(argv[1], "--golden") == 0) {
    if (argc < 4) {
      fprintf(stderr, "usage: %s --golden <label> <font>\n", argv[0]);
      return 2;
    }
    return golden(argv[2], argv[3]);
  }
  path = argv[1];
  if (argc > 2) {
    stride = (size_t)strtoul(argv[2], NULL, 10);
    if (stride == 0) {
      stride = 1;
    }
  }
  if (argc > 3) {
    first = (size_t)strtoul(argv[3], NULL, 10);
  }

  gfnt_error_clear(&error);
  result = gfnt_blob_create_file(path, NULL, NULL, &blob, &error);
  if (result != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  result = gfnt_face_load(blob, 0, NULL, NULL, &face, &error);
  if (result != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }

  printf("container: %s\n", gfnt_tag_string(gfnt_face_flavour(face), tag));
  if (gfnt_face_strike_count(face, &strikes, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }
  printf("strikes: %zu\n", strikes);
  for (size_t i = 0; i < strikes; ++i) {
    if (gfnt_face_strike_at(face, i, &strike, &error) != GFNT_OK) {
      gfnt_error_dump(&error, stderr);
      gfnt_face_free(face);
      gfnt_blob_destroy(blob);
      return 1;
    }
    printf("strike %zu: ppem %u %u, depth %u, ascent %d, descent %d\n", i,
        (unsigned)strike.ppem_x, (unsigned)strike.ppem_y,
        (unsigned)strike.bit_depth, (int)strike.ascent, (int)strike.descent);
  }
  if (gfnt_face_num_glyphs(face, &glyphs, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }
  printf("glyphs: %zu\n", glyphs);

  // The encoding, enumerated rather than probed: a bitmap container states which
  // characters it has, and printing the list is what lets a differential compare
  // a mapping without guessing which codepoints to ask about.
  if (gfnt_face_bitmap_encoding_count(face, &mappings, &error) == GFNT_OK) {
    printf("encoding: %zu\n", mappings);
    for (size_t i = 0; i < mappings; ++i) {
      uint32_t codepoint = 0;
      uint32_t glyph = 0;

      if (gfnt_face_bitmap_encoding_at(face, i, &codepoint, &glyph, &error)
          != GFNT_OK) {
        gfnt_error_dump(&error, stderr);
        break;
      }
      printf("map U+%04X: glyph %u\n", (unsigned)codepoint, (unsigned)glyph);
    }
  }
  else {
    printf("encoding: none\n");
  }

  for (size_t glyph = first; glyph < glyphs; glyph += stride) {
    GFNT_BitmapGlyph bitmap;
    char * name = NULL;

    if (gfnt_face_glyph_bitmap(face, (uint32_t)glyph, 0, &bitmap, &error)
        != GFNT_OK) {
      printf("glyph %zu: refused %s\n", glyph,
          gfnt_result_string(error.result));
      continue;
    }
    printf("glyph %zu: ", glyph);
    if (gfnt_bitmap_dump(&bitmap, stdout) != GFNT_OK) {
      gfnt_face_free(face);
      gfnt_blob_destroy(blob);
      return 1;
    }
    if (gfnt_face_glyph_name(face, (uint32_t)glyph, NULL, &name, NULL, &error)
        == GFNT_OK) {
      printf("glyph %zu name: %s\n", glyph, name);
      gfnt_glyph_name_free(NULL, name);
    }
    else {
      printf("glyph %zu name: -\n", glyph);
    }
  }

  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
