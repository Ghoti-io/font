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
 * Rasterise every glyph at every size and sub-pixel offset, and print a hash.
 *
 * Usage: font-render <label> <font> [--art <glyph> <ppem>]
 *
 * **This is the golden-bitmap gate's driver** (documentation/design.md section
 * 14.4). One line per rendering, carrying the shape, the origin, the coverage
 * total and the hash; `tests/data/golden/coverage.txt` is the committed
 * collection of those lines, and the same driver built for a big-endian target
 * has to produce them byte for byte. A hash that differs between the two is a
 * host-dependent read or a `float` that crept in, and that gate is the only one
 * that sees it.
 *
 * The total is printed beside the hash deliberately. A hash says "different"
 * and nothing else; the total says *how* different, which is the difference
 * between a rendering that moved by a pixel and one that lost a contour.
 *
 * `--art` prints one rendering as characters instead, for a person looking at a
 * glyph that came out wrong.
 *
 * `label` rather than the path, because the committed file has to be
 * independent of where anyone's checkout lives.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>

/**
 * The sizes, from design.md section 14.4.
 *
 * Eight is small enough that a whole glyph is a handful of pixels and every
 * rounding shows; ninety-six is large enough that a curve is flattened into
 * many segments. The four in between are the sizes text is actually read at.
 */
static const uint32_t sizes[] = {8, 12, 16, 24, 48, 96};

/** Sub-pixel offsets: 0, a quarter, a half and three quarters of a pixel. */
static const GFNT_F26Dot6 offsets[] = {0, 16, 32, 48};

/**
 * The one vertical offset, at one size.
 *
 * Vertical sub-pixel positioning is supported and usually unwanted (section
 * 8.3), so it gets one case rather than a quarter of the file: the code path is
 * covered and the committed data does not quadruple for something no caller
 * asks for.
 */
#define GFNT_GOLDEN_VERTICAL_PPEM 16
#define GFNT_GOLDEN_VERTICAL_OFFSET 32

static int render_one(const char * label, const GFNT_Face * face,
    uint32_t glyph, uint32_t ppem, GFNT_F26Dot6 x, GFNT_F26Dot6 y) {
  GFNT_RasterOptions options;
  GFNT_Coverage coverage;
  GFNT_Error error;
  GFNT_Result result;

  memset(&options, 0, sizeof options);
  memset(&coverage, 0, sizeof coverage);
  gfnt_error_clear(&error);
  options.origin_x = x;
  options.origin_y = y;

  result = gfnt_face_render_glyph(face, glyph, ppem, &options, NULL, &coverage,
      &error);
  if (result != GFNT_OK) {
    // Printed rather than skipped: a refusal is a fact about this font at this
    // size, and a gate that omitted it would read a newly refused glyph as
    // agreement.
    printf("%s %u %u %d %d refused %s\n", label, glyph, ppem, x, y,
        gfnt_result_string(result));
    return 0;
  }
  printf("%s %u %u %d %d %ux%u+%d+%d %llu %016llX\n", label, glyph, ppem, x, y,
      coverage.width, coverage.height, coverage.left, coverage.top,
      (unsigned long long)gfnt_coverage_total(&coverage),
      (unsigned long long)gfnt_coverage_hash(&coverage));
  gfnt_coverage_destroy(&coverage);
  return 0;
}

int main(int argc, char ** argv) {
  const char * label;
  const char * path;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  size_t glyphs = 0;

  if (argc < 3) {
    fprintf(stderr, "usage: %s <label> <font> [--art <glyph> <ppem>]\n",
        argv[0]);
    return 2;
  }
  label = argv[1];
  path = argv[2];

  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(path, NULL, NULL, &blob, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  if (gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK) {
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

  if (argc >= 5 && strcmp(argv[3], "--art") == 0) {
    GFNT_RasterOptions options;
    GFNT_Coverage coverage;
    uint32_t glyph = (uint32_t)strtoul(argv[4], NULL, 10);
    uint32_t ppem = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 10) : 16;

    memset(&options, 0, sizeof options);
    memset(&coverage, 0, sizeof coverage);
    if (gfnt_face_render_glyph(face, glyph, ppem, &options, NULL, &coverage,
            &error)
        == GFNT_OK) {
      gfnt_coverage_dump(&coverage, stdout);
      gfnt_coverage_dump_art(&coverage, stdout);
      gfnt_coverage_destroy(&coverage);
    }
    else {
      gfnt_error_dump(&error, stderr);
    }
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 0;
  }

  for (size_t glyph = 0; glyph < glyphs; ++glyph) {
    for (size_t size = 0; size < sizeof sizes / sizeof *sizes; ++size) {
      for (size_t offset = 0; offset < sizeof offsets / sizeof *offsets;
          ++offset) {
        render_one(label, face, (uint32_t)glyph, sizes[size], offsets[offset],
            0);
      }
    }
    render_one(label, face, (uint32_t)glyph, GFNT_GOLDEN_VERTICAL_PPEM, 0,
        GFNT_GOLDEN_VERTICAL_OFFSET);
  }

  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
