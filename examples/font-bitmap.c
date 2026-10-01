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
 *        font-bitmap --strikes <font> [face-index] [stride]
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
 * `--strikes` is `tools/oracle/eblc_diff.py`'s input, and differs from the
 * default mode in the two ways an `EBLC` face differs from a standalone
 * container: it takes a **face index**, because the only two Debian packages
 * with this table include a four-face collection, and it prints **every strike**
 * rather than strike 0. Each strike's three glyph counts - present, absent and
 * corrupt - are over every glyph whatever the stride, because a strike's glyph
 * set is what the index subtable walk decides and a sampled comparison can step
 * over a subtable that went missing.
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

/**
 * One glyph of one strike, as the lines `tools/oracle/eblc_diff.py` compares.
 *
 * `state` first and always, because "this strike does not carry this glyph" is a
 * fact to compare rather than the absence of one: a reader that silently dropped
 * a glyph it should have found would otherwise differ from the reference by
 * printing nothing, and a comparison keyed on what was printed cannot see that.
 */
static void strike_glyph(const GFNT_Face * face, uint32_t glyph,
    size_t strike) {
  GFNT_BitmapGlyph bitmap;
  GFNT_Error error;
  GFNT_Result result;

  gfnt_error_clear(&error);
  result = gfnt_face_glyph_bitmap(face, glyph, strike, &bitmap, &error);
  if (result == GFNT_ERR_UNSUPPORTED) {
    printf("g %u %zu state absent\n", (unsigned)glyph, strike);
    return;
  }
  if (result != GFNT_OK) {
    // The message as well as the code, because a glyph the font gets wrong is
    // refused *for a reason* (M11) and a differential that printed only the code
    // could not tell two different defects apart.
    printf("g %u %zu state %s: %s\n", (unsigned)glyph, strike,
        gfnt_result_string(result), error.message);
    return;
  }
  printf("g %u %zu state present\n", (unsigned)glyph, strike);
  printf("g %u %zu box %u %u %d %d %d %u\n", (unsigned)glyph, strike,
      (unsigned)bitmap.width, (unsigned)bitmap.height, (int)bitmap.bearing_x,
      (int)bitmap.bearing_y, (int)bitmap.advance, (unsigned)bitmap.bit_depth);
  for (uint32_t y = 0; y < bitmap.height; ++y) {
    printf("g %u %zu row %u ", (unsigned)glyph, strike, (unsigned)y);
    for (uint32_t x = 0; x < bitmap.width; ++x) {
      putchar(gfnt_bitmap_pixel(&bitmap, x, y) ? '#' : '.');
    }
    putchar('\n');
  }
}

/**
 * Every strike of one face, and every @p stride th glyph of each.
 *
 * The three per-strike counts are over **every** glyph whatever the stride, and
 * they are the reason this mode exists rather than the pixels alone: a stride
 * samples, and a strike's glyph *set* is what the whole index subtable walk
 * decides. Present, absent and corrupt sum to the face's glyph count by
 * construction, so a reader that lost a subtable shows up in a count that a
 * sampled comparison could step over.
 */
static int all_strikes(const char * path, size_t index, size_t stride) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  size_t glyphs = 0;
  size_t count = 0;
  size_t faces = 0;
  GFNT_Result result;

  gfnt_error_clear(&error);
  result = gfnt_blob_create_file(path, NULL, NULL, &blob, &error);
  if (result != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  if (gfnt_face_count(blob, NULL, &faces, &error) != GFNT_OK) {
    faces = 1;
  }
  result = gfnt_face_load(blob, index, NULL, NULL, &face, &error);
  if (result != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  if (gfnt_face_num_glyphs(face, &glyphs, &error) != GFNT_OK
      || gfnt_face_strike_count(face, &count, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }
  printf("faces %zu\n", faces);
  printf("glyphs %zu\n", glyphs);
  printf("strikes %zu\n", count);
  for (size_t i = 0; i < count; ++i) {
    GFNT_Strike strike;
    size_t present = 0;
    size_t absent = 0;
    size_t corrupt = 0;

    if (gfnt_face_strike_at(face, i, &strike, &error) != GFNT_OK) {
      gfnt_error_dump(&error, stderr);
      gfnt_face_free(face);
      gfnt_blob_destroy(blob);
      return 1;
    }
    for (size_t glyph = 0; glyph < glyphs; ++glyph) {
      GFNT_BitmapGlyph bitmap;

      switch (gfnt_face_glyph_bitmap(face, (uint32_t)glyph, i, &bitmap,
          &error)) {
        case GFNT_OK: ++present; break;
        case GFNT_ERR_UNSUPPORTED: ++absent; break;
        default: ++corrupt; break;
      }
    }
    // One fact per line rather than one strike per line, so that a
    // disagreement names the field: `strike 2 descent` is a finding and `strike
    // 2 ...` with eight numbers in it is a line somebody has to diff by eye.
    printf("strike %zu ppem_x %u\n", i, (unsigned)strike.ppem_x);
    printf("strike %zu ppem_y %u\n", i, (unsigned)strike.ppem_y);
    printf("strike %zu depth %u\n", i, (unsigned)strike.bit_depth);
    printf("strike %zu ascent %d\n", i, (int)strike.ascent);
    printf("strike %zu descent %d\n", i, (int)strike.descent);
    printf("strike %zu present %zu\n", i, present);
    printf("strike %zu absent %zu\n", i, absent);
    printf("strike %zu corrupt %zu\n", i, corrupt);
    // The last glyph is always printed as well as every stride'th, because the
    // final entry of an offset array is the one a producer gets wrong - mona.ttf
    // is four bytes short of it - and a stride that stepped over it would leave
    // the only glyph in that font either side disagrees about unsampled.
    for (size_t glyph = 0; glyph < glyphs; ++glyph) {
      if (glyph % stride != 0 && glyph + 1 != glyphs) {
        continue;
      }
      strike_glyph(face, (uint32_t)glyph, i);
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
                    "       %s --golden <label> <font>\n"
                    "       %s --strikes <font> [face-index] [stride]\n",
        argv[0], argv[0], argv[0]);
    return 2;
  }
  if (strcmp(argv[1], "--golden") == 0) {
    if (argc < 4) {
      fprintf(stderr, "usage: %s --golden <label> <font>\n", argv[0]);
      return 2;
    }
    return golden(argv[2], argv[3]);
  }
  if (strcmp(argv[1], "--strikes") == 0) {
    size_t index = 0;
    size_t every = 1;

    if (argc < 3) {
      fprintf(stderr, "usage: %s --strikes <font> [face-index] [stride]\n",
          argv[0]);
      return 2;
    }
    if (argc > 3) {
      index = (size_t)strtoul(argv[3], NULL, 10);
    }
    if (argc > 4) {
      every = (size_t)strtoul(argv[4], NULL, 10);
      if (every == 0) {
        every = 1;
      }
    }
    return all_strikes(argv[2], index, every);
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
