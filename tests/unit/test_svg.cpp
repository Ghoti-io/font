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
 * The `SVG ` table: the index, the lookup of a glyph's document, gzip documents, and the
 * ways a table can be wrong. The tables are built here, byte by byte, from the specification.
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fstream>

#include <ghoti.io/font/svg.h>
#include <ghoti.io/font/write.h>

namespace {

struct Doc {
  uint16_t first, last;
  std::string bytes;
};

// `<svg id="x"><g id="glyph3"/></svg>` as gzip writes it (mtime 0).
const uint8_t kGzipped[] = {
    0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xFF, 0xB3, 0x29, 0x2E, 0x4B, 0x57,
    0xC8, 0x4C, 0xB1, 0x55, 0xAA, 0x50, 0xB2, 0xB3, 0x81, 0xB0, 0xD2, 0x73, 0x2A, 0x0B, 0x32,
    0x8C, 0x95, 0xF4, 0xED, 0x6C, 0xF4, 0x81, 0x92, 0x76, 0x00, 0x51, 0x73, 0xEF, 0x2F, 0x22,
    0x00, 0x00, 0x00};
const char kPlainText[] = "<svg><g id=\"glyph1\"/></svg>";
const char kGzippedText[] = "<svg id=\"x\"><g id=\"glyph3\"/></svg>";

/** An `SVG ` table of the documents, in the order given. */
std::vector<uint8_t> svg_table(const std::vector<Doc> & docs, uint16_t version = 0,
    size_t list_at = 10) {
  std::vector<uint8_t> t;
  gfnttest::put_u16(t, version);
  gfnttest::put_u32(t, static_cast<uint32_t>(list_at));
  gfnttest::put_u32(t, 0);                    // reserved
  gfnttest::put_u16(t, static_cast<uint16_t>(docs.size()));
  uint32_t offset = static_cast<uint32_t>(2 + 12 * docs.size());
  for (const Doc & d : docs) {
    gfnttest::put_u16(t, d.first);
    gfnttest::put_u16(t, d.last);
    gfnttest::put_u32(t, offset);
    gfnttest::put_u32(t, static_cast<uint32_t>(d.bytes.size()));
    offset += static_cast<uint32_t>(d.bytes.size());
  }
  for (const Doc & d : docs) {
    t.insert(t.end(), d.bytes.begin(), d.bytes.end());
  }
  return t;
}

struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  explicit Font(const std::vector<uint8_t> & svg, bool whole = false) {
    if (whole) {
      bytes = svg;
      open();
      return;
    }
    std::vector<std::pair<uint16_t, int16_t>> metrics(8, {500, 0});
    std::vector<gfnttest::Table> tables = {
        {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
        {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, 8)},
        {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics)},
        {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(8)},
    };
    if (!svg.empty()) {
      tables.push_back({GFNT_TAG('S', 'V', 'G', ' '), svg});
    }
    bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
    open();
  }
  void open() {
    GFNT_Error error;
    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED, nullptr,
                  nullptr, &blob, &error), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
  }
  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;
  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
};

std::string text_of(const Font & font, const GFNT_SvgDocument & doc) {
  GFNT_Blob * blob = nullptr;
  EXPECT_EQ(gfnt_svg_document_inflate(font.face, &doc, nullptr, &blob, nullptr), GFNT_OK);
  std::string out;
  if (blob) {
    out.assign(reinterpret_cast<const char *>(gfnt_blob_data(blob)), gfnt_blob_size(blob));
    gfnt_blob_destroy(blob);
  }
  return out;
}

const std::vector<Doc> kDocs = {
    {1, 2, kPlainText},
    {3, 3, std::string(reinterpret_cast<const char *>(kGzipped), sizeof kGzipped)},
    {6, 7, "<svg/>"},
};

}  // namespace

TEST(Svg, AFaceWithoutTheTableHasNoDocuments) {
  Font font({});
  EXPECT_FALSE(gfnt_face_has_svg(font.face));
  size_t n = 0;
  EXPECT_EQ(gfnt_face_svg_document_count(font.face, &n, nullptr), GFNT_ERR_UNSUPPORTED);
  GFNT_SvgDocument doc{};
  bool found = true;
  EXPECT_EQ(gfnt_face_glyph_svg(font.face, 1, &doc, &found, nullptr), GFNT_ERR_UNSUPPORTED);
  EXPECT_FALSE(found);
}

