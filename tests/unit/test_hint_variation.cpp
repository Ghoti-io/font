/*
 * Tests for hinting a variable face at a location: `gvar` moving the points a program
 * works on, `cvar` moving its control values, and what the interpreter is told about the
 * location (GETVARIATION, GETINFO).
 *
 * Every face is one weight axis on a 640-unit em, hinted at 10 pixels per em, so a font
 * unit is one 26.6 unit. The tuples peak at the axis maximum, so a location of 0.5 moves
 * things by half what 1.0 does.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "hint_fixture.h"

namespace {

using gfnttest::put_u16;
using gfnttest::put_u32;

constexpr GFNT_F2Dot14 kFull = 16384;
constexpr GFNT_F2Dot14 kHalf = 8192;

/** An `fvar` with one axis, wght 100..400..900. */
gfnttest::Table fvar() {
  Bytes t;
  put_u16(t, 1);
  put_u16(t, 0);
  put_u16(t, 16);
  put_u16(t, 2);
  put_u16(t, 1);
  put_u16(t, 20);
  put_u16(t, 0);
  put_u16(t, 8);
  put_u32(t, GFNT_TAG('w', 'g', 'h', 't'));
  put_u32(t, 100u << 16);
  put_u32(t, 400u << 16);
  put_u32(t, 900u << 16);
  put_u16(t, 0);
  put_u16(t, 256);
  return {GFNT_TAG('f', 'v', 'a', 'r'), t};
}

/** One tuple peaking at 1.0 that moves every point, and the data a tuple carries. */
Bytes tuple_data(const std::vector<int> & dx, const std::vector<int> & dy) {
  Bytes d;
  d.push_back(0);                                      // every point
  for (const std::vector<int> * axis : {&dx, &dy}) {
    // Words, in runs of up to 64.
    for (size_t i = 0; i < axis->size(); i += 64) {
      size_t n = std::min<size_t>(64, axis->size() - i);

      d.push_back(static_cast<uint8_t>(0x40 | (n - 1)));
      for (size_t k = 0; k < n; ++k) put_u16(d, static_cast<uint16_t>((*axis)[i + k]));
    }
  }
  return d;
}

/** A `gvar` (one axis) whose glyph @p which has a tuple moving all its points; others none. */
gfnttest::Table gvar(size_t glyphs, size_t which, const std::vector<int> & dx,
    const std::vector<int> & dy) {
  Bytes data;
  Bytes serial = tuple_data(dx, dy);

  put_u16(data, 1);                                    // tupleVariationCount
  put_u16(data, 4 + 6);                                // where the serialized data starts
  put_u16(data, static_cast<uint16_t>(serial.size()));
  put_u16(data, 0xA000);                               // embedded peak, private points
  put_u16(data, static_cast<uint16_t>(kFull));
  data.insert(data.end(), serial.begin(), serial.end());
  if (data.size() % 2) data.push_back(0);

  Bytes t;
  put_u16(t, 1);
  put_u16(t, 0);
  put_u16(t, 1);                                       // axisCount
  put_u16(t, 0);                                       // sharedTupleCount
  put_u32(t, 0);
  put_u16(t, static_cast<uint16_t>(glyphs));
  put_u16(t, 0);                                       // short offsets
  put_u32(t, static_cast<uint32_t>(20 + 2 * (glyphs + 1)));
  size_t at = 0;
  for (size_t g = 0; g <= glyphs; ++g) {
    put_u16(t, static_cast<uint16_t>(at / 2));
    if (g == which) at += data.size();
  }
  t.insert(t.end(), data.begin(), data.end());
  return {GFNT_TAG('g', 'v', 'a', 'r'), t};
}

/** A `cvar` (one axis) with a tuple at 1.0 moving the control values by @p deltas. */
gfnttest::Table cvar(const std::vector<int> & deltas) {
  Bytes serial;
  serial.push_back(0);                                 // every value
  serial.push_back(static_cast<uint8_t>(0x40 | (deltas.size() - 1)));
  for (int v : deltas) put_u16(serial, static_cast<uint16_t>(v));
  Bytes t;
  put_u16(t, 1);
  put_u16(t, 0);
  put_u16(t, 1);                                       // tupleVariationCount
  put_u16(t, 8 + 6);                                   // data offset
  put_u16(t, static_cast<uint16_t>(serial.size()));
  put_u16(t, 0xA000);
  put_u16(t, static_cast<uint16_t>(kFull));
  t.insert(t.end(), serial.begin(), serial.end());
  return {GFNT_TAG('c', 'v', 'a', 'r'), t};
}

gfnttest::Table cvt(const std::vector<int> & values) {
  Bytes t;
  for (int v : values) put_u16(t, static_cast<uint16_t>(v));
  return {GFNT_TAG('c', 'v', 't', ' '), t};
}

/** Where a program leaves point 0's y, read by moving it there with SCFS. */
Bytes read_into_y(const Bytes & pushes_one_value) {
  return cat({op({0x00}), pushes_one_value, push({0}), op({0x23}), op({0x48})});   // SVTCA y; ...; SWAP; SCFS
}

}  // namespace

