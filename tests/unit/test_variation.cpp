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
 * `fvar` and `avar`, and the normalisation that turns a user's coordinates into
 * the ones every accessor takes.
 *
 * Built by hand rather than from a fixture, for the same reason the truncation
 * and `loca` suites are: almost every case here is a table that contradicts
 * itself, which fontTools will not write. The values that matter are checked
 * twice elsewhere - `ttx_diff` reads every table of every variable font in the
 * oracle image against fontTools, and `var_diff` normalises a location against
 * fontTools *and* FreeType - so what this file adds is the arms those never
 * reach.
 *
 * documentation/design.md section 7.7.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <string>
#include <vector>

#include <ghoti.io/font/variation.h>

namespace {

using gfnttest::put_u16;
using gfnttest::put_u32;

/** A 16.16 value from a whole number. */
int32_t fix(int whole) {
  return whole * 65536;
}

struct AxisSpec {
  const char * tag;
  int32_t min;
  int32_t def;
  int32_t max;
  uint16_t flags;
  uint16_t name_id;
};

struct InstanceSpec {
  uint16_t name_id;
  std::vector<int32_t> coordinates;
  int32_t postscript_name_id;  // -1: the record has none
};

void put_tag(std::vector<uint8_t> & out, const char * tag) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<uint8_t>(tag[i]));
  }
}

/** An `fvar` from its parts. Every size is spelled out so a test can break one. */
std::vector<uint8_t> build_fvar(const std::vector<AxisSpec> & axes,
    const std::vector<InstanceSpec> & instances, uint16_t axis_size = 20,
    int instance_size_delta = 0, uint16_t major = 1) {
  std::vector<uint8_t> out;
  bool with_postscript = false;

  for (const InstanceSpec & instance : instances) {
    with_postscript = with_postscript || instance.postscript_name_id >= 0;
  }
  const uint16_t instance_size = static_cast<uint16_t>(
      4 + 4 * axes.size() + (with_postscript ? 2 : 0) + instance_size_delta);
  put_u16(out, major);
  put_u16(out, 0);
  put_u16(out, 16);                                  // axesArrayOffset
  put_u16(out, 2);                                   // reserved
  put_u16(out, static_cast<uint16_t>(axes.size()));
  put_u16(out, axis_size);
  put_u16(out, static_cast<uint16_t>(instances.size()));
  put_u16(out, instance_size);
  for (const AxisSpec & axis : axes) {
    put_tag(out, axis.tag);
    put_u32(out, static_cast<uint32_t>(axis.min));
    put_u32(out, static_cast<uint32_t>(axis.def));
    put_u32(out, static_cast<uint32_t>(axis.max));
    put_u16(out, axis.flags);
    put_u16(out, axis.name_id);
  }
  for (const InstanceSpec & instance : instances) {
    put_u16(out, instance.name_id);
    put_u16(out, 0);
    for (int32_t coordinate : instance.coordinates) {
      put_u32(out, static_cast<uint32_t>(coordinate));
    }
    if (with_postscript) {
      put_u16(out, instance.postscript_name_id >= 0
          ? static_cast<uint16_t>(instance.postscript_name_id) : 0xFFFFu);
    }
  }
  return out;
}

/** One axis's segment map, as (from, to) pairs in 2.14. */
using SegmentMap = std::vector<std::pair<int16_t, int16_t>>;

std::vector<uint8_t> build_avar(const std::vector<SegmentMap> & maps,
    uint16_t major = 1, int axis_count_delta = 0) {
  std::vector<uint8_t> out;

  put_u16(out, major);
  put_u16(out, 0);
  put_u16(out, 0);
  put_u16(out, static_cast<uint16_t>(maps.size() + axis_count_delta));
  for (const SegmentMap & map : maps) {
    put_u16(out, static_cast<uint16_t>(map.size()));
    for (const auto & pair : map) {
      put_u16(out, static_cast<uint16_t>(pair.first));
      put_u16(out, static_cast<uint16_t>(pair.second));
    }
  }
  return out;
}

