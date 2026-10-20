/**
 * @file
 *
 * This library as `shape-scene`'s glyph path provider (story 14b of the shape
 * family's first milestone; scene.h, design.md section 8.7), through
 * `shape-cpu`, which draws a run from a font by both routes.
 *
 * - **AD-5's gate.** With hinting off, the fast tier (this provider's device
 *   paths) renders byte for byte what the general route renders under the
 *   snapped map (the size snapped to the 16.16 scale, each origin rounded to
 *   26.6, under a quarter turn and its inverse so that the map's tag sends the
 *   run to the general route), for every glyph of every committed outline
 *   fixture and a 2,048-unit face of ties, at 8 to 72 ppem, with each glyph at
 *   every pair of origin fractions {0, 1, 16, 33, 63} over the sizes; and the
 *   fast tier is taken (the provider's fast count is the glyph count).
 * - **AD-1's gate.** `gfnt_face_render_glyph`'s coverage is a one-glyph scene's
 *   alpha, pixel for pixel, in every mode.
 * - Hinting reaches the waist; strict hinting refuses a glyph; a corrupt glyph is
 *   that glyph's refusal; instances; the binding's lifetime; the hint-context
 *   LRU and its bound; threads; R1's identity; hostile fonts.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>
#include <ghoti.io/font/scene.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/shape-cpu/render.h>
#include <ghoti.io/shape-geometry/device.h>
#include <ghoti.io/shape-geometry/map.h>
#include <ghoti.io/shape-geometry/path.h>
#include <ghoti.io/shape-scene/glyph.h>
#include <ghoti.io/shape-scene/list.h>
#include <ghoti.io/shape-scene/report.h>
#include <ghoti.io/shape-scene/resource.h>
#include <ghoti.io/shape-scene/sink.h>

namespace {

using Bytes = std::vector<uint8_t>;

const uint32_t kFractions[5] = {0, 1, 16, 33, 63};

/** The committed outline fixtures (tools/golden/make_golden.py's WITH_OUTLINES). */
const char * const kOutlineFixtures[] = {
    "basic.ttf", "outline-simple.ttf", "outline-composite.ttf", "outline-loca-long.ttf",
    "outline-cubic.ttf", "outline-cubic-flag.ttf", "outline-broken-loca.ttf", "cff-curves.otf",
    "cff-type1.otf", "cff-arith.otf", "cff-cid.otf", "cff-hints.otf", "cff-seac.otf",
    "cff-subrs.otf", "type1-big.pfb",
};

/** A font held in memory, loaded, with a scene font over it. */
struct Font {
  Bytes bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_SceneFont * scene = nullptr;
  uint16_t upem = 0;
  size_t glyphs = 0;

  explicit Font(Bytes b, const GFNT_Limits * limits = nullptr) : bytes(std::move(b)) {
    GFNT_Error error{};
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED, limits,
        nullptr, &blob, &error), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, limits, nullptr, &face, &error), GFNT_OK)
        << (error.message ? error.message : "");
    if (face) {
      EXPECT_EQ(gfnt_scene_font_create(face, &scene, &error), GFNT_OK)
          << (error.message ? error.message : "");
      gfnt_face_units_per_em(face, &upem, nullptr);
      gfnt_face_num_glyphs(face, &glyphs, nullptr);
    }
  }
  ~Font() {
    gfnt_scene_font_destroy(scene);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;

  GFNT_SceneStats stats() const {
    GFNT_SceneStats s{};
    EXPECT_EQ(gfnt_scene_font_stats(scene, &s), GFNT_OK);
    return s;
  }
};

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

/** A store and what it holds; released in order. */
struct Store {
  GSHSCENE_Store * store = nullptr;
  std::vector<const GSHSCENE_Resource *> held;
  std::vector<GSHSCENE_List *> lists;

  Store() { EXPECT_EQ(gshscene_store_create(nullptr, nullptr, &store), GSHSCENE_OK); }
  ~Store() {
    for (GSHSCENE_List * l : lists) {
      gshscene_list_destroy(l);
    }
    for (auto it = held.rbegin(); it != held.rend(); ++it) {
      gshscene_resource_release(*it);
    }
    gshscene_store_destroy(store);
  }
  Store(const Store &) = delete;
  Store & operator=(const Store &) = delete;

  const GSHSCENE_Resource * face(Font & font, const std::vector<GFNT_F2Dot14> & coords = {}) {
    const GSHSCENE_Resource * f = nullptr;
    GFNT_Variation v{};
    v.coords = coords.empty() ? nullptr : coords.data();
    v.count = coords.size();
    GFNT_Error error{};
    EXPECT_EQ(gfnt_scene_intern_face(font.scene, store, coords.empty() ? nullptr : &v, &f, &error),
        GFNT_OK) << (error.message ? error.message : "");
    if (f) {
      held.push_back(f);
    }
    return f;
  }

  const GSHSCENE_Resource * run(const GSHSCENE_Resource * face, double size, uint32_t mode,
      const std::vector<GSHSCENE_Glyph> & glyphs, uint32_t version = 0) {
    GSHSCENE_Glyph_Run r{};
    r.source = GSHSCENE_GLYPHS_FROM_FONT;
    r.face = face;
    r.size = size;
    r.hint_mode = mode;
    r.hint_version = version;
    r.glyph_count = glyphs.size();
    r.glyphs = glyphs.data();
    const GSHSCENE_Resource * out = nullptr;
    EXPECT_EQ(gshscene_store_intern_glyph_run(store, &r, &out), GSHSCENE_OK);
    if (out) {
      held.push_back(out);
    }
    return out;
  }