TEST(Svg, TheIndexListsEveryDocumentWithItsRangeAndWhetherItIsCompressed) {
  Font font(svg_table(kDocs));
  ASSERT_TRUE(gfnt_face_has_svg(font.face));
  size_t n = 0;
  ASSERT_EQ(gfnt_face_svg_document_count(font.face, &n, nullptr), GFNT_OK);
  ASSERT_EQ(n, 3u);
  GFNT_SvgDocument doc{};
  ASSERT_EQ(gfnt_face_svg_document_at(font.face, 0, &doc, nullptr), GFNT_OK);
  EXPECT_EQ(doc.first_glyph, 1u);
  EXPECT_EQ(doc.last_glyph, 2u);
  EXPECT_FALSE(doc.compressed);
  EXPECT_EQ(doc.length, sizeof kPlainText - 1);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(doc.data), doc.length), kPlainText);
  ASSERT_EQ(gfnt_face_svg_document_at(font.face, 1, &doc, nullptr), GFNT_OK);
  EXPECT_TRUE(doc.compressed);
  EXPECT_EQ(doc.length, sizeof kGzipped);
  EXPECT_EQ(gfnt_face_svg_document_at(font.face, 3, &doc, nullptr), GFNT_ERR_INVALID);
}

TEST(Svg, AGlyphFindsTheDocumentWhoseRangeHoldsItAndAGapFindsNone) {
  Font font(svg_table(kDocs));
  GFNT_SvgDocument doc{};
  bool found = false;
  for (uint32_t g : {1u, 2u}) {
    ASSERT_EQ(gfnt_face_glyph_svg(font.face, g, &doc, &found, nullptr), GFNT_OK);
    ASSERT_TRUE(found) << g;
    EXPECT_EQ(doc.first_glyph, 1u);
    EXPECT_EQ(text_of(font, doc), kPlainText);
  }
  ASSERT_EQ(gfnt_face_glyph_svg(font.face, 3, &doc, &found, nullptr), GFNT_OK);
  ASSERT_TRUE(found);
  EXPECT_EQ(text_of(font, doc), kGzippedText) << "inflated";
  ASSERT_EQ(gfnt_face_glyph_svg(font.face, 7, &doc, &found, nullptr), GFNT_OK);
  ASSERT_TRUE(found);
  EXPECT_EQ(doc.first_glyph, 6u);
  for (uint32_t g : {0u, 4u, 5u, 8u, 65535u, 70000u}) {
    found = true;
    EXPECT_EQ(gfnt_face_glyph_svg(font.face, g, &doc, &found, nullptr), GFNT_OK) << g;
    EXPECT_FALSE(found) << g;
  }
}

TEST(Svg, ATableOfOneDocumentOrNoneIsRead) {
  Font none(svg_table({}));
  size_t n = 9;
  ASSERT_EQ(gfnt_face_svg_document_count(none.face, &n, nullptr), GFNT_OK);
  EXPECT_EQ(n, 0u);
  GFNT_SvgDocument doc{};
  bool found = true;
  ASSERT_EQ(gfnt_face_glyph_svg(none.face, 1, &doc, &found, nullptr), GFNT_OK);
  EXPECT_FALSE(found);
  Font one(svg_table({{0, 0xFFFF, "<svg/>"}}));
  ASSERT_EQ(gfnt_face_glyph_svg(one.face, 0xFFFF, &doc, &found, nullptr), GFNT_OK);
  EXPECT_TRUE(found);
}

TEST(Svg, ATableThatIsWrongIsRefusedAtTheRecordItSpoils) {
  GFNT_SvgDocument doc{};
  bool found = false;
  // A version other than 0.
  Font version(svg_table(kDocs, 1));
  size_t n = 0;
  EXPECT_EQ(gfnt_face_svg_document_count(version.face, &n, nullptr), GFNT_ERR_UNSUPPORTED);
  // A list that starts past the table.
  Font list(svg_table(kDocs, 0, 5000));
  EXPECT_EQ(gfnt_face_svg_document_count(list.face, &n, nullptr), GFNT_ERR_CORRUPT);
  // A document that runs past the end.
  auto table = svg_table(kDocs);
  table[10 + 2 + 8 + 3] = 0xFF;   // the first record's length, high byte of its low word
  Font long_doc(table);
  EXPECT_EQ(gfnt_face_glyph_svg(long_doc.face, 1, &doc, &found, nullptr), GFNT_ERR_CORRUPT);
  EXPECT_FALSE(found);
  // A document that runs one byte past the end, and one that ends exactly at it.
  table = svg_table({{1, 1, "<svg/>"}});
  Font exact(table);
  EXPECT_EQ(gfnt_face_glyph_svg(exact.face, 1, &doc, &found, nullptr), GFNT_OK);
  EXPECT_TRUE(found);
  table[10 + 2 + 11] += 1;   // the record's length, low byte
  Font over(table);
  EXPECT_EQ(gfnt_face_glyph_svg(over.face, 1, &doc, &found, nullptr), GFNT_ERR_CORRUPT);
  // A range that ends before it starts.
  table = svg_table({{5, 2, "<svg/>"}});
  Font backwards(table);
  EXPECT_EQ(gfnt_face_svg_document_at(backwards.face, 0, &doc, nullptr), GFNT_ERR_CORRUPT);
  // A list whose records are cut by the table's end.
  table = svg_table(kDocs);
  table.resize(10 + 2 + 20);
  Font cut(table);
  EXPECT_EQ(gfnt_face_svg_document_count(cut.face, &n, nullptr), GFNT_ERR_CORRUPT);
}