/** A face over the given variation tables, with one glyph and nothing else. */
struct Variable {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  Variable(const std::vector<uint8_t> & fvar,
      const std::vector<uint8_t> & avar = {}, const GFNT_Limits * limits = nullptr) {
    std::vector<gfnttest::Table> tables = {
        {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
        {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(1)},
    };
    if (!fvar.empty()) {
      tables.push_back({GFNT_TAG('f', 'v', 'a', 'r'), fvar});
    }
    if (!avar.empty()) {
      tables.push_back({GFNT_TAG('a', 'v', 'a', 'r'), avar});
    }
    bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, limits, nullptr, &face, &error), GFNT_OK);
  }
  ~Variable() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Variable(const Variable &) = delete;
  Variable & operator=(const Variable &) = delete;

  GFNT_Result normalise(const std::vector<int32_t> & user,
      std::vector<GFNT_F2Dot14> * out, size_t capacity = 8) {
    out->assign(capacity, 0x7777);
    gfnt_error_clear(&error);
    GFNT_Result result = gfnt_face_normalize(face, user.data(), user.size(),
        out->data(), capacity, &error);
    return result;
  }
};

const AxisSpec kWeight = {"wght", fix(100), fix(400), fix(900), 0, 256};
const AxisSpec kWidth = {"wdth", fix(75), fix(100), fix(125), 1, 257};

}  // namespace

