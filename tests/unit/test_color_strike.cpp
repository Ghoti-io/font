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
 * Color bitmap strikes: `CBLC`/`CBDT` read through the public calls, a picture
 * decoded by `image`, and a glyph measured as HarfBuzz measures one.
 *
 * The expectations are the fixtures' (`tools/fixtures/strike_color_fixtures.py`),
 * whose pictures are a formula of the glyph and the pixel; `make
 * check-oracle-strike-images` holds the same reading to fontTools and Pillow over
 * Noto Color Emoji, and `make check-oracle-color-extents` holds the boxes to
 * `hb-shape`.
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <ghoti.io/font/bitmap.h>
#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/glyph.h>

extern "C" {
#include "../../src/shape/fallback.h"
}

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
  explicit Font(std::vector<uint8_t> data) : bytes(std::move(data)) { open(); }
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

/** Where a table's bytes are in the file, or false. */
bool table_range(const std::vector<uint8_t> & b, const char * tag, size_t * at,
    size_t * length) {
  const size_t n = (b[4] << 8) | b[5];
  for (size_t i = 0; i < n; ++i) {
    const size_t e = 12 + 16 * i;
    if (std::memcmp(&b[e], tag, 4) == 0) {
      *at = (static_cast<size_t>(b[e + 8]) << 24) | (b[e + 9] << 16) | (b[e + 10] << 8) | b[e + 11];
      *length = (static_cast<size_t>(b[e + 12]) << 24) | (b[e + 13] << 16) | (b[e + 14] << 8) | b[e + 15];
      return true;
    }
  }
  return false;
}

bool is_png(const GFNT_GlyphImage & image) {
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  return image.length > 8 && std::memcmp(image.data, sig, 8) == 0;
}

GFNT_GlyphImage image_of(const Font & font, uint32_t glyph, size_t strike) {
  GFNT_GlyphImage image{};
  GFNT_Error error;
  EXPECT_EQ(gfnt_face_glyph_image(font.face, glyph, strike, &image, &error), GFNT_OK)
      << "glyph " << glyph << " strike " << strike << ": " << (error.message ? error.message : "");
  return image;
}

struct Pixel {
  uint8_t r, g, b, a;
};

/** Decode a glyph; the caller frees. */
struct Decoded {
  GFNT_ColorBitmap bitmap{};
  GFNT_Result result = GFNT_OK;
  Decoded(const Font & font, uint32_t glyph, size_t strike) {
    result = gfnt_face_glyph_color_bitmap(font.face, glyph, strike, &bitmap, nullptr);
  }
  ~Decoded() { gfnt_color_bitmap_free(&bitmap); }
  Pixel at(uint32_t x, uint32_t y) const {
    const uint8_t * p = bitmap.rgba + y * bitmap.stride + 4u * x;
    return {p[0], p[1], p[2], p[3]};
  }
};

#define EXPECT_PIXEL(d, x, y, R, G, B, A)                                    \
  do {                                                                       \
    const Pixel p_ = (d).at(x, y);                                           \
    EXPECT_EQ(p_.r, R) << "at " << (x) << "," << (y);                        \
    EXPECT_EQ(p_.g, G) << "at " << (x) << "," << (y);                        \
    EXPECT_EQ(p_.b, B) << "at " << (x) << "," << (y);                        \
    EXPECT_EQ(p_.a, A) << "at " << (x) << "," << (y);                        \
  } while (0)

}  // namespace

TEST(ColorStrike, TheStrikeListNamesTwoColorStrikes) {
  Font font("color-cbdt.ttf");
  size_t count = 0;
  ASSERT_EQ(gfnt_face_strike_count(font.face, &count, nullptr), GFNT_OK);
  ASSERT_EQ(count, 2u);
  GFNT_Strike s0{}, s1{};
  ASSERT_EQ(gfnt_face_strike_at(font.face, 0, &s0, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_strike_at(font.face, 1, &s1, nullptr), GFNT_OK);
  EXPECT_EQ(s0.ppem_x, 16u);
  EXPECT_EQ(s1.ppem_y, 24u);
  EXPECT_EQ(s0.bit_depth, 32);
  EXPECT_EQ(s0.kind, GFNT_GLYPH_BITMAP_PNG);
  EXPECT_EQ(s0.ascent, 14);
  EXPECT_EQ(s0.descent, -4);
  EXPECT_EQ(s1.ascent, 21);
  EXPECT_EQ(s1.descent, -6);
}

TEST(ColorStrike, EveryIndexFormatYieldsItsPicturesAndMetrics) {
  Font font("color-cbdt.ttf");
  struct Case {
    uint32_t glyph;
    size_t strike;
    uint32_t w, h;
    int32_t bx, by, adv;
  };
  const Case cases[] = {
      {1, 0, 10, 12, 1, 11, 12},   // index 1, image 17
      {2, 0, 10, 12, 1, 11, 12},
      {3, 0, 9, 11, -2, 10, 13},   // index 3, image 18: a negative bearing
      {4, 0, 9, 11, -2, 10, 13},
      {5, 0, 8, 8, 0, 8, 9},       // index 5, image 19: metrics from CBLC
      {6, 0, 8, 8, 0, 8, 9},
      {7, 0, 7, 9, 0, 9, 8},       // index 4, image 17
      {9, 0, 7, 9, 0, 9, 8},
      {1, 1, 16, 18, 2, 17, 19},   // index 2, image 19: constant size
      {2, 1, 16, 18, 2, 17, 19},
      {3, 1, 14, 15, -3, 20, 17},  // bearings of both signs
      {4, 1, 14, 15, 4, -1, 17},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE("glyph " + std::to_string(c.glyph) + " strike " + std::to_string(c.strike));
    const GFNT_GlyphImage image = image_of(font, c.glyph, c.strike);
    EXPECT_EQ(image.format, GFNT_IMAGE_PNG);
    EXPECT_TRUE(is_png(image));
    EXPECT_EQ(image.width, c.w);
    EXPECT_EQ(image.height, c.h);
    EXPECT_EQ(image.bearing_x, c.bx);
    EXPECT_EQ(image.bearing_y, c.by);
    EXPECT_EQ(image.advance, c.adv);
    EXPECT_EQ(image.strike.index, c.strike);
  }
}

TEST(ColorStrike, AGlyphTheStrikeDoesNotCarryIsUnsupportedAndNotCorrupt) {
  Font font("color-cbdt.ttf");
  GFNT_GlyphImage image{};
  GFNT_Error error;
  // Glyph 8 is inside strike 0's range and absent from its sparse index; 10 is
  // beyond the range; 0 is before it.
  for (uint32_t glyph : {0u, 8u, 10u, 11u}) {
    EXPECT_EQ(gfnt_face_glyph_image(font.face, glyph, 0, &image, &error), GFNT_ERR_UNSUPPORTED)
        << glyph;
    EXPECT_NE(std::strstr(error.message, "does not carry"), nullptr) << error.message;
  }
  // Strike 1 does not carry glyph 5, which strike 0 does.
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 5, 1, &image, nullptr), GFNT_ERR_UNSUPPORTED);
}

