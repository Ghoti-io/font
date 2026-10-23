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
 * Outlined text: a line of text from a font, filled and then stroked, as a canvas draws `fillText` and `strokeText`, and
 * the stroke checked against the stroke of the path the scene defines for the run.
 *
 * One run holds the whole line.  A fill and a stroke of that one run are the two items of a text element, and the stroke
 * is one union across the line (so glyphs that overlap do not darken a translucent stroke where they cross), made from the
 * glyphs' unhinted outlines whatever the fill's hinting asks.  The program also strokes
 * `gshscene_glyph_run_centerline`'s path as a path of its own and requires the same pixels, which is what makes a stroke
 * of text no more than a stroke of a path: a sink that cannot stroke a run can stroke that path.
 *
 *     font-stroke-text FONT [TEXT [OUT.pam]]
 *
 * TEXT is ASCII ("Stroke" by default).  OUT.pam, if given, receives the frame (a PAM image, RGBA).
 */

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/scene.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/shape-cpu/shape-cpu.h>
#include <ghoti.io/shape-scene/shape-scene.h>

#include <stdio.h>
#include <string.h>

#define WIDTH 320
#define HEIGHT 96
#define MAX_GLYPHS 64
#define SIZE 56.0

/** A list of the line filled and stroked, or only stroked (@p fill 0), the stroke of @p centerline in @p style. */
static GSHSCENE_List * make_list(GSHSCENE_Store * store, const GSHSCENE_Resource * run, const GSHSCENE_Resource * centerline,
    const GSHSCENE_Resource * style, int fill) {
  GSHSCENE_List * list = NULL;
  GSHSCENE_Paint ink = {0};
  GSHSCENE_Paint outline = {0};
  ink.kind = GSHSCENE_PAINT_SOLID;
  ink.rgba[0] = 0.98;
  ink.rgba[1] = 0.85;
  ink.rgba[2] = 0.35;
  ink.rgba[3] = 1.0;
  outline.kind = GSHSCENE_PAINT_SOLID;
  outline.rgba[0] = 0.1;
  outline.rgba[1] = 0.1;
  outline.rgba[2] = 0.45;
  outline.rgba[3] = 0.7; /* translucent: where glyphs cross, the stroke is not darker */
  GSHGEOM_Map place;
  gshgeom_map_translate(2, 8.0, 0.0, 0.0, &place);
  if (gshscene_list_create(NULL, NULL, store, &list) != GSHSCENE_OK) {
    return NULL;
  }
  if ((fill && gshscene_list_fill(list, run, &place, GSHGEOM_FILL_NONZERO, &ink, 0, 0.0) != GSHSCENE_OK) ||
      gshscene_list_stroke(list, centerline, style, &place, &outline, 0, 0.0) != GSHSCENE_OK ||
      gshscene_list_close(list) != GSHSCENE_OK) {
    gshscene_list_destroy(list);
    return NULL;
  }
  return list;
}

