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
 * Usage: font-color [--location wght=700,wdth=80] <font>
 *
 *   {"palettes":[{"flags":F,"label":L,"colors":[[r,g,b,a],...]}...],
 *    "entry_labels":[...],"version":V,
 *    "layers":{"<base glyph>":[[glyph,palette_index],...],...},
 *    "paints":{"<base glyph>":NODE,...},"clips":{"<glyph>":[x0,y0,x1,y1],...}}
 *
 * A NODE is {"f":format,...,"c":[children]}: version 1's paint graph walked by
 * gfnt_face_color_paint_walk(), in the file's own units, the variable forms at
 * the --location.
 *
 * Every glyph of the face is asked, so a glyph the table lists and the reader
 * cannot find is a difference. A table the face lacks is null.
 */

#include <ghoti.io/font/font.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** How many numbers v[] holds for each paint format. */
static int value_count(GFNT_PaintFormat f) {
  switch (f) {
    case GFNT_PAINT_LINEAR_GRADIENT: case GFNT_PAINT_VAR_LINEAR_GRADIENT:
    case GFNT_PAINT_RADIAL_GRADIENT: case GFNT_PAINT_VAR_RADIAL_GRADIENT:
    case GFNT_PAINT_TRANSFORM: case GFNT_PAINT_VAR_TRANSFORM:
      return 6;
    case GFNT_PAINT_SWEEP_GRADIENT: case GFNT_PAINT_VAR_SWEEP_GRADIENT:
    case GFNT_PAINT_SCALE_AROUND_CENTER: case GFNT_PAINT_VAR_SCALE_AROUND_CENTER:
    case GFNT_PAINT_SKEW_AROUND_CENTER: case GFNT_PAINT_VAR_SKEW_AROUND_CENTER:
      return 4;
    case GFNT_PAINT_SCALE_UNIFORM_AROUND_CENTER:
    case GFNT_PAINT_VAR_SCALE_UNIFORM_AROUND_CENTER:
    case GFNT_PAINT_ROTATE_AROUND_CENTER: case GFNT_PAINT_VAR_ROTATE_AROUND_CENTER:
      return 3;
    case GFNT_PAINT_TRANSLATE: case GFNT_PAINT_VAR_TRANSLATE:
    case GFNT_PAINT_SCALE: case GFNT_PAINT_VAR_SCALE:
    case GFNT_PAINT_SKEW: case GFNT_PAINT_VAR_SKEW:
      return 2;
    case GFNT_PAINT_SCALE_UNIFORM: case GFNT_PAINT_VAR_SCALE_UNIFORM:
    case GFNT_PAINT_ROTATE: case GFNT_PAINT_VAR_ROTATE:
      return 1;
    default:
      return 0;
  }
}

typedef struct Printer {
  const GFNT_Face * face;
  const GFNT_Variation * variation;
  bool first[128];   // per depth: no child printed yet
  bool failed;
} Printer;

static bool enter(void * user, const GFNT_ColorPaint * paint, size_t depth) {
  Printer * p = user;
  int n = value_count(paint->format);

  if (depth > 0) {
    printf("%s", p->first[depth - 1] ? "" : ",");
    p->first[depth - 1] = false;
  }
  p->first[depth] = true;
  printf("{\"f\":%d", (int)paint->format);
  if (paint->format == GFNT_PAINT_SOLID || paint->format == GFNT_PAINT_VAR_SOLID) {
    printf(",\"pal\":%u,\"a\":%d", paint->palette_index, paint->v[0]);
  }
  if (paint->format == GFNT_PAINT_GLYPH || paint->format == GFNT_PAINT_COLR_GLYPH) {
    printf(",\"g\":%u", paint->glyph);
  }
  if (paint->format == GFNT_PAINT_COMPOSITE) {
    printf(",\"mode\":%u", paint->composite_mode);
  }
  if (n) {
    printf(",\"v\":[");
    for (int i = 0; i < n; i++) {
      printf("%s%d", i ? "," : "", paint->v[i]);
    }
    printf("]");
  }
  if (paint->color_line) {
    GFNT_ColorLine line;

    if (gfnt_face_color_line(p->face, paint, &line, NULL) != GFNT_OK) {
      p->failed = true;
      printf(",\"line\":null");
    }
    else {
      printf(",\"line\":{\"ext\":%d,\"stops\":[", (int)line.extend);
      for (size_t k = 0; k < line.stop_count; k++) {
        GFNT_ColorStop stop;

        if (gfnt_face_color_stop(p->face, paint, k, p->variation, &stop, NULL)
            != GFNT_OK) {
          p->failed = true;
          printf("%snull", k ? "," : "");
          continue;
        }
        printf("%s[%d,%u,%d]", k ? "," : "", stop.offset, stop.palette_index,
            stop.alpha);
      }
      printf("]}");
    }
  }
  printf(",\"c\":[");
  return true;
}

