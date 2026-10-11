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
 * Collections: a TrueType Collection and a WOFF 2 collection written from several
 * faces. The source is the `woff2-collection` fixture's two faces, which share every
 * table but `name` and `head`; each output is read back face by face and compared with
 * the face it was made from, and the sharing is checked by where the tables landed.
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/write.h>

#include <gtest/gtest.h>

namespace {

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

/** A collection's blob, and each of its faces loaded. */
struct Loaded {
  GFNT_Blob * blob = nullptr;
  std::vector<GFNT_Face *> faces;
  GFNT_Result result = GFNT_ERR_INTERNAL;
  GFNT_Error error{};

  explicit Loaded(const std::string & bytes) {
    gfnt_error_clear(&error);
    result = gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_COPY,
        nullptr, nullptr, &blob, &error);
    size_t count = 0;
    if (result == GFNT_OK) result = gfnt_face_count(blob, nullptr, &count, &error);
    for (size_t i = 0; result == GFNT_OK && i < count; ++i) {
      GFNT_Face * face = nullptr;
      result = gfnt_face_load(blob, i, nullptr, nullptr, &face, &error);
      if (result == GFNT_OK) faces.push_back(face);
    }
  }
  ~Loaded() {
    for (auto * f : faces) gfnt_face_free(f);
    gfnt_blob_destroy(blob);
  }
  Loaded(const Loaded &) = delete;
  Loaded & operator=(const Loaded &) = delete;
};

/** The write-side view of a loaded face: owned tag/length pairs into the face. */
struct Source {
  std::vector<GFNT_WriteTable> tables;
  GFNT_Tag flavour;
  explicit Source(const GFNT_Face * face) : flavour(gfnt_face_flavour(face)) {
    for (size_t i = 0; i < gfnt_face_table_count(face); ++i) {
      GFNT_Tag t = 0;
      const uint8_t * data = nullptr;
      size_t length = 0;
      EXPECT_EQ(gfnt_face_table_tag_at(face, i, &t), GFNT_OK);
      EXPECT_EQ(gfnt_face_table_data(face, t, &data, &length), GFNT_OK);
      tables.push_back({t, data, length});
    }
  }
};

std::string table_of(const GFNT_Face * face, GFNT_Tag t) {
  const uint8_t * data = nullptr;
  size_t length = 0;
  if (gfnt_face_table_data(face, t, &data, &length) != GFNT_OK) {
    return std::string("\0missing", 8);
  }
  return std::string(reinterpret_cast<const char *>(data), length);
}

constexpr GFNT_Tag tag(const char (&t)[5]) {
  return (GFNT_Tag(uint8_t(t[0])) << 24) | (GFNT_Tag(uint8_t(t[1])) << 16)
      | (GFNT_Tag(uint8_t(t[2])) << 8) | GFNT_Tag(uint8_t(t[3]));
}