  /** A closed list filling @p run under @p map with opaque white. */
  GSHSCENE_List * fill(const GSHSCENE_Resource * run, const GSHGEOM_Map * map) {
    GSHSCENE_List * l = nullptr;
    EXPECT_EQ(gshscene_list_create(nullptr, nullptr, store, &l), GSHSCENE_OK);
    lists.push_back(l);
    GSHSCENE_Paint ink{};
    ink.kind = GSHSCENE_PAINT_SOLID;
    ink.rgba[0] = ink.rgba[1] = ink.rgba[2] = ink.rgba[3] = 1.0;
    EXPECT_EQ(gshscene_list_fill(l, run, map, GSHGEOM_FILL_NONZERO, &ink, 0, 0.0), GSHSCENE_OK);
    EXPECT_EQ(gshscene_list_close(l), GSHSCENE_OK);
    return l;
  }
};

/** A render's raster (RGBA8 straight, from transparent), its report and its result. */
struct Rendered {
  std::string pixels;
  uint32_t width = 0;
  GSHSCENE_Result result = GSHSCENE_ERR_INTERNAL;
  std::vector<GSHSCENE_Report_Entry> entries;

  uint8_t alpha(uint32_t x, uint32_t y) const {
    return static_cast<uint8_t>(pixels[(static_cast<size_t>(y) * width + x) * 4u + 3u]);
  }
};

Rendered render(const GSHSCENE_List * list, uint32_t w, uint32_t h, uint32_t flags = 0) {
  Rendered out;
  out.width = w;
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
  if (!raster) {
    return out;
  }
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
  rq.flags = flags;
  out.result = gshscene_sink_render(sink, &rq);
  for (size_t i = 0; i < gshscene_report_count(report); i++) {
    GSHSCENE_Report_Entry e{};
    gshscene_report_entry(report, i, &e);
    e.chain = nullptr;   // not valid past the report
    out.entries.push_back(e);
  }
  for (uint32_t y = 0; y < h; y++) {
    out.pixels.append(reinterpret_cast<const char *>(px + y * stride), static_cast<size_t>(w) * 4u);
  }
  gshscene_report_destroy(report);
  EXPECT_EQ(gshscene_sink_destroy(sink), GSHSCENE_OK);
  gimg_raster_destroy(raster);
  return out;
}

bool blank(const Rendered & r) {
  for (char c : r.pixels) {
    if (c != 0) {
      return false;
    }
  }
  return true;
}

GSHGEOM_Map tagged_identity() {
  GSHGEOM_Map a;
  GSHGEOM_Map b;
  gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, 90.0, &a);
  gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, -90.0, &b);
  gshgeom_map_compose(&a, &b, &a);
  return a;
}

// --- built fonts --------------------------------------------------------------

/** A 2,048-unit face whose points are odd units: every one a tie at 16 ppem. */
Bytes tie_font() {
  std::vector<Bytes> glyphs = {
      {},
      gfnttest::build_glyf_glyph({{{-3, 3, true}, {1021, 3, true}, {1021, 1501, true},
          {-3, 1501, true}}, {{301, 301, true}, {301, 1203, true}, {723, 1203, true},
          {723, 301, true}}}),
      gfnttest::build_glyf_glyph({{{1023, 1, true}, {1771, 1, false}, {1771, 1501, false},
          {275, 1501, false}, {275, -201, false}}}),
      gfnttest::build_glyf_glyph({{{0, 0, true}, {202, 101, false}, {606, 505, false},
          {808, 0, true}}}),
  };
  Bytes glyf;
  Bytes loca;
  bool long_loca = false;
  gfnttest::build_glyf_and_loca(glyphs, &glyf, &loca, &long_loca);
  return gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(2048, long_loca ? 1 : 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(1800, -400, 0, 4)},
      {GFNT_TAG('h', 'm', 't', 'x'),
          gfnttest::build_hmtx({{1024, 0}, {1031, -3}, {1800, 275}, {1025, 0}}, {})},
      {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(4)},
      {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
      {GFNT_TAG('l', 'o', 'c', 'a'), loca},
  });
}

/** Round every point of a four-point glyph to the grid, in x then in y. */
Bytes round_four() {
  return {0x01, 0xB3, 0, 1, 2, 3, 0x2F, 0x2F, 0x2F, 0x2F,
          0x00, 0xB3, 0, 1, 2, 3, 0x2F, 0x2F, 0x2F, 0x2F};
}

/**
 * A 1,000-unit TrueType face with glyph programs: 1 is a rectangle its program
 * rounds to the grid (HINTED); 2 the same rectangle whose program rounds its
 * first point in y and then pops an empty stack (it fails: WITH_ERRORS, the
 * point moved, when tolerant; PROGRAM_FAILED when strict); 3 the rectangle with
 * no program (NO_PROGRAM); 4 a glyph whose bytes stop after its header
 * (corrupt). With @p gasp, a `gasp` that grid-fits from 12 ppem only.
 */
Bytes hinted_font(bool gasp = false) {
  const std::vector<std::vector<gfnttest::GlyfPoint>> rect = {
      {{101, 103, true}, {613, 103, true}, {613, 707, true}, {101, 707, true}}};
  Bytes corrupt;
  gfnttest::put_s16(corrupt, 1);
  for (int i = 0; i < 4; i++) {
    gfnttest::put_s16(corrupt, 16);
  }
  std::vector<Bytes> glyphs = {
      {},
      gfnttest::build_glyf_glyph(rect, round_four()),
      gfnttest::build_glyf_glyph(rect, {0x00, 0xB0, 0, 0x2F, 0x2F}),
      gfnttest::build_glyf_glyph(rect),
      corrupt,
  };
  Bytes glyf;
  Bytes loca;
  bool long_loca = false;
  gfnttest::build_glyf_and_loca(glyphs, &glyf, &loca, &long_loca);
  Bytes maxp = gfnttest::build_maxp(5);
  gfnttest::patch_u16(maxp, 14, 2);    // zones
  gfnttest::patch_u16(maxp, 16, 8);    // twilight points
  gfnttest::patch_u16(maxp, 18, 8);    // storage
  gfnttest::patch_u16(maxp, 20, 4);    // functions
  gfnttest::patch_u16(maxp, 22, 2);    // instruction definitions
  gfnttest::patch_u16(maxp, 24, 64);   // stack
  gfnttest::patch_u16(maxp, 26, 32);   // the longest glyph program
  std::vector<gfnttest::Table> tables = {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, long_loca ? 1 : 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, 5)},
      {GFNT_TAG('h', 'm', 't', 'x'),
          gfnttest::build_hmtx({{500, 0}, {700, 101}, {700, 101}, {700, 101}, {700, 0}}, {})},
      {GFNT_TAG('m', 'a', 'x', 'p'), maxp},
      {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
      {GFNT_TAG('l', 'o', 'c', 'a'), loca},
  };
  if (gasp) {
    Bytes table;
    gfnttest::put_u16(table, 1);        // version
    gfnttest::put_u16(table, 2);        // ranges
    gfnttest::put_u16(table, 11);       // to 11 ppem: no grid-fitting
    gfnttest::put_u16(table, 0x0002);
    gfnttest::put_u16(table, 0xFFFF);   // above: grid-fit
    gfnttest::put_u16(table, 0x0003);
    tables.push_back({GFNT_TAG('g', 'a', 's', 'p'), table});
  }
  return gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
}

