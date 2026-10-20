/**
 * @file
 *
 * Color glyphs lowered into a scene (story 14c of the shape family's first
 * milestone; scene_color.h, design.md section 8.8): a `CPAL` palette as a
 * palette resource, each `COLR` version 0 and version 1 fixture glyph's lowering
 * item by item, every refusal with its reason and an unchanged list, and the
 * lowered lists rendered through `shape-cpu` as their solid twins render.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/font/color.h>
#include <ghoti.io/font/scene.h>
#include <ghoti.io/font/scene_color.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/shape-cpu/render.h>
#include <ghoti.io/shape-geometry/map.h>
#include <ghoti.io/shape-scene/list.h>
#include <ghoti.io/shape-scene/report.h>
#include <ghoti.io/shape-scene/resource.h>
#include <ghoti.io/shape-scene/sink.h>

namespace {

using Bytes = std::vector<uint8_t>;

// The color fixtures' glyphs (tools/fixtures/color_fixtures.py: ORDER, then SCENE_EXTRA).
const uint32_t kSpace = 1, kA = 2, kB = 3, kAlt = 4, kLayerA = 5, kLayerB = 6, kLayerC = 7;
const uint32_t kPaintD = 8, kPaintE = 9, kPaintF = 10, kPaintG = 11, kPaintH = 12, kPaintI = 13, kPaintJ = 14;

Bytes file_bytes(const std::string & name) {
  const std::string path = gfnttest::data("fonts/" + name);
  Bytes out;
  FILE * f = std::fopen(path.c_str(), "rb");
  EXPECT_NE(f, nullptr) << path;
  if (!f) {
    return out;
  }
  int c;
  while ((c = std::fgetc(f)) != EOF) {
    out.push_back(static_cast<uint8_t>(c));
  }
  std::fclose(f);
  return out;
}

/** A fixture loaded, bound to a store, with a face resource. */
struct Color {
  Bytes bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_SceneFont * scene = nullptr;
  GSHSCENE_Store * store = nullptr;
  const GSHSCENE_Resource * face_resource = nullptr;
  std::vector<GSHSCENE_List *> lists;
  std::vector<const GSHSCENE_Resource *> held;
  uint16_t upem = 0;

  explicit Color(const std::string & name) : Color(file_bytes(name)) {}
  explicit Color(Bytes b) : bytes(std::move(b)) {
    GFNT_Error error{};
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED, nullptr, nullptr, &blob,
        &error), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
    EXPECT_EQ(gfnt_scene_font_create(face, &scene, &error), GFNT_OK);
    gfnt_face_units_per_em(face, &upem, nullptr);
    EXPECT_EQ(gshscene_store_create(nullptr, nullptr, &store), GSHSCENE_OK);
    EXPECT_EQ(gfnt_scene_intern_face(scene, store, nullptr, &face_resource, &error), GFNT_OK);
  }
  ~Color() {
    for (GSHSCENE_List * l : lists) {
      gshscene_list_destroy(l);
    }
    for (auto it = held.rbegin(); it != held.rend(); ++it) {
      gshscene_resource_release(*it);
    }
    gshscene_resource_release(face_resource);
    gshscene_store_destroy(store);
    gfnt_scene_font_destroy(scene);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Color(const Color &) = delete;
  Color & operator=(const Color &) = delete;

  GSHSCENE_List * open() {
    GSHSCENE_List * l = nullptr;
    EXPECT_EQ(gshscene_list_create(nullptr, nullptr, store, &l), GSHSCENE_OK);
    lists.push_back(l);
    return l;
  }
  const GSHSCENE_Resource * palette(size_t index) {
    const GSHSCENE_Resource * p = nullptr;
    GFNT_Error error{};
    EXPECT_EQ(gfnt_scene_intern_palette(face_resource, store, index, &p, &error), GFNT_OK);
    if (p) {
      held.push_back(p);
    }
    return p;
  }
  GFNT_Result lower(GSHSCENE_List * l, uint32_t glyph, GFNT_ColorLowering * out, int32_t size = 20 * 64,
      int32_t x = 10 * 64, int32_t y = 30 * 64, int32_t layer = 3) {
    GFNT_ColorRun run{};
    run.size = size;
    run.x = x;
    run.y = y;
    run.bits = 6;
    run.hint_mode = GSHSCENE_HINT_NONE;
    run.layer = layer;
    GFNT_Error error{};
    return gfnt_scene_record_color_glyph(face_resource, store, l, glyph, &run, out, &error);
  }
};

const GSHSCENE_Item * item_of(const GSHSCENE_List * l, size_t i) {
  const GSHSCENE_Item * item = nullptr;
  EXPECT_EQ(gshscene_list_item(l, i, &item), GSHSCENE_OK);
  return item;
}

/** The glyph of an item's one-glyph run. */
uint32_t glyph_of(const GSHSCENE_Item * item) {
  GSHSCENE_Glyph_Run run{};
  EXPECT_EQ(gshscene_resource_glyph_run(item->resource, &run), GSHSCENE_OK);
  EXPECT_EQ(run.glyph_count, 1u);
  return run.glyph_count ? run.glyphs[0].id : 0xFFFFFFFFu;
}

