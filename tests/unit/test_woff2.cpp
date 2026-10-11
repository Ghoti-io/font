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
 * WOFF 2: the sfnt it wraps is rebuilt, `glyf`, `loca` and `hmtx` included.
 *
 * Positive cases are fixtures. Three come from fontTools and one from this
 * repository's own WOFF 2 writer, which exists because fontTools writes only some
 * of the format: all 128 triplet classes, every spelling of 255UInt16, explicit
 * boxes, an overlap bitmap, a composite with instructions, and a collection (see
 * `tools/fixtures/woff2_fixtures.py`). Each is read back here and compared with the
 * font it was made from, glyph by glyph.
 *
 * The damaged cases are built here, by a small writer of the same format whose
 * Brotli stream is the uncompressed kind, so each refusal is one the format's own
 * rules make: a valid file first (the control), then one rule broken at a time.
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/write.h>

#include "../../src/blob/woff2.h"

#include <gtest/gtest.h>

namespace {

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

constexpr uint32_t tag(const char (&t)[5]) {
  return (uint32_t(uint8_t(t[0])) << 24) | (uint32_t(uint8_t(t[1])) << 16)
      | (uint32_t(uint8_t(t[2])) << 8) | uint32_t(uint8_t(t[3]));
}

std::string table_of(const GFNT_Face * face, uint32_t t) {
  const uint8_t * data = nullptr;
  size_t length = 0;
  if (gfnt_face_table_data(face, t, &data, &length) != GFNT_OK) {
    return std::string("\0missing", 8);
  }
  return std::string(reinterpret_cast<const char *>(data), length);
}

std::vector<uint32_t> tags_of(const GFNT_Face * face) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < gfnt_face_table_count(face); ++i) {
    GFNT_Tag t = 0;
    EXPECT_EQ(gfnt_face_table_tag_at(face, i, &t), GFNT_OK);
    out.push_back(t);
  }
  return out;
}


uint32_t be16(const std::string & s, size_t at);

/** The instructions of a glyph and whether its first flag says OVERLAP_SIMPLE. */
struct GlyphExtras {
  std::string instructions;
  bool overlap = false;
};

std::string glyph_bytes(const GFNT_Face * face, uint32_t glyph);

GlyphExtras extras_of(const GFNT_Face * face, uint32_t glyph) {
  GlyphExtras out;
  const std::string g = glyph_bytes(face, glyph);
  if (g.size() < 10) return out;
  const int contours = int16_t(be16(g, 0));
  if (contours > 0) {
    const size_t at = 10 + 2 * size_t(contours);
    const size_t n = be16(g, at);
    out.instructions = g.substr(at + 2, n);
    out.overlap = (uint8_t(g[at + 2 + n]) & 0x40) != 0;
  } else if (contours < 0) {
    size_t at = 10;
    bool more = true, instructions = false;
    while (more && at + 4 <= g.size()) {
      const uint32_t flags = be16(g, at);
      at += 4 + ((flags & 1) ? 4 : 2);
      at += (flags & 0x08) ? 2 : (flags & 0x40) ? 4 : (flags & 0x80) ? 8 : 0;
      more = (flags & 0x20) != 0;
      instructions |= (flags & 0x100) != 0;
    }
    if (instructions && at + 2 <= g.size()) {
      out.instructions = g.substr(at + 2, be16(g, at));
    }
  }
  return out;
}

/** Every outline, stated box and composite flag the same in both faces. */
void expect_same_glyphs(const GFNT_Face * a, const GFNT_Face * b,
    const std::string & label) {
  size_t na = 0, nb = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(a, &na, nullptr), GFNT_OK) << label;
  ASSERT_EQ(gfnt_face_num_glyphs(b, &nb, nullptr), GFNT_OK) << label;
  ASSERT_EQ(na, nb) << label;
  for (uint32_t g = 0; g < na; ++g) {
    GFNT_Outline * oa = nullptr;
    GFNT_Outline * ob = nullptr;
    ASSERT_EQ(gfnt_face_glyph_outline(a, g, nullptr, nullptr, &oa, nullptr),
        GFNT_OK) << label << " glyph " << g;
    ASSERT_EQ(gfnt_face_glyph_outline(b, g, nullptr, nullptr, &ob, nullptr),
        GFNT_OK) << label << " glyph " << g;
    ASSERT_EQ(gfnt_outline_point_count(oa), gfnt_outline_point_count(ob))
        << label << " glyph " << g;
    ASSERT_EQ(gfnt_outline_contour_count(oa), gfnt_outline_contour_count(ob))
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
    if (gfnt_face_has_table(a, tag("glyf")) && gfnt_face_has_table(b, tag("glyf"))) {
      // What an outline does not show: the instructions, and the overlap flag.
      const GlyphExtras ea = extras_of(a, g);
      const GlyphExtras eb = extras_of(b, g);
      EXPECT_EQ(ea.instructions, eb.instructions) << label << " glyph " << g;
      EXPECT_EQ(ea.overlap, eb.overlap) << label << " glyph " << g;
    }

    GFNT_Box ba{}, bb{};
    const GFNT_Result ra = gfnt_face_glyph_stated_box(a, g, &ba, nullptr);
    const GFNT_Result rb = gfnt_face_glyph_stated_box(b, g, &bb, nullptr);
    ASSERT_EQ(ra, rb) << label << " glyph " << g;
    if (ra == GFNT_ERR_UNSUPPORTED) {
      continue;  // a CFF font has no glyf box or composite flag to compare
    }
    ASSERT_EQ(ra, GFNT_OK);
    EXPECT_EQ(ba.x_min, bb.x_min) << label << " glyph " << g;
    EXPECT_EQ(ba.y_min, bb.y_min) << label << " glyph " << g;
    EXPECT_EQ(ba.x_max, bb.x_max) << label << " glyph " << g;
    EXPECT_EQ(ba.y_max, bb.y_max) << label << " glyph " << g;
    bool ca = false, cb = true;
    ASSERT_EQ(gfnt_face_glyph_is_composite(a, g, &ca, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_glyph_is_composite(b, g, &cb, nullptr), GFNT_OK);
    EXPECT_EQ(ca, cb) << label << " glyph " << g;
  }
}