static int render(const GSHSCENE_List * list, GIMG_Raster * raster) {
  GSHSCENE_Sink * sink = NULL;
  /* A line of text stroked is dense: the inner sides of its curves overlap in a pixel.  The sink refuses a stroke with more
   * pieces in a pixel than `max_exact_fragments` (512 by default) whole, so it is given room. */
  GSHCPU_Limits limits;
  gshcpu_limits_default(&limits);
  limits.max_exact_fragments = 1u << 14;
  GSHSCENE_Report * report = NULL;
  unsigned char * px = gimg_raster_pixels(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  for (int y = 0; y < HEIGHT; y++) {
    memset(px + (size_t)y * stride, 0, (size_t)WIDTH * 4u);
  }
  GSHSCENE_Target target;
  memset(&target, 0, sizeof target);
  target.kind = GSHSCENE_TARGET_RASTER;
  target.surface = raster;
  GSHSCENE_Render_Request request;
  memset(&request, 0, sizeof request);
  request.list = list;
  request.target = &target;
  int ok = gshcpu_sink_create(NULL, NULL, &limits, &sink) == GSHCPU_OK && gshscene_report_create(NULL, NULL, &report) == GSHSCENE_OK;
  if (ok) {
    request.report = report;
    ok = gshscene_sink_render(sink, &request) == GSHSCENE_OK;
    for (size_t i = 0; ok && i < gshscene_report_count(report); i++) {
      GSHSCENE_Report_Entry e;
      gshscene_report_entry(report, i, &e);
      printf("font-stroke-text: %s (%s) glyph %zu\n", gshscene_entry_class_string(e.entry_class), gshscene_reason_string(e.reason), e.second);
    }
  }
  gshscene_report_destroy(report);
  if (sink) {
    gshscene_sink_destroy(sink);
  }
  return ok;
}

int main(int argc, char ** argv) {
  if (argc < 2 || argc > 4) {
    fprintf(stderr, "usage: font-stroke-text FONT [TEXT [OUT.pam]]\n");
    return 2;
  }
  const char * text = argc >= 3 ? argv[2] : "Stroke";
  GFNT_Error error;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_SceneFont * font = NULL;
  uint16_t upem = 0;
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error) != GFNT_OK ||
      gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK ||
      gfnt_face_units_per_em(face, &upem, &error) != GFNT_OK || gfnt_scene_font_create(face, &font, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  GSHSCENE_Store * store = NULL;
  const GSHSCENE_Resource * face_resource = NULL;
  if (gshscene_store_create(NULL, NULL, &store) != GSHSCENE_OK ||
      gfnt_scene_intern_face(font, store, NULL, &face_resource, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  /* The glyphs of the line, each at the pen position the advances give. */
  GSHSCENE_Glyph glyphs[MAX_GLYPHS];
  size_t count = 0;
  double pen = 0.0;
  for (const char * c = text; *c && count < MAX_GLYPHS; c++) {
    uint32_t id = 0;
    int32_t advance = 0;
    if (gfnt_face_glyph_for_codepoint(face, (unsigned char)*c, &id, NULL) != GFNT_OK ||
        gfnt_face_glyph_advance(face, id, NULL, &advance, NULL) != GFNT_OK) {
      continue;
    }
    glyphs[count].id = id;
    glyphs[count].x = pen;
    glyphs[count].y = 70.0;
    glyphs[count].outline = NULL;
    count++;
    pen += advance * SIZE / upem;
  }
  GSHSCENE_Glyph_Run run;
  memset(&run, 0, sizeof run);
  run.source = GSHSCENE_GLYPHS_FROM_FONT;
  run.face = face_resource;
  run.size = SIZE;
  run.hint_mode = GSHSCENE_HINT_FONT; /* the fill is hinted where the font hints; the stroke never is */
  run.glyph_count = count;
  run.glyphs = glyphs;
  const GSHSCENE_Resource * line = NULL;
  GSHGEOM_Path * path = NULL;
  const GSHSCENE_Resource * centerline = NULL;
  GSHSCENE_Stroke_Style st;
  gshscene_stroke_style_init(&st);
  st.width = 3.0;
  st.join = GSHSCENE_STROKE_JOIN_ROUND;
  st.cap = GSHSCENE_STROKE_CAP_ROUND;
  const GSHSCENE_Resource * style = NULL;
  GIMG_Raster * frame = NULL;
  GIMG_Raster * check = NULL;
  int failed = 1;
  if (gshscene_store_intern_glyph_run(store, &run, &line) == GSHSCENE_OK &&
      gshscene_glyph_run_centerline(line, NULL, NULL, &path, NULL) == GSHSCENE_OK &&
      gshscene_store_intern_path(store, path, &centerline) == GSHSCENE_OK &&
      gshscene_store_intern_stroke_style(store, &st, &style) == GSHSCENE_OK &&
      gimg_raster_create(WIDTH, HEIGHT, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0, &frame) == GIMG_OK &&
      gimg_raster_create(WIDTH, HEIGHT, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0, &check) == GIMG_OK) {
    GSHSCENE_List * by_run = make_list(store, line, line, style, 1);
    GSHSCENE_List * by_path = make_list(store, line, centerline, style, 1);
    if (by_run && by_path && render(by_run, frame) && render(by_path, check)) {
      size_t bytes = gimg_raster_stride_bytes(frame) * HEIGHT;
      const unsigned char * px = gimg_raster_pixels_const(frame);
      size_t ink = 0;
      for (size_t k = 3; k < bytes; k += 4) {
        ink += px[k] != 0;
      }
      failed = memcmp(px, gimg_raster_pixels_const(check), bytes) != 0 || ink == 0;
      if (ink == 0) {
        fprintf(stderr, "font-stroke-text: nothing was drawn\n");
      }
      printf("font-stroke-text: %zu glyphs, the stroke of the run %s the stroke of its path\n", count, failed ? "differs from" : "equals");
    }
    gshscene_list_destroy(by_run);
    gshscene_list_destroy(by_path);
  }
  if (!failed && argc == 4) {
    FILE * out = fopen(argv[3], "wb");
    if (out) {
      fprintf(out, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", WIDTH, HEIGHT);
      for (int y = 0; y < HEIGHT; y++) {
        fwrite((const unsigned char *)gimg_raster_pixels_const(frame) + (size_t)y * gimg_raster_stride_bytes(frame), 4, WIDTH, out);
      }
      fclose(out);
    }
  }
  gimg_raster_destroy(frame);
  gimg_raster_destroy(check);
  gshgeom_path_destroy(path);
  gshscene_resource_release(style);
  gshscene_resource_release(centerline);
  gshscene_resource_release(line);
  gshscene_resource_release(face_resource);
  gshscene_store_destroy(store);
  gfnt_scene_font_destroy(font);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return failed ? 1 : 0;
}
