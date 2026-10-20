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
 * Text from a real font in a retained scene, rendered by two sinks (the shape
 * family's first milestone, CAP-11 and its success signal).
 *
 * The application records one scene: a star filled under both fill rules, a
 * group of opacity 0.6 clipped to a circle, a glyph on the hinted fast tier
 * (an axis-aligned map at a whole pixel size) and a glyph turned by 30 degrees,
 * which takes the general route.  It publishes it and renders the version with
 * `shape-cpu` and with the test sink, on a raster and as a writer's stream in
 * millimetres.  A driver, given an edit, turns the second glyph further and
 * returns the edit unpublished; the application publishes it, diffs the two
 * versions and redraws only the damage, which must equal a whole render.
 *
 * Every hash is in the family's one spelling (FNV-1a 64 over little-endian
 * fields), so the output is the same on every target; `font-scene.hash` folds
 * them.  The program exits 1 if a check fails: both routes taken, the
 * damage-limited frame equal to the whole render.
 *
 *     font-scene FONT [TEXT [OUT.pam]]
 *
 * TEXT is two characters, the hinted one and the turned one ("AB" by default).
 * OUT.pam, if given, receives the last frame (a PAM image, RGBA).
 */

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/scene.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/shape-cpu/shape-cpu.h>
#include <ghoti.io/shape-scene/shape-scene.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define WIDTH 160
#define HEIGHT 96

static int g_failed = 0;
static uint64_t g_fold = 0xCBF29CE484222325ull;

/** Print one named hash and fold it in (little-endian bytes, the family's spelling). */
static void report_hash(const char * name, uint64_t h) {
  printf("font-scene.%s %016" PRIx64 "\n", name, h);
  for (int i = 0; i < 8; i++) {
    g_fold = (g_fold ^ ((h >> (8 * i)) & 0xFF)) * 0x100000001B3ull;
  }
}

static int check(int ok, const char * what) {
  if (!ok) {
    fprintf(stderr, "font-scene: %s\n", what);
    g_failed = 1;
  }
  return ok;
}

/* --- paths ------------------------------------------------------------------------------------------------- */

static const GSHSCENE_Resource * intern(GSHSCENE_Store * store, GSHGEOM_Path * path) {
  const GSHSCENE_Resource * r = NULL;
  check(gshscene_store_intern_path(store, path, &r) == GSHSCENE_OK, "intern a path");
  gshgeom_path_destroy(path);
  return r;
}

/** A five-pointed star drawn in one stroke, so that its middle is wound twice: filled under nonzero, a hole under even-odd. */
static const GSHSCENE_Resource * star(GSHSCENE_Store * store) {
  static const double xy[5][2] = {{0.0, -20.0}, {11.755705045849463, 16.180339887498949},
      {-19.021130325903071, -6.180339887498949}, {19.021130325903071, -6.180339887498949},
      {-11.755705045849463, 16.180339887498949}};
  GSHGEOM_Path * path = NULL;
  gshgeom_path_create(NULL, NULL, 2, &path);
  gshgeom_path_move_to(path, (GSHGEOM_Point){xy[0][0], xy[0][1], 0});
  for (int i = 1; i < 5; i++) {
    gshgeom_path_line_to(path, (GSHGEOM_Point){xy[i][0], xy[i][1], 0});
  }
  gshgeom_path_close(path);
  return intern(store, path);
}

static const GSHSCENE_Resource * circle(GSHSCENE_Store * store, double r) {
  static const double w[3] = {1.0, 0.70710678118654752440, 1.0}; /* four exact quarter arcs */
  GSHGEOM_Path * path = NULL;
  gshgeom_path_create(NULL, NULL, 2, &path);
  gshgeom_path_move_to(path, (GSHGEOM_Point){r, 0, 0});
  gshgeom_path_rational_quadratic_to(path, (GSHGEOM_Point){r, r, 0}, (GSHGEOM_Point){0, r, 0}, w);
  gshgeom_path_rational_quadratic_to(path, (GSHGEOM_Point){-r, r, 0}, (GSHGEOM_Point){-r, 0, 0}, w);
  gshgeom_path_rational_quadratic_to(path, (GSHGEOM_Point){-r, -r, 0}, (GSHGEOM_Point){0, -r, 0}, w);
  gshgeom_path_rational_quadratic_to(path, (GSHGEOM_Point){r, -r, 0}, (GSHGEOM_Point){r, 0, 0}, w);
  gshgeom_path_close(path);
  return intern(store, path);
}

