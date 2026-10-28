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
#include <ghoti.io/font/outline.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/shape-cpu/render.h>
#include <ghoti.io/shape-geometry/map.h>
#include <ghoti.io/shape-scene/list.h>
#include <ghoti.io/shape-scene/report.h>
#include <ghoti.io/shape-scene/resource.h>
#include <ghoti.io/shape-scene/sink.h>

extern "C" {
#include "../../src/shape/fallback.h"
}

namespace {

using Bytes = std::vector<uint8_t>;

// The color fixtures' glyphs (tools/fixtures/color_fixtures.py: ORDER, then SCENE_EXTRA).
const uint32_t kSpace = 1, kA = 2, kB = 3, kAlt = 4, kLayerA = 5, kLayerB = 6, kLayerC = 7;
const uint32_t kPaintD = 8, kPaintE = 9, kPaintF = 10, kPaintG = 11, kPaintH = 12, kPaintI = 13, kPaintJ = 14;
const uint32_t kPaintK = 15, kPaintL = 16, kPaintM = 17, kPaintN = 18, kPaintO = 19, kPaintP = 20, kPaintQ = 21,
    kPaintR = 22;

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
      int32_t x = 10 * 64, int32_t y = 30 * 64, int32_t layer = 3, const GSHSCENE_Resource * at = nullptr,
      uint32_t space = GFNT_COLOR_SPACE_STANDARD, uint32_t policy = GFNT_COLOR_ON_REFUSAL_PLACEHOLDER) {
    GFNT_ColorRun run{};
    run.refusal_policy = policy;
    run.size = size;
    run.x = x;
    run.y = y;
    run.bits = 6;
    run.hint_mode = GSHSCENE_HINT_NONE;
    run.layer = layer;
    run.space = space;
    GFNT_Error error{};
    return gfnt_scene_record_color_glyph(at ? at : face_resource, store, l, glyph, &run, out, &error);
  }
  /** A lowering whose refusals are the call's error (R3): nothing recorded, ::GFNT_ERR_UNSUPPORTED and the reason. */
  GFNT_Result lower_none(GSHSCENE_List * l, uint32_t glyph, GFNT_ColorLowering * out) {
    return lower(l, glyph, out, 20 * 64, 10 * 64, 30 * 64, 3, nullptr, GFNT_COLOR_SPACE_STANDARD,
        GFNT_COLOR_ON_REFUSAL_NONE);
  }
  /** The face interned at the normalised coordinates @p coords (2.14), held until the end. */
  const GSHSCENE_Resource * instance(std::vector<GFNT_F2Dot14> coords) {
    GFNT_Variation v{coords.data(), coords.size(), GFNT_DELTA_ROUND_HALF_UP};
    const GSHSCENE_Resource * r = nullptr;
    GFNT_Error error{};
    EXPECT_EQ(gfnt_scene_intern_face(scene, store, &v, &r, &error), GFNT_OK);
    if (r) {
      held.push_back(r);
    }
    return r;
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

/** The bytes of @p font's `COLR` table. */
Bytes colr_table(const Bytes & font) {
  const size_t count = (size_t)font[4] << 8 | font[5];
  for (size_t i = 0; i < count; i++) {
    const size_t entry = 12 + 16 * i;
    if (be32(font, entry) == GFNT_TAG('C', 'O', 'L', 'R')) {
      const size_t offset = be32(font, entry + 8);
      const size_t length = be32(font, entry + 12);
      return Bytes(font.begin() + (long)offset, font.begin() + (long)(offset + length));
    }
  }
  return Bytes();
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

namespace {

// The glyph group of L8: item 0 and the last item of a version 1 glyph's list (a standard-space lowering).
void expect_group(const GSHSCENE_List * l, size_t count, GCOL_Transfer transfer = GCOL_TRANSFER_LINEAR) {
  ASSERT_EQ(gshscene_list_count(l) >= count, true);
  const GSHSCENE_Item * g = item_of(l, 0);
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(g->kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_EQ(g->group.composite, (uint32_t)GSHSCENE_COMPOSITE_SRC_OVER);
  EXPECT_EQ(g->group.opacity, 1.0);
  EXPECT_EQ(g->group.isolation, (uint32_t)GSHSCENE_ISOLATION_ISOLATED);
  EXPECT_EQ(g->group.blend_space.primaries, GCOL_PRIMARIES_SRGB);
  EXPECT_EQ(g->group.blend_space.transfer, transfer) << "the glyph composites in the standard's space (L8)";
  EXPECT_EQ(g->layer, 3);
  EXPECT_EQ(item_of(l, count - 1)->kind, GSHSCENE_ITEM_GROUP_POP);
}

}  // namespace

TEST(SceneColor, VersionOneLayersOfSolidsAreFillsWithTheirAlphas) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kA, &out), GFNT_OK);
  EXPECT_EQ(out.source, (uint32_t)GFNT_COLOR_SOURCE_V1);
  EXPECT_EQ(out.items, 5u);
  expect_group(l, 5);
  expect_fill(l, 1, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "solid 0");
  expect_fill(l, 2, kLayerB, GSHSCENE_PAINT_FOREGROUND, 0, 0.5, "the foreground at half alpha");
  expect_fill(l, 3, kLayerC, GSHSCENE_PAINT_PALETTE, 2, 0.25, "solid 2 at a quarter");
}

TEST(SceneColor, TheEncodedSpaceIsAFieldOfTheRunAndTheDefaultIsTheStandards) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * standard = c.open();
  GSHSCENE_List * encoded = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(standard, kPaintK, &out), GFNT_OK);
  ASSERT_EQ(c.lower(encoded, kPaintK, &out, 20 * 64, 10 * 64, 30 * 64, 3, nullptr, GFNT_COLOR_SPACE_ENCODED), GFNT_OK);
  expect_group(standard, 3, GCOL_TRANSFER_LINEAR);
  expect_group(encoded, 3, GCOL_TRANSFER_SRGB);
  GSHSCENE_Gradient a{}, b{};
  ASSERT_EQ(gshscene_resource_gradient(item_of(standard, 1)->paint.resource, &a), GSHSCENE_OK);
  ASSERT_EQ(gshscene_resource_gradient(item_of(encoded, 1)->paint.resource, &b), GSHSCENE_OK);
  EXPECT_EQ(a.space.primaries, GCOL_PRIMARIES_SRGB);
  EXPECT_EQ(a.space.transfer, GCOL_TRANSFER_LINEAR) << "L3: the standard interpolates in linear light";
  EXPECT_EQ(a.premultiplied, 1u) << "premultiplied";
  EXPECT_EQ(b.space.transfer, GCOL_TRANSFER_SRGB) << "the encoded setting is what Skia draws";
  EXPECT_EQ(b.premultiplied, 0u);
  EXPECT_NE(item_of(standard, 1)->paint.resource, item_of(encoded, 1)->paint.resource);
  // Not a version 1 glyph's field: version 0 has no group and no space.
  Color v0("color-v0.ttf");
  GSHSCENE_List * l0 = v0.open();
  ASSERT_EQ(v0.lower(l0, kA, &out, 20 * 64, 10 * 64, 30 * 64, 3, nullptr, GFNT_COLOR_SPACE_ENCODED), GFNT_OK);
  EXPECT_EQ(gshscene_list_count(l0), 3u);
  EXPECT_EQ(item_of(l0, 0)->kind, GSHSCENE_ITEM_FILL);
}

TEST(SceneColor, ASolidsAlphaIsClampedToZeroAndOne) {
  // F2.14 stores 1.5 and -0.5; COLR clamps them, and the scene's paint takes only [0, 1].
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kPaintI, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 4u);
  expect_fill(l, 1, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "1.5 clamped to 1");
  expect_fill(l, 2, kLayerB, GSHSCENE_PAINT_PALETTE, 1, 0.0, "-0.5 clamped to 0");
}

TEST(SceneColor, AGlyphOverTransformsOverASolidIsOneFillUnderTheTransformsAboveIt) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kB, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 3u);
  expect_fill(l, 1, kLayerA, GSHSCENE_PAINT_PALETTE, 1, 0.75, "B");
  // T = translate(-30, 40) . rotate 45 degrees about (50, -50), in design units, y up; the scale under the glyph
  // moves only its solid.  A design point p is drawn at g + s (T(p).x, -T(p).y), g = (10, 30), s = 20 / upem.
  const GSHGEOM_Map & m = item_of(l, 1)->map;
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
  ASSERT_EQ(gshscene_list_count(l), 8u);
  expect_group(l, 8);
  const GSHSCENE_Item * g = item_of(l, 1);
  EXPECT_EQ(g->kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_EQ(g->group.composite, (uint32_t)GSHSCENE_COMPOSITE_SRC_OVER);
  EXPECT_EQ(g->layer, 3);
  EXPECT_EQ(g->group.blend_space.primaries, 0) << "the composite's groups inherit the glyph's space";
  expect_fill(l, 2, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "the backdrop");
  g = item_of(l, 3);
  EXPECT_EQ(g->kind, GSHSCENE_ITEM_GROUP_PUSH);
  EXPECT_EQ(g->group.composite, (uint32_t)GSHSCENE_COMPOSITE_SRC_IN) << "COLR's src_in (5) is the scene's";
  EXPECT_EQ(g->group.opacity, 1.0);
  expect_fill(l, 4, kLayerB, GSHSCENE_PAINT_PALETTE, 1, 1.0, "the source");
  EXPECT_EQ(item_of(l, 5)->kind, GSHSCENE_ITEM_GROUP_POP);
  EXPECT_EQ(item_of(l, 6)->kind, GSHSCENE_ITEM_GROUP_POP);
}

TEST(SceneColor, AGlyphOverLayersIsAClipAroundThem) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kPaintD, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 6u);
  expect_group(l, 6);
  const GSHSCENE_Item * clip = item_of(l, 1);
  EXPECT_EQ(clip->kind, GSHSCENE_ITEM_CLIP_PUSH);
  EXPECT_EQ(glyph_of(clip), kLayerC);
  EXPECT_EQ(clip->layer, 3);
  expect_fill(l, 2, kLayerA, GSHSCENE_PAINT_PALETTE, 2, 1.0, "inside the clip");
  expect_fill(l, 3, kLayerB, GSHSCENE_PAINT_PALETTE, 0, 0.5, "inside the clip");
  EXPECT_EQ(item_of(l, 4)->kind, GSHSCENE_ITEM_CLIP_POP);
}

TEST(SceneColor, AClipTakesTheTransformsAboveItAndASiblingDoesNot) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kPaintJ, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 7u);
  expect_group(l, 7);
  const GSHSCENE_Item * clip = item_of(l, 1);
  EXPECT_EQ(clip->kind, GSHSCENE_ITEM_CLIP_PUSH);
  EXPECT_EQ(glyph_of(clip), kLayerC);
  expect_fill(l, 2, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "inside the clip");
  expect_fill(l, 3, kLayerB, GSHSCENE_PAINT_PALETTE, 1, 1.0, "inside the clip");
  EXPECT_EQ(item_of(l, 4)->kind, GSHSCENE_ITEM_CLIP_POP);
  expect_fill(l, 5, kLayerB, GSHSCENE_PAINT_PALETTE, 2, 1.0, "the sibling");
  // The translation (50, -20) in design units is (50 s, 20 s) in the list, s = 20 / upem.
  const double s = 20.0 / c.upem;
  for (size_t i : {1u, 2u, 3u}) {
    const GSHGEOM_Map & m = item_of(l, i)->map;
    EXPECT_EQ(m.t[0], s * 50.0) << i;
    EXPECT_EQ(m.t[1], -(s * -20.0)) << i;
  }
  EXPECT_TRUE(is_identity(item_of(l, 5)->map)) << "the transform does not reach the sibling";
}

