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
 * The subsetter and bitmap strikes: `CBLC`/`CBDT`, `EBLC`/`EBDT` and `sbix` keep the
 * kept glyphs' pictures, renumbered. Each test compares the subset with its source
 * through the public readers; `make check-oracle-subset-strikes` does the same through
 * fontTools and HarfBuzz over every font with strikes in the oracle image, Noto Color
 * Emoji among them.
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include <ghoti.io/font/bitmap.h>
#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/write.h>

#include <gtest/gtest.h>

namespace {

struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  explicit Font(const std::string & name) {
    std::ifstream in(gfnttest::data("fonts/" + name), std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    open();
  }
  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;
  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  void open() {
    ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED,
                  nullptr, nullptr, &blob, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr), GFNT_OK);
  }
};

struct Subset {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_SubsetMap map{};
  GFNT_Result result = GFNT_OK;
  GFNT_Error error;

  Subset(const GFNT_Face * source, std::vector<uint32_t> glyphs, bool retain = false) {
    GFNT_SubsetOptions o;
    gfnt_subset_options_init(&o);
    o.glyphs = glyphs.data();
    o.glyph_count = glyphs.size();
    o.retain_gids = retain;
    o.map = &map;
    result = gfnt_subset(source, &o, nullptr, nullptr, &blob, &error);
    if (result == GFNT_OK) {
      EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr), GFNT_OK);
    }
  }
  Subset(const Subset &) = delete;
  Subset & operator=(const Subset &) = delete;
  ~Subset() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    gfnt_subset_map_free(&map, nullptr);
  }
  long new_of(uint32_t old) const {
    for (size_t i = 0; i < map.count; ++i) {
      if (map.old_of_new[i] == old) {
        return static_cast<long>(i);
      }
    }
    return -1;
  }
};

/** Two pictures are the same: the bytes and every number the font states. */
void expect_same_picture(const GFNT_GlyphImage & a, const GFNT_GlyphImage & b) {
  EXPECT_EQ(a.format, b.format);
  ASSERT_EQ(a.length, b.length);
  EXPECT_EQ(std::memcmp(a.data, b.data, a.length), 0);
  EXPECT_EQ(a.width, b.width);
  EXPECT_EQ(a.height, b.height);
  EXPECT_EQ(a.bearing_x, b.bearing_x);
  EXPECT_EQ(a.bearing_y, b.bearing_y);
  EXPECT_EQ(a.origin_y, b.origin_y);
  EXPECT_EQ(a.advance, b.advance);
  EXPECT_EQ(a.strike.ppem_x, b.strike.ppem_x);
}

bool has_table(const GFNT_Face * face, const char * tag) {
  return gfnt_face_has_table(face, GFNT_TAG(tag[0], tag[1], tag[2], tag[3]));
}

}  // namespace

