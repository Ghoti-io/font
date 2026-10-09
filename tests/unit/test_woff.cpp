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
 * WOFF 1 as a wrapper: the sfnt it holds is rebuilt, and then the face is an
 * ordinary one. Two fixtures carry it - one written by fontTools, one by the
 * generator with every table stored - and the damaged cases are those two files
 * with a field changed, so each refusal is one the format's own rules make.
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/metrics.h>

#include <gtest/gtest.h>

namespace {

constexpr size_t kHeader = 44;
constexpr size_t kEntry = 20;

std::string read_fixture(const std::string & name) {
  const std::string path = gfnttest::data("fonts/" + name);
  FILE * handle = fopen(path.c_str(), "rb");
  std::string out;
  if (!handle) {
    return out;
  }
  char buffer[4096];
  size_t got;
  while ((got = fread(buffer, 1, sizeof buffer, handle)) > 0) {
    out.append(buffer, got);
  }
  fclose(handle);
  return out;
}

uint32_t get32(const std::string & s, size_t at) {
  return (uint32_t(uint8_t(s[at])) << 24) | (uint32_t(uint8_t(s[at + 1])) << 16)
      | (uint32_t(uint8_t(s[at + 2])) << 8) | uint32_t(uint8_t(s[at + 3]));
}

void put32(std::string & s, size_t at, uint32_t v) {
  s[at] = char(v >> 24);
  s[at + 1] = char(v >> 16);
  s[at + 2] = char(v >> 8);
  s[at + 3] = char(v);
}

void put16(std::string & s, size_t at, uint32_t v) {
  s[at] = char(v >> 8);
  s[at + 1] = char(v);
}

/** A face made from bytes the test owns. */
struct Crafted {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  GFNT_Result result = GFNT_ERR_INTERNAL;

  explicit Crafted(const std::string & bytes,
      const GFNT_Limits * limits = nullptr, size_t index = 0) {
    gfnt_error_clear(&error);
    result = gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_COPY,
        nullptr, nullptr, &blob, &error);
    if (result != GFNT_OK) {
      return;
    }
    result = gfnt_face_load(blob, index, limits, nullptr, &face, &error);
  }

  ~Crafted() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }

  Crafted(const Crafted &) = delete;
  Crafted & operator=(const Crafted &) = delete;
};

/** The index of the first directory entry that is stored compressed, or -1. */
int first_compressed(const std::string & woff) {
  const size_t count = (uint8_t(woff[12]) << 8) | uint8_t(woff[13]);
  for (size_t i = 0; i < count; ++i) {
    const size_t at = kHeader + i * kEntry;
    if (get32(woff, at + 8) < get32(woff, at + 12)) {
      return int(i);
    }
  }
  return -1;
}

std::string gzip_of(const std::string & data) {
  void * out = nullptr;
  size_t size = 0;
  std::string result;

  if (gcomp_encode_alloc(nullptr, "gzip", nullptr, data.data(), data.size(), &out,
          &size) != GCOMP_OK) {
    return result;
  }
  result.assign(static_cast<const char *>(out), size);
  gcomp_buffer_free(nullptr, out);
  return result;
}

const char * const kWoffs[] = {"woff-basic.woff", "woff-stored.woff"};

}  // namespace

