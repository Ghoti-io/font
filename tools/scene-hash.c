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
 * The cross-target probe of the glyph path provider (story 14b of the shape
 * family's first milestone): AD-5's and AD-1's fixtures, rendered through
 * `shape-cpu`, hashed in one spelling on every target.
 *
 *     scene-hash FONTS_DIR
 *
 * For each fixture below, at each size of kPpems, every glyph (up to
 * kMaxGlyphs) is drawn twice through shape-cpu with hinting off: on the fast
 * tier (this library's device paths) and on the general route under the
 * snapped map. The two rasters must be equal (AD-5) and the fast one is folded
 * into the hash. Then, for up to kAd1Glyphs glyphs, in modes NONE and FONT, at
 * two sizes, `gfnt_face_render_glyph` must equal a one-glyph scene's alpha
 * (AD-1), and the coverage is folded. A failure of either equality prints
 * `font-scene.FAIL` and exits 1, so a target is checked for the property
 * itself and not only for agreeing with the host.
 *
 * Output: `font-scene.fixture NAME HASH` per fixture, then `font-scene.hash`
 * and `font-scene.count` (the fold over every fixture, and the renders), which
 * `tests/data/golden/scene.hash` commits and `make check-scene-hash`,
 * `suite/tools/xarch/shape.sh` and `suite/tools/xwin/shape.sh` compare.
 *
 * Two switches, read from the environment so that one build serves: with
 * `FONT_SCENE_PLANT=1` the fold has one bit flipped (the gates' plant); with
 * `FONT_SCENE_NATIVE_ORDER=1` each number is folded in the host's byte order
 * (the control: the committed hash on a little-endian target, another on a
 * big-endian one).
 */

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/raster.h>
#include <ghoti.io/font/scene.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/shape-cpu/render.h>
#include <ghoti.io/shape-geometry/map.h>
#include <ghoti.io/shape-scene/glyph.h>
#include <ghoti.io/shape-scene/list.h>
#include <ghoti.io/shape-scene/report.h>
#include <ghoti.io/shape-scene/resource.h>
#include <ghoti.io/shape-scene/sink.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char * const kFixtures[] = {
  "basic.ttf", "outline-composite.ttf", "outline-simple.ttf", "cff-curves.otf", "cff-seac.otf",
  "type1-big.pfb", "variable-gvar.ttf",
  // Bearings that are not the glyphs' xMin, simple and composite: where a glyph's origin is,
  // on every target.
  "outline-placement.ttf",
};
static const uint32_t kPpems[] = {8, 12, 16, 21, 33, 48, 72};
static const uint32_t kFractions[5] = {0, 1, 16, 33, 63};
#define kMaxGlyphs 24u
#define kAd1Glyphs 8u

static uint64_t g_hash = 0xCBF29CE484222325ull;
static uint64_t g_fixture = 0xCBF29CE484222325ull;
static unsigned long g_count = 0;
static int g_native = 0;

static void fold_byte(uint8_t b) {
  g_hash = (g_hash ^ b) * 0x00000100000001B3ull;
  g_fixture = (g_fixture ^ b) * 0x00000100000001B3ull;
}

static void fold_bytes(const uint8_t * p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    fold_byte(p[i]);
  }
}

/** A number, least significant byte first, or (the control) in the host's order. */
static void fold_u64(uint64_t v) {
  if (g_native) {
    uint8_t b[8];
    memcpy(b, &v, sizeof b);
    fold_bytes(b, sizeof b);
    return;
  }
  for (int i = 0; i < 8; i++) {
    fold_byte((uint8_t)(v >> (8 * i)));
  }
}

static uint8_t * read_file(const char * path, size_t * size) {
  FILE * f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  size_t cap = 1u << 16;
  size_t n = 0;
  uint8_t * buf = malloc(cap);
  while (buf) {
    size_t got = fread(buf + n, 1, cap - n, f);
    n += got;
    if (n < cap) {
      break;
    }
    uint8_t * grown = realloc(buf, cap * 2u);
    if (!grown) {
      free(buf);
      buf = NULL;
      break;
    }
    buf = grown;
    cap *= 2u;
  }
  fclose(f);
  *size = n;
  return buf;
}

/** A render's pixels (RGBA8 straight, from transparent), rows without the stride; NULL on failure. */
static uint8_t * render(const GSHSCENE_List * list, uint32_t w, uint32_t h) {
  GIMG_Pixel_Format fmt;
  memset(&fmt, 0, sizeof fmt);
  fmt.channel_model = GIMG_CHANNEL_RGBA;
  fmt.layout = GIMG_LAYOUT_INTERLEAVED;
  fmt.channel_count = 4;
  fmt.channel_type = GIMG_CHANNEL_UNORM;
  for (int i = 0; i < 4; i++) {
    fmt.bits_per_channel[i] = 8;
  }
  fmt.alpha_form = GIMG_ALPHA_STRAIGHT;
  GIMG_Raster * raster = NULL;
  GSHSCENE_Sink * sink = NULL;
  GSHSCENE_Report * report = NULL;
  uint8_t * out = malloc((size_t)w * h * 4u);
  int ok = out && gimg_raster_create(w, h, &fmt, GIMG_RASTER_OWNED, NULL, 0, &raster) == GIMG_OK
      && gshcpu_sink_create(NULL, NULL, NULL, &sink) == GSHCPU_OK
      && gshscene_report_create(NULL, NULL, &report) == GSHSCENE_OK;
  if (ok) {
    uint8_t * px = gimg_raster_pixels(raster);
    size_t stride = gimg_raster_stride_bytes(raster);
    for (uint32_t y = 0; y < h; y++) {
      memset(px + y * stride, 0, (size_t)w * 4u);
    }
    GSHSCENE_Target t;
    memset(&t, 0, sizeof t);
    t.kind = GSHSCENE_TARGET_RASTER;
    t.surface = raster;
    GSHSCENE_Render_Request rq;
    memset(&rq, 0, sizeof rq);
    rq.list = list;
    rq.target = &t;
    rq.report = report;
    ok = gshscene_sink_render(sink, &rq) == GSHSCENE_OK;
    for (uint32_t y = 0; ok && y < h; y++) {
      memcpy(out + (size_t)y * w * 4u, px + y * stride, (size_t)w * 4u);
    }
  }
  gshscene_report_destroy(report);
  if (sink) {
    gshscene_sink_destroy(sink);
  }
  gimg_raster_destroy(raster);
  if (!ok) {
    free(out);
    return NULL;
  }
  return out;
}

static GSHSCENE_List * fill(GSHSCENE_Store * store, const GSHSCENE_Resource * run, const GSHGEOM_Map * map) {
  GSHSCENE_List * l = NULL;
  GSHSCENE_Paint ink;
  memset(&ink, 0, sizeof ink);
  ink.kind = GSHSCENE_PAINT_SOLID;
  ink.rgba[0] = ink.rgba[1] = ink.rgba[2] = ink.rgba[3] = 1.0;
  if (gshscene_list_create(NULL, NULL, store, &l) != GSHSCENE_OK
      || gshscene_list_fill(l, run, map, GSHGEOM_FILL_NONZERO, &ink, 0, 0.0) != GSHSCENE_OK
      || gshscene_list_close(l) != GSHSCENE_OK) {
    gshscene_list_destroy(l);
    return NULL;
  }
  return l;
}

static const GSHSCENE_Resource * run_of(GSHSCENE_Store * store, const GSHSCENE_Resource * face, double size,
    uint32_t mode, size_t n, const GSHSCENE_Glyph * glyphs) {
  GSHSCENE_Glyph_Run r;
  memset(&r, 0, sizeof r);
  r.source = GSHSCENE_GLYPHS_FROM_FONT;
  r.face = face;
  r.size = size;
  r.hint_mode = mode;
  r.glyph_count = n;
  r.glyphs = glyphs;
  const GSHSCENE_Resource * out = NULL;
  return gshscene_store_intern_glyph_run(store, &r, &out) == GSHSCENE_OK ? out : NULL;
}

/** AD-5 at one size: fast against general under the snapped map; folds the fast raster. 0 on success. */
static int ad5(const char * name, GFNT_SceneFont * font, uint16_t upem, size_t n, uint32_t ppem) {
  GSHSCENE_Store * store = NULL;
  const GSHSCENE_Resource * face = NULL;
  const GSHSCENE_Resource * fast = NULL;
  const GSHSCENE_Resource * general = NULL;
  GSHSCENE_List * fl = NULL;
  GSHSCENE_List * gl = NULL;
  uint8_t * a = NULL;
  uint8_t * b = NULL;
  GSHSCENE_Glyph fg[kMaxGlyphs];
  GSHSCENE_Glyph sg[kMaxGlyphs];
  const uint32_t cell = 2u * ppem + 8u;
  const uint32_t cols = 8u;
  const uint32_t rows = (uint32_t)((n + cols - 1u) / cols);
  int rc = 1;
  int reported = 0;
  if (gshscene_store_create(NULL, NULL, &store) != GSHSCENE_OK
      || gfnt_scene_intern_face(font, store, NULL, &face, NULL) != GFNT_OK) {
    goto done;
  }
  for (size_t i = 0; i < n; i++) {
    size_t pair = (i + ppem) % 25u;
    double x = (double)((i % cols) * cell) + ppem / 2.0 + kFractions[pair % 5u] / 64.0;
    double y = (double)((i / cols) * cell) + 1.5 * ppem + kFractions[pair / 5u] / 64.0;
    fg[i].id = (uint32_t)i;
    fg[i].x = 2.0 * (x - 1.0);
    fg[i].y = 2.0 * y;
    fg[i].outline = NULL;
  }
  GSHGEOM_Map half;
  GSHGEOM_Map map;
  gshgeom_map_scale(2, 0.5, 0.5, 1.0, &half);
  gshgeom_map_translate(2, 1.0, 0.0, 0.0, &map);
  gshgeom_map_compose(&map, &half, &map);
  fast = run_of(store, face, 2.0 * ppem, GSHSCENE_HINT_NONE, n, fg);
  GSHSCENE_Glyph_Route route;
  if (!fast || gshscene_glyph_route(fast, &map, GSHSCENE_TARGET_RASTER, &route) != GSHSCENE_OK
      || route.route != GSHSCENE_GLYPH_FAST || route.ppem != ppem) {
    goto done;
  }
  for (size_t i = 0; i < n; i++) {
    GSHSCENE_Glyph_Origin o;
    if (gshscene_glyph_origin(fast, &map, i, &o) != GSHSCENE_OK) {
      goto done;
    }
    sg[i].id = (uint32_t)i;
    sg[i].x = o.x / 64.0;
    sg[i].y = o.y / 64.0;
    sg[i].outline = NULL;
  }
  general = run_of(store, face, (double)route.scale * upem / 65536.0, GSHSCENE_HINT_NONE, n, sg);
  GSHGEOM_Map q1;
  GSHGEOM_Map q2;
  gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, 90.0, &q1);
  gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, -90.0, &q2);
  gshgeom_map_compose(&q1, &q2, &q1);
  if (!general || !(fl = fill(store, fast, &map)) || !(gl = fill(store, general, &q1))
      || !(a = render(fl, cols * cell, rows * cell)) || !(b = render(gl, cols * cell, rows * cell))) {
    goto done;
  }
  if (memcmp(a, b, (size_t)cols * cell * rows * cell * 4u) != 0) {
    printf("font-scene.FAIL ad5 %s %u\n", name, ppem);
    reported = 1;
    goto done;
  }
  fold_u64(ppem);
  fold_bytes(a, (size_t)cols * cell * rows * cell * 4u);
  g_count++;
  rc = 0;