TEST(ColorStrike, BadArgumentsAreInvalid) {
  Font font("color-cbdt.ttf");
  GFNT_GlyphImage image{};
  EXPECT_EQ(gfnt_face_glyph_image(nullptr, 1, 0, &image, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 1, 0, nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 1, 2, &image, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 12, 0, &image, nullptr), GFNT_ERR_INVALID);
  Font plain("basic.ttf");
  EXPECT_EQ(gfnt_face_glyph_image(plain.face, 1, 0, &image, nullptr), GFNT_ERR_INVALID);
  GFNT_ColorBitmap bitmap{};
  EXPECT_EQ(gfnt_face_glyph_color_bitmap(font.face, 1, 0, nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_color_bitmap(plain.face, 1, 0, &bitmap, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(bitmap.rgba, nullptr);
  gfnt_color_bitmap_free(nullptr);   // harmless
}

TEST(ColorStrike, TheMonochromeCallDeclinesAColorStrike) {
  Font font("color-cbdt.ttf");
  GFNT_BitmapGlyph glyph{};
  EXPECT_EQ(gfnt_face_glyph_bitmap(font.face, 1, 0, &glyph, nullptr), GFNT_ERR_UNSUPPORTED);
}

TEST(ColorStrike, AnRgbaPictureDecodesWithItsStraightAlpha) {
  Font font("color-cbdt.ttf");
  Decoded d(font, 1, 0);
  ASSERT_EQ(d.result, GFNT_OK);
  EXPECT_EQ(d.bitmap.width, 10u);
  EXPECT_EQ(d.bitmap.height, 12u);
  EXPECT_EQ(d.bitmap.stride, 40u);
  EXPECT_EQ(d.bitmap.bearing_y, 11);
  EXPECT_EQ(d.bitmap.strike.ppem_x, 16u);
  // The picture is (g*37 + 9x, g*11 + 17y, 200 on row g%h else 30, alpha 90 when
  // (x+y) is a multiple of five). 90 is not 255, and it is kept as written.
  EXPECT_PIXEL(d, 0, 0, 37, 11, 30, 90);
  EXPECT_PIXEL(d, 1, 0, 46, 11, 30, 255);
  EXPECT_PIXEL(d, 3, 1, 64, 28, 200, 255);   // row 1 is g % 12
  EXPECT_PIXEL(d, 4, 1, 73, 28, 200, 90);
  EXPECT_PIXEL(d, 9, 11, 118, 198, 30, 90);
}

TEST(ColorStrike, EveryPngColorTypeDecodesToRgba) {
  Font font("color-cbdt.ttf");
  {
    // Glyph 2, RGB with no alpha: opaque.
    Decoded d(font, 2, 0);
    ASSERT_EQ(d.result, GFNT_OK);
    EXPECT_PIXEL(d, 0, 0, 58, 0, 2, 255);
    EXPECT_PIXEL(d, 2, 2, 84, 255, 14, 255);   // the stripe row
  }
  {
    // Glyph 3, a palette with a tRNS chunk: entry 0 clear, 1 half, 2 and 3 opaque.
    Decoded d(font, 3, 0);
    ASSERT_EQ(d.result, GFNT_OK);
    EXPECT_PIXEL(d, 0, 0, 40, 40, 200, 255);
    EXPECT_PIXEL(d, 1, 0, 0, 0, 0, 0);
    EXPECT_PIXEL(d, 2, 0, 200, 40, 40, 128);
    EXPECT_PIXEL(d, 3, 0, 40, 200, 40, 255);
  }
  {
    // Glyph 4, grey: the sample is all three channels, and the picture is opaque.
    Decoded d(font, 4, 0);
    ASSERT_EQ(d.result, GFNT_OK);
    EXPECT_PIXEL(d, 0, 0, 4, 4, 4, 255);
    EXPECT_PIXEL(d, 2, 0, 50, 50, 50, 255);
    EXPECT_PIXEL(d, 3, 4, 255, 255, 255, 255);   // the stripe row
  }
  {
    // Glyph 5, grey with alpha.
    Decoded d(font, 5, 0);
    ASSERT_EQ(d.result, GFNT_OK);
    EXPECT_PIXEL(d, 0, 0, 5, 5, 5, 64);
    EXPECT_PIXEL(d, 1, 3, 36, 36, 36, 255);
    EXPECT_PIXEL(d, 3, 0, 98, 98, 98, 64);
  }
  {
    // Glyph 6, one bit a pixel: 0 and 255.
    Decoded d(font, 6, 0);
    ASSERT_EQ(d.result, GFNT_OK);
    EXPECT_PIXEL(d, 0, 0, 255, 255, 255, 255);
    EXPECT_PIXEL(d, 1, 0, 0, 0, 0, 255);
    EXPECT_PIXEL(d, 2, 0, 0, 0, 0, 255);
    EXPECT_PIXEL(d, 0, 3, 255, 255, 255, 255);
  }
}

TEST(ColorStrike, ADecodedBitmapIsTheCallersAndFreeingItIsIdempotent) {
  Font font("color-cbdt.ttf");
  GFNT_ColorBitmap bitmap{};
  ASSERT_EQ(gfnt_face_glyph_color_bitmap(font.face, 1, 0, &bitmap, nullptr), GFNT_OK);
  ASSERT_NE(bitmap.rgba, nullptr);
  gfnt_color_bitmap_free(&bitmap);
  EXPECT_EQ(bitmap.rgba, nullptr);
  gfnt_color_bitmap_free(&bitmap);
}

// ---- the picture the font states is bounded by the bytes the index gives it

namespace {

/** Offsets in CBDT, from its start: strike 0, glyph 1 is first, after the 4-byte version. */
constexpr size_t kFirstGlyph = 4;

std::vector<uint8_t> with_cbdt_byte(const std::vector<uint8_t> & source, size_t at, uint8_t value) {
  std::vector<uint8_t> bytes = source;
  size_t table = 0, length = 0;
  EXPECT_TRUE(table_range(bytes, "CBDT", &table, &length));
  bytes[table + at] = value;
  return bytes;
}

}  // namespace

TEST(ColorStrike, ADataLenPastTheIndexIsCorruptAndSparesItsNeighbors) {
  Font source("color-cbdt.ttf");
  // Glyph 1 is small metrics (5 bytes) and then dataLen (4): make its top byte 0x7F.
  Font font(with_cbdt_byte(source.bytes, kFirstGlyph + 5, 0x7F));
  GFNT_GlyphImage image{};
  GFNT_Error error;
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 1, 0, &image, &error), GFNT_ERR_CORRUPT);
  EXPECT_NE(std::strstr(error.message, "dataLen"), nullptr) << error.message;
  // M11: one glyph's bytes do not condemn the strike.
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 2, 0, &image, nullptr), GFNT_OK);
  EXPECT_TRUE(is_png(image));
}

