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
 * Lay a paragraph out and draw it.
 *
 * Usage: font-paragraph [--size PX] [--width PX] [--align start|end|left|right|center|justify]
 *            [--bidi ltr|rtl|auto] [--margin PX] [--guides] <font> <out.pgm> <text>
 *
 * One face, one size, black ink on white as a binary PGM. The lines come from
 * gfnt_paragraph_layout(): where they break, where each glyph sits, and for a
 * justified line how far its spaces were widened. `--guides` draws the column's
 * left and right edges in gray, so a justified paragraph can be seen to end on
 * the right edge and the others to fall short of it.
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/layout.h>
#include <ghoti.io/font/raster.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char ** argv) {
  const char * path = NULL, * out_path = NULL, * text = NULL;
  unsigned size = 20;
  int width = 520, margin = 24;
  bool guides = false;
  GFNT_ParagraphOptions options;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  const GFNT_Face * faces[1];
  GFNT_TextSpan span;
  GFNT_Paragraph * paragraph = NULL;
  GFNT_Layout layout;
  uint8_t * canvas;
  int w, h;

  memset(&options, 0, sizeof options);
  memset(&layout, 0, sizeof layout);
  for (int a = 1; a < argc; a++) {
    if (!strcmp(argv[a], "--size") && a + 1 < argc) size = (unsigned)atoi(argv[++a]);
    else if (!strcmp(argv[a], "--width") && a + 1 < argc) width = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--margin") && a + 1 < argc) margin = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--guides")) guides = true;
    else if (!strcmp(argv[a], "--align") && a + 1 < argc) {
      const char * v = argv[++a];

      options.align = !strcmp(v, "end") ? GFNT_ALIGN_END
          : !strcmp(v, "left") ? GFNT_ALIGN_LEFT
          : !strcmp(v, "right") ? GFNT_ALIGN_RIGHT
          : !strcmp(v, "center") ? GFNT_ALIGN_CENTER
          : !strcmp(v, "justify") ? GFNT_ALIGN_JUSTIFY : GFNT_ALIGN_START;
    }
    else if (!strcmp(argv[a], "--bidi") && a + 1 < argc) {
      const char * v = argv[++a];

      options.direction = !strcmp(v, "rtl") ? GFNT_BIDI_RTL
          : !strcmp(v, "auto") ? GFNT_BIDI_AUTO : GFNT_BIDI_LTR;
    }
    else if (argv[a][0] != '-' && !path) path = argv[a];
    else if (argv[a][0] != '-' && !out_path) out_path = argv[a];
    else if (argv[a][0] != '-' && !text) text = argv[a];
    else { fprintf(stderr, "unknown option %s\n", argv[a]); return 2; }
  }
  if (!path || !out_path || !text) {
    fprintf(stderr, "usage: font-paragraph [options] <font> <out.pgm> <text>\n");
    return 2;
  }
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(path, NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  faces[0] = face;
  memset(&span, 0, sizeof span);
  span.length = strlen(text);
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = (GFNT_F26Dot6)(size * 64);
  if (gfnt_paragraph_create(text, span.length, &span, 1, &options, NULL, &paragraph,
          &error) != GFNT_OK
      || gfnt_paragraph_layout(paragraph, (GFNT_F26Dot6)(width * 64), &layout, &error)
          != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  w = width + 2 * margin;
  h = (layout.height + 63) / 64 + 2 * margin;
  canvas = malloc((size_t)w * h);
  memset(canvas, 255, (size_t)w * h);
  if (guides) {
    for (int y = 0; y < h; y++) {
      canvas[(size_t)y * w + margin - 1] = 200;
      canvas[(size_t)y * w + margin + width] = 200;
    }
  }
  for (size_t i = 0; i < layout.line_count; i++) {
    const GFNT_Line * line = &layout.lines[i];

    for (size_t k = 0; k < line->run_count; k++) {
      const GFNT_LineRun * run = &line->runs[k];

      for (size_t g = 0; g < run->glyph_count; g++) {
        const GFNT_PlacedGlyph * pg = &run->glyphs[g];
        GFNT_RasterOptions ro;
        GFNT_Coverage cov;
        int gx = (int)floor(pg->x / 64.0), gy = (int)lround(pg->y / 64.0);

        memset(&ro, 0, sizeof ro);
        ro.origin_x = pg->x - gx * 64;
        if (gfnt_face_render_glyph(run->face, pg->glyph, size, &ro, NULL, &cov, NULL)
                != GFNT_OK) {
          continue;
        }
        for (uint32_t row = 0; row < cov.height; row++) {
          for (uint32_t col = 0; col < cov.width; col++) {
            int x = margin + gx + cov.left + (int)col;
            int y = margin + gy - cov.top + (int)row;

            if (x >= 0 && y >= 0 && x < w && y < h) {
              int ink = cov.data[row * cov.stride + col];
              int v = canvas[(size_t)y * w + x] - ink;

              canvas[(size_t)y * w + x] = (uint8_t)(v < 0 ? 0 : v);
            }
          }
        }
        gfnt_coverage_destroy(&cov);
      }
    }
    printf("line %zu: bytes %zu..%zu, x %.2f, width %.2f%s\n", i, line->start, line->end,
        line->offset / 64.0, line->extent / 64.0, line->justified ? ", justified" : "");
  }
  {
    FILE * out = fopen(out_path, "wb");

    if (!out) {
      perror(out_path);
      return 1;
    }
    fprintf(out, "P5\n%d %d\n255\n", w, h);
    fwrite(canvas, 1, (size_t)w * h, out);
    fclose(out);
  }
  free(canvas);
  gfnt_layout_free(&layout);
  gfnt_paragraph_free(paragraph);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
