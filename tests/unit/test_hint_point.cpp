/*
 * Tests for the instructions that measure and move points.
 *
 * Every context here is 10 pixels per em on a 640-unit em, so one font unit is
 * exactly one 26.6 unit and the arithmetic can be worked out by hand: a point at
 * y = 100 is 100/64 of a pixel up, and rounding it to the grid gives 128.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <initializer_list>
#include <utility>
#include <vector>

#include <ghoti.io/font/allocator.h>

extern "C" {
#include "../../src/hint/vm.h"
}

#include <gtest/gtest.h>

namespace {

using Bytes = std::vector<uint8_t>;

struct P {
  int x;
  int y;
  bool on = true;
};

Bytes push(std::initializer_list<int> values) {
  Bytes out{0x41, static_cast<uint8_t>(values.size())};
  for (int v : values) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
  }
  return out;
}

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes & part : parts) {
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

Bytes op(std::initializer_list<int> opcodes) {
  Bytes out;
  for (int o : opcodes) {
    out.push_back(static_cast<uint8_t>(o));
  }
  return out;
}

enum : int {
  SVTCA_Y = 0x00, SVTCA_X = 0x01, SPVTCA_Y = 0x02, SFVTCA_X = 0x05,
  SRP0 = 0x10, SRP1 = 0x11, SRP2 = 0x12, SZP0 = 0x13, SZPS = 0x16, SLOOP = 0x17,
  RTG = 0x18, SMD = 0x1A, SCVTCI = 0x1D, ALIGNPTS = 0x27, UTP = 0x29,
  MDAP0 = 0x2E, MDAP1 = 0x2F, IUP_Y = 0x30, IUP_X = 0x31, SHP0 = 0x32, SHP1 = 0x33,
  SHC0 = 0x34, SHZ0 = 0x36, SHPIX = 0x38, IP = 0x39, MSIRP0 = 0x3A, MSIRP1 = 0x3B,
  ALIGNRP = 0x3C, MIAP0 = 0x3E, MIAP1 = 0x3F, GC0 = 0x46, SCFS = 0x48, MD0 = 0x49,
  MD1 = 0x4A, SWAP = 0x23, DELTAP1 = 0x5D, FLIPPT = 0x80, ISECT = 0x0F,
  MDRP_RND_MIN = 0xCC, MIRP_BASE = 0xE0, MDRP_BASE = 0xC0, RTDG = 0x3D,
  SDB = 0x5E, SPVTL0 = 0x06, SFVTL0 = 0x08, SPVFS = 0x0A, GPV = 0x0C,
};

/** A run of a glyph program over the given points. */
struct Hinted {
  GFNT_HintContext * ctx = nullptr;
  GFNT_HintRun * run = nullptr;
  GFNT_Result result = GFNT_ERR_INTERNAL;
  std::vector<int32_t> xs, ys;
  std::vector<uint8_t> on;
  std::vector<size_t> ends;
  Bytes program;

  Hinted(std::vector<P> points, std::vector<size_t> contour_ends, Bytes code,
         std::vector<int32_t> cvt = {}, uint32_t ppem = 10,
         const Bytes & prep = {}) {
    GFNT_HintParams params{};
    params.ppem = ppem;
    params.units_per_em = 640;
    params.version = 35;
    GFNT_HintShape shape{64, 32, 16, 4, 8};
    EXPECT_EQ(gfnt_hint_ctx_create(gfnt_allocator_default(), nullptr, &params,
        &shape, nullptr, 0, prep.data(), prep.size(), cvt.data(), cvt.size(),
        &ctx), GFNT_OK);
    for (const P & p : points) {
      xs.push_back(p.x);
      ys.push_back(p.y);
      on.push_back(p.on);
    }
    // Four phantom points at the origin; no program here reads them.
    for (int i = 0; i < 4; ++i) {
      xs.push_back(0);
      ys.push_back(0);
      on.push_back(1);
    }
    ends = std::move(contour_ends);
    program = std::move(code);
    GFNT_HintGlyphInput input{xs.size(), xs.data(), ys.data(), on.data(),
        ends.size(), ends.data(), false};
    result = gfnt_hint_run_glyph(ctx, &input, program.data(), program.size(), &run);
  }
  ~Hinted() {
    gfnt_hint_run_destroy(run);
    gfnt_hint_ctx_destroy(ctx);
  }
  Hinted(const Hinted &) = delete;
  Hinted & operator=(const Hinted &) = delete;

