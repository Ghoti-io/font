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
 * composite, m00, m01, m10, m11, t0, t1, extra]` (glyph -1 for a group or a
 * pop, -2 for the plane, -3 for a clip of a rectangle). `extra` is null, or an
 * object: `{"g": ...}` for a gradient paint (its kind, extend, space,
 * premultiplied flag, geometry, map and sorted stops, every double as the
 * library holds it), `{"space": [primaries, transfer]}` for a group with a
 * blend space, `{"box": [x0, y0, x1, y1]}` for a clip of a rectangle.
 *
 * The instance (color-paint story P4, L1) is the face resource's: the optional
 * arguments after the font are the normalised coordinates, 2.14 integers, the
 * face is interned at; none is the default instance. A `--encoded` argument
 * lowers in the encoded space (GFNT_COLOR_SPACE_ENCODED).
 *
 * With `--policies` (and `--summary`) each refused glyph is lowered again under the placeholder policy, the fallback and none
 * (color-paint story P5), into a list of its own, and `policies <glyph> <refusal> <placeholder outcome> <items> <fallback
 * outcome> <items> <none's result>` is printed, for the whole-font gate to check each policy's answer on every refusal the
 * real fonts make (`<none's result>` is 0 for a call that succeeded, the library's result number otherwise).
 *
 * With `--summary` it prints lines instead, for the whole-font gate
 * (tools/check_colr_lowered.py): `refused <glyph> <reason> <paint format>` for each
 * refused glyph, `limit <glyph>` for each past the walk's bounds, and last
 * `summary glyphs=<color glyphs> lowered=<lowered> hash=<16 hex digits>`, the hash
 * a 64-bit FNV-1a fold of each color glyph's number, outcome and (when lowered)
 * the list's own content hash and item count, each as little-endian bytes, so it
 * is the same on every target the lowering is.
 *
 *     font-colr-scene <font> [--encoded] [--summary] [--policies] [coordinate...]
 */

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/scene.h>
#include <ghoti.io/font/scene_color.h>
#include <ghoti.io/shape-scene/list.h>
#include <ghoti.io/shape-scene/resource.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static uint64_t fold(uint64_t h, uint64_t value) {
  for (int i = 0; i < 8; i++) {
    h ^= (value >> (8 * i)) & 0xFFu;
    h *= UINT64_C(1099511628211);
  }
  return h;
}

static void print_map(const GSHGEOM_Map * m) {
  printf("[%.17g,%.17g,%.17g,%.17g,%.17g,%.17g]", m->m[0][0], m->m[0][1], m->m[1][0], m->m[1][1], m->t[0], m->t[1]);
}