static const GSHSCENE_Resource * rect(GSHSCENE_Store * store, double x0, double y0, double x1, double y1) {
  GSHGEOM_Path * path = NULL;
  gshgeom_path_create(NULL, NULL, 2, &path);
  gshgeom_path_move_to(path, (GSHGEOM_Point){x0, y0, 0});
  gshgeom_path_line_to(path, (GSHGEOM_Point){x1, y0, 0});
  gshgeom_path_line_to(path, (GSHGEOM_Point){x1, y1, 0});
  gshgeom_path_line_to(path, (GSHGEOM_Point){x0, y1, 0});
  gshgeom_path_close(path);
  return intern(store, path);
}

/** One glyph of the face at 24 model units (24 px under the identity: a whole ppem the fast tier takes). */
static const GSHSCENE_Resource * glyph(GSHSCENE_Store * store, const GSHSCENE_Resource * face, uint32_t id) {
  GSHSCENE_Glyph g = {id, 0.0, 0.0, NULL};
  GSHSCENE_Glyph_Run run;
  memset(&run, 0, sizeof run);
  run.source = GSHSCENE_GLYPHS_FROM_FONT;
  run.face = face;
  run.size = 24.0;
  run.hint_mode = GSHSCENE_HINT_FONT;
  run.glyph_count = 1;
  run.glyphs = &g;
  const GSHSCENE_Resource * r = NULL;
  check(gshscene_store_intern_glyph_run(store, &run, &r) == GSHSCENE_OK, "intern a glyph run");
  return r;
}

/* --- the scene --------------------------------------------------------------------------------------------- */

static GSHSCENE_Node shape(const GSHSCENE_Resource * resource, GSHGEOM_Fill_Rule rule, double r, double g, double b) {
  GSHSCENE_Node n;
  gshscene_node_init(GSHSCENE_NODE_SHAPE, &n);
  n.resource = resource;
  n.fill_rule = rule;
  n.paint.kind = GSHSCENE_PAINT_SOLID;
  n.paint.rgba[0] = r;
  n.paint.rgba[1] = g;
  n.paint.rgba[2] = b;
  n.paint.rgba[3] = 1.0;
  return n;
}

/** The turned glyph's placement: about its origin, then to (100, 70). */
static void turned(double degrees, GSHGEOM_Map * out) {
  GSHGEOM_Map turn, move;
  gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, degrees, &turn);
  gshgeom_map_translate(2, 100.0, 70.0, 0.0, &move);
  gshgeom_map_compose(&move, &turn, out);
}

/* --- the driver: given an edit, it turns a node; it cannot publish ------------------------------------------ */

static int drive(GSHSCENE_Edit * edit, uint64_t node, double degrees) {
  GSHSCENE_Node_Info info;
  if (gshscene_edit_node(edit, node, &info) != GSHSCENE_OK) {
    return 0;
  }
  turned(degrees, &info.node.map);
  return gshscene_edit_set(edit, node, &info.node) == GSHSCENE_OK;
}

/* --- rendering --------------------------------------------------------------------------------------------- */

typedef struct Stream {
  uint64_t hash;
  size_t bytes;
} Stream;

static GSHSCENE_Result to_hash(void * ctx, const void * bytes, size_t count) {
  Stream * s = ctx;
  for (size_t i = 0; i < count; i++) {
    s->hash = (s->hash ^ ((const unsigned char *)bytes)[i]) * 0x100000001B3ull;
  }
  s->bytes += count;
  return GSHSCENE_OK;
}

static GSHSCENE_Result render(GSHSCENE_Sink * sink, const GSHSCENE_List * list, const GSHSCENE_Target * target,
    GSHSCENE_Report * report) {
  GSHSCENE_Render_Request request;
  memset(&request, 0, sizeof request);
  request.list = list;
  request.target = target;
  request.report = report;
  return gshscene_sink_render(sink, &request);
}

static GSHSCENE_Target raster_target(GIMG_Raster * raster, const GSHSCENE_Rect * damage) {
  GSHSCENE_Target t;
  memset(&t, 0, sizeof t);
  t.kind = GSHSCENE_TARGET_RASTER;
  t.surface = raster;
  t.damage = damage;
  return t;
}