TEST(Variation, AFaceWithNoFvarHasNoDesignSpace) {
  Variable font({});
  size_t count = 99;
  GFNT_Axis axis{};
  std::vector<GFNT_F2Dot14> out;

  EXPECT_FALSE(gfnt_face_is_variable(font.face));
  EXPECT_EQ(gfnt_face_axis_count(font.face, &count, &font.error), GFNT_OK);
  EXPECT_EQ(count, 0u);
  count = 99;
  EXPECT_EQ(gfnt_face_instance_count(font.face, &count, &font.error), GFNT_OK);
  EXPECT_EQ(count, 0u);
  // Counting nothing is an answer; asking for the first of it is not.
  EXPECT_EQ(gfnt_face_axis_at(font.face, 0, &axis, &font.error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(font.normalise({}, &out), GFNT_ERR_UNSUPPORTED);
  EXPECT_FALSE(gfnt_face_is_variable(nullptr));
}

TEST(Variation, AxesAndInstancesAreReadAsTheFileStatesThem) {
  Variable font(build_fvar({kWeight, kWidth}, {
      {300, {fix(300), fix(100)}, -1},
      {301, {fix(700), fix(75)}, 400},
  }));
  size_t count = 0;
  GFNT_Axis axis{};
  GFNT_NamedInstance instance{};

  EXPECT_TRUE(gfnt_face_is_variable(font.face));
  ASSERT_EQ(gfnt_face_axis_count(font.face, &count, &font.error), GFNT_OK);
  EXPECT_EQ(count, 2u);
  ASSERT_EQ(gfnt_face_axis_at(font.face, 1, &axis, &font.error), GFNT_OK);
  EXPECT_EQ(axis.tag, GFNT_TAG('w', 'd', 't', 'h'));
  EXPECT_EQ(axis.min, fix(75));
  EXPECT_EQ(axis.def, fix(100));
  EXPECT_EQ(axis.max, fix(125));
  EXPECT_EQ(axis.flags, GFNT_AXIS_HIDDEN);
  EXPECT_EQ(axis.name_id, 257);
  EXPECT_EQ(gfnt_face_axis_at(font.face, 2, &axis, &font.error),
      GFNT_ERR_INVALID);

  ASSERT_EQ(gfnt_face_instance_count(font.face, &count, &font.error), GFNT_OK);
  EXPECT_EQ(count, 2u);
  ASSERT_EQ(gfnt_face_instance_at(font.face, 1, &instance, &font.error), GFNT_OK);
  EXPECT_EQ(instance.name_id, 301);
  EXPECT_EQ(instance.postscript_name_id, 400);
  ASSERT_EQ(instance.coordinate_count, 2u);
  EXPECT_EQ(instance.coordinates[0], fix(700));
  EXPECT_EQ(instance.coordinates[1], fix(75));
  // The first instance states no PostScript name, and says so with the format's
  // own value rather than with a zero, which is a legal name ID.
  ASSERT_EQ(gfnt_face_instance_at(font.face, 0, &instance, &font.error), GFNT_OK);
  EXPECT_EQ(instance.postscript_name_id, GFNT_INSTANCE_NO_POSTSCRIPT_NAME);
  EXPECT_EQ(gfnt_face_instance_at(font.face, 2, &instance, &font.error),
      GFNT_ERR_INVALID);
}

TEST(Variation, NormalisationIsLinearOnEachSideOfTheDefault) {
  // 100..400..900: the two sides have different spans, which is why a normalised
  // coordinate is not simply (v - min) / (max - min).
  Variable font(build_fvar({kWeight}, {}));
  struct Case {
    int32_t user;
    GFNT_F2Dot14 normalised;
    const char * why;
  };
  const Case cases[] = {
    {fix(400), 0, "the default is zero"},
    {fix(100), -16384, "the minimum is -1"},
    {fix(900), 16384, "the maximum is 1"},
    {fix(250), -8192, "halfway to the minimum is -1/2"},
    {fix(650), 8192, "halfway to the maximum is 1/2"},
    // 1/3 is not representable, and the rule is round to nearest: -5461.33.
    {fix(300), -5461, "a third of the lower span"},
    {fix(50), -16384, "below the minimum clamps"},
    {fix(2000), 16384, "above the maximum clamps"},
  };
  for (const Case & test : cases) {
    std::vector<GFNT_F2Dot14> out;

    ASSERT_EQ(font.normalise({test.user}, &out), GFNT_OK) << test.why;
    EXPECT_EQ(out[0], test.normalised) << test.why;
    // One value per axis and no more: the rest of the caller's array is theirs.
    EXPECT_EQ(out[1], 0x7777) << test.why;
  }
}

TEST(Variation, TheRoundingToTwoPointFourteenIsHalfUpAndNotTruncation) {
  // The 16.16 value is exactly the user coordinate on an axis whose span is one,
  // so what is being tested is the last step alone: two bits are dropped, and
  // the rule is the one FreeType uses - add two, then floor - which rounds a tie
  // towards positive infinity and not away from zero.
  Variable font(build_fvar({{"test", fix(-1), 0, fix(1), 0, 0}}, {}));
  struct Case {
    int32_t user;
    GFNT_F2Dot14 normalised;
    const char * why;
  };
  const Case cases[] = {
    {1, 0, "a quarter of a unit rounds down"},
    {2, 1, "a positive tie rounds up"},
    {3, 1, "three quarters rounds up"},
    {-1, 0, "a negative quarter rounds up to zero"},
    {-2, 0, "a negative tie rounds towards positive infinity, not away from zero"},
    {-3, -1, "negative three quarters rounds to -1"},
    // 2/3 of the span is 43,691 in 16.16 and 10,922.75 in 2.14, which truncation
    // takes to 10,922: the case where the two rules differ for a value that is
    // not a tie.
    {43691, 10923, "two thirds"},
  };
  for (const Case & test : cases) {
    std::vector<GFNT_F2Dot14> out;

    ASSERT_EQ(font.normalise({test.user}, &out), GFNT_OK) << test.why;
    EXPECT_EQ(out[0], test.normalised) << test.why;
  }
}

TEST(Variation, AnAxisNotGivenTakesItsDefault) {
  Variable font(build_fvar({kWeight, kWidth}, {}));
  std::vector<GFNT_F2Dot14> out;

  // Only weight is set; width stays where the designer put it.
  ASSERT_EQ(font.normalise({fix(900)}, &out), GFNT_OK);
  EXPECT_EQ(out[0], 16384);
  EXPECT_EQ(out[1], 0);
  ASSERT_EQ(font.normalise({}, &out), GFNT_OK);
  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(out[1], 0);
  // Two axes, in fvar's order: the second coordinate is width.
  ASSERT_EQ(font.normalise({fix(400), fix(125)}, &out), GFNT_OK);
  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(out[1], 16384);
}

TEST(Variation, NormaliseRefusesWhatItCannotAnswerFor) {
  Variable font(build_fvar({kWeight}, {}));
  std::vector<GFNT_F2Dot14> out;

  // More coordinates than axes: the extra one names an axis the face lacks.
  EXPECT_EQ(font.normalise({fix(400), fix(1)}, &out), GFNT_ERR_INVALID);
  // An output too small to hold one per axis.
  EXPECT_EQ(font.normalise({}, &out, 0), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_normalize(nullptr, nullptr, 0, out.data(), 8, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_normalize(font.face, nullptr, 1, out.data(), 8, nullptr),
      GFNT_ERR_INVALID);
}

TEST(Variation, AvarBendsTheNormalisedScale) {
  // The weight axis again, with a map that puts the lower half-way point at -1/4
  // rather than -1/2. Only the two sides the map names are bent.
  Variable font(build_fvar({kWeight}, {}), build_avar({
      {{-16384, -16384}, {-8192, -4096}, {0, 0}, {16384, 16384}},
  }));
  std::vector<GFNT_F2Dot14> out;

  ASSERT_EQ(font.normalise({fix(250)}, &out), GFNT_OK);
  EXPECT_EQ(out[0], -4096) << "a point the map names";
  // Halfway between -1 and the mapped half-way point: -3/4 maps to -5/8.
  ASSERT_EQ(font.normalise({fix(175)}, &out), GFNT_OK);
  EXPECT_EQ(out[0], -10240);
  ASSERT_EQ(font.normalise({fix(900)}, &out), GFNT_OK);
  EXPECT_EQ(out[0], 16384);
  ASSERT_EQ(font.normalise({fix(650)}, &out), GFNT_OK);
  EXPECT_EQ(out[0], 8192) << "the other side has no bend";
}

TEST(Variation, AvarSeesTheClampedValueNotTheOneTheCallerAsked) {
  // Without a map the final clamp to -1..1 hides an axis that forgot to clamp: a
  // weight of 50 on a 100..900 axis would normalise to -1.17 and be clamped
  // afterwards to the same -1. A map in between is where the two differ, because
  // it would carry -1.17 past its first pair as an offset instead of seeing -1.
  Variable font(build_fvar({kWeight}, {}), build_avar({
      {{-16384, -8192}, {0, 0}, {16384, 16384}},
  }));
  std::vector<GFNT_F2Dot14> out;

  ASSERT_EQ(font.normalise({fix(50)}, &out), GFNT_OK);
  EXPECT_EQ(out[0], -8192) << "clamped to the minimum, then mapped";

  // And the other end, which is a separate clamp.
  Variable top(build_fvar({kWeight}, {}), build_avar({
      {{-16384, -16384}, {0, 0}, {16384, 8192}},
  }));
  ASSERT_EQ(top.normalise({fix(2000)}, &out), GFNT_OK);
  EXPECT_EQ(out[0], 8192) << "clamped to the maximum, then mapped";
}

TEST(Variation, AvarCarriesItsOffsetBeyondTheEndsOfAMapThatDoesNotCoverTheAxis) {
  // A map that begins at -1/2 does not say what -1 maps to. The offset of its
  // first pair carries on, which is fontTools' rule; the alternative is to
  // refuse a font that every other reader draws.
  Variable font(build_fvar({kWeight}, {}), build_avar({
      {{-8192, -4096}, {0, 0}, {8192, 4096}},
  }));
  std::vector<GFNT_F2Dot14> out;

  ASSERT_EQ(font.normalise({fix(100)}, &out), GFNT_OK);
  EXPECT_EQ(out[0], -12288) << "-1 plus the first pair's +1/4";
  ASSERT_EQ(font.normalise({fix(900)}, &out), GFNT_OK);
  EXPECT_EQ(out[0], 12288) << "1 minus the last pair's 1/4";
  // And a map whose first pair is the axis's own minimum, mapped elsewhere: the
  // pair itself is applied, not just the interior.
  Variable moved(build_fvar({kWeight}, {}), build_avar({
      {{-16384, -12288}, {0, 0}, {16384, 16384}},
  }));
  ASSERT_EQ(moved.normalise({fix(100)}, &out), GFNT_OK);
  EXPECT_EQ(out[0], -12288);
}

TEST(Variation, AnAxisWithNoPairsInItsAvarMapIsLeftAlone) {
  Variable font(build_fvar({kWeight, kWidth}, {}), build_avar({
      {{-16384, -16384}, {0, 0}, {16384, 8192}},
      {},
  }));
  std::vector<GFNT_F2Dot14> out;

  ASSERT_EQ(font.normalise({fix(900), fix(125)}, &out), GFNT_OK);
  EXPECT_EQ(out[0], 8192) << "the mapped axis";
  EXPECT_EQ(out[1], 16384) << "an empty map is the identity";
}

TEST(Variation, AFvarThatContradictsItselfIsRefusedByName) {
  struct Case {
    const char * why;
    std::vector<uint8_t> fvar;
    GFNT_Result expected;
  };
  std::vector<uint8_t> truncated = build_fvar({kWeight}, {});
  truncated.resize(10);
  std::vector<uint8_t> past_the_end = build_fvar({kWeight}, {{1, {fix(400)}, -1}});
  past_the_end.resize(past_the_end.size() - 2);
  const Case cases[] = {
    {"shorter than its own header", truncated, GFNT_ERR_CORRUPT},
    {"an axis record that is not twenty bytes",
        build_fvar({kWeight}, {}, 19), GFNT_ERR_CORRUPT},
    // No instances, so the extent check has nothing to say and only the size
    // rule can refuse it. With one instance the declared size also made the table
    // too short, and a test that passed through that check would have kept passing
    // with the size rule deleted.
    {"an instance size that matches no axis count",
        build_fvar({kWeight}, {}, 20, 1), GFNT_ERR_CORRUPT},
    {"a default below its own minimum",
        build_fvar({{"wght", fix(400), fix(100), fix(900), 0, 0}}, {}),
        GFNT_ERR_CORRUPT},
    {"a default above its own maximum",
        build_fvar({{"wght", fix(100), fix(1000), fix(900), 0, 0}}, {}),
        GFNT_ERR_CORRUPT},
    {"an instance that runs past the table", past_the_end, GFNT_ERR_CORRUPT},
    {"a major version this library does not read",
        build_fvar({kWeight}, {}, 20, 0, 2), GFNT_ERR_UNSUPPORTED},
  };
  for (const Case & test : cases) {
    Variable font(test.fvar);
    size_t count = 0;

    EXPECT_EQ(gfnt_face_axis_count(font.face, &count, &font.error), test.expected)
        << test.why;
    ASSERT_NE(font.error.message, nullptr) << test.why;
    EXPECT_EQ(font.error.table, GFNT_TAG('f', 'v', 'a', 'r')) << test.why;
    // The refusal is the table's, remembered: every question about the design
    // space gets it, and none gets a half-built answer.
    EXPECT_EQ(gfnt_face_instance_count(font.face, &count, nullptr), test.expected)
        << test.why;
    std::vector<GFNT_F2Dot14> out;
    EXPECT_EQ(font.normalise({}, &out), test.expected) << test.why;
  }
}

TEST(Variation, MoreAxesThanTheLimitAllowsIsALimitNotACorruption) {
  GFNT_Limits limits;

  gfnt_limits_default(&limits);
  limits.max_axes = 1;
  Variable font(build_fvar({kWeight, kWidth}, {}), {}, &limits);
  size_t count = 0;

  EXPECT_EQ(gfnt_face_axis_count(font.face, &count, &font.error), GFNT_ERR_LIMIT);
}

TEST(Variation, AnAvarThatIsNotAFunctionOfItsAxisIsRefused) {
  struct Case {
    const char * why;
    std::vector<uint8_t> avar;
    GFNT_Result expected;
  };
  std::vector<uint8_t> short_map = build_avar({{{0, 0}, {16384, 16384}}});
  short_map.resize(short_map.size() - 2);
  const Case cases[] = {
    {"an axis count that is not fvar's",
        build_avar({{{0, 0}}}, 1, 1), GFNT_ERR_CORRUPT},
    {"source coordinates that do not increase",
        build_avar({{{0, 0}, {0, 100}}}), GFNT_ERR_CORRUPT},
    {"source coordinates that decrease",
        build_avar({{{8192, 0}, {0, 100}}}), GFNT_ERR_CORRUPT},
    {"a map that runs past the table", short_map, GFNT_ERR_CORRUPT},
    // Version 2 re-maps one axis as a function of the others. Read as version 1
    // it would apply the segment maps and silently drop that.
    {"avar version 2", build_avar({{}}, 2), GFNT_ERR_UNSUPPORTED},
  };
  for (const Case & test : cases) {
    Variable font(build_fvar({kWeight}, {}), test.avar);
    std::vector<GFNT_F2Dot14> out;

    EXPECT_EQ(font.normalise({fix(900)}, &out), test.expected) << test.why;
    ASSERT_NE(font.error.message, nullptr) << test.why;
    EXPECT_EQ(font.error.table, GFNT_TAG('a', 'v', 'a', 'r')) << test.why;
    // The design space is still readable: it is the *mapping* that was refused,
    // and a face does not lose its axes because its avar is bad.
    size_t count = 0;
    EXPECT_EQ(gfnt_face_axis_count(font.face, &count, nullptr), GFNT_OK)
        << test.why;
    EXPECT_EQ(count, 1u) << test.why;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
