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
 * The cases of rewriting layout tables under renumbering that the fixtures do not
 * reach, on fonts built here: a subtable too big for 16-bit offsets once its shared
 * parts are written out (split in two), a `GSUB` with `FeatureVariations`, and every
 * form of `kern` the shaper reads. Each compares the subset's shaping with the
 * source's through the glyph map the subsetter reports.
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/shape.h>
#include <ghoti.io/font/write.h>

#include <gtest/gtest.h>

using gfnttest::put_u16;
using gfnttest::put_s16;
using gfnttest::put_u32;

namespace {

constexpr uint32_t kFirstChar = 0x4E00;   // glyph g (g >= 1) is U+4E00 + g - 1

struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  Font() = default;
  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;
  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  GFNT_Result open(std::vector<uint8_t> data) {
    bytes = std::move(data);
    GFNT_Result r = gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_COPY,
        nullptr, nullptr, &blob, nullptr);
    if (r != GFNT_OK) {
      return r;
    }
    return gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr);
  }
};

/** A font of @p n empty glyphs, glyph g reached by U+4E00 + g - 1, plus @p extra tables. */
std::vector<uint8_t> glyph_font(uint16_t n, std::vector<gfnttest::Table> extra) {
  std::vector<uint8_t> hmtx;
  std::vector<uint8_t> loca;
  for (uint16_t g = 0; g < n; ++g) {
    put_u16(hmtx, 500);
    put_s16(hmtx, 0);
  }
  for (uint16_t g = 0; g <= n; ++g) {
    put_u16(loca, 0);
  }
  std::vector<gfnttest::Table> tables = {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, n)},
      {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(n)},
      {GFNT_TAG('h', 'm', 't', 'x'), hmtx},
      {GFNT_TAG('l', 'o', 'c', 'a'), loca},
      {GFNT_TAG('g', 'l', 'y', 'f'), {}},
      {GFNT_TAG('c', 'm', 'a', 'p'), gfnttest::build_cmap({{3, 10,
          gfnttest::build_cmap_format12({{kFirstChar, kFirstChar + n - 2u, 1}})}})},
  };
  for (auto & t : extra) {
    tables.push_back(std::move(t));
  }
  return gfnttest::build_sfnt(0x00010000, tables);
}

/** A layout table: one script (DFLT), the features given, and the lookups given. */
struct LookupSpec {
  uint16_t type;
  uint16_t flags = 0;
  std::vector<std::vector<uint8_t>> subtables;
};

std::vector<uint8_t> layout_table(const std::vector<std::pair<std::string, std::vector<uint16_t>>> & features,
    const std::vector<LookupSpec> & lookups, const std::vector<uint8_t> & variations = {}) {
  std::vector<uint8_t> scripts;
  put_u16(scripts, 1);
  scripts.insert(scripts.end(), {'D', 'F', 'L', 'T'});
  put_u16(scripts, 8);
  put_u16(scripts, 4);          // default LangSys
  put_u16(scripts, 0);
  put_u16(scripts, 0);          // lookupOrder
  put_u16(scripts, 0xFFFF);     // required feature
  put_u16(scripts, static_cast<uint16_t>(features.size()));
  for (size_t i = 0; i < features.size(); ++i) {
    put_u16(scripts, static_cast<uint16_t>(i));
  }

  std::vector<uint8_t> flist;
  std::vector<uint8_t> ftabs;
  put_u16(flist, static_cast<uint16_t>(features.size()));
  size_t base = 2 + 6 * features.size();
  for (const auto & f : features) {
    flist.insert(flist.end(), f.first.begin(), f.first.end());
    put_u16(flist, static_cast<uint16_t>(base + ftabs.size()));
    put_u16(ftabs, 0);
    put_u16(ftabs, static_cast<uint16_t>(f.second.size()));
    for (uint16_t l : f.second) {
      put_u16(ftabs, l);
    }
  }
  flist.insert(flist.end(), ftabs.begin(), ftabs.end());

  std::vector<uint8_t> llist;
  std::vector<uint8_t> ltabs;
  put_u16(llist, static_cast<uint16_t>(lookups.size()));
  size_t lbase = 2 + 2 * lookups.size();
  for (const LookupSpec & l : lookups) {
    put_u16(llist, static_cast<uint16_t>(lbase + ltabs.size()));
    std::vector<uint8_t> lt;
    put_u16(lt, l.type);
    put_u16(lt, l.flags);
    put_u16(lt, static_cast<uint16_t>(l.subtables.size()));
    size_t sbase = 6 + 2 * l.subtables.size();
    std::vector<uint8_t> body;
    for (const auto & s : l.subtables) {
      put_u16(lt, static_cast<uint16_t>(sbase + body.size()));
      body.insert(body.end(), s.begin(), s.end());
    }
    lt.insert(lt.end(), body.begin(), body.end());
    ltabs.insert(ltabs.end(), lt.begin(), lt.end());
  }
  llist.insert(llist.end(), ltabs.begin(), ltabs.end());

  std::vector<uint8_t> out;
  put_u32(out, variations.empty() ? 0x00010000u : 0x00010001u);
  put_u16(out, variations.empty() ? 10 : 14);
  put_u16(out, static_cast<uint16_t>((variations.empty() ? 10 : 14) + scripts.size()));
  put_u16(out, static_cast<uint16_t>((variations.empty() ? 10 : 14) + scripts.size() + flist.size()));
  size_t fv_at = (variations.empty() ? 10 : 14) + scripts.size() + flist.size() + llist.size();
  if (!variations.empty()) {
    put_u32(out, static_cast<uint32_t>(fv_at));
  }
  out.insert(out.end(), scripts.begin(), scripts.end());
  out.insert(out.end(), flist.begin(), flist.end());
  out.insert(out.end(), llist.begin(), llist.end());
  out.insert(out.end(), variations.begin(), variations.end());
  return out;
}

