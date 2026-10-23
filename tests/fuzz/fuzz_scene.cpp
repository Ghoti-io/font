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
 * The glyph path provider and the COLR lowering, fed a font's bytes (story 16
 * of the shape family's first milestone; CAP-15, AD-19).  Both read untrusted
 * bytes on the way to a scene and no other harness reaches them: the provider's
 * ops are called by a sink, and the lowering walks a `COLR` paint graph.
 *
 * Four option bytes, then the font: the hint mode and whether the run is turned
 * (the general route) or placed on whole pixels (the fast tier), the ppem, the
 * first glyph and how many.  Each glyph is drawn through `shape-cpu` from a
 * one-glyph run, and its color description is lowered into a list, which is
 * drawn too when the lowering succeeds.  A refusal is an answer; a crash, a hang
 * or an abort is the failure.
 */

#include <cstdint>
#include <cstring>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/scene.h>
#include <ghoti.io/font/scene_color.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/shape-cpu/shape-cpu.h>
#include <ghoti.io/shape-scene/shape-scene.h>

namespace {

constexpr uint32_t kSide = 48;

void draw(GSHSCENE_Sink * sink, GSHSCENE_Report * report, GIMG_Raster * raster, const GSHSCENE_List * list) {
  GSHSCENE_Target target;
  std::memset(&target, 0, sizeof target);
  target.kind = GSHSCENE_TARGET_RASTER;
  target.surface = raster;
  GSHSCENE_Render_Request request;
  std::memset(&request, 0, sizeof request);
  request.list = list;
  request.target = &target;
  request.report = report;
  gshscene_sink_render(sink, &request);
}

/**
 * The face resource the lowering and the runs use: the default instance, or (options bit 6) the instance whose
 * normalised coordinates come from two header bytes, tried with two axes and then one (a face with fewer axes refuses
 * the longer variation), so that the variable paints are read at a location the fuzzer picks (L1 of color-paint story P4).
 */
bool intern_instance(GFNT_SceneFont * font, GSHSCENE_Store * store, uint8_t options, uint8_t a, uint8_t b,
    const GSHSCENE_Resource ** out) {
  if (!(options & 0x40u)) {
    return gfnt_scene_intern_face(font, store, nullptr, out, nullptr) == GFNT_OK;
  }
  GFNT_F2Dot14 coords[2] = {(GFNT_F2Dot14)((int)a * 128 - 16384), (GFNT_F2Dot14)((int)b * 128 - 16384)};
  for (size_t count = 2; count >= 1; count--) {
    GFNT_Variation v = {coords, count, GFNT_DELTA_ROUND_HALF_UP};
    if (gfnt_scene_intern_face(font, store, &v, out, nullptr) == GFNT_OK) {
      return true;
    }
  }
  return gfnt_scene_intern_face(font, store, nullptr, out, nullptr) == GFNT_OK;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 5) {
    return 0;
  }
  const uint8_t options = data[0];
  const uint32_t ppem = 6u + data[1] % 40u;
  const uint32_t first = data[2];
  const uint32_t count = 1u + data[3] % 8u;

  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_SceneFont * font = nullptr;
  GSHSCENE_Store * store = nullptr;
  const GSHSCENE_Resource * face_resource = nullptr;
  GSHSCENE_Sink * sink = nullptr;
  GSHSCENE_Report * report = nullptr;
  GIMG_Raster * raster = nullptr;
  size_t glyphs = 0;
  if (gfnt_blob_create_memory(data + 4, size - 4, GFNT_BLOB_BORROWED, &limits, nullptr, &blob, nullptr) != GFNT_OK ||
      gfnt_face_load(blob, 0, &limits, nullptr, &face, nullptr) != GFNT_OK ||
      gfnt_face_num_glyphs(face, &glyphs, nullptr) != GFNT_OK ||
      gfnt_scene_font_create(face, &font, nullptr) != GFNT_OK ||
      gshscene_store_create(nullptr, nullptr, &store) != GSHSCENE_OK ||
      !intern_instance(font, store, options, data[2], data[3], &face_resource) ||
      gshcpu_sink_create(nullptr, nullptr, nullptr, &sink) != GSHCPU_OK ||
      gshscene_report_create(nullptr, nullptr, &report) != GSHSCENE_OK ||
      gimg_raster_create(kSide, kSide, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &raster) != GIMG_OK) {
    goto done;
  }
  for (uint32_t g = first; g < first + count && g < glyphs + 2u; g++) {
    // The outline, through a one-glyph run on the route the map picks.
    GSHSCENE_Glyph glyph = {g, 0.0, 0.0, nullptr};
    GSHSCENE_Glyph_Run run;
    std::memset(&run, 0, sizeof run);
    run.source = GSHSCENE_GLYPHS_FROM_FONT;
    run.face = face_resource;
    run.size = (double)ppem;
    run.hint_mode = options % 3u;
    run.glyph_count = 1;
    run.glyphs = &glyph;
    const GSHSCENE_Resource * text = nullptr;
    GSHSCENE_List * list = nullptr;
    if (gshscene_store_intern_glyph_run(store, &run, &text) == GSHSCENE_OK &&
        gshscene_list_create(nullptr, nullptr, store, &list) == GSHSCENE_OK) {
      GSHGEOM_Map map;
      if (options & 4u) {
        GSHGEOM_Map turn, move;
        gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, 20.0 + (double)(options >> 4), &turn);
        gshgeom_map_translate(2, 12.0, 36.0, 0.0, &move);
        gshgeom_map_compose(&move, &turn, &map);
      } else {
        gshgeom_map_translate(2, 8.0, 36.0, 0.0, &map);
      }
      if (gshscene_list_fill(list, text, &map, GSHGEOM_FILL_NONZERO, nullptr, 0, 0.0) == GSHSCENE_OK &&
          gshscene_list_close(list) == GSHSCENE_OK) {
        draw(sink, report, raster, list);
      }
    }
    gshscene_list_destroy(list);
    gshscene_resource_release(text);

    // The color description, lowered into a list of its own, and drawn when it lowers.
    if (options & 8u) {
      GSHSCENE_List * colored = nullptr;
      if (gshscene_list_create(nullptr, nullptr, store, &colored) == GSHSCENE_OK) {
        GFNT_ColorRun crun;
        std::memset(&crun, 0, sizeof crun);
        crun.size = (int32_t)ppem;
        crun.x = 8;
        crun.y = 36;
        crun.hint_mode = run.hint_mode;
        crun.space = (options & 0x80u) ? GFNT_COLOR_SPACE_ENCODED : GFNT_COLOR_SPACE_STANDARD;
        crun.refusal_policy = (options >> 4) % 3u;   // a refused glyph is a placeholder, a fallback or an error: all drawn or refused
        GFNT_ColorLowering out;
        if (gfnt_scene_record_color_glyph(face_resource, store, colored, g, &crun, &out, nullptr) == GFNT_OK &&
            gshscene_list_close(colored) == GSHSCENE_OK) {
          draw(sink, report, raster, colored);
        }
      }
      gshscene_list_destroy(colored);
    }
  }

done:
  gimg_raster_destroy(raster);
  gshscene_report_destroy(report);
  if (sink) {
    gshscene_sink_destroy(sink);
  }
  gshscene_resource_release(face_resource);
  gshscene_store_destroy(store);
  gfnt_scene_font_destroy(font);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