void expect_fill(const GSHSCENE_List * l, size_t i, uint32_t glyph, GSHSCENE_Paint_Kind kind, uint32_t entry,
    double alpha, const char * what) {
  const GSHSCENE_Item * item = item_of(l, i);
  ASSERT_NE(item, nullptr) << what;
  EXPECT_EQ(item->kind, GSHSCENE_ITEM_FILL) << what;
  EXPECT_EQ(glyph_of(item), glyph) << what;
  EXPECT_EQ(item->paint.kind, kind) << what;
  EXPECT_EQ(item->paint.entry, entry) << what;
  EXPECT_EQ(item->paint.rgba[3], alpha) << what;
  EXPECT_EQ(item->fill_rule, GSHGEOM_FILL_NONZERO) << what;
  EXPECT_EQ(item->layer, 3) << what;
}

bool is_identity(const GSHGEOM_Map & m) {
  GSHGEOM_Map id;
  gshgeom_map_identity(2, &id);
  return gshgeom_map_hash(&m) == gshgeom_map_hash(&id);
}

/** Render a closed list on a w by h RGBA8 raster from transparent; the pixels and the report's reasons. */
std::string render(const GSHSCENE_List * list, uint32_t w, uint32_t h, std::vector<GSHSCENE_Reason> * reasons = nullptr) {
  GIMG_Pixel_Format f{};
  f.channel_model = GIMG_CHANNEL_RGBA;
  f.layout = GIMG_LAYOUT_INTERLEAVED;
  f.channel_count = 4;
  f.channel_type = GIMG_CHANNEL_UNORM;
  for (int i = 0; i < 4; i++) {
    f.bits_per_channel[i] = 8;
  }
  f.alpha_form = GIMG_ALPHA_STRAIGHT;
  GIMG_Raster * raster = nullptr;
  EXPECT_EQ(gimg_raster_create(w, h, &f, GIMG_RASTER_OWNED, nullptr, 0, &raster), GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  const size_t stride = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < h; y++) {
    std::memset(px + y * stride, 0, static_cast<size_t>(w) * 4u);
  }
  GSHSCENE_Sink * sink = nullptr;
  EXPECT_EQ(gshcpu_sink_create(nullptr, nullptr, nullptr, &sink), GSHCPU_OK);
  GSHSCENE_Report * report = nullptr;
  EXPECT_EQ(gshscene_report_create(nullptr, nullptr, &report), GSHSCENE_OK);
  GSHSCENE_Target t{};
  t.kind = GSHSCENE_TARGET_RASTER;
  t.surface = raster;
  GSHSCENE_Render_Request rq{};
  rq.list = list;
  rq.target = &t;
  rq.report = report;
  EXPECT_EQ(gshscene_sink_render(sink, &rq), GSHSCENE_OK);
  for (size_t i = 0; reasons && i < gshscene_report_count(report); i++) {
    GSHSCENE_Report_Entry e{};
    gshscene_report_entry(report, i, &e);
    reasons->push_back(e.reason);
  }
  std::string out;
  for (uint32_t y = 0; y < h; y++) {
    out.append(reinterpret_cast<const char *>(px + y * stride), static_cast<size_t>(w) * 4u);
  }
  gshscene_report_destroy(report);
  EXPECT_EQ(gshscene_sink_destroy(sink), GSHSCENE_OK);
  gimg_raster_destroy(raster);
  return out;
}

bool any_ink(const std::string & px) {
  for (size_t i = 3; i < px.size(); i += 4) {
    if (px[i] != 0) {
      return true;
    }
  }
  return false;
}

uint32_t be32(const Bytes & b, size_t at) {
  return (uint32_t)b[at] << 24 | (uint32_t)b[at + 1] << 16 | (uint32_t)b[at + 2] << 8 | b[at + 3];
}

/** @p font with its `COLR` table replaced by @p colr (every other table as it was). */
Bytes with_colr(const Bytes & font, const Bytes & colr) {
  std::vector<gfnttest::Table> tables;
  const size_t count = (size_t)font[4] << 8 | font[5];
  for (size_t i = 0; i < count; i++) {
    const size_t entry = 12 + 16 * i;
    gfnttest::Table t;
    t.tag = be32(font, entry);
    const size_t offset = be32(font, entry + 8);
    const size_t length = be32(font, entry + 12);
    t.data = t.tag == GFNT_TAG('C', 'O', 'L', 'R') ? colr : Bytes(font.begin() + (long)offset,
        font.begin() + (long)(offset + length));
    tables.push_back(t);
  }
  return gfnttest::build_sfnt(be32(font, 0), tables);
}

void put24(Bytes & out, uint32_t v) {
  out.push_back((uint8_t)(v >> 16));
  out.push_back((uint8_t)(v >> 8));
  out.push_back((uint8_t)v);
}

/**
 * A `COLR` version 1 table, built here: glyph @p no_record's graph is a `PaintColrGlyph` of @p target (which has no
 * record), and glyph @p deep's is @p levels nested `PaintColrLayers` of one layer each, the last a glyph over a
 * solid, so the graph is @p levels deep without a transform or a list push.
 */