  int x(size_t i) const { int32_t a = 0, b = 0; gfnt_hint_run_point(run, i, &a, &b); return a; }
  int y(size_t i) const { int32_t a = 0, b = 0; gfnt_hint_run_point(run, i, &a, &b); return b; }
  uint8_t touch(size_t i) const { return gfnt_hint_run_touch(run, i); }
  GFNT_HintFailure failure() const { return gfnt_hint_run_fault(run).failure; }
};

const std::vector<P> kSquare = {{0, 0}, {300, 0}, {300, 300}, {0, 300}};
const std::vector<size_t> kOne = {3};

}  // namespace

TEST(HintPoint, MdapRoundsOrOnlyTouches) {
  Hinted r({{50, 100}, {200, 90}}, {1}, cat({op({SVTCA_Y, RTG}), push({0}), op({MDAP1}),
      push({1}), op({MDAP0})}));
  ASSERT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(r.y(0), 128);      // 100 -> 128
  EXPECT_EQ(r.x(0), 50);       // only along the freedom vector
  EXPECT_EQ(r.touch(0), 2u);   // touched in y
  EXPECT_EQ(r.y(1), 90);       // MDAP[0] marks the point and does not move it
  EXPECT_EQ(r.touch(1), 2u);
}

TEST(HintPoint, MiapUsesTheControlValueWhenItIsCloseEnough) {
  // The cut-in is 68 (17/16 pixel). A control value of 150 is 50 from the point.
  Hinted near({{0, 100}}, {0}, cat({op({SVTCA_Y}), push({0, 0}), op({MIAP1})}), {150});
  EXPECT_EQ(near.y(0), 128);   // round(150) = 128
  // 300 is 200 away, so the original position is rounded instead.
  Hinted far({{0, 100}}, {0}, cat({op({SVTCA_Y}), push({0, 0}), op({MIAP1})}), {300});
  EXPECT_EQ(far.y(0), 128);    // round(100) = 128
  // Without rounding the control value is taken as it stands.
  Hinted raw({{0, 100}}, {0}, cat({op({SVTCA_Y}), push({0, 0}), op({MIAP0})}), {300});
  EXPECT_EQ(raw.y(0), 300);
  // MIAP also sets rp0 and rp1: MDRP then measures from this point.
  Hinted chain({{0, 100}, {0, 200}}, {1}, cat({op({SVTCA_Y}), push({0, 0}), op({MIAP0}),
      push({1}), op({MDRP_BASE})}), {300});
  EXPECT_EQ(chain.y(1), 400);  // 300 + (200 - 100)
}

TEST(HintPoint, MdrpKeepsTheOriginalDistanceUnlessToldToRound) {
  // Point 1 is 100 above point 0, which is at 0. rp0 = 0.
  const Bytes setup = op({SVTCA_Y});
  Hinted plain({{0, 0}, {0, 100}}, {1}, cat({setup, push({0}), op({SRP0}), push({1}),
      op({MDRP_BASE})}));
  EXPECT_EQ(plain.y(1), 100);
  Hinted rounded({{0, 0}, {0, 100}}, {1}, cat({setup, push({0}), op({SRP0}), push({1}),
      op({MDRP_BASE + 4})}));
  EXPECT_EQ(rounded.y(1), 128);
  // The minimum distance (64 by default) holds when flag b is set: 20 -> 64,
  // and -20 -> -64 even though rounding would give zero.
  Hinted min({{0, 100}, {0, 120}}, {1}, cat({setup, push({0}), op({SRP0}), push({1}),
      op({MDRP_BASE + 8})}));
  EXPECT_EQ(min.y(1), 164);
  Hinted behind({{0, 100}, {0, 80}}, {1}, cat({setup, push({0}), op({SRP0}), push({1}),
      op({MDRP_BASE + 8 + 4})}));
  EXPECT_EQ(behind.y(1), 36);  // 100 - 64
  // Flag a moves rp0 to the point just moved.
  Hinted moves_rp0({{0, 0}, {0, 100}, {0, 250}}, {2}, cat({setup, push({0}), op({SRP0}),
      push({1}), op({MDRP_BASE + 16}), push({2}), op({MDRP_BASE})}));
  EXPECT_EQ(moves_rp0.y(2), 250);   // 100 + (250 - 100): relative to the new rp0
}

