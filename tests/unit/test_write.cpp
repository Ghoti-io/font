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
 * The serialisers. A font is written from the tables of a fixture and read back
 * through the ordinary loader, which is the check that matters: the writer is
 * right when the reader finds every table where the directory says it is, with
 * the bytes it was given, and the checksums the format requires.
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/write.h>

#include <gtest/gtest.h>

namespace {

const GFNT_Tag kHead = GFNT_TAG('h', 'e', 'a', 'd');

struct Source {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  std::vector<std::vector<uint8_t>> bytes;
  std::vector<GFNT_WriteTable> tables;

  explicit Source(const std::string & name) {
    EXPECT_EQ(gfnt_blob_create_file(gfnttest::data("fonts/" + name).c_str(),
        nullptr, nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr), GFNT_OK);
    const size_t n = gfnt_face_table_count(face);
    bytes.resize(n);
    for (size_t i = 0; i < n; ++i) {
      GFNT_Tag tag = 0;
      size_t offset = 0, length = 0;
      EXPECT_EQ(gfnt_face_table_tag_at(face, i, &tag), GFNT_OK);
      EXPECT_EQ(gfnt_face_table_range(face, tag, &offset, &length), GFNT_OK);
      const uint8_t * data = gfnt_blob_data(blob);
      bytes[i].assign(data + offset, data + offset + length);
      tables.push_back({tag, bytes[i].data(), bytes[i].size()});
    }
  }
  ~Source() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Source(const Source &) = delete;
  Source & operator=(const Source &) = delete;
};

struct Loaded {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Result result = GFNT_OK;

  explicit Loaded(GFNT_Blob * owned) : blob(owned) {
    result = gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr);
  }
  ~Loaded() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Loaded(const Loaded &) = delete;
  Loaded & operator=(const Loaded &) = delete;
};

std::vector<uint8_t> table_of(const GFNT_Face * face, GFNT_Blob * blob, GFNT_Tag tag) {
  size_t offset = 0, length = 0;
  EXPECT_EQ(gfnt_face_table_range(face, tag, &offset, &length), GFNT_OK);
  const uint8_t * data = gfnt_blob_data(blob);
  return std::vector<uint8_t>(data + offset, data + offset + length);
}

uint32_t sum_words(const uint8_t * data, size_t length) {
  uint32_t sum = 0;
  for (size_t i = 0; i < length; i += 4) {
    uint32_t w = 0;
    for (size_t k = 0; k < 4; ++k) {
      w = (w << 8) | (i + k < length ? data[i + k] : 0);
    }
    sum += w;
  }
  return sum;
}

}  // namespace

TEST(WriteSfnt, ReadsBackTableForTable) {
  Source source("basic.ttf");
  GFNT_Blob * out = nullptr;
  GFNT_Error error{};
  ASSERT_EQ(gfnt_write_sfnt(gfnt_face_flavour(source.face), source.tables.data(),
      source.tables.size(), nullptr, nullptr, &out, &error), GFNT_OK);
  Loaded back(out);
  ASSERT_EQ(back.result, GFNT_OK);
  EXPECT_EQ(gfnt_face_table_count(back.face), source.tables.size());
  for (size_t i = 0; i < source.tables.size(); ++i) {
    const GFNT_Tag tag = source.tables[i].tag;
    std::vector<uint8_t> got = table_of(back.face, back.blob, tag);
    std::vector<uint8_t> want = source.bytes[i];
    if (tag == kHead) {
      // checkSumAdjustment is the one field the writer owns.
      ASSERT_GE(got.size(), 12u);
      std::fill(got.begin() + 8, got.begin() + 12, 0);
      std::fill(want.begin() + 8, want.begin() + 12, 0);
    }
    EXPECT_EQ(got, want) << "table " << i;
  }
}

TEST(WriteSfnt, ChecksumsAreWhatTheFormatRequires) {
  Source source("basic.ttf");
  GFNT_Blob * out = nullptr;
  ASSERT_EQ(gfnt_write_sfnt(gfnt_face_flavour(source.face), source.tables.data(),
      source.tables.size(), nullptr, nullptr, &out, nullptr), GFNT_OK);
  Loaded back(out);
  ASSERT_EQ(back.result, GFNT_OK);
  for (size_t i = 0; i < gfnt_face_table_count(back.face); ++i) {
    GFNT_Tag tag = 0;
    ASSERT_EQ(gfnt_face_table_tag_at(back.face, i, &tag), GFNT_OK);
    uint32_t stored = 0, computed = 1;
    ASSERT_EQ(gfnt_face_table_checksum(back.face, tag, &stored, &computed), GFNT_OK);
    EXPECT_EQ(stored, computed) << "table " << i;
  }
  // The whole file, adjustment included, sums to the specification's constant.
  EXPECT_EQ(sum_words(gfnt_blob_data(back.blob), gfnt_blob_size(back.blob)),
      0xB1B0AFBAu);
}