// --- AD-5 -------------------------------------------------------------------------

/**
 * AD-5's gate for one face at one size: every glyph, glyph i at the fraction
 * pair (i + ppem) mod 25, fast against general under the snapped map. Returns
 * the glyphs drawn.
 */
size_t gate_one(Font & font, const std::string & label, uint32_t ppem) {
  Store s;
  const GSHSCENE_Resource * face = s.face(font);
  if (!face) {
    ADD_FAILURE() << label;
    return 0;
  }
  const size_t n = font.glyphs;
  const uint32_t cell = 2u * ppem + 8u;
  const uint32_t cols = 10u;
  const uint32_t rows = static_cast<uint32_t>((n + cols - 1) / cols);
  // The fast scene: the run at twice the size under a scale of one half moved by
  // a whole pixel, so that the device ppem is the integer and the map is not the
  // identity.
  std::vector<GSHSCENE_Glyph> fast;
  std::vector<std::pair<double, double>> at;
  for (size_t i = 0; i < n; i++) {
    const size_t pair = (i + ppem) % 25u;
    const double x = (i % cols) * cell + ppem / 2.0 + kFractions[pair % 5] / 64.0;
    const double y = (i / cols) * cell + 1.5 * ppem + kFractions[pair / 5] / 64.0;
    at.push_back({x, y});
    fast.push_back({static_cast<uint32_t>(i), 2.0 * (x - 1.0), 2.0 * y, nullptr});
  }
  GSHGEOM_Map half;
  GSHGEOM_Map map;
  gshgeom_map_scale(2, 0.5, 0.5, 1.0, &half);
  gshgeom_map_translate(2, 1.0, 0.0, 0.0, &map);
  gshgeom_map_compose(&map, &half, &map);
  const GSHSCENE_Resource * fast_run = s.run(face, 2.0 * ppem, GSHSCENE_HINT_NONE, fast);
  GSHSCENE_Glyph_Route route{};
  EXPECT_EQ(gshscene_glyph_route(fast_run, &map, GSHSCENE_TARGET_RASTER, &route), GSHSCENE_OK);
  EXPECT_EQ(route.route, static_cast<uint32_t>(GSHSCENE_GLYPH_FAST)) << label << " " << ppem;
  EXPECT_EQ(route.ppem, ppem);
  // The snapped scene, on the general route.
  std::vector<GSHSCENE_Glyph> snapped;
  for (size_t i = 0; i < n; i++) {
    GSHSCENE_Glyph_Origin o{};
    EXPECT_EQ(gshscene_glyph_origin(fast_run, &map, i, &o), GSHSCENE_OK);
    snapped.push_back({static_cast<uint32_t>(i), o.x / 64.0, o.y / 64.0, nullptr});
  }
  const double snapped_size = static_cast<double>(route.scale) * font.upem / 65536.0;
  const GSHSCENE_Resource * general_run = s.run(face, snapped_size, GSHSCENE_HINT_NONE, snapped);
  const GSHGEOM_Map tag = tagged_identity();
  GSHSCENE_Glyph_Route general{};
  EXPECT_EQ(gshscene_glyph_route(general_run, &tag, GSHSCENE_TARGET_RASTER, &general), GSHSCENE_OK);
  EXPECT_EQ(general.route, static_cast<uint32_t>(GSHSCENE_GLYPH_GENERAL));
  GSHSCENE_List * fl = s.fill(fast_run, &map);
  GSHSCENE_List * gl = s.fill(general_run, &tag);

  const GFNT_SceneStats before = font.stats();
  const Rendered f = render(fl, cols * cell, rows * cell);
  const GFNT_SceneStats mid = font.stats();
  const Rendered g = render(gl, cols * cell, rows * cell);
  const GFNT_SceneStats after = font.stats();
  EXPECT_EQ(f.result, GSHSCENE_OK);
  EXPECT_EQ(g.result, GSHSCENE_OK);
  EXPECT_EQ(mid.fast_calls - before.fast_calls, n) << "the fast tier is taken: " << label;
  EXPECT_EQ(mid.general_calls - before.general_calls, 0u) << label;
  EXPECT_EQ(after.fast_calls - mid.fast_calls, 0u) << label;
  EXPECT_EQ(after.general_calls - mid.general_calls, n) << label;
  EXPECT_TRUE(f.pixels == g.pixels) << "AD-5: " << label << " at " << ppem << " ppem";
  EXPECT_EQ(f.entries.size(), g.entries.size()) << label;
  size_t refused = 0;
  for (const GSHSCENE_Report_Entry & e : f.entries) {
    refused += e.entry_class == GSHSCENE_ENTRY_REFUSED;
  }
  if (refused < n) {
    EXPECT_FALSE(blank(f)) << "the comparison is not of two empty rasters: " << label;
  }
  else {
    // A fixture this library refuses whole (outline-cubic.ttf: a flag it does not
    // read) is refused by both routes, glyph by glyph.
    EXPECT_TRUE(blank(f) && blank(g)) << label;
  }
  return n - refused;
}