done:
  if (rc && !reported) {
    printf("font-scene.FAIL ad5-setup %s %u\n", name, ppem);
  }
  free(a);
  free(b);
  gshscene_list_destroy(fl);
  gshscene_list_destroy(gl);
  gshscene_resource_release(general);
  gshscene_resource_release(fast);
  gshscene_resource_release(face);
  gshscene_store_destroy(store);
  return rc;
}

/** AD-1 for one glyph: the direct call's coverage against a one-glyph scene's alpha; folds the coverage. */
static int ad1(const char * name, const GFNT_Face * gface, GFNT_SceneFont * font, uint32_t glyph, uint32_t ppem,
    uint32_t mode, uint32_t fx, uint32_t fy) {
  GFNT_RasterOptions options;
  memset(&options, 0, sizeof options);
  options.hint_mode = (GFNT_HintMode)mode;
  options.origin_x = (GFNT_F26Dot6)fx;
  options.origin_y = -(GFNT_F26Dot6)fy;
  GFNT_Coverage coverage;
  memset(&coverage, 0, sizeof coverage);
  GFNT_Result direct = gfnt_face_render_glyph(gface, glyph, ppem, &options, NULL, &coverage, NULL);
  const uint32_t w = 3u * ppem + 16u;
  const uint32_t h = 3u * ppem + 16u;
  const int64_t px = (int64_t)ppem + 8;
  const int64_t py = 2 * (int64_t)ppem + 8;
  GSHSCENE_Store * store = NULL;
  const GSHSCENE_Resource * face = NULL;
  const GSHSCENE_Resource * run = NULL;
  GSHSCENE_List * l = NULL;
  uint8_t * pix = NULL;
  int rc = 1;
  GSHSCENE_Glyph g = {glyph, (double)px + fx / 64.0, (double)py + fy / 64.0, NULL};
  if (gshscene_store_create(NULL, NULL, &store) != GSHSCENE_OK
      || gfnt_scene_intern_face(font, store, NULL, &face, NULL) != GFNT_OK
      || !(run = run_of(store, face, ppem, mode, 1, &g)) || !(l = fill(store, run, NULL))
      || !(pix = render(l, w, h))) {
    printf("font-scene.FAIL ad1-setup %s %u\n", name, glyph);
    goto done;
  }
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      int64_t col = (int64_t)x - px - coverage.left;
      int64_t row = (int64_t)y - py + coverage.top;
      uint8_t want = 0;
      if (direct == GFNT_OK && col >= 0 && row >= 0 && col < coverage.width && row < coverage.height) {
        want = gfnt_coverage_at(&coverage, (uint32_t)col, (uint32_t)row);
      }
      if (pix[((size_t)y * w + x) * 4u + 3u] != want) {
        printf("font-scene.FAIL ad1 %s glyph %u ppem %u mode %u\n", name, glyph, ppem, mode);
        goto done;
      }
    }
  }
  fold_u64(direct == GFNT_OK ? gfnt_coverage_hash(&coverage) : 0u);
  g_count++;
  rc = 0;