static uint64_t raster_hash(const GIMG_Raster * raster) {
  GSHSCENE_Rect all = {0, 0, WIDTH, HEIGHT};
  uint64_t h = 0;
  check(gshcpu_render_hash(raster, &all, &h) == GSHCPU_OK, "hash a raster");
  return h;
}

/** Print the report's entries, each named by the node it came from (the version's IDs). */
static void print_entries(const GSHSCENE_Report * report, const GSHSCENE_Version * version) {
  for (size_t i = 0; i < gshscene_report_count(report); i++) {
    GSHSCENE_Report_Entry e;
    uint64_t node = 0, path[8];
    size_t depth = 0;
    gshscene_report_entry(report, i, &e);
    gshscene_report_resolve(report, version, i, &node, path, 8, &depth);
    printf("font-scene.entry %s %s node %" PRIu64 "\n", gshscene_entry_class_string(e.entry_class),
        gshscene_reason_string(e.reason), node);
  }
}

static void write_pam(const char * file, const GIMG_Raster * raster) {
  FILE * out = fopen(file, "wb");
  if (!check(out != NULL, "open the image file")) {
    return;
  }
  fprintf(out, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", WIDTH, HEIGHT);
  const unsigned char * px = gimg_raster_pixels_const(raster);
  for (int y = 0; y < HEIGHT; y++) {
    fwrite(px + (size_t)y * gimg_raster_stride_bytes(raster), 4, WIDTH, out);
  }
  check(fclose(out) == 0, "write the image file");
}

int main(int argc, char ** argv) {
  if (argc < 2 || argc > 4 || (argc >= 3 && strlen(argv[2]) != 2)) {
    fprintf(stderr, "usage: font-scene FONT [TEXT [OUT.pam]]   (TEXT: two characters, default AB)\n");
    return 2;
  }
  const char * text = argc >= 3 ? argv[2] : "AB";
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_SceneFont * font = NULL;
  GFNT_Error error;
  gfnt_error_clear(&error);
  uint32_t ids[2] = {0, 0};
  if (gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error) != GFNT_OK ||
      gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK ||
      gfnt_face_glyph_for_codepoint(face, (unsigned char)text[0], &ids[0], &error) != GFNT_OK ||
      gfnt_face_glyph_for_codepoint(face, (unsigned char)text[1], &ids[1], &error) != GFNT_OK ||
      gfnt_scene_font_create(face, &font, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }

  /* The scene, and the face as a resource of its store, bound to this library's glyph path provider. */
  GSHSCENE_Scene * scene = NULL;
  const GSHSCENE_Resource * face_resource = NULL;
  if (gshscene_scene_create(NULL, NULL, NULL, &scene) != GSHSCENE_OK ||
      gfnt_scene_intern_face(font, gshscene_scene_store(scene), NULL, &face_resource, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  GSHSCENE_Store * store = gshscene_scene_store(scene);
  const GSHSCENE_Resource * res[7] = {rect(store, 0, 0, WIDTH, HEIGHT), star(store), circle(store, 20.0),
      rect(store, -24, -24, 24, 24), glyph(store, face_resource, ids[0]), glyph(store, face_resource, ids[1]), NULL};

  /* Version 1, recorded by the application. */
  GSHSCENE_Edit * edit = NULL;
  uint64_t turned_id = 0;
  check(gshscene_scene_open_edit(scene, NULL, &edit) == GSHSCENE_OK, "open an edit");
  GSHSCENE_Node n = shape(res[0], GSHGEOM_FILL_NONZERO, 1.0, 1.0, 1.0);
  check(gshscene_edit_add(edit, GSHSCENE_ROOT_ID, GSHSCENE_APPEND, &n, NULL, NULL) == GSHSCENE_OK, "background");
  n = shape(res[1], GSHGEOM_FILL_NONZERO, 0.85, 0.2, 0.1);
  gshgeom_map_translate(2, 28.0, 30.0, 0.0, &n.map);
  check(gshscene_edit_add(edit, GSHSCENE_ROOT_ID, GSHSCENE_APPEND, &n, NULL, NULL) == GSHSCENE_OK, "star, nonzero");
  n.fill_rule = GSHGEOM_FILL_EVEN_ODD;
  gshgeom_map_translate(2, 76.0, 30.0, 0.0, &n.map);
  check(gshscene_edit_add(edit, GSHSCENE_ROOT_ID, GSHSCENE_APPEND, &n, NULL, NULL) == GSHSCENE_OK, "star, even-odd");
  GSHSCENE_Node group;
  gshscene_node_init(GSHSCENE_NODE_GROUP, &group);
  group.opacity = 0.6;
  group.clip = res[2];
  gshgeom_map_translate(2, 124.0, 30.0, 0.0, &group.map);
  uint64_t group_id = 0;
  check(gshscene_edit_add(edit, GSHSCENE_ROOT_ID, GSHSCENE_APPEND, &group, NULL, &group_id) == GSHSCENE_OK, "group");
  n = shape(res[3], GSHGEOM_FILL_NONZERO, 0.1, 0.3, 0.9);
  check(gshscene_edit_add(edit, group_id, GSHSCENE_APPEND, &n, NULL, NULL) == GSHSCENE_OK, "square in the group");
  n = shape(res[4], GSHGEOM_FILL_NONZERO, 0.0, 0.0, 0.0);
  gshgeom_map_translate(2, 20.0, 84.0, 0.0, &n.map); /* whole pixels: the fast tier, hinted where the font hints */
  check(gshscene_edit_add(edit, GSHSCENE_ROOT_ID, GSHSCENE_APPEND, &n, NULL, NULL) == GSHSCENE_OK, "hinted glyph");
  n = shape(res[5], GSHGEOM_FILL_NONZERO, 0.0, 0.45, 0.2);
  turned(30.0, &n.map); /* a rotation: the general route */
  check(gshscene_edit_add(edit, GSHSCENE_ROOT_ID, GSHSCENE_APPEND, &n, NULL, &turned_id) == GSHSCENE_OK, "turned glyph");
  const GSHSCENE_Version * v1 = NULL;
  check(gshscene_scene_publish(scene, edit, NULL, &v1) == GSHSCENE_OK, "publish version 1");
  gshscene_edit_destroy(edit);
  edit = NULL;
  if (g_failed) {
    return 1;
  }
  const GSHSCENE_List * list1 = NULL;
  check(gshscene_version_list(v1, &list1) == GSHSCENE_OK, "the version's list");
  report_hash("version1", gshscene_version_hash(v1));
  report_hash("list1", gshscene_list_hash(list1));

  /* The route each glyph takes, decided by its map (AD-5). */
  for (int i = 0; i < 2; i++) {
    GSHGEOM_Map map;
    GSHSCENE_Glyph_Route route;
    if (i == 0) {
      gshgeom_map_translate(2, 20.0, 84.0, 0.0, &map);
    } else {
      turned(30.0, &map);
    }
    if (check(gshscene_glyph_route(res[4 + i], &map, GSHSCENE_TARGET_RASTER, &route) == GSHSCENE_OK, "route")) {
      printf("font-scene.route %c %s ppem %" PRIu32 "\n", text[i], route.route == GSHSCENE_GLYPH_GENERAL ? "general" : "fast",
          route.ppem);
      check((route.route == GSHSCENE_GLYPH_GENERAL) == (i == 1), "the hinted glyph is on the fast tier, the turned one general");
    }
  }

  /* Two sinks behind one interface: shape-cpu, and the test sink on a raster and on a writer's stream. */
  GSHSCENE_Sink * cpu = NULL;
  GSHSCENE_Sink * test = NULL;
  GSHSCENE_Report * report = NULL;
  GIMG_Raster * screen = NULL;
  GIMG_Raster * whole = NULL;
  GIMG_Raster * stamps = NULL;
  GIMG_Pixel_Format format = GIMG_PIXEL_RGBA8;
  if (!check(gshcpu_sink_create(NULL, NULL, NULL, &cpu) == GSHCPU_OK &&
                 gshscene_dump_sink_create(NULL, NULL, NULL, &test) == GSHSCENE_OK &&
                 gshscene_report_create(NULL, NULL, &report) == GSHSCENE_OK &&
                 gimg_raster_create(WIDTH, HEIGHT, &format, GIMG_RASTER_OWNED, NULL, 0, &screen) == GIMG_OK &&
                 gimg_raster_create(WIDTH, HEIGHT, &format, GIMG_RASTER_OWNED, NULL, 0, &whole) == GIMG_OK &&
                 gimg_raster_create(WIDTH, HEIGHT, &format, GIMG_RASTER_OWNED, NULL, 0, &stamps) == GIMG_OK,
          "make the sinks and rasters")) {
    return 1;
  }
  GFNT_SceneStats before, after;
  gfnt_scene_font_stats(font, &before);
  GSHSCENE_Target t = raster_target(screen, NULL);
  check(render(cpu, list1, &t, report) == GSHSCENE_OK, "shape-cpu renders version 1");
  gfnt_scene_font_stats(font, &after);
  check(after.fast_calls > before.fast_calls && after.general_calls > before.general_calls,
      "shape-cpu asked the font for both routes");
  report_hash("cpu1", raster_hash(screen));
  report_hash("report1", gshscene_report_hash(report));
  print_entries(report, v1);

  t = raster_target(stamps, NULL);
  check(render(test, list1, &t, report) == GSHSCENE_OK, "the test sink renders version 1");
  report_hash("test-sink-raster1", raster_hash(stamps));
  Stream stream = {0xCBF29CE484222325ull, 0};
  GSHSCENE_Writer writer = {to_hash, &stream};
  GSHSCENE_Target page;
  memset(&page, 0, sizeof page);
  page.kind = GSHSCENE_TARGET_STREAM;
  page.surface = &writer;
  page.unit = GSHSCENE_UNIT_MILLIMETER;
  gshgeom_map_scale(2, 0.25, 0.25, 1.0, &page.map); /* 4 model units to the millimetre */
  check(render(test, list1, &page, report) == GSHSCENE_OK && stream.bytes > 0, "the test sink writes version 1");
  report_hash("test-sink-stream1", stream.hash);

  /* The driver turns the second glyph in an edit it is given; the application publishes and redraws the damage. */
  check(gshscene_scene_open_edit(scene, v1, &edit) == GSHSCENE_OK && drive(edit, turned_id, 45.0), "the driver");
  const GSHSCENE_Version * v2 = NULL;
  check(gshscene_scene_publish(scene, edit, NULL, &v2) == GSHSCENE_OK, "publish version 2");
  gshscene_edit_destroy(edit);
  GSHSCENE_Diff * diff = NULL;
  GSHSCENE_Damage damage;
  memset(&damage, 0, sizeof damage);
  check(gshscene_diff(v1, v2, NULL, NULL, &diff) == GSHSCENE_OK && gshscene_diff_damage(diff, &damage) == GSHSCENE_OK &&
          !damage.unbounded && !damage.empty,
      "a bounded damage");
  printf("font-scene.damage x %" PRId64 "..%" PRId64 " y %" PRId64 "..%" PRId64 "\n", damage.rect.x0, damage.rect.x1,
      damage.rect.y0, damage.rect.y1);
  const GSHSCENE_List * list2 = NULL;
  check(gshscene_version_list(v2, &list2) == GSHSCENE_OK, "version 2's list");
  t = raster_target(screen, &damage.rect);
  check(render(cpu, list2, &t, report) == GSHSCENE_OK, "the damage-limited frame");
  t = raster_target(whole, NULL);
  check(render(cpu, list2, &t, report) == GSHSCENE_OK, "the whole frame");
  const uint64_t h_screen = raster_hash(screen);
  check(h_screen == raster_hash(whole), "the damage-limited frame equals the whole render");
  report_hash("cpu2", h_screen);
  printf("font-scene.hash %016" PRIx64 "\n", g_fold);
  if (argc == 4) {
    write_pam(argv[3], screen);
  }

  gshscene_diff_destroy(diff);
  gimg_raster_destroy(stamps);
  gimg_raster_destroy(whole);
  gimg_raster_destroy(screen);
  gshscene_report_destroy(report);
  gshscene_sink_destroy(test);
  gshscene_sink_destroy(cpu);
  for (int i = 0; res[i]; i++) {
    gshscene_resource_release(res[i]);
  }
  gshscene_version_release(v2);
  gshscene_version_release(v1);
  gshscene_resource_release(face_resource);
  gshscene_scene_destroy(scene);
  gfnt_scene_font_destroy(font);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return g_failed;
}