TEST(SceneColor, AnotherGlyphsGraphIsLoweredUnderTheTransformsAboveIt) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kPaintE, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 5u);
  expect_group(l, 5);
  expect_fill(l, 1, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "A's layer 0");
  expect_fill(l, 2, kLayerB, GSHSCENE_PAINT_FOREGROUND, 0, 0.5, "A's layer 1");
  expect_fill(l, 3, kLayerC, GSHSCENE_PAINT_PALETTE, 2, 0.25, "A's layer 2");
  GSHGEOM_Map want;
  gshgeom_map_translate(2, 20.0 / c.upem * 100.0, -0.0, 0.0, &want);
  for (size_t i = 1; i < 4; i++) {
    const GSHGEOM_Map & m = item_of(l, i)->map;
    EXPECT_EQ(m.kind, (uint32_t)GSHGEOM_MAP_TRANSLATES) << "a translation stays a translation";
    EXPECT_EQ(m.t[0], want.t[0]);
    EXPECT_EQ(m.t[1], 0.0);
  }
}

TEST(SceneColor, ColrV1sTransformsFromTheTableReachTheMap) {
  // color-v1.ttf's B: every transform paint, chained ten deep, over a glyph and a solid; the font states a clip box
  // for it (the clip list shares one box across three glyphs), so a clip of that rectangle is inside the group.
  Color c("color-v1.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kB, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 5u);
  expect_group(l, 5);
  EXPECT_EQ(item_of(l, 1)->kind, GSHSCENE_ITEM_CLIP_PUSH) << "the font's clip box for B (L5)";
  EXPECT_EQ(item_of(l, 3)->kind, GSHSCENE_ITEM_CLIP_POP);
  expect_fill(l, 2, kLayerA, GSHSCENE_PAINT_PALETTE, 1, 1.0, "B");
  EXPECT_FALSE(is_identity(item_of(l, 2)->map));
  EXPECT_TRUE(item_of(l, 2)->map.kind & GSHGEOM_MAP_SKEWS);
}

TEST(SceneColor, TranslucentLayersCompositeInLinearLightByDefaultAndInEncodedValuesOnRequest) {
  // Glyph A of the scene fixture: an opaque red triangle, the foreground (black) at half alpha over part of it, a
  // third layer on top elsewhere. Where the black half-alpha layer lies over the red alone, the encoded result is
  // red halved (127 or 128) and the standard's (the compositing done on linear-light values, then encoded again) is
  // the sRGB encoding of one half, 188: the amount that OpenType's linear-light sentence makes of the difference.
  auto render_in = [](Color & c, uint32_t space) {
    GSHSCENE_List * l = c.open();
    GFNT_ColorLowering out{};
    EXPECT_EQ(c.lower(l, kA, &out, 40 * 64, 4 * 64, 44 * 64, 3, nullptr, space), GFNT_OK);
    EXPECT_EQ(gshscene_list_set_palette(l, c.palette(0)), GSHSCENE_OK);
    GSHSCENE_Color fg = {{0.0, 0.0, 0.0, 1.0}, {GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB, 0.0}};
    EXPECT_EQ(gshscene_list_set_foreground(l, &fg), GSHSCENE_OK);
    EXPECT_EQ(gshscene_list_close(l), GSHSCENE_OK);
    return render(l, 64, 64);
  };
  Color c("color-v1-scene.ttf");
  const std::string standard = render_in(c, GFNT_COLOR_SPACE_STANDARD);
  const std::string encoded = render_in(c, GFNT_COLOR_SPACE_ENCODED);
  auto srgb = [](double x) { return x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055; };
  const int linear_half = (int)std::lround(255.0 * srgb(0.5));
  EXPECT_EQ(linear_half, 188);
  size_t overlapped = 0;
  for (size_t i = 0; i + 3 < encoded.size(); i += 4) {
    const int r = (uint8_t)encoded[i];
    if (r >= 127 && r <= 128 && encoded[i + 1] == 0 && encoded[i + 2] == 0 && (uint8_t)encoded[i + 3] == 255) {
      overlapped++;
      EXPECT_NEAR((int)(uint8_t)standard[i], linear_half, 1) << "pixel " << i / 4;
      EXPECT_EQ(standard[i + 1], 0);
      EXPECT_EQ((uint8_t)standard[i + 3], 255);
    }
  }
  EXPECT_GT(overlapped, 20u) << "the half-alpha layer over the red is on screen";
  EXPECT_NE(standard, encoded);
}

// --- gradients, planes, boxes, blend modes (L2 to L8) -----------------------------------------------------------------

namespace {

const GSHSCENE_Resource * plane_resource_of(const GSHSCENE_Item * item) {
  return gshscene_resource_kind(item->resource) == GSHSCENE_RESOURCE_PLANE ? item->resource : nullptr;
}

GSHSCENE_Gradient gradient_of(const GSHSCENE_Item * item) {
  GSHSCENE_Gradient g{};
  EXPECT_EQ(item->kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(item->paint.kind, GSHSCENE_PAINT_GRADIENT);
  EXPECT_EQ(gshscene_resource_gradient(item->paint.resource, &g), GSHSCENE_OK);
  EXPECT_EQ(item->paint.rgba[3], 1.0) << "the stops carry the alphas";
  return g;
}

/** Where a design point lands under a gradient's map at a run of size 20 at (10, 30): g + s (T(p).x, -T(p).y). */
GSHGEOM_Point mapped(const GSHGEOM_Map & m, double x, double y) {
  GSHGEOM_Point out{};
  EXPECT_EQ(gshgeom_map_apply_point(&m, GSHGEOM_Point{x, y, 0.0}, &out), GSHGEOM_OK);
  return out;
}

void expect_stop(const GSHSCENE_Gradient_Stop & s, double offset, GSHSCENE_Paint_Kind kind, uint32_t entry, double alpha,
    const char * what) {
  EXPECT_EQ(s.offset, offset) << what;
  EXPECT_EQ(s.color.kind, kind) << what;
  EXPECT_EQ(s.color.entry, entry) << what;
  EXPECT_EQ(s.color.rgba[3], alpha) << what;
}

}  // namespace

TEST(SceneColor, ALinearGradientUnderAGlyphIsOneFillAndItsOwnTransformsAreTheGradients) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kPaintK, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 3u);
  expect_group(l, 3);
  const GSHSCENE_Item * fill = item_of(l, 1);
  EXPECT_EQ(glyph_of(fill), kLayerA) << "the glyph's own outline is filled";
  const double s = 20.0 / c.upem;
  // The fill's map is the transform above the glyph (translate (20, -10)); the scale between the glyph and the
  // gradient is not on the outline (L4).
  EXPECT_EQ(fill->map.t[0], s * 20.0);
  EXPECT_EQ(fill->map.t[1], -(s * -10.0));
  EXPECT_EQ(fill->map.m[0][0], 1.0);
  EXPECT_EQ(fill->map.m[1][1], 1.0);
  GSHSCENE_Gradient g = gradient_of(fill);
  EXPECT_EQ(g.kind, (uint32_t)GSHSCENE_GRADIENT_LINEAR);
  EXPECT_EQ(g.extend, (uint32_t)GSHSCENE_EXTEND_PAD);
  EXPECT_EQ(g.space.primaries, GCOL_PRIMARIES_SRGB);
  EXPECT_EQ(g.space.transfer, GCOL_TRANSFER_LINEAR);
  EXPECT_EQ(g.premultiplied, 1u);
  EXPECT_EQ(g.p0[0], 0.0);
  EXPECT_EQ(g.p1[0], 600.0);
  EXPECT_EQ(g.d[0], 0.0) << "d = p2 - p0";
  EXPECT_EQ(g.d[1], 600.0);
  ASSERT_EQ(g.stop_count, 3u);
  expect_stop(g.stops[0], 0.0, GSHSCENE_PAINT_PALETTE, 0, 1.0, "stop 0");
  expect_stop(g.stops[1], 0.75, GSHSCENE_PAINT_FOREGROUND, 0, 0.5, "0xFFFF is the foreground");
  expect_stop(g.stops[2], 1.25, GSHSCENE_PAINT_PALETTE, 2, 0.25, "an offset past 1 is kept");
  // The gradient's map is design_to_model . T, T the scale (1.5, 0.5) between the glyph and the gradient.
  GSHGEOM_Point q = mapped(g.map, 100.0, 200.0);
  EXPECT_NEAR(q.x, 10.0 + s * 150.0, 1e-9);
  EXPECT_NEAR(q.y, 30.0 - s * 100.0, 1e-9);
}

TEST(SceneColor, ARadialAndASweepGradientCarryTheirGeometryExtendsAndSortedStops) {
  Color c("color-v1-scene.ttf");
  GFNT_ColorLowering out{};
  const double s = 20.0 / c.upem;
  GSHSCENE_List * l = c.open();
  ASSERT_EQ(c.lower(l, kPaintL, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 3u);
  GSHSCENE_Gradient r = gradient_of(item_of(l, 1));
  EXPECT_EQ(glyph_of(item_of(l, 1)), kLayerB);
  EXPECT_EQ(r.kind, (uint32_t)GSHSCENE_GRADIENT_RADIAL);
  EXPECT_EQ(r.extend, (uint32_t)GSHSCENE_EXTEND_REFLECT);
  EXPECT_EQ(r.c0[0], 300.0);
  EXPECT_EQ(r.c0[1], 300.0);
  EXPECT_EQ(r.r0, 20.0);
  EXPECT_EQ(r.c1[0], 350.0);
  EXPECT_EQ(r.c1[1], 280.0);
  EXPECT_EQ(r.r1, 400.0);
  ASSERT_EQ(r.stop_count, 2u);
  expect_stop(r.stops[0], 0.0, GSHSCENE_PAINT_PALETTE, 0, 0.75, "sorted: the stop given second");
  expect_stop(r.stops[1], 0.5, GSHSCENE_PAINT_PALETTE, 1, 1.0, "sorted: the stop given first");
  // The rotation about (100, 200) is between the glyph and the gradient, so it is the gradient's, not the outline's:
  // the fill's map is the identity.
  EXPECT_TRUE(is_identity(item_of(l, 1)->map));
  GSHGEOM_Point q = mapped(r.map, 100.0, 200.0);
  EXPECT_NEAR(q.x, 10.0 + s * 100.0, 1e-9) << "the center of the rotation is fixed";
  EXPECT_NEAR(q.y, 30.0 - s * 200.0, 1e-9);

  GSHSCENE_List * m = c.open();
  ASSERT_EQ(c.lower(m, kPaintM, &out), GFNT_OK);
  GSHSCENE_Gradient sw = gradient_of(item_of(m, 1));
  EXPECT_EQ(sw.kind, (uint32_t)GSHSCENE_GRADIENT_SWEEP);
  EXPECT_EQ(sw.extend, (uint32_t)GSHSCENE_EXTEND_REPEAT);
  EXPECT_EQ(sw.center[0], 300.0);
  EXPECT_EQ(sw.center[1], 300.0);
  EXPECT_EQ(sw.angle_unit, (uint32_t)GSHGEOM_ANGLE_DEGREES);
  // 10 and 200 degrees are 910/16384 and 18204/16384 half turns once stored in F2.14.
  EXPECT_EQ(sw.a0, 910.0 * 180.0 / 16384.0);
  EXPECT_EQ(sw.a1, 18204.0 * 180.0 / 16384.0);
}

TEST(SceneColor, APaintInALayersNodeUnderAGlyphFillsThePlaneUnderTheGlyphsClip) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kPaintN, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 7u);
  expect_group(l, 7);
  EXPECT_EQ(item_of(l, 1)->kind, GSHSCENE_ITEM_CLIP_PUSH);
  EXPECT_EQ(glyph_of(item_of(l, 1)), kLayerC);
  const double s = 20.0 / c.upem;
  // 14c refused all three of these; each is a fill of the plane now.
  const GSHSCENE_Item * first = item_of(l, 2);
  ASSERT_NE(plane_resource_of(first), nullptr);
  EXPECT_EQ(first->fill_rule, GSHGEOM_FILL_NONZERO);
  EXPECT_TRUE(is_identity(first->map)) << "no transform above the glyph";
  GSHSCENE_Gradient g1 = gradient_of(first);
  EXPECT_EQ(g1.extend, (uint32_t)GSHSCENE_EXTEND_REPEAT);
  GSHGEOM_Point q = mapped(g1.map, 0.0, 0.0);
  EXPECT_NEAR(q.x, 10.0 + s * 40.0, 1e-9) << "the translation between the glyph and the paint is the gradient's";
  EXPECT_NEAR(q.y, 30.0, 1e-9);
  const GSHSCENE_Item * solid_fill = item_of(l, 3);
  ASSERT_NE(plane_resource_of(solid_fill), nullptr);
  EXPECT_EQ(solid_fill->paint.kind, GSHSCENE_PAINT_PALETTE);
  EXPECT_EQ(solid_fill->paint.entry, 1u);
  EXPECT_EQ(solid_fill->paint.rgba[3], 0.5);
  const GSHSCENE_Item * third = item_of(l, 4);
  ASSERT_NE(plane_resource_of(third), nullptr);
  GSHSCENE_Gradient g3 = gradient_of(third);
  EXPECT_EQ(g3.kind, (uint32_t)GSHSCENE_GRADIENT_RADIAL);
  q = mapped(g3.map, 100.0, 0.0);
  EXPECT_NEAR(q.x, 10.0 + s * 175.0, 1e-9) << "the scale between the glyph and the paint";
  EXPECT_EQ(item_of(l, 5)->kind, GSHSCENE_ITEM_CLIP_POP);
  EXPECT_EQ(plane_resource_of(first), plane_resource_of(third)) << "one plane";
}