TEST(ColorStrike, AnEmptyPictureIsCorrupt) {
  Font source("color-cbdt.ttf");
  std::vector<uint8_t> bytes = source.bytes;
  size_t table = 0, length = 0;
  ASSERT_TRUE(table_range(bytes, "CBDT", &table, &length));
  for (size_t k = 5; k < 9; ++k) {
    bytes[table + kFirstGlyph + k] = 0;   // dataLen of glyph 1
  }
  Font font(bytes);
  GFNT_GlyphImage image{};
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 1, 0, &image, nullptr), GFNT_ERR_CORRUPT);
}

TEST(ColorStrike, ACbdtVersionThisLibraryDoesNotKnowIsUnsupported) {
  Font source("color-cbdt.ttf");
  Font font(with_cbdt_byte(source.bytes, 1, 0x04));   // 0x00040000
  GFNT_GlyphImage image{};
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 1, 0, &image, nullptr), GFNT_ERR_UNSUPPORTED);
}

TEST(ColorStrike, APictureThatIsNotAPngIsCorruptWhenDecodedAndStillTheFontsBytes) {
  Font source("color-cbdt.ttf");
  // The PNG signature's second byte, in glyph 1 (9 bytes of header after the version).
  Font font(with_cbdt_byte(source.bytes, kFirstGlyph + 9 + 1, 'X'));
  GFNT_GlyphImage image{};
  ASSERT_EQ(gfnt_face_glyph_image(font.face, 1, 0, &image, nullptr), GFNT_OK);
  EXPECT_FALSE(is_png(image));
  GFNT_ColorBitmap bitmap{};
  GFNT_Error error;
  const GFNT_Result r = gfnt_face_glyph_color_bitmap(font.face, 1, 0, &bitmap, &error);
  EXPECT_TRUE(r == GFNT_ERR_CORRUPT || r == GFNT_ERR_UNSUPPORTED) << r;
  EXPECT_EQ(bitmap.rgba, nullptr);
}

