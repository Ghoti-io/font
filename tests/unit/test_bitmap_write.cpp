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
 * The bitmap font writers: every bitmap fixture written as every container and
 * read back, with the refusals each container is documented to make.
 *
 * Two containers keep a glyph's box and baseline (BDF, PCF) and two do not (PSF,
 * `.hex`: a cell and no bearings), so what is compared is what survives in all of
 * them: the advance, and which pixels are on, positioned from the left edge and
 * from the top of the line. A glyph is found by codepoint where the source states
 * an encoding and by index where it does not.
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <ghoti.io/font/bitmap.h>
#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/write.h>

#include <gtest/gtest.h>

namespace {

struct Loaded {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Result result = GFNT_ERR_INTERNAL;
  GFNT_Error error{};

  Loaded(const std::string & bytes) {
    gfnt_error_clear(&error);
    result = gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_COPY,
        nullptr, nullptr, &blob, &error);
    if (result == GFNT_OK) {
      result = gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error);
    }
  }
  ~Loaded() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Loaded(const Loaded &) = delete;
  Loaded & operator=(const Loaded &) = delete;
};

std::string read_fixture(const std::string & name) {
  FILE * handle = fopen(gfnttest::data("fonts/" + name).c_str(), "rb");
  std::string out;
  if (!handle) return out;
  char buffer[4096];
  size_t got;
  while ((got = fread(buffer, 1, sizeof buffer, handle)) > 0) out.append(buffer, got);
  fclose(handle);
  return out;
}

struct Picture {
  int32_t advance = 0;
  std::set<std::pair<int32_t, int32_t>> pixels;  // (x from the pen, row from the top)
  bool operator==(const Picture & o) const {
    return advance == o.advance && pixels == o.pixels;
  }
};

/** Glyph @p glyph of @p face as a Picture, or false when the strike lacks it. */
bool picture_of(const GFNT_Face * face, uint32_t glyph, const GFNT_Strike & strike,
    Picture * out) {
  GFNT_BitmapGlyph g;
  if (gfnt_face_glyph_bitmap(face, glyph, strike.index, &g, nullptr) != GFNT_OK) {
    return false;
  }
  out->advance = g.advance;
  out->pixels.clear();
  for (uint32_t y = 0; y < g.height; ++y) {
    for (uint32_t x = 0; x < g.width; ++x) {
      if (gfnt_bitmap_pixel(&g, x, y)) {
        out->pixels.insert({g.bearing_x + int32_t(x),
            strike.ascent - g.bearing_y + int32_t(y)});
      }
    }
  }
  return true;
}

/** Everything a face says, keyed by codepoint, or by index when it has no encoding. */
std::map<int64_t, Picture> pictures(const GFNT_Face * face, bool * by_codepoint) {
  std::map<int64_t, Picture> out;
  GFNT_Strike strike{};
  EXPECT_EQ(gfnt_face_strike_at(face, 0, &strike, nullptr), GFNT_OK);
  size_t pairs = 0;
  *by_codepoint = gfnt_face_bitmap_encoding_count(face, &pairs, nullptr) == GFNT_OK
      && pairs > 0;
  if (*by_codepoint) {
    for (size_t i = 0; i < pairs; ++i) {
      uint32_t cp = 0, glyph = 0;
      EXPECT_EQ(gfnt_face_bitmap_encoding_at(face, i, &cp, &glyph, nullptr), GFNT_OK);
      Picture p;
      if (picture_of(face, glyph, strike, &p)) out[cp] = p;
    }
  } else {
    size_t count = 0;
    EXPECT_EQ(gfnt_face_num_glyphs(face, &count, nullptr), GFNT_OK);
    for (uint32_t g = 0; g < count; ++g) {
      Picture p;
      if (picture_of(face, g, strike, &p)) out[g] = p;
    }
  }
  return out;
}

std::string write_as(const GFNT_Face * face, GFNT_BitmapFormat format,
    GFNT_Result * result) {
  GFNT_Blob * out = nullptr;
  GFNT_Error error{};
  *result = gfnt_write_bitmap_font(face, 0, format, nullptr, nullptr, &out, &error);
  std::string bytes;
  if (*result == GFNT_OK) {
    bytes.assign(reinterpret_cast<const char *>(gfnt_blob_data(out)),
        gfnt_blob_size(out));
  } else {
    EXPECT_EQ(out, nullptr);
  }
  gfnt_blob_destroy(out);
  return bytes;
}