// --- AD-1 -------------------------------------------------------------------------

/**
 * AD-1 for one glyph: the direct call's coverage against a one-glyph scene's
 * alpha, at whole pixel (px, py) plus the fraction (fx, fy), y down. Returns
 * whether the glyph was drawn by both (both refusing is agreement too).
 */
bool direct_equals_scene(Font & font, const std::string & label, uint32_t glyph, uint32_t ppem,
    uint32_t mode, uint32_t fx, uint32_t fy) {
  GFNT_RasterOptions options{};
  options.hint_mode = static_cast<GFNT_HintMode>(mode);
  options.origin_x = static_cast<GFNT_F26Dot6>(fx);
  options.origin_y = -static_cast<GFNT_F26Dot6>(fy);   // y up
  GFNT_Coverage coverage{};
  const GFNT_Result direct = gfnt_face_render_glyph(font.face, glyph, ppem, &options, nullptr,
      &coverage, nullptr);

  const uint32_t w = 3u * ppem + 16u;
  const uint32_t h = 3u * ppem + 16u;
  const int32_t px = static_cast<int32_t>(ppem) + 8;
  const int32_t py = 2 * static_cast<int32_t>(ppem) + 8;
  Store s;
  const GSHSCENE_Resource * face = s.face(font);
  const GSHSCENE_Resource * run = s.run(face, ppem, mode,
      {{glyph, px + fx / 64.0, py + fy / 64.0, nullptr}});
  const Rendered r = render(s.fill(run, nullptr), w, h);
  EXPECT_EQ(r.result, GSHSCENE_OK);
  bool refused = false;
  for (const GSHSCENE_Report_Entry & e : r.entries) {
    refused = refused || e.entry_class == GSHSCENE_ENTRY_REFUSED;
  }
  const std::string where = label + " glyph " + std::to_string(glyph) + " ppem "
      + std::to_string(ppem) + " mode " + std::to_string(mode) + " at " + std::to_string(fx) + ","
      + std::to_string(fy);
  if (direct != GFNT_OK) {
    EXPECT_TRUE(refused) << "the direct call refused what the scene drew: " << where;
    EXPECT_TRUE(blank(r)) << where;
    return false;
  }
  EXPECT_FALSE(refused) << where;
  size_t mismatches = 0;
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      const int64_t col = static_cast<int64_t>(x) - px - coverage.left;
      const int64_t row = static_cast<int64_t>(y) - py + coverage.top;
      uint8_t want = 0;
      if (col >= 0 && row >= 0 && col < coverage.width && row < coverage.height) {
        want = gfnt_coverage_at(&coverage, static_cast<uint32_t>(col), static_cast<uint32_t>(row));
      }
      mismatches += r.alpha(x, y) != want;
    }
  }
  EXPECT_EQ(mismatches, 0u) << "AD-1: " << where;
  const bool drew = gfnt_coverage_total(&coverage) != 0;
  gfnt_coverage_destroy(&coverage);
  return drew;
}

}  // namespace

// AD-5: fast(map) == general(snap(map)), byte for byte, with hinting off; the fast tier is taken.
TEST(SceneGate, TheFastTierIsTheGeneralRouteUnderTheSnappedMap) {
  size_t glyphs = 0;
  size_t cases = 0;
  for (const char * name : kOutlineFixtures) {
    Font font(file_bytes(name));
    ASSERT_NE(font.scene, nullptr) << name;
    for (uint32_t ppem = 8; ppem <= 72; ppem++) {
      glyphs += gate_one(font, name, ppem);
      cases++;
    }
  }
  Font ties(tie_font());
  ASSERT_NE(ties.scene, nullptr);
  for (uint32_t ppem = 8; ppem <= 72; ppem++) {
    glyphs += gate_one(ties, "2048-unit ties", ppem);
    cases++;
  }
  EXPECT_EQ(cases, 16u * 65u);
  EXPECT_GT(glyphs, 16u * 65u * 4u);
}

// Hinting is what separates the routes: a hinted glyph differs from the general route, and HINTED says nothing.
TEST(SceneGate, HintingReachesTheWaist) {
  Font font(hinted_font());
  ASSERT_NE(font.scene, nullptr);
  Store s;
  const GSHSCENE_Resource * face = s.face(font);
  GSHSCENE_Face_Info info{};
  ASSERT_EQ(gshscene_face_info(face, &info), GSHSCENE_OK);
  EXPECT_NE(info.flags & GSHSCENE_FACE_HINTING, 0u) << "glyph programs, by maxp";
  // A whole-pixel origin, so that the hinted points are on the device grid; and
  // the version-35 interpreter, since version 40's backward compatibility ignores
  // a move in x (the run's hint version reaches the hinter).
  const std::vector<GSHSCENE_Glyph> g = {{1, 4.0, 20.0, nullptr}};
  const GSHSCENE_Resource * hinted = s.run(face, 13.0, GSHSCENE_HINT_FONT, g, GFNT_HINT_VERSION_35);
  const GSHSCENE_Resource * plain = s.run(face, 13.0, GSHSCENE_HINT_NONE, g);
  const Rendered a = render(s.fill(hinted, nullptr), 24, 24);
  const Rendered b = render(s.fill(plain, nullptr), 24, 24);
  EXPECT_NE(a.pixels, b.pixels);
  EXPECT_TRUE(a.entries.empty()) << "HINTED gives no entry";
  EXPECT_TRUE(b.entries.empty()) << "NONE gives no entry";
  // Hinted means on the grid: every point of the rectangle lands on a whole pixel,
  // so every pixel is empty or full.
  for (size_t i = 3; i < a.pixels.size(); i += 4) {
    const uint8_t v = static_cast<uint8_t>(a.pixels[i]);
    EXPECT_TRUE(v == 0 || v == 255) << i / 4;
  }
}