TEST(ColorStrike, NoByteOfTheTablesCrashesAReader) {
  Font source("color-cbdt.ttf");
  size_t at[2], length[2];
  ASSERT_TRUE(table_range(source.bytes, "CBLC", &at[0], &length[0]));
  ASSERT_TRUE(table_range(source.bytes, "CBDT", &at[1], &length[1]));
  size_t tried = 0;
  for (int t = 0; t < 2; ++t) {
    for (size_t k = 0; k < length[t]; ++k) {
      for (uint8_t value : {uint8_t{0x00}, uint8_t{0xFF}}) {
        std::vector<uint8_t> bytes = source.bytes;
        bytes[at[t] + k] = value;
        GFNT_Blob * blob = nullptr;
        GFNT_Face * face = nullptr;
        ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED,
                      nullptr, nullptr, &blob, nullptr), GFNT_OK);
        if (gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr) == GFNT_OK) {
          size_t strikes = 0;
          if (gfnt_face_strike_count(face, &strikes, nullptr) == GFNT_OK) {
            for (size_t s = 0; s < strikes; ++s) {
              for (uint32_t g = 0; g < 13; ++g) {
                GFNT_ColorBitmap bitmap{};
                if (gfnt_face_glyph_color_bitmap(face, g, s, &bitmap, nullptr) == GFNT_OK) {
                  EXPECT_NE(bitmap.rgba, nullptr);
                  EXPECT_EQ(bitmap.stride, size_t{bitmap.width} * 4u);
                }
                gfnt_color_bitmap_free(&bitmap);
              }
            }
          }
          gfnt_face_free(face);
        }
        gfnt_blob_destroy(blob);
        ++tried;
      }
    }
  }
  EXPECT_GT(tried, 1000u);
}

TEST(ColorStrike, EveryAllocationFailureLeavesNothingBehind) {
  Font source("color-cbdt.ttf");
  // The face is loaded under the allocator, so the sweep starts after the loading.
  size_t base = 0;
  {
    gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
    GFNT_Face * face = nullptr;
    ASSERT_EQ(gfnt_face_load(source.blob, 0, nullptr, counting.get(), &face, nullptr), GFNT_OK);
    base = counting.requests();
    GFNT_ColorBitmap bitmap{};
    ASSERT_EQ(gfnt_face_glyph_color_bitmap(face, 1, 0, &bitmap, nullptr), GFNT_OK);
    gfnt_color_bitmap_free(&bitmap);
    // Parse the index first, so the sweep does not also sweep the strike list memo.
    const size_t cost = counting.requests() - base;
    gfnt_face_free(face);
    EXPECT_EQ(counting.live(), 0u);
    ASSERT_GT(cost, 0u);
  }
  for (size_t fail = base; fail < base + 12; ++fail) {
    gfnttest::FailingAllocator failing(fail);
    GFNT_Face * face = nullptr;
    ASSERT_EQ(gfnt_face_load(source.blob, 0, nullptr, failing.get(), &face, nullptr), GFNT_OK);
    GFNT_ColorBitmap bitmap{};
    const GFNT_Result r = gfnt_face_glyph_color_bitmap(face, 1, 0, &bitmap, nullptr);
    if (r == GFNT_OK) {
      gfnt_color_bitmap_free(&bitmap);
    }
    else {
      EXPECT_EQ(r, GFNT_ERR_OOM) << "request " << fail;
      EXPECT_EQ(bitmap.rgba, nullptr);
    }
    failing.stop_failing();
    gfnt_face_free(face);
    EXPECT_EQ(failing.live(), 0u) << "request " << fail << " leaked";
  }
}

// ---- the box a shaper measures a color bitmap glyph with

namespace {

GFNT_Extents extents(const Font & font, uint32_t glyph, uint32_t ppem = 0) {
  GFNT_Extents e{};
  EXPECT_TRUE(gfnt_glyph_extents_at(font.face, glyph, nullptr, ppem, &e));
  return e;
}

#define EXPECT_BOX(e, xb, yb, w, h)                                          \
  do {                                                                       \
    EXPECT_EQ((e).x_bearing, xb);                                            \
    EXPECT_EQ((e).y_bearing, yb);                                            \
    EXPECT_EQ((e).width, w);                                                 \
    EXPECT_EQ((e).height, h);                                                \
  } while (0)

}  // namespace

TEST(ColorBitmapExtents, WithNoSizeTheLargestStrikeAnswers) {
  Font font("color-cbdt.ttf");
  // 24 ppem in a 1000-unit em: a pixel is 41.67 units. Glyph 3 is 14 x 15 at (-3, 20).
  EXPECT_BOX(extents(font, 3), -125, 833, 583, -625);
  EXPECT_BOX(extents(font, 4), 167, -42, 583, -625);   // (4, -1): -41.67 rounds to -42
}

TEST(ColorBitmapExtents, ASizePicksTheSmallestStrikeAtLeastAsLarge) {
  Font font("color-cbdt.ttf");
  // 16 ppem: 62.5 units a pixel, so every half is on a tie. A half rounds up, and the
  // height is negated before it does: -687.5 is -687, not -688.
  EXPECT_BOX(extents(font, 1, 16), 63, 688, 625, -750);
  EXPECT_BOX(extents(font, 3, 16), -125, 625, 563, -687);
  EXPECT_BOX(extents(font, 3, 10), -125, 625, 563, -687);   // below the smallest: it
  EXPECT_BOX(extents(font, 3, 17), -125, 833, 583, -625);   // 17 is past 16: the 24
  EXPECT_BOX(extents(font, 3, 4000), -125, 833, 583, -625);   // past all: the largest
}

TEST(ColorBitmapExtents, ACellHarfBuzzDoesNotReadHasNoBoxHere) {
  Font font("color-cbdt.ttf");
  // Glyph 7 is in an index format 4 subtable, and glyph 5 behind image format 19; the
  // reader returns both pictures and the shaper's measure, like HarfBuzz's, has no
  // box for either (the font's glyphs are empty outlines, so the box is empty).
  EXPECT_BOX(extents(font, 7, 16), 0, 0, 0, 0);
  EXPECT_BOX(extents(font, 5, 16), 0, 0, 0, 0);
  EXPECT_EQ(image_of(font, 7, 0).width, 7u);
}