const GFNT_BitmapFormat kFormats[] = {GFNT_BITMAP_FORMAT_BDF, GFNT_BITMAP_FORMAT_PCF,
    GFNT_BITMAP_FORMAT_PSF1, GFNT_BITMAP_FORMAT_PSF2, GFNT_BITMAP_FORMAT_HEX};
const char * const kFormatNames[] = {"bdf", "pcf", "psf1", "psf2", "hex"};

const char * const kSources[] = {"bitmap.bdf", "bitmap.pcf", "bitmap.psf",
    "bitmap-v1.psf", "bitmap.hex", "bitmap-wide.hex", "bitmap-lsb.pcf",
    "bitmap-swap.pcf", "bitmap-ink.bdf", "strikes.ttf"};

/**
 * Whether the container is documented to refuse this source: a cell format needs
 * one advance (and PSF 1 needs it to be 8), `.hex` needs 16 rows, an encoding and
 * codepoints it can print, and a two-byte encoding table cannot name U+10000 up.
 */
bool refused(const GFNT_Face * face, const std::map<int64_t, Picture> & shown,
    bool by_codepoint, GFNT_BitmapFormat format) {
  std::set<int32_t> advances;
  for (const auto & entry : shown) advances.insert(entry.second.advance);
  GFNT_Strike strike{};
  gfnt_face_strike_at(face, 0, &strike, nullptr);
  bool wide_codepoint = false;
  if (by_codepoint) {
    for (const auto & entry : shown) wide_codepoint |= entry.first > 0xFFFF;
  }
  // A glyph that sticks out of the cell a cell format would give it: left of the
  // pen, past the advance, above the line or below it.
  const int32_t height = strike.ascent - strike.descent;
  bool outside = false;
  bool outside_hex = false;
  for (const auto & entry : shown) {
    for (const auto & pixel : entry.second.pixels) {
      outside_hex |= pixel.first < 0 || pixel.second < 0 || pixel.second >= 16;
      outside |= pixel.first < 0 || pixel.first >= entry.second.advance
          || pixel.second < 0 || pixel.second >= height;
    }
  }
  switch (format) {
    case GFNT_BITMAP_FORMAT_BDF: return false;
    case GFNT_BITMAP_FORMAT_PCF: return wide_codepoint;
    case GFNT_BITMAP_FORMAT_PSF1:
      return advances.size() != 1 || *advances.begin() != 8 || wide_codepoint
          || outside;
    case GFNT_BITMAP_FORMAT_PSF2: return advances.size() != 1 || outside;
    case GFNT_BITMAP_FORMAT_HEX:
      return strike.ascent - strike.descent != 16 || !by_codepoint || outside_hex;
    default: return false;
  }
}

}  // namespace

TEST(BitmapWrite, EveryFixtureSurvivesEveryContainerItCanBeWrittenAs) {
  size_t written = 0;
  size_t refusals = 0;
  for (const char * name : kSources) {
    const std::string bytes = read_fixture(name);
    ASSERT_FALSE(bytes.empty()) << name;
    Loaded source(bytes);
    ASSERT_EQ(source.result, GFNT_OK) << name;
    bool source_codepoints = false;
    const auto expected = pictures(source.face, &source_codepoints);
    ASSERT_FALSE(expected.empty()) << name;
    for (size_t f = 0; f < 5; ++f) {
      const std::string label = std::string(name) + " as " + kFormatNames[f];
      GFNT_Result result = GFNT_ERR_INTERNAL;
      const std::string out = write_as(source.face, kFormats[f], &result);
      if (refused(source.face, expected, source_codepoints, kFormats[f])) {
        EXPECT_EQ(result, GFNT_ERR_UNSUPPORTED) << label;
        ++refusals;
        continue;
      }
      ASSERT_EQ(result, GFNT_OK) << label;
      Loaded back(out);
      ASSERT_EQ(back.result, GFNT_OK) << label << ": " << back.error.message;
      bool back_codepoints = false;
      const auto got = pictures(back.face, &back_codepoints);
      if (source_codepoints) {
        EXPECT_TRUE(back_codepoints) << label;
      }
      if (source_codepoints == back_codepoints) {
        EXPECT_EQ(got.size(), expected.size()) << label;
        for (const auto & entry : expected) {
          auto found = got.find(entry.first);
          ASSERT_NE(found, got.end()) << label << " lost " << entry.first;
          EXPECT_TRUE(found->second == entry.second)
              << label << " changed " << entry.first;
        }
      }
      ++written;
    }
  }
  EXPECT_GE(written, 30u);
  EXPECT_GE(refusals, 4u) << "the refusals are exercised, not assumed";
}