/** The rebuilt font is the source font: tables, glyphs, and honest checksums. */
void expect_same_font(const GFNT_Face * woff2, const GFNT_Face * source,
    const std::string & label) {
  EXPECT_EQ(gfnt_face_flavour(woff2), gfnt_face_flavour(source)) << label;
  EXPECT_EQ(tags_of(woff2), tags_of(source)) << label;
  for (uint32_t t : tags_of(source)) {
    std::string a = table_of(woff2, t);
    std::string b = table_of(source, t);
    if (t == tag("glyf") || t == tag("loca")) {
      // The same glyphs, laid out the decoder's way; compared as outlines below.
      continue;
    }
    if (t == tag("head")) {
      // Not the adjustment, which is recomputed for the rebuilt file, nor bit 11 of
      // the flags, which a WOFF 2 writer sets to say the font was transformed.
      ASSERT_EQ(a.size(), b.size()) << label;
      a.erase(8, 4);
      b.erase(8, 4);
      a[12] = char(a[12] & ~0x08);
      b[12] = char(b[12] & ~0x08);
    }
    EXPECT_EQ(a, b) << label << " table " << std::hex << t;
  }
  for (uint32_t t : tags_of(woff2)) {
    uint32_t stored = 0, computed = 1;
    ASSERT_EQ(gfnt_face_table_checksum(woff2, t, &stored, &computed), GFNT_OK);
    EXPECT_EQ(stored, computed) << label << " checksum of " << std::hex << t;
  }
  expect_same_glyphs(woff2, source, label);
}

/** The sum of a whole file's words, which `checkSumAdjustment` makes 0xB1B0AFBA. */
uint32_t file_sum(const uint8_t * data, size_t size) {
  uint32_t sum = 0;
  for (size_t i = 0; i < size; i += 4) {
    uint32_t word = 0;
    for (size_t k = 0; k < 4; ++k) {
      word = (word << 8) | (i + k < size ? data[i + k] : 0);
    }
    sum += word;
  }
  return sum;
}

uint32_t be16(const std::string & s, size_t at) {
  return (uint32_t(uint8_t(s[at])) << 8) | uint8_t(s[at + 1]);
}

uint32_t be32(const std::string & s, size_t at) {
  return (be16(s, at) << 16) | be16(s, at + 2);
}

/** Glyph @p glyph's bytes in a rebuilt `glyf`, by way of its `loca`. */
std::string glyph_bytes(const GFNT_Face * face, uint32_t glyph) {
  const std::string head = table_of(face, tag("head"));
  const std::string loca = table_of(face, tag("loca"));
  const std::string glyf = table_of(face, tag("glyf"));
  const bool long_format = be16(head, 50) == 1;
  size_t from = long_format ? be32(loca, 4 * glyph) : 2 * be16(loca, 2 * glyph);
  size_t to = long_format ? be32(loca, 4 * glyph + 4)
                          : 2 * be16(loca, 2 * glyph + 2);
  return glyf.substr(from, to - from);
}

}  // namespace

TEST(Woff2, EveryFixtureIsTheFontItWasMadeFrom) {
  const std::string basic = read_fixture("basic.ttf");
  const std::string triplets_source = read_fixture("woff2-triplets-source.ttf");
  ASSERT_FALSE(basic.empty());
  ASSERT_FALSE(triplets_source.empty());
  Crafted plain(basic);
  Crafted triplets_plain(triplets_source);
  ASSERT_EQ(plain.result, GFNT_OK) << plain.error.message;
  ASSERT_EQ(triplets_plain.result, GFNT_OK) << triplets_plain.error.message;

  for (const char * name : {"woff2-basic.woff2", "woff2-hmtx.woff2",
                            "woff2-stored.woff2"}) {
    const std::string bytes = read_fixture(name);
    ASSERT_FALSE(bytes.empty()) << name;
    Crafted woff2(bytes);
    ASSERT_EQ(woff2.result, GFNT_OK) << name << ": " << woff2.error.message;
    expect_same_font(woff2.face, plain.face, name);
  }
  const std::string bytes = read_fixture("woff2-triplets.woff2");
  ASSERT_FALSE(bytes.empty());
  Crafted woff2(bytes);
  ASSERT_EQ(woff2.result, GFNT_OK) << woff2.error.message;
  expect_same_font(woff2.face, triplets_plain.face, "woff2-triplets.woff2");
}

TEST(Woff2, TheFileSumsToTheMagicNumberOnceItsAdjustmentIsSet) {
  for (const char * name : {"woff2-basic.woff2", "woff2-hmtx.woff2",
                            "woff2-stored.woff2", "woff2-triplets.woff2"}) {
    const std::string bytes = read_fixture(name);
    GFNT_Blob * in = nullptr;
    GFNT_Blob * out = nullptr;
    GFNT_Limits limits;
    gfnt_limits_default(&limits);
    ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_COPY,
        nullptr, nullptr, &in, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_woff2_to_sfnt(in, &limits, nullptr, &out, nullptr), GFNT_OK)
        << name;
    EXPECT_EQ(file_sum(gfnt_blob_data(out), gfnt_blob_size(out)), 0xB1B0AFBAu)
        << name;
    gfnt_blob_destroy(out);
    gfnt_blob_destroy(in);
  }
}

