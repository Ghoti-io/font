/**
 * @file
 *
 * The one rounding the `trak` table's tracking needs: the floor of a float as an
 * int32, written without libm (AD-22) and checked against the libm one here,
 * where libm is allowed because a test is not the library.
 *
 * `floorf()` was what trak.c called, and a libm function is not promised to be
 * the same on every C library; this one is arithmetic on a conversion, exact for
 * every float that fits. What the shaping level does with it - the advance and
 * the offset for a point size - is `ShapeTrak` in test_shape.cpp.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "../../src/layout_tables/layout.h"

namespace {

float from_bits(uint32_t bits) {
  float value;

  std::memcpy(&value, &bits, sizeof value);
  return value;
}

/** What floorf() says, for a value that fits an int32. */
int32_t reference(float value) {
  return static_cast<int32_t>(std::floor(value));
}

}  // namespace

TEST(Trak, TheFloorOfWholeAndFractionalValuesOfEitherSign) {
  const float values[] = {0.0f, -0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 41.999996f,
      -0.25f, -0.5f, -0.75f, -1.0f, -1.5f, -2.0f, -41.999996f, 1e-30f, -1e-30f,
      std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min(),
      8388607.5f, -8388607.5f, 8388608.0f, -8388608.0f, 16777216.0f, -16777216.0f,
      16777218.0f, -16777218.0f, 1.0e9f, -1.0e9f};

  for (const float value : values) {
    EXPECT_EQ(gfnt_trak_floor(value), reference(value)) << value;
  }
}

TEST(Trak, TheFloorAtTheEdgesOfTheInt32Range) {
  // 2^31 - 128 is the largest float below 2^31; -2^31 is exactly INT32_MIN.
  EXPECT_EQ(gfnt_trak_floor(2147483520.0f), 2147483520);
  EXPECT_EQ(gfnt_trak_floor(-2147483648.0f), INT32_MIN);
  EXPECT_EQ(gfnt_trak_floor(-2147483520.0f), -2147483520);
  // The next float below -2^31 and the first at or above 2^31 are out of range.
  EXPECT_EQ(gfnt_trak_floor(-2147483904.0f), INT32_MIN);
  EXPECT_EQ(gfnt_trak_floor(2147483648.0f), INT32_MAX);
  EXPECT_EQ(gfnt_trak_floor(3.0e38f), INT32_MAX);
  EXPECT_EQ(gfnt_trak_floor(-3.0e38f), INT32_MIN);
  EXPECT_EQ(gfnt_trak_floor(std::numeric_limits<float>::infinity()), INT32_MAX);
  EXPECT_EQ(gfnt_trak_floor(-std::numeric_limits<float>::infinity()), INT32_MIN);
  EXPECT_EQ(gfnt_trak_floor(std::numeric_limits<float>::quiet_NaN()), 0);
}

/** Every float exponent, with mantissas at both ends and the middle, both signs. */
TEST(Trak, TheFloorOfEveryExponentThatFitsMatchesLibm) {
  for (uint32_t exponent = 0; exponent < 255; ++exponent) {
    for (const uint32_t mantissa : {0u, 1u, 0x2AAAAAu, 0x400000u, 0x555555u, 0x7FFFFEu, 0x7FFFFFu}) {
      for (const uint32_t sign : {0u, 0x80000000u}) {
        const float value = from_bits(sign | (exponent << 23) | mantissa);

        if (value >= -2147483648.0f && value < 2147483648.0f) {
          ASSERT_EQ(gfnt_trak_floor(value), reference(value)) << value;
        }
      }
    }
  }
}

/** A stream of pseudo-random floats around the magnitudes tracking takes. */
TEST(Trak, TheFloorOfManyRandomValuesMatchesLibm) {
  uint64_t state = 0x9E3779B97F4A7C15ULL;

  for (int i = 0; i < 200000; ++i) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    const uint64_t draw = state * 0x2545F4914F6CDD1DULL;
    // A value in (-2^14, 2^14) with a fraction of up to six bits.
    const float value = static_cast<float>(static_cast<int64_t>(draw % 2097152) - 1048576) / 64.0f;

    ASSERT_EQ(gfnt_trak_floor(value), reference(value)) << value;
  }
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
