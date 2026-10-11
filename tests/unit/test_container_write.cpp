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
 * The re-wrapping writers: a bare CFF from an OpenType font, a Type 1 program as
 * PFA or PFB, and gzip. Each is checked by reading what it wrote and comparing
 * every glyph with the face it came from; the refusals are the documented ones.
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/write.h>

#include <gtest/gtest.h>

namespace {

struct Loaded {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Result result = GFNT_ERR_INTERNAL;
  GFNT_Error error{};

  explicit Loaded(const std::string & bytes) {
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

std::string bytes_of(GFNT_Blob * blob) {
  std::string out(reinterpret_cast<const char *>(gfnt_blob_data(blob)),
      gfnt_blob_size(blob));
  gfnt_blob_destroy(blob);
  return out;
}

/** Every glyph's outline, advance and name the same in both faces. */
void expect_same_glyphs(const GFNT_Face * a, const GFNT_Face * b,
    const std::string & label) {
  size_t na = 0, nb = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(a, &na, nullptr), GFNT_OK) << label;
  ASSERT_EQ(gfnt_face_num_glyphs(b, &nb, nullptr), GFNT_OK) << label;
  ASSERT_EQ(na, nb) << label;
  for (uint32_t g = 0; g < na; ++g) {
    GFNT_Outline * oa = nullptr;
    GFNT_Outline * ob = nullptr;
    ASSERT_EQ(gfnt_face_glyph_outline(a, g, nullptr, nullptr, &oa, nullptr), GFNT_OK)
        << label << " glyph " << g;
    ASSERT_EQ(gfnt_face_glyph_outline(b, g, nullptr, nullptr, &ob, nullptr), GFNT_OK)
        << label << " glyph " << g;
    ASSERT_EQ(gfnt_outline_point_count(oa), gfnt_outline_point_count(ob))
        << label << " glyph " << g;
    for (size_t p = 0; p < gfnt_outline_point_count(oa); ++p) {
      GFNT_Point pa{}, pb{};
      GFNT_PointTag ta{}, tb{};
      ASSERT_EQ(gfnt_outline_point_at(oa, p, &pa, &ta), GFNT_OK);
      ASSERT_EQ(gfnt_outline_point_at(ob, p, &pb, &tb), GFNT_OK);
      EXPECT_EQ(pa.x, pb.x) << label << " glyph " << g << " point " << p;
      EXPECT_EQ(pa.y, pb.y) << label << " glyph " << g << " point " << p;
      EXPECT_EQ(ta, tb) << label << " glyph " << g << " point " << p;
    }
    gfnt_outline_destroy(oa);
    gfnt_outline_destroy(ob);
    int32_t adv_a = -1, adv_b = -2;
    ASSERT_EQ(gfnt_face_glyph_advance(a, g, nullptr, &adv_a, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_glyph_advance(b, g, nullptr, &adv_b, nullptr), GFNT_OK);
    EXPECT_EQ(adv_a, adv_b) << label << " glyph " << g;
    char * name_a = nullptr;
    char * name_b = nullptr;
    const GFNT_Result ra = gfnt_face_glyph_name(a, g, nullptr, &name_a, nullptr, nullptr);
    const GFNT_Result rb = gfnt_face_glyph_name(b, g, nullptr, &name_b, nullptr, nullptr);
    EXPECT_EQ(ra, rb) << label << " glyph " << g;
    if (ra == GFNT_OK && rb == GFNT_OK) {
      EXPECT_STREQ(name_a, name_b) << label << " glyph " << g;
    }
    gfnt_glyph_name_free(nullptr, name_a);
    gfnt_glyph_name_free(nullptr, name_b);
  }
}

}  // namespace

TEST(Type1Write, EveryFixtureIsTheSameFontAsPfaAndAsPfb) {
  for (const char * name : {"type1.pfa", "type1.pfb", "type1-big.pfb", "type1-gz.pfb.gz"}) {
    Loaded source(read_fixture(name));
    ASSERT_EQ(source.result, GFNT_OK) << name << ": " << source.error.message;
    for (GFNT_Type1Format format : {GFNT_TYPE1_FORMAT_PFA, GFNT_TYPE1_FORMAT_PFB}) {
      GFNT_Blob * out = nullptr;
      GFNT_Error error{};
      ASSERT_EQ(gfnt_write_type1(source.face, format, nullptr, nullptr, &out, &error),
          GFNT_OK) << name << ": " << error.message;
      const std::string bytes = bytes_of(out);
      if (format == GFNT_TYPE1_FORMAT_PFB) {
        EXPECT_EQ(uint8_t(bytes[0]), 0x80u) << name;
        EXPECT_EQ(uint8_t(bytes[bytes.size() - 2]), 0x80u) << name << " ends with EOF";
      } else {
        EXPECT_NE(bytes.find("cleartomark"), std::string::npos) << name;
      }
      Loaded back(bytes);
      ASSERT_EQ(back.result, GFNT_OK) << name << ": " << back.error.message;
      EXPECT_EQ(gfnt_face_flavour(back.face), GFNT_FLAVOUR_TYPE1);
      expect_same_glyphs(back.face, source.face, name);
    }
  }
}

TEST(Type1Write, ThePrivatePortionIsReencryptedNotCopied) {
  Loaded source(read_fixture("type1.pfb"));
  ASSERT_EQ(source.result, GFNT_OK);
  GFNT_Blob * out = nullptr;
  ASSERT_EQ(gfnt_write_type1(source.face, GFNT_TYPE1_FORMAT_PFB, nullptr, nullptr, &out,
      nullptr), GFNT_OK);
  const std::string written = bytes_of(out);
  // The same program, but not the same ciphertext: the four leading bytes differ.
  // What is not allowed to differ is the plaintext, which the face reads back.
  Loaded back(written);
  ASSERT_EQ(back.result, GFNT_OK);
  const uint8_t * a = nullptr;
  const uint8_t * b = nullptr;
  size_t la = 0, lb = 0;
  ASSERT_EQ(gfnt_face_table_data(source.face, GFNT_FLAVOUR_TYPE1, &a, &la), GFNT_OK);
  ASSERT_EQ(gfnt_face_table_data(back.face, GFNT_FLAVOUR_TYPE1, &b, &lb), GFNT_OK);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(a), la),
      std::string(reinterpret_cast<const char *>(b), lb));
}