std::vector<uint8_t> coverage1(const std::vector<uint16_t> & glyphs) {
  std::vector<uint8_t> c;
  put_u16(c, 1);
  put_u16(c, static_cast<uint16_t>(glyphs.size()));
  for (uint16_t g : glyphs) {
    put_u16(c, g);
  }
  return c;
}

using Shaped = std::vector<std::array<int64_t, 6>>;

Shaped shape(const GFNT_Face * face, const std::vector<uint32_t> & text) {
  GFNT_ShapedRun run{};
  Shaped out;
  EXPECT_EQ(gfnt_face_shape(face, text.data(), text.size(), nullptr, nullptr, &run, nullptr), GFNT_OK);
  for (size_t i = 0; i < run.count; ++i) {
    const GFNT_ShapedGlyph & g = run.glyphs[i];
    out.push_back({g.glyph, g.cluster, g.x_advance, g.y_advance, g.x_offset, g.y_offset});
  }
  gfnt_shaped_run_free(&run);
  return out;
}

struct Subset {
  Font font;
  GFNT_SubsetMap map{};
  GFNT_Result result = GFNT_OK;
  GFNT_Error error{};
  ~Subset() { gfnt_subset_map_free(&map, nullptr); }
};

/** Subset @p src for @p chars, renumbered. */
void make_subset(Subset & out, const Font & src, std::vector<uint32_t> chars,
    bool drop_layout = false) {
  GFNT_SubsetOptions o;
  gfnt_subset_options_init(&o);
  o.codepoints = chars.data();
  o.codepoint_count = chars.size();
  o.drop_layout = drop_layout;
  o.map = &out.map;
  GFNT_Blob * blob = nullptr;
  out.result = gfnt_subset(src.face, &o, nullptr, nullptr, &blob, &out.error);
  if (out.result != GFNT_OK) {
    return;
  }
  out.font.blob = blob;
  ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &out.font.face, nullptr), GFNT_OK);
}

/** Shape @p text in the source and in the subset; every glyph must agree under the map. */
void expect_same(const Font & src, const Subset & sub, const std::vector<uint32_t> & text,
    const std::string & note) {
  const Shaped want = shape(src.face, text);
  const Shaped got = shape(sub.font.face, text);
  ASSERT_EQ(got.size(), want.size()) << note;
  for (size_t i = 0; i < want.size(); ++i) {
    ASSERT_LT(static_cast<size_t>(got[i][0]), sub.map.count) << note;
    EXPECT_EQ(static_cast<int64_t>(sub.map.old_of_new[got[i][0]]), want[i][0]) << note;
    for (size_t k = 1; k < 6; ++k) {
      EXPECT_EQ(got[i][k], want[i][k]) << note << " glyph " << i << " field " << k;
    }
  }
}

std::vector<uint8_t> table_of(const Font & f, const char * t) {
  size_t offset = 0, length = 0;
  EXPECT_EQ(gfnt_face_table_range(f.face, GFNT_TAG(t[0], t[1], t[2], t[3]), &offset, &length),
      GFNT_OK) << t;
  const uint8_t * p = gfnt_blob_data(f.blob);
  return std::vector<uint8_t>(p + offset, p + offset + length);
}