TEST(Woff2, EveryTripletClassDecodes) {
  // 128 points, one per class, are in the glyph twice: on the curve and off it.
  // The comparison with the source font already covers where they land; this
  // says the fixture is the one it claims to be.
  const std::string bytes = read_fixture("woff2-triplets.woff2");
  Crafted woff2(bytes);
  ASSERT_EQ(woff2.result, GFNT_OK) << woff2.error.message;
  for (uint32_t glyph : {1u, 2u}) {
    GFNT_Outline * outline = nullptr;
    ASSERT_EQ(gfnt_face_glyph_outline(woff2.face, glyph, nullptr, nullptr, &outline,
        nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_outline_point_count(outline), 128u);
    for (size_t p = 0; p < 128; ++p) {
      GFNT_PointTag point_tag{};
      ASSERT_EQ(gfnt_outline_point_at(outline, p, nullptr, &point_tag), GFNT_OK);
      EXPECT_EQ(point_tag == GFNT_POINT_ON, glyph == 1) << "glyph " << glyph;
    }
    gfnt_outline_destroy(outline);
  }
}

TEST(Woff2, EverySpellingOf255UInt16Decodes) {
  const std::string bytes = read_fixture("woff2-triplets.woff2");
  Crafted woff2(bytes);
  ASSERT_EQ(woff2.result, GFNT_OK) << woff2.error.message;
  // Contours of 1, 252, 300, 600 and 800 points take a byte, 253 and a byte, 506
  // and a byte, and 253 and a 16-bit value in the point-count stream.
  GFNT_Outline * outline = nullptr;
  ASSERT_EQ(gfnt_face_glyph_outline(woff2.face, 6, nullptr, nullptr, &outline,
      nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_outline_contour_count(outline), 5u);
  const size_t expect[5] = {1, 252, 300, 600, 800};
  for (size_t c = 0; c < 5; ++c) {
    size_t count = 0;
    ASSERT_EQ(gfnt_outline_contour_at(outline, c, nullptr, &count), GFNT_OK);
    EXPECT_EQ(count, expect[c]) << "contour " << c;
  }
  gfnt_outline_destroy(outline);
  // The same forms for instruction lengths: 300, 600 and 800 bytes.
  const uint32_t lengths[3] = {300, 600, 800};
  for (uint32_t i = 0; i < 3; ++i) {
    const std::string glyph = glyph_bytes(woff2.face, 7 + i);
    ASSERT_GT(glyph.size(), 12u);
    EXPECT_EQ(be16(glyph, 10 + 2), lengths[i]) << "instruction length " << i;
  }
}

TEST(Woff2, TheOverlapBitmapSetsTheFlagOnItsGlyphOnly) {
  const std::string bytes = read_fixture("woff2-triplets.woff2");
  Crafted woff2(bytes);
  ASSERT_EQ(woff2.result, GFNT_OK) << woff2.error.message;
  // Glyph 3 ('box') is in the bitmap; glyph 1 is not. The first flag of a simple
  // glyph sits after the header, the end points and the instructions.
  auto first_flag = [&](uint32_t glyph) {
    const std::string g = glyph_bytes(woff2.face, glyph);
    const size_t contours = be16(g, 0);
    const size_t instructions = be16(g, 10 + 2 * contours);
    return uint8_t(g[10 + 2 * contours + 2 + instructions]);
  };
  EXPECT_NE(first_flag(3) & 0x40u, 0u);
  EXPECT_EQ(first_flag(1) & 0x40u, 0u);
  EXPECT_EQ(first_flag(4) & 0x40u, 0u);
}

TEST(Woff2, ACollectionComesOutAsACollection) {
  const std::string bytes = read_fixture("woff2-collection.woff2");
  const std::string basic = read_fixture("basic.ttf");
  ASSERT_FALSE(bytes.empty());
  GFNT_Blob * blob = nullptr;
  ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_COPY,
      nullptr, nullptr, &blob, nullptr), GFNT_OK);
  size_t faces = 0;
  EXPECT_EQ(gfnt_face_count(blob, nullptr, &faces, nullptr), GFNT_OK);
  EXPECT_EQ(faces, 2u);
  GFNT_Face * too_far = nullptr;
  EXPECT_EQ(gfnt_face_load(blob, 2, nullptr, nullptr, &too_far, nullptr),
      GFNT_ERR_INVALID);
  gfnt_blob_destroy(blob);

  Crafted plain(basic);
  ASSERT_EQ(plain.result, GFNT_OK);
  Crafted a(bytes, nullptr, 0);
  Crafted b(bytes, nullptr, 1);
  ASSERT_EQ(a.result, GFNT_OK) << a.error.message;
  ASSERT_EQ(b.result, GFNT_OK) << b.error.message;
  EXPECT_EQ(gfnt_face_index(a.face), 0u);
  EXPECT_EQ(gfnt_face_index(b.face), 1u);
  EXPECT_NE(table_of(a.face, tag("name")), table_of(b.face, tag("name")));
  EXPECT_EQ(table_of(a.face, tag("glyf")), table_of(b.face, tag("glyf")))
      << "the shared table is stored once";
  expect_same_glyphs(a.face, plain.face, "face 0");
  expect_same_glyphs(b.face, plain.face, "face 1");
  for (const GFNT_Face * face : {a.face, b.face}) {
    for (uint32_t t : tags_of(face)) {
      uint32_t stored = 0, computed = 1;
      ASSERT_EQ(gfnt_face_table_checksum(face, t, &stored, &computed), GFNT_OK);
      EXPECT_EQ(stored, computed) << std::hex << t;
    }
  }
}

TEST(Woff2, OneFaceAndOnlyFaceZero) {
  const std::string bytes = read_fixture("woff2-basic.woff2");
  GFNT_Blob * blob = nullptr;
  ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_COPY,
      nullptr, nullptr, &blob, nullptr), GFNT_OK);
  size_t faces = 0;
  EXPECT_EQ(gfnt_face_count(blob, nullptr, &faces, nullptr), GFNT_OK);
  EXPECT_EQ(faces, 1u);
  GFNT_Face * face = nullptr;
  EXPECT_EQ(gfnt_face_load(blob, 1, nullptr, nullptr, &face, nullptr),
      GFNT_ERR_INVALID);
  gfnt_blob_destroy(blob);
}

// ---------------------------------------------------------------------------
// A WOFF 2 writer for the damaged cases.

