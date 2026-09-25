/**
 * @file
 *
 * Fixed-point arithmetic: the rounding rule, the saturation, and the one
 * place font units become pixels.
 *
 * documentation/design.md section 5.2. Every expectation here is the
 * specified value computed by hand, not what the implementation happens to
 * answer: the point of these types is that two platforms agree on a number,
 * so the number has to be written down somewhere other than in the code that
 * produces it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstdint>

TEST(Fixed, OneIsTheDocumentedScale) {
  // 26.6, 16.16 and 2.14 are named for their layouts; a wrong constant here
  // would make every conversion consistently wrong, which no other test in
  // this file could distinguish from correct.
  EXPECT_EQ(GFNT_F26DOT6_ONE, 1 << 6);
  EXPECT_EQ(GFNT_F16DOT16_ONE, 1 << 16);
  EXPECT_EQ(GFNT_F2DOT14_ONE, 1 << 14);
}

TEST(Fixed, MulMultipliesInSixteenSixteen) {
  EXPECT_EQ(gfnt_f16dot16_mul(GFNT_F16DOT16_ONE, GFNT_F16DOT16_ONE),
      GFNT_F16DOT16_ONE);
  EXPECT_EQ(gfnt_f16dot16_mul(GFNT_F16DOT16_ONE / 2, GFNT_F16DOT16_ONE / 2),
      GFNT_F16DOT16_ONE / 4);
  EXPECT_EQ(gfnt_f16dot16_mul(-GFNT_F16DOT16_ONE / 2, GFNT_F16DOT16_ONE / 2),
      -GFNT_F16DOT16_ONE / 4);
  EXPECT_EQ(gfnt_f16dot16_mul(0, GFNT_F16DOT16_ONE), 0);
}

TEST(Fixed, MulRoundsHalfAwayFromZero) {
  // 1/65536 * 1/2 is exactly half a unit. FreeType's FT_MulFix rounds it up
  // in magnitude, and this library states that rule so that a test vector
  // generated against FreeType is usable here.
  EXPECT_EQ(gfnt_f16dot16_mul(1, GFNT_F16DOT16_ONE / 2), 1);
  EXPECT_EQ(gfnt_f16dot16_mul(-1, GFNT_F16DOT16_ONE / 2), -1);
  // Just under half rounds toward zero, in both directions.
  EXPECT_EQ(gfnt_f16dot16_mul(1, GFNT_F16DOT16_ONE / 2 - 1), 0);
  EXPECT_EQ(gfnt_f16dot16_mul(-1, GFNT_F16DOT16_ONE / 2 - 1), 0);
}

TEST(Fixed, MulSaturatesRatherThanWrapping) {
  // Signed overflow is undefined behaviour and the inputs come from a font
  // file, so the product is clamped. A wrapped coordinate is unbounded
  // nonsense; a clamped one is bounded nonsense the rasteriser can refuse.
  EXPECT_EQ(gfnt_f16dot16_mul(INT32_MAX, INT32_MAX), INT32_MAX);
  EXPECT_EQ(gfnt_f16dot16_mul(INT32_MIN, INT32_MAX), INT32_MIN);
  EXPECT_EQ(gfnt_f16dot16_mul(INT32_MIN, INT32_MIN), INT32_MAX);
}

TEST(Fixed, DivDividesInSixteenSixteen) {
  EXPECT_EQ(gfnt_f16dot16_div(GFNT_F16DOT16_ONE, GFNT_F16DOT16_ONE),
      GFNT_F16DOT16_ONE);
  EXPECT_EQ(gfnt_f16dot16_div(GFNT_F16DOT16_ONE, GFNT_F16DOT16_ONE * 2),
      GFNT_F16DOT16_ONE / 2);
  EXPECT_EQ(gfnt_f16dot16_div(-GFNT_F16DOT16_ONE, GFNT_F16DOT16_ONE * 2),
      -GFNT_F16DOT16_ONE / 2);
}

TEST(Fixed, DivByZeroIsBoundedRatherThanATrap) {
  EXPECT_EQ(gfnt_f16dot16_div(GFNT_F16DOT16_ONE, 0), INT32_MAX);
  EXPECT_EQ(gfnt_f16dot16_div(-GFNT_F16DOT16_ONE, 0), INT32_MIN);
  EXPECT_EQ(gfnt_f16dot16_div(0, 0), 0);
}

TEST(Fixed, DivSaturatesRatherThanWrapping) {
  EXPECT_EQ(gfnt_f16dot16_div(INT32_MAX, 1), INT32_MAX);
  EXPECT_EQ(gfnt_f16dot16_div(INT32_MIN, 1), INT32_MIN);
}

TEST(Fixed, TwoDotFourteenWidensExactly) {
  // 1/16384 is exactly four units of 1/65536, so no rounding can occur here
  // and the variation coordinates a font supplies survive the widening.
  EXPECT_EQ(gfnt_f2dot14_to_f16dot16(GFNT_F2DOT14_ONE), GFNT_F16DOT16_ONE);
  EXPECT_EQ(gfnt_f2dot14_to_f16dot16(GFNT_F2DOT14_ONE / 2),
      GFNT_F16DOT16_ONE / 2);
  EXPECT_EQ(gfnt_f2dot14_to_f16dot16(-GFNT_F2DOT14_ONE), -GFNT_F16DOT16_ONE);
  EXPECT_EQ(gfnt_f2dot14_to_f16dot16(1), 4);
  // 2.14 reaches just past -2.0, and the widening must not overflow there.
  EXPECT_EQ(gfnt_f2dot14_to_f16dot16(INT16_MIN), -2 * GFNT_F16DOT16_ONE);
}

TEST(Fixed, ScaleForPpemIsPixelsPerFontUnit) {
  // 16 ppem on a 2048-unit em is exactly 1/128 of a pixel per unit.
  EXPECT_EQ(gfnt_scale_for_ppem(2048, 16), GFNT_F16DOT16_ONE / 128);
  // 12 ppem on a 1000-unit em is 0.012 px/unit: 786.432 in 16.16, which
  // rounds to 786.
  EXPECT_EQ(gfnt_scale_for_ppem(1000, 12), 786);
}

TEST(Fixed, ScaleForZeroEmIsZero) {
  // A face with no em cannot scale; the caller has a diagnostic for the face
  // already and does not need this call to invent one.
  EXPECT_EQ(gfnt_scale_for_ppem(0, 16), 0);
}

TEST(Fixed, AnEmOfUnitsIsThePixelSize) {
  // The property that makes the scale usable: unitsPerEm font units at N ppem
  // is N pixels, for every em size and every ppem, in 26.6.
  const uint16_t upems[] = {16, 1000, 2048, 4096};
  const uint32_t ppems[] = {8, 11, 12, 16, 24, 48, 96};

  for (uint16_t upem : upems) {
    for (uint32_t ppem : ppems) {
      GFNT_F16Dot16 scale = gfnt_scale_for_ppem(upem, ppem);
      EXPECT_EQ(gfnt_units_to_pixels(upem, scale),
          (GFNT_F26Dot6)(ppem * GFNT_F26DOT6_ONE))
          << "upem " << upem << " at " << ppem << " ppem";
      EXPECT_EQ(gfnt_units_to_pixels(-(int32_t)upem, scale),
          -(GFNT_F26Dot6)(ppem * GFNT_F26DOT6_ONE))
          << "upem " << upem << " at " << ppem << " ppem, negative";
    }
  }
}

TEST(Fixed, UnitsToPixelsSaturatesRatherThanWrapping) {
  EXPECT_EQ(gfnt_units_to_pixels(INT32_MAX, INT32_MAX), INT32_MAX);
  EXPECT_EQ(gfnt_units_to_pixels(INT32_MIN, INT32_MAX), INT32_MIN);
}

TEST(Fixed, RoundGoesHalfAwayFromZero) {
  EXPECT_EQ(gfnt_f26dot6_round(0), 0);
  EXPECT_EQ(gfnt_f26dot6_round(GFNT_F26DOT6_ONE), 1);
  EXPECT_EQ(gfnt_f26dot6_round(GFNT_F26DOT6_ONE + 32), 2) << "1.5 rounds to 2";
  EXPECT_EQ(gfnt_f26dot6_round(-GFNT_F26DOT6_ONE - 32), -2)
      << "-1.5 rounds to -2, not -1";
  EXPECT_EQ(gfnt_f26dot6_round(31), 0);
  EXPECT_EQ(gfnt_f26dot6_round(32), 1);
  EXPECT_EQ(gfnt_f26dot6_round(-32), -1);
}

TEST(Fixed, FloorAndCeilStraddleTheValue) {
  EXPECT_EQ(gfnt_f26dot6_floor(GFNT_F26DOT6_ONE), 1);
  EXPECT_EQ(gfnt_f26dot6_ceil(GFNT_F26DOT6_ONE), 1);
  EXPECT_EQ(gfnt_f26dot6_floor(1), 0);
  EXPECT_EQ(gfnt_f26dot6_ceil(1), 1);
  // The negative side is where a shift-based implementation and a
  // division-based one differ, and where C leaves >> implementation-defined.
  EXPECT_EQ(gfnt_f26dot6_floor(-1), -1);
  EXPECT_EQ(gfnt_f26dot6_ceil(-1), 0);
  EXPECT_EQ(gfnt_f26dot6_floor(-GFNT_F26DOT6_ONE), -1);
  EXPECT_EQ(gfnt_f26dot6_ceil(-GFNT_F26DOT6_ONE), -1);
  EXPECT_EQ(gfnt_f26dot6_floor(-GFNT_F26DOT6_ONE - 1), -2);
  EXPECT_EQ(gfnt_f26dot6_ceil(-GFNT_F26DOT6_ONE - 1), -1);
}

TEST(Fixed, FloorAndCeilAgreeOnlyOnWholePixels) {
  for (GFNT_F26Dot6 v = -200; v <= 200; ++v) {
    int32_t floor = gfnt_f26dot6_floor(v);
    int32_t ceil = gfnt_f26dot6_ceil(v);
    if (v % GFNT_F26DOT6_ONE == 0) {
      EXPECT_EQ(floor, ceil) << "at " << v;
    }
    else {
      EXPECT_EQ(ceil, floor + 1) << "at " << v;
    }
    EXPECT_LE(floor * GFNT_F26DOT6_ONE, v) << "at " << v;
    EXPECT_GE(ceil * GFNT_F26DOT6_ONE, v) << "at " << v;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
