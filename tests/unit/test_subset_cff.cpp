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
 * The subsetter and `CFF ` outlines: the kept glyphs keep their drawing and their
 * advance, subroutines are flattened away, an accented character brings its parts,
 * a CID-keyed font keeps only the Font DICTs in use. `make check-oracle-subset`
 * reads the same subsets through fontTools and HarfBuzz over every CFF font in the
 * oracle image. Then the variable fonts: CFF2's blend, the tables around it, and
 * `gvar` and `cvar` of a TrueType one.
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/charstring.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/variation.h>
#include <ghoti.io/font/write.h>

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
  void open() {
    ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED,
                  nullptr, nullptr, &blob, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr), GFNT_OK);
  }
  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;
  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  size_t glyphs() const {
    size_t n = 0;
    EXPECT_EQ(gfnt_face_num_glyphs(face, &n, nullptr), GFNT_OK);
    return n;
  }
};

struct Subset {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_SubsetMap map{};
  GFNT_Result result = GFNT_OK;
  GFNT_Error error;
  size_t size = 0;

  Subset(const GFNT_Face * source, std::vector<uint32_t> glyphs, bool retain = false,
      bool drop_hinting = false) {
    GFNT_SubsetOptions o;
    gfnt_subset_options_init(&o);
    o.glyphs = glyphs.data();
    o.glyph_count = glyphs.size();
    o.retain_gids = retain;
    o.drop_hinting = drop_hinting;
    o.map = &map;
    result = gfnt_subset(source, &o, nullptr, nullptr, &blob, &error);
    if (result == GFNT_OK) {
      size = gfnt_blob_size(blob);
      EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr), GFNT_OK);
    }
  }
  Subset(const Subset &) = delete;
  Subset & operator=(const Subset &) = delete;
  ~Subset() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    gfnt_subset_map_free(&map, nullptr);
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

std::vector<uint32_t> all_glyphs(size_t n) {
  std::vector<uint32_t> g;
  for (uint32_t i = 0; i < n; ++i) {
    g.push_back(i);
  }
  return g;
}

/** Every point of a glyph's outline, flattened with its tag. */
std::vector<int64_t> outline_of(const GFNT_Face * face, uint32_t glyph) {
  std::vector<int64_t> out;
  GFNT_Outline * outline = nullptr;
  if (gfnt_face_glyph_outline(face, glyph, nullptr, nullptr, &outline, nullptr) != GFNT_OK) {
    out.push_back(-1);
    return out;
  }
  const size_t n = gfnt_outline_point_count(outline);
  for (size_t i = 0; i < n; ++i) {
    GFNT_Point p;
    GFNT_PointTag tag;
    EXPECT_EQ(gfnt_outline_point_at(outline, i, &p, &tag), GFNT_OK);
    out.push_back(p.x);
    out.push_back(p.y);
    out.push_back(static_cast<int64_t>(tag));
  }
  out.push_back(static_cast<int64_t>(gfnt_outline_contour_count(outline)));
  gfnt_outline_destroy(outline);
  return out;
}

GFNT_CharstringMetrics metrics_of(const GFNT_Face * face, uint32_t glyph) {
  GFNT_CharstringMetrics m{};
  EXPECT_EQ(gfnt_face_glyph_charstring_metrics(face, glyph, &m, nullptr), GFNT_OK);
  return m;
}

/** The program of a glyph's charstring, one operator a line. */
std::string program_of(const GFNT_Face * face, uint32_t glyph) {
  GFNT_CharstringType type;
  const uint8_t * bytes = nullptr;
  size_t length = 0;
  EXPECT_EQ(gfnt_face_glyph_charstring(face, glyph, &type, &bytes, &length, nullptr), GFNT_OK);
  const GFNT_CharstringMetrics m = metrics_of(face, glyph);
  FILE * f = std::tmpfile();
  EXPECT_NE(f, nullptr);
  EXPECT_EQ(gfnt_charstring_dump(type, bytes, length, &m, f), GFNT_OK);
  const long end = std::ftell(f);
  std::string text(static_cast<size_t>(end), '\0');
  std::rewind(f);
  EXPECT_EQ(std::fread(text.data(), 1, text.size(), f), text.size());
  std::fclose(f);
  return text;
}

