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
#include "failing_allocator.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/write.h>

#include <gtest/gtest.h>

namespace {

enum : uint32_t {
  kX = 2, kY = 3, kZ = 4, kParen = 5, kParenV1 = 6, kParenV2 = 7, kParenBt = 8, kParenEx = 9,
  kParenTp = 10, kBar = 11, kBarV1 = 12, kIntegral = 13, kIntegralV1 = 14, kIntegralBt = 15,
  kIntegralEx = 16, kIntegralTp = 17, kOverbrace = 18, kOverbraceH1 = 19, kOverbraceLf = 20,
  kOverbraceMid = 21, kOverbraceRt = 22, kOverbraceExt = 23, kHat = 24,
};

std::vector<uint8_t> read_font(const std::string & name) {
  std::ifstream in(gfnttest::data("fonts/" + name), std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Face {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  explicit Face(std::vector<uint8_t> data) : bytes(std::move(data)) {
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

/** A subset, with its numbering. */
struct Subset {
  GFNT_Blob * blob = nullptr;
  GFNT_SubsetMap map{};
  GFNT_Result result = GFNT_OK;

  Subset(const GFNT_Face * source, std::vector<uint32_t> cps, bool retain,
      bool drop_layout = false) {
    GFNT_SubsetOptions o;
    gfnt_subset_options_init(&o);
    o.codepoints = cps.data();
    o.codepoint_count = cps.size();
    o.retain_gids = retain;
    o.drop_layout = drop_layout;
    o.map = &map;
    result = gfnt_subset(source, &o, nullptr, nullptr, &blob, nullptr);
  }
  Subset(const Subset &) = delete;
  Subset & operator=(const Subset &) = delete;
  ~Subset() {
    gfnt_blob_destroy(blob);
    gfnt_subset_map_free(&map, nullptr);
  }
  std::vector<uint8_t> bytes() const {
    const uint8_t * d = static_cast<const uint8_t *>(gfnt_blob_data(blob));
    return std::vector<uint8_t>(d, d + gfnt_blob_size(blob));
  }
  /** The kept glyphs, by their ids in the source, ascending. */
  std::vector<uint32_t> kept() const {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < map.count; ++i) {
      if (map.old_of_new[i] != 0xFFFFFFFFu) {      // retained ids leave gaps
        out.push_back(map.old_of_new[i]);
      }
    }
    std::sort(out.begin(), out.end());
    return out;
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

std::vector<uint8_t> table_bytes(const std::vector<uint8_t> & b, const char * tag) {
  const size_t n = (b[4] << 8) | b[5];
  for (size_t i = 0; i < n; ++i) {
    const size_t e = 12 + 16 * i;
    if (std::string(reinterpret_cast<const char *>(&b[e]), 4) == tag) {
      const size_t off = (static_cast<size_t>(b[e + 8]) << 24) | (b[e + 9] << 16)
          | (b[e + 10] << 8) | b[e + 11];
      const size_t len = (static_cast<size_t>(b[e + 12]) << 24) | (b[e + 13] << 16)
          | (b[e + 14] << 8) | b[e + 15];
      return std::vector<uint8_t>(b.begin() + off, b.begin() + off + len);
    }
  }
  return {};
}

/** The bytes of a `MATH` table, read the way the format says. */
struct Math {
  std::vector<uint8_t> t;

  explicit Math(std::vector<uint8_t> table) : t(std::move(table)) {}
  bool present() const { return !t.empty(); }
  uint32_t u16(size_t at) const { return at + 2 <= t.size() ? (t[at] << 8) | t[at + 1] : 0xFFFFFFu; }
  size_t constants() const { return u16(4); }
  size_t info() const { return u16(6); }
  size_t variants() const { return u16(8); }

  /** The glyphs of a coverage table, in order. */
  std::vector<uint32_t> coverage(size_t at) const {
    std::vector<uint32_t> out;
    if (!at) {
      return out;
    }
    const uint32_t format = u16(at), n = u16(at + 2);
    for (uint32_t i = 0; i < n; ++i) {
      if (format == 1) {
        out.push_back(u16(at + 4 + 2 * i));
      }
      else {
        for (uint32_t g = u16(at + 4 + 6 * i); g <= u16(at + 6 + 6 * i); ++g) {
          out.push_back(g);
        }
      }
    }
    return out;
  }
  /** A sub-table of MathGlyphInfo (0 italics, 1 top accent, 2 extended shapes, 3 kern info). */
  size_t info_part(int which) const {
    const size_t off = info() ? u16(info() + 2 * which) : 0;
    return off ? info() + off : 0;
  }
  std::vector<uint32_t> italics() const {
    const size_t at = info_part(0);
    return at ? coverage(at + u16(at)) : std::vector<uint32_t>();
  }
  std::vector<uint32_t> accents() const {
    const size_t at = info_part(1);
    return at ? coverage(at + u16(at)) : std::vector<uint32_t>();
  }
  std::vector<uint32_t> extended() const { return coverage(info_part(2)); }
  std::vector<uint32_t> kerned() const {
    const size_t at = info_part(3);
    return at ? coverage(at + u16(at)) : std::vector<uint32_t>();
  }
  /** One direction's covered glyphs (vertical false is horizontal). */
  std::vector<uint32_t> covered(bool vertical) const {
    const size_t v = variants();
    return v ? coverage(u16(v + (vertical ? 2 : 4)) ? v + u16(v + (vertical ? 2 : 4)) : 0)
             : std::vector<uint32_t>();
  }
  /** The construction of the nth covered glyph of a direction, as its offset in the table. */
  size_t construction(bool vertical, size_t n) const {
    const size_t v = variants();
    const size_t first = v + 10 + (vertical ? 0 : 2 * u16(v + 6));
    return v + u16(first + 2 * n);
  }
  std::vector<uint32_t> sizes(size_t con) const {
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < u16(con + 2); ++i) {
      out.push_back(u16(con + 4 + 4 * i));
    }
    return out;
  }
  std::vector<uint32_t> parts(size_t con) const {
    std::vector<uint32_t> out;
    if (!u16(con)) {
      return out;
    }
    const size_t a = con + u16(con);
    for (uint32_t i = 0; i < u16(a + 4); ++i) {
      out.push_back(u16(a + 6 + 10 * i));
    }
    return out;
  }
};

Math math_of(const Subset & s) { return Math(table_bytes(s.bytes(), "MATH")); }

TEST(SubsetMath, AGlyphBringsItsSizesAndItsPieces) {
  Face source(read_font("math.ttf"));
  Subset s(source.face, {'('}, true);
  ASSERT_EQ(s.result, GFNT_OK);
  EXPECT_EQ(s.kept(), (std::vector<uint32_t>{0, kParen, kParenV1, kParenV2, kParenBt, kParenEx,
                kParenTp}));
  Math m = math_of(s);
  ASSERT_TRUE(m.present());
  ASSERT_EQ(m.covered(true), (std::vector<uint32_t>{kParen}));
  const size_t con = m.construction(true, 0);
  EXPECT_EQ(m.sizes(con), (std::vector<uint32_t>{kParen, kParenV1, kParenV2}));
  EXPECT_EQ(m.parts(con), (std::vector<uint32_t>{kParenBt, kParenEx, kParenTp}));
  EXPECT_EQ(m.covered(false), std::vector<uint32_t>()) << "no horizontal construction is kept";
  EXPECT_EQ(m.u16(m.variants()), 50u) << "the connector overlap";
}

TEST(SubsetMath, OnlyTheKeptGlyphsKeepTheirEntries) {
  Face source(read_font("math.ttf"));
  Subset s(source.face, {'x', '('}, true);
  ASSERT_EQ(s.result, GFNT_OK);
  Math m = math_of(s);
  ASSERT_TRUE(m.present());
  EXPECT_EQ(m.italics(), (std::vector<uint32_t>{kX}));
  EXPECT_EQ(m.accents(), (std::vector<uint32_t>{kX}));
  EXPECT_EQ(m.kerned(), (std::vector<uint32_t>{kX}));
  EXPECT_EQ(m.extended(), (std::vector<uint32_t>{kParenV2}));
  // x has a kern at its top right and bottom left only.
  const size_t ki = m.info_part(3);
  ASSERT_NE(ki, 0u);
  const size_t rec = ki + 4;
  EXPECT_NE(m.u16(rec), 0u) << "top right";
  EXPECT_EQ(m.u16(rec + 2), 0u) << "top left";
  EXPECT_EQ(m.u16(rec + 4), 0u) << "bottom right";
  EXPECT_NE(m.u16(rec + 6), 0u) << "bottom left";
  const size_t top_right = ki + m.u16(rec);
  EXPECT_EQ(m.u16(top_right), 2u) << "two correction heights";
  EXPECT_EQ(m.u16(top_right + 2), 100u);
  EXPECT_EQ(m.u16(top_right + 6), 300u);
  EXPECT_EQ(m.u16(top_right + 10), 10u) << "the first kern";
  EXPECT_EQ(m.u16(top_right + 18), 30u) << "the last";
}

TEST(SubsetMath, TheConstantsAreCarriedWithTheirDevices) {
  Face source(read_font("math.ttf"));
  Subset s(source.face, {'x'}, true);
  ASSERT_EQ(s.result, GFNT_OK);
  Math ours = math_of(s);
  Math theirs(table_bytes(source.bytes, "MATH"));
  ASSERT_TRUE(ours.present());
  const size_t a = ours.constants(), b = theirs.constants();
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(ours.u16(a + 2 * i), theirs.u16(b + 2 * i)) << i;
  }
  EXPECT_EQ(ours.u16(a + 8 + 4 * 51), 60u) << "the radical degree raise";
  size_t devices = 0;
  for (size_t i = 0; i < 51; ++i) {
    EXPECT_EQ(ours.u16(a + 8 + 4 * i), theirs.u16(b + 8 + 4 * i)) << "value " << i;
    const uint32_t d = ours.u16(a + 10 + 4 * i), e = theirs.u16(b + 10 + 4 * i);
    EXPECT_EQ(d != 0, e != 0) << "device " << i;
    if (d) {
      ++devices;
      // StartSize, EndSize, DeltaFormat and the packed deltas, byte for byte.
      const size_t words = 3 + (ours.u16(a + d + 4) == 1 ? 1 : 1);
      for (size_t w = 0; w < words; ++w) {
        EXPECT_EQ(ours.u16(a + d + 2 * w), theirs.u16(b + e + 2 * w)) << i << " word " << w;
      }
    }
  }
  EXPECT_EQ(devices, 2u);
}

TEST(SubsetMath, AValueKeepsItsDeviceTable) {
  Face source(read_font("math.ttf"));
  Subset s(source.face, {'y'}, true);
  ASSERT_EQ(s.result, GFNT_OK);
  Math m = math_of(s);
  ASSERT_EQ(m.italics(), (std::vector<uint32_t>{kY}));
  const size_t at = m.info_part(0);
  EXPECT_EQ(m.u16(at + 2), 1u);
  EXPECT_EQ(m.u16(at + 4), 32u);
  const uint32_t dev = m.u16(at + 6);
  ASSERT_NE(dev, 0u);
  EXPECT_EQ(m.u16(at + dev), 12u) << "StartSize";
  EXPECT_EQ(m.u16(at + dev + 2), 14u) << "EndSize";
  EXPECT_EQ(m.u16(at + dev + 4), 1u) << "DeltaFormat";
  // [1, 1, 0] as 2-bit signed values from the high end: 01 01 00 -> 0x5000.
  EXPECT_EQ(m.u16(at + dev + 6), 0x5000u);
}

TEST(SubsetMath, AnAssemblyKeepsItsPartsAndTheItalicsCorrectionOfItsDevice) {
  Face source(read_font("math.ttf"));
  Subset s(source.face, {'('}, true);
  Math m = math_of(s);
  const size_t con = m.construction(true, 0);
  const size_t a = con + m.u16(con);
  EXPECT_EQ(m.u16(a), 7u) << "the assembly's italics correction";
  const uint32_t dev = m.u16(a + 2);
  ASSERT_NE(dev, 0u);
  EXPECT_EQ(m.u16(a + dev), 11u);
  EXPECT_EQ(m.u16(a + dev + 2), 13u);
  EXPECT_EQ(m.u16(a + 4), 3u) << "three parts";
  // The middle part is the extender: flag 1, connectors 100 and 100, advance 300.
  EXPECT_EQ(m.u16(a + 6 + 10 + 2), 100u);
  EXPECT_EQ(m.u16(a + 6 + 10 + 4), 100u);
  EXPECT_EQ(m.u16(a + 6 + 10 + 6), 300u);
  EXPECT_EQ(m.u16(a + 6 + 10 + 8), 1u);
}

TEST(SubsetMath, AHorizontalConstructionIsKeptAsOne) {
  Face source(read_font("math.ttf"));
  Subset s(source.face, {0x23DE}, true);
  ASSERT_EQ(s.result, GFNT_OK);
  Math m = math_of(s);
  ASSERT_TRUE(m.present());
  EXPECT_EQ(m.covered(true), std::vector<uint32_t>());
  ASSERT_EQ(m.covered(false), (std::vector<uint32_t>{kOverbrace}));
  const size_t con = m.construction(false, 0);
  EXPECT_EQ(m.sizes(con), (std::vector<uint32_t>{kOverbrace, kOverbraceH1}));
  EXPECT_EQ(m.parts(con), (std::vector<uint32_t>{kOverbraceLf, kOverbraceExt, kOverbraceMid,
                kOverbraceExt, kOverbraceRt}));
}

TEST(SubsetMath, ConstructionsOfSeveralGlyphsStayInOrder) {
  Face source(read_font("math.ttf"));
  Subset s(source.face, {'(', '|', 0x222B, 0x23DE}, true);
  ASSERT_EQ(s.result, GFNT_OK);
  Math m = math_of(s);
  ASSERT_EQ(m.covered(true), (std::vector<uint32_t>{kParen, kBar, kIntegral}));
  EXPECT_EQ(m.sizes(m.construction(true, 1)), (std::vector<uint32_t>{kBar, kBarV1}));
  EXPECT_EQ(m.u16(m.construction(true, 1)), 0u) << "a bar has no assembly";
  EXPECT_EQ(m.parts(m.construction(true, 2)),
      (std::vector<uint32_t>{kIntegralBt, kIntegralEx, kIntegralTp}));
  EXPECT_EQ(m.covered(false), (std::vector<uint32_t>{kOverbrace}));
  EXPECT_EQ(m.extended(), (std::vector<uint32_t>{kParenV2, kBarV1, kIntegral}));
}

TEST(SubsetMath, RenumberedEntriesNameTheNewIds) {
  Face source(read_font("math.ttf"));
  Subset s(source.face, {'x', '(', 0x23DE}, false);
  ASSERT_EQ(s.result, GFNT_OK);
  Math m = math_of(s);
  ASSERT_TRUE(m.present());
  EXPECT_EQ(s.kept(), (std::vector<uint32_t>{0, kX, kParen, kParenV1, kParenV2, kParenBt, kParenEx,
                kParenTp, kOverbrace, kOverbraceH1, kOverbraceLf, kOverbraceMid, kOverbraceRt,
                kOverbraceExt}));
  const auto id = [&](uint32_t old) { return static_cast<uint32_t>(s.new_of(old)); };
  EXPECT_EQ(m.italics(), (std::vector<uint32_t>{id(kX)}));
  EXPECT_EQ(m.extended(), (std::vector<uint32_t>{id(kParenV2)}));
  ASSERT_EQ(m.covered(true), (std::vector<uint32_t>{id(kParen)}));
  EXPECT_EQ(m.sizes(m.construction(true, 0)),
      (std::vector<uint32_t>{id(kParen), id(kParenV1), id(kParenV2)}));
  EXPECT_EQ(m.parts(m.construction(true, 0)),
      (std::vector<uint32_t>{id(kParenBt), id(kParenEx), id(kParenTp)}));
  EXPECT_EQ(m.parts(m.construction(false, 0)),
      (std::vector<uint32_t>{id(kOverbraceLf), id(kOverbraceExt), id(kOverbraceMid),
                id(kOverbraceExt), id(kOverbraceRt)}));
}

/** The rows of a `GDEF` item variation store, one vector of deltas per row, per data set. */
std::vector<std::vector<std::vector<int>>> store_rows(const std::vector<uint8_t> & gdef) {
  std::vector<std::vector<std::vector<int>>> sets;
  Math g(gdef);
  if (gdef.empty() || g.u16(2) < 3) {
    return sets;
  }
  const size_t offset = (static_cast<size_t>(g.u16(14)) << 16) | g.u16(16);
  if (!offset) {
    return sets;
  }
  const size_t count = g.u16(offset + 6);
  for (size_t d = 0; d < count; ++d) {
    const size_t at = offset + ((static_cast<size_t>(g.u16(offset + 8 + 4 * d)) << 16)
                                | g.u16(offset + 10 + 4 * d));
    const uint32_t items = g.u16(at), words = g.u16(at + 2) & 0x7FFF, regions = g.u16(at + 4);
    const bool wide = (g.u16(at + 2) & 0x8000) != 0;
    size_t pos = at + 6 + 2 * regions;
    std::vector<std::vector<int>> rows;
    for (uint32_t i = 0; i < items; ++i) {
      std::vector<int> row;
      for (uint32_t r = 0; r < regions; ++r) {
        if (r < words) {
          const int v = wide ? static_cast<int32_t>((static_cast<uint32_t>(g.u16(pos)) << 16)
                                                   | g.u16(pos + 2))
                             : static_cast<int16_t>(g.u16(pos));
          row.push_back(v);
          pos += wide ? 4 : 2;
        }
        else {
          row.push_back(static_cast<int8_t>(gdef[pos]));
          pos += 1;
        }
      }
      rows.push_back(row);
    }
    sets.push_back(rows);
  }
  return sets;
}

/** A device table that is a variation index: (outer, inner), or (-1, -1) when it is not one. */
std::pair<int, int> variation_index(const Math & m, size_t table, size_t device_field) {
  const uint32_t off = m.u16(device_field);
  if (!off || m.u16(table + off + 4) != 0x8000) {
    return {-1, -1};
  }
  return {static_cast<int>(m.u16(table + off)), static_cast<int>(m.u16(table + off + 2))};
}

TEST(SubsetMath, RetainedIdsKeepTheirVariationIndicesAndTheWholeStore) {
  Face source(read_font("math-variable.ttf"));
  Subset s(source.face, {'x'}, true);
  ASSERT_EQ(s.result, GFNT_OK);
  Math m = math_of(s);
  ASSERT_TRUE(m.present());
  // AxisHeight is the second value record: (outer 1, inner 2), as the source has it.
  EXPECT_EQ(variation_index(m, m.constants(), m.constants() + 8 + 4 * 1 + 2), (std::pair<int, int>(1, 2)));
  const auto rows = store_rows(table_bytes(s.bytes(), "GDEF"));
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].size(), 3u);
  EXPECT_EQ(rows[1].size(), 3u);
}

TEST(SubsetMath, RenumberedVariationIndicesNameTheRowsOfThePrunedStore) {
  Face source(read_font("math-variable.ttf"));
  Subset s(source.face, {'x'}, false);
  ASSERT_EQ(s.result, GFNT_OK);
  Math m = math_of(s);
  ASSERT_TRUE(m.present());
  // The rows the kept entries name: AxisHeight's (1, 2) = [8, 8], first seen, and x's kern value
  // (1, 0) = [10, 0], second. Nothing else names a row, so nothing else is kept.
  const auto rows = store_rows(table_bytes(s.bytes(), "GDEF"));
  ASSERT_EQ(rows.size(), 1u) << "one set of the two is used";
  ASSERT_EQ(rows[0].size(), 2u) << "and two rows of its three";
  EXPECT_EQ(rows[0][0], (std::vector<int>{8, 8}));
  EXPECT_EQ(rows[0][1], (std::vector<int>{10, 0}));
  EXPECT_EQ(variation_index(m, m.constants(), m.constants() + 8 + 4 * 1 + 2), (std::pair<int, int>(0, 0)));
  // x's top-right kern's middle value: the table's second value record after the heights.
  const size_t ki = m.info_part(3);
  const size_t kern = ki + m.u16(ki + 4);
  EXPECT_EQ(variation_index(m, kern, kern + 2 + 4 * (2 + 1) + 2), (std::pair<int, int>(0, 1)));
}

TEST(SubsetMath, AVariationIndexNoKeptEntryNamesLeavesNoRow) {
  Face source(read_font("math-variable.ttf"));
  Subset s(source.face, {' '}, false);
  ASSERT_EQ(s.result, GFNT_OK);
  // Only the constants survive, and AxisHeight is the one value that names a row.
  const auto rows = store_rows(table_bytes(s.bytes(), "GDEF"));
  ASSERT_EQ(rows.size(), 1u);
  ASSERT_EQ(rows[0].size(), 1u);
  EXPECT_EQ(rows[0][0], (std::vector<int>{8, 8}));
}

TEST(SubsetMath, ATextWithNoMathKeepsOnlyTheConstants) {
  Face source(read_font("math.ttf"));
  Subset s(source.face, {' '}, true);
  ASSERT_EQ(s.result, GFNT_OK);
  Math m = math_of(s);
  ASSERT_TRUE(m.present());
  EXPECT_NE(m.constants(), 0u);
  EXPECT_EQ(m.info(), 0u) << "nothing in MathGlyphInfo says anything about the kept glyphs";
  EXPECT_EQ(m.covered(true), std::vector<uint32_t>());
  EXPECT_EQ(m.covered(false), std::vector<uint32_t>());
  EXPECT_EQ(m.u16(m.variants()), 50u) << "the connector overlap is still stated";
}

TEST(SubsetMath, DropLayoutLeavesTheTableOut) {
  Face source(read_font("math.ttf"));
  Subset s(source.face, {'('}, true, true);
  ASSERT_EQ(s.result, GFNT_OK);
  EXPECT_FALSE(math_of(s).present());
  EXPECT_EQ(s.kept(), (std::vector<uint32_t>{0, kParen})) << "and it brings no sizes";
}

TEST(SubsetMath, ATableThatCannotBeReadIsLeftOut) {
  std::vector<uint8_t> bytes = read_font("math.ttf");
  const std::vector<uint8_t> original = table_bytes(bytes, "MATH");
  // Point MathVariants far outside the table.
  const size_t n = (bytes[4] << 8) | bytes[5];
  size_t at = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t e = 12 + 16 * i;
    if (std::string(reinterpret_cast<const char *>(&bytes[e]), 4) == "MATH") {
      at = (static_cast<size_t>(bytes[e + 8]) << 24) | (bytes[e + 9] << 16) | (bytes[e + 10] << 8)
          | bytes[e + 11];
    }
  }
  ASSERT_NE(at, 0u);
  bytes[at + 8] = 0xFF;
  bytes[at + 9] = 0xF0;
  Face source(bytes);
  Subset s(source.face, {'('}, true);
  ASSERT_EQ(s.result, GFNT_OK) << "a font whose MATH is unreadable still subsets";
  EXPECT_FALSE(math_of(s).present());
}

TEST(SubsetMath, AnAllocationFailureAnywhereIsReported) {
  Face source(read_font("math.ttf"));
  std::vector<uint32_t> cps = {'x', 'y', '(', '|', 0x222B, 0x23DE};
  GFNT_SubsetOptions o;
  gfnt_subset_options_init(&o);
  o.codepoints = cps.data();
  o.codepoint_count = cps.size();
  size_t requests = 0;
  {
    gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
    GFNT_Blob * out = nullptr;
    ASSERT_EQ(gfnt_subset(source.face, &o, nullptr, counting.get(), &out, nullptr), GFNT_OK);
    requests = counting.requests();
    gfnt_blob_destroy(out);
    EXPECT_EQ(counting.live(), 0u);
  }
  ASSERT_GT(requests, 10u);
  for (size_t fail = 0; fail < requests; ++fail) {
    gfnttest::FailingAllocator failing(fail);
    GFNT_Blob * out = nullptr;
    const GFNT_Result r = gfnt_subset(source.face, &o, nullptr, failing.get(), &out, nullptr);
    if (r != GFNT_OK) {
      EXPECT_EQ(r, GFNT_ERR_OOM) << "request " << fail;
      EXPECT_EQ(out, nullptr);
    }
    gfnt_blob_destroy(out);
    EXPECT_EQ(failing.live(), 0u) << "request " << fail << " leaked";
  }
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