TEST(ColorBitmapExtents, WithNoSizeTheLargerOfTwoPpemsDecidesWhichStrikeIsLargest) {
  Font font("color-cbdt-sizes.ttf");
  // Strike 3 is 10 by 30 ppem, and 30 is the largest of any strike's two. Glyph 1 is
  // 8 x 8 at (3, 8) there, with x over 10 ppem (100 units a pixel) and y over 30
  // (33.3): so a measure that scaled the y axis by the x ppem, or chose by x alone,
  // answers differently.
  EXPECT_BOX(extents(font, 1), 300, 267, 800, -267);
  EXPECT_BOX(extents(font, 2), -200, 167, 700, -133);   // 4 x 7 at (-2, 5)
  // Asked for 28, the smallest strike of at least 28 is the same one.
  EXPECT_BOX(extents(font, 1, 28), 300, 267, 800, -267);
}

TEST(ColorBitmapExtents, AGlyphTheChosenStrikeLacksKeepsItsOutline) {
  Font font("color-cbdt-sizes.ttf");
  // Glyphs 4, 5 and 6 have outlines. Strike 3 carries none of them, and another
  // strike carrying one is not consulted: the outline's box stands, which is the
  // header's (xMin 50 of glyph 4's 50..250 is not the left bearing hmtx gives, 0).
  EXPECT_BOX(extents(font, 4), 0, 900, 200, -1000);
  EXPECT_BOX(extents(font, 5), 0, 500, 400, -400);
  EXPECT_BOX(extents(font, 6), 0, 900, 200, -1000);
}

TEST(ColorBitmapExtents, ThePictureWinsOverTheOutline) {
  Font font("color-cbdt-sizes.ttf");
  // Glyph 5 has an outline (100..500 square) and, in the 20 by 20 strike, a 5 x 5
  // picture at (0, 5), 50 units a pixel. The picture is the box.
  EXPECT_BOX(extents(font, 5, 20), 0, 250, 250, -250);
  // Glyph 1 has the same outline and a picture in every strike.
  EXPECT_BOX(extents(font, 1, 20), 50, 300, 300, -300);   // 6 x 6 at (1, 6)
}

TEST(ColorBitmapExtents, EqualStrikesGoToTheFirst) {
  Font font("color-cbdt-sizes.ttf");
  // Strikes 0 and 2 are both 24 by 12. Strike 0 carries glyph 1 as 8 x 8 and strike 2
  // as 2 x 2; with x over 24 (41.7 a pixel) and y over 12 (83.3) the first is
  // 333 by 667. Glyph 3 is in strike 0 alone: 4 x 4 at (2, 3).
  EXPECT_BOX(extents(font, 1, 24), 0, 667, 333, -667);
  EXPECT_BOX(extents(font, 3, 24), 83, 250, 167, -333);
}

TEST(ColorStrike, APictureThatIsNotTheSizeItsMetricsStateIsCorruptToDecode) {
  Font source("color-cbdt.ttf");
  // Glyph 1: small metrics are height, width, ...; make the width 11 where the PNG is 10.
  Font font(with_cbdt_byte(source.bytes, kFirstGlyph + 1, 11));
  GFNT_GlyphImage image{};
  ASSERT_EQ(gfnt_face_glyph_image(font.face, 1, 0, &image, nullptr), GFNT_OK);
  EXPECT_EQ(image.width, 11u);   // the font's statement, as stated
  GFNT_ColorBitmap bitmap{};
  GFNT_Error error;
  EXPECT_EQ(gfnt_face_glyph_color_bitmap(font.face, 1, 0, &bitmap, &error), GFNT_ERR_CORRUPT);
  EXPECT_NE(std::strstr(error.message, "metrics"), nullptr) << error.message;
  EXPECT_EQ(bitmap.rgba, nullptr);
  // Its neighbor is untouched.
  Decoded d(font, 2, 0);
  EXPECT_EQ(d.result, GFNT_OK);
}

// ---- 16-bit pictures

namespace {

/** The fixture's samples: tools/fixtures/strike_color_fixtures.py deep_sample(). */
uint16_t deep_sample(uint32_t glyph, uint32_t x, uint32_t y, uint32_t c) {
  return static_cast<uint16_t>((glyph * 4099u + x * 7919u + y * 12289u + c * 20011u) % 65536u);
}

uint16_t word(const GFNT_ColorBitmap & b, uint32_t x, uint32_t y, uint32_t c) {
  uint16_t v;
  std::memcpy(&v, b.rgba + y * b.stride + (4u * x + c) * 2u, 2);
  return v;
}

}  // namespace

