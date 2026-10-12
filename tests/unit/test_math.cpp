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
 * The subsetter and `MATH`. A glyph that is kept brings the sizes it grows to and the pieces it is
 * built from, the table keeps the entries of the kept glyphs under their new ids, and the device
 * tables behind the values go with them. The library has no reader for `MATH`, so these tests read
 * the table's bytes themselves; `make check-oracle-subset-math` holds real fonts to fontTools.
 *
 * `math.ttf` glyph ids: x 2, y 3, z 4, parenleft 5 (sizes 6 and 7, pieces 8 9 10), bar 11 (size
 * 12), integral 13 (size 14, pieces 15 16 17), overbrace 18 (size 19, pieces 20 21 22 23), hat 24.
 */

#include "test_helpers.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/math.h>

#include <gtest/gtest.h>

namespace {

enum : uint32_t { kX = 2, kY = 3, kZ = 4, kParen = 5, kBar = 11, kBarV1 = 12, kIntegral = 13,
  kOverbrace = 18, kHat = 24, kParenV2 = 7 };

struct Face {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  explicit Face(const std::string & name) {
    std::ifstream in(gfnttest::data("fonts/" + name), std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED, nullptr,
                  nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr), GFNT_OK);
  }
  Face(const Face &) = delete;
  Face & operator=(const Face &) = delete;
  ~Face() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
};

int32_t constant(const Face & f, GFNT_MathConstant c, const GFNT_Variation * v = nullptr) {
  int32_t out = -999999;
  EXPECT_EQ(gfnt_face_math_constant(f.face, v, c, &out, nullptr), GFNT_OK);
  return out;
}

GFNT_Variation at(const GFNT_F2Dot14 * coords) {
  GFNT_Variation v{};
  v.coords = coords;
  v.count = 2;
  return v;
}

}  // namespace

TEST(MathReader, ConstantsComeFromBothKindsOfEntry) {
  Face f("math.ttf");
  ASSERT_TRUE(gfnt_face_has_math(f.face));
  // The four plain values, the value records (100 + 7n), and the plain percentage after them.
  EXPECT_EQ(constant(f, GFNT_MATH_MATH_LEADING), 100);
  EXPECT_EQ(constant(f, GFNT_MATH_AXIS_HEIGHT), 107);
  EXPECT_EQ(constant(f, GFNT_MATH_RADICAL_KERN_AFTER_DEGREE), 100 + 7 * 50);
  EXPECT_EQ(constant(f, GFNT_MATH_RADICAL_DEGREE_BOTTOM_RAISE_PERCENT), 60);
  int32_t sink = 0;
  EXPECT_EQ(gfnt_face_math_constant(f.face, nullptr, GFNT_MATH_CONSTANT_COUNT, &sink, nullptr),
      GFNT_ERR_INVALID);
}

TEST(MathReader, AGlyphTheTableDoesNotListIsAbsentAndNotAnError) {
  Face f("math.ttf");
  bool present = true;
  int32_t v = 7;
  ASSERT_EQ(gfnt_face_math_glyph_value(f.face, nullptr, kX, GFNT_MATH_ITALICS_CORRECTION, &present,
      &v, nullptr), GFNT_OK);
  EXPECT_TRUE(present);
  EXPECT_EQ(v, 31);
  ASSERT_EQ(gfnt_face_math_glyph_value(f.face, nullptr, kBar, GFNT_MATH_ITALICS_CORRECTION,
      &present, &v, nullptr), GFNT_OK);
  EXPECT_FALSE(present);
  EXPECT_EQ(v, 0);
  ASSERT_EQ(gfnt_face_math_glyph_value(f.face, nullptr, kHat, GFNT_MATH_TOP_ACCENT_ATTACHMENT,
      &present, &v, nullptr), GFNT_OK);
  EXPECT_TRUE(present);
  EXPECT_EQ(v, 280);
  // A hinting device table is for a pixel size; the design value is what comes back.
  ASSERT_EQ(gfnt_face_math_glyph_value(f.face, nullptr, kY, GFNT_MATH_ITALICS_CORRECTION, &present,
      &v, nullptr), GFNT_OK);
  EXPECT_EQ(v, 32);
  bool extended = false;
  ASSERT_EQ(gfnt_face_math_is_extended_shape(f.face, kIntegral, &extended, nullptr), GFNT_OK);
  EXPECT_TRUE(extended);
  ASSERT_EQ(gfnt_face_math_is_extended_shape(f.face, kX, &extended, nullptr), GFNT_OK);
  EXPECT_FALSE(extended);
}