// AD-1: the direct calls equal a one-glyph scene, in every mode.
TEST(SceneGate, TheDirectCallIsAOneGlyphScene) {
  size_t drawn = 0;
  struct Case {
    std::string label;
    Bytes bytes;
  };
  std::vector<Case> cases;
  for (const char * name : {"basic.ttf", "outline-composite.ttf", "outline-broken-loca.ttf",
           "cff-curves.otf", "cff-seac.otf", "type1-big.pfb", "variable-gvar.ttf"}) {
    cases.push_back({name, file_bytes(name)});
  }
  cases.push_back({"ties", tie_font()});
  cases.push_back({"hinted", hinted_font()});
  for (const Case & c : cases) {
    Font font(c.bytes);
    ASSERT_NE(font.scene, nullptr) << c.label;
    const size_t n = font.glyphs < 12 ? font.glyphs : 12;
    for (uint32_t mode : {GSHSCENE_HINT_NONE, GSHSCENE_HINT_FONT, GSHSCENE_HINT_FONT_STRICT}) {
      for (uint32_t ppem : {8u, 13u, 16u, 31u, 48u}) {
        for (size_t g = 0; g < n; g++) {
          const uint32_t fx = kFractions[(g + ppem) % 5];
          const uint32_t fy = kFractions[(g + ppem / 2) % 5];
          drawn += direct_equals_scene(font, c.label, static_cast<uint32_t>(g), ppem, mode, fx, fy);
        }
      }
    }
  }
  EXPECT_GT(drawn, 500u);
}

// Strict hinting: a failing program refuses its glyph while drawing, and the rest is drawn.
TEST(Scene, AFailingProgramUnderStrictRefusesItsGlyph) {
  Font font(hinted_font());
  Store s;
  const GSHSCENE_Resource * face = s.face(font);
  const std::vector<GSHSCENE_Glyph> g = {{1, 2.0, 20.0, nullptr}, {2, 14.0, 20.0, nullptr}};
  const GSHSCENE_Resource * strict = s.run(face, 13.0, GSHSCENE_HINT_FONT_STRICT, g);
  const Rendered r = render(s.fill(strict, nullptr), 32, 24);
  EXPECT_EQ(r.result, GSHSCENE_OK);
  ASSERT_EQ(r.entries.size(), 1u);
  EXPECT_EQ(r.entries[0].entry_class, GSHSCENE_ENTRY_REFUSED);
  EXPECT_EQ(r.entries[0].reason, GSHSCENE_REASON_HINT_PROGRAM_FAILED);
  EXPECT_EQ(r.entries[0].second, 1u);
  EXPECT_FALSE(blank(r)) << "glyph 0 of the run is drawn";
  const Rendered refused = render(s.fill(strict, nullptr), 32, 24, GSHSCENE_RENDER_STRICT);
  EXPECT_EQ(refused.result, GSHSCENE_ERR_REFUSED);
  // Tolerant, the same program is drawn as far as it got, and says so per glyph.
  const GSHSCENE_Resource * tolerant = s.run(face, 13.0, GSHSCENE_HINT_FONT, g);
  const Rendered t = render(s.fill(tolerant, nullptr), 32, 24);
  ASSERT_EQ(t.entries.size(), 1u);
  EXPECT_EQ(t.entries[0].entry_class, GSHSCENE_ENTRY_HINT_STATUS);
  EXPECT_EQ(t.entries[0].second, 1u);
  // A glyph with no program in a face with one: a glyph-level NO_PROGRAM entry, drawn unhinted.
  const GSHSCENE_Resource * bare = s.run(face, 13.0, GSHSCENE_HINT_FONT, {{3, 2.0, 20.0, nullptr}});
  const Rendered u = render(s.fill(bare, nullptr), 32, 24);
  ASSERT_EQ(u.entries.size(), 1u);
  EXPECT_EQ(u.entries[0].reason, GSHSCENE_REASON_HINT_NO_PROGRAM);
  const GSHSCENE_Resource * none = s.run(face, 13.0, GSHSCENE_HINT_NONE, {{3, 2.0, 20.0, nullptr}});
  EXPECT_EQ(u.pixels, render(s.fill(none, nullptr), 32, 24).pixels);
}

// gasp decides the sizes a face is hinted at: below its range the glyph is unhinted, and says so.
TEST(Scene, ASizeGaspDoesNotFitIsDrawnUnhinted) {
  Font font(hinted_font(true));
  Store s;
  const GSHSCENE_Resource * face = s.face(font);
  for (double size : {10.0, 13.0}) {
    const GSHSCENE_Resource * hinted = s.run(face, size, GSHSCENE_HINT_FONT, {{1, 4.0, 20.0, nullptr}});
    const GSHSCENE_Resource * plain = s.run(face, size, GSHSCENE_HINT_NONE, {{1, 4.0, 20.0, nullptr}});
    const Rendered a = render(s.fill(hinted, nullptr), 24, 24);
    const Rendered b = render(s.fill(plain, nullptr), 24, 24);
    if (size < 12.0) {
      EXPECT_EQ(a.pixels, b.pixels) << "gasp does not grid-fit at " << size;
      ASSERT_EQ(a.entries.size(), 1u);
      EXPECT_EQ(a.entries[0].reason, GSHSCENE_REASON_HINT_NO_PROGRAM);
    }
    else {
      EXPECT_NE(a.pixels, b.pixels) << "gasp grid-fits at " << size;
      EXPECT_TRUE(a.entries.empty());
    }
  }
}