TEST(BitmapWrite, TheBaselineSurvivesTheContainersThatStateOne) {
  for (const char * name : {"bitmap.bdf", "bitmap.pcf", "strikes.ttf"}) {
    Loaded source(read_fixture(name));
    ASSERT_EQ(source.result, GFNT_OK);
    GFNT_Strike want{};
    ASSERT_EQ(gfnt_face_strike_at(source.face, 0, &want, nullptr), GFNT_OK);
    for (GFNT_BitmapFormat format : {GFNT_BITMAP_FORMAT_BDF, GFNT_BITMAP_FORMAT_PCF}) {
      GFNT_Result result;
      Loaded back(write_as(source.face, format, &result));
      ASSERT_EQ(result, GFNT_OK) << name;
      ASSERT_EQ(back.result, GFNT_OK) << name;
      GFNT_Strike got{};
      ASSERT_EQ(gfnt_face_strike_at(back.face, 0, &got, nullptr), GFNT_OK);
      EXPECT_EQ(got.ascent, want.ascent) << name;
      EXPECT_EQ(got.descent, want.descent) << name;
      EXPECT_EQ(got.ppem_y, want.ppem_y) << name;
    }
  }
}

TEST(BitmapWrite, RefusesWhatItCannotWrite) {
  Loaded source(read_fixture("bitmap.bdf"));
  ASSERT_EQ(source.result, GFNT_OK);
  GFNT_Blob * out = nullptr;
  EXPECT_EQ(gfnt_write_bitmap_font(nullptr, 0, GFNT_BITMAP_FORMAT_BDF, nullptr,
      nullptr, &out, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_write_bitmap_font(source.face, 0, GFNT_BITMAP_FORMAT_BDF, nullptr,
      nullptr, nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_write_bitmap_font(source.face, 0, GFNT_BITMAP_FORMAT_COUNT, nullptr,
      nullptr, &out, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_write_bitmap_font(source.face, 7, GFNT_BITMAP_FORMAT_BDF, nullptr,
      nullptr, &out, nullptr), GFNT_ERR_INVALID) << "a strike the face lacks";
  EXPECT_EQ(out, nullptr);
  // An outline font has no strikes to write.
  Loaded outline(read_fixture("basic.ttf"));
  ASSERT_EQ(outline.result, GFNT_OK);
  EXPECT_NE(gfnt_write_bitmap_font(outline.face, 0, GFNT_BITMAP_FORMAT_BDF, nullptr,
      nullptr, &out, nullptr), GFNT_OK);
  EXPECT_EQ(out, nullptr);
  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_blob_bytes = 20;
  EXPECT_EQ(gfnt_write_bitmap_font(source.face, 0, GFNT_BITMAP_FORMAT_PCF, &limits,
      nullptr, &out, nullptr), GFNT_ERR_LIMIT);
}

TEST(BitmapWrite, AnAllocationFailureAnywhereIsReported) {
  Loaded source(read_fixture("bitmap.pcf"));
  ASSERT_EQ(source.result, GFNT_OK);
  for (GFNT_BitmapFormat format : kFormats) {
    size_t refused_count = 0;
    for (size_t fail_at = 0; fail_at < 80; ++fail_at) {
      gfnttest::FailingAllocator allocator(fail_at);
      GFNT_Blob * out = nullptr;
      const GFNT_Result result = gfnt_write_bitmap_font(source.face, 0, format,
          nullptr, allocator.get(), &out, nullptr);
      if (result != GFNT_OK) {
        ++refused_count;
        EXPECT_TRUE(result == GFNT_ERR_OOM || result == GFNT_ERR_UNSUPPORTED)
            << "allocation " << fail_at;
        EXPECT_EQ(out, nullptr);
      }
      gfnt_blob_destroy(out);
      EXPECT_EQ(allocator.live(), 0u) << "allocation " << fail_at << " leaked";
    }
    EXPECT_GT(refused_count, 3u);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