bool has_table(const GFNT_Face * face, const char * tag) {
  return gfnt_face_has_table(face, GFNT_TAG(tag[0], tag[1], tag[2], tag[3]));
}

/** One subset glyph is the source glyph it stands for: the drawing, the widths. */
void expect_same_glyph(const GFNT_Face * source, uint32_t old, const GFNT_Face * sub,
    uint32_t now) {
  SCOPED_TRACE(old);
  EXPECT_EQ(outline_of(sub, now), outline_of(source, old));
  const GFNT_CharstringMetrics a = metrics_of(source, old);
  const GFNT_CharstringMetrics b = metrics_of(sub, now);
  EXPECT_EQ(b.width, a.width);
  EXPECT_EQ(b.width_stated, a.width_stated);
  EXPECT_EQ(b.seac, a.seac);
  int32_t adv_a = -1;
  int32_t adv_b = -1;
  EXPECT_EQ(gfnt_face_glyph_advance(source, old, nullptr, &adv_a, nullptr), GFNT_OK);
  EXPECT_EQ(gfnt_face_glyph_advance(sub, now, nullptr, &adv_b, nullptr), GFNT_OK);
  EXPECT_EQ(adv_b, adv_a);
  // And its name, or the same reason for having none.
  char * name_a = nullptr;
  char * name_b = nullptr;
  size_t len = 0;
  const GFNT_Result ra = gfnt_face_glyph_name(source, old, nullptr, &name_a, &len, nullptr);
  const GFNT_Result rb = gfnt_face_glyph_name(sub, now, nullptr, &name_b, &len, nullptr);
  EXPECT_EQ(rb, ra);
  if (ra == GFNT_OK && rb == GFNT_OK) {
    EXPECT_STREQ(name_b, name_a);
  }
  if (ra == GFNT_OK) {
    gfnt_glyph_name_free(nullptr, name_a);
  }
  if (rb == GFNT_OK) {
    gfnt_glyph_name_free(nullptr, name_b);
  }
}

const char * const kFonts[] = {"cff.otf", "cff-subrs.otf", "cff-cid.otf", "cff-hints.otf",
    "cff-seac.otf", "cff-arith.otf", "cff-curves.otf", "cff-cid3.otf", "cff-subset.otf"};

}  // namespace

TEST(SubsetCff, EveryGlyphKeepsItsDrawingAndItsWidth) {
  for (const char * name : kFonts) {
    for (bool retain : {false, true}) {
      SCOPED_TRACE(std::string(name) + (retain ? " retained" : " renumbered"));
      Font source(name);
      Subset sub(source.face, all_glyphs(source.glyphs()), retain);
      ASSERT_EQ(sub.result, GFNT_OK);
      EXPECT_TRUE(has_table(sub.face, "CFF "));
      EXPECT_FALSE(has_table(sub.face, "glyf"));
      EXPECT_FALSE(has_table(sub.face, "loca"));
      ASSERT_EQ(sub.map.count, source.glyphs());
      for (uint32_t old = 0; old < source.glyphs(); ++old) {
        const long now = sub.new_of(old);
        ASSERT_GE(now, 0);
        EXPECT_EQ(static_cast<uint32_t>(now), old) << "all kept: the numbering is the same";
        expect_same_glyph(source.face, old, sub.face, static_cast<uint32_t>(now));
      }
    }
  }
}

TEST(SubsetCff, ASubsetOfSomeGlyphsRenumbersAndIsSmaller) {
  for (const char * name : kFonts) {
    SCOPED_TRACE(name);
    Font source(name);
    const size_t n = source.glyphs();
    ASSERT_GE(n, 3u);
    Subset sub(source.face, {static_cast<uint32_t>(n - 1)});
    ASSERT_EQ(sub.result, GFNT_OK);
    EXPECT_LT(sub.size, source.bytes.size());
    ASSERT_GE(sub.map.count, 2u);
    EXPECT_EQ(sub.map.old_of_new[0], 0u);
    for (size_t now = 0; now < sub.map.count; ++now) {
      expect_same_glyph(source.face, sub.map.old_of_new[now], sub.face,
          static_cast<uint32_t>(now));
    }
    EXPECT_LT(sub.new_of(static_cast<uint32_t>(n - 1)), static_cast<long>(sub.map.count));
  }
}