TEST(HintPoint, MirpUsesAndMayFlipTheControlValue) {
  const Bytes setup = op({SVTCA_Y});
  // cvt 150 above rp0, the point is 100 above: |150 - 100| <= 68, round(150) = 128.
  Hinted a({{0, 0}, {0, 100}}, {1}, cat({setup, push({0}), op({SRP0}), push({1, 0}),
      op({MIRP_BASE + 4})}), {150});
  EXPECT_EQ(a.y(1), 128);
  // The control value is on the other side of the reference point: auto-flip
  // turns it round; with FLIPOFF it is used as written.
  Hinted flipped({{0, 0}, {0, 100}}, {1}, cat({setup, push({0}), op({SRP0}), push({1, 0}),
      op({MIRP_BASE})}), {-150});
  EXPECT_EQ(flipped.y(1), 150);
  Hinted unflipped({{0, 0}, {0, 100}}, {1}, cat({setup, op({0x4E}), push({0}), op({SRP0}),
      push({1, 0}), op({MIRP_BASE})}), {-150});
  EXPECT_EQ(unflipped.y(1), -150);
  // Too far from the outline: the original distance is rounded instead.
  Hinted far({{0, 0}, {0, 100}}, {1}, cat({setup, push({0}), op({SRP0}), push({1, 0}),
      op({MIRP_BASE + 4})}), {400});
  EXPECT_EQ(far.y(1), 128);
}

TEST(HintPoint, MsirpAlignrpAndAlignptsMoveByTheMeasuredDistance) {
  Hinted msirp({{0, 0}, {0, 100}}, {1}, cat({op({SVTCA_Y}), push({0}), op({SRP0}),
      push({1, 40}), op({MSIRP0})}));
  EXPECT_EQ(msirp.y(1), 40);   // exactly the distance given
  Hinted align({{0, 50}, {10, 100}, {20, 200}}, {2}, cat({op({SVTCA_Y}), push({0}),
      op({SRP0}), push({1}), op({ALIGNRP})}));
  EXPECT_EQ(align.y(1), 50);
  EXPECT_EQ(align.x(1), 10);
  Hinted loop({{0, 50}, {10, 100}, {20, 200}}, {2}, cat({op({SVTCA_Y}), push({0}),
      op({SRP0}), push({1, 2, 2}), op({SLOOP}), op({ALIGNRP})}));
  EXPECT_EQ(loop.y(1), 50);
  EXPECT_EQ(loop.y(2), 50);
  Hinted pts({{0, 0}, {0, 100}}, {1}, cat({op({SVTCA_Y}), push({0, 1}), op({ALIGNPTS})}));
  EXPECT_EQ(pts.y(0), pts.y(1));   // both meet in the middle
  EXPECT_EQ(pts.y(0), 50);
}

TEST(HintPoint, IpKeepsAPointsPlaceBetweenItsReferencePoints) {
  // 0 at y = 0 and 2 at y = 300; point 1 is halfway. Move the references to 64
  // and 256, and the point goes to 64 + (256 - 64) / 2 = 160.
  Hinted r({{0, 0}, {0, 150}, {0, 300}}, {2}, cat({op({SVTCA_Y}),
      push({0, 64}), op({SHPIX}), push({2, -44}), op({SHPIX}),
      push({0}), op({SRP1}), push({2}), op({SRP2}), push({1}), op({IP})}));
  ASSERT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(r.y(0), 64);
  EXPECT_EQ(r.y(2), 256);
  EXPECT_EQ(r.y(1), 160);
}

TEST(HintPoint, ShiftsFollowTheReferencePointsMove) {
  // Move point 0 up by 28 (rounding 100 to 128), then shift 1 and 2 by the same.
  Hinted shp({{0, 100}, {0, 200}, {0, 300}}, {2}, cat({op({SVTCA_Y}), push({0}),
      op({MDAP1}), push({0}), op({SRP1}), push({1, 2, 2}), op({SLOOP}), op({SHP1})}));
  EXPECT_EQ(shp.y(0), 128);
  EXPECT_EQ(shp.y(1), 228);
  EXPECT_EQ(shp.y(2), 328);
  // SHPIX shifts by an amount, not by a reference's move.
  Hinted one({{0, 100}, {0, 200}}, {1}, cat({op({SVTCA_Y}), push({1, -7}), op({SHPIX})}));
  EXPECT_EQ(one.y(1), 193);
  EXPECT_EQ(one.touch(1), 2u);
  // SHC shifts a whole contour, SHZ a whole zone; neither marks points touched.
  Hinted shc({{0, 100}, {0, 200}, {0, 300}, {0, 400}}, {1, 3}, cat({op({SVTCA_Y}),
      push({0}), op({MDAP1}), push({0}), op({SRP1}), push({1}), op({SHC0 + 1})}));
  EXPECT_EQ(shc.y(0), 128);
  EXPECT_EQ(shc.y(1), 200);    // same contour as the reference: left alone
  EXPECT_EQ(shc.y(2), 328);    // contour 1 moved by 28
  EXPECT_EQ(shc.y(3), 428);
}