Bytes colr_nested(uint16_t no_record, uint16_t deep, uint16_t target, size_t levels, bool v0_too = false) {
  Bytes paints;
  std::vector<uint32_t> layer_at;   // offsets of the layer paints within `paints`
  const uint32_t no_record_at = (uint32_t)paints.size();
  paints.push_back(11);                          // PaintColrGlyph
  gfnttest::put_u16(paints, target);
  const uint32_t deep_at = (uint32_t)paints.size();
  paints.push_back(1);                           // PaintColrLayers: one layer, the first
  paints.push_back(1);
  gfnttest::put_u32(paints, 0);
  for (size_t k = 0; k + 1 < levels; k++) {
    layer_at.push_back((uint32_t)paints.size());
    paints.push_back(1);
    paints.push_back(1);
    gfnttest::put_u32(paints, (uint32_t)(k + 1));
  }
  layer_at.push_back((uint32_t)paints.size());
  paints.push_back(10);                          // PaintGlyph over the solid that follows it
  put24(paints, 6);
  gfnttest::put_u16(paints, target);
  paints.push_back(2);                           // PaintSolid: entry 0, opaque
  gfnttest::put_u16(paints, 0);
  gfnttest::put_u16(paints, 16384);

  // With @p v0_too, glyph @p deep also has a version 0 record: one layer, glyph @p no_record in entry 2.
  const uint32_t header = 34;
  const uint32_t v0 = header;                    // BaseGlyphRecord, then LayerRecord
  const uint32_t list = v0_too ? v0 + 6 + 4 : header;  // BaseGlyphList: a count and two records
  const uint32_t layers = list + 4 + 2 * 6;      // LayerList: a count and an offset per layer
  const uint32_t blob = layers + 4 + 4 * (uint32_t)layer_at.size();
  Bytes out;
  gfnttest::put_u16(out, 1);                     // version
  gfnttest::put_u16(out, v0_too ? 1 : 0);        // version 0 records
  gfnttest::put_u32(out, v0_too ? v0 : 0);
  gfnttest::put_u32(out, v0_too ? v0 + 6 : 0);
  gfnttest::put_u16(out, v0_too ? 1 : 0);
  gfnttest::put_u32(out, list);
  gfnttest::put_u32(out, layers);
  gfnttest::put_u32(out, 0);                     // no clip list, no variations
  gfnttest::put_u32(out, 0);
  gfnttest::put_u32(out, 0);
  if (v0_too) {
    gfnttest::put_u16(out, deep);                // BaseGlyphRecord: the glyph, its first layer, one layer
    gfnttest::put_u16(out, 0);
    gfnttest::put_u16(out, 1);
    gfnttest::put_u16(out, no_record);           // LayerRecord: the glyph, entry 2
    gfnttest::put_u16(out, 2);
  }
  gfnttest::put_u32(out, 2);
  const uint16_t first = no_record < deep ? no_record : deep;
  const uint16_t second = no_record < deep ? deep : no_record;
  gfnttest::put_u16(out, first);
  gfnttest::put_u32(out, blob - list + (first == no_record ? no_record_at : deep_at));
  gfnttest::put_u16(out, second);
  gfnttest::put_u32(out, blob - list + (second == no_record ? no_record_at : deep_at));
  gfnttest::put_u32(out, (uint32_t)layer_at.size());
  for (uint32_t at : layer_at) {
    gfnttest::put_u32(out, blob - layers + at);
  }
  out.insert(out.end(), paints.begin(), paints.end());
  return out;
}

}  // namespace

// --- the palette ------------------------------------------------------------------------------------------------

TEST(SceneColor, APaletteIsTheCpalPaletteInEightBitSrgb) {
  Color c("color-v0.ttf");
  size_t palettes = 0;
  size_t entries = 0;
  ASSERT_EQ(gfnt_face_palette_count(c.face, &palettes, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_palette_entry_count(c.face, &entries, nullptr), GFNT_OK);
  ASSERT_EQ(palettes, 3u);
  for (size_t p = 0; p < palettes; p++) {
    const GSHSCENE_Resource * pal = c.palette(p);
    ASSERT_NE(pal, nullptr);
    EXPECT_EQ(gshscene_resource_kind(pal), GSHSCENE_RESOURCE_PALETTE);
    ASSERT_EQ(gshscene_resource_palette_count(pal), entries);
    for (size_t e = 0; e < entries; e++) {
      GFNT_Color want{};
      ASSERT_EQ(gfnt_face_palette_color(c.face, p, e, &want, nullptr), GFNT_OK);
      GSHSCENE_Color got;
      ASSERT_EQ(gshscene_resource_palette_color(pal, e, &got), GSHSCENE_OK);
      const uint8_t channels[4] = {want.red, want.green, want.blue, want.alpha};
      for (int k = 0; k < 4; k++) {
        EXPECT_EQ(got.rgba[k], (double)channels[k] / 255.0) << p << " " << e << " " << k;
      }
      EXPECT_EQ(got.space.primaries, GCOL_PRIMARIES_SRGB);
      EXPECT_EQ(got.space.transfer, GCOL_TRANSFER_SRGB);
    }
  }
  // Interned by content: the same palette twice is one handle.
  EXPECT_EQ(c.palette(1), c.palette(1));
  const GSHSCENE_Resource * none = nullptr;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_scene_intern_palette(c.face_resource, c.store, palettes, &none, &error), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_scene_intern_palette(nullptr, c.store, 0, &none, &error), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_scene_intern_palette(c.face_resource, nullptr, 0, &none, &error), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_scene_intern_palette(c.face_resource, c.store, 0, nullptr, &error), GFNT_ERR_INVALID);
  EXPECT_EQ(none, nullptr);
  // A face with no CPAL, and a face resource another binds (here: none).
  Color plain("basic.ttf");
  EXPECT_EQ(gfnt_scene_intern_palette(plain.face_resource, plain.store, 0, &none, &error), GFNT_ERR_UNSUPPORTED);
  GSHSCENE_Face_Desc desc{};
  desc.font_key = 0x1234;
  const GSHSCENE_Resource * unbound = nullptr;
  ASSERT_EQ(gshscene_store_intern_face(c.store, &desc, &unbound), GSHSCENE_OK);
  EXPECT_EQ(gfnt_scene_intern_palette(unbound, c.store, 0, &none, &error), GFNT_ERR_INVALID);
  gshscene_resource_release(unbound);
}