TEST(MathReader, KernsGiveTheTablesHeightsAndOneMoreValue) {
  Face f("math.ttf");
  bool present = false;
  size_t heights = 0;
  ASSERT_EQ(gfnt_face_math_kern_count(f.face, kX, GFNT_MATH_KERN_TOP_RIGHT, &present, &heights,
      nullptr), GFNT_OK);
  ASSERT_TRUE(present);
  ASSERT_EQ(heights, 2u);
  const int32_t want_h[] = {100, 300};
  const int32_t want_k[] = {10, 20, 30};
  for (size_t i = 0; i < 2; ++i) {
    int32_t h = 0;
    ASSERT_EQ(gfnt_face_math_kern_height_at(f.face, nullptr, kX, GFNT_MATH_KERN_TOP_RIGHT, i, &h,
        nullptr), GFNT_OK);
    EXPECT_EQ(h, want_h[i]);
  }
  for (size_t i = 0; i <= 2; ++i) {
    int32_t k = 0;
    ASSERT_EQ(gfnt_face_math_kern_value_at(f.face, nullptr, kX, GFNT_MATH_KERN_TOP_RIGHT, i, &k,
        nullptr), GFNT_OK);
    EXPECT_EQ(k, want_k[i]);
  }
  int32_t out = 0;
  EXPECT_EQ(gfnt_face_math_kern_height_at(f.face, nullptr, kX, GFNT_MATH_KERN_TOP_RIGHT, 2, &out,
      nullptr), GFNT_ERR_INVALID) << "there is one height fewer than values";
  EXPECT_EQ(gfnt_face_math_kern_value_at(f.face, nullptr, kX, GFNT_MATH_KERN_TOP_RIGHT, 3, &out,
      nullptr), GFNT_ERR_INVALID);
  // Corners are told apart: x has the top-right and bottom-left only.
  ASSERT_EQ(gfnt_face_math_kern_count(f.face, kX, GFNT_MATH_KERN_TOP_LEFT, &present, &heights,
      nullptr), GFNT_OK);
  EXPECT_FALSE(present);
  ASSERT_EQ(gfnt_face_math_kern_count(f.face, kX, GFNT_MATH_KERN_BOTTOM_LEFT, &present, &heights,
      nullptr), GFNT_OK);
  EXPECT_TRUE(present);
  EXPECT_EQ(heights, 1u);
}

TEST(MathReader, VariantsAndAssemblies) {
  Face f("math.ttf");
  uint32_t overlap = 0;
  ASSERT_EQ(gfnt_face_math_min_connector_overlap(f.face, &overlap, nullptr), GFNT_OK);
  EXPECT_EQ(overlap, 50u);
  size_t n = 0;
  ASSERT_EQ(gfnt_face_math_variant_count(f.face, kParen, true, &n, nullptr), GFNT_OK);
  ASSERT_EQ(n, 3u);
  GFNT_MathVariant v{};
  ASSERT_EQ(gfnt_face_math_variant_at(f.face, kParen, true, 2, &v, nullptr), GFNT_OK);
  EXPECT_EQ(v.glyph, kParenV2);
  EXPECT_EQ(v.advance, 2000u);
  ASSERT_EQ(gfnt_face_math_variant_count(f.face, kParen, false, &n, nullptr), GFNT_OK);
  EXPECT_EQ(n, 0u) << "the parenthesis grows vertically only";
  ASSERT_EQ(gfnt_face_math_variant_count(f.face, kOverbrace, false, &n, nullptr), GFNT_OK);
  EXPECT_EQ(n, 2u);
  EXPECT_EQ(gfnt_face_math_variant_at(f.face, kParen, true, 3, &v, nullptr), GFNT_ERR_INVALID);

  ASSERT_EQ(gfnt_face_math_assembly_count(f.face, kParen, true, &n, nullptr), GFNT_OK);
  ASSERT_EQ(n, 3u);
  GFNT_MathPart p{};
  ASSERT_EQ(gfnt_face_math_assembly_part_at(f.face, kParen, true, 1, &p, nullptr), GFNT_OK);
  EXPECT_EQ(p.glyph, 9u);
  EXPECT_EQ(p.start_connector_length, 100u);
  EXPECT_EQ(p.end_connector_length, 100u);
  EXPECT_EQ(p.full_advance, 300u);
  EXPECT_EQ(p.flags, GFNT_MATH_PART_EXTENDER);
  int32_t italics = 0;
  ASSERT_EQ(gfnt_face_math_assembly_italics_correction(f.face, nullptr, kParen, true, &italics,
      nullptr), GFNT_OK);
  EXPECT_EQ(italics, 7);
  // A glyph with variants and no assembly.
  ASSERT_EQ(gfnt_face_math_assembly_count(f.face, kBar, true, &n, nullptr), GFNT_OK);
  EXPECT_EQ(n, 0u);
  EXPECT_EQ(gfnt_face_math_assembly_italics_correction(f.face, nullptr, kBar, true, &italics,
      nullptr), GFNT_ERR_INVALID);
  // The same glyph in the other direction is a different construction.
  ASSERT_EQ(gfnt_face_math_assembly_count(f.face, kOverbrace, false, &n, nullptr), GFNT_OK);
  EXPECT_EQ(n, 5u);
  ASSERT_EQ(gfnt_face_math_assembly_count(f.face, kOverbrace, true, &n, nullptr), GFNT_OK);
  EXPECT_EQ(n, 0u);
}

