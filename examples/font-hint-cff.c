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
 * Print every glyph's hinted outline of a CFF, CFF2 or Type 1 face at a pixel size, in the
 * form the FreeType oracle driver prints it, so the two can be compared line by line.
 *
 * Usage: font-hint-cff <font> <face> <ppem> <stride> <first> [coords [auto]]
 *
 * `auto` hints with the automatic hinter instead of the face's own stems.
 *
 * `coords` is an optional location for a `CFF2` face: comma-separated normalised coordinates in 2.14, or `-`.
 *
 * A glyph that is not hinted prints `g<N> unhinted <why>`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/hint.h>
#include <ghoti.io/font/outline.h>

int main(int argc, char ** argv) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_HintContext * ctx = NULL;
  GFNT_Error error;
  GFNT_Result result;
  size_t glyphs = 0;
  long stride;
  long first;

  if (argc < 6) {
    fprintf(stderr, "usage: %s <font> <face> <ppem> <stride> <first>\n", argv[0]);
    return 2;
  }
  stride = strtol(argv[4], NULL, 10);
  first = strtol(argv[5], NULL, 10);
  if (stride < 1) {
    stride = 1;
  }
  gfnt_error_clear(&error);
  result = gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error);
  if (result == GFNT_OK) {
    result = gfnt_face_load(blob, (size_t)strtoul(argv[2], NULL, 10), NULL, NULL, &face, &error);
  }
  if (result == GFNT_OK) {
    GFNT_F2Dot14 coords[64];
    GFNT_Variation variation = {coords, 0, 0};

    for (const char * c = argc > 6 && strcmp(argv[6], "-") != 0 ? argv[6] : ""; *c && variation.count < 64;) {
      char * end;

      coords[variation.count++] = (GFNT_F2Dot14)strtol(c, &end, 10);
      c = *end == ',' ? end + 1 : end;
    }
    result = gfnt_hint_context_create(face, (uint32_t)strtoul(argv[3], NULL, 10), GFNT_HINT_VERSION_AUTO,
        argc > 7 && strcmp(argv[7], "auto") == 0 ? GFNT_HINT_MODE_AUTO : GFNT_HINT_MODE_FONT,
        variation.count ? &variation : NULL, &ctx, &error);
  }
  if (result != GFNT_OK || gfnt_face_num_glyphs(face, &glyphs, &error) != GFNT_OK) {
    fprintf(stderr, "%s: %s\n", argv[1], gfnt_result_string(result));
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  for (size_t g = (size_t)first; g < glyphs; g += (size_t)stride) {
    GFNT_Outline * outline = NULL;
    GFNT_HintStatus status = GFNT_HINT_STATUS_HINTED;
    GFNT_F26Dot6 advance = 0;

    result = gfnt_face_hinted_outline(face, ctx, (uint32_t)g, NULL, &outline, &status, &advance, &error);
    if (result != GFNT_OK) {
      printf("g%zu error %s\n", g, gfnt_result_string(result));
      continue;
    }
    if (status != GFNT_HINT_STATUS_HINTED) {
      printf("g%zu unhinted status %d\n", g, (int)status);
      gfnt_outline_destroy(outline);
      continue;
    }
    printf("g%zu adv %d pts %zu ctr %zu\n", g, (int)advance, gfnt_outline_point_count(outline),
        gfnt_outline_contour_count(outline));
    printf("e");
    for (size_t c = 0; c < gfnt_outline_contour_count(outline); ++c) {
      size_t f = 0;
      size_t n = 0;

      gfnt_outline_contour_at(outline, c, &f, &n);
      printf(" %zu", f + n - 1u);
    }
    printf("\n");
    for (size_t p = 0; p < gfnt_outline_point_count(outline); ++p) {
      GFNT_Point pt;
      GFNT_PointTag tag;

      gfnt_outline_point_at(outline, p, &pt, &tag);
      printf("p %d %d %d t0\n", (int)pt.x, (int)pt.y, tag == GFNT_POINT_ON);
    }
    gfnt_outline_destroy(outline);
  }
  gfnt_hint_context_destroy(ctx);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