// --- version 0 -------------------------------------------------------------------------------------------------

TEST(SceneColor, VersionZeroLayersAreFillsOfTheirEntriesBottomFirst) {
  Color c("color-v0.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kA, &out), GFNT_OK);
  EXPECT_EQ(out.source, (uint32_t)GFNT_COLOR_SOURCE_V0);
  EXPECT_EQ(out.refusal, (uint32_t)GFNT_COLOR_LOWERED);
  EXPECT_EQ(out.items, 3u);
  ASSERT_EQ(gshscene_list_count(l), 3u);
  expect_fill(l, 0, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "A layer 0");
  expect_fill(l, 1, kLayerB, GSHSCENE_PAINT_PALETTE, 1, 1.0, "A layer 1");
  expect_fill(l, 2, kLayerC, GSHSCENE_PAINT_FOREGROUND, 0, 1.0, "A layer 2: 0xFFFF is the foreground");
  for (size_t i = 0; i < 3; i++) {
    EXPECT_TRUE(is_identity(item_of(l, i)->map)) << i;
    GSHSCENE_Glyph_Run run{};
    ASSERT_EQ(gshscene_resource_glyph_run(item_of(l, i)->resource, &run), GSHSCENE_OK);
    EXPECT_EQ(run.face, c.face_resource);
    EXPECT_EQ(run.size, 20.0);
    EXPECT_EQ(run.glyphs[0].x, 10.0);
    EXPECT_EQ(run.glyphs[0].y, 30.0);
    EXPECT_EQ(run.hint_mode, (uint32_t)GSHSCENE_HINT_NONE);
  }
  ASSERT_EQ(c.lower(l, kB, &out), GFNT_OK);
  EXPECT_EQ(out.items, 1u);
  expect_fill(l, 3, kLayerB, GSHSCENE_PAINT_PALETTE, 2, 1.0, "B");
  // Not a color glyph: nothing recorded, no refusal.
  ASSERT_EQ(c.lower(l, kSpace, &out), GFNT_OK);
  EXPECT_EQ(out.source, (uint32_t)GFNT_COLOR_SOURCE_NONE);
  EXPECT_EQ(out.items, 0u);
  EXPECT_EQ(gshscene_list_count(l), 4u);
  Color plain("basic.ttf");
  GSHSCENE_List * p = plain.open();
  ASSERT_EQ(plain.lower(p, 2, &out), GFNT_OK);
  EXPECT_EQ(out.source, (uint32_t)GFNT_COLOR_SOURCE_NONE);
  EXPECT_EQ(gshscene_list_count(p), 0u);
}

// --- version 1 -------------------------------------------------------------------------------------------------

TEST(SceneColor, VersionOneLayersOfSolidsAreFillsWithTheirAlphas) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kA, &out), GFNT_OK);
  EXPECT_EQ(out.source, (uint32_t)GFNT_COLOR_SOURCE_V1);
  EXPECT_EQ(out.items, 3u);
  expect_fill(l, 0, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "solid 0");
  expect_fill(l, 1, kLayerB, GSHSCENE_PAINT_FOREGROUND, 0, 0.5, "the foreground at half alpha");
  expect_fill(l, 2, kLayerC, GSHSCENE_PAINT_PALETTE, 2, 0.25, "solid 2 at a quarter");
}

TEST(SceneColor, ASolidsAlphaIsClampedToZeroAndOne) {
  // F2.14 stores 1.5 and -0.5; COLR clamps them, and the scene's paint takes only [0, 1].
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kPaintI, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 2u);
  expect_fill(l, 0, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "1.5 clamped to 1");
  expect_fill(l, 1, kLayerB, GSHSCENE_PAINT_PALETTE, 1, 0.0, "-0.5 clamped to 0");
}

