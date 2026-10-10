/**
 * @file
 *
 * Color: `CPAL` palettes and `COLR` version 0 layers.
 *
 * The fixtures are written by fontTools (`tools/fixtures/color_fixtures.py`), so
 * the expected values below are fontTools' own dump of the tables, not this
 * library's reading of them. `tools/oracle/color_diff.py` holds the same reads to
 * fontTools' decompiler over every glyph. The last test overwrites every byte of
 * both tables and requires an error or an answer, never a crash.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstdint>
#include <fstream>
#include <vector>

#include <ghoti.io/font/color.h>
#include <ghoti.io/font/variation.h>

namespace {

/** A font loaded from bytes. */
struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  explicit Font(const std::string & name) {
    std::ifstream in(gfnttest::data("fonts/" + name), std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    open();
  }
  explicit Font(std::vector<uint8_t> data) : bytes(std::move(data)) { open(); }
  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;
  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  void open() {
    GFNT_Error error;
    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED,
                  nullptr, nullptr, &blob, &error), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
  }
};

struct Rgba {
  uint8_t r, g, b, a;
  bool operator==(const Rgba & o) const {
    return r == o.r && g == o.g && b == o.b && a == o.a;
  }
};

std::ostream & operator<<(std::ostream & out, const Rgba & c) {
  return out << "#" << std::hex << int(c.r) << "," << int(c.g) << "," << int(c.b)
             << "," << int(c.a) << std::dec;
}

Rgba color_of(const Font & font, size_t palette, size_t entry) {
  GFNT_Color c{};
  EXPECT_EQ(gfnt_face_palette_color(font.face, palette, entry, &c, nullptr), GFNT_OK);
  return {c.red, c.green, c.blue, c.alpha};
}

/** Where a table's bytes start in an sfnt, and how long they are. */
bool table_range(const std::vector<uint8_t> & b, const char * tag, size_t * at,
    size_t * length) {
  const uint16_t tables = static_cast<uint16_t>(b[4] << 8 | b[5]);
  for (uint16_t i = 0; i < tables; ++i) {
    const size_t rec = 12 + 16u * i;
    if (std::string(reinterpret_cast<const char *>(&b[rec]), 4) == tag) {
      *at = static_cast<size_t>(b[rec + 8]) << 24 | b[rec + 9] << 16 | b[rec + 10] << 8
          | b[rec + 11];
      *length = static_cast<size_t>(b[rec + 12]) << 24 | b[rec + 13] << 16
          | b[rec + 14] << 8 | b[rec + 15];
      return true;
    }
  }
  return false;
}

// Glyph IDs of the fixture: .notdef 0, space 1, A 2, B 3, ghoti.alt 4, then the
// layer glyphs 5, 6 and 7.
constexpr uint32_t kSpace = 1, kA = 2, kB = 3, kAlt = 4;

TEST(ColorPalettes, AVersionOneTableGivesEveryColorAndLabel) {
  Font font("color-v0.ttf");
  ASSERT_TRUE(gfnt_face_has_palettes(font.face));
  size_t n = 0;
  ASSERT_EQ(gfnt_face_palette_count(font.face, &n, nullptr), GFNT_OK);
  EXPECT_EQ(n, 3u);
  ASSERT_EQ(gfnt_face_palette_entry_count(font.face, &n, nullptr), GFNT_OK);
  EXPECT_EQ(n, 3u);
  // fontTools' dump: #RRGGBBAA.
  const Rgba want[3][3] = {
      {{255, 0, 0, 255}, {0, 128, 0, 255}, {0, 0, 255, 128}},
      {{16, 32, 48, 255}, {200, 210, 220, 64}, {1, 2, 3, 0}},
      {{255, 255, 255, 255}, {128, 128, 128, 255}, {0, 0, 0, 255}}};
  for (size_t p = 0; p < 3; ++p) {
    for (size_t e = 0; e < 3; ++e) {
      EXPECT_EQ(color_of(font, p, e), want[p][e]) << p << "," << e;
    }
  }
  GFNT_PaletteInfo info{};
  ASSERT_EQ(gfnt_face_palette_info(font.face, 0, &info, nullptr), GFNT_OK);
  EXPECT_EQ(info.flags, GFNT_PALETTE_USABLE_ON_LIGHT);
  EXPECT_EQ(info.label, 256);
  ASSERT_EQ(gfnt_face_palette_info(font.face, 1, &info, nullptr), GFNT_OK);
  EXPECT_EQ(info.flags, GFNT_PALETTE_USABLE_ON_DARK);
  EXPECT_EQ(info.label, 257);
  ASSERT_EQ(gfnt_face_palette_info(font.face, 2, &info, nullptr), GFNT_OK);
  EXPECT_EQ(info.flags, 0u);
  EXPECT_EQ(info.label, GFNT_PALETTE_NO_LABEL);
  uint16_t label = 0;
  ASSERT_EQ(gfnt_face_palette_entry_label(font.face, 0, &label, nullptr), GFNT_OK);
  EXPECT_EQ(label, 258);
  ASSERT_EQ(gfnt_face_palette_entry_label(font.face, 1, &label, nullptr), GFNT_OK);
  EXPECT_EQ(label, GFNT_PALETTE_NO_LABEL);
  ASSERT_EQ(gfnt_face_palette_entry_label(font.face, 2, &label, nullptr), GFNT_OK);
  EXPECT_EQ(label, 259);
}