TEST(HintVariation, GvarMovesThePointsAProgramWorksOn) {
  // Four points, each moved up by 64 font units at 1.0 (and x by 10).
  std::vector<int> dx(8, 10);
  std::vector<int> dy(8, 64);
  Bytes glyph = gfnttest::build_glyf_glyph({kSquare});
  Fixture full({glyph}, {{300, 40}}, {}, 40, 10, {}, {fvar(), gvar(1, 0, dx, dy)}, {kFull});
  Fixture half({glyph}, {{300, 40}}, {}, 40, 10, {}, {fvar(), gvar(1, 0, dx, dy)}, {kHalf});
  Fixture none({glyph}, {{300, 40}}, {}, 40, 10, {}, {fvar(), gvar(1, 0, dx, dy)});

  Result a(full, 0);
  Result b(half, 0);
  Result c(none, 0);
  ASSERT_EQ(a.result, GFNT_OK);
  ASSERT_EQ(b.result, GFNT_OK);
  EXPECT_EQ(a.status, GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM);
  EXPECT_EQ(a.at(0).y, 13 + 64);
  EXPECT_EQ(b.at(0).y, 13 + 32);
  EXPECT_EQ(c.at(0).y, 13);                     // no location: the default instance
  // x moves by 10 too, but the outline is placed by its origin, which moved with it.
  EXPECT_EQ(a.at(1).x - a.at(0).x, 200);
}

TEST(HintVariation, TheProgramIsToldTheLocationAndThatTheFaceIsVariable) {
  Bytes glyph = gfnttest::build_glyf_glyph({kSquare},
      read_into_y(op({0x91})));                           // GETVARIATION
  Fixture f({glyph}, {{300, 40}}, {}, 40, 10, {}, {fvar(), gvar(1, 99, {}, {})}, {kHalf});
  Result r(f, 0);

  ASSERT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(r.status, GFNT_HINT_STATUS_HINTED);
  EXPECT_EQ(r.at(0).y, kHalf);

  Bytes info = gfnttest::build_glyf_glyph({kSquare}, read_into_y(cat({push({8}), op({0x88})})));
  Fixture g({info}, {{300, 40}}, {}, 40, 10, {}, {fvar(), gvar(1, 99, {}, {})}, {kHalf});
  Fixture plain({info}, {{300, 40}}, {}, 40, 10);
  Result s(g, 0);
  Result t(plain, 0);
  EXPECT_EQ(s.at(0).y, 1 << 10);                         // GETINFO: a face with variations
  EXPECT_EQ(t.at(0).y, 0);                               // a face without them
}

TEST(HintVariation, CvarMovesTheControlValues) {
  // Control value 0 is 100 font units, moved by +50 at 1.0: 125 at a half.
  Bytes glyph = gfnttest::build_glyf_glyph({kSquare}, read_into_y(cat({push({0}), op({0x45})})));   // RCVT
  Fixture f({glyph}, {{300, 40}}, {}, 40, 10, {},
      {fvar(), gvar(1, 99, {}, {}), cvt({100}), cvar({50})}, {kHalf});
  Fixture g({glyph}, {{300, 40}}, {}, 40, 10, {}, {fvar(), gvar(1, 99, {}, {}), cvt({100}), cvar({50})});
  Result r(f, 0);
  Result s(g, 0);

  EXPECT_EQ(r.at(0).y, 125);
  EXPECT_EQ(s.at(0).y, 100);
}

TEST(HintVariation, GvarMovesACompositesComponentOffsets) {
  // The composite's one component sits at (0, 0), and gvar moves it by (+64, -32) at 1.0 (its phantom points stay).
  Bytes part = gfnttest::build_glyf_glyph({kTriangle});
  Bytes composite = gfnttest::build_glyf_composite({{0, XY | 0x1, 0, 0, {}}});
  Fixture f({part, composite}, {{170, 0}, {500, 0}}, {}, 40, 10, {},
      {fvar(), gvar(2, 1, {64, 0, 0, 0, 0}, {-32, 0, 0, 0, 0})}, {kFull});
  Fixture g({part, composite}, {{170, 0}, {500, 0}}, {}, 40, 10);
  Result r(f, 1);
  Result s(g, 1);

  ASSERT_EQ(r.count(), 3u);
  EXPECT_EQ(r.at(0).x - s.at(0).x, 64);
  EXPECT_EQ(r.at(0).y - s.at(0).y, -32);
}

TEST(HintVariation, ALocationNeedsTheGvarOfAFaceItFits) {
  Fixture f({gfnttest::build_glyf_glyph({kSquare})}, {{300, 40}}, {}, 40, 10, {}, {fvar()},
      {kFull});
  Result r(f, 0);

  ASSERT_EQ(r.result, GFNT_OK);                         // no gvar: nothing moves, nothing fails
  EXPECT_EQ(r.at(0).y, 13);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