TEST(SceneColor, AGlyphOverTransformsOverASolidIsOneFillUnderTheTransformsAboveIt) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kB, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 1u);
  expect_fill(l, 0, kLayerA, GSHSCENE_PAINT_PALETTE, 1, 0.75, "B");
  // T = translate(-30, 40) . rotate 45 degrees about (50, -50), in design units, y up; the scale under the glyph
  // moves only its solid.  A design point p is drawn at g + s (T(p).x, -T(p).y), g = (10, 30), s = 20 / upem.
  const GSHGEOM_Map & m = item_of(l, 0)->map;
  const double s = 20.0 / c.upem;
  const double r = std::sqrt(0.5);
  const double points[3][2] = {{0.0, 0.0}, {100.0, 0.0}, {0.0, 100.0}};
  for (const auto & p : points) {
    double qx = p[0] - 50.0;
    double qy = p[1] + 50.0;
    double tx = r * qx - r * qy + 50.0 - 30.0;
    double ty = r * qx + r * qy - 50.0 + 40.0;
    GSHGEOM_Point in{10.0 + s * p[0], 30.0 - s * p[1], 0.0};
    GSHGEOM_Point got{};
    ASSERT_EQ(gshgeom_map_apply_point(&m, in, &got), GSHGEOM_OK);
    EXPECT_NEAR(got.x, 10.0 + s * tx, 1e-9);
    EXPECT_NEAR(got.y, 30.0 - s * ty, 1e-9);
  }
}

TEST(SceneColor, APorterDuffCompositeIsAnIsolatedPairOfGroups) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kAlt, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 6u);
  const GSHSCENE_Item * g = item_of(l, 0);
  EXPECT_EQ(g->kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_EQ(g->group.composite, (uint32_t)GSHSCENE_COMPOSITE_SRC_OVER);
  EXPECT_EQ(g->layer, 3);
  expect_fill(l, 1, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "the backdrop");
  g = item_of(l, 2);
  EXPECT_EQ(g->kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_EQ(g->group.composite, (uint32_t)GSHSCENE_COMPOSITE_SRC_IN) << "COLR's src_in (5) is the scene's";
  EXPECT_EQ(g->group.opacity, 1.0);
  expect_fill(l, 3, kLayerB, GSHSCENE_PAINT_PALETTE, 1, 1.0, "the source");
  EXPECT_EQ(item_of(l, 4)->kind, GSHSCENE_ITEM_GROUP_POP);
  EXPECT_EQ(item_of(l, 5)->kind, GSHSCENE_ITEM_GROUP_POP);
}

TEST(SceneColor, AGlyphOverLayersIsAClipAroundThem) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kPaintD, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 4u);
  const GSHSCENE_Item * clip = item_of(l, 0);
  EXPECT_EQ(clip->kind, GSHSCENE_ITEM_CLIP_PUSH);
  EXPECT_EQ(glyph_of(clip), kLayerC);
  EXPECT_EQ(clip->layer, 3);
  expect_fill(l, 1, kLayerA, GSHSCENE_PAINT_PALETTE, 2, 1.0, "inside the clip");
  expect_fill(l, 2, kLayerB, GSHSCENE_PAINT_PALETTE, 0, 0.5, "inside the clip");
  EXPECT_EQ(item_of(l, 3)->kind, GSHSCENE_ITEM_CLIP_POP);
}

TEST(SceneColor, AClipTakesTheTransformsAboveItAndASiblingDoesNot) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kPaintJ, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 5u);
  const GSHSCENE_Item * clip = item_of(l, 0);
  EXPECT_EQ(clip->kind, GSHSCENE_ITEM_CLIP_PUSH);
  EXPECT_EQ(glyph_of(clip), kLayerC);
  expect_fill(l, 1, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "inside the clip");
  expect_fill(l, 2, kLayerB, GSHSCENE_PAINT_PALETTE, 1, 1.0, "inside the clip");
  EXPECT_EQ(item_of(l, 3)->kind, GSHSCENE_ITEM_CLIP_POP);
  expect_fill(l, 4, kLayerB, GSHSCENE_PAINT_PALETTE, 2, 1.0, "the sibling");
  // The translation (50, -20) in design units is (50 s, 20 s) in the list, s = 20 / upem.
  const double s = 20.0 / c.upem;
  for (size_t i : {0u, 1u, 2u}) {
    const GSHGEOM_Map & m = item_of(l, i)->map;
    EXPECT_EQ(m.t[0], s * 50.0) << i;
    EXPECT_EQ(m.t[1], -(s * -20.0)) << i;
  }
  EXPECT_TRUE(is_identity(item_of(l, 4)->map)) << "the transform does not reach the sibling";
}

TEST(SceneColor, AnotherGlyphsGraphIsLoweredUnderTheTransformsAboveIt) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kPaintE, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 3u);
  expect_fill(l, 0, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "A's layer 0");
  expect_fill(l, 1, kLayerB, GSHSCENE_PAINT_FOREGROUND, 0, 0.5, "A's layer 1");
  expect_fill(l, 2, kLayerC, GSHSCENE_PAINT_PALETTE, 2, 0.25, "A's layer 2");
  GSHGEOM_Map want;
  gshgeom_map_translate(2, 20.0 / c.upem * 100.0, -0.0, 0.0, &want);
  for (size_t i = 0; i < 3; i++) {
    const GSHGEOM_Map & m = item_of(l, i)->map;
    EXPECT_EQ(m.kind, (uint32_t)GSHGEOM_MAP_TRANSLATES) << "a translation stays a translation";
    EXPECT_EQ(m.t[0], want.t[0]);
    EXPECT_EQ(m.t[1], 0.0);
  }
}

