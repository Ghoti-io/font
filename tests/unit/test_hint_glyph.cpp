/*
 * Tests for hinting whole glyphs from a face: simple glyphs with and without a
 * program, composites (offsets, rounding, transforms, point matching, metrics,
 * programs of their own), and the failures that leave a glyph unhinted.
 *
 * Every face here is 10 pixels per em on a 640-unit em, so a font unit is one 26.6
 * unit and the expected numbers can be worked out by hand.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <utility>
#include <vector>

#include <ghoti.io/font/outline.h>

extern "C" {
#include "../../src/hint/vm.h"
}

#include <gtest/gtest.h>

namespace {

using Bytes = std::vector<uint8_t>;
using gfnttest::GlyfComponent;

enum : uint16_t {
  XY = 0x0002, ROUND = 0x0004, USE_MY = 0x0200, SCALED = 0x0800, UNSCALED = 0x1000,
  SCALE_FLAG = 0x0008,
};

Bytes op(std::initializer_list<int> opcodes) {
  Bytes out;
  for (int o : opcodes) out.push_back(static_cast<uint8_t>(o));
  return out;
}

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes & part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

/** Push signed words. */
Bytes push(std::initializer_list<int> values) {
  Bytes out{0x41, static_cast<uint8_t>(values.size())};
  for (int v : values) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
  }
  return out;
}

struct Metric {
  uint16_t advance;
  int16_t bearing;
};

/** A face over the given glyphs, and a hinting context over it. */
struct Fixture {
  Bytes bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_HintContext * ctx = nullptr;
  GFNT_Error error{};

