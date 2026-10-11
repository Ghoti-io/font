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
 * The box the shaper measures each glyph with, for `tools/oracle/color_extents_diff.py`.
 *
 * Usage: font-extents [--ppem N] <font>
 *
 * Reads hexadecimal code points from standard input, one per line, and prints
 * one JSON line each: `[glyph,x_bearing,y_bearing,width,height]` in font units
 * with y up (HarfBuzz's `hb_glyph_extents_t`), or `null` when the face cannot
 * say.
 *
 * **This reaches past the public headers on purpose.** `gfnt_glyph_extents()`
 * is the shaper's own measure, used to place a mark when no `GPOS` does, and it
 * is not part of the API; the differential holds it to HarfBuzz directly rather
 * than through the offsets it happens to produce. With `--ppem N` it measures as a font
 * drawn at that many pixels per em is, which is how a color strike is chosen.
 */

#include <ghoti.io/font/font.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/shape/fallback.h"

int main(int argc, char ** argv) {
  const char * path = NULL;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  char line[64];
  uint32_t ppem = 0;

  for (int a = 1; a < argc; a++) {
    if (strcmp(argv[a], "--ppem") == 0 && a + 1 < argc) {
      ppem = (uint32_t)strtoul(argv[++a], NULL, 10);
    }
    else if (argv[a][0] != '-') {
      path = argv[a];
    }
    else {
      fprintf(stderr, "unknown option %s\n", argv[a]);
      return 2;
    }
  }
  if (!path) {
    fprintf(stderr, "usage: font-extents <font>\n");
    return 2;
  }
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(path, NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  while (fgets(line, sizeof line, stdin)) {
    uint32_t cp = (uint32_t)strtoul(line, NULL, 16);
    uint32_t glyph = 0;
    GFNT_Extents e;

    if (gfnt_face_glyph_for_codepoint(face, cp, &glyph, NULL) != GFNT_OK
        || !gfnt_glyph_extents_at(face, glyph, NULL, ppem, &e)) {
      printf("null\n");
      continue;
    }
    printf("[%u,%d,%d,%d,%d]\n", glyph, e.x_bearing, e.y_bearing, e.width, e.height);
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
