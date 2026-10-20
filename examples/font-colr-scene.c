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
 * Lower every color glyph of a face into a scene list (story 14c;
 * scene_color.h) and print what was recorded, as JSON, for the differential
 * tools/oracle/colr_scene_diff.py.
 *
 * Each glyph is lowered at an em of the face's own units per em at the origin,
 * so the scale from design units to the list is 1 and an item's map is the
 * design map carried into y down: `L = diag(1, -1) A diag(1, -1)`,
 * `t = (b_x, -b_y)`. Per glyph with a color description:
 * `{"src": S, "refusal": R, "format": F, "mode": M, "items": [...]}` or
 * `{"error": "limit" | ...}`; an item is `[kind, glyph, paint, entry, alpha,
 * composite, m00, m01, m10, m11, t0, t1]` (glyph -1 for a group or a pop).
 *
 *     font-colr-scene <font>
 */

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/scene.h>
#include <ghoti.io/font/scene_color.h>
#include <ghoti.io/shape-scene/list.h>
#include <ghoti.io/shape-scene/resource.h>
#include <stdio.h>

static const char * error_name(GFNT_Result r) {
  switch (r) {
    case GFNT_ERR_LIMIT:
      return "limit";
    case GFNT_ERR_CORRUPT:
      return "corrupt";
    case GFNT_ERR_INVALID:
      return "invalid";
    case GFNT_ERR_OOM:
      return "oom";
    default:
      return "other";
  }
}

int main(int argc, char ** argv) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_SceneFont * scene = NULL;
  GSHSCENE_Store * store = NULL;
  const GSHSCENE_Resource * face_resource = NULL;
  GFNT_Error error;
  size_t glyphs = 0;
  uint16_t upem = 0;
  int first = 1;

  if (argc != 2) {
    fprintf(stderr, "usage: font-colr-scene <font>\n");
    return 2;
  }
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK
      || gfnt_scene_font_create(face, &scene, &error) != GFNT_OK
      || gshscene_store_create(NULL, NULL, &store) != GSHSCENE_OK
      || gfnt_scene_intern_face(scene, store, NULL, &face_resource, &error) != GFNT_OK
      || gfnt_face_num_glyphs(face, &glyphs, &error) != GFNT_OK
      || gfnt_face_units_per_em(face, &upem, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  printf("{\"glyphs\":{");
  for (size_t g = 0; g < glyphs; g++) {
    GSHSCENE_List * list = NULL;
    GFNT_ColorLowering out;
    GFNT_ColorRun run = {0};
    if (gshscene_list_create(NULL, NULL, store, &list) != GSHSCENE_OK) {
      return 1;
    }
    run.size = (int32_t)upem;
    run.hint_mode = GSHSCENE_HINT_NONE;
    GFNT_Result r = gfnt_scene_record_color_glyph(face_resource, store, list, (uint32_t)g, &run, &out, &error);
    if (r == GFNT_OK && out.source == GFNT_COLOR_SOURCE_NONE) {
      gshscene_list_destroy(list);
      continue;
    }
    printf("%s\"%zu\":", first ? "" : ",", g);
    first = 0;
    if (r != GFNT_OK && r != GFNT_ERR_UNSUPPORTED) {
      printf("{\"error\":\"%s\"}", error_name(r));
      gshscene_list_destroy(list);
      continue;
    }
    printf("{\"src\":%u,\"refusal\":%u,\"format\":%u,\"mode\":%u,\"items\":[", out.source, out.refusal,
        out.paint_format, out.composite_mode);
    for (size_t i = 0; i < gshscene_list_count(list); i++) {
      const GSHSCENE_Item * item = NULL;
      GSHSCENE_Glyph_Run glyph_run;
      long id = -1;
      gshscene_list_item(list, i, &item);
      if (item->resource && gshscene_resource_glyph_run(item->resource, &glyph_run) == GSHSCENE_OK
          && glyph_run.glyph_count == 1) {
        id = (long)glyph_run.glyphs[0].id;
      }
      printf("%s[%d,%ld,%d,%u,%.17g,%u,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g]", i ? "," : "", (int)item->kind, id,
          (int)item->paint.kind, item->paint.entry, item->paint.rgba[3], item->group.composite, item->map.m[0][0],
          item->map.m[0][1], item->map.m[1][0], item->map.m[1][1], item->map.t[0], item->map.t[1]);
    }
    printf("]}");
    gshscene_list_destroy(list);
  }
  printf("}}\n");
  gshscene_resource_release(face_resource);
  gshscene_store_destroy(store);
  gfnt_scene_font_destroy(scene);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