  Fixture(const std::vector<Bytes> & glyphs, const std::vector<Metric> & metrics,
          const Bytes & prep = {}, int version = 35, uint32_t ppem = 10) {
    Bytes glyf;
    Bytes loca;
    bool is_long = false;
    Bytes maxp = gfnttest::build_maxp(static_cast<uint16_t>(glyphs.size()));
    std::vector<std::pair<uint16_t, int16_t>> hmtx;

    gfnttest::build_glyf_and_loca(glyphs, &glyf, &loca, &is_long);
    gfnttest::patch_u16(maxp, 16, 8);    // twilight
    gfnttest::patch_u16(maxp, 18, 8);    // storage
    gfnttest::patch_u16(maxp, 20, 4);    // functions
    gfnttest::patch_u16(maxp, 22, 2);
    gfnttest::patch_u16(maxp, 24, 64);   // stack
    for (const Metric & m : metrics) hmtx.push_back({m.advance, m.bearing});
    std::vector<gfnttest::Table> tables = {
        {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(640, is_long ? 1 : 0)},
        {GFNT_TAG('h', 'h', 'e', 'a'),
            gfnttest::build_hhea(500, -100, 0, static_cast<uint16_t>(glyphs.size()))},
        {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(hmtx, {})},
        {GFNT_TAG('m', 'a', 'x', 'p'), maxp},
        {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
        {GFNT_TAG('l', 'o', 'c', 'a'), loca},
    };
    if (!prep.empty()) tables.push_back({GFNT_TAG('p', 'r', 'e', 'p'), prep});
    bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED,
        nullptr, nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
    EXPECT_EQ(gfnt_hint_ctx_from_face(face, ppem, version, true, nullptr, &ctx, &error),
        GFNT_OK);
  }
  ~Fixture() {
    gfnt_hint_ctx_destroy(ctx);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Fixture(const Fixture &) = delete;
  Fixture & operator=(const Fixture &) = delete;
};

/** One hinted glyph: points, status, advance. */
struct Result {
  GFNT_Outline * outline = nullptr;
  GFNT_HintStatus status = GFNT_HINT_STATUS_HINTED;
  GFNT_HintFault fault{};
  int32_t advance = 0;
  GFNT_Result result = GFNT_ERR_INTERNAL;

  Result(const Fixture & f, uint32_t glyph) {
    GFNT_Error error{};
    result = gfnt_hint_glyph_outline(f.face, f.ctx, glyph, &outline, &status, &fault,
        &advance, nullptr, 0, nullptr, &error);
  }
  ~Result() { gfnt_outline_destroy(outline); }
  Result(const Result &) = delete;
  Result & operator=(const Result &) = delete;

  size_t count() const { return gfnt_outline_point_count(outline); }
  GFNT_Point at(size_t i) const {
    GFNT_Point p{};
    gfnt_outline_point_at(outline, i, &p, nullptr);
    return p;
  }
};

const std::vector<gfnttest::GlyfPoint> kSquare = {
    {40, 13, true}, {240, 13, true}, {240, 213, true}, {40, 213, true}};
const std::vector<gfnttest::GlyfPoint> kTriangle = {
    {0, 0, true}, {100, 0, true}, {50, 100, true}};

}  // namespace

TEST(HintGlyph, ASimpleGlyphsProgramMovesItsPointsAndTheAdvanceIsWholePixels) {
  // MDAP[1] on point 0 and point 2 in y: 13 -> 0 and 213 -> 192.
  Fixture f({gfnttest::build_glyf_glyph({kSquare}, cat({op({0x00}), push({0}), op({0x2F}), push({2}), op({0x2F})}))},
      {{300, 40}});
  Result r(f, 0);

  ASSERT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(r.status, GFNT_HINT_STATUS_HINTED);
  ASSERT_EQ(r.count(), 4u);
  EXPECT_EQ(r.at(0).y, 0);
  EXPECT_EQ(r.at(2).y, 192);
  EXPECT_EQ(r.at(1).y, 13);       // untouched points stay where they were
  EXPECT_EQ(r.at(0).x, 40);       // the origin is at 40 - 40 = 0
  EXPECT_EQ(r.advance, 320);      // 300 to the nearest pixel, 5 px
}

TEST(HintGlyph, AGlyphWithNoProgramIsLeftAloneAndSaysSo) {
  Fixture f({gfnttest::build_glyf_glyph({kSquare})}, {{300, 40}});
  Result r(f, 0);

  ASSERT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(r.status, GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM);
  EXPECT_EQ(r.at(0).y, 13);
  EXPECT_EQ(r.advance, 320);
}

TEST(HintGlyph, TheOriginMovesTheOutlineByItsRoundedPlace) {
  // Left bearing 0 but the points start at x = 70: the origin phantom point is at
  // 70, rounds to 64 in the original interpreter, and the outline is shifted by it.
  Fixture f({gfnttest::build_glyf_glyph({{{70, 13, true}, {270, 13, true}, {270, 213, true}}})},
      {{500, 0}});
  Result r(f, 0);

  EXPECT_EQ(r.at(0).x, 6);        // 70 - 64
  EXPECT_EQ(r.advance, 512);      // 570 -> 576, less the origin's 64
  Fixture v40({gfnttest::build_glyf_glyph({{{70, 13, true}, {270, 13, true}, {270, 213, true}}})},
      {{500, 0}}, {}, 40);
  Result q(v40, 0);
  EXPECT_EQ(q.at(0).x, 0);        // version 40 leaves x alone: exact shift
  EXPECT_EQ(q.advance, 512);
}

TEST(HintGlyph, ACompositePlacesHintedComponentsByTheirOffsets) {
  // Glyph 0 hints its y (13 -> 0, 213 -> 192); glyph 1 is left as it is.
  Bytes hinted = gfnttest::build_glyf_glyph({kSquare},
      cat({op({0x00}), push({0}), op({0x2F}), push({2}), op({0x2F})}));
  Bytes plain = gfnttest::build_glyf_glyph({kTriangle});
  Bytes composite = gfnttest::build_glyf_composite({
      {0, XY | 0x1, 100, 50, {}}, {1, XY | 0x1, 300, 17, {}}});
  Fixture f({hinted, plain, composite}, {{300, 40}, {170, 0}, {500, 0}});
  Result r(f, 2);

  ASSERT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(r.status, GFNT_HINT_STATUS_HINTED);
  ASSERT_EQ(r.count(), 7u);
  // The composite's box starts at x = 0, its bearing is 0: no shift. The first
  // component sits at (100, 50) over its hinted points.
  EXPECT_EQ(r.at(0).x, 140);
  EXPECT_EQ(r.at(0).y, 50);       // hinted 0 + 50
  EXPECT_EQ(r.at(2).y, 242);      // hinted 192 + 50
  EXPECT_EQ(r.at(1).y, 63);       // 13 + 50
  EXPECT_EQ(r.at(4).x, 300);      // the triangle, unhinted, at (300, 17)
  EXPECT_EQ(r.at(4).y, 17);
}

TEST(HintGlyph, RoundXyToGridRoundsTheOffsetsAndVersion40OnlyTheVertical) {
  Bytes plain = gfnttest::build_glyf_glyph({kTriangle});
  Bytes composite = gfnttest::build_glyf_composite({{0, XY | ROUND | 0x1, 37, 41, {}}});
  Fixture v35({plain, composite}, {{170, 0}, {500, 0}});
  Result a(v35, 1);
  EXPECT_EQ(a.at(0).x, 64);       // 37 -> 64
  EXPECT_EQ(a.at(0).y, 64);       // 41 -> 64
  Fixture v40({plain, composite}, {{170, 0}, {500, 0}}, {}, 40);
  Result b(v40, 1);
  EXPECT_EQ(b.at(0).x, 37);       // x is left alone in the compatibility behaviour
  EXPECT_EQ(b.at(0).y, 64);
  // A tie goes up: 32 rounds to 64 and -32 to 0, as FreeType's does.
  Bytes ties = gfnttest::build_glyf_composite({{0, XY | ROUND | 0x1, 32, -32, {}}});
  Fixture t({plain, ties}, {{170, 0}, {500, 0}});
  Result c(t, 1);
  EXPECT_EQ(c.at(0).x, 64);
  EXPECT_EQ(c.at(0).y, 0);
}

TEST(HintGlyph, AComponentIsTransformedAfterItIsHintedAndBeforeItIsPlaced) {
  Bytes plain = gfnttest::build_glyf_glyph({kSquare});
  // WE_HAVE_A_SCALE with 0.5 in 2.14 (0x2000).
  Bytes composite = gfnttest::build_glyf_composite({
      {0, XY | SCALE_FLAG | 0x1, 100, 50, {0x2000}}});
  Fixture f({plain, composite}, {{300, 0}, {500, 0}});
  Result r(f, 1);

  ASSERT_EQ(r.count(), 4u);
  // (40, 13) * 0.5 = (20, 6.5 -> 7?) ; 6.5 rounds half up to 7; plus (100, 50).
  EXPECT_EQ(r.at(0).y, 50 + 7);
  EXPECT_EQ(r.at(2).y, 50 + 107);       // 213 / 2 = 106.5 -> 107
  EXPECT_EQ(r.at(1).x - r.at(0).x, 100);
}

TEST(HintGlyph, ComponentsMayBePlacedByMatchingPoints) {
  Bytes plain = gfnttest::build_glyf_glyph({kSquare});
  Bytes tri = gfnttest::build_glyf_glyph({kTriangle});
  // The triangle's point 1 goes where the placed square's point 2 is.
  Bytes composite = gfnttest::build_glyf_composite({
      {0, XY | 0x1, 100, 50, {}}, {1, 0x0000, 2, 1, {}}});
  Fixture f({plain, tri, composite}, {{300, 40}, {170, 0}, {500, 0}});
  Result r(f, 2);

  ASSERT_EQ(r.count(), 7u);
  EXPECT_EQ(r.at(5).x, r.at(2).x);
  EXPECT_EQ(r.at(5).y, r.at(2).y);
  EXPECT_EQ(r.at(4).x, r.at(2).x - 100);
}

TEST(HintGlyph, UseMyMetricsTakesTheComponentsAdvance) {
  Bytes plain = gfnttest::build_glyf_glyph({kSquare});
  Bytes tri = gfnttest::build_glyf_glyph({kTriangle});
  Bytes without = gfnttest::build_glyf_composite({{0, XY | 0x1, 0, 0, {}}, {1, XY | 0x1, 300, 0, {}}});
  Bytes with = gfnttest::build_glyf_composite({{0, XY | 0x1, 0, 0, {}}, {1, XY | USE_MY | 0x1, 300, 0, {}}});
  Fixture f({plain, tri, without, with}, {{300, 40}, {170, 0}, {500, 0}, {500, 0}});

  EXPECT_EQ(Result(f, 2).advance, 512);
  EXPECT_EQ(Result(f, 3).advance, 192);    // the triangle's 170, to the pixel
}

TEST(HintGlyph, ACompositesOwnProgramSeesTheHintedComponentsAsTheirOriginals) {
  Bytes hinted = gfnttest::build_glyf_glyph({kSquare},
      cat({op({0x00}), push({0}), op({0x2F})}));        // y of point 0: 13 -> 0
  Bytes tri = gfnttest::build_glyf_glyph({kTriangle});
  // GC[1] (original) of point 0 is the hinted 0 + 50, not the unhinted 13 + 50.
  Bytes composite = gfnttest::build_glyf_composite(
      {{0, XY | 0x1, 100, 50, {}}, {1, XY | 0x1, 300, 17, {}}},
      cat({op({0x00}), push({0}), op({0x47}), push({4}), op({0x23, 0x48})}));
  Fixture f({hinted, tri, composite}, {{300, 40}, {170, 0}, {500, 0}});
  Result r(f, 2);

  ASSERT_EQ(r.count(), 7u);
  EXPECT_EQ(r.at(4).y, 50);
}

TEST(HintGlyph, AFailingProgramLeavesTheGlyphUnhintedAndNamesTheFault) {
  Fixture f({gfnttest::build_glyf_glyph({kSquare}, op({0x60}))}, {{300, 40}});   // ADD, no operands
  Result r(f, 0);

  ASSERT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(r.status, GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED);
  EXPECT_EQ(r.fault.failure, GFNT_HINT_FAIL_STACK_UNDERFLOW);
  EXPECT_EQ(r.at(0).y, 13);        // the scaled outline, as the font drew it
}

TEST(HintGlyph, ACompositeThatIncludesItselfIsRefused) {
  Bytes self = gfnttest::build_glyf_composite({{0, XY | 0x1, 0, 0, {}}});
  Fixture f({self}, {{500, 0}});
  GFNT_Outline * outline = nullptr;
  GFNT_HintStatus status;
  GFNT_Error error{};

  EXPECT_EQ(gfnt_hint_glyph_outline(f.face, f.ctx, 0, &outline, &status, nullptr,
      nullptr, nullptr, 0, nullptr, &error), GFNT_ERR_CORRUPT);
  EXPECT_EQ(outline, nullptr);
}

TEST(HintGlyph, AnEmptyGlyphHasNothingToHintButStillAnAdvance) {
  Fixture f({Bytes{}}, {{300, 0}});
  Result r(f, 0);

  ASSERT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(r.count(), 0u);
  EXPECT_EQ(r.advance, 320);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