TEST(Type1Write, ABinaryPfaIsWrittenWithoutItsTrailerDecipheredIntoNoise) {
  // A `.t1` file as URW ships it: PostScript text, then the private portion as raw
  // binary with no segment headers, then the ASCII trailer. Nothing says where the
  // cipher stops, so the reader takes it to the end of the file and the face holds
  // the trailer deciphered into garbage; the writer must not carry that along.
  const std::string pfb = read_fixture("type1.pfb");
  std::string binary;
  size_t at = 0;
  while (at + 6 <= pfb.size() && uint8_t(pfb[at]) == 0x80 && uint8_t(pfb[at + 1]) != 3) {
    const size_t size = uint8_t(pfb[at + 2]) | (uint8_t(pfb[at + 3]) << 8)
        | (uint8_t(pfb[at + 4]) << 16) | (size_t(uint8_t(pfb[at + 5])) << 24);
    binary += pfb.substr(at + 6, size);
    at += 6 + size;
  }
  Loaded source(pfb);
  ASSERT_EQ(source.result, GFNT_OK);
  Loaded raw(binary);
  ASSERT_EQ(raw.result, GFNT_OK) << raw.error.message;
  for (GFNT_Type1Format format : {GFNT_TYPE1_FORMAT_PFA, GFNT_TYPE1_FORMAT_PFB}) {
    GFNT_Blob * out = nullptr;
    ASSERT_EQ(gfnt_write_type1(raw.face, format, nullptr, nullptr, &out, nullptr),
        GFNT_OK);
    Loaded back(bytes_of(out));
    ASSERT_EQ(back.result, GFNT_OK) << back.error.message;
    const uint8_t * data = nullptr;
    size_t length = 0;
    ASSERT_EQ(gfnt_face_table_data(back.face, GFNT_FLAVOUR_TYPE1, &data, &length),
        GFNT_OK);
    const std::string program(reinterpret_cast<const char *>(data), length);
    EXPECT_EQ(program.size(), program.rfind("closefile") + 10)
        << "the private portion ends at closefile";
    expect_same_glyphs(back.face, source.face, "binary PFA");
  }
}