TEST(SceneColor, ARootGradientFillsThePlaneInsideTheGlyphsClipBoxAndIsRefusedWithoutOne) {
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kPaintO, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 5u);
  expect_group(l, 5);
  const GSHSCENE_Item * clip = item_of(l, 1);
  EXPECT_EQ(clip->kind, GSHSCENE_ITEM_CLIP_PUSH);
  EXPECT_EQ(clip->layer, 3);
  EXPECT_EQ(clip->fill_rule, GSHGEOM_FILL_NONZERO);
  const GSHGEOM_Path * box = gshscene_resource_path(clip->resource);
  ASSERT_NE(box, nullptr);
  GSHGEOM_Bounds b{};
  ASSERT_EQ(gshgeom_path_bounds(box, &b), GSHGEOM_OK);
  EXPECT_EQ(b.min.x, 0.0);
  EXPECT_EQ(b.min.y, 0.0);
  EXPECT_EQ(b.max.x, 300.0);
  EXPECT_EQ(b.max.y, 500.0);
  // The rectangle is in the design space: the box's far corner is where the glyph outline puts that design point.
  const double s = 20.0 / c.upem;
  GSHGEOM_Point q = mapped(clip->map, 300.0, 500.0);
  EXPECT_NEAR(q.x, 10.0 + s * 300.0, 1e-9);
  EXPECT_NEAR(q.y, 30.0 - s * 500.0, 1e-9);
  ASSERT_NE(plane_resource_of(item_of(l, 2)), nullptr);
  EXPECT_EQ(gradient_of(item_of(l, 2)).kind, (uint32_t)GSHSCENE_GRADIENT_LINEAR);
  EXPECT_EQ(item_of(l, 3)->kind, GSHSCENE_ITEM_CLIP_POP);
  // The same graph without a box states no extent: refused, nothing recorded.
  GSHSCENE_List * l2 = c.open();
  size_t before = gshscene_list_count(l2);
  std::memset(&out, 0xAB, sizeof out);
  EXPECT_EQ(c.lower_none(l2, kPaintP, &out), GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(out.refusal, (uint32_t)GFNT_COLOR_REFUSED_UNBOUNDED);
  EXPECT_EQ(out.paint_format, (uint32_t)GFNT_PAINT_RADIAL_GRADIENT);
  EXPECT_EQ(out.items, 0u);
  EXPECT_EQ(gshscene_list_count(l2), before);
}

TEST(SceneColor, AColorGlyphNamedByAnotherBringsItsOwnBoxUnderTheTransformsAtTheReference) {
  // paint.r is paint.o's graph moved by (100, 0), with a box of its own: two clips, paint.r's around paint.o's, and the
  // box of the glyph that is named moves with the transform above the reference (L5).
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kPaintR, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(l), 7u);
  expect_group(l, 7);
  const GSHSCENE_Item * outer = item_of(l, 1);
  ASSERT_EQ(outer->kind, GSHSCENE_ITEM_CLIP_PUSH);
  GSHGEOM_Bounds b{};
  ASSERT_EQ(gshgeom_path_bounds(gshscene_resource_path(outer->resource), &b), GSHGEOM_OK);
  EXPECT_EQ(b.min.x, -100.0);
  EXPECT_EQ(b.max.x, 900.0);
  const double s = 20.0 / c.upem;
  const GSHSCENE_Item * inner = item_of(l, 2);
  ASSERT_EQ(inner->kind, GSHSCENE_ITEM_CLIP_PUSH);
  ASSERT_EQ(gshgeom_path_bounds(gshscene_resource_path(inner->resource), &b), GSHGEOM_OK);
  EXPECT_EQ(b.min.x, 0.0);
  EXPECT_EQ(b.max.x, 300.0);
  EXPECT_EQ(b.max.y, 500.0);
  GSHGEOM_Point q = mapped(inner->map, 300.0, 500.0);
  EXPECT_NEAR(q.x, 10.0 + s * 400.0, 1e-9) << "the referenced glyph's box moves with the translation above the reference";
  EXPECT_NEAR(q.y, 30.0 - s * 500.0, 1e-9);
  const GSHSCENE_Item * fill = item_of(l, 3);
  ASSERT_NE(plane_resource_of(fill), nullptr);
  EXPECT_FALSE(is_identity(fill->map)) << "the translation is above the innermost clip, so it is the fill's";
  EXPECT_EQ(item_of(l, 4)->kind, GSHSCENE_ITEM_CLIP_POP);
  EXPECT_EQ(item_of(l, 5)->kind, GSHSCENE_ITEM_CLIP_POP);
  // paint.o itself, which has the same box and nothing above it: one clip.
  GSHSCENE_List * alone = c.open();
  ASSERT_EQ(c.lower(alone, kPaintO, &out), GFNT_OK);
  EXPECT_EQ(gshscene_list_count(alone), 5u);
}

TEST(SceneColor, ABoxThatCutsChangesPixelsAndOneThatDoesNotChangesNone) {
  const Bytes base = file_bytes("color-v1-scene.ttf");
  // The clip list's box for paint.o is (0, 0, 300, 500) as four int16: find it and widen it.
  const Bytes colr = colr_table(base);
  const uint8_t cut[8] = {0, 0, 0, 0, 0x01, 0x2C, 0x01, 0xF4};
  size_t at = colr.size();
  for (size_t i = 0; i + 8 <= colr.size(); i++) {
    if (std::memcmp(colr.data() + i, cut, 8) == 0) {
      at = i;
    }
  }
  ASSERT_LT(at, colr.size()) << "paint.o's box is in the table";
  auto with_box = [&](int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    Bytes patched = colr;
    const int16_t v[4] = {x0, y0, x1, y1};
    for (int k = 0; k < 4; k++) {
      patched[at + 2u * k] = (uint8_t)((uint16_t)v[k] >> 8);
      patched[at + 2u * k + 1] = (uint8_t)v[k];
    }
    return with_colr(base, patched);
  };
  auto draw = [&](Bytes font) {
    Color c(std::move(font));
    GSHSCENE_List * l = c.open();
    GFNT_ColorLowering out{};
    EXPECT_EQ(c.lower(l, kPaintO, &out, 40 * 64, 4 * 64, 44 * 64), GFNT_OK);
    const GSHSCENE_Resource * pal = c.palette(0);
    EXPECT_EQ(gshscene_list_set_palette(l, pal), GSHSCENE_OK);
    EXPECT_EQ(gshscene_list_close(l), GSHSCENE_OK);
    return render(l, 64, 64);
  };
  const std::string cutting = draw(base);
  const std::string wide = draw(with_box(-4000, -4000, 8000, 8000));
  const std::string wider = draw(with_box(-8000, -8000, 12000, 12000));
  EXPECT_TRUE(any_ink(cutting));
  EXPECT_NE(cutting, wide) << "a box that cuts changes pixels";
  EXPECT_EQ(wide, wider) << "a box that cuts nothing changes none";
  // The wide box lets the gradient fill the whole 64 by 64 target: every pixel is ink.
  for (size_t i = 3; i < wide.size(); i += 4) {
    ASSERT_NE(wide[i], 0) << "pixel " << i / 4;
  }
}

TEST(SceneColor, EveryBlendModeReachesTheSceneAsItsNumberAndUnknownOnesAreClear) {
  const Bytes base = file_bytes("color-v1-scene.ttf");
  const Bytes colr = colr_table(base);
  // ghoti.alt's graph is a PaintComposite (format 32): format, source offset (24 bits), mode, backdrop offset.
  Color probe(base);
  GFNT_PaintRef root = 0;
  ASSERT_EQ(gfnt_face_color_base_paint(probe.face, kAlt, &root, nullptr), GFNT_OK);
  ASSERT_EQ(colr[root], 32u);
  ASSERT_EQ(colr[root + 4], 5u) << "src_in";
  for (unsigned mode : {0u, 3u, 12u, 13u, 20u, 23u, 26u, 27u, 28u, 29u, 100u, 255u}) {
    Bytes patched = colr;
    patched[root + 4] = (uint8_t)mode;
    Color c(with_colr(base, patched));
    GSHSCENE_List * l = c.open();
    GFNT_ColorLowering out{};
    ASSERT_EQ(c.lower(l, kAlt, &out), GFNT_OK) << mode;
    ASSERT_EQ(gshscene_list_count(l), 8u);
    // The table of 14c for the Porter-Duff modes, the identity for the blend modes, clear for the rest (L7).
    static const uint32_t table[13] = {GSHSCENE_COMPOSITE_CLEAR, GSHSCENE_COMPOSITE_SRC, GSHSCENE_COMPOSITE_DEST,
        GSHSCENE_COMPOSITE_SRC_OVER, GSHSCENE_COMPOSITE_DEST_OVER, GSHSCENE_COMPOSITE_SRC_IN,
        GSHSCENE_COMPOSITE_DEST_IN, GSHSCENE_COMPOSITE_SRC_OUT, GSHSCENE_COMPOSITE_DEST_OUT,
        GSHSCENE_COMPOSITE_SRC_ATOP, GSHSCENE_COMPOSITE_DEST_ATOP, GSHSCENE_COMPOSITE_XOR, GSHSCENE_COMPOSITE_PLUS};
    const uint32_t want = mode <= 12 ? table[mode] : mode <= 27 ? mode : (uint32_t)GSHSCENE_COMPOSITE_CLEAR;
    EXPECT_EQ(item_of(l, 3)->group.composite, want) << "COLR mode " << mode;
    EXPECT_EQ(out.refusal, (uint32_t)GFNT_COLOR_LOWERED);
  }
}

