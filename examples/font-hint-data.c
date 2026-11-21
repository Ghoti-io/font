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
 * Print the hint data of a CFF face's glyphs.
 *
 * Usage: font-hint-data <font> [face-index] [glyph-stride]
 *
 * `P` lines give the face's units per em and the Private dictionary's parameters (16.16,
 * as integers); then for each glyph `g<id> valid|invalid`, its stems (`s h|v <edge> <width>`,
 * 16.16 font units) and its mask changes (`m <first point> <hex>`). **This is the driver
 * `tools/oracle/hint_cff_edges.py` reads** when it measures what FreeType does with those
 * stems.
 */

#include <stdio.h>
#include <stdlib.h>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/charstring.h>

static void list(const char * name, const GFNT_F16Dot16 * v, size_t n) {
  size_t i;

  printf("P %s", name);
  for (i = 0; i < n; i++) {
    printf(" %d", (int)v[i]);
  }
  printf("\n");
}

int main(int argc, char ** argv) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  GFNT_Result result;
  GFNT_CharstringHintParams params;
  GFNT_CharstringHints * hints;
  size_t glyphs = 0;
  size_t stride = 1;
  uint16_t upem = 0;
  size_t index = 0;
  size_t g;
  size_t i;
  size_t k;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <font> [face-index] [glyph-stride]\n", argv[0]);
    return 2;
  }
  if (argc > 2) {
    index = (size_t)strtoul(argv[2], NULL, 10);
  }
  if (argc > 3) {
    stride = (size_t)strtoul(argv[3], NULL, 10);
    if (stride == 0) {
      stride = 1;
    }
  }
  gfnt_error_clear(&error);
  result = gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error);
  if (result == GFNT_OK) {
    result = gfnt_face_load(blob, index, NULL, NULL, &face, &error);
  }
  if (result != GFNT_OK) {
    fprintf(stderr, "%s: %s\n", argv[1], error.message);
    return 1;
  }
  hints = malloc(sizeof *hints);
  if (!hints || gfnt_face_num_glyphs(face, &glyphs, &error) != GFNT_OK
      || gfnt_face_units_per_em(face, &upem, &error) != GFNT_OK) {
    return 1;
  }
  printf("P upem %u\n", (unsigned)upem);
  for (g = 0; g < glyphs; g += stride) {
    result = gfnt_face_glyph_hints(face, (uint32_t)g, hints, &params, &error);
    if (result != GFNT_OK) {
      printf("g%zu error %d\n", g, (int)result);
      continue;
    }
    if (g == 0) {
      list("blue_values", params.blue_values, params.blue_value_count);
      list("other_blues", params.other_blues, params.other_blue_count);
      list("family_blues", params.family_blues, params.family_blue_count);
      list("family_other_blues", params.family_other_blues, params.family_other_blue_count);
      printf("P blue_scale %d\nP blue_shift %d\nP blue_fuzz %d\nP std_hw %d\nP std_vw %d\n",
          (int)params.blue_scale, (int)params.blue_shift, (int)params.blue_fuzz,
          (int)params.std_hw, (int)params.std_vw);
      list("stem_snap_h", params.stem_snap_h, params.stem_snap_h_count);
      list("stem_snap_v", params.stem_snap_v, params.stem_snap_v_count);
      printf("P force_bold %d\n", params.force_bold ? 1 : 0);
    }
    printf("g%zu %s\n", g, hints->valid ? "valid" : "invalid");
    for (i = 0; i < hints->stem_count; i++) {
      printf("s %c %d %d\n", hints->stems[i].vertical ? 'v' : 'h', (int)hints->stems[i].edge,
          (int)hints->stems[i].width);
    }
    for (i = 0; i < hints->mask_count; i++) {
      printf("m %u ", (unsigned)hints->masks[i].first_point);
      for (k = 0; k < (hints->stem_count + 7) / 8; k++) {
        printf("%02x", hints->masks[i].bits[k]);
      }
      printf("\n");
    }
  }
  free(hints);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