TEST(SceneColor, ColrV1sTransformsFromTheTableReachTheMap) {
  // color-v1.ttf's B: every transform paint, chained ten deep, over a glyph and a solid.
  Color c("color-v1.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kB, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 1u);
  expect_fill(l, 0, kLayerA, GSHSCENE_PAINT_PALETTE, 1, 1.0, "B");
  EXPECT_FALSE(is_identity(item_of(l, 0)->map));
  EXPECT_TRUE(item_of(l, 0)->map.kind & GSHGEOM_MAP_SKEWS);
}

// --- refusals ---------------------------------------------------------------------------------------------------

TEST(SceneColor, EachRefusalNamesItsReasonAndLeavesTheListAsItWas) {
  struct Case {
    const char * font;
    uint32_t glyph;
    GFNT_ColorRefusal refusal;
    uint32_t format;
  } cases[] = {
      {"color-v1.ttf", kA, GFNT_COLOR_REFUSED_GRADIENT, GFNT_PAINT_LINEAR_GRADIENT},
      {"color-v1.ttf", kAlt, GFNT_COLOR_REFUSED_BLEND_MODE, GFNT_PAINT_COMPOSITE},
      {"color-v1-scene.ttf", kPaintF, GFNT_COLOR_REFUSED_BLEND_MODE, GFNT_PAINT_COMPOSITE},
      {"color-v1-scene.ttf", kPaintG, GFNT_COLOR_REFUSED_UNBOUNDED, GFNT_PAINT_SOLID},
  };
  for (const Case & k : cases) {
    Color c(k.font);
    GSHSCENE_List * l = c.open();
    // Something recorded before, which must survive.
    GFNT_ColorLowering first{};
    if (std::string(k.font) == "color-v1-scene.ttf") {
      ASSERT_EQ(c.lower(l, kAlt, &first), GFNT_OK);
    }
    size_t before = gshscene_list_count(l);
    uint64_t hash = gshscene_list_hash(l);
    GFNT_ColorLowering out{};
    std::memset(&out, 0xAB, sizeof out);
    GFNT_ColorRun run{};
    run.size = 1280;
    run.bits = 6;
    run.hint_mode = GSHSCENE_HINT_NONE;
    GFNT_Error error{};
    EXPECT_EQ(gfnt_scene_record_color_glyph(c.face_resource, c.store, l, k.glyph, &run, &out, &error),
        GFNT_ERR_UNSUPPORTED) << k.font << " " << k.glyph;
    EXPECT_EQ(error.result, GFNT_ERR_UNSUPPORTED);
    EXPECT_NE(error.message, nullptr);
    EXPECT_EQ(out.refusal, (uint32_t)k.refusal) << k.font << " " << k.glyph;
    EXPECT_EQ(out.source, (uint32_t)GFNT_COLOR_SOURCE_V1);
    if (k.format) {
      EXPECT_EQ(out.paint_format, k.format) << k.font << " " << k.glyph;
    } else {
      EXPECT_NE(out.paint_format, 0u);
    }
    EXPECT_EQ(out.items, 0u);
    EXPECT_EQ(gshscene_list_count(l), before) << "all or nothing";
    EXPECT_EQ(gshscene_list_hash(l), hash);
    if (k.refusal == GFNT_COLOR_REFUSED_BLEND_MODE) {
      EXPECT_EQ(out.composite_mode, 23u) << "multiply (COLR numbers it 23)";
    }
  }
}

TEST(SceneColor, EveryColorGlyphOfAVariableGraphIsRefusedAsVariable) {
  // color-v1-var.ttf: its two base glyphs are made of Var forms only (tools/fixtures/color_fixtures.py).
  Color c("color-v1-var.ttf");
  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(c.face, &glyphs, nullptr), GFNT_OK);
  GSHSCENE_List * l = c.open();
  int refused = 0;
  for (uint32_t g = 0; g < glyphs; g++) {
    GFNT_ColorLowering out{};
    GFNT_Result r = c.lower(l, g, &out);
    if (out.source == GFNT_COLOR_SOURCE_NONE) {
      EXPECT_EQ(r, GFNT_OK) << g;
      continue;
    }
    EXPECT_EQ(r, GFNT_ERR_UNSUPPORTED) << g;
    EXPECT_EQ(out.refusal, (uint32_t)GFNT_COLOR_REFUSED_VARIABLE) << g;
    refused++;
  }
  EXPECT_EQ(refused, 2);
  EXPECT_EQ(gshscene_list_count(l), 0u);
}

TEST(SceneColor, AGraphThatNamesItselfIsALimitAndLeavesTheListAsItWas) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kA, &out), GFNT_OK);
  EXPECT_EQ(c.lower(l, kPaintH, &out), GFNT_ERR_LIMIT);
  EXPECT_EQ(gshscene_list_count(l), 3u);
}