TEST(SceneColor, ABlendModeOverAGradientIsTheFixtureGlyph) {
  Color c("color-v1-scene.ttf");
  GFNT_ColorLowering out{};
  GSHSCENE_List * multiply = c.open();
  ASSERT_EQ(c.lower(multiply, kPaintF, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(multiply), 8u);
  EXPECT_EQ(item_of(multiply, 3)->group.composite, 23u) << "multiply (COLR numbers it 23)";
  EXPECT_EQ(out.composite_mode, 0u);
  GSHSCENE_List * hue = c.open();
  ASSERT_EQ(c.lower(hue, kPaintQ, &out), GFNT_OK);
  ASSERT_EQ(gshscene_list_count(hue), 8u);
  EXPECT_EQ(item_of(hue, 3)->group.composite, 24u) << "hsl_hue (COLR numbers it 24)";
  EXPECT_EQ(item_of(hue, 4)->kind, GSHSCENE_ITEM_FILL);
  EXPECT_EQ(gradient_of(item_of(hue, 4)).kind, (uint32_t)GSHSCENE_GRADIENT_LINEAR);
}

// --- variation (L1) -----------------------------------------------------------------------------------------------

TEST(SceneColor, EveryVarFormIsReadAtTheFaceResourcesInstance) {
  // color-v1-var.ttf: its two base glyphs are made of Var forms (tools/fixtures/color_fixtures.py), over weight and
  // width.  The face resource is the instance: at the default every number is the table's, at another it moves.
  Color c("color-v1-var.ttf");
  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(c.face, &glyphs, nullptr), GFNT_OK);
  const GSHSCENE_Resource * moved = c.instance({16384, -8192});
  const GSHSCENE_Resource * halfway = c.instance({8192, 0});
  ASSERT_NE(moved, nullptr);
  ASSERT_NE(halfway, nullptr);
  int lowered = 0;
  for (uint32_t g = 0; g < glyphs; g++) {
    GSHSCENE_List * at_default = c.open();
    GFNT_ColorLowering out{};
    GFNT_Result r = c.lower(at_default, g, &out);
    if (out.source == GFNT_COLOR_SOURCE_NONE) {
      EXPECT_EQ(r, GFNT_OK) << g;
      continue;
    }
    ASSERT_EQ(r, GFNT_OK) << "a variable paint is lowered, never refused (L1): glyph " << g;
    lowered++;
    GSHSCENE_List * at_moved = c.open();
    ASSERT_EQ(c.lower(at_moved, g, &out, 20 * 64, 10 * 64, 30 * 64, 3, moved), GFNT_OK) << g;
    GSHSCENE_List * at_half = c.open();
    ASSERT_EQ(c.lower(at_half, g, &out, 20 * 64, 10 * 64, 30 * 64, 3, halfway), GFNT_OK) << g;
    EXPECT_EQ(gshscene_list_count(at_moved), gshscene_list_count(at_default)) << "the same graph, other numbers";
    EXPECT_NE(gshscene_list_hash(at_default), gshscene_list_hash(at_moved)) << "the instance moves the numbers: " << g;
    EXPECT_NE(gshscene_list_hash(at_half), gshscene_list_hash(at_moved)) << g;
    EXPECT_NE(gshscene_list_hash(at_half), gshscene_list_hash(at_default)) << g;
    // The same face resource twice is the same lowering, and the default instance is a face with no coordinates.
    GSHSCENE_List * again = c.open();
    ASSERT_EQ(c.lower(again, g, &out, 20 * 64, 10 * 64, 30 * 64, 3, moved), GFNT_OK);
    EXPECT_EQ(gshscene_list_hash(again), gshscene_list_hash(at_moved));
    GSHSCENE_List * zero = c.open();
    const GSHSCENE_Resource * zeros = c.instance({0, 0});
    ASSERT_EQ(c.lower(zero, g, &out, 20 * 64, 10 * 64, 30 * 64, 3, zeros), GFNT_OK);
    EXPECT_EQ(gshscene_list_hash(zero), gshscene_list_hash(at_default)) << "all zero is the default instance";
  }
  EXPECT_EQ(lowered, 2);
}

TEST(SceneColor, AVariablePaintsNumbersAreTheReadersAtTheInstanceAndNotAtTheDefault) {
  Color c("color-v1-var.ttf");
  // The linear gradient of the glyph "tri" (the first layer): x1 = 600 at the default, with a delta by the instance.
  GFNT_Variation v{};
  GFNT_F2Dot14 coords[2] = {16384, -8192};
  v.coords = coords;
  v.count = 2;
  GFNT_PaintRef tri = 0;
  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(c.face, &glyphs, nullptr), GFNT_OK);
  uint32_t first = 0;
  for (uint32_t g = 0; g < glyphs && !first; g++) {
    size_t layers = 0;
    GFNT_PaintRef root = 0;
    if (gfnt_face_color_base_paint(c.face, g, &root, nullptr) == GFNT_OK && root) {
      first = g;
      tri = root;
    }
    (void)layers;
  }
  ASSERT_NE(tri, 0u);
  // Walk to the first gradient leaf at both locations and compare to what the lowering recorded.
  struct Find {
    const GFNT_Face * face;
    const GFNT_Variation * at;
    int32_t x1 = 0;
    bool found = false;
  };
  auto first_linear = [&](const GFNT_Variation * at) {
    Find f{c.face, at};
    struct W {
      static bool enter(void * user, const GFNT_ColorPaint * p, size_t) {
        Find * f = static_cast<Find *>(user);
        if (!f->found && (p->format == GFNT_PAINT_LINEAR_GRADIENT || p->format == GFNT_PAINT_VAR_LINEAR_GRADIENT)) {
          f->found = true;
          f->x1 = p->v[2];
        }
        return true;
      }
    };
    GFNT_PaintVisitor vis{W::enter, nullptr};
    EXPECT_EQ(gfnt_face_color_paint_walk(c.face, first, at, &vis, &f, nullptr), GFNT_OK);
    return f.x1;
  };
  const int32_t at_default = first_linear(nullptr);
  const int32_t at_moved = first_linear(&v);
  ASSERT_NE(at_default, at_moved) << "the fixture's gradient varies";
  const GSHSCENE_Resource * moved = c.instance({16384, -8192});
  for (const GSHSCENE_Resource * face : {static_cast<const GSHSCENE_Resource *>(nullptr), moved}) {
    GSHSCENE_List * l = c.open();
    GFNT_ColorLowering out{};
    ASSERT_EQ(c.lower(l, first, &out, 20 * 64, 10 * 64, 30 * 64, 3, face), GFNT_OK);
    double x1 = -1.0;
    for (size_t i = 0; i < gshscene_list_count(l) && x1 < 0.0; i++) {
      const GSHSCENE_Item * item = item_of(l, i);
      if (item->kind == GSHSCENE_ITEM_FILL && item->paint.kind == GSHSCENE_PAINT_GRADIENT) {
        GSHSCENE_Gradient g{};
        ASSERT_EQ(gshscene_resource_gradient(item->paint.resource, &g), GSHSCENE_OK);
        if (g.kind == GSHSCENE_GRADIENT_LINEAR) {
          x1 = g.p1[0];
        }
      }
    }
    EXPECT_EQ(x1, (double)(face ? at_moved : at_default)) << (face ? "at the instance" : "at the default");
  }
}

// --- refusals ---------------------------------------------------------------------------------------------------

TEST(SceneColor, UnderTheNonePolicyEachRefusalNamesItsReasonAndLeavesTheListAsItWas) {
  // After L10 two reasons are left: a paint no clip bounds in a glyph that states no box, and a color glyph naming a
  // glyph with no version 1 record (the second is in the test of the depth bound); R4 adds the walk's bound (below).
  struct Case {
    const char * font;
    uint32_t glyph;
    GFNT_ColorRefusal refusal;
    uint32_t format;
  } cases[] = {
      {"color-v1-scene.ttf", kPaintG, GFNT_COLOR_REFUSED_UNBOUNDED, GFNT_PAINT_SOLID},
      {"color-v1-scene.ttf", kPaintP, GFNT_COLOR_REFUSED_UNBOUNDED, GFNT_PAINT_RADIAL_GRADIENT},
  };
  for (const Case & k : cases) {
    Color c(k.font);
    GSHSCENE_List * l = c.open();
    // Something recorded before, which must survive.
    GFNT_ColorLowering first{};
    ASSERT_EQ(c.lower(l, kAlt, &first), GFNT_OK);
    size_t before = gshscene_list_count(l);
    uint64_t hash = gshscene_list_hash(l);
    GFNT_ColorLowering out{};
    std::memset(&out, 0xAB, sizeof out);
    GFNT_ColorRun run{};
    run.size = 1280;
    run.bits = 6;
    run.hint_mode = GSHSCENE_HINT_NONE;
    run.refusal_policy = GFNT_COLOR_ON_REFUSAL_NONE;   // the call's own answer (R3); the default records a placeholder
    GFNT_Error error{};
    EXPECT_EQ(gfnt_scene_record_color_glyph(c.face_resource, c.store, l, k.glyph, &run, &out, &error),
        GFNT_ERR_UNSUPPORTED) << k.font << " " << k.glyph;
    EXPECT_EQ(error.result, GFNT_ERR_UNSUPPORTED);
    EXPECT_NE(error.message, nullptr);
    EXPECT_EQ(out.refusal, (uint32_t)k.refusal) << k.font << " " << k.glyph;
    EXPECT_EQ(out.source, (uint32_t)GFNT_COLOR_SOURCE_V1);
    EXPECT_EQ(out.paint_format, k.format) << k.font << " " << k.glyph;
    EXPECT_EQ(out.items, 0u);
    EXPECT_EQ(gshscene_list_count(l), before) << "all or nothing, the glyph's group and clip included";
    EXPECT_EQ(gshscene_list_hash(l), hash);
  }
}

// --- what a refusal is given (color-paint story P5: R3, R4, R5; PH9 to PH13) --------------------------------------------