TEST(ColorStrikeDeep, ASixteenBitPngKeepsEverySample) {
  Font font("color-cbdt-deep.ttf");
  {
    Decoded d(font, 1, 0);   // RGBA
    ASSERT_EQ(d.result, GFNT_OK);
    EXPECT_EQ(d.bitmap.bits_per_channel, 16);
    EXPECT_FALSE(d.bitmap.premultiplied);
    EXPECT_EQ(d.bitmap.stride, 6u * 8u);
    for (uint32_t y = 0; y < 5; ++y) {
      for (uint32_t x = 0; x < 6; ++x) {
        for (uint32_t c = 0; c < 4; ++c) {
          EXPECT_EQ(word(d.bitmap, x, y, c), deep_sample(1, x, y, c)) << x << "," << y << "," << c;
        }
      }
    }
  }
  {
    Decoded d(font, 2, 0);   // RGB: alpha is full
    ASSERT_EQ(d.result, GFNT_OK);
    EXPECT_EQ(word(d.bitmap, 2, 3, 1), deep_sample(2, 2, 3, 1));
    EXPECT_EQ(word(d.bitmap, 2, 3, 3), 65535u);
  }
  {
    Decoded d(font, 3, 0);   // grey: the one sample in all three colors
    ASSERT_EQ(d.result, GFNT_OK);
    for (uint32_t c = 0; c < 3; ++c) {
      EXPECT_EQ(word(d.bitmap, 4, 1, c), deep_sample(3, 4, 1, 0));
    }
    EXPECT_EQ(word(d.bitmap, 4, 1, 3), 65535u);
  }
  {
    Decoded d(font, 4, 0);   // grey with alpha
    ASSERT_EQ(d.result, GFNT_OK);
    EXPECT_EQ(word(d.bitmap, 1, 1, 0), deep_sample(4, 1, 1, 0));
    EXPECT_EQ(word(d.bitmap, 1, 1, 2), deep_sample(4, 1, 1, 0));
    EXPECT_EQ(word(d.bitmap, 1, 1, 3), deep_sample(4, 1, 1, 1));
  }
  {
    Decoded d(font, 5, 0);   // an 8-bit one beside them
    ASSERT_EQ(d.result, GFNT_OK);
    EXPECT_EQ(d.bitmap.bits_per_channel, 8);
    EXPECT_EQ(d.bitmap.stride, 6u * 4u);
  }
}

TEST(ColorStrikeDeep, NarrowingTakesTheHighByteAsImageDoes) {
  Font font("color-cbdt-deep.ttf");
  Decoded d(font, 1, 0);
  ASSERT_EQ(d.result, GFNT_OK);
  ASSERT_EQ(gfnt_color_bitmap_to_8bit(&d.bitmap), GFNT_OK);
  EXPECT_EQ(d.bitmap.bits_per_channel, 8);
  EXPECT_EQ(d.bitmap.stride, 6u * 4u);
  for (uint32_t y = 0; y < 5; ++y) {
    for (uint32_t x = 0; x < 6; ++x) {
      for (uint32_t c = 0; c < 4; ++c) {
        EXPECT_EQ(d.bitmap.rgba[y * d.bitmap.stride + 4u * x + c], deep_sample(1, x, y, c) >> 8)
            << x << "," << y << "," << c;
      }
    }
  }
  // Already 8 bits: left alone. No bitmap: invalid.
  ASSERT_EQ(gfnt_color_bitmap_to_8bit(&d.bitmap), GFNT_OK);
  EXPECT_EQ(d.bitmap.stride, 6u * 4u);
  EXPECT_EQ(gfnt_color_bitmap_to_8bit(nullptr), GFNT_ERR_INVALID);
  GFNT_ColorBitmap empty{};
  EXPECT_EQ(gfnt_color_bitmap_to_8bit(&empty), GFNT_ERR_INVALID);
}

// ---- uncompressed BGRA

namespace {

/** tools/fixtures/strike_color_fixtures.py bgra_rows(), pixel (x, y) of glyph g: B, G, R, A. */
void bgra_pixel(uint32_t g, uint32_t x, uint32_t y, uint8_t out[4]) {
  const uint32_t a = 40 + ((g * 29 + x * 17 + y * 11) % 216);
  const uint32_t b = a * ((g * 3 + x) % 5) / 4;
  const uint32_t gr = a * ((g + y * 2) % 5) / 4;
  const uint32_t r = a * ((g * 7 + x + y) % 5) / 4;
  out[0] = static_cast<uint8_t>(b < a ? b : a);
  out[1] = static_cast<uint8_t>(gr < a ? gr : a);
  out[2] = static_cast<uint8_t>(r < a ? r : a);
  out[3] = static_cast<uint8_t>(a);
}

}  // namespace

TEST(ColorStrikeBgra, EveryRawImageFormatYieldsItsPixelsAndMetrics) {
  Font font("color-cbdt-bgra.ttf");
  struct Case {
    uint32_t glyph;
    int32_t bx, by, adv;
  };
  // Small metrics (glyphs 1, 2) put the pixel at (g - 3, 4 + g); big (3 to 5) at
  // (2 - g, 5); glyph 6's are the index's: (1, 6).
  const Case cases[] = {{1, -2, 5, 9}, {2, -1, 6, 9}, {3, -1, 5, 9}, {4, -2, 5, 9},
      {5, -3, 5, 9}, {6, 1, 6, 9}};
  for (const Case & c : cases) {
    SCOPED_TRACE(c.glyph);
    const GFNT_GlyphImage image = image_of(font, c.glyph, 0);
    EXPECT_EQ(image.format, GFNT_IMAGE_BGRA);
    EXPECT_EQ(image.length, 7u * 4u * 4u);
    EXPECT_EQ(image.width, 7u);
    EXPECT_EQ(image.height, 4u);
    EXPECT_EQ(image.bearing_x, c.bx);
    EXPECT_EQ(image.bearing_y, c.by);
    EXPECT_EQ(image.origin_y, c.by - 4);
    EXPECT_EQ(image.advance, c.adv);
    for (uint32_t y = 0; y < 4; ++y) {
      for (uint32_t x = 0; x < 7; ++x) {
        uint8_t want[4];
        bgra_pixel(c.glyph, x, y, want);
        EXPECT_EQ(std::memcmp(image.data + (y * 7 + x) * 4, want, 4), 0) << x << "," << y;
      }
    }
  }
}