void expect_same_face(const GFNT_Face * got, const GFNT_Face * want,
    const std::string & label) {
  EXPECT_EQ(gfnt_face_flavour(got), gfnt_face_flavour(want)) << label;
  ASSERT_EQ(gfnt_face_table_count(got), gfnt_face_table_count(want)) << label;
  for (size_t i = 0; i < gfnt_face_table_count(want); ++i) {
    GFNT_Tag t = 0;
    ASSERT_EQ(gfnt_face_table_tag_at(want, i, &t), GFNT_OK);
    std::string a = table_of(got, t);
    std::string b = table_of(want, t);
    if (t == tag("glyf") || t == tag("loca")) continue;  // compared as outlines
    if (t == tag("head")) {
      // The adjustment is the writer's to leave and the flags' bit 11 a WOFF 2's.
      a.erase(8, 4);
      b.erase(8, 4);
      a[12] = char(a[12] & ~0x08);
      b[12] = char(b[12] & ~0x08);
    }
    EXPECT_EQ(a, b) << label << " table " << std::hex << t;
  }
  size_t ng = 0, nw = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(got, &ng, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_num_glyphs(want, &nw, nullptr), GFNT_OK);
  ASSERT_EQ(ng, nw);
  for (uint32_t g = 0; g < nw; ++g) {
    GFNT_Outline * a = nullptr;
    GFNT_Outline * b = nullptr;
    ASSERT_EQ(gfnt_face_glyph_outline(got, g, nullptr, nullptr, &a, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_glyph_outline(want, g, nullptr, nullptr, &b, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_outline_point_count(a), gfnt_outline_point_count(b)) << label;
    for (size_t p = 0; p < gfnt_outline_point_count(a); ++p) {
      GFNT_Point pa{}, pb{};
      gfnt_outline_point_at(a, p, &pa, nullptr);
      gfnt_outline_point_at(b, p, &pb, nullptr);
      EXPECT_EQ(pa.x, pb.x) << label << " glyph " << g;
      EXPECT_EQ(pa.y, pb.y) << label << " glyph " << g;
    }
    gfnt_outline_destroy(a);
    gfnt_outline_destroy(b);
  }
}

std::string bytes_of(GFNT_Blob * blob) {
  std::string out(reinterpret_cast<const char *>(gfnt_blob_data(blob)),
      gfnt_blob_size(blob));
  gfnt_blob_destroy(blob);
  return out;
}

}  // namespace

TEST(CollectionWrite, ATtcReadsBackFaceByFaceAndSharesWhatIsShared) {
  const std::string source_bytes = read_fixture("woff2-collection.woff2");
  Loaded source(source_bytes);
  ASSERT_EQ(source.result, GFNT_OK) << source.error.message;
  ASSERT_EQ(source.faces.size(), 2u);
  Source a(source.faces[0]);
  Source b(source.faces[1]);
  const GFNT_WriteFace faces[2] = {{a.flavour, a.tables.data(), a.tables.size()},
      {b.flavour, b.tables.data(), b.tables.size()}};
  GFNT_Blob * out = nullptr;
  GFNT_Error error{};
  ASSERT_EQ(gfnt_write_collection(faces, 2, nullptr, nullptr, &out, &error), GFNT_OK)
      << error.message;
  const std::string ttc = bytes_of(out);
  EXPECT_EQ(ttc.substr(0, 4), "ttcf");
  Loaded back(ttc);
  ASSERT_EQ(back.result, GFNT_OK) << back.error.message;
  ASSERT_EQ(back.faces.size(), 2u);
  for (size_t i = 0; i < 2; ++i) {
    expect_same_face(back.faces[i], source.faces[i], "ttc face " + std::to_string(i));
    for (size_t k = 0; k < gfnt_face_table_count(back.faces[i]); ++k) {
      GFNT_Tag t = 0;
      gfnt_face_table_tag_at(back.faces[i], k, &t);
      uint32_t stored = 0, computed = 1;
      ASSERT_EQ(gfnt_face_table_checksum(back.faces[i], t, &stored, &computed), GFNT_OK);
      EXPECT_EQ(stored, computed) << "face " << i << " table " << std::hex << t;
    }
  }
  // glyf is shared: the same bytes at the same offset from both faces. name is not.
  size_t off0 = 0, off1 = 1, noff0 = 0, noff1 = 0;
  ASSERT_EQ(gfnt_face_table_range(back.faces[0], tag("glyf"), &off0, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_table_range(back.faces[1], tag("glyf"), &off1, nullptr), GFNT_OK);
  EXPECT_EQ(off0, off1);
  ASSERT_EQ(gfnt_face_table_range(back.faces[0], tag("name"), &noff0, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_table_range(back.faces[1], tag("name"), &noff1, nullptr), GFNT_OK);
  EXPECT_NE(noff0, noff1);
  // Sharing makes it smaller than two separate fonts.
  size_t separate = 0;
  for (const Source * s : {&a, &b}) {
    GFNT_Blob * one = nullptr;
    ASSERT_EQ(gfnt_write_sfnt(s->flavour, s->tables.data(), s->tables.size(), nullptr,
        nullptr, &one, nullptr), GFNT_OK);
    separate += gfnt_blob_size(one);
    gfnt_blob_destroy(one);
  }
  EXPECT_LT(ttc.size(), separate);
}

TEST(CollectionWrite, AWoff2CollectionReadsBackAndIsTransformed) {
  Loaded source(read_fixture("woff2-collection.woff2"));
  ASSERT_EQ(source.result, GFNT_OK);
  Source a(source.faces[0]);
  Source b(source.faces[1]);
  const GFNT_WriteFace faces[2] = {{a.flavour, a.tables.data(), a.tables.size()},
      {b.flavour, b.tables.data(), b.tables.size()}};
  GFNT_Blob * out = nullptr;
  GFNT_Error error{};
  ASSERT_EQ(gfnt_write_woff2_collection(faces, 2, nullptr, nullptr, &out, &error),
      GFNT_OK) << error.message;
  const std::string woff2 = bytes_of(out);
  ASSERT_EQ(woff2.substr(0, 4), "wOF2");
  EXPECT_EQ(woff2.substr(4, 4), "ttcf");
  Loaded back(woff2);
  ASSERT_EQ(back.result, GFNT_OK) << back.error.message;
  ASSERT_EQ(back.faces.size(), 2u);
  for (size_t i = 0; i < 2; ++i) {
    expect_same_face(back.faces[i], source.faces[i], "woff2 face " + std::to_string(i));
  }
  EXPECT_EQ(table_of(back.faces[0], tag("glyf")), table_of(back.faces[1], tag("glyf")));
  // Smaller than the TTC of the same faces, because glyf is transformed and the
  // stream is compressed.
  GFNT_Blob * ttc = nullptr;
  ASSERT_EQ(gfnt_write_collection(faces, 2, nullptr, nullptr, &ttc, nullptr), GFNT_OK);
  EXPECT_LT(woff2.size(), gfnt_blob_size(ttc));
  gfnt_blob_destroy(ttc);
}

TEST(CollectionWrite, AGlyfSharedWithDifferentLocasIsStoredNotTransformed) {
  Loaded source(read_fixture("woff2-collection.woff2"));
  ASSERT_EQ(source.result, GFNT_OK);
  Source a(source.faces[0]);
  Source b(source.faces[1]);
  // Face B gets a loca of its own: same glyf, different loca (a padded copy).
  std::string loca = table_of(source.faces[1], tag("loca"));
  loca.append(4, '\0');
  for (auto & t : b.tables) {
    if (t.tag == tag("loca")) {
      t.data = reinterpret_cast<const uint8_t *>(loca.data());
      t.length = loca.size();
    }
  }
  const GFNT_WriteFace faces[2] = {{a.flavour, a.tables.data(), a.tables.size()},
      {b.flavour, b.tables.data(), b.tables.size()}};
  GFNT_Blob * out = nullptr;
  ASSERT_EQ(gfnt_write_woff2_collection(faces, 2, nullptr, nullptr, &out, nullptr),
      GFNT_OK);
  Loaded back(bytes_of(out));
  ASSERT_EQ(back.result, GFNT_OK) << back.error.message;
  ASSERT_EQ(back.faces.size(), 2u);
  EXPECT_EQ(table_of(back.faces[0], tag("loca")), table_of(source.faces[0], tag("loca")));
  EXPECT_EQ(table_of(back.faces[1], tag("loca")), loca) << "kept as given";
  EXPECT_EQ(table_of(back.faces[0], tag("glyf")), table_of(source.faces[0], tag("glyf")))
      << "stored, so the bytes are the source's";
}

TEST(CollectionWrite, OneFaceCollectionAndMixedFlavours) {
  Loaded ttf(read_fixture("basic.ttf"));
  Loaded otf(read_fixture("cff.otf"));
  ASSERT_EQ(ttf.result, GFNT_OK);
  ASSERT_EQ(otf.result, GFNT_OK);
  Source a(ttf.faces[0]);
  Source b(otf.faces[0]);
  const GFNT_WriteFace faces[2] = {{a.flavour, a.tables.data(), a.tables.size()},
      {b.flavour, b.tables.data(), b.tables.size()}};
  for (bool woff2 : {false, true}) {
    GFNT_Blob * out = nullptr;
    ASSERT_EQ(woff2 ? gfnt_write_woff2_collection(faces, 2, nullptr, nullptr, &out, nullptr)
                    : gfnt_write_collection(faces, 2, nullptr, nullptr, &out, nullptr),
        GFNT_OK);
    Loaded back(bytes_of(out));
    ASSERT_EQ(back.result, GFNT_OK) << back.error.message;
    ASSERT_EQ(back.faces.size(), 2u);
    expect_same_face(back.faces[0], ttf.faces[0], "basic.ttf");
    EXPECT_EQ(gfnt_face_flavour(back.faces[1]), gfnt_face_flavour(otf.faces[0]));
    EXPECT_EQ(table_of(back.faces[1], tag("CFF ")), table_of(otf.faces[0], tag("CFF ")));
  }
  GFNT_Blob * one = nullptr;
  ASSERT_EQ(gfnt_write_woff2_collection(faces, 1, nullptr, nullptr, &one, nullptr), GFNT_OK);
  Loaded back(bytes_of(one));
  ASSERT_EQ(back.result, GFNT_OK);
  EXPECT_EQ(back.faces.size(), 1u);
}

TEST(CollectionWrite, RefusesWhatItCannotWrite) {
  Loaded ttf(read_fixture("basic.ttf"));
  ASSERT_EQ(ttf.result, GFNT_OK);
  Source a(ttf.faces[0]);
  const GFNT_WriteFace face = {a.flavour, a.tables.data(), a.tables.size()};
  GFNT_Blob * out = nullptr;
  for (bool woff2 : {false, true}) {
    auto write = [&](const GFNT_WriteFace * f, size_t n, const GFNT_Limits * limits) {
      return woff2 ? gfnt_write_woff2_collection(f, n, limits, nullptr, &out, nullptr)
                   : gfnt_write_collection(f, n, limits, nullptr, &out, nullptr);
    };
    EXPECT_EQ(write(&face, 0, nullptr), GFNT_ERR_INVALID) << "no faces";
    EXPECT_EQ(write(nullptr, 1, nullptr), GFNT_ERR_INVALID);
    const GFNT_WriteFace empty = {a.flavour, a.tables.data(), 0};
    EXPECT_EQ(write(&empty, 1, nullptr), GFNT_ERR_INVALID) << "a face with no tables";
    auto doubled = a.tables;
    doubled.push_back(doubled[0]);
    const GFNT_WriteFace twice = {a.flavour, doubled.data(), doubled.size()};
    EXPECT_EQ(write(&twice, 1, nullptr), GFNT_ERR_INVALID) << "a repeated tag";
    GFNT_Limits limits;
    gfnt_limits_default(&limits);
    limits.max_blob_bytes = 100;
    EXPECT_EQ(write(&face, 1, &limits), GFNT_ERR_LIMIT);
    gfnt_limits_default(&limits);
    limits.max_tables = 2;
    EXPECT_EQ(write(&face, 1, &limits), GFNT_ERR_LIMIT);
  }
  EXPECT_EQ(gfnt_write_collection(&face, 1, nullptr, nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(out, nullptr);
}

TEST(CollectionWrite, AnAllocationFailureAnywhereIsReported) {
  Loaded source(read_fixture("woff2-collection.woff2"));
  ASSERT_EQ(source.result, GFNT_OK);
  Source a(source.faces[0]);
  Source b(source.faces[1]);
  const GFNT_WriteFace faces[2] = {{a.flavour, a.tables.data(), a.tables.size()},
      {b.flavour, b.tables.data(), b.tables.size()}};
  for (bool woff2 : {false, true}) {
    size_t refused = 0;
    for (size_t fail_at = 0; fail_at < 100; ++fail_at) {
      gfnttest::FailingAllocator allocator(fail_at);
      GFNT_Blob * out = nullptr;
      const GFNT_Result result = woff2
          ? gfnt_write_woff2_collection(faces, 2, nullptr, allocator.get(), &out, nullptr)
          : gfnt_write_collection(faces, 2, nullptr, allocator.get(), &out, nullptr);
      if (result != GFNT_OK) {
        ++refused;
        EXPECT_EQ(result, GFNT_ERR_OOM) << fail_at;
        EXPECT_EQ(out, nullptr);
      }
      gfnt_blob_destroy(out);
      EXPECT_EQ(allocator.live(), 0u) << "allocation " << fail_at << " leaked";
    }
    EXPECT_GT(refused, 5u);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