// A glyph this library cannot give is refused alone, with its index; the rest is drawn.
TEST(Scene, ACorruptGlyphIsRefusedAndTheRestDrawn) {
  Font font(hinted_font());
  Store s;
  const GSHSCENE_Resource * face = s.face(font);
  for (uint32_t mode : {GSHSCENE_HINT_NONE, GSHSCENE_HINT_FONT}) {
    const GSHSCENE_Resource * run = s.run(face, 13.0, mode,
        {{3, 2.0, 20.0, nullptr}, {4, 12.0, 20.0, nullptr}, {99, 20.0, 20.0, nullptr}});
    const Rendered r = render(s.fill(run, nullptr), 32, 24);
    EXPECT_EQ(r.result, GSHSCENE_OK);
    std::vector<size_t> refused;
    for (const GSHSCENE_Report_Entry & e : r.entries) {
      if (e.entry_class == GSHSCENE_ENTRY_REFUSED) {
        EXPECT_EQ(e.reason, GSHSCENE_REASON_GLYPH_UNAVAILABLE);
        refused.push_back(e.second);
      }
    }
    EXPECT_EQ(refused, (std::vector<size_t>{1, 2})) << "mode " << mode;
    EXPECT_FALSE(blank(r));
  }
}

// Instances: two coordinates are two faces whose outlines differ; equal coordinates are one face.
TEST(Scene, TwoInstancesAreTwoFacesWithDifferentOutlines) {
  Font font(file_bytes("variable-gvar.ttf"));
  ASSERT_NE(font.scene, nullptr);
  Store s;
  const GSHSCENE_Resource * a = s.face(font);
  const GSHSCENE_Resource * b = s.face(font, {GFNT_F2Dot14(16384)});
  const GSHSCENE_Resource * c = s.face(font, {GFNT_F2Dot14(16384)});
  const GSHSCENE_Resource * zero = s.face(font, {GFNT_F2Dot14(0)});
  EXPECT_NE(a, b);
  EXPECT_EQ(b, c);
  EXPECT_EQ(a, zero) << "a zero coordinate is the default instance";
  size_t differ = 0;
  for (uint32_t g = 0; g < font.glyphs; g++) {
    std::string pa;
    std::string pb;
    for (const GSHSCENE_Resource * face : {a, b}) {
      const GSHSCENE_Resource * run = s.run(face, 40.0, GSHSCENE_HINT_NONE, {{g, 10.0, 50.0, nullptr}});
      (face == a ? pa : pb) = render(s.fill(run, nullptr), 80, 64).pixels;
    }
    differ += pa != pb;
  }
  EXPECT_GT(differ, 0u);
  GFNT_Variation too_many{};
  std::vector<GFNT_F2Dot14> coords(65, 0);
  too_many.coords = coords.data();
  too_many.count = coords.size();
  const GSHSCENE_Resource * out = nullptr;
  EXPECT_EQ(gfnt_scene_intern_face(font.scene, s.store, &too_many, &out, nullptr), GFNT_ERR_INVALID);
  GFNT_Variation rounding{};
  rounding.coords = coords.data();
  rounding.count = 1;
  rounding.delta_rounding = static_cast<GFNT_DeltaRounding>(1);
  EXPECT_EQ(gfnt_scene_intern_face(font.scene, s.store, &rounding, &out, nullptr),
      GFNT_ERR_UNSUPPORTED);
  coords[0] = -16385;
  rounding.delta_rounding = static_cast<GFNT_DeltaRounding>(0);
  EXPECT_EQ(gfnt_scene_intern_face(font.scene, s.store, &rounding, &out, nullptr), GFNT_ERR_INVALID);
}

// The binding lives as long as its face resource: released once, its cache freed with it.
TEST(Scene, TheLastReleaseFreesTheBindingAndItsCache) {
  Font font(hinted_font());
  {
    Store s;
    const GSHSCENE_Resource * face = s.face(font);
    const GSHSCENE_Resource * again = s.face(font);
    EXPECT_EQ(face, again);
    EXPECT_EQ(font.stats().bindings_live, 1u) << "the second intern's binding was not kept";
    const GSHSCENE_Resource * run = s.run(face, 13.0, GSHSCENE_HINT_FONT, {{1, 2.0, 20.0, nullptr}});
    render(s.fill(run, nullptr), 24, 24);
    EXPECT_EQ(font.stats().contexts_live, 1u);
  }
  EXPECT_EQ(font.stats().bindings_live, 0u) << "the face's last release freed its binding";
  EXPECT_EQ(font.stats().contexts_live, 0u) << "and its hint contexts";
  // The scene font may go before the scene: its bindings hold it (ASan sees the rest).
  Font early(hinted_font());
  Store s;
  const GSHSCENE_Resource * face = s.face(early);
  gfnt_scene_font_destroy(early.scene);
  early.scene = nullptr;
  const GSHSCENE_Resource * run = s.run(face, 13.0, GSHSCENE_HINT_FONT, {{1, 2.0, 20.0, nullptr}});
  EXPECT_FALSE(blank(render(s.fill(run, nullptr), 24, 24)));
}

// D8: the hint contexts are an LRU of a stated capacity, per face resource.
TEST(Scene, TheHintContextCacheIsBoundedAndLeastRecentlyUsed) {
  Font font(hinted_font());
  Store s;
  const GSHSCENE_Resource * face = s.face(font);
  auto draw = [&](uint32_t ppem) {
    const GSHSCENE_Resource * run = s.run(face, ppem, GSHSCENE_HINT_FONT, {{1, 2.0, 2.0 * ppem, nullptr}});
    render(s.fill(run, nullptr), 3 * ppem, 3 * ppem);
  };
  const uint32_t extra = 4;
  for (uint32_t i = 0; i < GFNT_SCENE_HINT_CONTEXTS + extra; i++) {
    draw(10 + i);
    EXPECT_LE(font.stats().contexts_live, GFNT_SCENE_HINT_CONTEXTS) << "never past the capacity";
  }
  GFNT_SceneStats st = font.stats();
  EXPECT_EQ(st.contexts_made, GFNT_SCENE_HINT_CONTEXTS + extra);
  EXPECT_EQ(st.contexts_live, GFNT_SCENE_HINT_CONTEXTS);
  EXPECT_EQ(st.contexts_dropped, extra);
  // The most recent is kept; the least recent was dropped and is made again.
  draw(10 + GFNT_SCENE_HINT_CONTEXTS + extra - 1);
  EXPECT_EQ(font.stats().contexts_made, st.contexts_made);
  draw(10);
  EXPECT_EQ(font.stats().contexts_made, st.contexts_made + 1);
  // Mode and version are in the key.
  const GSHSCENE_Resource * strict = s.run(face, 10.0, GSHSCENE_HINT_FONT_STRICT, {{1, 2.0, 20.0, nullptr}});
  render(s.fill(strict, nullptr), 30, 30);
  EXPECT_EQ(font.stats().contexts_made, st.contexts_made + 2);
}