namespace {

std::string base128(uint32_t v) {
  std::string out(1, char(v & 0x7F));
  v >>= 7;
  while (v) {
    out.insert(out.begin(), char(0x80 | (v & 0x7F)));
    v >>= 7;
  }
  return out;
}

std::string u255(uint32_t v) {
  if (v < 253) return std::string(1, char(v));
  if (v < 506) return std::string{char(255), char(v - 253)};
  if (v < 762) return std::string{char(254), char(v - 506)};
  return std::string{char(253), char(v >> 8), char(v & 0xFF)};
}

std::string be(uint32_t v, int bytes) {
  std::string out;
  for (int i = bytes - 1; i >= 0; --i) out += char((v >> (8 * i)) & 0xFF);
  return out;
}

struct Entry {
  uint32_t tag;
  std::string stream;        // what the decompressed stream holds
  uint32_t orig = 0;         // the stated original length
  int version = 0;
  bool transformed = false;
  Entry(uint32_t t, std::string s, uint32_t o, int v = 0, bool x = false)
      : tag(t), stream(std::move(s)), orig(o), version(v), transformed(x) {}
};

int known_index(uint32_t t) {
  struct { uint32_t tag; int index; } known[] = {
    {tag("cmap"), 0}, {tag("head"), 1}, {tag("hhea"), 2}, {tag("hmtx"), 3},
    {tag("maxp"), 4}, {tag("glyf"), 10}, {tag("loca"), 11}};
  for (auto & k : known) if (k.tag == t) return k.index;
  return 63;
}

std::string brotli_of(const std::string & data) {
  void * out = nullptr;
  size_t size = 0;
  std::string result;
  if (gcomp_encode_alloc(nullptr, "brotli", nullptr, data.data(), data.size(),
          &out, &size) != GCOMP_OK) {
    return result;
  }
  result.assign(static_cast<const char *>(out), size);
  gcomp_buffer_free(nullptr, out);
  return result;
}

struct Woff2Spec {
  uint32_t flavor = 0x00010000;
  std::vector<Entry> tables;
  std::string collection;    // the collection directory, when there is one
  std::string raw_directory; // replaces the generated directory, when set
};

std::string make_woff2(const Woff2Spec & spec) {
  std::string directory;
  std::string stream;
  for (const Entry & e : spec.tables) {
    const int index = known_index(e.tag);
    directory += char(index | (e.version << 6));
    if (index == 63) directory += be(e.tag, 4);
    directory += base128(e.orig);
    if (e.transformed) directory += base128(uint32_t(e.stream.size()));
    stream += e.stream;
  }
  if (!spec.raw_directory.empty()) directory = spec.raw_directory;
  directory += spec.collection;
  const std::string packed = brotli_of(stream);
  const uint32_t total = uint32_t(48 + directory.size() + packed.size());
  std::string out = "wOF2";
  out += be(spec.flavor, 4) + be(total, 4) + be(uint32_t(spec.tables.size()), 2)
      + be(0, 2) + be(0, 4) + be(uint32_t(packed.size()), 4) + be(1, 2) + be(0, 2)
      + be(0, 4) + be(0, 4) + be(0, 4) + be(0, 4) + be(0, 4);
  return out + directory + packed;
}

/** The pieces of a transformed `glyf`, each of which a test can spoil. */
struct GlyfParts {
  uint32_t options = 0;
  uint32_t glyphs = 2;
  uint32_t index_format = 0;
  std::string ncontour = be(0, 2) + be(1, 2);      // glyph 0 empty, glyph 1 simple
  std::string npoints = u255(3);
  std::string flags = std::string(3, char(0x7F));  // class 127: 16 bits each, both +
  std::string glyph = be(10, 2) + be(20, 2) + be(30, 2) + be(40, 2)
      + be(50, 2) + be(60, 2) + u255(0);
  std::string composite;
  std::string bbox = be(0, 4);                     // the bitmap: no explicit boxes
  std::string instructions;
  std::string overlap;
  uint32_t header_size_delta = 0;                  // makes a stream size lie
  int lying_stream = -1;

  std::string build() const {
    const std::string * streams[7] = {&ncontour, &npoints, &flags, &glyph,
        &composite, &bbox, &instructions};
    std::string out = be(0, 2) + be(options, 2) + be(glyphs, 2)
        + be(index_format, 2);
    for (int i = 0; i < 7; ++i) {
      out += be(uint32_t(streams[i]->size()) + (i == lying_stream ? header_size_delta : 0),
          4);
    }
    for (const auto * s : streams) out += *s;
    return out + overlap;
  }
};

std::string zeros(size_t n) { return std::string(n, '\0'); }

/** A control font: six tables, the `glyf` and `loca` transformed. */
Woff2Spec valid_spec(const GlyfParts & parts = GlyfParts(),
    uint32_t loca_length = 6) {
  Woff2Spec spec;
  const std::string glyf = parts.build();
  spec.tables.emplace_back(tag("glyf"), glyf, 0, 0, true);
  spec.tables.emplace_back(tag("loca"), "", loca_length, 0, true);
  spec.tables.emplace_back(tag("head"), zeros(54), 54);
  spec.tables.emplace_back(tag("hhea"), zeros(36), 36);
  spec.tables.emplace_back(tag("maxp"), zeros(6), 6);
  return spec;
}

GFNT_Result load_result(const std::string & bytes,
    const GFNT_Limits * limits = nullptr, size_t index = 0) {
  Crafted face(bytes, limits, index);
  return face.result;
}

}  // namespace

TEST(Woff2, TheControlLoadsSoEveryRefusalBelowIsTheRuleBroken) {
  Crafted face(make_woff2(valid_spec()));
  ASSERT_EQ(face.result, GFNT_OK) << face.error.message;
  EXPECT_EQ(table_of(face.face, tag("loca")).size(), 6u);
  // Glyph 1: 10 bytes of header, 2 of end point, 2 of instruction length, 3 flags,
  // and 3 points of 1-byte x and 1-byte y once recoded, padded to four.
  EXPECT_GT(table_of(face.face, tag("glyf")).size(), 20u);
}