TEST(WriteSfnt, SortsPadsAndDerivesTheSearchFields) {
  const uint8_t a[] = {1, 2, 3, 4, 5};
  const uint8_t b[] = {9};
  const uint8_t c[] = {7, 7, 7, 7};
  // Deliberately out of tag order, with two lengths that are not a multiple of 4.
  GFNT_WriteTable tables[] = {
    {GFNT_TAG('z', 'z', 'z', 'z'), c, sizeof c},
    {GFNT_TAG('a', 'a', 'a', 'a'), a, sizeof a},
    {GFNT_TAG('m', 'm', 'm', 'm'), b, sizeof b},
  };
  GFNT_Blob * out = nullptr;
  ASSERT_EQ(gfnt_write_sfnt(GFNT_TAG('O', 'T', 'T', 'O'), tables, 3, nullptr,
      nullptr, &out, nullptr), GFNT_OK);
  const uint8_t * p = gfnt_blob_data(out);
  EXPECT_EQ(std::string((const char *)p, 4), "OTTO");
  EXPECT_EQ(p[5], 3);                    // numTables
  EXPECT_EQ((p[6] << 8) | p[7], 32);     // searchRange: 2^1 * 16
  EXPECT_EQ((p[8] << 8) | p[9], 1);      // entrySelector
  EXPECT_EQ((p[10] << 8) | p[11], 16);   // rangeShift: 3*16 - 32
  EXPECT_EQ(std::string((const char *)p + 12, 4), "aaaa");
  EXPECT_EQ(std::string((const char *)p + 28, 4), "mmmm");
  EXPECT_EQ(std::string((const char *)p + 44, 4), "zzzz");
  // 12 + 3*16 = 60; aaaa at 60 (8 with padding), mmmm at 68 (4), zzzz at 72.
  EXPECT_EQ(p[12 + 8 + 3], 60);
  EXPECT_EQ(p[28 + 8 + 3], 68);
  EXPECT_EQ(p[44 + 8 + 3], 72);
  EXPECT_EQ(p[12 + 15], 5);              // the directory states the unpadded length
  EXPECT_EQ(gfnt_blob_size(out), 76u);
  EXPECT_EQ(p[60 + 5], 0);               // padding is zeros
  gfnt_blob_destroy(out);
}