TEST(SubsetStrikes, TheKeptGlyphsPicturesAreCarriedUnderTheirNewNumbers) {
  Font source("color-cbdt.ttf");
  // Glyphs 1, 3, 5 and 7: index formats 1, 3, 5 and 4, image formats 17, 18, 19 and 17.
  Subset sub(source.face, {1, 3, 5, 7});
  ASSERT_EQ(sub.result, GFNT_OK);
  ASSERT_TRUE(has_table(sub.face, "CBLC"));
  ASSERT_TRUE(has_table(sub.face, "CBDT"));
  for (uint32_t old : {1u, 3u, 5u, 7u}) {
    SCOPED_TRACE(old);
    const long now = sub.new_of(old);
    ASSERT_GE(now, 1);
    GFNT_GlyphImage a{}, b{};
    ASSERT_EQ(gfnt_face_glyph_image(source.face, old, 0, &a, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_glyph_image(sub.face, static_cast<uint32_t>(now), 0, &b, nullptr), GFNT_OK);
    expect_same_picture(a, b);
  }
}

TEST(SubsetStrikes, AnImageFormatThatNeededItsIndexForMetricsIsRewrittenWithThem) {
  Font source("color-cbdt.ttf");
  Subset sub(source.face, {5});   // index 5, image 19: no metrics of its own
  ASSERT_EQ(sub.result, GFNT_OK);
  const long now = sub.new_of(5);
  ASSERT_GE(now, 1);
  GFNT_GlyphImage b{};
  ASSERT_EQ(gfnt_face_glyph_image(sub.face, static_cast<uint32_t>(now), 0, &b, nullptr), GFNT_OK);
  EXPECT_EQ(b.width, 8u);   // the index's metrics, now in the glyph's own data
  EXPECT_EQ(b.bearing_y, 8);
  EXPECT_EQ(b.advance, 9);
}

TEST(SubsetStrikes, TheVerticalHalfOfAnIndexsMetricsSurvivesTheRewrite) {
  Font source("color-cbdt.ttf");
  // Glyph 5: index 5, image 19, whose index states vertical metrics 5, -6, 12 (format 18
  // gains height 8, width 8, bearing 0, 8, advance 9 and then those three). The
  // 4-byte dataLen sits between the metrics and the PNG in format 18, so look for the
  // metrics ahead of it.
  Subset sub(source.face, {5});
  ASSERT_EQ(sub.result, GFNT_OK);
  const std::vector<uint8_t> metrics = {8, 8, 0, 8, 9, 5, 0xFA, 12};
  const uint8_t * d = gfnt_blob_data(sub.blob);
  const size_t n = gfnt_blob_size(sub.blob);
  bool found = false;
  static const uint8_t png[4] = {0x89, 'P', 'N', 'G'};
  for (size_t i = 0; i + 8 + 4 + 4 <= n && !found; ++i) {
    found = std::memcmp(d + i, metrics.data(), 8) == 0
        && std::memcmp(d + i + 12, png, 4) == 0;
  }
  EXPECT_TRUE(found);
}

TEST(SubsetStrikes, EveryStrikeStaysBecauseASizeChoosesTheStrike) {
  Font source("color-cbdt-sizes.ttf");
  // Glyph 5 is in the 20 by 20 strike alone. The others carry nothing of the subset but
  // stay as empty strikes, so a size still picks the strike it picked before.
  Subset sub(source.face, {5});
  ASSERT_EQ(sub.result, GFNT_OK);
  size_t strikes = 0;
  ASSERT_EQ(gfnt_face_strike_count(sub.face, &strikes, nullptr), GFNT_OK);
  EXPECT_EQ(strikes, 4u);
  GFNT_GlyphImage b{};
  EXPECT_EQ(gfnt_face_glyph_image(sub.face, static_cast<uint32_t>(sub.new_of(5)), 1, &b, nullptr), GFNT_OK);
  EXPECT_EQ(gfnt_face_glyph_image(sub.face, static_cast<uint32_t>(sub.new_of(5)), 0, &b, nullptr),
      GFNT_ERR_UNSUPPORTED);
}

TEST(SubsetStrikes, NoPictureKeptMeansNoStrikeTables) {
  Font source("color-cbdt.ttf");
  Subset sub(source.face, {10, 11});   // in no strike
  ASSERT_EQ(sub.result, GFNT_OK);
  EXPECT_FALSE(has_table(sub.face, "CBLC"));
  EXPECT_FALSE(has_table(sub.face, "CBDT"));
}

TEST(SubsetStrikes, RetainingIdsKeepsThePicturesAtTheirOwnIds) {
  Font source("color-cbdt.ttf");
  Subset sub(source.face, {3, 7}, true);
  ASSERT_EQ(sub.result, GFNT_OK);
  for (uint32_t old : {3u, 7u}) {
    GFNT_GlyphImage a{}, b{};
    ASSERT_EQ(gfnt_face_glyph_image(source.face, old, 0, &a, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_glyph_image(sub.face, old, 0, &b, nullptr), GFNT_OK);
    expect_same_picture(a, b);
  }
  GFNT_GlyphImage dropped{};
  EXPECT_EQ(gfnt_face_glyph_image(sub.face, 4, 0, &dropped, nullptr), GFNT_ERR_UNSUPPORTED);
}

TEST(SubsetStrikes, ABitmapOnlyFontIsSubsetByItsPictures) {
  Font source("color-cbdt-only.ttf");
  ASSERT_FALSE(has_table(source.face, "glyf"));
  Subset sub(source.face, {2, 4});
  ASSERT_EQ(sub.result, GFNT_OK) << sub.error.message;
  EXPECT_FALSE(has_table(sub.face, "glyf"));
  EXPECT_FALSE(has_table(sub.face, "loca"));
  EXPECT_TRUE(has_table(sub.face, "CBDT"));
  for (uint32_t old : {2u, 4u}) {
    GFNT_GlyphImage a{}, b{};
    ASSERT_EQ(gfnt_face_glyph_image(source.face, old, 0, &a, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_glyph_image(sub.face, static_cast<uint32_t>(sub.new_of(old)), 0, &b, nullptr),
        GFNT_OK);
    expect_same_picture(a, b);
  }
}

TEST(SubsetStrikes, SbixKeepsAPictureAndFollowsADupeToItsNewNumber) {
  Font source("color-sbix.ttf");
  // Glyph 4 dupes glyph 3 which dupes glyph 1. Keep 3 and 4 and not 1: glyph 3 can no
  // longer point at its target and carries the picture, and glyph 4 still points at 3.
  Subset sub(source.face, {3, 4});
  ASSERT_EQ(sub.result, GFNT_OK) << sub.error.message;
  ASSERT_TRUE(has_table(sub.face, "sbix"));
  for (uint32_t old : {3u, 4u}) {
    SCOPED_TRACE(old);
    for (size_t strike = 0; strike < 2; ++strike) {
      GFNT_GlyphImage a{}, b{};
      const GFNT_Result want = gfnt_face_glyph_image(source.face, old, strike, &a, nullptr);
      const GFNT_Result got = gfnt_face_glyph_image(sub.face,
          static_cast<uint32_t>(sub.new_of(old)), strike, &b, nullptr);
      ASSERT_EQ(got, want);
      if (want == GFNT_OK) {
        expect_same_picture(a, b);
      }
    }
  }
}

TEST(SubsetStrikes, SbixKeepsAJpegAndATiffAndTheOddRecords) {
  Font source("color-sbix.ttf");
  Subset sub(source.face, {5, 6, 7});
  ASSERT_EQ(sub.result, GFNT_OK);
  for (uint32_t old : {5u, 6u}) {
    GFNT_GlyphImage a{}, b{};
    ASSERT_EQ(gfnt_face_glyph_image(source.face, old, 1, &a, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_glyph_image(sub.face, static_cast<uint32_t>(sub.new_of(old)), 1, &b, nullptr),
        GFNT_OK);
    expect_same_picture(a, b);
  }
  // The mask record is carried as it was and is still refused by name.
  GFNT_GlyphImage b{};
  GFNT_Error error;
  EXPECT_EQ(gfnt_face_glyph_image(sub.face, static_cast<uint32_t>(sub.new_of(7)), 1, &b, &error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::strstr(error.message, "graphicType"), nullptr);
}

TEST(SubsetStrikes, SbixFlagsAndVersionAreKept) {
  Font source("color-sbix-dupes.ttf");   // flags 3
  Subset sub(source.face, {1, 2});
  ASSERT_EQ(sub.result, GFNT_OK);
  GFNT_GlyphImage b{};
  ASSERT_EQ(gfnt_face_glyph_image(sub.face, static_cast<uint32_t>(sub.new_of(2)), 0, &b, nullptr),
      GFNT_OK);
  EXPECT_TRUE(b.draw_outlines);
}

TEST(SubsetStrikes, MonochromeStrikesAreCarriedToo) {
  Font source("strikes.ttf");
  Subset sub(source.face, {1, 3});
  ASSERT_EQ(sub.result, GFNT_OK);
  ASSERT_TRUE(has_table(sub.face, "EBLC"));
  size_t strikes = 0;
  ASSERT_EQ(gfnt_face_strike_count(sub.face, &strikes, nullptr), GFNT_OK);
  EXPECT_EQ(strikes, 3u);
  for (uint32_t old : {1u, 3u}) {
    for (size_t strike = 0; strike < 3; ++strike) {
      GFNT_BitmapGlyph a{}, b{};
      ASSERT_EQ(gfnt_face_glyph_bitmap(source.face, old, strike, &a, nullptr), GFNT_OK);
      ASSERT_EQ(gfnt_face_glyph_bitmap(sub.face, static_cast<uint32_t>(sub.new_of(old)), strike,
                    &b, nullptr), GFNT_OK);
      EXPECT_EQ(a.width, b.width);
      EXPECT_EQ(a.height, b.height);
      EXPECT_EQ(a.bearing_x, b.bearing_x);
      EXPECT_EQ(a.bearing_y, b.bearing_y);
      EXPECT_EQ(a.advance, b.advance);
      ASSERT_EQ(a.height * a.stride, b.height * b.stride);
      EXPECT_EQ(std::memcmp(a.bits, b.bits, a.height * a.stride), 0);
    }
  }
}

TEST(SubsetStrikes, TheScaledSizesTableRidesAlongBecauseNoStrikeMoves) {
  // The EBSC fixtures live in the corpus, not here; what is checked is that a font
  // without one gains none.
  Font source("strikes.ttf");
  Subset sub(source.face, {1});
  ASSERT_EQ(sub.result, GFNT_OK);
  EXPECT_FALSE(has_table(sub.face, "EBSC"));
}

TEST(SubsetStrikes, ACompositeBitmapGlyphIsRefusedByName) {
  Font source("strike-composite.ttf");
  Subset sub(source.face, {6});   // an image format 8 composite
  EXPECT_EQ(sub.result, GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::strstr(sub.error.message, "composite"), nullptr) << sub.error.message;
}

TEST(SubsetStrikes, AnAllocationFailureAnywhereIsReported) {
  for (const char * name : {"color-cbdt.ttf", "color-sbix.ttf", "strikes.ttf"}) {
    Font source(name);
    GFNT_SubsetOptions o;
    gfnt_subset_options_init(&o);
    std::vector<uint32_t> glyphs = {1, 3};
    o.glyphs = glyphs.data();
    o.glyph_count = glyphs.size();
    size_t requests = 0;
    {
      gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
      GFNT_Blob * out = nullptr;
      ASSERT_EQ(gfnt_subset(source.face, &o, nullptr, counting.get(), &out, nullptr), GFNT_OK);
      requests = counting.requests();
      gfnt_blob_destroy(out);
      EXPECT_EQ(counting.live(), 0u) << name;
    }
    ASSERT_GT(requests, 10u);
    for (size_t fail = 0; fail < requests; ++fail) {
      gfnttest::FailingAllocator failing(fail);
      GFNT_Blob * out = nullptr;
      const GFNT_Result r = gfnt_subset(source.face, &o, nullptr, failing.get(), &out, nullptr);
      if (r != GFNT_OK) {
        EXPECT_EQ(r, GFNT_ERR_OOM) << name << " request " << fail;
        EXPECT_EQ(out, nullptr);
      }
      gfnt_blob_destroy(out);
      EXPECT_EQ(failing.live(), 0u) << name << " request " << fail << " leaked";
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