TEST(ColorStrikeBgra, DecodingSwapsToRgbaAndSaysPremultiplied) {
  Font font("color-cbdt-bgra.ttf");
  Decoded d(font, 1, 0);
  ASSERT_EQ(d.result, GFNT_OK);
  EXPECT_TRUE(d.bitmap.premultiplied);
  EXPECT_EQ(d.bitmap.bits_per_channel, 8);
  EXPECT_EQ(d.bitmap.stride, 28u);
  uint8_t bgra[4];
  bgra_pixel(1, 3, 2, bgra);
  EXPECT_PIXEL(d, 3, 2, bgra[2], bgra[1], bgra[0], bgra[3]);
  EXPECT_EQ(d.bitmap.bearing_y, 5);
}

TEST(ColorStrikeBgra, ACompositeIsDeclinedByName) {
  Font font("color-cbdt-bgra.ttf");
  GFNT_GlyphImage image{};
  GFNT_Error error;
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 7, 0, &image, &error), GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::strstr(error.message, "composites"), nullptr) << error.message;
}

TEST(ColorStrikeBgra, PixelsPastTheIndexAreCorrupt) {
  Font source("color-cbdt-bgra.ttf");
  // Glyph 1's small metrics start at CBDT byte 4; make its height 200, so that
  // 7 x 200 x 4 bytes is far more than the index gives it.
  Font font(with_cbdt_byte(source.bytes, 4, 200));
  GFNT_GlyphImage image{};
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 1, 0, &image, nullptr), GFNT_ERR_CORRUPT);
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 2, 0, &image, nullptr), GFNT_OK);
}

TEST(ColorStrikeBgra, NothingHereIsMeasuredByTheShaper) {
  // HarfBuzz reads image formats 17 and 18 only; so does the shaper's box.
  Font font("color-cbdt-bgra.ttf");
  EXPECT_BOX(extents(font, 1), 0, 0, 0, 0);
}

// ---- sbix

TEST(SbixStrike, TheStrikeListIsInTheTablesOrder) {
  Font font("color-sbix.ttf");
  size_t count = 0;
  ASSERT_EQ(gfnt_face_strike_count(font.face, &count, nullptr), GFNT_OK);
  ASSERT_EQ(count, 2u);
  GFNT_Strike a{}, b{};
  ASSERT_EQ(gfnt_face_strike_at(font.face, 0, &a, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_strike_at(font.face, 1, &b, nullptr), GFNT_OK);
  EXPECT_EQ(a.ppem_x, 40u);   // 40 is first in the file, so it is strike 0
  EXPECT_EQ(b.ppem_y, 20u);
  EXPECT_EQ(a.kind, GFNT_GLYPH_BITMAP_PNG);
  EXPECT_EQ(a.bit_depth, 32);
  EXPECT_EQ(gfnt_face_strike_at(font.face, 2, &a, nullptr), GFNT_ERR_INVALID);
}

TEST(SbixStrike, APictureComesWithItsOriginAndNoAdvance) {
  Font font("color-sbix.ttf");
  const GFNT_GlyphImage g1 = image_of(font, 1, 1);   // 10 x 12 at origin (1, -2)
  EXPECT_EQ(g1.format, GFNT_IMAGE_PNG);
  EXPECT_TRUE(is_png(g1));
  EXPECT_EQ(g1.width, 10u);
  EXPECT_EQ(g1.height, 12u);
  EXPECT_EQ(g1.bearing_x, 1);
  EXPECT_EQ(g1.origin_y, -2);
  EXPECT_EQ(g1.bearing_y, 10);   // the bottom edge plus the height
  EXPECT_EQ(g1.advance, -1);
  const GFNT_GlyphImage big = image_of(font, 1, 0);   // the same glyph at 40 ppem
  EXPECT_EQ(big.width, 20u);
  EXPECT_EQ(big.origin_y, -4);
}

TEST(SbixStrike, ADupeIsFollowedToThePictureItNames) {
  Font font("color-sbix.ttf");
  const GFNT_GlyphImage g1 = image_of(font, 1, 1);
  for (uint32_t glyph : {3u, 4u}) {   // 3 dupes 1; 4 dupes 3
    const GFNT_GlyphImage d = image_of(font, glyph, 1);
    ASSERT_EQ(d.length, g1.length) << glyph;
    EXPECT_EQ(d.data, g1.data) << glyph;   // the same bytes, not a copy
    EXPECT_EQ(d.origin_y, -2);
  }
  // In the large strike glyph 3 dupes glyph 2, not 1: the same record, another picture.
  EXPECT_EQ(image_of(font, 3, 0).data, image_of(font, 2, 0).data);
}

TEST(SbixStrike, JpegAndTiffAreReturnedUnmeasuredAndDecode) {
  Font font("color-sbix.ttf");
  const GFNT_GlyphImage jpeg = image_of(font, 5, 1);
  EXPECT_EQ(jpeg.format, GFNT_IMAGE_JPEG);
  EXPECT_EQ(jpeg.width, 0u);   // not stated until decoded
  EXPECT_EQ(jpeg.height, 0u);
  EXPECT_EQ(image_of(font, 6, 1).format, GFNT_IMAGE_TIFF);

  Decoded j(font, 5, 1);
  ASSERT_EQ(j.result, GFNT_OK);
  EXPECT_EQ(j.bitmap.width, 16u);
  EXPECT_EQ(j.bitmap.height, 16u);
  EXPECT_EQ(j.bitmap.bearing_y, 20);   // origin 4 plus the decoded height
  Decoded t(font, 6, 1);
  ASSERT_EQ(t.result, GFNT_OK);
  EXPECT_EQ(t.bitmap.width, 8u);
  EXPECT_EQ(t.bitmap.height, 6u);
  EXPECT_EQ(t.bitmap.bearing_y, 8);
  EXPECT_PIXEL(t, 0, 0, 0, 0, 99, 100);
  EXPECT_PIXEL(t, 1, 0, 31, 0, 99, 255);
}

TEST(SbixStrike, WhatIsNotThereOrNotReadIsRefusedByName) {
  Font font("color-sbix.ttf");
  GFNT_GlyphImage image{};
  GFNT_Error error;
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 8, 1, &image, &error), GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::strstr(error.message, "does not carry"), nullptr) << error.message;
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 7, 1, &image, &error), GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::strstr(error.message, "graphicType"), nullptr) << error.message;
  // A dupe of itself is a cycle, bounded at eight hops.
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 9, 1, &image, &error), GFNT_ERR_CORRUPT);
  // The large strike does not carry glyph 5.
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 5, 0, &image, nullptr), GFNT_ERR_UNSUPPORTED);
}