TEST(MathReader, VariationIndicesMoveValuesAndOtherDevicesDoNot) {
  Face f("math-variable.ttf");
  // The store has two regions (wght, then wdth, each rising from 0 to 1) and the entries named
  // here are rows [8, 8] (AxisHeight), [-4, 2] (z's italics) and [5, -2] (the assembly's).
  const GFNT_F2Dot14 def[2] = {0, 0};
  const GFNT_F2Dot14 heavy[2] = {16384, 0};
  const GFNT_F2Dot14 wide[2] = {0, 16384};
  const GFNT_F2Dot14 half[2] = {8192, 0};
  GFNT_Variation vd = at(def);
  GFNT_Variation vh = at(heavy);
  GFNT_Variation vw = at(wide);
  GFNT_Variation vm = at(half);
  EXPECT_EQ(constant(f, GFNT_MATH_AXIS_HEIGHT), 107) << "no variation: the design value";
  EXPECT_EQ(constant(f, GFNT_MATH_AXIS_HEIGHT, &vd), 107);
  EXPECT_EQ(constant(f, GFNT_MATH_AXIS_HEIGHT, &vh), 115);
  EXPECT_EQ(constant(f, GFNT_MATH_AXIS_HEIGHT, &vw), 115);
  EXPECT_EQ(constant(f, GFNT_MATH_AXIS_HEIGHT, &vm), 111) << "half way into the region";
  EXPECT_EQ(constant(f, GFNT_MATH_MATH_LEADING, &vh), 100) << "no device: unmoved";
  bool present = false;
  int32_t v = 0;
  ASSERT_EQ(gfnt_face_math_glyph_value(f.face, &vh, kZ, GFNT_MATH_ITALICS_CORRECTION, &present, &v,
      nullptr), GFNT_OK);
  EXPECT_EQ(v, 29);
  int32_t italics = 0;
  ASSERT_EQ(gfnt_face_math_assembly_italics_correction(f.face, &vh, kParen, true, &italics,
      nullptr), GFNT_OK);
  EXPECT_EQ(italics, 12);
  ASSERT_EQ(gfnt_face_math_assembly_italics_correction(f.face, &vw, kParen, true, &italics,
      nullptr), GFNT_OK);
  EXPECT_EQ(italics, 5);
  // The static font's hinting device tables are not applied at a location either.
  Face s("math.ttf");
  EXPECT_EQ(constant(s, GFNT_MATH_AXIS_HEIGHT, &vh), 107);
}