TEST(Woff2, TheHeaderIsHeldToTheFormat) {
  const std::string good = make_woff2(valid_spec());
  auto with = [&](size_t at, const std::string & value) {
    std::string bytes = good;
    bytes.replace(at, value.size(), value);
    return bytes;
  };
  EXPECT_EQ(load_result(with(14, be(1, 2))), GFNT_ERR_CORRUPT) << "reserved";
  EXPECT_EQ(load_result(with(8, be(uint32_t(good.size() + 1), 4))), GFNT_ERR_CORRUPT)
      << "longer than the file";
  EXPECT_EQ(load_result(with(12, be(0, 2))), GFNT_ERR_CORRUPT) << "no tables";
  EXPECT_EQ(load_result(good.substr(0, 40)), GFNT_ERR_CORRUPT) << "short header";
  EXPECT_NE(load_result(with(12, be(0xFFFF, 2))), GFNT_OK) << "too many tables";
  EXPECT_EQ(load_result(with(20, be(uint32_t(good.size()), 4))), GFNT_ERR_CORRUPT)
      << "compressed tables past the file";
  EXPECT_EQ(load_result(with(28, be(40, 4)) ), GFNT_ERR_CORRUPT)
      << "metadata offset with no length, inside the tables";
}

TEST(Woff2, TheDirectoryIsHeldToTheFormat) {
  auto raw = [&](const std::string & directory) {
    Woff2Spec spec = valid_spec();
    spec.raw_directory = directory;
    return make_woff2(spec);
  };
  // Entries of the control, spelled out: glyf (10, transformed), loca (11,
  // transformed, length 6), head, hhea, maxp.
  const std::string tail = std::string(1, char(11)) + base128(6) + base128(0)
      + char(1) + base128(54) + char(2) + base128(36) + char(4) + base128(6);
  {
    // UIntBase128 with a leading zero (0x80 first).
    const std::string bad = std::string(1, char(10)) + std::string("\x80\x01", 2)
        + base128(0) + tail;
    EXPECT_EQ(load_result(raw(bad)), GFNT_ERR_CORRUPT) << "leading zero";
  }
  {
    // Six bytes of continuation.
    const std::string bad = std::string(1, char(10))
        + std::string("\x81\x80\x80\x80\x80\x00", 6) + base128(0) + tail;
    EXPECT_EQ(load_result(raw(bad)), GFNT_ERR_CORRUPT) << "longer than five bytes";
  }
  {
    // Larger than 32 bits.
    const std::string bad = std::string(1, char(10))
        + std::string("\xFF\xFF\xFF\xFF\x7F", 5) + base128(0) + tail;
    EXPECT_NE(load_result(raw(bad)), GFNT_OK) << "overflow";
  }
  {
    // A glyf with transform version 1, which the format lacks.
    const std::string bad = std::string(1, char(10 | (1 << 6))) + base128(0)
        + base128(0) + tail;
    EXPECT_EQ(load_result(raw(bad)), GFNT_ERR_CORRUPT) << "glyf version 1";
  }
  {
    // Any other table with a transform version.
    Woff2Spec spec = valid_spec();
    spec.tables[2].version = 1;
    EXPECT_EQ(load_result(make_woff2(spec)), GFNT_ERR_CORRUPT) << "head version 1";
  }
  {
    // A directory cut short.
    const std::string cut = std::string(1, char(10));
    EXPECT_EQ(load_result(raw(cut)), GFNT_ERR_CORRUPT) << "cut directory";
  }
}

TEST(Woff2, TheCompressedStreamIsHeldToTheDirectory) {
  {
    // The directory states a table longer than the stream holds.
    Woff2Spec spec = valid_spec();
    spec.tables[2].orig += 1;
    EXPECT_EQ(load_result(make_woff2(spec)), GFNT_ERR_CORRUPT) << "stream short";
  }
  {
    // ... and shorter.
    Woff2Spec spec = valid_spec();
    spec.tables[2].orig -= 1;
    EXPECT_EQ(load_result(make_woff2(spec)), GFNT_ERR_CORRUPT) << "stream long";
  }
  {
    // Damage inside the Brotli stream: the uncompressed meta-block header.
    std::string bytes = make_woff2(valid_spec());
    for (size_t k = bytes.size() - 12; k < bytes.size(); ++k) {
      bytes[k] = char(bytes[k] ^ 0xA5);
    }
    EXPECT_EQ(load_result(bytes), GFNT_ERR_CORRUPT) << "garbled Brotli";
  }
}