/** A gradient resource as the extra object of its fill. */
static void print_gradient(const GSHSCENE_Resource * resource) {
  GSHSCENE_Gradient g;

  if (gshscene_resource_gradient(resource, &g) != GSHSCENE_OK) {
    printf("null");
    return;
  }
  printf("{\"g\":{\"kind\":%u,\"extend\":%u,\"space\":[%d,%d],\"premul\":%u,\"geo\":[", g.kind, g.extend,
      (int)g.space.primaries, (int)g.space.transfer, g.premultiplied);
  if (g.kind == GSHSCENE_GRADIENT_LINEAR) {
    printf("%.17g,%.17g,%.17g,%.17g,%.17g,%.17g", g.p0[0], g.p0[1], g.p1[0], g.p1[1], g.d[0], g.d[1]);
  } else if (g.kind == GSHSCENE_GRADIENT_RADIAL) {
    printf("%.17g,%.17g,%.17g,%.17g,%.17g,%.17g", g.c0[0], g.c0[1], g.r0, g.c1[0], g.c1[1], g.r1);
  } else {
    printf("%.17g,%.17g,%.17g,%.17g", g.center[0], g.center[1], g.a0, g.a1);
  }
  printf("],\"map\":");
  print_map(&g.map);
  printf(",\"stops\":[");
  for (size_t i = 0; i < g.stop_count; i++) {
    printf("%s[%.17g,%d,%u,%.17g]", i ? "," : "", g.stops[i].offset, (int)g.stops[i].color.kind,
        g.stops[i].color.entry, g.stops[i].color.rgba[3]);
  }
  printf("]}}");
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

  GFNT_F2Dot14 coords[32];
  GFNT_Variation variation = {coords, 0, 0};
  uint32_t space = GFNT_COLOR_SPACE_STANDARD;
  int summary = 0;
  int policies = 0;
  uint64_t hash = UINT64_C(14695981039346656037);
  size_t color_glyphs = 0;
  size_t lowered = 0;

  if (argc < 2) {
    fprintf(stderr, "usage: font-colr-scene <font> [--encoded] [--summary] [coordinate...]\n");
    return 2;
  }
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--encoded") == 0) {
      space = GFNT_COLOR_SPACE_ENCODED;
    } else if (strcmp(argv[i], "--summary") == 0) {
      summary = 1;
    } else if (strcmp(argv[i], "--policies") == 0) {
      policies = 1;
    } else if (variation.count < 32) {
      coords[variation.count++] = (GFNT_F2Dot14)atoi(argv[i]);
    }
  }
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK
      || gfnt_scene_font_create(face, &scene, &error) != GFNT_OK
      || gshscene_store_create(NULL, NULL, &store) != GSHSCENE_OK
      || gfnt_scene_intern_face(scene, store, variation.count ? &variation : NULL, &face_resource, &error) != GFNT_OK
      || gfnt_face_num_glyphs(face, &glyphs, &error) != GFNT_OK
      || gfnt_face_units_per_em(face, &upem, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  if (!summary) {
    printf("{\"glyphs\":{");
  }
  for (size_t g = 0; g < glyphs; g++) {
    GSHSCENE_List * list = NULL;
    GFNT_ColorLowering out;
    GFNT_ColorRun run = {0};
    if (gshscene_list_create(NULL, NULL, store, &list) != GSHSCENE_OK) {
      return 1;
    }
    run.size = (int32_t)upem;
    run.hint_mode = GSHSCENE_HINT_NONE;
    run.space = space;
    run.refusal_policy = GFNT_COLOR_ON_REFUSAL_NONE;   /* the differential compares refusals, so none is a placeholder */
    GFNT_Result r = gfnt_scene_record_color_glyph(face_resource, store, list, (uint32_t)g, &run, &out, &error);
    if (r == GFNT_ERR_UNSUPPORTED && out.refusal == (uint32_t)GFNT_COLOR_REFUSED_WALK_LIMIT) {
      r = GFNT_ERR_LIMIT;   /* a graph past the walk's bound, which this tool and its references print as a limit */
    }
    if (r == GFNT_OK && out.source == GFNT_COLOR_SOURCE_NONE) {
      gshscene_list_destroy(list);
      continue;
    }
    if (summary) {
      color_glyphs++;
      uint32_t status = r == GFNT_OK ? 0u : r == GFNT_ERR_UNSUPPORTED ? out.refusal : 100u;
      hash = fold(hash, (uint64_t)g);
      hash = fold(hash, status);
      if (r == GFNT_OK) {
        lowered++;
        hash = fold(hash, gshscene_list_hash(list));
        hash = fold(hash, (uint64_t)out.items);
      } else if (r == GFNT_ERR_UNSUPPORTED) {
        printf("refused %zu %u %u\n", g, out.refusal, out.paint_format);
      } else {
        printf("limit %zu\n", g);
      }
      if (policies && r != GFNT_OK) {
        /* P5: the same glyph under each policy, each into a list of its own. */
        uint32_t refusal = out.refusal;
        uint32_t outcome[3] = {0, 0, 0};
        size_t items[3] = {0, 0, 0};
        GFNT_Result result[3] = {GFNT_OK, GFNT_OK, GFNT_OK};
        for (uint32_t k = 0; k < 3; k++) {
          GSHSCENE_List * own = NULL;
          GFNT_ColorLowering again;
          GFNT_ColorRun by = run;
          by.refusal_policy = k == 0 ? GFNT_COLOR_ON_REFUSAL_PLACEHOLDER
              : k == 1               ? GFNT_COLOR_ON_REFUSAL_FALLBACK
                                     : GFNT_COLOR_ON_REFUSAL_NONE;
          if (gshscene_list_create(NULL, NULL, store, &own) != GSHSCENE_OK) {
            return 1;
          }
          result[k] = gfnt_scene_record_color_glyph(face_resource, store, own, (uint32_t)g, &by, &again, NULL);
          outcome[k] = again.outcome;
          items[k] = gshscene_list_count(own);
          if (again.refusal != refusal) {
            outcome[k] = 99;   /* the gate fails on a policy that gives another reason */
          }
          gshscene_list_destroy(own);
        }
        printf("policies %zu %u %u %zu %u %zu %d\n", g, refusal, outcome[0], items[0], outcome[1], items[1],
            (int)result[2]);
      }
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
      } else if (item->resource && gshscene_resource_kind(item->resource) == GSHSCENE_RESOURCE_PLANE) {
        id = -2;
      } else if (item->resource && gshscene_resource_path(item->resource)) {
        id = -3;
      }
      printf("%s[%d,%ld,%d,%u,%.17g,%u,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,", i ? "," : "", (int)item->kind, id,
          (int)item->paint.kind, item->paint.entry, item->paint.rgba[3], item->group.composite, item->map.m[0][0],
          item->map.m[0][1], item->map.m[1][0], item->map.m[1][1], item->map.t[0], item->map.t[1]);
      if (item->kind == GSHSCENE_ITEM_FILL && item->paint.kind == GSHSCENE_PAINT_GRADIENT) {
        print_gradient(item->paint.gradient);
      } else if (item->kind == GSHSCENE_ITEM_GROUP_PUSH && item->group.blend_space.primaries) {
        printf("{\"space\":[%d,%d]}", (int)item->group.blend_space.primaries, (int)item->group.blend_space.transfer);
      } else if (id == -3) {
        GSHGEOM_Bounds b;
        if (gshgeom_path_bounds(gshscene_resource_path(item->resource), &b) == GSHGEOM_OK && !b.empty) {
          printf("{\"box\":[%.17g,%.17g,%.17g,%.17g]}", b.min.x, b.min.y, b.max.x, b.max.y);
        } else {
          printf("null");
        }
      } else {
        printf("null");
      }
      printf("]");
    }
    printf("]}");
    gshscene_list_destroy(list);
  }
  if (summary) {
    printf("summary glyphs=%zu lowered=%zu hash=%016" PRIx64 "\n", color_glyphs, lowered, hash);
  } else {
    printf("}}\n");
  }
  gshscene_resource_release(face_resource);
  gshscene_store_destroy(store);
  gfnt_scene_font_destroy(scene);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