TEST(SubsetCff, RetainedIdsLeaveTheOtherGlyphsEmpty) {
  Font source("cff-curves.otf");
  const size_t n = source.glyphs();
  ASSERT_GE(n, 4u);
  Subset sub(source.face, {static_cast<uint32_t>(n - 1)}, true);
  ASSERT_EQ(sub.result, GFNT_OK);
  ASSERT_EQ(sub.map.count, n);
  for (uint32_t g = 0; g < n; ++g) {
    if (g == 0 || g == n - 1) {
      expect_same_glyph(source.face, g, sub.face, g);
    }
    else {
      EXPECT_EQ(sub.map.old_of_new[g], 0xFFFFFFFFu) << g;
      EXPECT_EQ(outline_of(sub.face, g).back(), 0) << "no contours: " << g;
    }
  }
}

TEST(SubsetCff, ADroppedGlyphAheadOfAKeptOneRenumbersItsName) {
  Font source("cff-subset.otf");
  Subset sub(source.face, {6});
  ASSERT_EQ(sub.result, GFNT_OK);
  const long now = sub.new_of(6);
  ASSERT_GE(now, 1);
  char * name = nullptr;
  size_t len = 0;
  ASSERT_EQ(gfnt_face_glyph_name(sub.face, static_cast<uint32_t>(now), nullptr, &name, &len,
                nullptr), GFNT_OK);
  EXPECT_STREQ(name, "gamma.sub");
  gfnt_glyph_name_free(nullptr, name);
}

TEST(SubsetCff, ADroppedFontDictShiftsTheOnesAfterIt) {
  Font source("cff-cid3.otf");
  Subset sub(source.face, {3});
  ASSERT_EQ(sub.result, GFNT_OK);
  ASSERT_EQ(sub.map.count, 2u);
  const GFNT_CharstringMetrics a = metrics_of(source.face, 3);
  const GFNT_CharstringMetrics b = metrics_of(sub.face, 1);
  EXPECT_FALSE(a.width_stated);
  EXPECT_EQ(b.width, a.width) << "the default width of the third Font DICT, now the second";
  EXPECT_EQ(a.width, 350 * 65536);
}

TEST(SubsetCff, ARetainedGapIsAnEmptyCharstring) {
  Font source("cff-subset.otf");
  Subset sub(source.face, {6}, true);
  ASSERT_EQ(sub.result, GFNT_OK);
  GFNT_CharstringType type;
  const uint8_t * bytes = nullptr;
  size_t length = 0;
  ASSERT_EQ(gfnt_face_glyph_charstring(sub.face, 3, &type, &bytes, &length, nullptr), GFNT_OK);
  ASSERT_EQ(length, 1u);
  EXPECT_EQ(bytes[0], 14) << "endchar";
}

TEST(SubsetCff, SubroutinesAreFlattenedAway) {
  Font source("cff-subrs.otf");
  bool source_calls = false;
  for (uint32_t g = 0; g < source.glyphs(); ++g) {
    const std::string p = program_of(source.face, g);
    source_calls = source_calls || p.find("callsubr") != std::string::npos
        || p.find("callgsubr") != std::string::npos;
  }
  ASSERT_TRUE(source_calls) << "the fixture calls subroutines, or this proves nothing";
  Subset sub(source.face, all_glyphs(source.glyphs()));
  ASSERT_EQ(sub.result, GFNT_OK);
  for (uint32_t g = 0; g < sub.map.count; ++g) {
    const std::string p = program_of(sub.face, g);
    EXPECT_EQ(p.find("callsubr"), std::string::npos) << g << "\n" << p;
    EXPECT_EQ(p.find("callgsubr"), std::string::npos) << g << "\n" << p;
    EXPECT_EQ(p.find("return"), std::string::npos) << g << "\n" << p;
  }
}