// Any op may be called from any thread at once.
TEST(Scene, TheOpsMayBeCalledFromManyThreads) {
  Font font(hinted_font());
  Store s;
  const GSHSCENE_Resource * face = s.face(font);
  const GSHSCENE_Glyph_Provider * ops = nullptr;
  void * ctx = nullptr;
  ASSERT_EQ(gshscene_face_binding(face, &ops, &ctx), GSHSCENE_OK);
  GSHSCENE_Face content{};
  ASSERT_EQ(gshscene_resource_face(face, &content), GSHSCENE_OK);
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 6; t++) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 60; i++) {
        GSHSCENE_Glyph_Request rq{};
        rq.glyph = 1;
        rq.ppem = 9u + static_cast<uint32_t>((i * 7 + t) % 13);
        gshscene_glyph_scale(rq.ppem, font.upem, &rq.scale);
        rq.frac_x = kFractions[i % 5];
        rq.hint_mode = GSHSCENE_HINT_FONT;
        GSHGEOM_Device_Path * path = nullptr;
        if (gshgeom_device_path_create(nullptr, nullptr, &path) != GSHGEOM_OK) {
          bad++;
          continue;
        }
        GSHSCENE_Glyph_Answer answer{};
        if (ops->glyph_device_path(ctx, &content, &rq, path, &answer) != GSHSCENE_OK
            || answer.status != GSHSCENE_HINT_STATUS_HINTED || answer.advance % 64 != 0) {
          bad++;
        }
        gshgeom_device_path_destroy(path);
      }
    });
  }
  for (std::thread & t : threads) {
    t.join();
  }
  EXPECT_EQ(bad.load(), 0);
  EXPECT_LE(font.stats().contexts_live, GFNT_SCENE_HINT_CONTEXTS);
}