uint16_t u16_at(const std::vector<uint8_t> & v, size_t at) {
  return static_cast<uint16_t>((v[at] << 8) | v[at + 1]);
}

}  // namespace

// --- a subtable too big once its shared parts are written out ----------------

TEST(SubsetLayout, ASubtableThatDoesNotFitIsSplitAndShapesAsBefore) {
  // 300 first glyphs all point at one 200-pair PairSet, so the source is small and
  // the rewrite, which writes each set where it is used, is 240 KB.
  constexpr uint16_t kFirsts = 300;
  constexpr uint16_t kSeconds = 200;
  std::vector<uint8_t> set;
  put_u16(set, kSeconds);
  for (uint16_t k = 0; k < kSeconds; ++k) {
    put_u16(set, static_cast<uint16_t>(kFirsts + 1 + k));
    put_s16(set, static_cast<int16_t>(-10 - k));
  }
  std::vector<uint16_t> firsts;
  for (uint16_t g = 1; g <= kFirsts; ++g) {
    firsts.push_back(g);
  }
  std::vector<uint8_t> pair;
  put_u16(pair, 1);
  put_u16(pair, static_cast<uint16_t>(10 + 2 * kFirsts));
  put_u16(pair, 0x0004);   // XAdvance of the first glyph
  put_u16(pair, 0);
  put_u16(pair, kFirsts);
  const size_t set_at = 10 + 2 * kFirsts + 4 + 2 * kFirsts;
  for (uint16_t i = 0; i < kFirsts; ++i) {
    put_u16(pair, static_cast<uint16_t>(set_at));
  }
  const std::vector<uint8_t> cov = coverage1(firsts);
  pair.insert(pair.end(), cov.begin(), cov.end());
  ASSERT_EQ(pair.size(), set_at);
  pair.insert(pair.end(), set.begin(), set.end());

  const uint16_t glyphs = kFirsts + kSeconds + 1;
  Font src;
  ASSERT_EQ(src.open(glyph_font(glyphs, {{GFNT_TAG('G', 'P', 'O', 'S'),
      layout_table({{"kern", {0}}}, {{2, 0, {pair}}})}})), GFNT_OK);

  std::vector<uint32_t> chars;
  for (uint16_t g = 1; g < glyphs; ++g) {
    chars.push_back(kFirstChar + g - 1);
  }
  Subset sub;
  make_subset(sub, src, chars);
  ASSERT_EQ(sub.result, GFNT_OK) << (sub.error.message ? sub.error.message : "");
  const std::vector<uint8_t> gpos = table_of(sub.font, "GPOS");
  const size_t lookups = u16_at(gpos, 8);
  const size_t first_lookup = lookups + u16_at(gpos, lookups + 2);
  EXPECT_GE(u16_at(gpos, first_lookup + 4), 2u) << "the pair table was split";
  for (uint16_t a : {1, 2, 150, 299, 300}) {
    for (uint16_t b : {301, 302, 400, 500}) {
      expect_same(src, sub, {kFirstChar + a - 1u, kFirstChar + b - 1u},
          "pair " + std::to_string(a) + "," + std::to_string(b));
    }
  }
}

// --- FeatureVariations --------------------------------------------------------

namespace {

std::vector<uint8_t> single_subst(uint16_t from, uint16_t to) {
  std::vector<uint8_t> s;
  put_u16(s, 2);
  put_u16(s, 8);
  put_u16(s, 1);
  put_u16(s, to);
  const std::vector<uint8_t> cov = coverage1({from});
  s.insert(s.end(), cov.begin(), cov.end());
  return s;
}

/** FeatureVariations: one record whose condition set holds the given condition, swapping feature 0. */
std::vector<uint8_t> variations(const std::vector<uint8_t> & condition) {
  std::vector<uint8_t> v;
  put_u16(v, 1);
  put_u16(v, 0);
  put_u32(v, 1);
  put_u32(v, 16);                      // condition set
  put_u32(v, 16 + 6 + static_cast<uint32_t>(condition.size()));  // substitution
  put_u16(v, 1);                       // ConditionSet: one condition
  put_u32(v, 6);
  v.insert(v.end(), condition.begin(), condition.end());
  put_u16(v, 1);                       // FeatureTableSubstitution
  put_u16(v, 0);
  put_u16(v, 1);
  put_u16(v, 0);                       // feature 0
  put_u32(v, 12);
  put_u16(v, 0);                       // the alternate feature: one lookup, index 1
  put_u16(v, 1);
  put_u16(v, 1);
  return v;
}

std::vector<uint8_t> axis_condition() {
  std::vector<uint8_t> c;
  put_u16(c, 1);
  put_u16(c, 0);
  put_s16(c, -0x4000);
  put_s16(c, 0x4000);
  return c;
}

}  // namespace