namespace {

/**
 * A `COLR` table, built here, whose glyph @p base is a `PaintColrGlyph` of @p target (which has no record: the refusal
 * NO_RECORD) and which states a clip box for @p base: `x0 y0 x1 y1` in font units (PH11's first source).
 */
Bytes colr_clipped(uint16_t base, uint16_t target, const int16_t box[4]) {
  Bytes out;
  gfnttest::put_u16(out, 1);                       // version
  gfnttest::put_u16(out, 0);                       // no version 0 records
  gfnttest::put_u32(out, 0);
  gfnttest::put_u32(out, 0);
  gfnttest::put_u16(out, 0);
  gfnttest::put_u32(out, 34);                      // BaseGlyphList
  gfnttest::put_u32(out, 0);                       // no LayerList
  gfnttest::put_u32(out, 34 + 10 + 3);             // ClipList, after the one paint
  gfnttest::put_u32(out, 0);
  gfnttest::put_u32(out, 0);
  gfnttest::put_u32(out, 1);                       // BaseGlyphList: one record
  gfnttest::put_u16(out, base);
  gfnttest::put_u32(out, 10);                      // its paint, from the list's start
  out.push_back(11);                               // PaintColrGlyph
  gfnttest::put_u16(out, target);
  out.push_back(1);                                // ClipList: format 1, one clip
  gfnttest::put_u32(out, 1);
  gfnttest::put_u16(out, base);
  gfnttest::put_u16(out, base);
  put24(out, 12);                                  // the box, from the clip list's start: after the one record
  out.push_back(1);                                // ClipBox format 1
  for (int k = 0; k < 4; k++) {
    gfnttest::put_u16(out, (uint16_t)box[k]);
  }
  return out;
}

/** One refused glyph of a fixture and why. */
struct Refused {
  const char * what;
  Bytes font;
  uint32_t glyph;
  GFNT_ColorRefusal refusal;
  uint32_t format;   ///< The refused paint's format (0 for the walk's bound).
};

std::vector<Refused> refusals_of_the_fixtures() {
  const Bytes base = file_bytes("color-v1-scene.ttf");
  static const int16_t box[4] = {-30, -40, 700, 910};
  std::vector<Refused> out;
  out.push_back({"an unbounded solid", base, kPaintG, GFNT_COLOR_REFUSED_UNBOUNDED, GFNT_PAINT_SOLID});
  out.push_back({"an unbounded gradient", base, kPaintP, GFNT_COLOR_REFUSED_UNBOUNDED, GFNT_PAINT_RADIAL_GRADIENT});
  out.push_back({"a glyph with no record", with_colr(base, colr_nested(kA, kB, kLayerA, 3)), kA,
      GFNT_COLOR_REFUSED_NO_RECORD, GFNT_PAINT_COLR_GLYPH});
  out.push_back({"a space with no record", with_colr(base, colr_nested(kSpace, kB, kLayerA, 3)), kSpace,
      GFNT_COLOR_REFUSED_NO_RECORD, GFNT_PAINT_COLR_GLYPH});
  out.push_back({"a graph past the depth bound", with_colr(base, colr_nested(kA, kB, kLayerA, 70)), kB,
      GFNT_COLOR_REFUSED_WALK_LIMIT, 0});
  out.push_back({"a graph that names itself", base, kPaintH, GFNT_COLOR_REFUSED_WALK_LIMIT, 0});
  out.push_back({"a clip box and no record", with_colr(base, colr_clipped(kA, kLayerA, box)), kA,
      GFNT_COLOR_REFUSED_NO_RECORD, GFNT_PAINT_COLR_GLYPH});
  return out;
}

/** The base glyph's outline control box at the face resource's instance, 26.6; false for an outline with no points. */
bool control_box_of(Color & c, uint32_t glyph, GFNT_Box * box) {
  GFNT_Outline * outline = nullptr;
  if (gfnt_face_glyph_outline(c.face, glyph, nullptr, nullptr, &outline, nullptr) != GFNT_OK) {
    return false;
  }
  const bool has = gfnt_outline_point_count(outline) > 0 && gfnt_outline_control_box(outline, box) == GFNT_OK &&
      !gfnt_box_is_empty(box);
  gfnt_outline_destroy(outline);
  return has;
}

/** What PH11 says the placeholder's box is, from the font alone: the clip box, else the extents, else the outline's. */
bool expected_box(Color & c, const Refused & r, double out[4]) {
  GFNT_ColorBox clip;
  bool found = false;
  EXPECT_EQ(gfnt_face_color_clip(c.face, r.glyph, nullptr, &clip, &found, nullptr), GFNT_OK);
  if (found) {
    out[0] = clip.x_min;
    out[1] = clip.y_min;
    out[2] = clip.x_max;
    out[3] = clip.y_max;
    return true;
  }
  GFNT_Extents e;
  if (gfnt_glyph_extents(c.face, r.glyph, nullptr, &e) && (e.width != 0 || e.height != 0)) {
    out[0] = e.x_bearing;
    out[1] = e.y_bearing + e.height;
    out[2] = e.x_bearing + e.width;
    out[3] = e.y_bearing;
    return true;
  }
  GFNT_Box box;
  if (control_box_of(c, r.glyph, &box)) {
    out[0] = box.x_min / 64.0;
    out[1] = box.y_min / 64.0;
    out[2] = box.x_max / 64.0;
    out[3] = box.y_max / 64.0;
    return true;
  }
  return false;
}

/** The placeholder item at @p at is as PH1 and PH10 say for a glyph refused for @p r, with the box PH11 gives. */
void expect_placeholder(Color & c, const GSHSCENE_List * l, size_t at, const Refused & r, unsigned approximated) {
  const GSHSCENE_Item * item = item_of(l, at);
  ASSERT_NE(item, nullptr) << r.what;
  EXPECT_EQ(item->kind, GSHSCENE_ITEM_PLACEHOLDER) << r.what;
  EXPECT_EQ(item->placeholder.source, (uint16_t)GSHSCENE_PLACEHOLDER_SOURCE_COLR) << r.what;
  EXPECT_EQ(item->placeholder.code, (uint16_t)r.refusal) << r.what << ": the code is the refusal's";
  EXPECT_EQ(item->placeholder.approximated, approximated) << r.what;
  EXPECT_EQ(item->layer, 3) << r.what;
  EXPECT_EQ(item->depth, 0.0) << r.what;
  EXPECT_EQ(item->resource, nullptr) << r.what;
  double want[4] = {0, 0, 0, 0};
  const bool has = expected_box(c, r, want);
  EXPECT_EQ(item->placeholder.has_box, has ? 1u : 0u) << r.what;
  if (has) {
    for (int k = 0; k < 4; k++) {
      EXPECT_EQ(item->placeholder.box[k], want[k]) << r.what << " box " << k;
    }
    // Placed at the run's origin and size: a design corner lands where the glyph outline puts it (g + s (x, -y)).
    const double s = 20.0 / c.upem;
    GSHGEOM_Point q = mapped(item->map, want[2], want[3]);
    EXPECT_NEAR(q.x, 10.0 + s * want[2], 1e-9) << r.what;
    EXPECT_NEAR(q.y, 30.0 - s * want[3], 1e-9) << r.what;
  }
}

}  // namespace

TEST(SceneColor, UnderTheDefaultPolicyARefusedGlyphIsAPlaceholderTheCallSucceedsAndOnlyThePlaceholderIsRecorded) {
  size_t with_box = 0;
  for (const Refused & r : refusals_of_the_fixtures()) {
    Color c(r.font);
    GSHSCENE_List * l = c.open();
    // Something recorded before, which must survive.
    GFNT_ColorLowering first{};
    ASSERT_EQ(c.lower(l, kAlt, &first), GFNT_OK) << r.what;
    const size_t before = gshscene_list_count(l);
    GFNT_ColorLowering out{};
    std::memset(&out, 0xAB, sizeof out);
    GFNT_ColorRun run{};
    run.size = 20 * 64;
    run.x = 10 * 64;
    run.y = 30 * 64;
    run.bits = 6;
    run.layer = 3;
    run.hint_mode = GSHSCENE_HINT_NONE;
    GFNT_Error error{};
    ASSERT_EQ(gfnt_scene_record_color_glyph(c.face_resource, c.store, l, r.glyph, &run, &out, &error), GFNT_OK)
        << r.what;
    EXPECT_EQ(error.result, GFNT_OK) << r.what << ": a success leaves no diagnostic";
    EXPECT_EQ(out.outcome, (uint32_t)GFNT_COLOR_OUTCOME_PLACEHOLDER) << r.what;
    EXPECT_EQ(out.refusal, (uint32_t)r.refusal) << r.what;
    EXPECT_EQ(out.source, (uint32_t)GFNT_COLOR_SOURCE_V1) << r.what;
    EXPECT_EQ(out.paint_format, r.format) << r.what;
    EXPECT_EQ(out.items, 1u) << r.what;
    ASSERT_EQ(gshscene_list_count(l), before + 1) << r.what << ": the glyph's graph is not in the list, only its placeholder";
    expect_placeholder(c, l, before, r, 0u);
    with_box += item_of(l, before)->placeholder.has_box;
  }
  EXPECT_GE(with_box, 3u) << "the fixtures reach more than one source of the box";
}

TEST(SceneColor, ThePlaceholdersBoxIsTheClipBoxThenTheExtentsThenTheOutlinesThenNone) {
  const Bytes base = file_bytes("color-v1-scene.ttf");
  // The clip list's box, as the font states it (PH11, first).
  {
    static const int16_t box[4] = {-30, -40, 700, 910};
    Color c(with_colr(base, colr_clipped(kA, kLayerA, box)));
    GSHSCENE_List * l = c.open();
    GFNT_ColorLowering out{};
    ASSERT_EQ(c.lower(l, kA, &out), GFNT_OK);
    ASSERT_EQ(gshscene_list_count(l), 1u);
    const GSHSCENE_Item * item = item_of(l, 0);
    ASSERT_EQ(item->placeholder.has_box, 1u);
    EXPECT_EQ(item->placeholder.box[0], -30.0);
    EXPECT_EQ(item->placeholder.box[1], -40.0);
    EXPECT_EQ(item->placeholder.box[2], 700.0);
    EXPECT_EQ(item->placeholder.box[3], 910.0);
  }
  // No clip box and no ink anywhere: a placeholder with no box, reported all the same (PH11, last).
  {
    Color c(with_colr(base, colr_nested(kSpace, kB, kLayerA, 3)));
    GFNT_Box box;
    ASSERT_FALSE(control_box_of(c, kSpace, &box)) << "the fixture's space has no outline";
    GSHSCENE_List * l = c.open();
    GFNT_ColorLowering out{};
    ASSERT_EQ(c.lower(l, kSpace, &out), GFNT_OK);
    ASSERT_EQ(gshscene_list_count(l), 1u);
    const GSHSCENE_Item * item = item_of(l, 0);
    EXPECT_EQ(item->kind, GSHSCENE_ITEM_PLACEHOLDER);
    EXPECT_EQ(item->placeholder.has_box, 0u);
    for (double v : item->placeholder.box) {
      EXPECT_EQ(v, 0.0);
    }
  }
}

TEST(SceneColor, UnderTheNonePolicyEveryRefusalIsTheCallsErrorAndNothingIsRecorded) {
  for (const Refused & r : refusals_of_the_fixtures()) {
    Color c(r.font);
    GSHSCENE_List * l = c.open();
    GFNT_ColorLowering first{};
    ASSERT_EQ(c.lower(l, kAlt, &first), GFNT_OK) << r.what;
    const size_t before = gshscene_list_count(l);
    const uint64_t hash = gshscene_list_hash(l);
    GFNT_ColorLowering out{};
    EXPECT_EQ(c.lower_none(l, r.glyph, &out), GFNT_ERR_UNSUPPORTED) << r.what;
    EXPECT_EQ(out.refusal, (uint32_t)r.refusal) << r.what;
    EXPECT_EQ(out.outcome, (uint32_t)GFNT_COLOR_OUTCOME_LOWERED) << r.what << ": nothing is in the list";
    EXPECT_EQ(out.items, 0u) << r.what;
    EXPECT_EQ(gshscene_list_count(l), before) << r.what;
    EXPECT_EQ(gshscene_list_hash(l), hash) << r.what;
  }
}

TEST(SceneColor, UnderTheFallbackPolicyAGlyphWithAnOutlineDrawsItAndOneWithoutIsRefused) {
  size_t drawn = 0;
  size_t empty = 0;
  for (const Refused & r : refusals_of_the_fixtures()) {
    Color c(r.font);
    GSHSCENE_List * l = c.open();
    GFNT_ColorLowering first{};
    ASSERT_EQ(c.lower(l, kAlt, &first), GFNT_OK) << r.what;
    const size_t before = gshscene_list_count(l);
    GFNT_ColorLowering out{};
    ASSERT_EQ(c.lower(l, r.glyph, &out, 20 * 64, 10 * 64, 30 * 64, 3, nullptr, GFNT_COLOR_SPACE_STANDARD,
                  GFNT_COLOR_ON_REFUSAL_FALLBACK), GFNT_OK) << r.what;
    GFNT_Box box;
    const bool outline = control_box_of(c, r.glyph, &box);
    EXPECT_EQ(out.refusal, (uint32_t)r.refusal) << r.what << ": why the stand-in was used";
    if (outline) {
      drawn++;
      EXPECT_EQ(out.outcome, (uint32_t)GFNT_COLOR_OUTCOME_FALLBACK) << r.what;
      EXPECT_EQ(out.items, 2u) << r.what;
      ASSERT_EQ(gshscene_list_count(l), before + 2) << r.what;
      expect_placeholder(c, l, before, r, 1u);
      // Right after it, with its layer and depth: the base glyph's own outline in the foreground, at alpha 1.
      const GSHSCENE_Item * fill = item_of(l, before + 1);
      EXPECT_EQ(fill->kind, GSHSCENE_ITEM_FILL) << r.what;
      EXPECT_EQ(glyph_of(fill), r.glyph) << r.what;
      EXPECT_EQ(fill->paint.kind, GSHSCENE_PAINT_FOREGROUND) << r.what;
      EXPECT_EQ(fill->paint.rgba[3], 1.0) << r.what;
      EXPECT_EQ(fill->fill_rule, GSHGEOM_FILL_NONZERO) << r.what;
      EXPECT_EQ(fill->layer, 3) << r.what;
      EXPECT_EQ(fill->depth, 0.0) << r.what;
      EXPECT_TRUE(is_identity(fill->map)) << r.what << ": no design map";
    } else {
      empty++;
      EXPECT_EQ(out.outcome, (uint32_t)GFNT_COLOR_OUTCOME_PLACEHOLDER) << r.what << ": nothing to draw";
      EXPECT_EQ(out.items, 1u) << r.what;
      ASSERT_EQ(gshscene_list_count(l), before + 1) << r.what;
      expect_placeholder(c, l, before, r, 0u);
    }
  }
  EXPECT_GE(drawn, 1u) << "a fixture refuses a glyph that has an outline";
  EXPECT_GE(empty, 1u) << "a fixture refuses a glyph that has none";
}