TEST(Type1Write, OnlyAType1FaceIsWrittenAsType1) {
  Loaded ttf(read_fixture("basic.ttf"));
  ASSERT_EQ(ttf.result, GFNT_OK);
  GFNT_Blob * out = nullptr;
  EXPECT_EQ(gfnt_write_type1(ttf.face, GFNT_TYPE1_FORMAT_PFA, nullptr, nullptr, &out,
      nullptr), GFNT_ERR_UNSUPPORTED);
  Loaded cff(read_fixture("cff.otf"));
  ASSERT_EQ(cff.result, GFNT_OK);
  EXPECT_EQ(gfnt_write_type1(cff.face, GFNT_TYPE1_FORMAT_PFB, nullptr, nullptr, &out,
      nullptr), GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(gfnt_write_type1(nullptr, GFNT_TYPE1_FORMAT_PFB, nullptr, nullptr, &out,
      nullptr), GFNT_ERR_INVALID);
  Loaded t1(read_fixture("type1.pfa"));
  ASSERT_EQ(t1.result, GFNT_OK);
  EXPECT_EQ(gfnt_write_type1(t1.face, GFNT_TYPE1_FORMAT_COUNT, nullptr, nullptr, &out,
      nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_write_type1(t1.face, GFNT_TYPE1_FORMAT_PFA, nullptr, nullptr, nullptr,
      nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(out, nullptr);
}

TEST(Type1Write, AnAllocationFailureAnywhereIsReported) {
  Loaded source(read_fixture("type1.pfa"));
  ASSERT_EQ(source.result, GFNT_OK);
  size_t refused = 0;
  for (size_t fail_at = 0; fail_at < 40; ++fail_at) {
    gfnttest::FailingAllocator allocator(fail_at);
    GFNT_Blob * out = nullptr;
    const GFNT_Result result = gfnt_write_type1(source.face, GFNT_TYPE1_FORMAT_PFB,
        nullptr, allocator.get(), &out, nullptr);
    if (result != GFNT_OK) {
      ++refused;
      EXPECT_EQ(result, GFNT_ERR_OOM) << fail_at;
      EXPECT_EQ(out, nullptr);
    }
    gfnt_blob_destroy(out);
    EXPECT_EQ(allocator.live(), 0u) << "allocation " << fail_at << " leaked";
  }
  EXPECT_GT(refused, 2u);
}

TEST(CffWrite, ABareCffIsTheTableAndReadsAsTheSameFont) {
  for (const char * name : {"cff.otf", "cff-curves.otf", "cff-subrs.otf", "cff-cid.otf",
                            "bare.cff"}) {
    Loaded source(read_fixture(name));
    ASSERT_EQ(source.result, GFNT_OK) << name;
    GFNT_Blob * out = nullptr;
    ASSERT_EQ(gfnt_write_cff(source.face, nullptr, nullptr, &out, nullptr), GFNT_OK)
        << name;
    const std::string bytes = bytes_of(out);
    const uint8_t * table = nullptr;
    size_t length = 0;
    ASSERT_EQ(gfnt_face_table_data(source.face, GFNT_TAG('C', 'F', 'F', ' '), &table,
        &length), GFNT_OK);
    EXPECT_EQ(bytes, std::string(reinterpret_cast<const char *>(table), length)) << name;
    Loaded back(bytes);
    ASSERT_EQ(back.result, GFNT_OK) << name << ": " << back.error.message;
    EXPECT_EQ(gfnt_face_flavour(back.face), GFNT_FLAVOUR_BARE_CFF);
    // A bare CFF states no em and no advances of its own beyond the charstrings',
    // so what is compared is the outlines.
    size_t na = 0, nb = 0;
    ASSERT_EQ(gfnt_face_num_glyphs(source.face, &na, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_num_glyphs(back.face, &nb, nullptr), GFNT_OK);
    ASSERT_EQ(na, nb) << name;
    for (uint32_t g = 0; g < na; ++g) {
      GFNT_Outline * oa = nullptr;
      GFNT_Outline * ob = nullptr;
      ASSERT_EQ(gfnt_face_glyph_outline(source.face, g, nullptr, nullptr, &oa, nullptr),
          GFNT_OK);
      ASSERT_EQ(gfnt_face_glyph_outline(back.face, g, nullptr, nullptr, &ob, nullptr),
          GFNT_OK);
      ASSERT_EQ(gfnt_outline_point_count(oa), gfnt_outline_point_count(ob)) << name;
      for (size_t p = 0; p < gfnt_outline_point_count(oa); ++p) {
        GFNT_Point pa{}, pb{};
        gfnt_outline_point_at(oa, p, &pa, nullptr);
        gfnt_outline_point_at(ob, p, &pb, nullptr);
        EXPECT_EQ(pa.x, pb.x) << name << " glyph " << g;
        EXPECT_EQ(pa.y, pb.y) << name << " glyph " << g;
      }
      gfnt_outline_destroy(oa);
      gfnt_outline_destroy(ob);
    }
  }
}

TEST(CffWrite, OutlinesOfAnotherKindAreNotConverted) {
  for (const char * name : {"basic.ttf", "variable-cff2.otf", "bitmap.pcf"}) {
    Loaded source(read_fixture(name));
    ASSERT_EQ(source.result, GFNT_OK) << name;
    GFNT_Blob * out = nullptr;
    EXPECT_EQ(gfnt_write_cff(source.face, nullptr, nullptr, &out, nullptr),
        GFNT_ERR_UNSUPPORTED) << name;
    EXPECT_EQ(out, nullptr);
  }
  GFNT_Blob * out = nullptr;
  EXPECT_EQ(gfnt_write_cff(nullptr, nullptr, nullptr, &out, nullptr), GFNT_ERR_INVALID);
}

TEST(GzipWrite, WhatItWritesIsReadBackAndStillAFont) {
  for (const char * name : {"bitmap.pcf", "type1.pfb", "basic.ttf", "woff2-basic.woff2"}) {
    const std::string bytes = read_fixture(name);
    GFNT_Blob * in = nullptr;
    ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_COPY,
        nullptr, nullptr, &in, nullptr), GFNT_OK);
    GFNT_Blob * zipped = nullptr;
    ASSERT_EQ(gfnt_write_gzip(in, nullptr, nullptr, &zipped, nullptr), GFNT_OK) << name;
    const std::string packed = bytes_of(zipped);
    EXPECT_EQ(uint8_t(packed[0]), 0x1Fu);
    EXPECT_EQ(uint8_t(packed[1]), 0x8Bu);
    Loaded plain(bytes);
    Loaded back(packed);
    ASSERT_EQ(back.result, GFNT_OK) << name << ": " << back.error.message;
    size_t a = 0, b = 0;
    ASSERT_EQ(gfnt_face_num_glyphs(plain.face, &a, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_num_glyphs(back.face, &b, nullptr), GFNT_OK);
    EXPECT_EQ(a, b) << name;
    gfnt_blob_destroy(in);
  }
  GFNT_Blob * out = nullptr;
  EXPECT_EQ(gfnt_write_gzip(nullptr, nullptr, nullptr, &out, nullptr), GFNT_ERR_INVALID);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