TEST(SceneColor, AGraphDeeperThanTheWalksBoundIsALimitAndAShallowerOneIsLowered) {
  // Built here rather than as a fixture: the paint-graph differential (color_diff.py) walks every fixture with a
  // reference that has no depth bound and no answer for a glyph with no record.
  const Bytes base = file_bytes("color-v1-scene.ttf");
  for (size_t levels : {10u, 64u, 65u, 70u}) {
    Color c(with_colr(base, colr_nested(kA, kB, kLayerA, levels)));
    GSHSCENE_List * l = c.open();
    GFNT_ColorLowering out{};
    // The root is depth 0, layer k depth k + 1, the glyph's solid one deeper: a bound of 64 takes 63 levels.
    GFNT_Result r = c.lower(l, kB, &out);
    if (levels + 1 <= 64) {
      ASSERT_EQ(r, GFNT_OK) << levels;
      ASSERT_EQ(gshscene_list_count(l), 1u);
      expect_fill(l, 0, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "the deepest layer");
    } else {
      EXPECT_EQ(r, GFNT_ERR_LIMIT) << levels;
      EXPECT_EQ(gshscene_list_count(l), 0u);
    }
    // A color glyph naming a glyph with no version 1 record.
    std::memset(&out, 0, sizeof out);
    EXPECT_EQ(c.lower(l, kA, &out), GFNT_ERR_UNSUPPORTED);
    EXPECT_EQ(out.refusal, (uint32_t)GFNT_COLOR_REFUSED_NO_RECORD);
    EXPECT_EQ(out.paint_format, (uint32_t)GFNT_PAINT_COLR_GLYPH);
  }
}

TEST(SceneColor, AGlyphWithBothRecordsIsLoweredFromVersionOne) {
  Color c(with_colr(file_bytes("color-v1-scene.ttf"), colr_nested(kA, kB, kLayerA, 3, true)));
  size_t layers = 0;
  ASSERT_EQ(gfnt_face_color_layer_count(c.face, kB, &layers, nullptr), GFNT_OK);
  ASSERT_EQ(layers, 1u) << "the version 0 record is there";
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kB, &out), GFNT_OK);
  EXPECT_EQ(out.source, (uint32_t)GFNT_COLOR_SOURCE_V1);
  ASSERT_EQ(gshscene_list_count(l), 1u);
  expect_fill(l, 0, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "version 1's glyph and entry, not version 0's");
}

namespace {

GSHSCENE_Result other_info(void *, const GSHSCENE_Face *, GSHSCENE_Face_Info * out) {
  std::memset(out, 0, sizeof *out);
  out->upem = 1000;
  out->glyph_count = 16;
  return GSHSCENE_OK;
}
GSHSCENE_Result other_outline(void *, const GSHSCENE_Face *, uint32_t, GSHGEOM_Path *) {
  return GSHSCENE_OK;
}
GSHSCENE_Result other_box(void *, const GSHSCENE_Face *, uint32_t, GSHSCENE_Glyph_Box * out) {
  std::memset(out, 0, sizeof *out);
  out->empty = 1;
  return GSHSCENE_OK;
}

}  // namespace

TEST(SceneColor, AFaceAnotherProviderBindsIsNotThisLibrarys) {
  Color c("color-v1-scene.ttf");
  static const GSHSCENE_Glyph_Provider other = {GSHSCENE_GLYPH_PROVIDER_VERSION, other_info, other_outline, nullptr,
      other_box, nullptr, nullptr, nullptr};
  int context = 7;
  GSHSCENE_Face_Desc desc{};
  desc.font_key = 0x4242;
  desc.provider = &other;
  desc.ctx = &context;
  const GSHSCENE_Resource * face = nullptr;
  ASSERT_EQ(gshscene_store_intern_face(c.store, &desc, &face), GSHSCENE_OK);
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  GFNT_ColorRun run{};
  run.size = 640;
  run.bits = 6;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_scene_record_color_glyph(face, c.store, l, kA, &run, &out, &error), GFNT_ERR_INVALID);
  const GSHSCENE_Resource * palette = nullptr;
  EXPECT_EQ(gfnt_scene_intern_palette(face, c.store, 0, &palette, &error), GFNT_ERR_INVALID);
  EXPECT_EQ(palette, nullptr);
  EXPECT_EQ(gshscene_list_count(l), 0u);
  gshscene_resource_release(face);
}