// The ops one by one: R1's identity, the box, the bytes, the requests refused.
TEST(Scene, TheOpsAnswerAsTheSeamSays) {
  Bytes bytes = file_bytes("basic.ttf");
  Font font(bytes);
  Store s;
  const GSHSCENE_Resource * face = s.face(font);
  GSHSCENE_Face_Info info{};
  ASSERT_EQ(gshscene_face_info(face, &info), GSHSCENE_OK);
  EXPECT_EQ(info.upem, font.upem);
  EXPECT_EQ(info.glyph_count, font.glyphs);
  EXPECT_EQ(info.flags & GSHSCENE_FACE_HINTING, 0u) << "no program";
  ASSERT_NE(info.family_name, nullptr);
  ASSERT_NE(info.postscript_name, nullptr);
  char * family = nullptr;
  ASSERT_EQ(gfnt_face_name(font.face, GFNT_NAME_FAMILY, GFNT_LANGUAGE_ANY, nullptr, &family,
      nullptr, nullptr), GFNT_OK);
  EXPECT_STREQ(info.family_name, family);
  gfnt_name_free(nullptr, family);
  GSHSCENE_Face content{};
  ASSERT_EQ(gshscene_resource_face(face, &content), GSHSCENE_OK);
  EXPECT_EQ(content.font_key, gfnt_scene_font_key(font.scene));
  EXPECT_NE(content.font_key, 0u);
  Font other(file_bytes("outline-simple.ttf"));
  EXPECT_NE(gfnt_scene_font_key(other.scene), content.font_key) << "the key is the bytes'";
  Font same(bytes);
  EXPECT_EQ(gfnt_scene_font_key(same.scene), content.font_key);
  const GSHSCENE_Glyph_Provider * ops = nullptr;
  void * ctx = nullptr;
  ASSERT_EQ(gshscene_face_binding(face, &ops, &ctx), GSHSCENE_OK);
  EXPECT_EQ(ops->glyph_device_path, gfnt_scene_provider()->glyph_device_path) << "the face's copy of the ops";
  const uint8_t * data = nullptr;
  size_t size = 0;
  ASSERT_EQ(ops->face_data(ctx, &content, &data, &size), GSHSCENE_OK);
  EXPECT_EQ(Bytes(data, data + size), bytes);
  // The box is the control box in 26.6 design units; an empty glyph is empty.
  for (uint32_t g = 0; g < font.glyphs; g++) {
    GFNT_Outline * o = nullptr;
    ASSERT_EQ(gfnt_face_glyph_outline(font.face, g, nullptr, nullptr, &o, nullptr), GFNT_OK);
    GFNT_Box want{};
    gfnt_outline_control_box(o, &want);
    GSHSCENE_Glyph_Box box{};
    ASSERT_EQ(ops->glyph_box(ctx, &content, g, &box), GSHSCENE_OK);
    if (gfnt_box_is_empty(&want)) {
      EXPECT_NE(box.empty, 0u);
    }
    else {
      EXPECT_EQ(box.empty, 0u);
      EXPECT_EQ(box.x_min, want.x_min);
      EXPECT_EQ(box.y_min, want.y_min);
      EXPECT_EQ(box.x_max, want.x_max);
      EXPECT_EQ(box.y_max, want.y_max);
    }
    gfnt_outline_destroy(o);
  }
  GSHSCENE_Glyph_Box box{};
  EXPECT_EQ(ops->glyph_box(ctx, &content, static_cast<uint32_t>(font.glyphs), &box),
      GSHSCENE_ERR_INVALID);
  // A request that is not the seam's is refused: its scale, its fraction, its mode, its version, its size.
  GSHGEOM_Device_Path * path = nullptr;
  ASSERT_EQ(gshgeom_device_path_create(nullptr, nullptr, &path), GSHGEOM_OK);
  GSHSCENE_Glyph_Answer answer{};
  GSHSCENE_Glyph_Request rq{};
  rq.glyph = 1;
  rq.ppem = 16;
  ASSERT_EQ(gshscene_glyph_scale(16, font.upem, &rq.scale), GSHSCENE_OK);
  rq.hint_mode = GSHSCENE_HINT_NONE;
  EXPECT_EQ(ops->glyph_device_path(ctx, &content, &rq, path, &answer), GSHSCENE_OK);
  EXPECT_GT(answer.advance, 0);
  GSHSCENE_Glyph_Request bad = rq;
  bad.scale += 1;
  EXPECT_EQ(ops->glyph_device_path(ctx, &content, &bad, path, &answer), GSHSCENE_ERR_INVALID);
  bad = rq;
  bad.frac_x = 64;
  EXPECT_EQ(ops->glyph_device_path(ctx, &content, &bad, path, &answer), GSHSCENE_ERR_INVALID);
  bad = rq;
  bad.hint_mode = GSHSCENE_HINT_MODE_COUNT;
  EXPECT_EQ(ops->glyph_device_path(ctx, &content, &bad, path, &answer), GSHSCENE_ERR_INVALID);
  bad = rq;
  bad.hint_version = 3;
  EXPECT_EQ(ops->glyph_device_path(ctx, &content, &bad, path, &answer), GSHSCENE_ERR_INVALID);
  bad = rq;
  bad.ppem = 0;
  EXPECT_EQ(ops->glyph_device_path(ctx, &content, &bad, path, &answer), GSHSCENE_ERR_INVALID);
  gshgeom_device_path_destroy(path);
  EXPECT_EQ(gfnt_scene_font_stats(nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_scene_font_key(nullptr), 0u);
  EXPECT_EQ(gfnt_scene_font_create(nullptr, nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_scene_intern_face(nullptr, s.store, nullptr, nullptr, nullptr), GFNT_ERR_INVALID);
}

// Limits (AD-19): a face's own caps bound what its provider gives.
TEST(Scene, AFacesLimitsBoundItsProvider) {
  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_ppem = 20;
  limits.max_outline_points = 4;
  Font font(file_bytes("outline-simple.ttf"), &limits);
  ASSERT_NE(font.scene, nullptr);
  Store s;
  const GSHSCENE_Resource * face = s.face(font);
  GSHSCENE_Face_Info info{};
  ASSERT_EQ(gshscene_face_info(face, &info), GSHSCENE_OK);
  EXPECT_EQ(info.max_ppem, 20u) << "the face's max_ppem is the fast tier's";
  std::vector<GSHSCENE_Glyph> glyphs;
  for (uint32_t g = 0; g < font.glyphs; g++) {
    glyphs.push_back({g, 4.0 + 16.0 * g, 20.0, nullptr});
  }
  // At 24 ppem the route is general (HINT_SIZE under FONT); every glyph past four points is refused.
  for (double size : {16.0, 24.0}) {
    const GSHSCENE_Resource * run = s.run(face, size, GSHSCENE_HINT_FONT, glyphs);
    const Rendered r = render(s.fill(run, nullptr), 16 * static_cast<uint32_t>(font.glyphs) + 32, 40);
    size_t refused = 0;
    for (const GSHSCENE_Report_Entry & e : r.entries) {
      refused += e.entry_class == GSHSCENE_ENTRY_REFUSED;
    }
    EXPECT_GT(refused, 0u) << size;
    EXPECT_LT(refused, font.glyphs) << size;
  }
}

// Hostile fonts through the provider: no crash, no leak (ASan), whatever the bytes.
TEST(Scene, HostileFontsThroughTheProvider) {
  const Bytes seeds[] = {file_bytes("outline-composite.ttf"), file_bytes("cff-curves.otf"),
      hinted_font(), file_bytes("variable-gvar.ttf")};
  uint64_t state = 0x9E3779B97F4A7C15ull;
  auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  size_t loaded = 0;
  for (const Bytes & seed : seeds) {
    for (int k = 0; k < 120; k++) {
      Bytes b = seed;
      const int flips = 1 + static_cast<int>(next() % 8);
      for (int i = 0; i < flips; i++) {
        b[next() % b.size()] = static_cast<uint8_t>(next());
      }
      GFNT_Blob * blob = nullptr;
      GFNT_Face * face = nullptr;
      GFNT_SceneFont * sf = nullptr;
      if (gfnt_blob_create_memory(b.data(), b.size(), GFNT_BLOB_BORROWED, nullptr, nullptr, &blob,
              nullptr) == GFNT_OK
          && gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr) == GFNT_OK
          && gfnt_scene_font_create(face, &sf, nullptr) == GFNT_OK) {
        Store s;
        const GSHSCENE_Resource * f = nullptr;
        if (gfnt_scene_intern_face(sf, s.store, nullptr, &f, nullptr) == GFNT_OK) {
          s.held.push_back(f);
          loaded++;
          std::vector<GSHSCENE_Glyph> glyphs;
          for (uint32_t g = 0; g < 8; g++) {
            glyphs.push_back({g, 4.0 + 20.0 * g, 24.0, nullptr});
          }
          for (uint32_t mode : {GSHSCENE_HINT_NONE, GSHSCENE_HINT_FONT, GSHSCENE_HINT_FONT_STRICT}) {
            for (double size : {16.0, 16.5}) {
              const GSHSCENE_Resource * run = s.run(f, size, mode, glyphs);
              const Rendered r = render(s.fill(run, nullptr), 176, 40);
              EXPECT_TRUE(r.result == GSHSCENE_OK || r.result == GSHSCENE_ERR_LIMIT) << r.result;
            }
          }
        }
      }
      gfnt_scene_font_destroy(sf);
      gfnt_face_free(face);
      gfnt_blob_destroy(blob);
    }
  }
  EXPECT_GT(loaded, 100u) << "the mutations must leave most fonts loadable, or this tests the loader";
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
