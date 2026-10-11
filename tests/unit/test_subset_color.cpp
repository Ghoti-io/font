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
 * The subsetter and color. A subset of a color font keeps the color: the kept base
 * glyphs, the glyphs they paint with (a PaintColrGlyph's target is kept as a base
 * glyph too), their paints and layers renumbered, a clip list rebuilt, and the
 * palettes as they were. Each test compares the subset with its source through the
 * color reader rather than with a stored answer; `make check-oracle-subset-color`
 * does the same through fontTools over the real fonts.
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <vector>

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/color.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/metrics.h>
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

/** A subset, with its numbering. */
struct Subset {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_SubsetMap map{};
  GFNT_Result result = GFNT_OK;

  Subset(const GFNT_Face * source, std::vector<uint32_t> cps,
      std::vector<uint32_t> glyphs = {}, bool retain = false) {
    GFNT_SubsetOptions o;
    gfnt_subset_options_init(&o);
    o.codepoints = cps.data();
    o.codepoint_count = cps.size();
    o.glyphs = glyphs.data();
    o.glyph_count = glyphs.size();
    o.retain_gids = retain;
    o.map = &map;
    result = gfnt_subset(source, &o, nullptr, nullptr, &blob, nullptr);
    if (result == GFNT_OK) {
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
  /** The subset's bytes. */
  std::vector<uint8_t> blob_bytes() const {
    const uint8_t * d = static_cast<const uint8_t *>(gfnt_blob_data(blob));
    return std::vector<uint8_t>(d, d + gfnt_blob_size(blob));
  }
  /** The glyph the source's @p old glyph became, or -1. */
  long new_of(uint32_t old) const {
    for (size_t i = 0; i < map.count; ++i) {
      if (map.old_of_new[i] == old) {
        return static_cast<long>(i);
      }
    }
    return -1;
  }
};

constexpr uint32_t kA = 2, kB = 3, kAlt = 4;   // color-v1.ttf: A and B, and ghoti.alt (by glyph id)

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

struct Visit {
  std::vector<uint32_t> glyphs;      // every PaintGlyph / PaintColrGlyph glyph, in walk order
  std::vector<int> formats;
};

bool visit(void * user, const GFNT_ColorPaint * p, size_t) {
  auto * v = static_cast<Visit *>(user);
  v->formats.push_back(static_cast<int>(p->format));
  if (p->format == GFNT_PAINT_GLYPH || p->format == GFNT_PAINT_COLR_GLYPH) {
    v->glyphs.push_back(p->glyph);
  }
  return true;
}

Visit walk(const GFNT_Face * face, uint32_t glyph) {
  Visit v;
  GFNT_PaintVisitor visitor = {visit, nullptr};
  EXPECT_EQ(gfnt_face_color_paint_walk(face, glyph, nullptr, &visitor, &v, nullptr), GFNT_OK);
  return v;
}

bool has_base(const GFNT_Face * face, uint32_t glyph) {
  GFNT_PaintRef root = 0;
  return gfnt_face_color_base_paint(face, glyph, &root, nullptr) == GFNT_OK && root;
}

TEST(SubsetColor, APaletteIsCarriedAsItWas) {
  Font font("color-v0.ttf");
  Subset sub(font.face, {0x41});
  ASSERT_EQ(sub.result, GFNT_OK);
  size_t a = 0, b = 0, ea = 0, eb = 0;
  ASSERT_EQ(gfnt_face_palette_count(font.face, &a, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_palette_count(sub.face, &b, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_palette_entry_count(font.face, &ea, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_palette_entry_count(sub.face, &eb, nullptr), GFNT_OK);
  EXPECT_EQ(a, b);
  EXPECT_EQ(ea, eb);
  for (size_t p = 0; p < a; ++p) {
    for (size_t e = 0; e < ea; ++e) {
      GFNT_Color x{}, y{};
      ASSERT_EQ(gfnt_face_palette_color(font.face, p, e, &x, nullptr), GFNT_OK);
      ASSERT_EQ(gfnt_face_palette_color(sub.face, p, e, &y, nullptr), GFNT_OK);
      EXPECT_TRUE(x.red == y.red && x.green == y.green && x.blue == y.blue
          && x.alpha == y.alpha) << p << " " << e;
    }
  }
}

TEST(SubsetColor, AVersionZeroGlyphKeepsItsLayersUnderTheNewNumbers) {
  Font font("color-v0.ttf");
  Subset sub(font.face, {0x41});
  ASSERT_EQ(sub.result, GFNT_OK);
  size_t want = 0, got = 0;
  ASSERT_EQ(gfnt_face_color_layer_count(font.face, kA, &want, nullptr), GFNT_OK);
  const long a = sub.new_of(kA);
  ASSERT_GE(a, 0);
  ASSERT_EQ(gfnt_face_color_layer_count(sub.face, static_cast<uint32_t>(a), &got, nullptr),
      GFNT_OK);
  ASSERT_EQ(got, want);
  ASSERT_GE(want, 3u);
  for (size_t i = 0; i < want; ++i) {
    GFNT_ColorLayer x{}, y{};
    ASSERT_EQ(gfnt_face_color_layer(font.face, kA, i, &x, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_color_layer(sub.face, static_cast<uint32_t>(a), i, &y, nullptr),
        GFNT_OK);
    EXPECT_EQ(sub.new_of(x.glyph), static_cast<long>(y.glyph)) << i;
    EXPECT_EQ(x.palette_index, y.palette_index) << i;
  }
  // B was not asked for, and nothing brings it.
  EXPECT_EQ(sub.new_of(kB), -1);
  size_t none = 0;
  EXPECT_EQ(gfnt_face_color_layer_count(sub.face, 2, &none, nullptr), GFNT_OK);
}

TEST(SubsetColor, AVersionOneGlyphKeepsItsGraphAndClipUnderTheNewNumbers) {
  Font font("color-v1.ttf");
  Subset sub(font.face, {0x42});               // B: a deeper graph, under transforms
  ASSERT_EQ(sub.result, GFNT_OK);
  const long b = sub.new_of(kB);
  ASSERT_GE(b, 0);
  const Visit want = walk(font.face, kB);
  const Visit got = walk(sub.face, static_cast<uint32_t>(b));
  EXPECT_EQ(want.formats, got.formats);
  ASSERT_EQ(want.glyphs.size(), got.glyphs.size());
  ASSERT_FALSE(want.glyphs.empty());
  for (size_t i = 0; i < want.glyphs.size(); ++i) {
    EXPECT_EQ(sub.new_of(want.glyphs[i]), static_cast<long>(got.glyphs[i])) << i;
  }
  GFNT_ColorBox x{}, y{};
  bool fx = false, fy = false;
  ASSERT_EQ(gfnt_face_color_clip(font.face, kB, nullptr, &x, &fx, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_color_clip(sub.face, static_cast<uint32_t>(b), nullptr, &y, &fy,
                nullptr), GFNT_OK);
  ASSERT_TRUE(fx);
  EXPECT_TRUE(fy);
  EXPECT_TRUE(x.x_min == y.x_min && x.y_min == y.y_min && x.x_max == y.x_max
      && x.y_max == y.y_max);
  // The glyphs nobody asked for are gone, base records included.
  EXPECT_FALSE(has_base(sub.face, 0));
  EXPECT_LT(sub.map.count, 8u);
}

TEST(SubsetColor, AGlyphThatPaintsWithAnotherBaseGlyphBringsItAsABase) {
  // ghoti.alt (glyph 4) is a PaintColrGlyph of A; asking for it only, by id, keeps
  // A - as a base glyph, with its own graph.
  Font font("color-v1.ttf");
  Subset sub(font.face, {}, {kAlt});
  ASSERT_EQ(sub.result, GFNT_OK);
  const long alt = sub.new_of(kAlt), a = sub.new_of(kA);
  ASSERT_GE(alt, 0);
  ASSERT_GE(a, 0);
  EXPECT_TRUE(has_base(sub.face, static_cast<uint32_t>(alt)));
  EXPECT_TRUE(has_base(sub.face, static_cast<uint32_t>(a)));
  const Visit v = walk(sub.face, static_cast<uint32_t>(alt));
  ASSERT_FALSE(v.glyphs.empty());
  for (uint32_t g : v.glyphs) {
    EXPECT_LT(g, sub.map.count);
  }
}

TEST(SubsetColor, RetainedIdsLeaveEveryGraphWhereItWas) {
  Font font("color-v1.ttf");
  Subset sub(font.face, {0x41, 0x42}, {}, true);
  ASSERT_EQ(sub.result, GFNT_OK);
  for (uint32_t g : {kA, kB}) {
    ASSERT_TRUE(has_base(sub.face, g));
    EXPECT_EQ(walk(font.face, g).glyphs, walk(sub.face, g).glyphs) << g;
  }
  EXPECT_FALSE(has_base(sub.face, kAlt));   // not asked for, not reached by A or B
}

TEST(SubsetColor, AFontWithNoColorGlyphKeptStaysWithoutAnyBase) {
  Font font("color-v1.ttf");
  Subset sub(font.face, {0x20});
  ASSERT_EQ(sub.result, GFNT_OK);
  uint16_t version = 0;
  EXPECT_EQ(gfnt_face_color_version(sub.face, &version, nullptr), GFNT_OK);
  EXPECT_EQ(version, 1);
  for (uint32_t g = 0; g < sub.map.count; ++g) {
    EXPECT_FALSE(has_base(sub.face, g)) << g;
  }
}

namespace {

/** Every node of a graph at a place: its format, numbers, glyph (as a source glyph), stops, clip. */
struct Node {
  int format;
  std::vector<int32_t> v;
  uint32_t glyph;
  std::vector<std::vector<int32_t>> stops;
  bool operator==(const Node & o) const {
    return format == o.format && v == o.v && glyph == o.glyph && stops == o.stops;
  }
};

struct Collect {
  const GFNT_Face * face;
  const GFNT_Variation * variation;
  const GFNT_SubsetMap * map;     // sub glyph -> source glyph, or null for the source
  std::vector<Node> nodes;
};

bool collect(void * user, const GFNT_ColorPaint * p, size_t) {
  auto * c = static_cast<Collect *>(user);
  Node n;
  n.format = static_cast<int>(p->format);
  n.v.assign(p->v, p->v + 6);
  n.glyph = p->glyph;
  if (c->map && (p->format == GFNT_PAINT_GLYPH || p->format == GFNT_PAINT_COLR_GLYPH)) {
    n.glyph = c->map->old_of_new[p->glyph];
  }
  if (p->color_line) {
    GFNT_ColorLine line{};
    if (gfnt_face_color_line(c->face, p, &line, nullptr) == GFNT_OK) {
      for (size_t i = 0; i < line.stop_count; ++i) {
        GFNT_ColorStop stop{};
        EXPECT_EQ(gfnt_face_color_stop(c->face, p, i, c->variation, &stop, nullptr), GFNT_OK);
        n.stops.push_back({stop.offset, stop.palette_index, stop.alpha});
      }
    }
  }
  c->nodes.push_back(n);
  return true;
}

std::vector<Node> graph_at(const GFNT_Face * face, uint32_t glyph, const GFNT_SubsetMap * map,
    const std::vector<GFNT_F2Dot14> & coords) {
  GFNT_Variation v{coords.data(), coords.size(), GFNT_DELTA_ROUND_HALF_UP};
  Collect c{face, coords.empty() ? nullptr : &v, map, {}};
  GFNT_PaintVisitor visitor = {collect, nullptr};
  EXPECT_EQ(gfnt_face_color_paint_walk(face, glyph, c.variation, &visitor, &c, nullptr), GFNT_OK);
  return c.nodes;
}

std::vector<std::vector<GFNT_F2Dot14>> places(const GFNT_Face * face) {
  size_t axes = 0;
  EXPECT_EQ(gfnt_face_axis_count(face, &axes, nullptr), GFNT_OK);
  std::vector<std::vector<GFNT_F2Dot14>> out = {std::vector<GFNT_F2Dot14>(axes, 0)};
  for (size_t a = 0; a < axes; ++a) {
    for (int v : {-16384, -8192, 8192, 16384}) {
      std::vector<GFNT_F2Dot14> c(axes, 0);
      c[a] = static_cast<GFNT_F2Dot14>(v);
      out.push_back(c);
    }
  }
  out.push_back(std::vector<GFNT_F2Dot14>(axes, 16384));
  out.push_back(std::vector<GFNT_F2Dot14>(axes, -16384));
  return out;
}

}  // namespace

TEST(SubsetColor, AVariableTableKeepsItsStoreAndMapAndEveryVariedValueMatchesAtEveryPlace) {
  for (const char * name : {"color-v1-var.ttf", "color-v1-varmap.ttf", "color-v1-varnone.ttf"}) {
    for (bool retain : {false, true}) {
      for (std::vector<uint32_t> cps : {std::vector<uint32_t>{0x41}, std::vector<uint32_t>{0x42},
               std::vector<uint32_t>{0x41, 0x42}}) {
        SCOPED_TRACE(std::string(name) + (retain ? " retained" : " renumbered"));
        Font font(name);
        Subset sub(font.face, cps, {}, retain);
        ASSERT_EQ(sub.result, GFNT_OK);
        const std::vector<uint8_t> was = table_bytes(font.bytes, "COLR");
        const std::vector<uint8_t> now = table_bytes(sub.blob_bytes(), "COLR");
        ASSERT_GE(now.size(), 34u);
        // The store and the map are still there.
        // (color-v1-var.ttf has no map: the index is the store's row directly.)
        EXPECT_EQ((was[26] | was[27] | was[28] | was[29]) != 0,
            (now[26] | now[27] | now[28] | now[29]) != 0) << "the index map is carried";
        EXPECT_NE((was[30] | was[31] | was[32] | was[33]), 0) << "control: the source varies";
        EXPECT_NE((now[30] | now[31] | now[32] | now[33]), 0) << "the store is carried";
        size_t compared = 0;
        for (uint32_t cp : cps) {
          uint32_t old = 0;
          ASSERT_EQ(gfnt_face_glyph_for_codepoint(font.face, cp, &old, nullptr), GFNT_OK);
          const long now_glyph = sub.new_of(old);
          ASSERT_GE(now_glyph, 0);
          for (const auto & at : places(font.face)) {
            const auto want = graph_at(font.face, old, nullptr, at);
            const auto got = graph_at(sub.face, static_cast<uint32_t>(now_glyph), &sub.map, at);
            ASSERT_FALSE(want.empty());
            EXPECT_EQ(want.size(), got.size());
            for (size_t i = 0; i < want.size() && i < got.size(); ++i) {
              EXPECT_TRUE(want[i] == got[i]) << "node " << i << " at place " << compared;
            }
            ++compared;
          }
          // The clip box varies too.
          for (const auto & at : places(font.face)) {
            GFNT_Variation v{at.data(), at.size(), GFNT_DELTA_ROUND_HALF_UP};
            GFNT_ColorBox x{}, y{};
            bool fx = false, fy = false;
            ASSERT_EQ(gfnt_face_color_clip(font.face, old, &v, &x, &fx, nullptr), GFNT_OK);
            ASSERT_EQ(gfnt_face_color_clip(sub.face, static_cast<uint32_t>(now_glyph), &v, &y,
                          &fy, nullptr), GFNT_OK);
            EXPECT_EQ(fx, fy);
            EXPECT_TRUE(x.x_min == y.x_min && x.y_min == y.y_min && x.x_max == y.x_max
                && x.y_max == y.y_max);
          }
        }
        EXPECT_GT(compared, 6u);
      }
    }
  }
}

TEST(SubsetColor, AnIndexMapThatRunsPastTheTableIsCorruptNotCopied) {
  Font source("color-v1-varmap.ttf");
  const std::vector<uint8_t> colr = table_bytes(source.bytes, "COLR");
  ASSERT_GE(colr.size(), 34u);
  std::vector<uint8_t> bytes = source.bytes;
  size_t at = 0;
  for (size_t i = 0; i + colr.size() <= bytes.size(); ++i) {
    if (std::equal(colr.begin(), colr.begin() + 34, bytes.begin() + i)) {
      at = i;
      break;
    }
  }
  ASSERT_GT(at, 0u);
  // The map's count, made huge: it would run past the table.
  const size_t map = (colr[26] << 24) | (colr[27] << 16) | (colr[28] << 8) | colr[29];
  ASSERT_GT(map, 0u);
  bytes[at + map + 2] = 0xFF;
  bytes[at + map + 3] = 0xFF;
  Font font(bytes);
  Subset sub(font.face, {0x41});
  EXPECT_EQ(sub.result, GFNT_ERR_CORRUPT);
}

TEST(SubsetColor, EveryByteOfTheTableOverwrittenGivesASubsetOrAnError) {
  for (const char * name : {"color-v0.ttf", "color-v1.ttf"}) {
    Font source(name);
    const std::vector<uint8_t> colr = table_bytes(source.bytes, name[7] == '0' ? "COLR" : "COLR");
    size_t at = 0;
    for (size_t i = 0; i + colr.size() <= source.bytes.size(); ++i) {
      if (std::equal(colr.begin(), colr.begin() + 16, source.bytes.begin() + i)) {
        at = i;
        break;
      }
    }
    ASSERT_GT(at, 0u);
    size_t errors = 0, subsets = 0;
    for (size_t i = 0; i < colr.size(); ++i) {
      for (uint8_t value : {uint8_t(0x00), uint8_t(0xFF), uint8_t(0x01)}) {
        std::vector<uint8_t> bytes = source.bytes;
        bytes[at + i] = value;
        Font font(bytes);
        if (!font.face) {
          continue;
        }
        Subset sub(font.face, {0x41, 0x42}, {kAlt});
        if (sub.result == GFNT_OK) {
          ++subsets;
          EXPECT_NE(sub.face, nullptr);
        }
        else {
          ++errors;
        }
      }
    }
    EXPECT_GT(subsets, 10u) << name;
    EXPECT_GT(errors, 10u) << name;
  }
}

TEST(SubsetColor, AnAllocationFailureAnywhereIsReported) {
  Font source("color-v1.ttf");
  std::vector<uint32_t> cps = {0x41, 0x42};
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