TEST(Svg, ACompressedDocumentThatIsNotAGzipMemberIsCorruptAndALimitIsKept) {
  // The gzip magic and method, then rubbish.
  std::string bad(reinterpret_cast<const char *>(kGzipped), sizeof kGzipped);
  bad[20] ^= 0x55;
  Font font(svg_table({{1, 1, bad}}));
  GFNT_SvgDocument doc{};
  bool found = false;
  ASSERT_EQ(gfnt_face_glyph_svg(font.face, 1, &doc, &found, nullptr), GFNT_OK);
  ASSERT_TRUE(found);
  ASSERT_TRUE(doc.compressed);
  GFNT_Blob * blob = nullptr;
  EXPECT_EQ(gfnt_svg_document_inflate(font.face, &doc, nullptr, &blob, nullptr), GFNT_ERR_CORRUPT);
  EXPECT_EQ(blob, nullptr);
  // Arguments.
  EXPECT_EQ(gfnt_svg_document_inflate(nullptr, &doc, nullptr, &blob, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_svg_document_inflate(font.face, nullptr, nullptr, &blob, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_svg_document_inflate(font.face, &doc, nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
}

TEST(Svg, ADocumentOfThreeBytesStartingWithTheMagicIsNotMistakenForGzipWithoutTheMethod) {
  Font font(svg_table({{1, 1, std::string("\x1F\x8B\x07", 3)}}));
  GFNT_SvgDocument doc{};
  ASSERT_EQ(gfnt_face_svg_document_at(font.face, 0, &doc, nullptr), GFNT_OK);
  EXPECT_FALSE(doc.compressed);
  Font tiny(svg_table({{1, 1, std::string("\x1F\x8B", 2)}}));
  ASSERT_EQ(gfnt_face_svg_document_at(tiny.face, 0, &doc, nullptr), GFNT_OK);
  EXPECT_FALSE(doc.compressed);
}

namespace {

/** The committed fixture fontTools wrote: documents for glyphs 0, 2-3, 4 and 6-7. */
struct FixtureFont {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  FixtureFont() {
    std::ifstream in(gfnttest::data("fonts/svg-docs.ttf"), std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED, nullptr,
                  nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr), GFNT_OK);
  }
  FixtureFont(const FixtureFont &) = delete;
  FixtureFont & operator=(const FixtureFont &) = delete;
  ~FixtureFont() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
};

}  // namespace

TEST(SvgFixture, FontToolsDocumentsAreReadWithTheirRangesAndInflated) {
  FixtureFont font;
  size_t n = 0;
  ASSERT_EQ(gfnt_face_svg_document_count(font.face, &n, nullptr), GFNT_OK);
  ASSERT_EQ(n, 4u);
  const uint32_t firsts[] = {0, 2, 4, 6}, lasts[] = {0, 3, 4, 7};
  const bool compressed[] = {true, false, true, false};
  for (size_t i = 0; i < 4; ++i) {
    GFNT_SvgDocument doc{};
    ASSERT_EQ(gfnt_face_svg_document_at(font.face, i, &doc, nullptr), GFNT_OK);
    EXPECT_EQ(doc.first_glyph, firsts[i]) << i;
    EXPECT_EQ(doc.last_glyph, lasts[i]) << i;
    EXPECT_EQ(doc.compressed, compressed[i]) << i;
  }
  GFNT_SvgDocument doc{};
  bool found = false;
  ASSERT_EQ(gfnt_face_glyph_svg(font.face, 4, &doc, &found, nullptr), GFNT_OK);
  ASSERT_TRUE(found);
  GFNT_Blob * text = nullptr;
  ASSERT_EQ(gfnt_svg_document_inflate(font.face, &doc, nullptr, &text, nullptr), GFNT_OK);
  const std::string svg(reinterpret_cast<const char *>(gfnt_blob_data(text)), gfnt_blob_size(text));
  gfnt_blob_destroy(text);
  EXPECT_EQ(svg.size(), 499u);
  EXPECT_NE(svg.find("id=\"glyph4\""), std::string::npos);
  EXPECT_LT(doc.length, svg.size()) << "it is stored compressed";
  for (uint32_t g : {1u, 5u, 8u}) {
    ASSERT_EQ(gfnt_face_glyph_svg(font.face, g, &doc, &found, nullptr), GFNT_OK);
    EXPECT_FALSE(found) << g;
  }
}

namespace {

std::vector<std::pair<uint32_t, uint32_t>> ranges_of(const GFNT_Face * face) {
  std::vector<std::pair<uint32_t, uint32_t>> out;
  size_t n = 0;
  if (gfnt_face_svg_document_count(face, &n, nullptr) != GFNT_OK) {
    return out;
  }
  for (size_t i = 0; i < n; ++i) {
    GFNT_SvgDocument doc{};
    EXPECT_EQ(gfnt_face_svg_document_at(face, i, &doc, nullptr), GFNT_OK);
    out.push_back({doc.first_glyph, doc.last_glyph});
  }
  return out;
}

GFNT_Result subset_of(const GFNT_Face * source, std::vector<uint32_t> glyphs, bool retain,
    GFNT_Blob ** blob, GFNT_Face ** face) {
  GFNT_SubsetOptions o;
  gfnt_subset_options_init(&o);
  o.glyphs = glyphs.data();
  o.glyph_count = glyphs.size();
  o.retain_gids = retain;
  GFNT_Result r = gfnt_subset(source, &o, nullptr, nullptr, blob, nullptr);
  if (r == GFNT_OK) {
    EXPECT_EQ(gfnt_face_load(*blob, 0, nullptr, nullptr, face, nullptr), GFNT_OK);
  }
  return r;
}

}  // namespace

TEST(SvgSubset, RetainedIdsKeepTheDocumentsThatServeAKeptGlyphAsTheyWere) {
  FixtureFont font;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * sub = nullptr;
  ASSERT_EQ(subset_of(font.face, {3, 6}, true, &blob, &sub), GFNT_OK);
  // Glyph 0 is always kept; 3 is in 2-3 and 6 in 6-7. The one for glyph 4 goes.
  const auto kept = ranges_of(sub);
  ASSERT_EQ(kept.size(), 3u);
  EXPECT_EQ(kept[0], (std::pair<uint32_t, uint32_t>{0, 0}));
  EXPECT_EQ(kept[1], (std::pair<uint32_t, uint32_t>{2, 3}));
  EXPECT_EQ(kept[2], (std::pair<uint32_t, uint32_t>{6, 7}));
  // Each is the same bytes as in the source, compressed or not.
  for (size_t i = 0; i < 3; ++i) {
    GFNT_SvgDocument was{}, now{};
    bool found = false;
    ASSERT_EQ(gfnt_face_glyph_svg(sub, kept[i].first, &now, &found, nullptr), GFNT_OK);
    ASSERT_TRUE(found);
    ASSERT_EQ(gfnt_face_glyph_svg(font.face, kept[i].first, &was, &found, nullptr), GFNT_OK);
    ASSERT_EQ(now.length, was.length);
    EXPECT_EQ(std::memcmp(now.data, was.data, was.length), 0);
    EXPECT_EQ(now.compressed, was.compressed);
  }
  gfnt_face_free(sub);
  gfnt_blob_destroy(blob);
}

TEST(SvgSubset, ASubsetWithNoKeptGlyphInAnyDocumentHasNoTable) {
  FixtureFont font;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * sub = nullptr;
  ASSERT_EQ(subset_of(font.face, {5}, true, &blob, &sub), GFNT_OK);
  EXPECT_EQ(ranges_of(sub).size(), 1u) << "glyph 0's document";
  gfnt_face_free(sub);
  gfnt_blob_destroy(blob);
  // Move the first document (glyph 0's) to glyph 1 and keep only glyph 5, which no document
  // serves: nothing is left to carry, and the table is not written.
  std::vector<uint8_t> moved = font.bytes;
  const uint8_t * table = nullptr;
  size_t length = 0;
  ASSERT_EQ(gfnt_face_table_data(font.face, GFNT_TAG('S', 'V', 'G', ' '), &table, &length),
      GFNT_OK);
  const size_t at = static_cast<size_t>(table - font.bytes.data()) + 10 + 2;
  moved[at + 1] = 1;
  moved[at + 3] = 1;
  Font other(moved, true);
  ASSERT_EQ(subset_of(other.face, {5}, true, &blob, &sub), GFNT_OK);
  EXPECT_FALSE(gfnt_face_has_svg(sub));
  gfnt_face_free(sub);
  gfnt_blob_destroy(blob);
}

TEST(SvgSubset, RenumberedIdsLeaveTheTableOutBecauseTheDocumentsNameTheOldIds) {
  FixtureFont font;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * sub = nullptr;
  ASSERT_EQ(subset_of(font.face, {3, 6}, false, &blob, &sub), GFNT_OK);
  EXPECT_FALSE(gfnt_face_has_svg(sub));
  gfnt_face_free(sub);
  gfnt_blob_destroy(blob);
}


int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