TEST(SceneColor, AFailureOfTheCallIsStillAnErrorAndLeavesTheListAsItWas) {
  // R4: a limit of the list, a damaged table and a bad request are not the glyph's refusals.
  Color c("color-v1-scene.ttf");
  {
    GSHSCENE_Limits limits{};
    limits.max_list_items = 2;
    GSHSCENE_List * l = nullptr;
    ASSERT_EQ(gshscene_list_create(nullptr, &limits, c.store, &l), GSHSCENE_OK);
    c.lists.push_back(l);
    GFNT_ColorLowering out{};
    ASSERT_EQ(c.lower(l, kPaintD, &out), GFNT_ERR_LIMIT) << "the glyph's group, its clip and its fills do not fit";
    EXPECT_EQ(gshscene_list_count(l), 0u);
    EXPECT_EQ(out.refusal, (uint32_t)GFNT_COLOR_LOWERED);
    EXPECT_EQ(out.outcome, (uint32_t)GFNT_COLOR_OUTCOME_LOWERED);
  }
  {
    // A paint format the table does not define is a damaged table: CORRUPT, not a gap.
    const Bytes base = file_bytes("color-v1-scene.ttf");
    Color probe(base);
    GFNT_PaintRef root = 0;
    ASSERT_EQ(gfnt_face_color_base_paint(probe.face, kPaintG, &root, nullptr), GFNT_OK);
    Bytes colr = colr_table(base);
    colr[root] = 99;
    Color bad(with_colr(base, colr));
    GSHSCENE_List * l = bad.open();
    GFNT_ColorLowering out{};
    EXPECT_EQ(bad.lower(l, kPaintG, &out), GFNT_ERR_CORRUPT);
    EXPECT_EQ(gshscene_list_count(l), 0u);
  }
  {
    GSHSCENE_List * l = c.open();
    GFNT_ColorRun run{};
    run.size = 1280;
    run.bits = 6;
    run.refusal_policy = GFNT_COLOR_ON_REFUSAL_COUNT;
    GFNT_ColorLowering out{};
    EXPECT_EQ(gfnt_scene_record_color_glyph(c.face_resource, c.store, l, kPaintG, &run, &out, nullptr), GFNT_ERR_INVALID);
    EXPECT_EQ(gshscene_list_count(l), 0u);
  }
}

TEST(SceneColor, ARefusedGlyphReachesTheRenderReportAndTheFallbackDrawsAndIsApproximated) {
  // The criterion P5 exists for: an application that draws the list and reads only the report learns the glyph is missing.
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * refused = c.open();
  GSHSCENE_List * fallback = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(refused, kPaintG, &out, 40 * 64, 4 * 64, 44 * 64), GFNT_OK);
  ASSERT_EQ(c.lower(fallback, kPaintG, &out, 40 * 64, 4 * 64, 44 * 64, 3, nullptr, GFNT_COLOR_SPACE_STANDARD,
                GFNT_COLOR_ON_REFUSAL_FALLBACK), GFNT_OK);
  ASSERT_EQ(out.outcome, (uint32_t)GFNT_COLOR_OUTCOME_FALLBACK) << "the base glyph has an outline";
  for (GSHSCENE_List * l : {refused, fallback}) {
    ASSERT_EQ(gshscene_list_close(l), GSHSCENE_OK);
  }
  auto draw = [&](GSHSCENE_List * l, uint32_t flags, GSHSCENE_Result * result, std::vector<GSHSCENE_Report_Entry> * entries,
                  std::vector<size_t> * chains) {
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
    EXPECT_EQ(gimg_raster_create(64, 64, &f, GIMG_RASTER_OWNED, nullptr, 0, &raster), GIMG_OK);
    uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
    std::memset(px, 0, gimg_raster_stride_bytes(raster) * 64u);
    GSHSCENE_Sink * sink = nullptr;
    EXPECT_EQ(gshcpu_sink_create(nullptr, nullptr, nullptr, &sink), GSHCPU_OK);
    GSHSCENE_Report * report = nullptr;
    EXPECT_EQ(gshscene_report_create(nullptr, nullptr, &report), GSHSCENE_OK);
    GSHSCENE_Target t{};
    t.kind = GSHSCENE_TARGET_RASTER;
    t.surface = raster;
    GSHSCENE_Render_Request rq{};
    rq.list = l;
    rq.target = &t;
    rq.report = report;
    rq.flags = flags;
    *result = gshscene_sink_render(sink, &rq);
    for (size_t i = 0; i < gshscene_report_count(report); i++) {
      GSHSCENE_Report_Entry e{};
      gshscene_report_entry(report, i, &e);
      chains->insert(chains->end(), e.chain, e.chain + e.chain_count);
      e.chain = nullptr;
      entries->push_back(e);
    }
    std::string pixels(reinterpret_cast<const char *>(px), gimg_raster_stride_bytes(raster) * 64u);
    gshscene_report_destroy(report);
    EXPECT_EQ(gshscene_sink_destroy(sink), GSHSCENE_OK);
    gimg_raster_destroy(raster);
    return pixels;
  };
  const size_t pair = ((size_t)GSHSCENE_PLACEHOLDER_SOURCE_COLR << 16) | GFNT_COLOR_REFUSED_UNBOUNDED;
  GSHSCENE_Result result;
  std::vector<GSHSCENE_Report_Entry> entries;
  std::vector<size_t> chains;
  // Best effort: nothing drawn, and the report says why.
  std::string pixels = draw(refused, 0, &result, &entries, &chains);
  EXPECT_EQ(result, GSHSCENE_OK);
  EXPECT_FALSE(any_ink(pixels));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].entry_class, GSHSCENE_ENTRY_REFUSED);
  EXPECT_EQ(entries[0].reason, GSHSCENE_REASON_PLACEHOLDER);
  EXPECT_EQ(entries[0].second, pair);
  EXPECT_STREQ(gshscene_placeholder_string((unsigned)(pair >> 16), (unsigned)(pair & 0xffffu)), "colr:unbounded");
  ASSERT_EQ(chains.size(), 1u);
  EXPECT_EQ(chains[0], 0u) << "the placeholder is the list's one item";
  // Strict fails before the sink is called.
  entries.clear();
  chains.clear();
  pixels = draw(refused, GSHSCENE_RENDER_STRICT, &result, &entries, &chains);
  EXPECT_EQ(result, GSHSCENE_ERR_REFUSED);
  EXPECT_FALSE(any_ink(pixels));
  // The fallback draws the glyph's outline in the foreground (opaque black here) and is approximated, which never fails.
  for (uint32_t flags : {0u, (uint32_t)GSHSCENE_RENDER_STRICT}) {
    entries.clear();
    chains.clear();
    pixels = draw(fallback, flags, &result, &entries, &chains);
    EXPECT_EQ(result, GSHSCENE_OK) << flags;
    EXPECT_TRUE(any_ink(pixels)) << flags << ": the monochrome outline";
    ASSERT_EQ(entries.size(), 1u) << flags;
    EXPECT_EQ(entries[0].entry_class, GSHSCENE_ENTRY_APPROXIMATED) << flags;
    EXPECT_EQ(entries[0].reason, GSHSCENE_REASON_PLACEHOLDER) << flags;
    EXPECT_EQ(entries[0].second, pair) << flags;
  }
}

TEST(SceneColor, WhatWas14csRefusalsIsNowLowered) {
  // color-v1.ttf's A (a gradient of each kind, its own clip box) and ghoti.alt (a multiply composite) and the scene
  // fixture's blend mode were refused as GRADIENT and BLEND_MODE.
  Color c("color-v1.ttf");
  Color s("color-v1-scene.ttf");
  GFNT_ColorLowering out{};
  GSHSCENE_List * a = c.open();
  ASSERT_EQ(c.lower(a, kA, &out), GFNT_OK);
  EXPECT_EQ(out.refusal, (uint32_t)GFNT_COLOR_LOWERED);
  size_t gradients = 0;
  for (size_t i = 0; i < gshscene_list_count(a); i++) {
    if (item_of(a, i)->kind == GSHSCENE_ITEM_FILL && item_of(a, i)->paint.kind == GSHSCENE_PAINT_GRADIENT) {
      gradients++;
    }
  }
  EXPECT_EQ(gradients, 3u) << "linear, radial and sweep";
  GSHSCENE_List * alt = c.open();
  ASSERT_EQ(c.lower(alt, kAlt, &out), GFNT_OK);
  EXPECT_EQ(out.refusal, (uint32_t)GFNT_COLOR_LOWERED);
  GSHSCENE_List * multiply = s.open();
  ASSERT_EQ(s.lower(multiply, kPaintF, &out), GFNT_OK);
}

TEST(SceneColor, AGraphThatNamesItselfIsARefusalOfTheGlyphAndLeavesTheListAsItWas) {
  // R4: the walk's bound is unchanged (a hostile font still cannot make the call allocate without bound), but a graph
  // past it is a property of the glyph: ::GFNT_COLOR_REFUSED_WALK_LIMIT, not the call's limit.
  Color c("color-v1-scene.ttf");
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kA, &out), GFNT_OK);
  EXPECT_EQ(c.lower_none(l, kPaintH, &out), GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(out.refusal, (uint32_t)GFNT_COLOR_REFUSED_WALK_LIMIT);
  EXPECT_EQ(out.items, 0u);
  EXPECT_EQ(gshscene_list_count(l), 5u);
}

