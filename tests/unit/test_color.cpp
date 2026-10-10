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

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