TEST(SubsetCff, AnAccentedCharacterBringsItsParts) {
  Font source("cff-seac.otf");
  uint32_t accented = 0xFFFFFFFFu;
  for (uint32_t g = 0; g < source.glyphs(); ++g) {
    if (metrics_of(source.face, g).seac) {
      accented = g;
    }
  }
  ASSERT_NE(accented, 0xFFFFFFFFu);
  Subset sub(source.face, {accented});
  ASSERT_EQ(sub.result, GFNT_OK);
  EXPECT_EQ(sub.map.count, 4u) << ".notdef, the accented glyph, its base and its accent";
  const long now = sub.new_of(accented);
  ASSERT_GE(now, 1);
  expect_same_glyph(source.face, accented, sub.face, static_cast<uint32_t>(now));
  EXPECT_TRUE(metrics_of(sub.face, static_cast<uint32_t>(now)).seac);
  // Whatever the glyph draws, its parts were kept to draw it with.
  EXPECT_FALSE(outline_of(sub.face, static_cast<uint32_t>(now)).empty());
  EXPECT_GT(outline_of(sub.face, static_cast<uint32_t>(now)).size(), 1u);
}

TEST(SubsetCff, ACidFontKeepsTheFontDictsInUse) {
  Font source("cff-cid.otf");
  const size_t n = source.glyphs();
  ASSERT_GE(n, 3u);
  size_t smallest = static_cast<size_t>(-1);
  for (uint32_t g = 1; g < n; ++g) {
    Subset one(source.face, {g});
    ASSERT_EQ(one.result, GFNT_OK) << g;
    ASSERT_GE(one.new_of(g), 1);
    expect_same_glyph(source.face, g, one.face, static_cast<uint32_t>(one.new_of(g)));
    smallest = std::min(smallest, one.size);
  }
  Subset all(source.face, all_glyphs(n));
  ASSERT_EQ(all.result, GFNT_OK);
  EXPECT_LT(smallest, all.size) << "a font dict that nothing kept uses is gone";
}

TEST(SubsetCff, DroppingHintingKeepsTheDrawingAndTheWidthAndRemovesTheHints) {
  for (const char * name : {"cff-hints.otf", "cff-subset.otf"}) {
    SCOPED_TRACE(name);
    Font source(name);
    bool source_hinted = false;
    for (uint32_t g = 0; g < source.glyphs(); ++g) {
      const std::string p = program_of(source.face, g);
      source_hinted = source_hinted || p.find("hstem") != std::string::npos
          || p.find("vstem") != std::string::npos || p.find("hintmask") != std::string::npos
          || p.find("cntrmask") != std::string::npos;
    }
    ASSERT_TRUE(source_hinted);
    Subset sub(source.face, all_glyphs(source.glyphs()), false, true);
    ASSERT_EQ(sub.result, GFNT_OK);
    EXPECT_LT(sub.size, source.bytes.size());
    for (uint32_t g = 0; g < sub.map.count; ++g) {
      expect_same_glyph(source.face, g, sub.face, g);
      const std::string p = program_of(sub.face, g);
      for (const char * hint : {"hstem", "vstem", "hintmask", "cntrmask"}) {
        EXPECT_EQ(p.find(hint), std::string::npos) << g << " " << hint << "\n" << p;
      }
    }
  }
}

TEST(SubsetCff, ArithmeticOnCoordinatesSurvivesFlattening) {
  // cff-arith computes the coordinates it draws with, so its operands cannot be held back.
  Font ok("cff-arith.otf");
  Subset sub(ok.face, all_glyphs(ok.glyphs()));
  EXPECT_EQ(sub.result, GFNT_OK);
  for (uint32_t g = 0; g < sub.map.count; ++g) {
    expect_same_glyph(ok.face, g, sub.face, g);
  }
}

TEST(SubsetCff, TypeOneCharstringsInACffAreRefusedByName) {
  Font source("cff-type1.otf");
  Subset sub(source.face, {1});
  EXPECT_EQ(sub.result, GFNT_ERR_UNSUPPORTED);
}