TEST(SceneColor, AGraphDeeperThanTheWalksBoundIsRefusedAndAShallowerOneIsLowered) {
  // Built here rather than as a fixture: the paint-graph differential (color_diff.py) walks every fixture with a
  // reference that has no depth bound and no answer for a glyph with no record.
  const Bytes base = file_bytes("color-v1-scene.ttf");
  for (size_t levels : {10u, 64u, 65u, 70u}) {
    Color c(with_colr(base, colr_nested(kA, kB, kLayerA, levels)));
    GSHSCENE_List * l = c.open();
    GFNT_ColorLowering out{};
    // The root is depth 0, layer k depth k + 1, the glyph's solid one deeper: a bound of 64 takes 63 levels.
    GFNT_Result r = c.lower_none(l, kB, &out);
    if (levels + 1 <= 64) {
      ASSERT_EQ(r, GFNT_OK) << levels;
      ASSERT_EQ(gshscene_list_count(l), 3u);
      expect_fill(l, 1, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "the deepest layer");
    } else {
      EXPECT_EQ(r, GFNT_ERR_UNSUPPORTED) << levels;
      EXPECT_EQ(out.refusal, (uint32_t)GFNT_COLOR_REFUSED_WALK_LIMIT) << levels;
      EXPECT_EQ(gshscene_list_count(l), 0u);
    }
    // A color glyph naming a glyph with no version 1 record.
    std::memset(&out, 0, sizeof out);
    EXPECT_EQ(c.lower_none(l, kA, &out), GFNT_ERR_UNSUPPORTED);
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
  ASSERT_EQ(gshscene_list_count(l), 3u);
  expect_fill(l, 1, kLayerA, GSHSCENE_PAINT_PALETTE, 0, 1.0, "version 1's glyph and entry, not version 0's");
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
  EXPECT_EQ(gshscene_list_count(l), 8u);
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
      if (item->kind == GSHSCENE_ITEM_GROUP_PUSH) {
        ASSERT_EQ(gshscene_list_group_push(twin, &item->group, item->layer, 0.0), GSHSCENE_OK);
        continue;
      }
      if (item->kind == GSHSCENE_ITEM_GROUP_POP) {
        ASSERT_EQ(gshscene_list_group_pop(twin), GSHSCENE_OK);
        continue;
      }
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

// --- the lowering's own seams, each seen to fail (the planted-defect pass) ---------------------------------------------

namespace {

void put_i16(Bytes & out, int v) {
  gfnttest::put_u16(out, (uint16_t)(int16_t)v);
}

/** PaintSolid: entry @p entry at alpha @p alpha (F2.14). */
Bytes p_solid(uint16_t entry, uint16_t alpha = 16384) {
  Bytes b{2};
  gfnttest::put_u16(b, entry);
  gfnttest::put_u16(b, alpha);
  return b;
}

/** PaintGlyph of @p glyph over @p child (which follows it). */
Bytes p_glyph(uint16_t glyph, const Bytes & child) {
  Bytes b{10};
  put24(b, 6);
  gfnttest::put_u16(b, glyph);
  b.insert(b.end(), child.begin(), child.end());
  return b;
}

/** PaintTranslate by (@p dx, @p dy) over @p child. */
Bytes p_translate(int dx, int dy, const Bytes & child) {
  Bytes b{14};
  put24(b, 8);
  put_i16(b, dx);
  put_i16(b, dy);
  b.insert(b.end(), child.begin(), child.end());
  return b;
}

/** PaintLinearGradient with a two-stop color line of extend byte @p extend (0 pad, 1 repeat, 2 reflect). */
Bytes p_linear(uint8_t extend) {
  Bytes b{4};
  put24(b, 16);
  for (int v : {0, 0, 600, 0, 0, 600}) {
    put_i16(b, v);
  }
  b.push_back(extend);
  gfnttest::put_u16(b, 2);
  for (uint16_t offset : {0, 16384}) {
    gfnttest::put_u16(b, offset);
    gfnttest::put_u16(b, offset ? 1 : 0);
    gfnttest::put_u16(b, 16384);
  }
  return b;
}

/** PaintVarRotateAroundCenter: angle 0 and a center of (0, 0), its three numbers at variation index @p index. */
Bytes p_var_rotate(uint32_t index, const Bytes & child) {
  Bytes b{27};
  put24(b, 14);
  put_i16(b, 0);
  put_i16(b, 0);
  put_i16(b, 0);
  gfnttest::put_u32(b, index);
  b.insert(b.end(), child.begin(), child.end());
  return b;
}

/** PaintColrLayers over the layer list's entries @p first and on. */
Bytes p_layers(uint8_t count, uint32_t first) {
  Bytes b{1, count};
  gfnttest::put_u32(b, first);
  return b;
}

/**
 * A `COLR` version 1 table, built here, whose one base glyph @p glyph is @p root; @p layers are paints a
 * `PaintColrLayers` names; @p store, where there is one, is the item variation store (no index map).
 */
Bytes colr_graph(uint16_t glyph, const Bytes & root, const std::vector<Bytes> & layers = {}, const Bytes & store = {}) {
  const uint32_t list = 34;
  const uint32_t layer_list = list + 10;
  const uint32_t layer_bytes = layers.empty() ? 0 : 4 + 4 * (uint32_t)layers.size();
  Bytes paints;
  const uint32_t root_at = (uint32_t)paints.size();
  paints.insert(paints.end(), root.begin(), root.end());
  std::vector<uint32_t> layer_at;
  for (const Bytes & layer : layers) {
    layer_at.push_back((uint32_t)paints.size());
    paints.insert(paints.end(), layer.begin(), layer.end());
  }
  const uint32_t blob = layer_list + layer_bytes;
  Bytes out;
  gfnttest::put_u16(out, 1);                       // version
  gfnttest::put_u16(out, 0);                       // no version 0 records
  gfnttest::put_u32(out, 0);
  gfnttest::put_u32(out, 0);
  gfnttest::put_u16(out, 0);
  gfnttest::put_u32(out, list);
  gfnttest::put_u32(out, layers.empty() ? 0 : layer_list);
  gfnttest::put_u32(out, 0);                       // no clip list
  gfnttest::put_u32(out, 0);                       // no index map
  gfnttest::put_u32(out, store.empty() ? 0 : blob + (uint32_t)paints.size());
  gfnttest::put_u32(out, 1);                       // BaseGlyphList: one record
  gfnttest::put_u16(out, glyph);
  gfnttest::put_u32(out, 10 + layer_bytes + root_at);
  if (!layers.empty()) {
    gfnttest::put_u32(out, (uint32_t)layers.size());
    for (uint32_t at : layer_at) {
      gfnttest::put_u32(out, layer_bytes + at);
    }
  }
  out.insert(out.end(), paints.begin(), paints.end());
  out.insert(out.end(), store.begin(), store.end());
  return out;
}

}  // namespace

TEST(SceneColor, AGradientsTransformsAboveItsGlyphsClipAreTheFillsAndTheOnesBelowAreTheGradients) {
  // translate (50, 0) over a glyph whose child is a layer list of translate (0, 70) over a gradient: the glyph's child is not a leaf, so
  // the glyph is a clip, and the first translation is above the clip (the fill's) and the second below it (L4).
  const Bytes base = file_bytes("color-v1-scene.ttf");
  Color c(with_colr(base, colr_graph(kA, p_translate(50, 0, p_glyph(kLayerA, p_layers(1, 0))),
      {p_translate(0, 70, p_linear(0))})));
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kA, &out), GFNT_OK);
  ASSERT_EQ(out.outcome, (uint32_t)GFNT_COLOR_OUTCOME_LOWERED);
  const double s = 20.0 / c.upem;
  const GSHSCENE_Item * fill = nullptr;
  for (size_t i = 0; i < gshscene_list_count(l); i++) {
    const GSHSCENE_Item * item = item_of(l, i);
    if (item->kind == GSHSCENE_ITEM_FILL) {
      fill = item;
    }
  }
  ASSERT_NE(fill, nullptr);
  ASSERT_NE(plane_resource_of(fill), nullptr);
  EXPECT_EQ(fill->map.t[0], s * 50.0) << "the translation above the clip is the fill's map";
  GSHSCENE_Gradient g = gradient_of(fill);
  GSHGEOM_Point q = mapped(g.map, 0.0, 0.0);
  EXPECT_NEAR(q.x, 10.0, 1e-9) << "and is not in the gradient's";
  EXPECT_NEAR(q.y, 30.0 - s * 70.0, 1e-9) << "the translation below the clip is the gradient's";
}

TEST(SceneColor, ABoundEndsWithItsGlyphsClipAndASiblingAfterItIsUnbounded) {
  // Two layers: a glyph with a clip (its child is a layer list, so the glyph cannot be a leaf's), then a bare solid.  The solid is outside
  // the clip and the font states no box, so nothing bounds it (L5).
  const Bytes base = file_bytes("color-v1-scene.ttf");
  const std::vector<Bytes> layers = {p_glyph(kLayerA, p_layers(1, 2)), p_solid(1), p_solid(0)};
  Color c(with_colr(base, colr_graph(kA, p_layers(2, 0), layers)));
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower_none(l, kA, &out), GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(out.refusal, (uint32_t)GFNT_COLOR_REFUSED_UNBOUNDED);
  EXPECT_EQ(out.paint_format, (uint32_t)GFNT_PAINT_SOLID);
  EXPECT_EQ(gshscene_list_count(l), 0u);
}

TEST(SceneColor, AnUnknownColorLineExtendIsPadAndNotTheLastKnownOne) {
  const Bytes base = file_bytes("color-v1-scene.ttf");
  for (uint8_t extend : {uint8_t{0}, uint8_t{1}, uint8_t{2}, uint8_t{3}, uint8_t{255}}) {
    Color c(with_colr(base, colr_graph(kA, p_glyph(kLayerA, p_linear(extend)))));
    GSHSCENE_List * l = c.open();
    GFNT_ColorLowering out{};
    const GFNT_Result r = c.lower_none(l, kA, &out);
    if (extend > 2) {
      // The reader may refuse a color line it cannot read; if it hands one back, it is pad.
      if (r != GFNT_OK) {
        continue;
      }
    }
    ASSERT_EQ(r, GFNT_OK) << (int)extend;
    GSHSCENE_Gradient g{};
    bool found = false;
    for (size_t i = 0; i < gshscene_list_count(l) && !found; i++) {
      const GSHSCENE_Item * item = item_of(l, i);
      if (item->kind == GSHSCENE_ITEM_FILL && item->paint.kind == GSHSCENE_PAINT_GRADIENT) {
        g = gradient_of(item);
        found = true;
      }
    }
    ASSERT_TRUE(found) << (int)extend;
    const uint32_t want = extend == 1 ? GSHSCENE_EXTEND_REPEAT : extend == 2 ? GSHSCENE_EXTEND_REFLECT : GSHSCENE_EXTEND_PAD;
    EXPECT_EQ(g.extend, want) << (int)extend;
  }
}

TEST(SceneColor, ANumberTimesItsScaleThatDoesNotFitIsAnErrorAndNeverAWrappedValue) {
  // color-v1-var.ttf's face, a rotation about a center whose x is moved by a 32-bit delta of two million at the
  // instance: times 2^15 that is past 32 bits (a variation's delta can be any size, L1).
  Bytes store;
  gfnttest::put_u16(store, 1);                     // ItemVariationStore format 1
  gfnttest::put_u32(store, 12);                    // the region list
  gfnttest::put_u16(store, 1);                     // one ItemVariationData
  gfnttest::put_u32(store, 28);
  gfnttest::put_u16(store, 2);                     // RegionList: two axes, one region
  gfnttest::put_u16(store, 1);
  for (int v : {0, 16384, 16384, 0, 0, 0}) {       // axis 0 from 0 to 1, peak 1; axis 1 not used
    put_i16(store, v);
  }
  gfnttest::put_u16(store, 3);                     // ItemVariationData: three rows
  gfnttest::put_u16(store, 0x8001);                // one 32-bit delta each
  gfnttest::put_u16(store, 1);
  gfnttest::put_u16(store, 0);
  for (uint32_t d : {0u, 2000000u, 0u}) {
    gfnttest::put_u32(store, d);
  }
  const Bytes base = file_bytes("color-v1-var.ttf");
  Bytes root = p_var_rotate(0, p_glyph(kLayerA, p_solid(0)));
  Color c(with_colr(base, colr_graph(kA, root, {}, store)));
  GSHSCENE_List * l = c.open();
  const GSHSCENE_Resource * moved = c.instance({16384, 0});
  GFNT_ColorLowering out{};
  EXPECT_EQ(c.lower(l, kA, &out, 20 * 64, 10 * 64, 30 * 64, 3, nullptr), GFNT_OK) << "at the default the numbers fit";
  GSHSCENE_List * m = c.open();
  const size_t before = gshscene_list_count(m);
  GFNT_Error error{};
  GFNT_ColorRun run{};
  run.size = 1280;
  run.bits = 6;
  run.hint_mode = GSHSCENE_HINT_NONE;
  run.refusal_policy = GFNT_COLOR_ON_REFUSAL_NONE;
  EXPECT_EQ(gfnt_scene_record_color_glyph(moved, c.store, m, kA, &run, &out, &error), GFNT_ERR_LIMIT);
  EXPECT_EQ(gshscene_list_count(m), before) << "nothing is recorded";
}