TEST(Woff, EitherFixtureIsBasicTtf) {
  // The wrapper decides nothing about what the font is: the same flavour, the same
  // directory (tags and lengths), the same glyph for a code point and the same
  // advance as the sfnt both files were made from.
  const std::string plain_bytes = read_fixture("basic.ttf");
  ASSERT_FALSE(plain_bytes.empty());
  Crafted plain(plain_bytes);
  ASSERT_EQ(plain.result, GFNT_OK);

  for (const char * name : kWoffs) {
    const std::string bytes = read_fixture(name);
    ASSERT_FALSE(bytes.empty()) << name;
    Crafted woff(bytes);
    ASSERT_EQ(woff.result, GFNT_OK) << name << ": " << woff.error.message;

    EXPECT_EQ(gfnt_face_flavour(woff.face), gfnt_face_flavour(plain.face)) << name;
    ASSERT_EQ(gfnt_face_table_count(woff.face), gfnt_face_table_count(plain.face))
        << name;
    for (size_t i = 0; i < gfnt_face_table_count(plain.face); ++i) {
      GFNT_Tag tag = 0;
      ASSERT_EQ(gfnt_face_table_tag_at(plain.face, i, &tag), GFNT_OK);
      size_t offset = 0, length = 0, plain_length = 0;
      ASSERT_EQ(gfnt_face_table_range(woff.face, tag, &offset, &length), GFNT_OK)
          << name << " lost a table";
      ASSERT_EQ(gfnt_face_table_range(plain.face, tag, &offset, &plain_length),
          GFNT_OK);
      EXPECT_EQ(length, plain_length) << name << " table " << i;
    }

    size_t glyphs = 0, plain_glyphs = 0;
    ASSERT_EQ(gfnt_face_num_glyphs(woff.face, &glyphs, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_num_glyphs(plain.face, &plain_glyphs, nullptr), GFNT_OK);
    EXPECT_EQ(glyphs, plain_glyphs) << name;
    for (uint32_t cp : {0x41u, 0x42u, 0x43u, 0x20u, 0x3042u}) {
      uint32_t a = 99, b = 98;
      ASSERT_EQ(gfnt_face_glyph_for_codepoint(woff.face, cp, &a, nullptr), GFNT_OK);
      ASSERT_EQ(gfnt_face_glyph_for_codepoint(plain.face, cp, &b, nullptr), GFNT_OK);
      EXPECT_EQ(a, b) << name << " U+" << std::hex << cp;
    }
    for (uint32_t glyph = 0; glyph < glyphs; ++glyph) {
      int32_t a = -1, b = -2;
      ASSERT_EQ(gfnt_face_glyph_advance(woff.face, glyph, nullptr, &a, nullptr),
          GFNT_OK);
      ASSERT_EQ(gfnt_face_glyph_advance(plain.face, glyph, nullptr, &b, nullptr),
          GFNT_OK);
      EXPECT_EQ(a, b) << name << " glyph " << glyph;
    }
  }
}

TEST(Woff, TheFixturesHoldBothKindsOfTable) {
  // This asserts the fixtures, so that the other tests mean something: one file
  // has a table fontTools compressed and a table it stored, the other has only
  // stored ones.
  const std::string basic = read_fixture("woff-basic.woff");
  const std::string stored = read_fixture("woff-stored.woff");
  ASSERT_FALSE(basic.empty());
  ASSERT_FALSE(stored.empty());
  EXPECT_GE(first_compressed(basic), 0) << "no table in the fontTools file is compressed";
  EXPECT_EQ(first_compressed(stored), -1);
  const size_t count = (uint8_t(basic[12]) << 8) | uint8_t(basic[13]);
  bool has_stored = false;
  for (size_t i = 0; i < count; ++i) {
    const size_t at = kHeader + i * kEntry;
    has_stored |= get32(basic, at + 8) == get32(basic, at + 12);
  }
  EXPECT_TRUE(has_stored) << "no table in the fontTools file is stored as it is";
}

TEST(Woff, OneFaceAndOnlyFaceZero) {
  const std::string bytes = read_fixture("woff-basic.woff");
  GFNT_Blob * blob = nullptr;
  ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_COPY,
      nullptr, nullptr, &blob, nullptr), GFNT_OK);
  size_t faces = 0;
  EXPECT_EQ(gfnt_face_count(blob, nullptr, &faces, nullptr), GFNT_OK);
  EXPECT_EQ(faces, 1u);
  GFNT_Face * face = nullptr;
  EXPECT_EQ(gfnt_face_load(blob, 1, nullptr, nullptr, &face, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(face, nullptr);
  gfnt_blob_destroy(blob);
}

TEST(Woff, AGzippedWoffIsStillOne) {
  // Nobody ships one, and it is the case where two wrappers are undone in order and
  // each frees the bytes it consumed.
  const std::string zipped = gzip_of(read_fixture("woff-basic.woff"));
  ASSERT_FALSE(zipped.empty());
  Crafted face(zipped);
  EXPECT_EQ(face.result, GFNT_OK) << face.error.message;
}

TEST(Woff, BytesAfterTheStatedEndAreTolerated) {
  std::string bytes = read_fixture("woff-stored.woff");
  bytes.append("trailing");
  Crafted face(bytes);
  EXPECT_EQ(face.result, GFNT_OK) << face.error.message;
}

TEST(Woff, TheHeaderIsHeldToTheFormat) {
  const std::string good = read_fixture("woff-stored.woff");
  ASSERT_FALSE(good.empty());

  {
    std::string bytes = good;
    put16(bytes, 14, 1);
    Crafted face(bytes);
    EXPECT_EQ(face.result, GFNT_ERR_CORRUPT) << "reserved must be zero";
  }
  {
    std::string bytes = good;
    put32(bytes, 8, uint32_t(good.size() + 1));
    Crafted face(bytes);
    EXPECT_EQ(face.result, GFNT_ERR_CORRUPT) << "the file is shorter than it says";
  }
  {
    std::string bytes = good;
    put16(bytes, 12, 0);
    Crafted face(bytes);
    EXPECT_EQ(face.result, GFNT_ERR_CORRUPT) << "no tables";
  }
  {
    std::string bytes = good.substr(0, 30);
    Crafted face(bytes);
    EXPECT_EQ(face.result, GFNT_ERR_CORRUPT) << "cut inside the header";
  }
  {
    std::string bytes = good;
    put16(bytes, 12, 0xFFFF);
    put32(bytes, 8, uint32_t(bytes.size()));
    Crafted face(bytes);
    EXPECT_NE(face.result, GFNT_OK) << "a directory larger than the file";
  }
}

TEST(Woff, TheDirectoryIsHeldToTheFormat) {
  const std::string good = read_fixture("woff-stored.woff");
  const size_t e0 = kHeader;
  const size_t e1 = kHeader + kEntry;

  {
    std::string bytes = good;
    put32(bytes, e0 + 8, get32(good, e0 + 12) + 1);
    Crafted face(bytes);
    EXPECT_EQ(face.result, GFNT_ERR_CORRUPT) << "stored larger than the table";
  }
  {
    std::string bytes = good;
    put32(bytes, e0 + 4, uint32_t(good.size()));
    Crafted face(bytes);
    EXPECT_EQ(face.result, GFNT_ERR_CORRUPT) << "a table past the end";
  }
  {
    std::string bytes = good;
    put32(bytes, e0 + 4, 8);
    Crafted face(bytes);
    EXPECT_EQ(face.result, GFNT_ERR_CORRUPT) << "a table inside the directory";
  }
  {
    std::string bytes = good;
    for (size_t k = 0; k < 4; ++k) {
      std::swap(bytes[e0 + k], bytes[e1 + k]);
    }
    Crafted face(bytes);
    EXPECT_EQ(face.result, GFNT_ERR_CORRUPT) << "tags out of order";
  }
  {
    std::string bytes = good;
    for (size_t k = 0; k < 4; ++k) {
      bytes[e1 + k] = bytes[e0 + k];
    }
    Crafted face(bytes);
    EXPECT_EQ(face.result, GFNT_ERR_CORRUPT) << "a repeated tag";
  }
}

TEST(Woff, ACompressedTableMustInflateToItsStatedLength) {
  const std::string good = read_fixture("woff-basic.woff");
  const int i = first_compressed(good);
  ASSERT_GE(i, 0);
  const size_t at = kHeader + size_t(i) * kEntry;

  for (int delta : {1, 8, -1}) {
    std::string bytes = good;
    put32(bytes, at + 12, uint32_t(int64_t(get32(good, at + 12)) + delta));
    Crafted face(bytes);
    EXPECT_EQ(face.result, GFNT_ERR_CORRUPT) << "orig length off by " << delta;
  }
  {
    // Damage the compressed bytes themselves.
    std::string bytes = good;
    const size_t offset = get32(good, at + 4);
    bytes[offset + 2] = char(bytes[offset + 2] ^ 0xFF);
    bytes[offset + 3] = char(bytes[offset + 3] ^ 0xFF);
    Crafted face(bytes);
    EXPECT_EQ(face.result, GFNT_ERR_CORRUPT) << "a damaged zlib stream";
  }
}

TEST(Woff, TheLimitsHold) {
  const std::string good = read_fixture("woff-basic.woff");
  {
    GFNT_Limits limits;
    gfnt_limits_default(&limits);
    limits.max_tables = 2;
    Crafted face(good, &limits);
    EXPECT_EQ(face.result, GFNT_ERR_LIMIT);
  }
  {
    GFNT_Limits limits;
    gfnt_limits_default(&limits);
    limits.max_blob_bytes = 200;
    Crafted face(good, &limits);
    EXPECT_NE(face.result, GFNT_OK);
  }
}

TEST(Woff, ADeclaredSizeIsACeilingNotAnAllocation) {
  // A table that claims to inflate to the largest size the limits allow, but is
  // stored as a few bytes of zlib: the decoder is told the claimed length as its
  // ceiling, so the claim is what is refused, and the refusal is not an
  // allocation of that size.
  std::string bytes = read_fixture("woff-basic.woff");
  const int i = first_compressed(bytes);
  ASSERT_GE(i, 0);
  const size_t at = kHeader + size_t(i) * kEntry;
  put32(bytes, at + 12, 0x7FFFFFF0u);
  Crafted face(bytes);
  EXPECT_NE(face.result, GFNT_OK);
}

TEST(Woff, AnAllocationFailureAnywhereIsReported) {
  const std::string bytes = read_fixture("woff-basic.woff");
  size_t refused = 0;
  for (size_t fail_at = 0; fail_at < 60; ++fail_at) {
    gfnttest::FailingAllocator allocator(fail_at);
    GFNT_Blob * blob = nullptr;
    ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
    GFNT_Face * face = nullptr;
    GFNT_Error error{};
    const GFNT_Result result = gfnt_face_load(blob, 0, nullptr, allocator.get(),
        &face, &error);
    if (result != GFNT_OK) {
      ++refused;
      EXPECT_EQ(result, GFNT_ERR_OOM) << "allocation " << fail_at;
      EXPECT_EQ(face, nullptr) << "allocation " << fail_at;
    }
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    EXPECT_EQ(allocator.live(), 0u) << "allocation " << fail_at << " leaked";
  }
  EXPECT_GT(refused, 3u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