TEST(WriteSfnt, RefusesWhatItCannotWrite) {
  const uint8_t one[] = {1};
  GFNT_Blob * out = nullptr;
  GFNT_WriteTable twice[] = {{GFNT_TAG('a', 'a', 'a', 'a'), one, 1},
                             {GFNT_TAG('a', 'a', 'a', 'a'), one, 1}};
  EXPECT_EQ(gfnt_write_sfnt(0x00010000, twice, 2, nullptr, nullptr, &out, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_write_sfnt(0x00010000, twice, 0, nullptr, nullptr, &out, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_write_sfnt(0x00010000, nullptr, 1, nullptr, nullptr, &out, nullptr),
      GFNT_ERR_INVALID);
  GFNT_WriteTable nodata[] = {{GFNT_TAG('a', 'a', 'a', 'a'), nullptr, 4}};
  EXPECT_EQ(gfnt_write_sfnt(0x00010000, nodata, 1, nullptr, nullptr, &out, nullptr),
      GFNT_ERR_INVALID);
  GFNT_WriteTable empty[] = {{GFNT_TAG('a', 'a', 'a', 'a'), nullptr, 0}};
  EXPECT_EQ(gfnt_write_sfnt(0x00010000, empty, 1, nullptr, nullptr, &out, nullptr),
      GFNT_OK) << "an empty table is allowed";
  gfnt_blob_destroy(out);
  EXPECT_EQ(gfnt_write_sfnt(0x00010000, empty, 1, nullptr, nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);

  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_tables = 1;
  out = nullptr;
  EXPECT_EQ(gfnt_write_sfnt(0x00010000, twice, 2, &limits, nullptr, &out, nullptr),
      GFNT_ERR_LIMIT);
  gfnt_limits_default(&limits);
  limits.max_blob_bytes = 20;
  EXPECT_EQ(gfnt_write_sfnt(0x00010000, empty, 1, &limits, nullptr, &out, nullptr),
      GFNT_ERR_LIMIT) << "the header and directory alone exceed 20 bytes";
  EXPECT_EQ(out, nullptr);
}

TEST(WriteWoff, ReadsBackWhatWasPutIn) {
  Source source("basic.ttf");
  GFNT_Blob * out = nullptr;
  ASSERT_EQ(gfnt_write_woff(gfnt_face_flavour(source.face), source.tables.data(),
      source.tables.size(), nullptr, nullptr, &out, nullptr), GFNT_OK);
  const uint8_t * p = gfnt_blob_data(out);
  EXPECT_EQ(std::string((const char *)p, 4), "wOFF");
  // The stated length is the file's.
  EXPECT_EQ(size_t((p[8] << 24) | (p[9] << 16) | (p[10] << 8) | p[11]),
      gfnt_blob_size(out));
  size_t stored = 0, compressed = 0;
  const size_t n = (p[12] << 8) | p[13];
  for (size_t i = 0; i < n; ++i) {
    const uint8_t * e = p + 44 + i * 20;
    const uint32_t comp = (e[8] << 24) | (e[9] << 16) | (e[10] << 8) | e[11];
    const uint32_t orig = (e[12] << 24) | (e[13] << 16) | (e[14] << 8) | e[15];
    EXPECT_LE(comp, orig);
    (comp < orig ? compressed : stored)++;
  }
  EXPECT_GT(compressed, 0u) << "a fixture's glyf is compressible";
  Loaded back(out);
  ASSERT_EQ(back.result, GFNT_OK);
  EXPECT_EQ(gfnt_face_table_count(back.face), source.tables.size());
  for (size_t i = 0; i < source.tables.size(); ++i) {
    GFNT_Tag tag = source.tables[i].tag;
    uint32_t stored = 0, computed = 1;
    ASSERT_EQ(gfnt_face_table_checksum(back.face, tag, &stored, &computed), GFNT_OK);
    EXPECT_EQ(stored, computed) << "table " << i;
  }
}

TEST(WriteWoff, StoresATableThatDoesNotShrink) {
  // Eight bytes of noise cannot be made smaller by zlib's framing.
  const uint8_t noise[] = {0x9d, 0x31, 0xe7, 0x02, 0xc4, 0x5b, 0x88, 0x6f};
  GFNT_WriteTable tables[] = {{GFNT_TAG('n', 'o', 'i', 's'), noise, sizeof noise}};
  GFNT_Blob * out = nullptr;
  ASSERT_EQ(gfnt_write_woff(0x00010000, tables, 1, nullptr, nullptr, &out, nullptr),
      GFNT_OK);
  const uint8_t * p = gfnt_blob_data(out);
  const uint8_t * e = p + 44;
  EXPECT_EQ(e[11], 8);   // compLength
  EXPECT_EQ(e[15], 8);   // origLength
  gfnt_blob_destroy(out);
}

TEST(Write, AnAllocationFailureAnywhereIsReported) {
  Source source("basic.ttf");
  size_t refused = 0;
  for (int which = 0; which < 2; ++which) {
    for (size_t fail_at = 0; fail_at < 40; ++fail_at) {
      gfnttest::FailingAllocator allocator(fail_at);
      GFNT_Blob * out = nullptr;
      GFNT_Error error{};
      const GFNT_Result result = which == 0
          ? gfnt_write_sfnt(gfnt_face_flavour(source.face), source.tables.data(),
                source.tables.size(), nullptr, allocator.get(), &out, &error)
          : gfnt_write_woff(gfnt_face_flavour(source.face), source.tables.data(),
                source.tables.size(), nullptr, allocator.get(), &out, &error);
      if (result != GFNT_OK) {
        ++refused;
        EXPECT_EQ(result, GFNT_ERR_OOM) << which << " allocation " << fail_at;
        EXPECT_EQ(out, nullptr);
      }
      gfnt_blob_destroy(out);
      EXPECT_EQ(allocator.live(), 0u) << which << " allocation " << fail_at << " leaked";
    }
  }
  EXPECT_GT(refused, 4u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
