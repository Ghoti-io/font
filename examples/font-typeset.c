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
 * Shape a string, rasterise it, and write the picture.
 *
 * Usage: font-typeset [options] <font> <ppem> <out.pgm> <text>
 *
 *   --script <tag>, --language <tag>, --rtl, --features <list>, --face <n>
 *                       as for font-shape
 *   --fallback <font>   a face to draw with where the first lacks a character;
 *                       repeatable, in order of preference
 *   --location <list>   a location in a variable font's design space, as
 *                       `wght=700,wdth=90`, in user coordinates
 *   --margin <n>        pixels around the text (default 16)
 *
 * Black ink on white, 8-bit, as a binary PGM. The layout - margin, baseline,
 * canvas size - follows what `hb-view` does with the same font size, so that the
 * two pictures of one string can be set side by side and read against each other
 * (`tools/oracle/typeset_compare.py` does that). Each glyph is placed at the
 * shaper's position to a sixty-fourth of a pixel and rendered at that sub-pixel
 * offset, so the spacing is the shaper's and is not rounded to whole pixels.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/shape.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>

static long utf8_decode(const char * text, uint32_t * out, size_t capacity) {
  const unsigned char * p = (const unsigned char *)text;
  size_t n = 0;

  while (*p) {
    uint32_t u;
    int more;

    if (*p < 0x80) {
      u = *p;
      more = 0;
    }
    else if ((*p & 0xE0) == 0xC0) {
      u = *p & 0x1F;
      more = 1;
    }
    else if ((*p & 0xF0) == 0xE0) {
      u = *p & 0x0F;
      more = 2;
    }
    else if ((*p & 0xF8) == 0xF0) {
      u = *p & 0x07;
      more = 3;
    }
    else {
      return -1;
    }
    p++;
    while (more--) {
      if ((*p & 0xC0) != 0x80) {
        return -1;
      }
      u = (u << 6) | (*p & 0x3F);
      p++;
    }
    if (n >= capacity) {
      return -1;
    }
    out[n++] = u;
  }
  return (long)n;
}

static GFNT_Tag tag_of(const char * text) {
  unsigned char b[4] = {' ', ' ', ' ', ' '};
  size_t i;

  for (i = 0; i < 4 && text[i]; i++) {
    b[i] = (unsigned char)text[i];
  }
  return GFNT_TAG(b[0], b[1], b[2], b[3]);
}

/** Parse `[+-]tag[=value]`, the part of HarfBuzz's syntax a picture needs. */
static int parse_features(const char * list, GFNT_ShapeFeature * out,
    size_t capacity, size_t * count) {
  const char * p = list;

  *count = 0;
  while (*p) {
    const char * comma = strchr(p, ',');
    size_t length = comma ? (size_t)(comma - p) : strlen(p);
    char buffer[32];
    char * q = buffer;
    GFNT_ShapeFeature * f;
    size_t k = 0;

    if (length == 0 || length >= sizeof buffer || *count >= capacity) {
      return -1;
    }
    memcpy(buffer, p, length);
    buffer[length] = 0;
    f = &out[*count];
    f->value = 1;
    f->start = 0;
    f->end = GFNT_SHAPE_END;
    if (*q == '-') {
      f->value = 0;
      q++;
    }
    else if (*q == '+') {
      q++;
    }
    while (q[k] && q[k] != '=') {
      k++;
    }
    if (k == 0 || k > 4) {
      return -1;
    }
    {
      char name[5] = "    ";

      memcpy(name, q, k);
      f->tag = tag_of(name);
    }
    if (q[k] == '=') {
      f->value = (uint32_t)strtoul(q + k + 1, NULL, 10);
    }
    (*count)++;
    p += length;
    if (*p == ',') {
      p++;
    }
  }
  return 0;
}

/**
 * One glyph's coverage, at a location if there is one: the face's default outline
 * has its own entry point, and a located one goes through the outline and the
 * scan converter by hand.
 */