static void leave(void * user, const GFNT_ColorPaint * paint, size_t depth) {
  (void)user;
  (void)paint;
  (void)depth;
  printf("]}");
}

static int parse_location(const GFNT_Face * face, const char * text,
    GFNT_F2Dot14 * normalized, GFNT_Variation * variation) {
  GFNT_F16Dot16 user[64];
  GFNT_Axis axis;
  GFNT_Error error;
  char copy[512];
  char * cursor;
  size_t axes = 0;

  if (strlen(text) >= sizeof copy
      || gfnt_face_axis_count(face, &axes, &error) != GFNT_OK || axes > 64) {
    return -1;
  }
  memcpy(copy, text, strlen(text) + 1);
  for (size_t k = 0; k < axes; k++) {
    (void)gfnt_face_axis_at(face, k, &axis, &error);
    user[k] = axis.def;
  }
  for (cursor = strtok(copy, ","); cursor; cursor = strtok(NULL, ",")) {
    char * equals = strchr(cursor, '=');
    bool found = false;

    if (!equals || equals - cursor != 4) {
      return -1;
    }
    for (size_t k = 0; k < axes; k++) {
      (void)gfnt_face_axis_at(face, k, &axis, &error);
      if (axis.tag == GFNT_TAG(cursor[0], cursor[1], cursor[2], cursor[3])) {
        double value = strtod(equals + 1, NULL);

        user[k] = (GFNT_F16Dot16)(value * 65536.0 + (value < 0 ? -0.5 : 0.5));
        found = true;
      }
    }
    if (!found) {
      return -1;
    }
  }
  if (gfnt_face_normalize(face, user, axes, normalized, 64, &error) != GFNT_OK) {
    return -1;
  }
  memset(variation, 0, sizeof *variation);
  variation->coords = normalized;
  variation->count = axes;
  return 0;
}

int main(int argc, char ** argv) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  const char * location = NULL;
  const char * path;
  GFNT_F2Dot14 normalized[64];
  GFNT_Variation variation;
  const GFNT_Variation * at = NULL;
  size_t palettes = 0, entries = 0, glyphs = 0;
  uint16_t version = 0;
  bool first;

  if (argc == 4 && strcmp(argv[1], "--location") == 0) {
    location = argv[2];
    path = argv[3];
  }
  else if (argc == 2) {
    path = argv[1];
  }
  else {
    fprintf(stderr, "usage: font-color [--location axes] <font>\n");
    return 2;
  }
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(path, NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  printf("{");
  if (at) {
    printf("\"coords\":[");
    for (size_t k = 0; k < at->count; k++) {
      printf("%s%d", k ? "," : "", at->coords[k]);
    }
    printf("],");
  }
  printf("\"palettes\":");
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
  if (location) {
    if (parse_location(face, location, normalized, &variation) != 0) {
      fprintf(stderr, "bad --location, or not a variable font\n");
      return 2;
    }
    at = &variation;
  }
  printf(",\"version\":");
  if (gfnt_face_color_version(face, &version, NULL) == GFNT_OK) {
    printf("%u,\"layers\":{", version);
  }
  else {
    printf("null,\"layers\":null,\"paints\":null,\"clips\":null}\n");
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
  printf("}");
  if (version == 1) {
    GFNT_PaintVisitor visitor = {enter, leave};
    Printer printer;
    bool firstp = true;

    memset(&printer, 0, sizeof printer);
    printer.face = face;
    printer.variation = at;
    printf(",\"paints\":{");
    for (uint32_t g = 0; g < glyphs; g++) {
      GFNT_PaintRef root = 0;

      if (gfnt_face_color_base_paint(face, g, &root, NULL) != GFNT_OK || !root) {
        continue;
      }
      printf("%s\"%u\":", firstp ? "" : ",", g);
      firstp = false;
      if (gfnt_face_color_paint_walk(face, g, at, &visitor, &printer, &error)
          != GFNT_OK) {
        printf("null");
        printer.failed = true;
      }
    }
    printf("},\"clips\":{");
    firstp = true;
    for (uint32_t g = 0; g < glyphs; g++) {
      GFNT_ColorBox box;
      bool found = false;

      if (gfnt_face_color_clip(face, g, at, &box, &found, NULL) != GFNT_OK
          || !found) {
        continue;
      }
      printf("%s\"%u\":[%d,%d,%d,%d]", firstp ? "" : ",", g, box.x_min, box.y_min,
          box.x_max, box.y_max);
      firstp = false;
    }
    printf("}");
  }
  else {
    printf(",\"paints\":null,\"clips\":null");
  }
  printf("}\n");
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
