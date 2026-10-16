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
 * Print every glyph's hinted outline at a pixel size, in the form the FreeType
 * oracle driver prints it, so the two can be compared line by line.
 *
 * Usage: font-hint <font> <face> <version> <ppem> <stride> <first>
 *
 * `version` is 35 or 40. A glyph that is not hinted prints `g<N> unhinted <why>`
 * instead, and the comparison leaves it out.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>
#include "../src/hint/vm.h"

int main(int argc, char ** argv) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_HintContext * ctx = NULL;
  GFNT_Error error;
  GFNT_Result result;
  size_t glyphs = 0;
  long stride;
  long first;

  if (argc < 7) {
    fprintf(stderr, "usage: %s <font> <face> <version> <ppem> <stride> <first>\n", argv[0]);
    return 2;
  }
  stride = strtol(argv[5], NULL, 10);
  first = strtol(argv[6], NULL, 10);
  if (stride < 1) {
    stride = 1;
  }
  gfnt_error_clear(&error);
  result = gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error);
  if (result == GFNT_OK) {
    result = gfnt_face_load(blob, (size_t)strtoul(argv[2], NULL, 10), NULL, NULL, &face, &error);
  }
  if (result == GFNT_OK) {
    result = gfnt_hint_ctx_from_face(face, (uint32_t)strtoul(argv[4], NULL, 10),
        atoi(argv[3]), true, NULL, &ctx, &error);
  }
  if (result != GFNT_OK || gfnt_face_num_glyphs(face, &glyphs, &error) != GFNT_OK) {
    fprintf(stderr, "%s: %s\n", argv[1], gfnt_result_string(result));
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  if (!gfnt_hint_ctx_usable(ctx)) {
    GFNT_HintFault f = gfnt_hint_ctx_fault(ctx);

    printf("context unusable: %s in program %d at %zu (opcode 0x%02x)\n",
        gfnt_hint_failure_name(f.failure), (int)f.program, f.offset, f.opcode);
  }
  for (size_t g = (size_t)first; g < glyphs; g += (size_t)stride) {
    GFNT_Outline * outline = NULL;
    GFNT_HintStatus status;
    GFNT_HintFault fault;
    int32_t advance = 0;
    uint8_t touches[4096] = {0};
    bool normalised = false;

    result = gfnt_hint_glyph_outline(face, ctx, (uint32_t)g, &outline, &status,
        &fault, &advance, touches, sizeof touches, &normalised, &error);
    if (result != GFNT_OK) {
      printf("g%zu error %s\n", g, gfnt_result_string(result));
      continue;
    }
    if (status != GFNT_HINT_STATUS_HINTED) {
      const char * why = status == GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM ? "no-program"
          : status == GFNT_HINT_STATUS_UNHINTED_COMPOSITE ? "composite" : "failed";

      printf("g%zu unhinted %s", g, why);
      if (status == GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED) {
        printf(" %s at %zu opcode 0x%02x", gfnt_hint_failure_name(fault.failure),
            fault.offset, fault.opcode);
      }
      printf("\n");
      gfnt_outline_destroy(outline);
      continue;
    }
    printf("g%zu adv %d pts %zu ctr %zu%s\n", g, advance, gfnt_outline_point_count(outline),
        gfnt_outline_contour_count(outline), normalised ? " oblique" : "");
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
      printf("p %d %d %d t%d\n", (int)pt.x, (int)pt.y, tag == GFNT_POINT_ON,
          p < sizeof touches ? touches[p] : 0);
    }
    gfnt_outline_destroy(outline);
  }
  gfnt_hint_ctx_destroy(ctx);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