TEST(SbixStrike, NoByteOfTheTableCrashesAReader) {
  Font source("color-sbix.ttf");
  size_t at = 0, length = 0;
  ASSERT_TRUE(table_range(source.bytes, "sbix", &at, &length));
  size_t tried = 0;
  for (size_t k = 0; k < length; ++k) {
    for (uint8_t value : {uint8_t{0x00}, uint8_t{0xFF}}) {
      std::vector<uint8_t> bytes = source.bytes;
      bytes[at + k] = value;
      GFNT_Blob * blob = nullptr;
      GFNT_Face * face = nullptr;
      ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED,
                    nullptr, nullptr, &blob, nullptr), GFNT_OK);
      if (gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr) == GFNT_OK) {
        size_t strikes = 0;
        if (gfnt_face_strike_count(face, &strikes, nullptr) == GFNT_OK) {
          for (size_t s = 0; s < strikes; ++s) {
            for (uint32_t g = 0; g < 10; ++g) {
              GFNT_ColorBitmap bitmap{};
              (void)gfnt_face_glyph_color_bitmap(face, g, s, &bitmap, nullptr);
              gfnt_color_bitmap_free(&bitmap);
              GFNT_Extents e;
              (void)gfnt_glyph_extents_at(face, g, nullptr, 20, &e);
            }
          }
        }
        gfnt_face_free(face);
      }
      gfnt_blob_destroy(blob);
      ++tried;
    }
  }
  EXPECT_GT(tried, 1000u);
}

TEST(SbixExtents, ThePictureIsMeasuredByItsHeaderAndOrigin) {
  Font font("color-sbix.ttf");
  // No size: the 40 ppem strike, 25 units a pixel. Glyph 1 is 20 x 24 at origin (2, -4):
  // the top edge is -4 + 24 = 20 pixels up.
  EXPECT_BOX(extents(font, 1), 50, 500, 500, -600);
  EXPECT_BOX(extents(font, 2), -150, 400, 400, -400);
  EXPECT_BOX(extents(font, 3), -150, 400, 400, -400);   // a dupe of glyph 2 here
  // 20 ppem, 50 a pixel: glyph 1 is 10 x 12 at (1, -2), and a chain of dupes reaches it.
  EXPECT_BOX(extents(font, 1, 20), 50, 500, 500, -600);
  EXPECT_BOX(extents(font, 4, 20), 50, 500, 500, -600);
  // A JPEG, a TIFF, a mask, a hole and a cycle have no box.
  for (uint32_t g : {5u, 6u, 7u, 8u, 9u}) {
    { SCOPED_TRACE(g); EXPECT_BOX(extents(font, g, 20), 0, 0, 0, 0); }
  }
}

TEST(SbixStrike, ADupeChainIsFollowedForEightHopsAndNoFurther) {
  Font font("color-sbix-dupes.ttf");
  const GFNT_GlyphImage picture = image_of(font, 1, 0);
  EXPECT_EQ(image_of(font, 9, 0).data, picture.data);   // eight hops
  GFNT_GlyphImage image{};
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 10, 0, &image, nullptr), GFNT_ERR_CORRUPT);
  EXPECT_EQ(gfnt_face_glyph_image(font.face, 20, 0, &image, nullptr), GFNT_ERR_CORRUPT);
  // 280 dupes 270: an id that does not fit a byte.
  EXPECT_EQ(image_of(font, 280, 0).data, image_of(font, 270, 0).data);
  EXPECT_EQ(image_of(font, 280, 0).width, 6u);
}

TEST(SbixStrike, TheDrawOutlinesFlagIsTheFontsToAskFor) {
  Font plain("color-sbix.ttf");           // flags 1
  EXPECT_FALSE(image_of(plain, 1, 1).draw_outlines);
  Font outlined("color-sbix-dupes.ttf");  // flags 3
  EXPECT_TRUE(image_of(outlined, 1, 0).draw_outlines);
  Font cbdt("color-cbdt.ttf");
  EXPECT_FALSE(image_of(cbdt, 1, 0).draw_outlines);
}

TEST(SbixExtents, TheChainLimitIsHarfBuzzs) {
  Font font("color-sbix-dupes.ttf");
  EXPECT_BOX(extents(font, 9), 0, 400, 400, -400);   // 8 x 8 at 50 units a pixel
  EXPECT_BOX(extents(font, 10), 0, 0, 0, 0);
  EXPECT_BOX(extents(font, 280), 150, 300, 300, -250);   // 6 x 5 at (3, 1): top is 6
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