static GFNT_Result render(const GFNT_Face * face, uint32_t glyph, uint32_t ppem,
    const GFNT_RasterOptions * ro, const GFNT_Variation * variation,
    GFNT_Coverage * cov, GFNT_Error * error) {
  GFNT_Outline * outline = NULL;
  uint16_t upem = 0;
  GFNT_Result result;

  if (!variation) {
    return gfnt_face_render_glyph(face, glyph, ppem, ro, NULL, cov, error);
  }
  result = gfnt_face_units_per_em(face, &upem, error);
  if (result == GFNT_OK) {
    result = gfnt_face_glyph_outline(face, glyph, variation, NULL, &outline,
        error);
  }
  if (result == GFNT_OK) {
    result = gfnt_outline_scale(outline, gfnt_scale_for_ppem(upem, ppem), error);
  }
  if (result == GFNT_OK) {
    result = gfnt_raster_outline(outline, ro, NULL, NULL, cov, error);
  }
  gfnt_outline_destroy(outline);
  return result;
}

int main(int argc, char ** argv) {
  const char * positional[4];
  size_t positionals = 0;
  const char * features = NULL;
  const char * location = NULL;
  GFNT_F2Dot14 normalised[64];
  GFNT_Variation variation;
  GFNT_ShapeOptions options;
  GFNT_ShapeFeature list[32];
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  GFNT_FaceRuns runs;
  GFNT_LineMetrics lines;
  const char * fallback_path[8];
  GFNT_Blob * fallback_blob[8];
  GFNT_Face * fallback_face[8];
  uint16_t fallback_upem[8];
  size_t fallbacks = 0;
  const GFNT_Face * list_faces[9];
  uint16_t list_upem[9];
  size_t gcount = 0;
  GFNT_ShapedGlyph * glyphs;
  size_t * gface;
  uint32_t codepoints[2048];
  long count;
  size_t index = 0;
  int margin = 16;
  int arg;
  uint16_t upem = 0;
  uint32_t ppem;
  int64_t asc64;
  int64_t desc64;
  int64_t total64 = 0;
  int64_t pen64 = 0;
  int width;
  int height;
  int baseline;
  uint8_t * canvas;
  size_t i;

  memset(&options, 0, sizeof options);
  for (arg = 1; arg < argc; arg++) {
    if (strcmp(argv[arg], "--script") == 0 && arg + 1 < argc) {
      options.script = tag_of(argv[++arg]);
    }
    else if (strcmp(argv[arg], "--language") == 0 && arg + 1 < argc) {
      options.language = tag_of(argv[++arg]);
    }
    else if (strcmp(argv[arg], "--rtl") == 0) {
      options.direction = GFNT_DIRECTION_RTL;
    }
    else if (strcmp(argv[arg], "--features") == 0 && arg + 1 < argc) {
      features = argv[++arg];
    }
    else if (strcmp(argv[arg], "--face") == 0 && arg + 1 < argc) {
      index = (size_t)strtoul(argv[++arg], NULL, 10);
    }
    else if (strcmp(argv[arg], "--location") == 0 && arg + 1 < argc) {
      location = argv[++arg];
    }
    else if (strcmp(argv[arg], "--fallback") == 0 && arg + 1 < argc
        && fallbacks < 8) {
      fallback_path[fallbacks++] = argv[++arg];
    }
    else if (strcmp(argv[arg], "--margin") == 0 && arg + 1 < argc) {
      margin = atoi(argv[++arg]);
    }
    else if (positionals < 4) {
      positional[positionals++] = argv[arg];
    }
    else {
      positionals = 5;
      break;
    }
  }
  if (positionals != 4) {
    fprintf(stderr,
        "usage: %s [--script tag] [--language tag] [--rtl] [--features list]"
        " [--face n] [--fallback font]... [--margin n] <font> <ppem> <out.pgm> <text>\n", argv[0]);
    return 2;
  }
  ppem = (uint32_t)strtoul(positional[1], NULL, 10);
  count = utf8_decode(positional[3], codepoints,
      sizeof codepoints / sizeof codepoints[0]);
  if (count < 0 || ppem == 0) {
    fprintf(stderr, "bad text or size\n");
    return 2;
  }
  if (features) {
    size_t n = 0;

    if (parse_features(features, list, 32, &n) != 0) {
      fprintf(stderr, "bad --features\n");
      return 2;
    }
    options.features = list;
    options.feature_count = n;
  }

  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(positional[0], NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, index, NULL, NULL, &face, &error) != GFNT_OK
      || gfnt_face_units_per_em(face, &upem, &error) != GFNT_OK
      || gfnt_face_line_metrics(face, GFNT_LINE_METRICS_FONT, NULL, &lines,
             &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  if (location) {
    GFNT_F16Dot16 user[64];
    GFNT_Axis axis;
    char copy[512];
    char * cursor;
    size_t axes = 0;
    size_t k;

    if (strlen(location) >= sizeof copy
        || gfnt_face_axis_count(face, &axes, &error) != GFNT_OK || axes > 64) {
      fprintf(stderr, "bad --location, or not a variable font\n");
      return 2;
    }
    memcpy(copy, location, strlen(location) + 1);
    for (k = 0; k < axes; ++k) {
      (void)gfnt_face_axis_at(face, k, &axis, &error);
      user[k] = axis.def;
    }
    for (cursor = strtok(copy, ","); cursor; cursor = strtok(NULL, ",")) {
      char * equals = strchr(cursor, '=');
      bool found = false;

      if (!equals || equals - cursor != 4) {
        fprintf(stderr, "bad location item '%s'\n", cursor);
        return 2;
      }
      for (k = 0; k < axes; ++k) {
        (void)gfnt_face_axis_at(face, k, &axis, &error);
        if (axis.tag == GFNT_TAG(cursor[0], cursor[1], cursor[2], cursor[3])) {
          double value = strtod(equals + 1, NULL);

          user[k] = (GFNT_F16Dot16)(value * 65536.0 + (value < 0 ? -0.5 : 0.5));
          found = true;
        }
      }
      if (!found) {
        fprintf(stderr, "this face has no axis '%.4s'\n", cursor);
        return 2;
      }
    }
    if (gfnt_face_normalize(face, user, axes, normalised, 64, &error)
        != GFNT_OK) {
      gfnt_error_dump(&error, stderr);
      return 1;
    }
    variation.coords = normalised;
    variation.count = axes;
    variation.delta_rounding = GFNT_DELTA_ROUND_HALF_UP;
    options.variation = &variation;
  }
  if (fallbacks && location) {
    fprintf(stderr, "--fallback and --location cannot be combined\n");
    return 2;
  }
  list_faces[0] = face;
  list_upem[0] = upem;
  for (i = 0; i < fallbacks; i++) {
    if (gfnt_blob_create_file(fallback_path[i], NULL, NULL, &fallback_blob[i],
            &error) != GFNT_OK
        || gfnt_face_load(fallback_blob[i], 0, NULL, NULL, &fallback_face[i],
               &error) != GFNT_OK
        || gfnt_face_units_per_em(fallback_face[i], &fallback_upem[i], &error)
               != GFNT_OK) {
      gfnt_error_dump(&error, stderr);
      return 1;
    }
    list_faces[i + 1] = fallback_face[i];
    list_upem[i + 1] = fallback_upem[i];
  }
  memset(&runs, 0, sizeof runs);
  if (gfnt_faces_shape(list_faces, fallbacks + 1, codepoints, (size_t)count,
          &options, NULL, &runs, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }

  // Everything in 26.6 pixels: font units times ppem over the em, rounded.
#define SCALE_OF(units, em) \
  (((int64_t)(units) * (int64_t)ppem * 64 + ((units) >= 0 ? (int64_t)(em) / 2 \
      : -(int64_t)(em) / 2)) / (int64_t)(em))
#define SCALE64(units) SCALE_OF(units, upem)
  asc64 = SCALE64(lines.ascent);
  desc64 = SCALE64(-lines.descent);
  // One list of glyphs, each with the face that draws it, in drawing order.
  for (i = 0; i < runs.count; i++) {
    gcount += runs.runs[i].run.count;
  }
  glyphs = malloc((gcount ? gcount : 1) * sizeof *glyphs);
  gface = malloc((gcount ? gcount : 1) * sizeof *gface);
  if (!glyphs || !gface) {
    return 1;
  }
  gcount = 0;
  for (i = 0; i < runs.count; i++) {
    size_t k;

    for (k = 0; k < runs.runs[i].run.count; k++) {
      glyphs[gcount] = runs.runs[i].run.glyphs[k];
      gface[gcount++] = runs.runs[i].face;
    }
  }
  for (i = 0; i < gcount; i++) {
    total64 += SCALE_OF(glyphs[i].x_advance, list_upem[gface[i]]);
  }
  width = (int)((total64 + 63) / 64) + 2 * margin;
  baseline = margin + (int)((asc64 + 32) / 64);
  height = baseline + (int)((desc64 + 63) / 64) + margin;
  canvas = malloc((size_t)width * (size_t)height);
  if (!canvas) {
    return 1;
  }
  memset(canvas, 0, (size_t)width * (size_t)height); // coverage, then inverted

  for (i = 0; i < gcount; i++) {
    const GFNT_ShapedGlyph * g = &glyphs[i];
    const GFNT_Face * gf = list_faces[gface[i]];
    uint16_t gem = list_upem[gface[i]];
    int64_t x64 = (int64_t)margin * 64 + pen64 + SCALE_OF(g->x_offset, gem);
    int64_t y64 = -SCALE_OF(g->y_offset, gem); // y down
    GFNT_RasterOptions ro;
    GFNT_Coverage cov;
    int64_t ix = x64 >> 6;
    int64_t iy = (-y64) >> 6; // whole pixels up; the rest goes to the rasteriser
    uint32_t row;
    uint32_t col;

    memset(&ro, 0, sizeof ro);
    memset(&cov, 0, sizeof cov);
    ro.origin_x = (GFNT_F26Dot6)(x64 & 63);
    ro.origin_y = (GFNT_F26Dot6)((-y64) & 63);
    pen64 += SCALE_OF(g->x_advance, gem);
    if (render(gf, g->glyph, ppem, &ro, location ? &variation : NULL, &cov,
            &error) != GFNT_OK) {
      continue;
    }
    for (row = 0; row < cov.height; row++) {
      int64_t y = (int64_t)baseline - cov.top + (int64_t)row - iy;
      for (col = 0; col < cov.width; col++) {
        int64_t x = ix + cov.left + (int64_t)col;
        unsigned sum;

        if (x < 0 || x >= width || y < 0 || y >= height) {
          continue;
        }
        sum = canvas[(size_t)y * (size_t)width + (size_t)x]
            + gfnt_coverage_at(&cov, col, row);
        canvas[(size_t)y * (size_t)width + (size_t)x] =
            (uint8_t)(sum > 255 ? 255 : sum);
      }
    }
    gfnt_coverage_destroy(&cov);
  }
  {
    FILE * out = fopen(positional[2], "wb");

    if (!out) {
      perror(positional[2]);
      return 1;
    }
    fprintf(out, "P5\n%d %d\n255\n", width, height);
    for (i = 0; i < (size_t)width * (size_t)height; i++) {
      fputc(255 - canvas[i], out);
    }
    fclose(out);
  }
  free(canvas);
  free(glyphs);
  free(gface);
  gfnt_face_runs_free(&runs);
  for (i = 0; i < fallbacks; i++) {
    gfnt_face_free(fallback_face[i]);
    gfnt_blob_destroy(fallback_blob[i]);
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