TEST(Woff2, ATransformedGlyfIsHeldToTheFormat) {
  struct Case {
    const char * what;
    std::function<void(GlyfParts &)> spoil;
    GFNT_Result expect;
  };
  const Case cases[] = {
    {"more glyphs than contour counts", [](GlyfParts & p) { p.glyphs = 3; },
        GFNT_ERR_CORRUPT},
    {"index format 2", [](GlyfParts & p) { p.index_format = 2; }, GFNT_ERR_CORRUPT},
    {"a stream larger than the table",
        [](GlyfParts & p) { p.lying_stream = 3; p.header_size_delta = 100; },
        GFNT_ERR_CORRUPT},
    {"a contour count of -2", [](GlyfParts & p) { p.ncontour = be(0, 2) + be(0xFFFE, 2); },
        GFNT_ERR_CORRUPT},
    {"an empty glyph with a stated box",
        [](GlyfParts & p) { p.bbox = std::string("\x80\0\0\0", 4); },
        GFNT_ERR_CORRUPT},
    {"a composite glyph with no stated box",
        [](GlyfParts & p) { p.ncontour = be(0, 2) + be(0xFFFF, 2);
                            p.composite = be(0x0000, 2) + be(0, 2) + be(0, 2); },
        GFNT_ERR_CORRUPT},
    {"too few flags", [](GlyfParts & p) { p.flags = std::string(2, char(0x7F)); },
        GFNT_ERR_CORRUPT},
    {"no instruction length", [](GlyfParts & p) { p.glyph.pop_back(); },
        GFNT_ERR_CORRUPT},
    {"instructions longer than their stream",
        [](GlyfParts & p) { p.glyph.back() = char(5); }, GFNT_ERR_CORRUPT},
    {"an explicit box that is not there",
        [](GlyfParts & p) { p.bbox = std::string("\x40\0\0\0", 4); },
        GFNT_ERR_CORRUPT},
    {"a box bitmap longer than its stream",
        [](GlyfParts & p) { p.bbox = std::string(2, '\0'); }, GFNT_ERR_CORRUPT},
    {"a coordinate that leaves 16 bits",
        [](GlyfParts & p) { p.glyph = be(0xFFFF, 2) + be(0xFFFF, 2) + p.glyph; },
        GFNT_ERR_CORRUPT},
    {"an overlap bitmap that is promised and missing",
        [](GlyfParts & p) { p.options = 1; }, GFNT_ERR_CORRUPT},
  };
  for (const Case & c : cases) {
    GlyfParts parts;
    c.spoil(parts);
    EXPECT_EQ(load_result(make_woff2(valid_spec(parts))), c.expect) << c.what;
  }
  {
    // A header cut short.
    Woff2Spec spec = valid_spec();
    spec.tables[0].stream.resize(20);
    EXPECT_EQ(load_result(make_woff2(spec)), GFNT_ERR_CORRUPT) << "short header";
  }
  {
    // A loca that does not match the glyph count.
    EXPECT_EQ(load_result(make_woff2(valid_spec(GlyfParts(), 8))), GFNT_ERR_CORRUPT)
        << "loca length";
  }
  {
    // A transformed loca that states a transform length.
    Woff2Spec spec = valid_spec();
    spec.tables[1].stream = "x";
    EXPECT_EQ(load_result(make_woff2(spec)), GFNT_ERR_CORRUPT) << "loca length";
  }
  {
    // glyf transformed and loca not, and the other way round.
    Woff2Spec spec = valid_spec();
    spec.tables[1].transformed = false;
    spec.tables[1].version = 3;
    spec.tables[1].orig = 0;
    EXPECT_EQ(load_result(make_woff2(spec)), GFNT_ERR_CORRUPT) << "glyf only";
    spec = valid_spec();
    spec.tables[0].transformed = false;
    spec.tables[0].version = 3;
    spec.tables[0].stream = "";
    EXPECT_EQ(load_result(make_woff2(spec)), GFNT_ERR_CORRUPT) << "loca only";
  }
  {
    // Limits.
    GFNT_Limits limits;
    gfnt_limits_default(&limits);
    limits.max_outline_points = 2;
    EXPECT_EQ(load_result(make_woff2(valid_spec()), &limits), GFNT_ERR_LIMIT)
        << "points";
    gfnt_limits_default(&limits);
    limits.max_glyphs = 1;
    EXPECT_EQ(load_result(make_woff2(valid_spec()), &limits), GFNT_ERR_LIMIT)
        << "glyphs";
    gfnt_limits_default(&limits);
    limits.max_tables = 3;
    EXPECT_EQ(load_result(make_woff2(valid_spec()), &limits), GFNT_ERR_LIMIT)
        << "tables";
    gfnt_limits_default(&limits);
    limits.max_blob_bytes = 100;
    EXPECT_EQ(load_result(make_woff2(valid_spec()), &limits), GFNT_ERR_LIMIT)
        << "bytes";
  }
}

TEST(Woff2, ATransformedHmtxIsHeldToTheFormat) {
  auto hmtx_spec = [&](const std::string & stream, uint32_t metrics) {
    Woff2Spec spec = valid_spec();
    std::string hhea = zeros(36);
    hhea[34] = char(metrics >> 8);
    hhea[35] = char(metrics);
    spec.tables[3].stream = hhea;
    spec.tables.emplace_back(tag("hmtx"), stream, 0, 1, true);
    return spec;
  };
  // Two glyphs, both with proportional metrics: the flags byte, two advances, and
  // (bit 0 set) no bearings.
  const std::string fine = std::string(1, char(3)) + be(500, 2) + be(600, 2);
  EXPECT_EQ(load_result(make_woff2(hmtx_spec(fine, 2))), GFNT_OK);
  EXPECT_EQ(load_result(make_woff2(hmtx_spec(std::string(1, char(7)) + be(500, 2)
      + be(600, 2), 2))), GFNT_ERR_CORRUPT) << "reserved flag bits";
  EXPECT_EQ(load_result(make_woff2(hmtx_spec(std::string(1, char(3)) + be(500, 2), 2))),
      GFNT_ERR_CORRUPT) << "advances cut short";
  EXPECT_EQ(load_result(make_woff2(hmtx_spec(fine, 3))), GFNT_ERR_CORRUPT)
      << "more metrics than glyphs";
  {
    Woff2Spec spec = hmtx_spec(fine, 2);
    spec.tables.erase(spec.tables.begin() + 3);  // no hhea
    EXPECT_EQ(load_result(make_woff2(spec)), GFNT_ERR_CORRUPT) << "no hhea";
  }
}

TEST(Woff2, ACollectionDirectoryIsHeldToTheFormat) {
  auto collection = [&](uint32_t fonts, const std::vector<std::vector<uint32_t>> & faces) {
    Woff2Spec spec = valid_spec();
    spec.flavor = 0x74746366;
    spec.collection = be(0x00010000, 4) + u255(fonts);
    for (const auto & face : faces) {
      spec.collection += u255(uint32_t(face.size())) + be(0x00010000, 4);
      for (uint32_t index : face) spec.collection += u255(index);
    }
    return make_woff2(spec);
  };
  // glyf, loca, head, hhea, maxp are entries 0 to 4.
  EXPECT_EQ(load_result(collection(1, {{0, 1, 2, 3, 4}})), GFNT_OK);
  EXPECT_EQ(load_result(collection(0, {})), GFNT_ERR_CORRUPT) << "no fonts";
  EXPECT_EQ(load_result(collection(1, {{0, 1, 2, 3, 9}})), GFNT_ERR_CORRUPT)
      << "an index past the directory";
  EXPECT_EQ(load_result(collection(1, {{0, 1, 2, 3, 3}})), GFNT_ERR_CORRUPT)
      << "the same table twice in one font";
  EXPECT_EQ(load_result(collection(1, {{0, 2, 3, 4}})), GFNT_ERR_CORRUPT)
      << "glyf without its loca";
  EXPECT_EQ(load_result(collection(1, {{1, 2, 3, 4}})), GFNT_ERR_CORRUPT)
      << "loca without its glyf";
  EXPECT_EQ(load_result(collection(2, {{0, 1, 2, 3, 4}})), GFNT_ERR_CORRUPT)
      << "fewer fonts than stated";
  EXPECT_EQ(load_result(collection(1, {{0, 1, 2, 3, 4, 0, 1, 2, 3, 4, 0}})),
      GFNT_ERR_CORRUPT) << "more tables than the directory has";
}