TEST(SubsetCff, EveryAllocationFailureIsSurvived) {
  for (const char * name : {"cff-subrs.otf", "cff-cid.otf", "cff-seac.otf"}) {
    SCOPED_TRACE(name);
    Font source(name);
    std::vector<uint32_t> glyphs = all_glyphs(source.glyphs());
    GFNT_SubsetOptions o;
    gfnt_subset_options_init(&o);
    o.glyphs = glyphs.data();
    o.glyph_count = glyphs.size();
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
}

// --- CFF2 and the variation tables ------------------------------------------

namespace {

struct Place {
  GFNT_F16Dot16 wght;
  GFNT_F16Dot16 wdth;
};

constexpr GFNT_F16Dot16 fx(int v) { return v * 65536; }

const Place kPlaces[] = {{fx(100), fx(75)}, {fx(900), fx(125)}, {fx(700), fx(100)},
    {fx(400), fx(125)}, {fx(900), fx(75)}, {fx(250), fx(90)}, {fx(400), fx(100)}};

/** The outline and advance of a glyph at a place, flattened into numbers. */
std::vector<int64_t> at_place(const GFNT_Face * face, uint32_t glyph, const Place & p) {
  const GFNT_F16Dot16 user[2] = {p.wght, p.wdth};
  GFNT_F2Dot14 norm[2] = {0, 0};
  EXPECT_EQ(gfnt_face_normalize(face, user, 2, norm, 2, nullptr), GFNT_OK);
  GFNT_Variation v{};
  v.coords = norm;
  v.count = 2;
  std::vector<int64_t> out;
  GFNT_Outline * outline = nullptr;
  EXPECT_EQ(gfnt_face_glyph_outline(face, glyph, &v, nullptr, &outline, nullptr), GFNT_OK);
  if (outline) {
    const size_t n = gfnt_outline_point_count(outline);
    for (size_t i = 0; i < n; ++i) {
      GFNT_Point pt;
      GFNT_PointTag tag;
      EXPECT_EQ(gfnt_outline_point_at(outline, i, &pt, &tag), GFNT_OK);
      out.push_back(pt.x);
      out.push_back(pt.y);
    }
    gfnt_outline_destroy(outline);
  }
  int32_t advance = -1;
  EXPECT_EQ(gfnt_face_glyph_advance(face, glyph, &v, &advance, nullptr), GFNT_OK);
  out.push_back(advance);
  return out;
}

std::vector<uint8_t> table_of(const GFNT_Face * face, const char * tag) {
  const uint8_t * data = nullptr;
  size_t length = 0;
  if (gfnt_face_table_data(face, GFNT_TAG(tag[0], tag[1], tag[2], tag[3]), &data, &length)
      != GFNT_OK) {
    return {};
  }
  return std::vector<uint8_t>(data, data + length);
}

const char * const kVariable[] = {"variable-cff2.otf", "variable-cff2-hvar.otf",
    "variable-cff2-implicit.otf"};

}  // namespace

TEST(SubsetCff2, EveryKeptGlyphDrawsAndAdvancesAsItDidAtEveryPlace) {
  for (const char * name : kVariable) {
    for (bool retain : {false, true}) {
     for (bool unhinted : {false, true}) {
      for (std::vector<uint32_t> keep : {std::vector<uint32_t>{}, std::vector<uint32_t>{4, 5},
               std::vector<uint32_t>{7}, std::vector<uint32_t>{3, 6, 7},
               std::vector<uint32_t>{8}, std::vector<uint32_t>{9}}) {
        SCOPED_TRACE(std::string(name) + (retain ? " retained " : " renumbered ")
            + (unhinted ? "unhinted " : "") + std::to_string(keep.size()) + " glyphs");
        Font source(name);
        if (keep.empty()) {
          keep = all_glyphs(source.glyphs());
        }
        if (keep[0] >= source.glyphs()) {
          continue;     // the fixture without the eight-stem glyph
        }
        Subset sub(source.face, keep, retain, unhinted);
        ASSERT_EQ(sub.result, GFNT_OK) << sub.error.message;
        EXPECT_TRUE(has_table(sub.face, "CFF2"));
        EXPECT_FALSE(has_table(sub.face, "CFF "));
        EXPECT_FALSE(has_table(sub.face, "glyf"));
        EXPECT_EQ(table_of(sub.face, "fvar"), table_of(source.face, "fvar"));
        EXPECT_EQ(has_table(sub.face, "HVAR"), has_table(source.face, "HVAR"));
        EXPECT_EQ(has_table(sub.face, "VVAR"), has_table(source.face, "VVAR"));
        EXPECT_EQ(table_of(sub.face, "MVAR"), table_of(source.face, "MVAR"));
        ASSERT_GE(sub.map.count, keep.size() > 0 ? 2u : 1u);
        for (size_t now = 0; now < sub.map.count; ++now) {
          const uint32_t old = sub.map.old_of_new[now];
          if (old == 0xFFFFFFFFu) {
            continue;
          }
          for (const Place & p : kPlaces) {
            EXPECT_EQ(at_place(sub.face, static_cast<uint32_t>(now), p),
                at_place(source.face, old, p))
                << "glyph " << old << " at " << p.wght / 65536 << "," << p.wdth / 65536;
          }
        }
      }
     }
    }
  }
}

TEST(SubsetCff2, TheItemVariationStoreIsCarriedWhole) {
  Font source("variable-cff2-hvar.otf");
  Subset sub(source.face, {4, 7});
  ASSERT_EQ(sub.result, GFNT_OK);
  for (const char * tag : {"HVAR", "VVAR"}) {
    SCOPED_TRACE(tag);
    const auto a = table_of(source.face, tag);
    const auto b = table_of(sub.face, tag);
    ASSERT_GT(a.size(), 24u);
    ASSERT_GT(b.size(), 24u);
    auto u32 = [](const std::vector<uint8_t> & t, size_t at) {
      return static_cast<size_t>((t[at] << 24) | (t[at + 1] << 16) | (t[at + 2] << 8) | t[at + 3]);
    };
    const size_t start = u32(a, 4);
    // The store's extent by the format's own layout: the region list, then every
    // ItemVariationData with its rows.
    const uint8_t * p = a.data() + start;
    auto u16 = [&](size_t at) { return static_cast<size_t>((p[at] << 8) | p[at + 1]); };
    auto w32 = [&](size_t at) {
      return static_cast<size_t>((p[at] << 24) | (p[at + 1] << 16) | (p[at + 2] << 8) | p[at + 3]);
    };
    size_t end = w32(2) + 4 + u16(w32(2)) * u16(w32(2) + 2) * 6;
    for (size_t d = 0; d < u16(6); ++d) {
      const size_t at = w32(8 + 4 * d);
      const size_t items = u16(at);
      size_t words = u16(at + 2);
      const size_t regions = u16(at + 4);
      size_t row;
      if (words & 0x8000u) {
        words &= 0x7FFFu;
        row = words * 4 + (regions - words) * 2;
      }
      else {
        row = words * 2 + (regions - words);
      }
      end = std::max(end, at + 6 + 2 * regions + items * row);
    }
    const std::vector<uint8_t> store(a.begin() + start, a.begin() + start + end);
    EXPECT_NE(std::search(b.begin(), b.end(), store.begin(), store.end()), b.end());
  }
}

TEST(SubsetCff2, ARenumberedSubsetStatesTheAdvanceMapTheFontLeftToTheGlyphId) {
  Font source("variable-cff2-implicit.otf");
  Subset sub(source.face, {6, 2});
  ASSERT_EQ(sub.result, GFNT_OK);
  const auto hvar = table_of(sub.face, "HVAR");
  ASSERT_GT(hvar.size(), 20u);
  const uint32_t advance_map = (hvar[8] << 24) | (hvar[9] << 16) | (hvar[10] << 8) | hvar[11];
  EXPECT_NE(advance_map, 0u) << "the glyph id no longer names the row";
  // The rows are the source's: the advance of the glyph that was 6 differs from the one
  // that was 2 at the heavy end, as it did.
  EXPECT_NE(at_place(sub.face, static_cast<uint32_t>(sub.new_of(6)), kPlaces[1]).back(),
      at_place(sub.face, static_cast<uint32_t>(sub.new_of(2)), kPlaces[1]).back());
}

TEST(SubsetCff2, VariablePositioningKeepsItsDeviceTablesAndTheStoreTheyIndex) {
  Font source("variable-cff2-hvar.otf");
  Subset sub(source.face, all_glyphs(source.glyphs()));
  ASSERT_EQ(sub.result, GFNT_OK);
  const auto gdef = table_of(sub.face, "GDEF");
  ASSERT_GE(gdef.size(), 18u);
  EXPECT_EQ(gdef[3], 3) << "GDEF 1.3: it carries an item variation store";
  const uint32_t store = (gdef[14] << 24) | (gdef[15] << 16) | (gdef[16] << 8) | gdef[17];
  EXPECT_GE(store, 18u) << "past a header that has room for the store's offset";
  const auto src_gdef = table_of(source.face, "GDEF");
  const uint32_t src_store = (src_gdef[14] << 24) | (src_gdef[15] << 16) | (src_gdef[16] << 8)
      | src_gdef[17];
  ASSERT_LT(src_store, src_gdef.size());
  ASSERT_LT(store, gdef.size());
  const size_t span = src_gdef.size() - src_store;
  ASSERT_GE(gdef.size() - store, span);
  EXPECT_TRUE(std::equal(src_gdef.begin() + src_store, src_gdef.end(), gdef.begin() + store));
  const auto gpos = table_of(sub.face, "GPOS");
  ASSERT_FALSE(gpos.empty());
  // Both adjustments still carry their variation index (start, end, format 0x8000): the
  // pair's counts its offset from its PairSet, the single's from its subtable.
  const std::vector<uint8_t> pair_index = {0, 0, 0, 0, 0x80, 0x00};
  const std::vector<uint8_t> single_index = {0, 0, 0, 1, 0x80, 0x00};
  EXPECT_NE(std::search(gpos.begin(), gpos.end(), pair_index.begin(), pair_index.end()),
      gpos.end());
  EXPECT_NE(std::search(gpos.begin(), gpos.end(), single_index.begin(), single_index.end()),
      gpos.end());
}

TEST(SubsetCff2, EveryAllocationFailureIsSurvived) {
  Font source("variable-cff2-hvar.otf");
  std::vector<uint32_t> glyphs = {4, 5, 7};
  GFNT_SubsetOptions o;
  gfnt_subset_options_init(&o);
  o.glyphs = glyphs.data();
  o.glyph_count = glyphs.size();
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

// --- gvar and cvar ----------------------------------------------------------

namespace {

const char * const kGvarFonts[] = {"variable-gvar.ttf", "variable-hvar.ttf",
    "variable-cvar.ttf", "variable-avar2.ttf", "variable-stat.ttf",
    "variable-featurevars.ttf"};

uint32_t be16at(const std::vector<uint8_t> & t, size_t at) {
  return (static_cast<uint32_t>(t[at]) << 8) | t[at + 1];
}

}  // namespace

TEST(SubsetGvar, EveryKeptGlyphDrawsAndAdvancesAsItDidAtEveryPlace) {
  for (const char * name : kGvarFonts) {
    for (bool retain : {false, true}) {
      for (bool unhinted : {false, true}) {
        for (std::vector<uint32_t> keep : {std::vector<uint32_t>{}, std::vector<uint32_t>{4},
                 std::vector<uint32_t>{6}, std::vector<uint32_t>{7},
                 std::vector<uint32_t>{3, 5}}) {
          SCOPED_TRACE(std::string(name) + (retain ? " retained " : " renumbered ")
              + (unhinted ? "unhinted " : "") + std::to_string(keep.size()) + " glyphs");
          Font source(name);
          if (keep.empty()) {
            keep = all_glyphs(source.glyphs());
          }
          Subset sub(source.face, keep, retain, unhinted);
          ASSERT_EQ(sub.result, GFNT_OK) << sub.error.message;
          EXPECT_TRUE(has_table(sub.face, "gvar"));
          EXPECT_TRUE(has_table(sub.face, "glyf"));
          EXPECT_EQ(table_of(sub.face, "fvar"), table_of(source.face, "fvar"));
          EXPECT_EQ(table_of(sub.face, "avar"), table_of(source.face, "avar"));
          EXPECT_EQ(table_of(sub.face, "MVAR"), table_of(source.face, "MVAR"));
          EXPECT_EQ(has_table(sub.face, "HVAR"), has_table(source.face, "HVAR"));
          for (size_t now = 0; now < sub.map.count; ++now) {
            const uint32_t old = sub.map.old_of_new[now];
            if (old == 0xFFFFFFFFu) {
              continue;
            }
            for (const Place & p : kPlaces) {
              EXPECT_EQ(at_place(sub.face, static_cast<uint32_t>(now), p),
                  at_place(source.face, old, p))
                  << "glyph " << old << " at " << p.wght / 65536 << "," << p.wdth / 65536;
            }
          }
        }
      }
    }
  }
}

TEST(SubsetGvar, TheGlyphsAreMovedForRealAtTheseVariationPlaces) {
  // The control for the test above: a font whose glyphs did not vary would pass it.
  Font source("variable-gvar.ttf");
  bool varied = false;
  for (uint32_t g = 0; g < source.glyphs(); ++g) {
    varied = varied || at_place(source.face, g, kPlaces[0]) != at_place(source.face, g, kPlaces[1]);
  }
  EXPECT_TRUE(varied);
}

TEST(SubsetGvar, TheTableHasOneEntryForEachKeptGlyphAndNoDataForADroppedOne) {
  Font source("variable-gvar.ttf");
  const std::vector<uint8_t> full = table_of(source.face, "gvar");
  Subset one(source.face, {4});
  ASSERT_EQ(one.result, GFNT_OK);
  const std::vector<uint8_t> small = table_of(one.face, "gvar");
  ASSERT_GT(small.size(), 20u);
  EXPECT_EQ(be16at(small, 12), one.map.count);
  EXPECT_LT(small.size(), full.size());
  // The shared tuples are kept whole.
  EXPECT_EQ(be16at(small, 6), be16at(full, 6));

  Subset retained(source.face, {4}, true);
  ASSERT_EQ(retained.result, GFNT_OK);
  const std::vector<uint8_t> kept = table_of(retained.face, "gvar");
  // Ids are kept up to the highest glyph kept, and the ones between are empty.
  EXPECT_EQ(be16at(kept, 12), 5u);
  EXPECT_LT(kept.size(), full.size());
}

TEST(SubsetGvar, CvarGoesWithTheControlValuesItVaries) {
  Font source("variable-cvar.ttf");
  ASSERT_TRUE(has_table(source.face, "cvar"));
  ASSERT_TRUE(has_table(source.face, "cvt "));
  Subset hinted(source.face, {1, 2});
  ASSERT_EQ(hinted.result, GFNT_OK);
  EXPECT_EQ(table_of(hinted.face, "cvar"), table_of(source.face, "cvar"));
  EXPECT_EQ(table_of(hinted.face, "cvt "), table_of(source.face, "cvt "));
  Subset unhinted(source.face, {1, 2}, false, true);
  ASSERT_EQ(unhinted.result, GFNT_OK);
  EXPECT_FALSE(has_table(unhinted.face, "cvar"));
  EXPECT_FALSE(has_table(unhinted.face, "cvt "));
}

TEST(SubsetGvar, ACompositeBringsItsPartsAndTheirVariations) {
  Font source("variable-gvar.ttf");
  // acc2 is a composite of the composite acc, which is made of dot.
  Subset sub(source.face, {7});
  ASSERT_EQ(sub.result, GFNT_OK);
  EXPECT_GE(sub.map.count, 3u);
  for (size_t now = 0; now < sub.map.count; ++now) {
    for (const Place & p : kPlaces) {
      EXPECT_EQ(at_place(sub.face, static_cast<uint32_t>(now), p),
          at_place(source.face, sub.map.old_of_new[now], p));
    }
  }
}

TEST(SubsetGvar, EveryAllocationFailureIsSurvived) {
  Font source("variable-hvar.ttf");
  std::vector<uint32_t> glyphs = {4, 6};
  GFNT_SubsetOptions o;
  gfnt_subset_options_init(&o);
  o.glyphs = glyphs.data();
  o.glyph_count = glyphs.size();
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