TEST(SceneColor, AColorGlyphsListLeavesNothingHeldInTheStoreOnceItIsDestroyed) {
  // The plane a root gradient fills is one reference the lowering takes and gives back: after the list is gone the store
  // holds what it held before.
  Color c("color-v1-scene.ttf");
  GSHSCENE_Store_Stats before{};
  ASSERT_EQ(gshscene_store_stats(c.store, &before), GSHSCENE_OK);
  GSHSCENE_List * l = nullptr;
  ASSERT_EQ(gshscene_list_create(nullptr, nullptr, c.store, &l), GSHSCENE_OK);
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kPaintO, &out), GFNT_OK);
  ASSERT_NE(plane_resource_of(item_of(l, 2)), nullptr) << "a plane is in the list";
  GSHSCENE_Store_Stats during{};
  ASSERT_EQ(gshscene_store_stats(c.store, &during), GSHSCENE_OK);
  EXPECT_GT(during.live, before.live);
  gshscene_list_destroy(l);
  GSHSCENE_Store_Stats after{};
  ASSERT_EQ(gshscene_store_stats(c.store, &after), GSHSCENE_OK);
  EXPECT_EQ(after.live, before.live) << "no resource of the lowering is still held";
}

TEST(SceneColor, AVariableClipBoxAndColorStopsAreReadAtTheFaceResourcesInstance) {
  Color c("color-v1-var.ttf");
  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(c.face, &glyphs, nullptr), GFNT_OK);
  uint32_t first = 0;
  GFNT_PaintRef root = 0;
  for (uint32_t g = 0; g < glyphs && !first; g++) {
    if (gfnt_face_color_base_paint(c.face, g, &root, nullptr) == GFNT_OK && root) {
      first = g;
    }
  }
  ASSERT_NE(first, 0u);
  GFNT_F2Dot14 coords[2] = {16384, -8192};
  const GFNT_Variation v{coords, 2, GFNT_DELTA_ROUND_HALF_UP};
  const GSHSCENE_Resource * moved = c.instance({16384, -8192});

  // The clip box the font states for the glyph, at the default and at the instance.
  GFNT_ColorBox want_default{}, want_moved{};
  bool found = false;
  ASSERT_EQ(gfnt_face_color_clip(c.face, first, nullptr, &want_default, &found, nullptr), GFNT_OK);
  ASSERT_TRUE(found);
  ASSERT_EQ(gfnt_face_color_clip(c.face, first, &v, &want_moved, &found, nullptr), GFNT_OK);
  ASSERT_TRUE(found);
  ASSERT_TRUE(want_default.x_max != want_moved.x_max || want_default.x_min != want_moved.x_min ||
      want_default.y_min != want_moved.y_min || want_default.y_max != want_moved.y_max)
      << "the fixture's clip box varies";
  auto clip_bounds = [&](const GSHSCENE_Resource * face) {
    GSHSCENE_List * l = c.open();
    GFNT_ColorLowering out{};
    EXPECT_EQ(c.lower(l, first, &out, 20 * 64, 10 * 64, 30 * 64, 3, face), GFNT_OK);
    GSHGEOM_Bounds b{};
    bool seen = false;
    for (size_t i = 0; i < gshscene_list_count(l) && !seen; i++) {
      const GSHSCENE_Item * item = item_of(l, i);
      const GSHGEOM_Path * box = item->kind == GSHSCENE_ITEM_CLIP_PUSH ? gshscene_resource_path(item->resource) : nullptr;
      if (box) {
        EXPECT_EQ(gshgeom_path_bounds(box, &b), GSHGEOM_OK);
        seen = true;
      }
    }
    EXPECT_TRUE(seen);
    return b;
  };
  const GSHGEOM_Bounds at_default = clip_bounds(nullptr);
  const GSHGEOM_Bounds at_moved = clip_bounds(moved);
  EXPECT_EQ(at_default.max.x, (double)want_default.x_max);
  EXPECT_EQ(at_default.min.y, (double)want_default.y_min);
  EXPECT_EQ(at_moved.min.x, (double)want_moved.x_min);
  EXPECT_EQ(at_moved.min.y, (double)want_moved.y_min);
  EXPECT_EQ(at_moved.max.x, (double)want_moved.x_max);
  EXPECT_EQ(at_moved.max.y, (double)want_moved.y_max);

  // The color stops of the glyph's first gradient, at the instance.
  struct Find {
    GFNT_ColorPaint paint{};
    bool found = false;
  } f;
  struct W {
    static bool enter(void * user, const GFNT_ColorPaint * p, size_t) {
      Find * f = static_cast<Find *>(user);
      if (!f->found && (p->format == GFNT_PAINT_LINEAR_GRADIENT || p->format == GFNT_PAINT_VAR_LINEAR_GRADIENT)) {
        f->found = true;
        f->paint = *p;
      }
      return true;
    }
  };
  GFNT_PaintVisitor vis{W::enter, nullptr};
  ASSERT_EQ(gfnt_face_color_paint_walk(c.face, first, &v, &vis, &f, nullptr), GFNT_OK);
  ASSERT_TRUE(f.found);
  GFNT_ColorLine line{};
  ASSERT_EQ(gfnt_face_color_line(c.face, &f.paint, &line, nullptr), GFNT_OK);
  ASSERT_EQ(line.stop_count, 2u);
  GFNT_ColorStop s_default[2], s_moved[2];
  bool varies = false;
  for (size_t i = 0; i < 2; i++) {
    ASSERT_EQ(gfnt_face_color_stop(c.face, &f.paint, i, nullptr, &s_default[i], nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_color_stop(c.face, &f.paint, i, &v, &s_moved[i], nullptr), GFNT_OK);
    varies = varies || s_default[i].offset != s_moved[i].offset || s_default[i].alpha != s_moved[i].alpha;
  }
  ASSERT_TRUE(varies) << "the fixture's color stops vary";
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, first, &out, 20 * 64, 10 * 64, 30 * 64, 3, moved), GFNT_OK);
  bool checked = false;
  for (size_t i = 0; i < gshscene_list_count(l) && !checked; i++) {
    const GSHSCENE_Item * item = item_of(l, i);
    if (item->kind == GSHSCENE_ITEM_FILL && item->paint.kind == GSHSCENE_PAINT_GRADIENT) {
      GSHSCENE_Gradient g{};
      ASSERT_EQ(gshscene_resource_gradient(item->paint.resource, &g), GSHSCENE_OK);
      if (g.kind == GSHSCENE_GRADIENT_LINEAR) {
        ASSERT_EQ(g.stop_count, 2u);
        // Sorted by offset, which the deltas are small enough not to reorder.
        for (size_t k = 0; k < 2; k++) {
          EXPECT_EQ(g.stops[k].offset, s_moved[k].offset / 16384.0) << k;
          EXPECT_EQ(g.stops[k].color.rgba[3], s_moved[k].alpha / 16384.0) << k;
        }
        checked = true;
      }
    }
  }
  EXPECT_TRUE(checked);
}

TEST(SceneColor, APlaceholdersBoxIsAClipBoxEvenOfNoSizeAndTheExtentsAreTheFontsAndNotTheOutlines) {
  const Bytes base = file_bytes("color-v1-scene.ttf");
  // A clip box of no width and no height is still the font's box: the extents would pass it over.
  {
    static const int16_t box[4] = {7, 9, 7, 9};
    Color c(with_colr(base, colr_clipped(kA, kLayerA, box)));
    GSHSCENE_List * l = c.open();
    GFNT_ColorLowering out{};
    ASSERT_EQ(c.lower(l, kA, &out), GFNT_OK);
    const GSHSCENE_Item * item = item_of(l, 0);
    ASSERT_EQ(item->placeholder.has_box, 1u);
    EXPECT_EQ(item->placeholder.box[0], 7.0);
    EXPECT_EQ(item->placeholder.box[1], 9.0);
    EXPECT_EQ(item->placeholder.box[2], 7.0);
    EXPECT_EQ(item->placeholder.box[3], 9.0);
  }
  // No clip box: the glyph's extents, which are the box its header states and the side bearing `hmtx` states.  In the
  // fixture they are the outline's own box, so a header that says 33 more at the top and a bearing 55 more than the
  // outline's left edge are what tell the two apart.
  Color plain(with_colr(base, colr_nested(kA, kB, kLayerA, 3)));
  GFNT_Box control;
  ASSERT_TRUE(control_box_of(plain, kA, &control));
  GFNT_Extents e{};
  ASSERT_TRUE(gfnt_glyph_extents(plain.face, kA, nullptr, &e));
  ASSERT_EQ(e.x_bearing, (int32_t)(control.x_min >> 6)) << "the fixture's bearing is its outline's left edge";
  ASSERT_EQ(e.y_bearing, (int32_t)(control.y_max >> 6)) << "and its header's top is its outline's";
  Bytes moved = with_colr(base, colr_nested(kA, kB, kLayerA, 3));
  auto table_at = [&](uint32_t tag) {
    const size_t count = (size_t)moved[4] << 8 | moved[5];
    for (size_t i = 0; i < count; i++) {
      if (be32(moved, 12 + 16 * i) == tag) {
        return (size_t)be32(moved, 12 + 16 * i + 8);
      }
    }
    return (size_t)0;
  };
  const size_t hmtx = table_at(GFNT_TAG('h', 'm', 't', 'x'));
  const size_t hhea = table_at(GFNT_TAG('h', 'h', 'e', 'a'));
  const size_t head = table_at(GFNT_TAG('h', 'e', 'a', 'd'));
  const size_t loca = table_at(GFNT_TAG('l', 'o', 'c', 'a'));
  const size_t glyf = table_at(GFNT_TAG('g', 'l', 'y', 'f'));
  ASSERT_NE(hmtx, 0u);
  ASSERT_NE(hhea, 0u);
  ASSERT_NE(head, 0u);
  ASSERT_NE(loca, 0u);
  ASSERT_NE(glyf, 0u);
  const size_t long_metrics = (size_t)moved[hhea + 34] << 8 | moved[hhea + 35];
  ASSERT_GT(long_metrics, (size_t)kA);
  const size_t at = hmtx + 4 * kA + 2;
  const int16_t old_bearing = (int16_t)((moved[at] << 8) | moved[at + 1]);
  const int16_t new_bearing = (int16_t)(old_bearing + 55);
  moved[at] = (uint8_t)((uint16_t)new_bearing >> 8);
  moved[at + 1] = (uint8_t)new_bearing;
  const bool long_loca = moved[head + 51] != 0;
  const size_t offset = long_loca ? be32(moved, loca + 4 * kA)
                                  : 2 * ((size_t)moved[loca + 2 * kA] << 8 | moved[loca + 2 * kA + 1]);
  const size_t top = glyf + offset + 8;   // yMax of the glyph header
  const int16_t old_top = (int16_t)((moved[top] << 8) | moved[top + 1]);
  const int16_t new_top = (int16_t)(old_top + 33);
  moved[top] = (uint8_t)((uint16_t)new_top >> 8);
  moved[top + 1] = (uint8_t)new_top;
  Color c(moved);
  GSHSCENE_List * l = c.open();
  GFNT_ColorLowering out{};
  ASSERT_EQ(c.lower(l, kA, &out), GFNT_OK);
  const GSHSCENE_Item * item = item_of(l, 0);
  ASSERT_EQ(item->placeholder.has_box, 1u);
  GFNT_Extents moved_extents{};
  ASSERT_TRUE(gfnt_glyph_extents(c.face, kA, nullptr, &moved_extents));
  ASSERT_EQ(moved_extents.x_bearing, e.x_bearing + 55);
  ASSERT_EQ(moved_extents.y_bearing, e.y_bearing + 33);
  EXPECT_EQ(item->placeholder.box[0], (double)moved_extents.x_bearing);
  EXPECT_EQ(item->placeholder.box[1], (double)(moved_extents.y_bearing + moved_extents.height));
  EXPECT_EQ(item->placeholder.box[2], (double)(moved_extents.x_bearing + moved_extents.width));
  EXPECT_EQ(item->placeholder.box[3], (double)moved_extents.y_bearing);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