TEST(SubsetLayout, FeatureVariationsAreCarriedWithTheirLookupsRenumbered) {
  // Lookup 0 turns glyph 1 into glyph 2; lookup 1 would turn it into glyph 3; lookup 2 is
  // never called. The variation swaps feature 'ccmp' to lookup 1.
  Font src;
  const auto gsub = layout_table({{"ccmp", {0}}},
      {{1, 0, {single_subst(1, 2)}}, {1, 0, {single_subst(1, 3)}}, {1, 0, {single_subst(2, 3)}}},
      variations(axis_condition()));
  ASSERT_EQ(src.open(glyph_font(5, {{GFNT_TAG('G', 'S', 'U', 'B'), gsub}})), GFNT_OK);
  Subset sub;
  make_subset(sub, src, {kFirstChar});
  ASSERT_EQ(sub.result, GFNT_OK) << (sub.error.message ? sub.error.message : "");
  const std::vector<uint8_t> out = table_of(sub.font, "GSUB");
  EXPECT_EQ((static_cast<uint32_t>(u16_at(out, 0)) << 16) | u16_at(out, 2), 0x00010001u);
  const std::vector<uint8_t> orig = table_of(src, "GSUB");
  EXPECT_NE(u16_at(out, 12) | u16_at(out, 10), 0) << "the FeatureVariations offset is set";
  EXPECT_LT(out.size(), orig.size() + 64);
  expect_same(src, sub, {kFirstChar}, "glyph 1 under the variation");
}

TEST(SubsetLayout, AConditionOfAnotherFormatIsRefusedAndDropLayoutSubsetsAnyway) {
  std::vector<uint8_t> weird;
  put_u16(weird, 3);     // a conjunction, which this does not rewrite
  weird.push_back(0);
  Font src;
  const auto gsub = layout_table({{"ccmp", {0}}},
      {{1, 0, {single_subst(1, 2)}}, {1, 0, {single_subst(1, 3)}}}, variations(weird));
  ASSERT_EQ(src.open(glyph_font(5, {{GFNT_TAG('G', 'S', 'U', 'B'), gsub}})), GFNT_OK);
  Subset refused;
  make_subset(refused, src, {kFirstChar});
  EXPECT_EQ(refused.result, GFNT_ERR_UNSUPPORTED);
  Subset kept;
  make_subset(kept, src, {kFirstChar}, true);
  EXPECT_EQ(kept.result, GFNT_OK);
  EXPECT_FALSE(gfnt_face_has_table(kept.font.face, GFNT_TAG('G', 'S', 'U', 'B')));
}

// --- kern ---------------------------------------------------------------------

