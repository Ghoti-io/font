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
 * Print what this library can tell you about a font file.
 *
 * Usage: font-info <font> [face-index] [codepoint...]
 *
 * The example the README's snippet is taken from, and the quickest way to run
 * tier 0 against a font on the machine rather than against a fixture this
 * library built for itself. It prints the directory, the tables it can parse,
 * the `cmap` and `name` records, and for each codepoint named on the command
 * line the glyph and its advance.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>

/**
 * Report a failure the way a diagnostic is meant to be reported: the result,
 * and then where it happened.
 */
static int fail(const char * what, GFNT_Result result, const GFNT_Error * error) {
  fprintf(stderr, "%s: %s\n", what, gfnt_result_string(result));
  if (error && error->result != GFNT_OK) {
    fputs("  ", stderr);
    gfnt_error_dump(error, stderr);
  }
  return 1;
}

/** Print the name a font gives itself, when it gives itself one. */
static void print_name(const GFNT_Face * face, const char * label,
    uint16_t name_id) {
  char * text = NULL;
  GFNT_Error error;
  GFNT_Result result;

  gfnt_error_clear(&error);
  result = gfnt_face_name(face, name_id, GFNT_LANGUAGE_ANY, NULL, &text, NULL,
      &error);
  if (result == GFNT_OK) {
    printf("%s: %s\n", label, text);
    gfnt_name_free(NULL, text);
    return;
  }
  printf("%s: (%s)\n", label, gfnt_result_string(result));
}

int main(int argc, char ** argv) {
  const char * path;
  size_t index = 0;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  GFNT_Result result;
  size_t faces = 0;
  size_t glyphs = 0;
  uint16_t units = 0;
  GFNT_LineMetrics metrics;
  GFNT_CmapSubtable subtable;
  size_t strikes = 0;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <font> [face-index] [codepoint...]\n", argv[0]);
    return 2;
  }
  path = argv[1];
  if (argc > 2) {
    index = (size_t)strtoul(argv[2], NULL, 0);
  }

  gfnt_error_clear(&error);
  result = gfnt_blob_create_file(path, NULL, NULL, &blob, &error);
  if (result != GFNT_OK) {
    return fail(path, result, &error);
  }
  gfnt_blob_dump(blob, stdout);

  result = gfnt_face_count(blob, NULL, &faces, &error);
  if (result != GFNT_OK) {
    gfnt_blob_destroy(blob);
    return fail(path, result, &error);
  }
  printf("faces: %zu\n", faces);

  result = gfnt_face_load(blob, index, NULL, NULL, &face, &error);
  if (result != GFNT_OK) {
    gfnt_blob_destroy(blob);
    return fail(path, result, &error);
  }

  gfnt_face_dump(face, stdout);
  print_name(face, "family", GFNT_NAME_FAMILY);
  print_name(face, "subfamily", GFNT_NAME_SUBFAMILY);
  print_name(face, "PostScript name", GFNT_NAME_POSTSCRIPT);

  if (gfnt_face_units_per_em(face, &units, &error) == GFNT_OK) {
    printf("unitsPerEm: %u\n", units);
  }
  if (gfnt_face_num_glyphs(face, &glyphs, &error) == GFNT_OK) {
    size_t claimed = 0;

    printf("glyphs: %zu\n", glyphs);
    if (gfnt_face_num_glyphs_disagreement(face, &claimed)) {
      printf("glyphs: maxp claimed %zu; a shorter table governs\n", claimed);
    }
  }
  printf("outlines: %s\n", gfnt_face_has_outlines(face) ? "yes" : "no");
  if (gfnt_face_strike_count(face, &strikes, &error) == GFNT_OK) {
    printf("strikes: %zu\n", strikes);
  }
  else {
    printf("strikes: (%s)\n", gfnt_result_string(GFNT_ERR_UNSUPPORTED));
  }

  if (gfnt_face_line_metrics(face, GFNT_LINE_METRICS_FONT, NULL, &metrics,
          &error)
      == GFNT_OK) {
    static const char * sources[] = {"font", "typo", "hhea", "win"};

    printf("line: ascent %d, descent %d, gap %d, from %s\n", metrics.ascent,
        metrics.descent, metrics.line_gap, sources[metrics.source]);
  }

  if (gfnt_face_cmap_best(face, &subtable, &error) == GFNT_OK) {
    gfnt_face_cmap_dump(face, stdout);
  }

  for (int argument = 3; argument < argc; argument++) {
    uint32_t codepoint = (uint32_t)strtoul(argv[argument], NULL, 0);
    uint32_t glyph = 0;
    int32_t advance = 0;

    result = gfnt_face_glyph_for_codepoint(face, codepoint, &glyph, &error);
    if (result != GFNT_OK) {
      printf("U+%04" PRIX32 ": %s\n", codepoint, gfnt_result_string(result));
      continue;
    }
    if (gfnt_face_glyph_advance(face, glyph, NULL, &advance, &error)
        == GFNT_OK) {
      printf("U+%04" PRIX32 ": glyph %" PRIu32 ", advance %d\n", codepoint,
          glyph, advance);
    }
    else {
      printf("U+%04" PRIX32 ": glyph %" PRIu32 ", advance unavailable (%s)\n",
          codepoint, glyph, gfnt_result_string(result));
    }
  }

  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