TEST(Woff2, BytesAfterTheStatedEndAreTolerated) {
  std::string bytes = make_woff2(valid_spec());
  bytes.append("trailing");
  EXPECT_EQ(load_result(bytes), GFNT_OK);
}

TEST(Woff2, AnAllocationFailureAnywhereIsReported) {
  for (const char * name : {"woff2-triplets.woff2", "woff2-collection.woff2"}) {
    const std::string bytes = read_fixture(name);
    size_t refused = 0;
    for (size_t fail_at = 0; fail_at < 120; ++fail_at) {
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
        EXPECT_EQ(result, GFNT_ERR_OOM) << name << " allocation " << fail_at;
        EXPECT_EQ(face, nullptr);
      }
      gfnt_face_free(face);
      gfnt_blob_destroy(blob);
      EXPECT_EQ(allocator.live(), 0u) << name << " allocation " << fail_at
                                      << " leaked";
    }
    EXPECT_GT(refused, 10u) << name;
  }
}


// ---------------------------------------------------------------------------
// The writer.

namespace {

/** Every table of @p face, as the writer takes them. */
struct TableSet {
  std::vector<GFNT_WriteTable> tables;
  explicit TableSet(const GFNT_Face * face) {
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

/** The file @p source written as a WOFF 2. */
std::string written(const GFNT_Face * source, GFNT_Result * result = nullptr,
    const GFNT_Limits * limits = nullptr) {
  TableSet set(source);
  GFNT_Blob * out = nullptr;
  GFNT_Result r = gfnt_write_woff2(gfnt_face_flavour(source), set.tables.data(),
      set.tables.size(), limits, nullptr, &out, nullptr);
  if (result) *result = r;
  std::string bytes;
  if (r == GFNT_OK) {
    bytes.assign(reinterpret_cast<const char *>(gfnt_blob_data(out)),
        gfnt_blob_size(out));
  }
  gfnt_blob_destroy(out);
  return bytes;
}

/** The transform version the file's directory states for @p t, or -1. */
int transform_of(const std::string & woff2, uint32_t t) {
  const char known[][5] = {"cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2",
      "post", "cvt ", "fpgm", "glyf", "loca"};
  size_t count = be16(woff2, 12);
  size_t at = 48;
  for (size_t i = 0; i < count; ++i) {
    uint8_t flags = uint8_t(woff2[at++]);
    uint32_t entry_tag;
    if ((flags & 0x3F) == 63) {
      entry_tag = be32(woff2, at);
      at += 4;
    } else if ((flags & 0x3F) < 12) {
      const char * k = known[flags & 0x3F];
      entry_tag = tag(reinterpret_cast<const char (&)[5]>(*k));
    } else {
      entry_tag = 0;  // another known tag; none of the three asked about
    }
    while (uint8_t(woff2[at++]) & 0x80) {}                 // origLength
    const int version = flags >> 6;
    const bool transformed = (entry_tag == tag("glyf") || entry_tag == tag("loca"))
        ? version == 0 : (entry_tag == tag("hmtx") && version == 1);
    if (transformed) while (uint8_t(woff2[at++]) & 0x80) {}  // transformLength
    if (entry_tag == t) return version;
  }
  return -1;
}

}  // namespace

TEST(Woff2Writer, WhatItWritesReadsBackAsTheFontItWasGiven) {
  for (const char * name : {"basic.ttf", "woff2-triplets-source.ttf", "cff.otf",
                            "woff2-stored.woff2", "woff2-triplets.woff2"}) {
    const std::string bytes = read_fixture(name);
    Crafted source(bytes);
    ASSERT_EQ(source.result, GFNT_OK) << name;
    GFNT_Result result = GFNT_ERR_INTERNAL;
    const std::string woff2 = written(source.face, &result);
    ASSERT_EQ(result, GFNT_OK) << name;
    ASSERT_EQ(woff2.substr(0, 4), "wOF2") << name;
    EXPECT_EQ(woff2.size() % 4, 0u) << name;
    EXPECT_EQ(be32(woff2, 8), woff2.size()) << "the stated length";
    Crafted back(woff2);
    ASSERT_EQ(back.result, GFNT_OK) << name << ": " << back.error.message;
    expect_same_font(back.face, source.face, name);
  }
}

TEST(Woff2Writer, ATrueTypeFontIsTransformedAndACffFontIsNot) {
  Crafted truetype(read_fixture("woff2-triplets-source.ttf"));
  ASSERT_EQ(truetype.result, GFNT_OK);
  const std::string a = written(truetype.face);
  EXPECT_EQ(transform_of(a, tag("glyf")), 0);
  EXPECT_EQ(transform_of(a, tag("loca")), 0);
  EXPECT_EQ(transform_of(a, tag("hmtx")), 1) << "its bearings are its xMin";

  Crafted basic(read_fixture("basic.ttf"));
  ASSERT_EQ(basic.result, GFNT_OK);
  const std::string b = written(basic.face);
  EXPECT_EQ(transform_of(b, tag("glyf")), 0);

  Crafted cff(read_fixture("cff.otf"));
  ASSERT_EQ(cff.result, GFNT_OK);
  const std::string c = written(cff.face);
  EXPECT_EQ(transform_of(c, tag("glyf")), -1);
  EXPECT_EQ(transform_of(c, tag("hmtx")), 0);
}

TEST(Woff2Writer, TheTransformMakesTheFileSmallerThanTheSfnt) {
  const std::string bytes = read_fixture("woff2-triplets-source.ttf");
  Crafted source(bytes);
  ASSERT_EQ(source.result, GFNT_OK);
  EXPECT_LT(written(source.face).size(), bytes.size());
}

TEST(Woff2Writer, AGlyfThatDoesNotParseIsStoredAsItIs) {
  // Cut the glyph data of the last glyph short: the font loads (glyphs are read on
  // use) and the writer cannot transform it, so it stores glyf and loca and the
  // file still reads back to the same bytes.
  Crafted source(read_fixture("woff2-triplets-source.ttf"));
  ASSERT_EQ(source.result, GFNT_OK);
  TableSet set(source.face);
  std::string damaged;
  for (auto & t : set.tables) {
    if (t.tag == tag("glyf")) {
      damaged.assign(reinterpret_cast<const char *>(t.data), t.length);
      damaged[0] = char(0x7F);          // glyph 0: 32,000-odd contours, cut short
      damaged[1] = char(0x00);
      t.data = reinterpret_cast<const uint8_t *>(damaged.data());
    }
  }
  GFNT_Blob * out = nullptr;
  ASSERT_EQ(gfnt_write_woff2(gfnt_face_flavour(source.face), set.tables.data(),
      set.tables.size(), nullptr, nullptr, &out, nullptr), GFNT_OK);
  const std::string woff2(reinterpret_cast<const char *>(gfnt_blob_data(out)),
      gfnt_blob_size(out));
  gfnt_blob_destroy(out);
  EXPECT_EQ(transform_of(woff2, tag("glyf")), 3);
  EXPECT_EQ(transform_of(woff2, tag("loca")), 3);
  Crafted back(woff2);
  ASSERT_EQ(back.result, GFNT_OK) << back.error.message;
  EXPECT_EQ(table_of(back.face, tag("glyf")), damaged);
}

TEST(Woff2Writer, RefusesWhatItCannotWrite) {
  Crafted source(read_fixture("basic.ttf"));
  ASSERT_EQ(source.result, GFNT_OK);
  TableSet set(source.face);
  GFNT_Blob * out = nullptr;
  EXPECT_EQ(gfnt_write_woff2(0x00010000, set.tables.data(), 0, nullptr, nullptr,
      &out, nullptr), GFNT_ERR_INVALID) << "no tables";
  EXPECT_EQ(gfnt_write_woff2(0x00010000, nullptr, 3, nullptr, nullptr, &out,
      nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_write_woff2(0x00010000, set.tables.data(), set.tables.size(),
      nullptr, nullptr, nullptr, nullptr), GFNT_ERR_INVALID);
  {
    auto doubled = set.tables;
    doubled.push_back(doubled[0]);
    EXPECT_EQ(gfnt_write_woff2(0x00010000, doubled.data(), doubled.size(), nullptr,
        nullptr, &out, nullptr), GFNT_ERR_INVALID) << "a repeated tag";
  }
  {
    auto bad = set.tables;
    bad[0].data = nullptr;
    EXPECT_EQ(gfnt_write_woff2(0x00010000, bad.data(), bad.size(), nullptr, nullptr,
        &out, nullptr), GFNT_ERR_INVALID) << "bytes missing";
  }
  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_tables = 2;
  EXPECT_EQ(gfnt_write_woff2(0x00010000, set.tables.data(), set.tables.size(),
      &limits, nullptr, &out, nullptr), GFNT_ERR_LIMIT);
  gfnt_limits_default(&limits);
  limits.max_blob_bytes = 100;
  GFNT_Result result = GFNT_OK;
  written(source.face, &result, &limits);
  EXPECT_EQ(result, GFNT_ERR_LIMIT);
  EXPECT_EQ(out, nullptr);
}

TEST(Woff2Writer, AStatedBoxThatIsNotThePointsBoxIsKept) {
  // Each of the four numbers of a stored box, made wrong in turn: the writer must
  // state the box explicitly, and the reader must give it back.
  Crafted source(read_fixture("woff2-triplets-source.ttf"));
  ASSERT_EQ(source.result, GFNT_OK);
  const std::string head = table_of(source.face, tag("head"));
  const std::string loca = table_of(source.face, tag("loca"));
  const bool long_format = be16(head, 50) == 1;
  const size_t glyph3 = long_format ? be32(loca, 12) : 2 * be16(loca, 6);
  for (int field = 0; field < 4; ++field) {
    TableSet set(source.face);
    std::string glyf = table_of(source.face, tag("glyf"));
    const size_t at = glyph3 + 2 + 2 * size_t(field);
    glyf[at + 1] = char(glyf[at + 1] + 7);
    for (auto & t : set.tables) {
      if (t.tag == tag("glyf")) t.data = reinterpret_cast<const uint8_t *>(glyf.data());
    }
    GFNT_Blob * out = nullptr;
    ASSERT_EQ(gfnt_write_woff2(gfnt_face_flavour(source.face), set.tables.data(),
        set.tables.size(), nullptr, nullptr, &out, nullptr), GFNT_OK);
    const std::string woff2(reinterpret_cast<const char *>(gfnt_blob_data(out)),
        gfnt_blob_size(out));
    gfnt_blob_destroy(out);
    Crafted back(woff2);
    ASSERT_EQ(back.result, GFNT_OK) << back.error.message;
    const std::string rebuilt = glyph_bytes(back.face, 3);
    ASSERT_GE(rebuilt.size(), 10u);
    for (int k = 0; k < 4; ++k) {
      EXPECT_EQ(be16(rebuilt, 2 + 2 * size_t(k)), be16(glyf, glyph3 + 2 + 2 * size_t(k)))
          << "field " << field << " number " << k;
    }
  }
}

TEST(Woff2Writer, AnAllocationFailureAnywhereIsReported) {
  Crafted source(read_fixture("woff2-triplets-source.ttf"));
  ASSERT_EQ(source.result, GFNT_OK);
  TableSet set(source.face);
  size_t refused = 0;
  for (size_t fail_at = 0; fail_at < 80; ++fail_at) {
    gfnttest::FailingAllocator allocator(fail_at);
    GFNT_Blob * out = nullptr;
    const GFNT_Result result = gfnt_write_woff2(gfnt_face_flavour(source.face),
        set.tables.data(), set.tables.size(), nullptr, allocator.get(), &out,
        nullptr);
    if (result != GFNT_OK) {
      ++refused;
      EXPECT_EQ(result, GFNT_ERR_OOM) << "allocation " << fail_at;
      EXPECT_EQ(out, nullptr);
    }
    gfnt_blob_destroy(out);
    EXPECT_EQ(allocator.live(), 0u) << "allocation " << fail_at << " leaked";
  }
  EXPECT_GT(refused, 10u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