TEST(ColorPalettes, AVersionZeroTableHasNoTypesOrLabels) {
  Font font("color-cpal0.ttf");
  size_t n = 0;
  ASSERT_EQ(gfnt_face_palette_count(font.face, &n, nullptr), GFNT_OK);
  EXPECT_EQ(n, 2u);
  EXPECT_EQ(color_of(font, 0, 2), (Rgba{0, 0, 255, 128}));
  EXPECT_EQ(color_of(font, 1, 2), (Rgba{0, 0, 255, 128}));
  GFNT_PaletteInfo info{};
  ASSERT_EQ(gfnt_face_palette_info(font.face, 1, &info, nullptr), GFNT_OK);
  EXPECT_EQ(info.flags, 0u);
  EXPECT_EQ(info.label, GFNT_PALETTE_NO_LABEL);
  uint16_t label = 0;
  ASSERT_EQ(gfnt_face_palette_entry_label(font.face, 0, &label, nullptr), GFNT_OK);
  EXPECT_EQ(label, GFNT_PALETTE_NO_LABEL);
}

TEST(ColorPalettes, AnIndexOutOfRangeIsRefused) {
  Font font("color-v0.ttf");
  GFNT_Color c{};
  GFNT_PaletteInfo info{};
  uint16_t label = 0;
  EXPECT_EQ(gfnt_face_palette_color(font.face, 3, 0, &c, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_palette_color(font.face, 0, 3, &c, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_palette_info(font.face, 3, &info, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_palette_entry_label(font.face, 3, &label, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_palette_color(font.face, 0, 0, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_palette_color(nullptr, 0, 0, &c, nullptr), GFNT_ERR_INVALID);
}

TEST(ColorLayers, EachBaseGlyphListsItsLayersBottomUp) {
  Font font("color-v0.ttf");
  ASSERT_TRUE(gfnt_face_has_color_layers(font.face));
  uint16_t version = 9;
  ASSERT_EQ(gfnt_face_color_version(font.face, &version, nullptr), GFNT_OK);
  EXPECT_EQ(version, 0);
  struct Want { uint32_t base; std::vector<std::pair<uint32_t, uint16_t>> layers; };
  const std::vector<Want> wants = {
      {kA, {{5, 0}, {6, 1}, {7, GFNT_COLOR_FOREGROUND}}},
      {kB, {{6, 2}}},
      {kAlt, {{7, 1}, {5, 2}}}};
  for (const Want & w : wants) {
    size_t n = 99;
    ASSERT_EQ(gfnt_face_color_layer_count(font.face, w.base, &n, nullptr), GFNT_OK);
    ASSERT_EQ(n, w.layers.size()) << w.base;
    for (size_t i = 0; i < n; ++i) {
      GFNT_ColorLayer layer{};
      ASSERT_EQ(gfnt_face_color_layer(font.face, w.base, i, &layer, nullptr), GFNT_OK);
      EXPECT_EQ(layer.glyph, w.layers[i].first) << w.base << "/" << i;
      EXPECT_EQ(layer.palette_index, w.layers[i].second) << w.base << "/" << i;
    }
  }
}

TEST(ColorLayers, AGlyphWithNoRecordHasNoLayersAndThatIsNotAnError) {
  Font font("color-v0.ttf");
  size_t n = 99;
  for (uint32_t g : {0u, kSpace, 5u, 6u, 7u, 1000u, 0xFFFFFFFFu}) {
    n = 99;
    ASSERT_EQ(gfnt_face_color_layer_count(font.face, g, &n, nullptr), GFNT_OK) << g;
    EXPECT_EQ(n, 0u) << g;
  }
  GFNT_ColorLayer layer{};
  EXPECT_EQ(gfnt_face_color_layer(font.face, kSpace, 0, &layer, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_color_layer(font.face, kA, 3, &layer, nullptr),
      GFNT_ERR_INVALID);
}

TEST(ColorLayers, ACpalZeroFontKeepsItsLayers) {
  Font font("color-cpal0.ttf");
  size_t n = 0;
  ASSERT_EQ(gfnt_face_color_layer_count(font.face, kA, &n, nullptr), GFNT_OK);
  EXPECT_EQ(n, 3u);
}

TEST(Color, AFaceWithNeitherTableSaysSo) {
  Font font("basic.ttf");
  EXPECT_FALSE(gfnt_face_has_palettes(font.face));
  EXPECT_FALSE(gfnt_face_has_color_layers(font.face));
  size_t n = 0;
  uint16_t v = 0;
  GFNT_Color c{};
  GFNT_ColorLayer layer{};
  EXPECT_EQ(gfnt_face_palette_count(font.face, &n, nullptr), GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(gfnt_face_palette_color(font.face, 0, 0, &c, nullptr),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(gfnt_face_color_version(font.face, &v, nullptr), GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(gfnt_face_color_layer_count(font.face, 2, &n, nullptr),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(gfnt_face_color_layer(font.face, 2, 0, &layer, nullptr),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_FALSE(gfnt_face_has_palettes(nullptr));
}

TEST(ColorLayers, ALayerPastTheDeclaredLayerRecordsIsCorruptEvenIfBytesFollow) {
  // numLayerRecords says 5 of the 6 records the table holds, so the last layer of
  // ghoti.alt (record 5) is not one the header admits to, whatever follows.
  Font source("color-v0.ttf");
  size_t at = 0, length = 0;
  ASSERT_TRUE(table_range(source.bytes, "COLR", &at, &length));
  std::vector<uint8_t> bytes = source.bytes;
  ASSERT_EQ(bytes[at + 12] << 8 | bytes[at + 13], 6);
  bytes[at + 13] = 5;
  Font font(bytes);
  GFNT_ColorLayer layer{};
  EXPECT_EQ(gfnt_face_color_layer(font.face, kAlt, 0, &layer, nullptr), GFNT_OK);
  EXPECT_EQ(gfnt_face_color_layer(font.face, kAlt, 1, &layer, nullptr),
      GFNT_ERR_CORRUPT);
}

TEST(Color, EveryByteOfBothTablesOverwrittenGivesAnAnswerOrAnError) {
  Font source("color-v0.ttf");
  size_t counts[2] = {};
  for (const char * tag : {"CPAL", "COLR"}) {
    size_t at = 0, length = 0;
    ASSERT_TRUE(table_range(source.bytes, tag, &at, &length));
    for (size_t i = 0; i < length; ++i) {
      for (uint8_t value : {uint8_t(0x00), uint8_t(0xFF), uint8_t(0x7F)}) {
        std::vector<uint8_t> bytes = source.bytes;
        bytes[at + i] = value;
        Font font(bytes);
        if (!font.face) {
          continue;
        }
        size_t palettes = 0, entries = 0;
        if (gfnt_face_palette_count(font.face, &palettes, nullptr) == GFNT_OK
            && gfnt_face_palette_entry_count(font.face, &entries, nullptr) == GFNT_OK) {
          for (size_t p = 0; p < palettes && p < 8; ++p) {
            GFNT_PaletteInfo info;
            gfnt_face_palette_info(font.face, p, &info, nullptr);
            for (size_t e = 0; e < entries && e < 8; ++e) {
              GFNT_Color c;
              GFNT_Result r = gfnt_face_palette_color(font.face, p, e, &c, nullptr);
              EXPECT_TRUE(r == GFNT_OK || r == GFNT_ERR_CORRUPT || r == GFNT_ERR_INVALID);
              counts[0] += r == GFNT_ERR_CORRUPT;
            }
          }
          for (size_t e = 0; e < entries && e < 8; ++e) {
            uint16_t label;
            gfnt_face_palette_entry_label(font.face, e, &label, nullptr);
          }
        }
        for (uint32_t g = 0; g < 10; ++g) {
          size_t n = 0;
          if (gfnt_face_color_layer_count(font.face, g, &n, nullptr) == GFNT_OK) {
            for (size_t k = 0; k < n && k < 8; ++k) {
              GFNT_ColorLayer layer;
              GFNT_Result r = gfnt_face_color_layer(font.face, g, k, &layer, nullptr);
              EXPECT_TRUE(r == GFNT_OK || r == GFNT_ERR_CORRUPT);
              counts[1] += r == GFNT_ERR_CORRUPT;
            }
          }
        }
      }
    }
  }
  // The sweep reached the error arms of both readers.
  EXPECT_GT(counts[0], 0u);
  EXPECT_GT(counts[1], 0u);
}

// ---------------------------------------------------------- COLR version 1
//
// The expected numbers are fontTools' dump of the fixtures, written by
// `tools/oracle/color_diff.py`'s reference side and checked there at eight
// locations; here a handful are kept so that `make test` needs no container.

struct Node {
  GFNT_ColorPaint paint;
  size_t depth;
};

struct Recorder {
  std::vector<Node> nodes;
};

bool record_enter(void * user, const GFNT_ColorPaint * paint, size_t depth) {
  static_cast<Recorder *>(user)->nodes.push_back({*paint, depth});
  return true;
}

std::vector<int> formats_of(const Font & font, uint32_t glyph,
    const GFNT_Variation * variation = nullptr, Recorder * keep = nullptr) {
  Recorder local;
  Recorder & r = keep ? *keep : local;
  GFNT_PaintVisitor visitor = {record_enter, nullptr};
  EXPECT_EQ(gfnt_face_color_paint_walk(font.face, glyph, variation, &visitor, &r,
                nullptr), GFNT_OK);
  std::vector<int> out;
  for (const Node & n : r.nodes) {
    out.push_back(static_cast<int>(n.paint.format));
  }
  return out;
}

TEST(ColorPaint, AVersionOneTableHasNoVersionZeroLayersHere) {
  Font font("color-v1.ttf");
  uint16_t version = 0;
  ASSERT_EQ(gfnt_face_color_version(font.face, &version, nullptr), GFNT_OK);
  EXPECT_EQ(version, 1);
  size_t n = 9;
  ASSERT_EQ(gfnt_face_color_layer_count(font.face, 2, &n, nullptr), GFNT_OK);
  EXPECT_EQ(n, 0u);
  GFNT_PaintRef ref = 7;
  ASSERT_EQ(gfnt_face_color_base_paint(font.face, kSpace, &ref, nullptr), GFNT_OK);
  EXPECT_EQ(ref, 0u);   // a glyph with no record
  ASSERT_EQ(gfnt_face_color_base_paint(font.face, kA, &ref, nullptr), GFNT_OK);
  EXPECT_NE(ref, 0u);
  // A version 0 table has no version 1 records.
  Font v0("color-v0.ttf");
  ASSERT_EQ(gfnt_face_color_base_paint(v0.face, kA, &ref, nullptr), GFNT_OK);
  EXPECT_EQ(ref, 0u);
}

TEST(ColorPaint, LayersAreExpandedFromTheSharedLayerList) {
  Font font("color-v1.ttf");
  size_t list = 0;
  ASSERT_EQ(gfnt_face_color_layer_list_count(font.face, &list, nullptr), GFNT_OK);
  EXPECT_EQ(list, 4u);
  Recorder r;
  // layers, then four (glyph, fill) pairs: solid, linear, radial, sweep.
  EXPECT_EQ(formats_of(font, kA, nullptr, &r),
      (std::vector<int>{1, 10, 2, 10, 4, 10, 6, 10, 8}));
  EXPECT_EQ(r.nodes[0].paint.layer_count, 4u);
  EXPECT_EQ(r.nodes[0].paint.first_layer, 0u);
  EXPECT_EQ(r.nodes[1].paint.glyph, 5u);
  EXPECT_EQ(r.nodes[2].paint.palette_index, 0);
  EXPECT_EQ(r.nodes[2].paint.v[0], 8192);   // alpha 0.5 in F2Dot14
  EXPECT_EQ(r.nodes[2].depth, 2u);
}

TEST(ColorPaint, GradientsCarryTheirPointsAndColorLines) {
  Font font("color-v1.ttf");
  Recorder r;
  formats_of(font, kA, nullptr, &r);
  const GFNT_ColorPaint & linear = r.nodes[4].paint;
  ASSERT_EQ(linear.format, GFNT_PAINT_LINEAR_GRADIENT);
  EXPECT_EQ(std::vector<int32_t>(linear.v, linear.v + 6),
      (std::vector<int32_t>{0, 0, 600, 0, 0, 600}));
  GFNT_ColorLine line{};
  ASSERT_EQ(gfnt_face_color_line(font.face, &linear, &line, nullptr), GFNT_OK);
  EXPECT_EQ(line.extend, GFNT_COLOR_EXTEND_PAD);
  ASSERT_EQ(line.stop_count, 3u);
  const int32_t want[3][3] = {{0, 0, 16384}, {8192, 1, 8192}, {16384, 2, 4096}};
  for (size_t i = 0; i < 3; ++i) {
    GFNT_ColorStop stop{};
    ASSERT_EQ(gfnt_face_color_stop(font.face, &linear, i, nullptr, &stop, nullptr),
        GFNT_OK);
    EXPECT_EQ(stop.offset, want[i][0]);
    EXPECT_EQ(stop.palette_index, want[i][1]);
    EXPECT_EQ(stop.alpha, want[i][2]);
  }
  GFNT_ColorStop stop{};
  EXPECT_EQ(gfnt_face_color_stop(font.face, &linear, 3, nullptr, &stop, nullptr),
      GFNT_ERR_INVALID);
  const GFNT_ColorPaint & radial = r.nodes[6].paint;
  ASSERT_EQ(radial.format, GFNT_PAINT_RADIAL_GRADIENT);
  EXPECT_EQ(std::vector<int32_t>(radial.v, radial.v + 6),
      (std::vector<int32_t>{300, 300, 0, 300, 300, 400}));
  ASSERT_EQ(gfnt_face_color_line(font.face, &radial, &line, nullptr), GFNT_OK);
  EXPECT_EQ(line.extend, GFNT_COLOR_EXTEND_REPEAT);
  ASSERT_EQ(gfnt_face_color_stop(font.face, &radial, 1, nullptr, &stop, nullptr),
      GFNT_OK);
  EXPECT_EQ(stop.palette_index, GFNT_COLOR_FOREGROUND);
  const GFNT_ColorPaint & sweep = r.nodes[8].paint;
  ASSERT_EQ(sweep.format, GFNT_PAINT_SWEEP_GRADIENT);
  // 0 and 90 degrees, as stored: a half turn is 16384 and the bias is one.
  EXPECT_EQ(std::vector<int32_t>(sweep.v, sweep.v + 4),
      (std::vector<int32_t>{300, 300, -16384, -8192}));
  ASSERT_EQ(gfnt_face_color_line(font.face, &sweep, &line, nullptr), GFNT_OK);
  EXPECT_EQ(line.extend, GFNT_COLOR_EXTEND_REFLECT);
  // A paint that is not a gradient has no line.
  EXPECT_EQ(gfnt_face_color_line(font.face, &r.nodes[0].paint, &line, nullptr),
      GFNT_ERR_INVALID);
}

TEST(ColorPaint, TheTransformChainKeepsItsOrderAndItsNumbers) {
  Font font("color-v1.ttf");
  Recorder r;
  EXPECT_EQ(formats_of(font, kB, nullptr, &r),
      (std::vector<int>{12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 10, 2}));
  auto v = [&](size_t i, size_t n) {
    return std::vector<int32_t>(r.nodes[i].paint.v, r.nodes[i].paint.v + n);
  };
  EXPECT_EQ(v(0, 6), (std::vector<int32_t>{65536, 32768, -16384, 65536, 6553600,
                                           -13107200}));      // Fixed
  EXPECT_EQ(v(1, 2), (std::vector<int32_t>{-30, 40}));
  EXPECT_EQ(v(2, 2), (std::vector<int32_t>{28672, 8192}));    // 1.75 and 0.5
  EXPECT_EQ(v(3, 4), (std::vector<int32_t>{12288, 20480, 300, 0}));
  EXPECT_EQ(v(4, 1), (std::vector<int32_t>{24576}));          // 1.5
  EXPECT_EQ(v(5, 3), (std::vector<int32_t>{8192, 10, 20}));
  EXPECT_EQ(v(6, 1), (std::vector<int32_t>{-8192}));          // -90 degrees
  EXPECT_EQ(v(7, 3), (std::vector<int32_t>{4096, 50, -50}));  // 45 degrees
  EXPECT_EQ(v(8, 2), (std::vector<int32_t>{4096, 0}));
  EXPECT_EQ(v(9, 4), (std::vector<int32_t>{2048, -1024, 100, 200}));
  EXPECT_FALSE(r.nodes[0].paint.variable);
  EXPECT_NE(r.nodes[1].paint.child, 0u);
  EXPECT_EQ(r.nodes[10].paint.glyph, 5u);
  EXPECT_EQ(r.nodes[11].paint.palette_index, 1);
  EXPECT_EQ(r.nodes[11].paint.v[0], 16384);
}

TEST(ColorPaint, ACompositeNamesItsModeAndWalksIntoAColorGlyph) {
  Font font("color-v1.ttf");
  Recorder r;
  // composite, its source (a PaintColrGlyph standing for glyph A's whole graph,
  // which the walk enters), then its backdrop.
  EXPECT_EQ(formats_of(font, kAlt, nullptr, &r),
      (std::vector<int>{32, 11, 1, 10, 2, 10, 4, 10, 6, 10, 8, 10, 2}));
  EXPECT_EQ(r.nodes[0].paint.composite_mode, 23);   // multiply
  EXPECT_EQ(r.nodes[1].paint.glyph, kA);
  EXPECT_EQ(r.nodes[2].depth, 2u);
  EXPECT_EQ(r.nodes[11].depth, 1u);                 // the backdrop is a sibling
  EXPECT_EQ(r.nodes[12].paint.v[0], 12288);
}

TEST(ColorPaint, TheClipListGivesOneBoxToARangeOfGlyphs) {
  Font font("color-v1.ttf");
  GFNT_ColorBox box{};
  bool found = false;
  for (uint32_t g : {kA, kB, kAlt}) {
    ASSERT_EQ(gfnt_face_color_clip(font.face, g, nullptr, &box, &found, nullptr),
        GFNT_OK);
    EXPECT_TRUE(found) << g;
    EXPECT_EQ(box.x_min, 0);
    EXPECT_EQ(box.y_min, -100);
    EXPECT_EQ(box.x_max, 700);
    EXPECT_EQ(box.y_max, 900);
  }
  for (uint32_t g : {0u, kSpace, 5u, 1000u}) {
    ASSERT_EQ(gfnt_face_color_clip(font.face, g, nullptr, &box, &found, nullptr),
        GFNT_OK);
    EXPECT_FALSE(found) << g;
  }
}

GFNT_Variation location(const Font & font, std::initializer_list<double> user,
    std::vector<GFNT_F2Dot14> * storage, bool strict = true) {
  std::vector<GFNT_F16Dot16> coordinates;
  for (double v : user) {
    coordinates.push_back(static_cast<GFNT_F16Dot16>(v * 65536.0));
  }
  storage->assign(coordinates.size(), 0);
  const GFNT_Result r = gfnt_face_normalize(font.face, coordinates.data(),
      coordinates.size(), storage->data(), storage->size(), nullptr);
  if (strict) {
    EXPECT_EQ(r, GFNT_OK);
  }
  GFNT_Variation variation{};
  if (r != GFNT_OK) {
    return variation;   // a face with no axes: the default instance
  }
  variation.coords = storage->data();
  variation.count = storage->size();
  return variation;
}

TEST(ColorPaintVariable, TheDefaultLocationReadsTheStoredNumbers) {
  Font font("color-v1-var.ttf");
  Recorder r;
  formats_of(font, 2, nullptr, &r);          // "tri"
  ASSERT_GE(r.nodes.size(), 3u);
  EXPECT_TRUE(r.nodes[2].paint.variable);
  EXPECT_EQ(std::vector<int32_t>(r.nodes[2].paint.v, r.nodes[2].paint.v + 6),
      (std::vector<int32_t>{0, 0, 600, 0, 0, 600}));
  GFNT_ColorStop stop{};
  ASSERT_EQ(gfnt_face_color_stop(font.face, &r.nodes[2].paint, 1, nullptr, &stop,
                nullptr), GFNT_OK);
  EXPECT_EQ(stop.offset, 16384);
  EXPECT_EQ(stop.alpha, 8192);
}

TEST(ColorPaintVariable, AMovedLocationAddsTheStoresDeltas) {
  // wght 650 is 0.75 after avar and wdth 87.5 is -0.5: both exact, so fontTools'
  // floats and this library's fixed point agree on every delta.
  for (const char * name : {"color-v1-var.ttf", "color-v1-varmap.ttf"}) {
    Font font(name);
    std::vector<GFNT_F2Dot14> storage;
    GFNT_Variation at = location(font, {650.0, 87.5}, &storage);
    Recorder r;
    formats_of(font, 2, &at, &r);
    const bool mapped = std::string(name).find("map") != std::string::npos;
    const std::vector<int32_t> linear = mapped
        ? std::vector<int32_t>{3, -2, 593, 5, 5, 605}
        : std::vector<int32_t>{1, 6, 594, -1, 5, 593};
    EXPECT_EQ(std::vector<int32_t>(r.nodes[2].paint.v, r.nodes[2].paint.v + 6),
        linear) << name;
    GFNT_ColorStop stop{};
    ASSERT_EQ(gfnt_face_color_stop(font.face, &r.nodes[2].paint, 0, &at, &stop,
                  nullptr), GFNT_OK);
    EXPECT_EQ(stop.offset, mapped ? -5 : -8) << name;
    EXPECT_EQ(stop.alpha, mapped ? 16391 : 16381) << name;
    GFNT_ColorBox box{};
    bool found = false;
    ASSERT_EQ(gfnt_face_color_clip(font.face, 2, &at, &box, &found, nullptr), GFNT_OK);
    ASSERT_TRUE(found);
    EXPECT_EQ(box.x_min, mapped ? 3 : 1) << name;
    EXPECT_EQ(box.y_min, mapped ? -102 : -94) << name;
    // The same location read without it is the stored numbers.
    ASSERT_EQ(gfnt_face_color_clip(font.face, 2, nullptr, &box, &found, nullptr),
        GFNT_OK);
    EXPECT_EQ(box.x_min, 0);
  }
}

TEST(ColorPaintVariable, AMapEntryOfNoVariationReadsTheStoredNumbers) {
  // Every entry of this font's index map is 0xFFFF:0xFFFF. Read as a store row it
  // names a VarData that does not exist and the walk fails; the entry means "no
  // variation", as fontTools reads it, so the moved location equals the default.
  Font font("color-v1-varnone.ttf");
  std::vector<GFNT_F2Dot14> storage;
  GFNT_Variation at = location(font, {650.0, 87.5}, &storage);
  Recorder moved, stored;
  GFNT_PaintVisitor visitor = {record_enter, nullptr};
  GFNT_Error error;
  gfnt_error_clear(&error);
  ASSERT_EQ(gfnt_face_color_paint_walk(font.face, 2, &at, &visitor, &moved, &error),
      GFNT_OK) << (error.message ? error.message : "");
  ASSERT_EQ(gfnt_face_color_paint_walk(font.face, 2, nullptr, &visitor, &stored,
                nullptr), GFNT_OK);
  ASSERT_EQ(moved.nodes.size(), stored.nodes.size());
  for (size_t i = 0; i < moved.nodes.size(); ++i) {
    EXPECT_EQ(std::vector<int32_t>(moved.nodes[i].paint.v, moved.nodes[i].paint.v + 6),
        std::vector<int32_t>(stored.nodes[i].paint.v, stored.nodes[i].paint.v + 6)) << i;
  }
  GFNT_ColorStop stop{};
  ASSERT_EQ(gfnt_face_color_stop(font.face, &moved.nodes[2].paint, 0, &at, &stop,
                nullptr), GFNT_OK);
  EXPECT_EQ(stop.offset, 0);
  EXPECT_EQ(stop.alpha, 16384);
  GFNT_ColorBox box{};
  bool found = false;
  ASSERT_EQ(gfnt_face_color_clip(font.face, 2, &at, &box, &found, nullptr), GFNT_OK);
  ASSERT_TRUE(found);
  EXPECT_EQ(box.x_min, 0);
  EXPECT_EQ(box.y_min, -100);
}

TEST(ColorPaintVariable, ABaseOfNoVariationLeavesEverySlotAlone) {
  // Set the linear gradient's VarIndexBase (the six numbers 0 0 600 0 0 600,
  // then the base) to 0xFFFFFFFF. Slot k of it is not 0xFFFFFFFF + k, which wraps
  // to row k - 1 and moves a number the font fixed; the whole paint stays put.
  Font source("color-v1-var.ttf");
  size_t at = 0, length = 0;
  ASSERT_TRUE(table_range(source.bytes, "COLR", &at, &length));
  const uint8_t pattern[] = {0, 0, 0, 0, 0x02, 0x58, 0, 0, 0, 0, 0x02, 0x58};
  std::vector<uint8_t> bytes = source.bytes;
  size_t patched = 0;
  for (size_t i = at + 4; i + 16 + 4 <= at + length; ++i) {
    if (bytes[i - 4] == 5 && std::equal(pattern, pattern + sizeof pattern,
                                         bytes.begin() + i)) {
      for (size_t k = 0; k < 4; ++k) {
        bytes[i + 12 + k] = 0xFF;
      }
      ++patched;
    }
  }
  ASSERT_EQ(patched, 1u);
  Font font(bytes);
  std::vector<GFNT_F2Dot14> storage;
  GFNT_Variation moved = location(font, {650.0, 87.5}, &storage);
  Recorder r;
  formats_of(font, 2, &moved, &r);
  EXPECT_EQ(std::vector<int32_t>(r.nodes[2].paint.v, r.nodes[2].paint.v + 6),
      (std::vector<int32_t>{0, 0, 600, 0, 0, 600}));
}

TEST(ColorPaintVariable, ANodeIsFlaggedVariableOnlyInItsVarForm) {
  Font font("color-v1-var.ttf");
  Recorder r;
  const std::vector<int> f = formats_of(font, 3, nullptr, &r);   // "bar"
  ASSERT_EQ(f.front(), static_cast<int>(GFNT_PAINT_VAR_TRANSFORM));
  for (const Node & n : r.nodes) {
    const bool is_var = (static_cast<int>(n.paint.format) & 1) != 0
        && n.paint.format != GFNT_PAINT_GLYPH && n.paint.format != GFNT_PAINT_COLR_GLYPH
        && n.paint.format != GFNT_PAINT_COLR_LAYERS;
    EXPECT_EQ(n.paint.variable, is_var) << static_cast<int>(n.paint.format);
  }
}

TEST(ColorPaint, ACycleThroughAColorGlyphIsStoppedByTheDepthLimit) {
  // Point glyph ghoti.alt's PaintColrGlyph (format 11, glyph 2) at itself (4).
  Font source("color-v1.ttf");
  size_t at = 0, length = 0;
  ASSERT_TRUE(table_range(source.bytes, "COLR", &at, &length));
  std::vector<uint8_t> bytes = source.bytes;
  size_t patched = 0;
  for (size_t i = at + 34; i + 2 < at + length; ++i) {
    if (bytes[i] == 11 && bytes[i + 1] == 0 && bytes[i + 2] == kA) {
      bytes[i + 2] = kAlt;
      ++patched;
    }
  }
  ASSERT_GE(patched, 1u);
  Font font(bytes);
  Recorder r;
  GFNT_PaintVisitor visitor = {record_enter, nullptr};
  GFNT_Error error;
  gfnt_error_clear(&error);
  EXPECT_EQ(gfnt_face_color_paint_walk(font.face, kAlt, nullptr, &visitor, &r, &error),
      GFNT_ERR_LIMIT);
  EXPECT_LE(r.nodes.size(), 130u);   // two nodes a level, sixty-four levels
}

TEST(ColorPaint, AVisitorThatStopsStopsTheWalk) {
  Font font("color-v1.ttf");
  GFNT_PaintVisitor visitor = {
      [](void *, const GFNT_ColorPaint *, size_t depth) { return depth < 2; },
      nullptr};
  EXPECT_EQ(gfnt_face_color_paint_walk(font.face, kA, nullptr, &visitor, nullptr,
                nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_color_paint_walk(font.face, kSpace, nullptr, &visitor, nullptr,
                nullptr), GFNT_ERR_INVALID);   // no record
  EXPECT_EQ(gfnt_face_color_paint_walk(font.face, kA, nullptr, nullptr, nullptr,
                nullptr), GFNT_ERR_INVALID);
}

TEST(ColorPaint, EveryByteOfAVersionOneTableOverwrittenGivesAnAnswerOrAnError) {
  for (const char * name : {"color-v1.ttf", "color-v1-var.ttf", "color-v1-varmap.ttf"}) {
    Font source(name);
    size_t at = 0, length = 0;
    ASSERT_TRUE(table_range(source.bytes, "COLR", &at, &length));
    size_t errors = 0;
    for (size_t i = 0; i < length; ++i) {
      for (uint8_t value : {uint8_t(0x00), uint8_t(0xFF), uint8_t(0x01)}) {
        std::vector<uint8_t> bytes = source.bytes;
        bytes[at + i] = value;
        Font font(bytes);
        if (!font.face) {
          continue;
        }
        std::vector<GFNT_F2Dot14> storage;
        GFNT_Variation moved = location(font, {650.0, 87.5}, &storage, false);
        for (const GFNT_Variation * v : {static_cast<const GFNT_Variation *>(nullptr),
                                         static_cast<const GFNT_Variation *>(&moved)}) {
          if (v && !gfnt_face_has_color_layers(font.face)) {
            continue;
          }
          for (uint32_t g = 0; g < 10; ++g) {
            Recorder r;
            GFNT_PaintVisitor visitor = {record_enter, nullptr};
            GFNT_Result res = gfnt_face_color_paint_walk(font.face, g, v, &visitor, &r,
                nullptr);
            EXPECT_TRUE(res == GFNT_OK || res == GFNT_ERR_CORRUPT
                        || res == GFNT_ERR_INVALID || res == GFNT_ERR_LIMIT
                        || res == GFNT_ERR_UNSUPPORTED) << name << " " << i;
            errors += res == GFNT_ERR_CORRUPT;
            for (const Node & n : r.nodes) {
              if (n.paint.color_line) {
                GFNT_ColorLine line;
                if (gfnt_face_color_line(font.face, &n.paint, &line, nullptr) == GFNT_OK) {
                  for (size_t k = 0; k < line.stop_count && k < 4; ++k) {
                    GFNT_ColorStop stop;
                    gfnt_face_color_stop(font.face, &n.paint, k, v, &stop, nullptr);
                  }
                }
              }
            }
            GFNT_ColorBox box;
            bool found;
            gfnt_face_color_clip(font.face, g, v, &box, &found, nullptr);
          }
          size_t layers = 0;
          GFNT_PaintRef ref;
          if (gfnt_face_color_layer_list_count(font.face, &layers, nullptr) == GFNT_OK) {
            for (size_t k = 0; k < layers && k < 6; ++k) {
              gfnt_face_color_layer_paint(font.face, k, &ref, nullptr);
            }
          }
        }
      }
    }
    EXPECT_GT(errors, 0u) << name << ": the sweep never reached an error arm";
  }
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
