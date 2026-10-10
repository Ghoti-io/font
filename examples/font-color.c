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
 * Print a face's palettes and color layers as JSON, for the differential.
 *
 * Usage: font-color <font>
 *
 *   {"palettes":[{"flags":F,"label":L,"colors":[[r,g,b,a],...]}...],
 *    "entry_labels":[...],"version":V,
 *    "layers":{"<base glyph>":[[glyph,palette_index],...],...}}
 *
 * Every glyph of the face is asked, so a glyph the table lists and the reader
 * cannot find is a difference. A table the face lacks is null.
 */

#include <ghoti.io/font/font.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char ** argv) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  size_t palettes = 0, entries = 0, glyphs = 0;
  uint16_t version = 0;
  bool first;

  if (argc != 2) {
    fprintf(stderr, "usage: font-color <font>\n");
    return 2;
  }
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  printf("{\"palettes\":");
  if (gfnt_face_palette_count(face, &palettes, NULL) == GFNT_OK
      && gfnt_face_palette_entry_count(face, &entries, NULL) == GFNT_OK) {
    printf("[");
    for (size_t p = 0; p < palettes; p++) {
      GFNT_PaletteInfo info;

      if (gfnt_face_palette_info(face, p, &info, &error) != GFNT_OK) {
        printf("%s{\"error\":\"info\"}", p ? "," : "");
        continue;
      }
      printf("%s{\"flags\":%u,\"label\":%u,\"colors\":[", p ? "," : "", info.flags,
          info.label);
      for (size_t e = 0; e < entries; e++) {
        GFNT_Color c;

        if (gfnt_face_palette_color(face, p, e, &c, &error) != GFNT_OK) {
          printf("%snull", e ? "," : "");
          continue;
        }
        printf("%s[%u,%u,%u,%u]", e ? "," : "", c.red, c.green, c.blue, c.alpha);
      }
      printf("]}");
    }
    printf("],\"entry_labels\":[");
    for (size_t e = 0; e < entries; e++) {
      uint16_t label = 0;

      gfnt_face_palette_entry_label(face, e, &label, NULL);
      printf("%s%u", e ? "," : "", label);
    }
    printf("]");
  }
  else {
    printf("null,\"entry_labels\":null");
  }
  printf(",\"version\":");
  if (gfnt_face_color_version(face, &version, NULL) == GFNT_OK) {
    printf("%u,\"layers\":{", version);
  }
  else {
    printf("null,\"layers\":null}\n");
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 0;
  }
  gfnt_face_num_glyphs(face, &glyphs, NULL);
  first = true;
  for (uint32_t g = 0; g < glyphs; g++) {
    size_t n = 0;

    if (gfnt_face_color_layer_count(face, g, &n, NULL) != GFNT_OK || n == 0) {
      continue;
    }
    printf("%s\"%u\":[", first ? "" : ",", g);
    first = false;
    for (size_t k = 0; k < n; k++) {
      GFNT_ColorLayer layer;

      if (gfnt_face_color_layer(face, g, k, &layer, NULL) != GFNT_OK) {
        printf("%snull", k ? "," : "");
        continue;
      }
      printf("%s[%u,%u]", k ? "," : "", layer.glyph, layer.palette_index);
    }
    printf("]");
  }
  printf("}}\n");
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