TEST(SceneColor, BadArgumentsAndAListTheSceneRefuses) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  GFNT_ColorRun run{};
  run.size = 640;
  run.bits = 6;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_scene_record_color_glyph(nullptr, c.store, l, kA, &run, &out, &error), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_scene_record_color_glyph(c.face_resource, nullptr, l, kA, &run, &out, &error), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_scene_record_color_glyph(c.face_resource, c.store, nullptr, kA, &run, &out, &error), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_scene_record_color_glyph(c.face_resource, c.store, l, kA, nullptr, &out, &error), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_scene_record_color_glyph(c.face_resource, c.store, l, kA, &run, nullptr, &error), GFNT_ERR_INVALID);
  GSHSCENE_Face_Desc desc{};
  desc.font_key = 0x77;
  const GSHSCENE_Resource * unbound = nullptr;
  ASSERT_EQ(gshscene_store_intern_face(c.store, &desc, &unbound), GSHSCENE_OK);
  EXPECT_EQ(gfnt_scene_record_color_glyph(unbound, c.store, l, kA, &run, &out, &error), GFNT_ERR_INVALID);
  gshscene_resource_release(unbound);
  // A run the scene refuses (17 fraction bits), and a closed list: nothing recorded.
  run.bits = 17;
  EXPECT_EQ(gfnt_scene_record_color_glyph(c.face_resource, c.store, l, kA, &run, &out, &error), GFNT_ERR_INVALID);
  run.bits = 6;
  ASSERT_EQ(gfnt_scene_record_color_glyph(c.face_resource, c.store, l, kAlt, &run, &out, &error), GFNT_OK);
  ASSERT_EQ(gshscene_list_close(l), GSHSCENE_OK);
  EXPECT_EQ(gfnt_scene_record_color_glyph(c.face_resource, c.store, l, kA, &run, &out, &error), GFNT_ERR_INVALID);
  EXPECT_EQ(gshscene_list_count(l), 6u);
  // A list of another store.
  GSHSCENE_Store * other = nullptr;
  ASSERT_EQ(gshscene_store_create(nullptr, nullptr, &other), GSHSCENE_OK);
  GSHSCENE_List * elsewhere = nullptr;
  ASSERT_EQ(gshscene_list_create(nullptr, nullptr, other, &elsewhere), GSHSCENE_OK);
  EXPECT_EQ(gfnt_scene_record_color_glyph(c.face_resource, c.store, elsewhere, kA, &run, &out, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gshscene_list_count(elsewhere), 0u);
  gshscene_list_destroy(elsewhere);
  gshscene_store_destroy(other);
}

// --- rendered -----------------------------------------------------------------------------------------------------

TEST(SceneColor, ALoweredGlyphRendersAsItsSolidTwinAndAMissingEntryIsRefused) {
  // Each lowered fill, with the palette set, draws as the solid fill of the entry's 8-bit color in sRGB (and the
  // foreground's); a palette of fewer entries leaves the fills past it refused by the plan.
  for (const char * font : {"color-v0.ttf", "color-v1-scene.ttf"}) {
    Color c(font);
    GSHSCENE_List * l = c.open();
    GFNT_ColorLowering out{};
    ASSERT_EQ(c.lower(l, kA, &out, 40 * 64, 4 * 64, 44 * 64), GFNT_OK);
    const GSHSCENE_Resource * pal = c.palette(0);
    ASSERT_EQ(gshscene_list_set_palette(l, pal), GSHSCENE_OK);
    GSHSCENE_Color fg = {{0.0, 0.5, 0.25, 1.0}, {}};
    ASSERT_EQ(gshscene_list_set_foreground(l, &fg), GSHSCENE_OK);
    ASSERT_EQ(gshscene_list_close(l), GSHSCENE_OK);
    GSHSCENE_List * twin = c.open();
    for (size_t i = 0; i < gshscene_list_count(l); i++) {
      const GSHSCENE_Item * item = item_of(l, i);
      GSHSCENE_Color color;
      ASSERT_EQ(gshscene_paint_resolve(&item->paint, &fg, pal, &color), GSHSCENE_OK);
      GSHSCENE_Paint solid{};
      solid.kind = GSHSCENE_PAINT_SOLID;
      std::memcpy(solid.rgba, color.rgba, sizeof solid.rgba);
      solid.space = color.space;
      ASSERT_EQ(gshscene_list_fill(twin, item->resource, &item->map, item->fill_rule, &solid, item->layer, 0.0),
          GSHSCENE_OK);
    }
    ASSERT_EQ(gshscene_list_close(twin), GSHSCENE_OK);
    std::vector<GSHSCENE_Reason> reasons;
    const std::string drawn = render(l, 64, 64, &reasons);
    EXPECT_TRUE(reasons.empty()) << font;
    EXPECT_TRUE(any_ink(drawn)) << font;
    EXPECT_EQ(drawn, render(twin, 64, 64)) << font;
    // A palette of one entry: the fills of entries 1 and 2 are refused, the rest drawn.
    GSHSCENE_List * short_palette = c.open();
    ASSERT_EQ(c.lower(short_palette, kA, &out, 40 * 64, 4 * 64, 44 * 64), GFNT_OK);
    const uint8_t one[4] = {255, 0, 0, 255};
    const GSHSCENE_Resource * small = nullptr;
    ASSERT_EQ(gshscene_store_intern_palette_rgba8(c.store, 1, one, &small), GSHSCENE_OK);
    c.held.push_back(small);
    ASSERT_EQ(gshscene_list_set_palette(short_palette, small), GSHSCENE_OK);
    ASSERT_EQ(gshscene_list_close(short_palette), GSHSCENE_OK);
    std::vector<GSHSCENE_Reason> refused;
    render(short_palette, 64, 64, &refused);
    ASSERT_EQ(refused.size(), 1u) << font << ": one layer names entry 1 or 2";
    EXPECT_EQ(refused[0], GSHSCENE_REASON_PALETTE_ENTRY);
  }
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