namespace {

/** Format 0 pairs as the subtable body after its header. */
std::vector<uint8_t> kern0_body(const std::vector<std::array<int, 3>> & pairs) {
  std::vector<uint8_t> b;
  put_u16(b, static_cast<uint16_t>(pairs.size()));
  put_u16(b, 0);
  put_u16(b, 0);
  put_u16(b, 0);
  for (const auto & p : pairs) {
    put_u16(b, static_cast<uint16_t>(p[0]));
    put_u16(b, static_cast<uint16_t>(p[1]));
    put_s16(b, static_cast<int16_t>(p[2]));
  }
  return b;
}

/**
 * A format 2 body: left glyphs 1..3 in rows 0, 1, 2 and right glyphs 4..5 in columns 0, 1.
 * A left class value is the offset from the subtable's start to its row, a right one the
 * offset of its column in the row (HarfBuzz's reading, which the shaper now shares).
 */
std::vector<uint8_t> kern2_body(size_t header) {
  std::vector<uint8_t> b;
  const uint16_t row_width = 4;
  const size_t left_at = header + 8;
  const size_t right_at = left_at + 4 + 2 * 3;
  const size_t array_at = right_at + 4 + 2 * 2;
  put_u16(b, row_width);
  put_u16(b, static_cast<uint16_t>(left_at));
  put_u16(b, static_cast<uint16_t>(right_at));
  put_u16(b, static_cast<uint16_t>(array_at));
  put_u16(b, 1);
  put_u16(b, 3);
  for (uint16_t row : {0, 1, 2}) {
    put_u16(b, static_cast<uint16_t>(array_at + row * row_width));
  }
  put_u16(b, 4);
  put_u16(b, 2);
  for (uint16_t col : {0, 1}) {
    put_u16(b, static_cast<uint16_t>(col * 2));
  }
  const int16_t values[3][2] = {{-20, -40}, {-60, -80}, {-100, -120}};
  for (const auto & row : values) {
    for (int16_t v : row) {
      put_s16(b, v);
    }
  }
  return b;
}

std::vector<uint8_t> kern_table(bool apple, const std::vector<std::pair<int, std::vector<uint8_t>>> & subs) {
  std::vector<uint8_t> t;
  if (apple) {
    put_u16(t, 1);
    put_u16(t, 0);
    put_u32(t, static_cast<uint32_t>(subs.size()));
  }
  else {
    put_u16(t, 0);
    put_u16(t, static_cast<uint16_t>(subs.size()));
  }
  for (const auto & s : subs) {
    const size_t header = apple ? 8 : 6;
    if (apple) {
      put_u32(t, static_cast<uint32_t>(header + s.second.size()));
      put_u16(t, static_cast<uint16_t>(s.first));
      put_u16(t, 0);
    }
    else {
      put_u16(t, 0);
      put_u16(t, static_cast<uint16_t>(header + s.second.size()));
      put_u16(t, static_cast<uint16_t>(1 | (s.first << 8)));
    }
    t.insert(t.end(), s.second.begin(), s.second.end());
  }
  return t;
}

void check_kern(bool apple, bool format2) {
  const auto body = format2 ? kern2_body(apple ? 8 : 6)
      : kern0_body({{1, 4, -30}, {1, 5, -31}, {2, 4, -32}, {2, 5, -34}, {3, 5, -33}});
  const auto kern = kern_table(apple, {{format2 ? 2 : 0, body}});
  Font src;
  ASSERT_EQ(src.open(glyph_font(8, {{GFNT_TAG('k', 'e', 'r', 'n'), kern}})), GFNT_OK);
  // Keep glyphs 1, 2, 4 and 5, and 6 and 7, which the class tables of format 2 do not
  // list; glyph 3 goes.
  Subset sub;
  make_subset(sub, src, {kFirstChar, kFirstChar + 1, kFirstChar + 3, kFirstChar + 4,
      kFirstChar + 5, kFirstChar + 6});
  ASSERT_EQ(sub.result, GFNT_OK) << (sub.error.message ? sub.error.message : "");
  ASSERT_TRUE(gfnt_face_has_table(sub.font.face, GFNT_TAG('k', 'e', 'r', 'n')));
  size_t distinct = 0;
  for (uint32_t a : {1u, 2u, 6u}) {
    for (uint32_t b : {4u, 5u, 7u}) {
      expect_same(src, sub, {kFirstChar + a - 1, kFirstChar + b - 1},
          std::string(apple ? "apple " : "ot ") + (format2 ? "f2 " : "f0 ") + std::to_string(a)
          + "," + std::to_string(b));
      const Shaped s = shape(src.face, {kFirstChar + a - 1, kFirstChar + b - 1});
      if (s.size() == 2 && s[0][2] != 500) {
        ++distinct;
      }
    }
  }
  EXPECT_GT(distinct, 0u) << "the source kerns these pairs, so the comparison means something";
  EXPECT_LT(table_of(sub.font, "kern").size(), kern.size());
}

}  // namespace

TEST(SubsetLayout, KernFormat0InTheMicrosoftHeaderFollowsTheNumbering) { check_kern(false, false); }
TEST(SubsetLayout, KernFormat0InAppleHeaderFollowsTheNumbering) { check_kern(true, false); }
TEST(SubsetLayout, KernFormat2InTheMicrosoftHeaderFollowsTheNumbering) { check_kern(false, true); }
TEST(SubsetLayout, KernFormat2InAppleHeaderFollowsTheNumbering) { check_kern(true, true); }

TEST(SubsetLayout, AKernSubtableOfAnotherFormatIsRefused) {
  const auto kern = kern_table(false, {{1, kern0_body({{1, 4, -30}})}});
  Font src;
  ASSERT_EQ(src.open(glyph_font(8, {{GFNT_TAG('k', 'e', 'r', 'n'), kern}})), GFNT_OK);
  Subset refused;
  make_subset(refused, src, {kFirstChar, kFirstChar + 3});
  EXPECT_EQ(refused.result, GFNT_ERR_UNSUPPORTED);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