TEST(HintPoint, IupInterpolatesBetweenTouchedPointsAndShiftsBeyondThem) {
  // A contour of four points in y: 0, 100, 200, 300. Touch the ends and move
  // them to 64 and 320; the two between follow in proportion.
  Hinted s({{0, 0}, {0, 100}, {0, 200}, {0, 300}}, {3}, cat({op({SVTCA_Y}),
      push({0, 64}), op({SHPIX}), push({3, 20}), op({SHPIX}), op({IUP_Y})}));
  ASSERT_EQ(s.result, GFNT_OK);
  EXPECT_EQ(s.y(0), 64);
  EXPECT_EQ(s.y(3), 320);
  // 64 + 100 * (320 - 64) / 300 = 149.33; 64 + 200 * 256 / 300 = 234.67
  EXPECT_NEAR(s.y(1), 149, 1);
  EXPECT_NEAR(s.y(2), 235, 1);
  // One touched point: the rest of the contour moves with it.
  Hinted one({{0, 0}, {0, 100}, {0, 200}}, {2}, cat({op({SVTCA_Y}), push({0}), op({MDAP0}),
      push({0}), op({SRP0}), push({0, 30}), op({MSIRP0}), op({IUP_Y})}));
  EXPECT_EQ(one.y(1), 130);
  EXPECT_EQ(one.y(2), 230);
  // Untouched contours are left alone, and IUP in the twilight zone is an error.
  Hinted untouched({{0, 5}, {0, 17}}, {1}, cat({op({SVTCA_Y, IUP_Y})}));
  EXPECT_EQ(untouched.y(0), 5);
  Hinted twilight({{0, 5}}, {0}, cat({push({0}), op({SZPS, IUP_Y})}));
  EXPECT_EQ(twilight.failure(), GFNT_HINT_FAIL_BAD_ARGUMENT);
}

TEST(HintPoint, MeasuringInstructionsReadTheOutline) {
  Hinted r({{0, 100}, {0, 300}, {0, 0}}, {2}, cat({op({SVTCA_Y}),
      push({0}), op({GC0}),                      // 100
      push({1, 0}), op({MD1}),                   // distance from 0 to 1 in the original
      push({2, 250}), op({SCFS})}));             // move point 2 to y = 250
  ASSERT_EQ(r.result, GFNT_OK);
  ASSERT_EQ(gfnt_hint_run_depth(r.run), 2u);
  EXPECT_EQ(gfnt_hint_run_stack(r.run, 0), 100);
  EXPECT_EQ(gfnt_hint_run_stack(r.run, 1), 200);   // 300 - 100
  EXPECT_EQ(r.y(2), 250);
}

TEST(HintPoint, DeltapMovesAPointAtItsSizeOnly) {
  // Relative ppem 1 (high nibble) from delta_base 9 is 10 ppem, our size. The low
  // nibble 12 is +5 steps; delta_shift 3 makes a step 8 units.
  const int arg = (1 << 4) | 12;
  Hinted at({{0, 100}}, {0}, cat({op({SVTCA_Y}), push({arg, 0, 1}), op({DELTAP1})}));
  EXPECT_EQ(at.y(0), 140);
  Hinted other({{0, 100}}, {0}, cat({op({SVTCA_Y}), push({(2 << 4) | 12, 0, 1}), op({DELTAP1})}));
  EXPECT_EQ(other.y(0), 100);
}

TEST(HintPoint, FlipPtTogglesOnCurveAndIsectFindsACrossing) {
  Hinted flip({{0, 0, true}, {10, 10, true}}, {1}, cat({push({1}), op({FLIPPT})}));
  ASSERT_EQ(flip.result, GFNT_OK);
  Hinted cross({{500, 500}, {0, 0}, {640, 640}, {0, 640}, {640, 0}}, {4},
      cat({push({0, 1, 2, 3, 4}), op({ISECT})}));
  ASSERT_EQ(cross.result, GFNT_OK);
  EXPECT_EQ(cross.x(0), 320);
  EXPECT_EQ(cross.y(0), 320);
  EXPECT_EQ(cross.touch(0), 3u);
  // Parallel lines meet at the mean of the four end points.
  Hinted parallel({{500, 500}, {0, 0}, {400, 0}, {0, 100}, {400, 100}}, {4},
      cat({push({0, 1, 2, 3, 4}), op({ISECT})}));
  EXPECT_EQ(parallel.x(0), 200);
  EXPECT_EQ(parallel.y(0), 50);
}