TEST(MathReader, AFaceWithoutMathAndBadArgumentsAreRefused) {
  Face f("variable-gvar.ttf");
  EXPECT_FALSE(gfnt_face_has_math(f.face));
  EXPECT_FALSE(gfnt_face_has_math(nullptr));
  int32_t v = 0;
  EXPECT_EQ(gfnt_face_math_constant(f.face, nullptr, GFNT_MATH_AXIS_HEIGHT, &v, nullptr),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(gfnt_face_math_constant(nullptr, nullptr, GFNT_MATH_AXIS_HEIGHT, &v, nullptr),
      GFNT_ERR_INVALID);
  Face m("math.ttf");
  EXPECT_EQ(gfnt_face_math_constant(m.face, nullptr, GFNT_MATH_AXIS_HEIGHT, nullptr, nullptr),
      GFNT_ERR_INVALID);
}

TEST(MathReader, ATruncatedTableIsCorruptAtEveryCut) {
  // Cut the MATH table's bytes off at each length and ask for everything: nothing may read past
  // the end, and what cannot be read is GFNT_ERR_CORRUPT, never a crash or a wrong number.
  std::ifstream in(gfnttest::data("fonts/math.ttf"), std::ios::binary);
  std::vector<uint8_t> whole((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t table_at = 0, table_len = 0;
  const uint32_t n = (whole[4] << 8) | whole[5];
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t * rec = &whole[12 + 16 * i];
    if (rec[0] == 'M' && rec[1] == 'A' && rec[2] == 'T' && rec[3] == 'H') {
      table_at = (size_t)rec[8] << 24 | rec[9] << 16 | rec[10] << 8 | rec[11];
      table_len = (size_t)rec[12] << 24 | rec[13] << 16 | rec[14] << 8 | rec[15];
    }
  }
  ASSERT_GT(table_len, 0u);
  for (size_t keep = 0; keep < table_len; keep += 7) {
    std::vector<uint8_t> cut(whole.begin(), whole.begin() + table_at + keep);
    // Shorten the directory entry to match; the blob ends where the table does.
    for (uint32_t i = 0; i < n; ++i) {
      uint8_t * rec = &cut[12 + 16 * i];
      if (rec[0] == 'M' && rec[1] == 'A' && rec[2] == 'T' && rec[3] == 'H') {
        rec[12] = (uint8_t)(keep >> 24); rec[13] = (uint8_t)(keep >> 16);
        rec[14] = (uint8_t)(keep >> 8); rec[15] = (uint8_t)keep;
      }
    }
    GFNT_Blob * blob = nullptr;
    GFNT_Face * face = nullptr;
    ASSERT_EQ(gfnt_blob_create_memory(cut.data(), cut.size(), GFNT_BLOB_BORROWED, nullptr, nullptr,
        &blob, nullptr), GFNT_OK);
    if (gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr) == GFNT_OK) {
      for (uint32_t g = 0; g < 27; ++g) {
        bool present;
        int32_t v;
        size_t c;
        GFNT_MathVariant mv;
        GFNT_MathPart part;
        for (int vert = 0; vert < 2; ++vert) {
          for (int w = 0; w < 56; ++w) {
            const GFNT_Result r = gfnt_face_math_constant(face, nullptr, (GFNT_MathConstant)w, &v,
                nullptr);
            ASSERT_TRUE(r == GFNT_OK || r == GFNT_ERR_CORRUPT || r == GFNT_ERR_UNSUPPORTED);
          }
          (void)gfnt_face_math_glyph_value(face, nullptr, g, GFNT_MATH_ITALICS_CORRECTION, &present,
              &v, nullptr);
          (void)gfnt_face_math_glyph_value(face, nullptr, g, GFNT_MATH_TOP_ACCENT_ATTACHMENT,
              &present, &v, nullptr);
          (void)gfnt_face_math_is_extended_shape(face, g, &present, nullptr);
          (void)gfnt_face_math_variant_count(face, g, vert != 0, &c, nullptr);
          (void)gfnt_face_math_variant_at(face, g, vert != 0, 0, &mv, nullptr);
          (void)gfnt_face_math_assembly_count(face, g, vert != 0, &c, nullptr);
          (void)gfnt_face_math_assembly_part_at(face, g, vert != 0, 0, &part, nullptr);
          (void)gfnt_face_math_assembly_italics_correction(face, nullptr, g, vert != 0, &v, nullptr);
          for (int corner = 0; corner < 4; ++corner) {
            (void)gfnt_face_math_kern_count(face, g, (GFNT_MathKernCorner)corner, &present, &c,
                nullptr);
            (void)gfnt_face_math_kern_height_at(face, nullptr, g, (GFNT_MathKernCorner)corner, 0,
                &v, nullptr);
            (void)gfnt_face_math_kern_value_at(face, nullptr, g, (GFNT_MathKernCorner)corner, 0,
                &v, nullptr);
          }
        }
      }
      gfnt_face_free(face);
    }
    gfnt_blob_destroy(blob);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