done:
  gfnt_coverage_destroy(&coverage);
  free(pix);
  gshscene_list_destroy(l);
  gshscene_resource_release(run);
  gshscene_resource_release(face);
  gshscene_store_destroy(store);
  return rc;
}

int main(int argc, char ** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: scene-hash FONTS_DIR\n");
    return 2;
  }
  const char * plant = getenv("FONT_SCENE_PLANT");
  const char * native = getenv("FONT_SCENE_NATIVE_ORDER");
  g_native = native && native[0] == '1';
  int failed = 0;
  for (size_t f = 0; f < sizeof kFixtures / sizeof kFixtures[0]; f++) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", argv[1], kFixtures[f]);
    size_t size = 0;
    uint8_t * bytes = read_file(path, &size);
    GFNT_Blob * blob = NULL;
    GFNT_Face * face = NULL;
    GFNT_SceneFont * font = NULL;
    uint16_t upem = 0;
    size_t glyphs = 0;
    if (!bytes || gfnt_blob_create_memory(bytes, size, GFNT_BLOB_BORROWED, NULL, NULL, &blob, NULL) != GFNT_OK
        || gfnt_face_load(blob, 0, NULL, NULL, &face, NULL) != GFNT_OK
        || gfnt_scene_font_create(face, &font, NULL) != GFNT_OK
        || gfnt_face_units_per_em(face, &upem, NULL) != GFNT_OK
        || gfnt_face_num_glyphs(face, &glyphs, NULL) != GFNT_OK || glyphs == 0) {
      printf("font-scene.FAIL load %s\n", kFixtures[f]);
      failed = 1;
    } else {
      g_fixture = 0xCBF29CE484222325ull;
      size_t n = glyphs < kMaxGlyphs ? glyphs : kMaxGlyphs;
      for (size_t p = 0; p < sizeof kPpems / sizeof kPpems[0]; p++) {
        failed |= ad5(kFixtures[f], font, upem, n, kPpems[p]);
      }
      size_t m = glyphs < kAd1Glyphs ? glyphs : kAd1Glyphs;
      for (uint32_t mode = 0; mode < 2; mode++) {
        for (uint32_t ppem = 13; ppem <= 31; ppem += 18) {
          for (size_t g = 0; g < m; g++) {
            failed |= ad1(kFixtures[f], face, font, (uint32_t)g, ppem, mode == 0 ? GSHSCENE_HINT_NONE
                : GSHSCENE_HINT_FONT, kFractions[(g + ppem) % 5u], kFractions[(g + 2u * ppem) % 5u]);
          }
        }
      }
      printf("font-scene.fixture %s %016llx\n", kFixtures[f], (unsigned long long)g_fixture);
    }
    gfnt_scene_font_destroy(font);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    free(bytes);
  }
  if (plant && plant[0] == '1') {
    g_hash ^= 1u;
  }
  printf("font-scene.hash %016llx\n", (unsigned long long)g_hash);
  printf("font-scene.count %lu\n", g_count);
  return failed ? 1 : 0;
}