TEST(HintPoint, VectorsFromPointsAndComponents) {
  // An exact 3-4-5 triangle: the unit vector is (0.6, 0.8) in 2.14.
  Hinted v({{0, 0}, {300, 400}}, {1}, cat({push({0, 1}), op({SPVTL0, GPV})}));
  ASSERT_EQ(v.result, GFNT_OK);
  ASSERT_EQ(gfnt_hint_run_depth(v.run), 2u);
  // Direction runs from the point popped first to the one popped second.
  EXPECT_EQ(gfnt_hint_run_stack(v.run, 0), -9830);  // -0.6 * 16384, truncated
  EXPECT_EQ(gfnt_hint_run_stack(v.run, 1), -13107);
  Hinted s({{0, 0}}, {0}, cat({push({0, 8192}), op({SPVFS, GPV})}));
  EXPECT_EQ(gfnt_hint_run_stack(s.run, 0), 0);
  EXPECT_EQ(gfnt_hint_run_stack(s.run, 1), 16384);  // normalised to unit length
}

TEST(HintPoint, ABadPointIndexIsAnErrorNotARead) {
  Hinted r({{0, 0}}, {0}, cat({push({9}), op({MDAP1})}));
  EXPECT_EQ(r.failure(), GFNT_HINT_FAIL_BAD_INDEX);
  Hinted neg({{0, 0}}, {0}, cat({push({-1}), op({MDAP1})}));
  EXPECT_EQ(neg.failure(), GFNT_HINT_FAIL_BAD_INDEX);
  Hinted refs({{0, 0}, {0, 5}}, {1}, cat({push({70}), op({SRP0}), push({1}), op({MDRP_BASE})}));
  EXPECT_EQ(refs.failure(), GFNT_HINT_FAIL_BAD_INDEX);
}

TEST(HintPoint, TheTwilightZoneHoldsPointsAProgramPlaces) {
  // zp0 = 0 (twilight): MIAP sets the point's original and current position from
  // the control value, along the freedom vector; MDRP in the glyph zone can then
  // measure from it. Twilight point 3 goes to y = 200 (the control value).
  Hinted r({{0, 0}, {0, 90}}, {1}, cat({op({SVTCA_Y}), push({0}), op({SZP0}),
      push({3, 0}), op({MIAP0}), push({1}), op({SZP0}), push({1}), op({MDAP0}),
      push({0}), op({SZP0}), push({3}), op({SRP0}), push({1}), op({SZP0}),
      push({1}), op({MDRP_BASE + 4})}), {200});
  ASSERT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(gfnt_hint_run_gstate(r.run)->zp[0], 1);
}

TEST(HintPoint, ThePhantomPointsAreRoundedWhereTheyAreAndNotWhereTheyBegan) {
  // A glyph input whose advance is 453 units at scale 1: its phantom point starts
  // at 453 and is placed at 448 (the nearest whole pixel); a program that shifts
  // a point by the origin's movement sees the 5 units.
  GFNT_HintParams params{};
  params.ppem = 10;
  params.units_per_em = 640;
  params.version = 35;
  GFNT_HintShape shape{64, 32, 16, 4, 8};
  GFNT_HintContext * ctx = nullptr;
  ASSERT_EQ(gfnt_hint_ctx_create(gfnt_allocator_default(), nullptr, &params, &shape,
      nullptr, 0, nullptr, 0, nullptr, 0, &ctx), GFNT_OK);
  int32_t xs[5] = {100, 0, 453, 0, 0};
  int32_t ys[5] = {0, 0, 0, 0, 0};
  uint8_t on[5] = {1, 1, 1, 1, 1};
  size_t ends[1] = {0};
  GFNT_HintGlyphInput input{5, xs, ys, on, 1, ends, false};
  // Point 2 is the advance phantom point (index count - 3). SVTCA x, SRP2 = 2
  // (zp1), SHP[0] on point 0 shifts it by (cur - org) of point 2.
  Bytes code = cat({op({SVTCA_X}), push({2}), op({SRP2}), push({0}), op({SHP0})});
  GFNT_HintRun * run = nullptr;
  ASSERT_EQ(gfnt_hint_run_glyph(ctx, &input, code.data(), code.size(), &run), GFNT_OK);
  int32_t x = 0, y = 0;
  ASSERT_TRUE(gfnt_hint_run_point(run, 0, &x, &y));
  EXPECT_EQ(x, 100 - 5);   // 448 - 453
  gfnt_hint_run_destroy(run);
  gfnt_hint_ctx_destroy(ctx);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
