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

#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/variation.h>

#include "../../src/glyf/glyf.h"

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

// ---------------------------------------------------------------------------
// gvar
//
// Every expectation below is worked out by hand from the specification's own
// rules and written as a number, never computed by the code under test. The
// encoder in this file is a convenience and is itself checked once against
// literal bytes (TheTestEncoderWritesWhatTheSpecificationShows), because a test
// whose encoder and decoder share a misreading would agree with itself about
// everything. The independent encoder is fontTools: `variable-gvar.ttf` is its
// work, and `var_diff.py` compares this library against it glyph by glyph.
// ---------------------------------------------------------------------------

namespace {

constexpr uint16_t kEmbeddedPeak = 0x8000;
constexpr uint16_t kIntermediate = 0x4000;
constexpr uint16_t kPrivatePoints = 0x2000;
constexpr uint16_t kSharedPoints = 0x8000;
constexpr uint16_t kArgsAreXy = 0x0002;

/** One tuple of a glyph's variation data, in the file's own terms. */
struct Tuple {
  std::vector<int16_t> peak{};         ///< Embedded peak, or empty for a shared one.
  int shared_index = 0;                ///< Which shared tuple, when `peak` is empty.
  std::vector<int16_t> start{};        ///< Intermediate region, or empty.
  std::vector<int16_t> end{};
  std::vector<uint8_t> data{};         ///< The tuple's serialised bytes, verbatim.
  bool private_points = false;
};

/** A `GlyphVariationData`, from its tuples and an optional shared point list. */
std::vector<uint8_t> glyph_variation_data(const std::vector<Tuple> & tuples,
    const std::vector<uint8_t> & shared_points = {}, bool shared = false) {
  std::vector<uint8_t> headers;
  std::vector<uint8_t> serialised = shared_points;

  for (const Tuple & tuple : tuples) {
    uint16_t index = static_cast<uint16_t>(tuple.shared_index);

    if (!tuple.peak.empty()) {
      index = kEmbeddedPeak;
    }
    if (!tuple.start.empty()) {
      index |= kIntermediate;
    }
    if (tuple.private_points) {
      index |= kPrivatePoints;
    }
    put_u16(headers, static_cast<uint16_t>(tuple.data.size()));
    put_u16(headers, index);
    for (int16_t value : tuple.peak) {
      put_u16(headers, static_cast<uint16_t>(value));
    }
    for (int16_t value : tuple.start) {
      put_u16(headers, static_cast<uint16_t>(value));
    }
    for (int16_t value : tuple.end) {
      put_u16(headers, static_cast<uint16_t>(value));
    }
    serialised.insert(serialised.end(), tuple.data.begin(), tuple.data.end());
  }
  std::vector<uint8_t> out;

  put_u16(out, static_cast<uint16_t>(tuples.size() | (shared ? kSharedPoints : 0)));
  put_u16(out, static_cast<uint16_t>(4 + headers.size()));
  out.insert(out.end(), headers.begin(), headers.end());
  out.insert(out.end(), serialised.begin(), serialised.end());
  return out;
}

struct GvarSpec {
  std::vector<std::vector<int16_t>> shared_tuples{};
  std::vector<std::vector<uint8_t>> glyphs{};   ///< Empty entries have no variation.
  bool long_offsets = false;
  uint16_t major = 1;
  int axis_count = 1;
  int glyph_count_delta = 0;
};

std::vector<uint8_t> build_gvar(const GvarSpec & spec) {
  std::vector<uint8_t> data;
  std::vector<uint32_t> offsets;

  for (const std::vector<uint8_t> & glyph : spec.glyphs) {
    offsets.push_back(static_cast<uint32_t>(data.size()));
    data.insert(data.end(), glyph.begin(), glyph.end());
    // The short form halves an offset, so every one has to be even.
    while (data.size() % 2 != 0) {
      data.push_back(0);
    }
  }
  offsets.push_back(static_cast<uint32_t>(data.size()));

  const size_t offsets_bytes = offsets.size() * (spec.long_offsets ? 4 : 2);
  const size_t shared_offset = 20 + offsets_bytes;
  const size_t shared_bytes = spec.shared_tuples.size() * spec.axis_count * 2;
  std::vector<uint8_t> out;

  put_u16(out, spec.major);
  put_u16(out, 0);
  put_u16(out, static_cast<uint16_t>(spec.axis_count));
  put_u16(out, static_cast<uint16_t>(spec.shared_tuples.size()));
  put_u32(out, static_cast<uint32_t>(shared_offset));
  put_u16(out, static_cast<uint16_t>(spec.glyphs.size() + spec.glyph_count_delta));
  put_u16(out, spec.long_offsets ? 1 : 0);
  put_u32(out, static_cast<uint32_t>(shared_offset + shared_bytes));
  for (uint32_t offset : offsets) {
    if (spec.long_offsets) {
      put_u32(out, offset);
    }
    else {
      put_u16(out, static_cast<uint16_t>(offset / 2));
    }
  }
  for (const std::vector<int16_t> & tuple : spec.shared_tuples) {
    for (int16_t value : tuple) {
      put_u16(out, static_cast<uint16_t>(value));
    }
  }
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

using Points = std::vector<std::pair<int, int>>;

/** A font with `glyf`, `loca`, an `fvar` of one axis, and the given `gvar`. */
struct Moving {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  Moving(const std::vector<std::vector<uint8_t>> & glyphs,
      const std::vector<uint8_t> & gvar, int axes = 1) {
    std::vector<uint8_t> glyf;
    std::vector<uint8_t> loca;
    bool long_form = false;
    std::vector<std::pair<uint16_t, int16_t>> metrics;
    std::vector<AxisSpec> specs = {kWeight, kWidth};

    specs.resize(axes);
    gfnttest::build_glyf_and_loca(glyphs, &glyf, &loca, &long_form);
    for (size_t i = 0; i < glyphs.size(); ++i) {
      metrics.push_back({500, 0});
    }
    std::vector<gfnttest::Table> tables = {
        {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, long_form ? 1 : 0)},
        {GFNT_TAG('h', 'h', 'e', 'a'),
            gfnttest::build_hhea(800, -200, 0, static_cast<uint16_t>(glyphs.size()))},
        {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics, {})},
        {GFNT_TAG('m', 'a', 'x', 'p'),
            gfnttest::build_maxp(static_cast<uint16_t>(glyphs.size()))},
        {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
        {GFNT_TAG('l', 'o', 'c', 'a'), loca},
        {GFNT_TAG('f', 'v', 'a', 'r'), build_fvar(specs, {})},
    };
    if (!gvar.empty()) {
      tables.push_back({GFNT_TAG('g', 'v', 'a', 'r'), gvar});
    }
    bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
  }
  ~Moving() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Moving(const Moving &) = delete;
  Moving & operator=(const Moving &) = delete;

  /** The outline's points in 26.6, or a failure's result through @p result. */
  Points points(uint32_t glyph, const std::vector<GFNT_F2Dot14> & coordinates,
      GFNT_Result * result = nullptr) {
    GFNT_Variation variation{coordinates.data(), coordinates.size()};
    GFNT_Outline * outline = nullptr;
    Points out;

    gfnt_error_clear(&error);
    GFNT_Result got = gfnt_face_glyph_outline(face, glyph,
        coordinates.empty() ? nullptr : &variation, nullptr, &outline, &error);
    if (result) {
      *result = got;
    }
    if (got == GFNT_OK) {
      for (size_t i = 0; i < gfnt_outline_point_count(outline); ++i) {
        GFNT_Point at;

        EXPECT_EQ(gfnt_outline_point_at(outline, i, &at, nullptr), GFNT_OK);
        out.push_back({at.x, at.y});
      }
    }
    gfnt_outline_destroy(outline);
    return out;
  }
};

/** A square: (0,0) (0,100) (100,100) (100,0), all on-curve. */
std::vector<uint8_t> square() {
  return gfnttest::build_glyf_glyph({{{0, 0, true}, {0, 100, true},
                                       {100, 100, true}, {100, 0, true}}});
}

/** The same square scaled to 64ths, for comparing against an outline. */
Points whole(std::initializer_list<std::pair<int, int>> units) {
  Points out;

  for (const auto & unit : units) {
    out.push_back({unit.first * 64, unit.second * 64});
  }
  return out;
}

const std::vector<GFNT_F2Dot14> kFull = {16384};
const std::vector<GFNT_F2Dot14> kHalf = {8192};

}  // namespace

TEST(Gvar, TheTestEncoderWritesWhatTheSpecificationShows) {
  // x deltas 10, 10, -10, -10 then four phantom zeros; y all zero. The packed
  // form is a run of four bytes (control 0x03), a run of four zeros (0x80 | 3),
  // and a run of eight zeros (0x80 | 7). The embedded peak is 1.0, which is 0x4000.
  Tuple tuple;

  tuple.peak = {0x4000};
  tuple.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  const std::vector<uint8_t> expected = {
      0x00, 0x01,              // one tuple, no shared points
      0x00, 0x0A,              // serialised data begins at byte 10
      0x00, 0x07,              // this tuple's data is seven bytes
      0x80, 0x00,              // tupleIndex: embedded peak
      0x40, 0x00,              // peak 1.0
      0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87,
  };
  EXPECT_EQ(glyph_variation_data({tuple}), expected);
}

TEST(Gvar, EveryPointMovesByItsDeltaScaledByHowFarIntoTheTupleTheLocationIs) {
  Tuple tuple;

  tuple.peak = {0x4000};
  tuple.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  GvarSpec spec;
  spec.glyphs = {{}, glyph_variation_data({tuple})};
  Moving font({square(), square()}, build_gvar(spec));

  // The default instance is the glyph as stored.
  EXPECT_EQ(font.points(1, {0}), whole({{0, 0}, {0, 100}, {100, 100}, {100, 0}}));
  // At the peak, the whole delta.
  EXPECT_EQ(font.points(1, kFull),
      whole({{10, 0}, {10, 100}, {90, 100}, {90, 0}}));
  // Halfway to the peak, half of it. 5 is exact in 26.6 and so is every value
  // here: a half of a whole delta is a whole number of 64ths.
  EXPECT_EQ(font.points(1, kHalf),
      whole({{5, 0}, {5, 100}, {95, 100}, {95, 0}}));
  // On the other side of the default the tuple's region does not reach.
  EXPECT_EQ(font.points(1, {-16384}),
      whole({{0, 0}, {0, 100}, {100, 100}, {100, 0}}));
  // A glyph with no variation data does not vary at any location.
  EXPECT_EQ(font.points(0, kFull),
      whole({{0, 0}, {0, 100}, {100, 100}, {100, 0}}));
}

TEST(Gvar, ALongOffsetArrayIsTheSameTableWrittenWider) {
  Tuple tuple;

  tuple.peak = {0x4000};
  tuple.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  GvarSpec spec;
  spec.glyphs = {{}, glyph_variation_data({tuple})};
  spec.long_offsets = true;
  Moving font({square(), square()}, build_gvar(spec));

  EXPECT_EQ(font.points(1, kFull),
      whole({{10, 0}, {10, 100}, {90, 100}, {90, 0}}));
}

TEST(Gvar, PointsATupleDoesNotNameTakeTheirNeighboursDeltasInProportion) {
  // Points 0 and 2 are named: p0 moves (20, 0) and p2 moves (0, 40). The
  // square's p1 = (0, 100) and p3 = (100, 0) are inferred, per axis separately:
  //
  //   p1.x = 0 is at p0.x, so it takes p0's x delta, 20.
  //   p1.y = 100 is at p2.y, so it takes p2's y delta, 40.
  //   p3.x = 100 is at p2.x, so it takes p2's x delta, 0.
  //   p3.y = 0 is at p0.y, so it takes p0's y delta, 0.
  //
  // Private point list: two points, numbers 0 and 2 stored as 0 and +2.
  Tuple tuple;

  tuple.peak = {0x4000};
  tuple.private_points = true;
  tuple.data = {
      0x02,                      // two points
      0x01, 0x00, 0x02,          // one run of two byte values: 0, then +2
      0x01, 0x14, 0x00,          // x deltas for the two named points: 20, 0
      0x01, 0x00, 0x28,          // y deltas: 0, 40
  };
  GvarSpec spec;
  spec.glyphs = {{}, glyph_variation_data({tuple})};
  Moving font({square(), square()}, build_gvar(spec));

  EXPECT_EQ(font.points(1, kFull),
      whole({{20, 0}, {20, 140}, {100, 140}, {100, 0}}));
  // Half-way: every delta halves, including the inferred ones.
  EXPECT_EQ(font.points(1, kHalf),
      whole({{10, 0}, {10, 120}, {100, 120}, {100, 0}}));
}

TEST(Gvar, AnInferredPointBetweenTwoNamedOnesIsInterpolatedLinearly) {
  // (0,0) (40,0) (100,0) (100,100). p0 moves +10 and p2 moves +70 in x; p1 sits
  // 40 of the way from p0's x to p2's, so it moves 10 + 0.4 * 60 = 34. p3 is at
  // p2's x and takes p2's whole delta, 70.
  Tuple tuple;

  tuple.peak = {0x4000};
  tuple.private_points = true;
  tuple.data = {
      0x02, 0x01, 0x00, 0x02,    // points 0 and 2
      0x01, 0x0A, 0x46,          // x: 10, 70
      0x01, 0x00, 0x00,          // y: 0, 0
  };
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple})};
  Moving font({gfnttest::build_glyf_glyph({{{0, 0, true}, {40, 0, true},
                                             {100, 0, true}, {100, 100, true}}})},
      build_gvar(spec));

  EXPECT_EQ(font.points(0, kFull),
      whole({{10, 0}, {74, 0}, {170, 0}, {170, 100}}));
}

TEST(Gvar, OneNamedPointInAContourMovesTheWholeContour) {
  Tuple tuple;

  tuple.peak = {0x4000};
  tuple.private_points = true;
  tuple.data = {
      0x01, 0x00, 0x02,          // one point, number 2
      0x00, 0x0F,                // x: one run of one byte value, 15
      0x00, 0xF9,                // y: -7
  };
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple})};
  Moving font({square()}, build_gvar(spec));

  EXPECT_EQ(font.points(0, kFull),
      whole({{15, -7}, {15, 93}, {115, 93}, {115, -7}}));
}

TEST(Gvar, SharedPointNumbersAreReadOnceAndUsedByEveryTupleThatDoesNotCarryItsOwn) {
  // Shared points: 0 and 2. Two tuples, one peak at +1 and one at -1, each naming
  // only x deltas for those two points. The peaks are *shared tuples* 0 and 1, so
  // this also covers a header with no embedded peak.
  Tuple up;
  Tuple down;

  up.shared_index = 0;
  up.data = {0x01, 0x14, 0x00,   // x: 20, 0
             0x01, 0x00, 0x00};  // y: 0, 0
  down.shared_index = 1;
  down.data = {0x01, 0xF6, 0x00, // x: -10, 0
               0x01, 0x00, 0x00};
  GvarSpec spec;
  spec.shared_tuples = {{16384}, {-16384}};
  spec.glyphs = {glyph_variation_data({up, down},
      {0x02, 0x01, 0x00, 0x02}, true)};
  Moving font({square()}, build_gvar(spec));

  // p1 = (0,100): x at p0.x, takes p0's 20. p3 = (100,0): at p2.x, takes 0.
  EXPECT_EQ(font.points(0, kFull),
      whole({{20, 0}, {20, 100}, {100, 100}, {100, 0}}));
  EXPECT_EQ(font.points(0, {-16384}),
      whole({{-10, 0}, {-10, 100}, {100, 100}, {100, 0}}));
}

TEST(Gvar, AnIntermediateRegionRampsUpToItsPeakAndDownAgain) {
  // Region 0.25 .. 0.5 .. 1.0 on the one axis, all points +16 in x.
  Tuple tuple;

  tuple.peak = {8192};
  tuple.start = {4096};
  tuple.end = {16384};
  tuple.data = {0x07, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple})};
  Moving font({square()}, build_gvar(spec));
  struct Case {
    GFNT_F2Dot14 at;
    int shift;
  };
  // 0.25 and 1.0 are the region's edges, where the scalar is zero; 0.375 and
  // 0.75 are half way up and half way down; 0.5 is the peak.
  const Case cases[] = {{4096, 0}, {6144, 8}, {8192, 16}, {12288, 8},
                        {16384, 0}, {2048, 0}, {0, 0}};
  for (const Case & test : cases) {
    EXPECT_EQ(font.points(0, {test.at}),
        whole({{test.shift, 0}, {test.shift, 100}, {100 + test.shift, 100},
               {100 + test.shift, 0}})) << "at " << test.at;
  }
}

TEST(Gvar, TwoAxesMultiplyTheirScalars) {
  // A corner tuple: peak (1, 1). At (0.5, 0.5) the scalar is 0.5 * 0.5 = 0.25 and
  // at (1, 0.5) it is 1 * 0.5; an axis at its default zeroes the whole tuple.
  Tuple tuple;

  tuple.peak = {16384, 16384};
  tuple.data = {0x07, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x87};
  GvarSpec spec;
  spec.axis_count = 2;
  spec.glyphs = {glyph_variation_data({tuple})};
  Moving font({square()}, build_gvar(spec), 2);

  EXPECT_EQ(font.points(0, {8192, 8192}),
      whole({{16, 0}, {16, 100}, {116, 100}, {116, 0}}));
  EXPECT_EQ(font.points(0, {16384, 8192}),
      whole({{32, 0}, {32, 100}, {132, 100}, {132, 0}}));
  EXPECT_EQ(font.points(0, {16384, 0}),
      whole({{0, 0}, {0, 100}, {100, 100}, {100, 0}}));
  // Fewer coordinates than axes: the rest are defaults, so the product is zero.
  EXPECT_EQ(font.points(0, {16384}),
      whole({{0, 0}, {0, 100}, {100, 100}, {100, 0}}));
}

TEST(Gvar, ADeltaTimesAScalarThatIsNotAWholeNumberKeepsItsFraction) {
  // 25 units at a scalar of 1/3 is 8 and a third, which 26.6 holds as 533/64
  // (8.328) rounded from 533.33: the point lands on a 64th, not on a font unit.
  Tuple tuple;

  tuple.peak = {16384};
  tuple.data = {0x07, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple})};
  Moving font({square()}, build_gvar(spec));
  // 16384 / 3 = 5461.33; scalar 5461/16384. 25 * 5461 / 16384 = 8.3328 units,
  // which is 533.3 sixty-fourths.
  const Points got = font.points(0, {5461});

  ASSERT_EQ(got.size(), 4u);
  EXPECT_EQ(got[0].first, 533);
  EXPECT_EQ(got[1].first, 533);
  EXPECT_EQ(got[2].first, 100 * 64 + 533);
}

TEST(Gvar, ACompositeComponentsOffsetsMoveAndItsPhantomPointsAreIgnored) {
  // glyph 0: the square, which has no variation of its own. glyph 1: two copies of
  // it, placed at (10, 20) and (30, 40) by byte offsets. Its tuple has six
  // "points" - two components and four phantoms - and moves the first component
  // by (7, -3) and the second by (0, 9).
  Tuple tuple;

  tuple.peak = {16384};
  tuple.data = {
      0x00, 0x07,                // x[0] = 7
      0x84,                      // x[1..5] = 0 (a run of five zeros)
      0x01, 0xFD, 0x09,          // y[0] = -3, y[1] = 9
      0x83,                      // y[2..5] = 0
  };
  GvarSpec spec;
  spec.glyphs = {{}, glyph_variation_data({tuple})};
  Moving font({square(),
      gfnttest::build_glyf_composite({
          {0, kArgsAreXy, 10, 20},
          {0, kArgsAreXy, 30, 40},
      })}, build_gvar(spec));

  EXPECT_EQ(font.points(1, {0}),
      whole({{10, 20}, {10, 120}, {110, 120}, {110, 20},
             {30, 40}, {30, 140}, {130, 140}, {130, 40}}));
  EXPECT_EQ(font.points(1, kFull),
      whole({{17, 17}, {17, 117}, {117, 117}, {117, 17},
             {30, 49}, {30, 149}, {130, 149}, {130, 49}}));
}

TEST(Gvar, AComponentPlacedByMatchingPointsIgnoresItsDeltaAndInheritsItsParentsMove) {
  // glyph 0 is the square with a tuple that moves x by +10, +10, -10, -10 (the
  // first test's). glyph 1 is a composite of it twice: first by an offset of
  // (100, 100), then by matching its point 1 to the assembled outline's point 0.
  //
  // At +1 the varied square is (10,0) (10,100) (90,100) (90,0). The first
  // component's offset is moved by (5, 5) to (105, 105): its points are
  // (115,105) (115,205) (195,205) (195,105). The second is matched, so there is
  // no offset to move and its delta of (1000, 1000) is ignored: its point 1,
  // (10, 100), is put on the assembled point 0, (115, 105), so it moves by
  // (105, 5) and lands at (115,5) (115,105) (195,105) (195,5).
  Tuple square_tuple;
  Tuple composite_tuple;

  square_tuple.peak = {16384};
  square_tuple.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  composite_tuple.peak = {16384};
  composite_tuple.data = {
      0x00, 0x05,                // x[0] = 5
      0x40, 0x03, 0xE8,          // x[1] = 1000: a run of one word (0x40 | 0)
      0x83,                      // x[2..5] = 0
      0x00, 0x05,                // y[0] = 5
      0x40, 0x03, 0xE8,          // y[1] = 1000
      0x83,
  };
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({square_tuple}),
                 glyph_variation_data({composite_tuple})};
  Moving font({square(),
      gfnttest::build_glyf_composite({
          {0, kArgsAreXy, 100, 100},
          {0, 0, 0, 1},
      })}, build_gvar(spec));

  EXPECT_EQ(font.points(1, kFull),
      whole({{115, 105}, {115, 205}, {195, 205}, {195, 105},
             {115, 5}, {115, 105}, {195, 105}, {195, 5}}));
}

TEST(Gvar, AGlyphWithNoContoursAndVariationDataIsStillNothing) {
  // `space` carries phantom-point deltas for its advance and draws nothing; the
  // outline is empty and the call succeeds.
  Tuple tuple;

  tuple.peak = {16384};
  // x: four phantom zeros; y: four more. Both are runs of zeros (0x80 | 3).
  tuple.data = {0x83, 0x83};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple})};
  Moving font({gfnttest::build_glyf_glyph({})}, build_gvar(spec));

  GFNT_Result result = GFNT_ERR_INTERNAL;
  EXPECT_TRUE(font.points(0, kFull, &result).empty());
  EXPECT_EQ(result, GFNT_OK);
}

TEST(Gvar, APointListLongerThanOneByteUsesTheTwoByteCount) {
  // 200 points and a tuple naming all 200 explicitly - not by the empty-count
  // shorthand - so the count is 0x80C8 and the numbers are two runs, 128 and 72.
  std::vector<std::vector<gfnttest::GlyfPoint>> contour(1);
  Tuple tuple;

  for (int i = 0; i < 200; ++i) {
    contour[0].push_back({static_cast<int16_t>(i * 3),
                          static_cast<int16_t>(i % 2 ? 100 : 0), true});
  }
  tuple.peak = {16384};
  tuple.private_points = true;
  tuple.data = {0x80, 0xC8};                // two-byte count: 200
  tuple.data.push_back(0x7F);               // run of 128 byte values
  tuple.data.push_back(0x00);               // point 0
  for (int i = 1; i < 128; ++i) {
    tuple.data.push_back(0x01);             // each next point, +1
  }
  tuple.data.push_back(0x47);               // run of 72 byte values
  for (int i = 128; i < 200; ++i) {
    tuple.data.push_back(0x01);
  }
  // x: 200 values of +1 and y: 200 zeros. A *delta* run holds at most 64 values -
  // its count is the low six bits - where a point-number run holds 128, so these
  // are three runs of 64 and one of 8 each.
  for (int run = 0; run < 3; ++run) {
    tuple.data.push_back(0x3F);
    tuple.data.insert(tuple.data.end(), 64, 0x01);
  }
  tuple.data.push_back(0x07);
  tuple.data.insert(tuple.data.end(), 8, 0x01);
  for (int run = 0; run < 3; ++run) {
    tuple.data.push_back(0xBF);             // 64 zeros
  }
  tuple.data.push_back(0x87);               // 8 zeros
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple})};
  Moving font({gfnttest::build_glyf_glyph(contour)}, build_gvar(spec));

  const Points moved = font.points(0, kFull);

  ASSERT_EQ(moved.size(), 200u);
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(moved[i].first, (i * 3 + 1) * 64) << "point " << i;
    EXPECT_EQ(moved[i].second, (i % 2 ? 100 : 0) * 64) << "point " << i;
  }
}

TEST(Gvar, WordSizedDeltasAreSignedSixteenBit) {
  Tuple tuple;

  tuple.peak = {16384};
  // x: a run of four words (0x40 | 3): 1000, -1000, 300, -300; then four zeros.
  tuple.data = {0x43, 0x03, 0xE8, 0xFC, 0x18, 0x01, 0x2C, 0xFE, 0xD4, 0x83,
                0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple})};
  Moving font({square()}, build_gvar(spec));

  EXPECT_EQ(font.points(0, kFull),
      whole({{1000, 0}, {-1000, 100}, {400, 100}, {-200, 0}}));
}

TEST(Gvar, AVariationOnAFaceThatCannotHonourItIsRefusedRatherThanIgnored) {
  Tuple tuple;

  tuple.peak = {16384};
  tuple.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple})};
  GFNT_Result result = GFNT_OK;

  // fvar and glyf, no gvar: a design space with nothing that moves an outline
  // through it. The default instance still answers, and so does a variation that
  // moves nothing; anything else would be a shape for a location it is not at.
  {
    Moving font({square()}, {});

    EXPECT_FALSE(font.points(0, {0}, &result).empty());
    EXPECT_EQ(result, GFNT_OK);
    font.points(0, kFull, &result);
    EXPECT_EQ(result, GFNT_ERR_UNSUPPORTED);
    ASSERT_NE(font.error.message, nullptr);
    EXPECT_NE(std::string(font.error.message).find("gvar"), std::string::npos);
  }
  // More coordinates than axes is the caller's mistake.
  {
    Moving font({square()}, build_gvar(spec));

    font.points(0, {16384, 16384}, &result);
    EXPECT_EQ(result, GFNT_ERR_INVALID);
  }
  // Coordinates promised and not given.
  {
    Moving font({square()}, build_gvar(spec));
    GFNT_Variation variation{nullptr, 1};
    GFNT_Outline * outline = nullptr;

    EXPECT_EQ(gfnt_face_glyph_outline(font.face, 0, &variation, nullptr,
        &outline, &font.error), GFNT_ERR_INVALID);
  }
}

TEST(Gvar, AFaceWithNoDesignSpaceTakesNoCoordinates) {
  // No fvar at all: one coordinate is more than the zero axes the face has.
  std::vector<uint8_t> glyf;
  std::vector<uint8_t> loca;
  bool long_form = false;
  gfnttest::build_glyf_and_loca({square()}, &glyf, &loca, &long_form);
  const std::vector<uint8_t> bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, long_form ? 1 : 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, 1)},
      {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx({{500, 0}}, {})},
      {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(1)},
      {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
      {GFNT_TAG('l', 'o', 'c', 'a'), loca},
  });
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Outline * outline = nullptr;
  GFNT_F2Dot14 coordinate = 16384;
  GFNT_Variation variation{&coordinate, 1};

  ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
      GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr), GFNT_OK);
  EXPECT_EQ(gfnt_face_glyph_outline(face, 0, &variation, nullptr, &outline,
      nullptr), GFNT_ERR_INVALID);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
}

TEST(Gvar, ATableThatContradictsItselfCondemnsTheGlyphAndNotTheFace) {
  // Glyph 0's data is wrong in one way per case and glyph 1's is the good tuple
  // from the first test; glyph 1 must answer whatever glyph 0 is.
  Tuple good;

  good.peak = {16384};
  good.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  const std::vector<uint8_t> fine = glyph_variation_data({good});

  struct Case {
    const char * why;
    std::vector<uint8_t> bad;
  };
  Tuple private_out_of_range;
  private_out_of_range.peak = {16384};
  private_out_of_range.private_points = true;
  // One point, named by a run of one byte value: 9, in a glyph whose points (and
  // phantoms) are numbered 0 to 7. The run itself is well formed, so only the
  // range check can refuse it.
  private_out_of_range.data = {0x01, 0x00, 0x09, 0x00, 0x01, 0x00, 0x01, 0x00};
  Tuple more_points_than_exist;
  more_points_than_exist.peak = {16384};
  more_points_than_exist.private_points = true;
  // Nine numbers for a glyph with eight (four points and four phantoms): 0 to 7,
  // then 7 again, so every number is in range and the runs are complete - only the
  // count can refuse it, and a reader that sized its delta array from the glyph
  // and trusted the count would write past it. Both delta lists are supplied.
  more_points_than_exist.data = {
      0x09, 0x08, 0, 1, 1, 1, 1, 1, 1, 1, 0,
      0x08, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      0x08, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  };
  Tuple bad_shared_index;
  bad_shared_index.shared_index = 5;
  bad_shared_index.data = {0x83, 0x87};
  Tuple run_too_long;
  run_too_long.peak = {16384};
  // Nine values for a list of eight (four points and four phantoms).
  run_too_long.data = {0x08, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0x87};
  Tuple run_runs_past_the_tuple;
  run_runs_past_the_tuple.peak = {16384};
  run_runs_past_the_tuple.data = {0x07, 1, 1};
  std::vector<uint8_t> truncated_header = {0x00, 0x01, 0x00, 0x04, 0x00};
  std::vector<uint8_t> serialised_past_the_end = {0x00, 0x00, 0x00, 0x40};
  const Case cases[] = {
    {"a point number past the glyph's points", glyph_variation_data(
        {private_out_of_range})},
    {"a point list longer than the glyph has points", glyph_variation_data(
        {more_points_than_exist})},
    {"a shared tuple the table does not have", glyph_variation_data(
        {bad_shared_index})},
    {"a delta run longer than the list it is in", glyph_variation_data(
        {run_too_long})},
    {"a delta run that runs past its tuple's data", glyph_variation_data(
        {run_runs_past_the_tuple})},
    {"a tuple header that ends past the data", truncated_header},
    {"serialised data that begins past the end", serialised_past_the_end},
  };
  for (const Case & test : cases) {
    GvarSpec spec;
    GFNT_Result result = GFNT_OK;

    spec.glyphs = {test.bad, fine};
    Moving font({square(), square()}, build_gvar(spec));
    font.points(0, kFull, &result);
    EXPECT_EQ(result, GFNT_ERR_CORRUPT) << test.why;
    ASSERT_NE(font.error.message, nullptr) << test.why;
    EXPECT_EQ(font.error.table, GFNT_TAG('g', 'v', 'a', 'r')) << test.why;
    // M11: the next glyph is untouched by it, and so is the default instance.
    EXPECT_EQ(font.points(1, kFull),
        whole({{10, 0}, {10, 100}, {90, 100}, {90, 0}})) << test.why;
    EXPECT_FALSE(font.points(0, {0}).empty()) << test.why;
  }
}

TEST(Gvar, AnOffsetArrayThatRunsBackwardsOrPastTheTableCondemnsOnlyThatGlyph) {
  Tuple good;

  good.peak = {16384};
  good.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({good}), glyph_variation_data({good})};
  std::vector<uint8_t> table = build_gvar(spec);
  // Glyph 0's end offset (the second entry, at byte 22) set before its start.
  std::vector<uint8_t> backwards = table;
  backwards[22] = 0x00;
  backwards[23] = 0x00;
  backwards[20] = 0x00;
  backwards[21] = 0x01;
  // Glyph 1's end offset (the third entry, at byte 24) set far past the table.
  std::vector<uint8_t> past = table;
  past[24] = 0x7F;
  past[25] = 0xFF;
  GFNT_Result result = GFNT_OK;
  {
    Moving font({square(), square()}, backwards);

    font.points(0, kFull, &result);
    EXPECT_EQ(result, GFNT_ERR_CORRUPT);
    // The entry that is wrong is wrong for the glyph before it and the glyph
    // after it shares the boundary - so the later glyph reads from a start that
    // is also wrong. What is asserted is only that the face still loads and the
    // default instance still answers.
    EXPECT_FALSE(font.points(1, {0}).empty());
  }
  {
    Moving font({square(), square()}, past);

    EXPECT_EQ(font.points(0, kFull).size(), 4u);
    font.points(1, kFull, &result);
    EXPECT_EQ(result, GFNT_ERR_CORRUPT);
  }
}

TEST(Gvar, AGlyphPastTheCountTheTableStatesIsCorruptAndTheRestAreNot) {
  Tuple good;

  good.peak = {16384};
  good.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({good})};
  // The table states one glyph and the face has two.
  Moving font({square(), square()}, build_gvar(spec));
  GFNT_Result result = GFNT_OK;

  EXPECT_EQ(font.points(0, kFull).size(), 4u);
  font.points(1, kFull, &result);
  EXPECT_EQ(result, GFNT_ERR_CORRUPT);
}

TEST(Gvar, AHeaderThatDisagreesWithTheFvarIsRefusedForEveryGlyphButTheDefault) {
  Tuple good;

  good.peak = {16384};
  good.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  struct Case {
    const char * why;
    GvarSpec spec;
    GFNT_Result expected;
  };
  Case cases[2];
  cases[0].why = "an axis count that is not the fvar's";
  cases[0].spec.axis_count = 2;
  cases[0].spec.glyphs = {glyph_variation_data({good})};
  cases[0].expected = GFNT_ERR_CORRUPT;
  cases[1].why = "a major version this library does not read";
  cases[1].spec.major = 2;
  cases[1].spec.glyphs = {glyph_variation_data({good})};
  cases[1].expected = GFNT_ERR_UNSUPPORTED;
  for (const Case & test : cases) {
    Moving font({square()}, build_gvar(test.spec));
    GFNT_Result result = GFNT_OK;

    font.points(0, kFull, &result);
    EXPECT_EQ(result, test.expected) << test.why;
    // The face is fine and so is its default outline.
    EXPECT_EQ(font.points(0, {0}).size(), 4u) << test.why;
  }
}

TEST(Gvar, AGvarTooShortForItsOwnHeaderOrOffsetsIsRefused) {
  GvarSpec spec;
  Tuple good;

  good.peak = {16384};
  good.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  spec.glyphs = {glyph_variation_data({good})};
  const std::vector<uint8_t> table = build_gvar(spec);
  const size_t cuts[] = {0, 10, 19, 21};
  for (size_t cut : cuts) {
    std::vector<uint8_t> shortened(table.begin(), table.begin() + cut);
    Moving font({square()}, shortened);
    GFNT_Result result = GFNT_OK;

    if (cut == 0) {
      // A zero-length table is not in the directory's way and is not a table.
      continue;
    }
    font.points(0, kFull, &result);
    EXPECT_EQ(result, GFNT_ERR_CORRUPT) << "cut to " << cut;
  }
}

TEST(Gvar, AnAxisATupleDoesNotConstrainDoesNotZeroIt) {
  // Peak (1, 0): the second axis has a peak of zero, so the tuple says nothing
  // about it and applies in full wherever it is. A reader that treated a zero peak
  // as a region of its own would zero the tuple at every location off the default
  // of that axis.
  Tuple tuple;

  tuple.peak = {16384, 0};
  tuple.data = {0x07, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x87};
  GvarSpec spec;
  spec.axis_count = 2;
  spec.glyphs = {glyph_variation_data({tuple})};
  Moving font({square()}, build_gvar(spec), 2);

  EXPECT_EQ(font.points(0, {16384, 8192}),
      whole({{16, 0}, {16, 100}, {116, 100}, {116, 0}}));
  EXPECT_EQ(font.points(0, {16384, -16384}),
      whole({{16, 0}, {16, 100}, {116, 100}, {116, 0}}));
}

TEST(Gvar, ARegionTheFormatCallsInvalidIsIgnoredForThatAxisNotRefused) {
  // The specification's word for a start past the peak, an end before it, or a
  // range that straddles zero around a peak that is not zero, is "ignored": the
  // axis contributes a factor of one. Refusing would condemn a glyph for a header
  // a writer got wrong that every other reader accepts.
  struct Case {
    const char * why;
    int16_t start;
    int16_t peak;
    int16_t end;
  };
  const Case cases[] = {
    {"a start past the peak", 12288, 8192, 16384},
    {"an end before the peak", 0, 8192, 4096},
    {"a range straddling zero around a nonzero peak", -8192, 8192, 16384},
  };
  for (const Case & test : cases) {
    Tuple tuple;

    tuple.peak = {test.peak};
    tuple.start = {test.start};
    tuple.end = {test.end};
    tuple.data = {0x07, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x87};
    GvarSpec spec;
    spec.glyphs = {glyph_variation_data({tuple})};
    Moving font({square()}, build_gvar(spec));
    GFNT_Result result = GFNT_ERR_INTERNAL;

    // Far from the stated peak, where a valid region would give a scalar of zero.
    EXPECT_EQ(font.points(0, {2048}, &result),
        whole({{16, 0}, {16, 100}, {116, 100}, {116, 0}})) << test.why;
    EXPECT_EQ(result, GFNT_OK) << test.why;
  }
}

TEST(Gvar, TwoNamedPointsAtOneCoordinateInferNothingBetweenThemUnlessTheyAgree) {
  // The square again, with p0 and p3 named: they share y = 0, so the points
  // between them in contour order (p1, p2, both at y = 100) have no span of y to
  // interpolate across. If the two deltas agree the others take it, and if they
  // do not there is no proportion to take and they take nothing.
  const struct {
    const char * why;
    int dy0;
    int dy3;
    int inferred;
  } cases[] = {
      {"deltas that agree", 7, 7, 7},
      {"deltas that differ", 6, 10, 0},
  };
  for (const auto & test : cases) {
    Tuple tuple;

    tuple.peak = {16384};
    tuple.private_points = true;
    tuple.data = {
        0x02, 0x01, 0x00, 0x03,    // points 0 and 3
        0x01, 0x00, 0x00,          // x: 0, 0
        0x01, static_cast<uint8_t>(test.dy0), static_cast<uint8_t>(test.dy3),
    };
    GvarSpec spec;
    spec.glyphs = {glyph_variation_data({tuple})};
    Moving font({square()}, build_gvar(spec));

    EXPECT_EQ(font.points(0, kFull),
        whole({{0, test.dy0}, {0, 100 + test.inferred}, {100, 100 + test.inferred},
               {100, test.dy3}})) << test.why;
  }
}

TEST(Gvar, InterpolationWorksWhenTheNamedPointsRunDownAnAxis) {
  // p1 = (0,100) and p3 = (100,0) are named, so along y the first reference is
  // the larger coordinate. The span is the same span either way round and the
  // deltas travel with their coordinates: p2 at y = 100 takes p1's dy and p0 at
  // y = 0 takes p3's.
  Tuple tuple;

  tuple.peak = {16384};
  tuple.private_points = true;
  tuple.data = {
      0x02, 0x01, 0x01, 0x02,    // points 1 and 3
      0x01, 0x02, 0x06,          // x: 2, 6
      0x01, 0x14, 0x28,          // y: 20, 40
  };
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple})};
  Moving font({square()}, build_gvar(spec));

  EXPECT_EQ(font.points(0, kFull),
      whole({{2, 40}, {2, 120}, {106, 120}, {106, 40}}));
}

TEST(Gvar, PhantomPointNumbersAreCountedAndMoveNothingTheOutlineHas) {
  // A square has four points and a tuple may name numbers 4 to 7, which are the
  // phantom points: they carry advance and bearing deltas and are not part of the
  // outline. Naming one is not an error and moves no point of the glyph.
  Tuple tuple;

  tuple.peak = {16384};
  tuple.private_points = true;
  tuple.data = {
      0x02, 0x01, 0x04, 0x01,    // points 4 and 5
      0x01, 0x32, 0x32,          // x: 50, 50
      0x01, 0x00, 0x00,
  };
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple})};
  Moving font({square()}, build_gvar(spec));
  GFNT_Result result = GFNT_ERR_INTERNAL;

  EXPECT_EQ(font.points(0, kFull, &result),
      whole({{0, 0}, {0, 100}, {100, 100}, {100, 0}}));
  EXPECT_EQ(result, GFNT_OK);
}

TEST(Gvar, ASharedPointListThatStatesEveryPointIsTheShorthandNotAnEmptyList) {
  // Shared points present with a count of zero: "all of them", for every tuple
  // that does not carry its own list.
  Tuple tuple;

  tuple.peak = {16384};
  tuple.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple}, {0x00}, true)};
  Moving font({square()}, build_gvar(spec));

  EXPECT_EQ(font.points(0, kFull),
      whole({{10, 0}, {10, 100}, {90, 100}, {90, 0}}));
}

TEST(Gvar, ATupleThatDoesNotApplyHereIsNotReadHere) {
  // The first tuple's data is garbage and its peak is +1; at -1 its scalar is zero
  // and nothing it holds is consulted. A reader that validated every tuple before
  // deciding which applied would refuse a glyph at a location the damage cannot
  // reach, which is the opposite of M11.
  Tuple broken;
  Tuple fine;

  broken.peak = {16384};
  broken.data = {0xFF};                            // a run that never ends
  fine.peak = {-16384};
  fine.data = {0x07, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({broken, fine})};
  Moving font({square()}, build_gvar(spec));
  GFNT_Result result = GFNT_OK;

  EXPECT_EQ(font.points(0, {-16384}, &result),
      whole({{5, 0}, {5, 100}, {105, 100}, {105, 0}}));
  EXPECT_EQ(result, GFNT_OK);
  // And here, where it applies, the damage is the glyph's.
  font.points(0, kFull, &result);
  EXPECT_EQ(result, GFNT_ERR_CORRUPT);
}

TEST(Gvar, AComponentOffsetIsMovedBeforeAScaledComponentScalesIt) {
  // SCALED_COMPONENT_OFFSET: the offset (20, 20) is in the component's space, so
  // the component's own 0.5 scale applies to it, and gvar varies the number in the
  // file. Offset 20 + 10 = 30, scaled to 15 - not 20 * 0.5 + 10 = 20.
  Tuple tuple;

  tuple.peak = {16384};
  tuple.data = {
      0x00, 0x0A, 0x83,          // x: 10, then four zeros (one component + 4 phantoms)
      0x84,                      // y: five zeros
  };
  GvarSpec spec;
  spec.glyphs = {{}, glyph_variation_data({tuple})};
  Moving font({square(),
      gfnttest::build_glyf_composite({
          {0, static_cast<uint16_t>(kArgsAreXy | 0x0800 | 0x0008), 20, 20,
              {0x2000}},
      })}, build_gvar(spec));

  EXPECT_EQ(font.points(1, {0}),
      whole({{10, 10}, {10, 60}, {60, 60}, {60, 10}}));
  EXPECT_EQ(font.points(1, kFull),
      whole({{15, 10}, {15, 60}, {65, 60}, {65, 10}}));
}

TEST(Gvar, ACompositeOfACompositeVariesTheInnerOneBeforePlacingIt) {
  // glyph 0: the square, moved by +10 in x on its left side. glyph 1: a composite
  // of glyph 0 at (100, 0). glyph 2: a composite of glyph 1 at (0, 200). At +1 the
  // innermost square is (10,0) (10,100) (90,100) (90,0), and each level adds its
  // own offset.
  Tuple tuple;

  tuple.peak = {16384};
  tuple.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple}), {}, {}};
  Moving font({square(),
      gfnttest::build_glyf_composite({{0, kArgsAreXy, 100, 0}}),
      gfnttest::build_glyf_composite({{1, kArgsAreXy, 0, 100}})},
      build_gvar(spec));

  EXPECT_EQ(font.points(2, kFull),
      whole({{110, 100}, {110, 200}, {190, 200}, {190, 100}}));
}

TEST(Gvar, ACharstringFaceWithAnFvarIsRefusedByNameForARealLocation) {
  // The face of cff.otf with an `fvar` added: a design space and outlines that
  // are charstrings, whose variations are CFF2's blend operators. A zero location
  // is the default instance and answers; anything else would be a shape for a
  // location it is not at.
  const std::string path = gfnttest::data("fonts/cff.otf");
  std::vector<uint8_t> file;
  GFNT_Blob * source = nullptr;
  GFNT_Face * original = nullptr;
  size_t tables = 0;

  // The file's own bytes, because a table's range is an offset into them.
  FILE * handle = fopen(path.c_str(), "rb");
  ASSERT_NE(handle, nullptr);
  for (int c = fgetc(handle); c != EOF; c = fgetc(handle)) {
    file.push_back(static_cast<uint8_t>(c));
  }
  fclose(handle);
  ASSERT_EQ(gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &source,
      nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_load(source, 0, nullptr, nullptr, &original, nullptr),
      GFNT_OK);
  tables = gfnt_face_table_count(original);
  std::vector<gfnttest::Table> copied;

  for (size_t i = 0; i < tables; ++i) {
    GFNT_Tag tag = 0;
    size_t offset = 0;
    size_t length = 0;

    ASSERT_EQ(gfnt_face_table_tag_at(original, i, &tag), GFNT_OK);
    ASSERT_EQ(gfnt_face_table_range(original, tag, &offset, &length), GFNT_OK);
    copied.push_back({tag, std::vector<uint8_t>(file.begin() + offset,
        file.begin() + offset + length)});
  }
  copied.push_back({GFNT_TAG('f', 'v', 'a', 'r'), build_fvar({kWeight}, {})});
  const std::vector<uint8_t> font = gfnttest::build_sfnt(GFNT_FLAVOUR_CFF, copied);
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  GFNT_F2Dot14 zero = 0;
  GFNT_F2Dot14 moved = 16384;
  GFNT_Variation at_default{&zero, 1};
  GFNT_Variation elsewhere{&moved, 1};

  ASSERT_EQ(gfnt_blob_create_memory(font.data(), font.size(), GFNT_BLOB_BORROWED,
      nullptr, nullptr, &blob, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
  ASSERT_TRUE(gfnt_face_is_variable(face));
  EXPECT_EQ(gfnt_face_glyph_outline(face, 1, &at_default, nullptr, &outline,
      &error), GFNT_OK);
  gfnt_outline_destroy(outline);
  outline = nullptr;
  EXPECT_EQ(gfnt_face_glyph_outline(face, 1, &elsewhere, nullptr, &outline,
      &error), GFNT_ERR_UNSUPPORTED);
  ASSERT_NE(error.message, nullptr);
  EXPECT_NE(std::string(error.message).find("charstring"), std::string::npos);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  gfnt_face_free(original);
  gfnt_blob_destroy(source);
}

TEST(Gvar, ARealFixtureReadsAndMovesAndTheDefaultIsTheStoredGlyph) {
  // variable-gvar.ttf is fontTools' encoding of the same constructions, and
  // `var_diff.py` compares every glyph of it at sixteen locations against
  // fontTools. This is the one assertion that needs no container: the fixture is
  // what it says it is, so the differential is comparing the right thing.
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  size_t axes = 0;
  size_t instances = 0;
  GFNT_Axis axis{};
  GFNT_NamedInstance instance{};

  ASSERT_EQ(gfnt_blob_create_file(gfnttest::data("fonts/variable-gvar.ttf").c_str(),
      nullptr, nullptr, &blob, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_axis_count(face, &axes, nullptr), GFNT_OK);
  EXPECT_EQ(axes, 2u);
  ASSERT_EQ(gfnt_face_axis_at(face, 1, &axis, nullptr), GFNT_OK);
  EXPECT_EQ(axis.tag, GFNT_TAG('w', 'd', 't', 'h'));
  EXPECT_EQ(axis.flags, GFNT_AXIS_HIDDEN);
  ASSERT_EQ(gfnt_face_instance_count(face, &instances, nullptr), GFNT_OK);
  EXPECT_EQ(instances, 3u);
  ASSERT_EQ(gfnt_face_instance_at(face, 1, &instance, nullptr), GFNT_OK);
  EXPECT_NE(instance.postscript_name_id, GFNT_INSTANCE_NO_POSTSCRIPT_NAME);
  ASSERT_EQ(gfnt_face_instance_at(face, 0, &instance, nullptr), GFNT_OK);
  EXPECT_EQ(instance.postscript_name_id, GFNT_INSTANCE_NO_POSTSCRIPT_NAME);

  // Weight 700 on this font's bent axis: (700 - 400) / 500 = 0.6, which avar's
  // segment from 0.5 to 1.0 (mapping to 0.75 and 1.0) takes to 0.75 + 0.1 * 0.5 =
  // 0.8; width at its default.
  const GFNT_F16Dot16 user[2] = {700 * 65536, 100 * 65536};
  GFNT_F2Dot14 normalised[2] = {0, 0};

  ASSERT_EQ(gfnt_face_normalize(face, user, 2, normalised, 2, nullptr), GFNT_OK);
  EXPECT_EQ(normalised[0], 13107) << "0.8 in 2.14, rounded";
  EXPECT_EQ(normalised[1], 0);

  // `tri` is glyph 2. At weight 700 its single-point tuple (peak +1) applies at
  // 0.8 and moves the whole contour up by 80; the tuple on the other side of the
  // axis does not reach.
  GFNT_Variation variation{normalised, 2};
  GFNT_Outline * outline = nullptr;
  GFNT_Point at;

  ASSERT_EQ(gfnt_face_glyph_outline(face, 2, &variation, nullptr, &outline,
      nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_outline_point_count(outline), 3u);
  ASSERT_EQ(gfnt_outline_point_at(outline, 2, &at, nullptr), GFNT_OK);
  // 150, 400 + 100 * 13107 / 16384 = 480 - exactly, since 13107 / 16384 is not
  // 0.8 but 0.79998: the point is 479.998, which 26.6 holds as 30720 - 1.
  EXPECT_EQ(at.x, 150 * 64);
  EXPECT_NEAR(at.y, 480 * 64, 1);
  gfnt_outline_destroy(outline);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
}

TEST(Gvar, ARegionWithNoWidthAtAnAxisIsNeverDividedBy) {
  // Start, peak and end all 0.5: a region of zero width. Anywhere but the peak the
  // scalar is zero, and the ramp's denominators - peak - start and end - peak -
  // are zero, so a reader that reaches them divides by zero.
  Tuple tuple;

  tuple.peak = {8192};
  tuple.start = {8192};
  tuple.end = {8192};
  tuple.data = {0x07, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple})};
  Moving font({square()}, build_gvar(spec));

  EXPECT_EQ(font.points(0, {4096}),
      whole({{0, 0}, {0, 100}, {100, 100}, {100, 0}}));
  EXPECT_EQ(font.points(0, {12288}),
      whole({{0, 0}, {0, 100}, {100, 100}, {100, 0}}));
  EXPECT_EQ(font.points(0, {8192}),
      whole({{16, 0}, {16, 100}, {116, 100}, {116, 0}}));
}

TEST(Gvar, EveryTupleThatAppliesIsSummedNotJustTheLast) {
  // Two tuples at the same peak with different deltas: 10 and 5 in x.
  Tuple first;
  Tuple second;

  first.peak = {16384};
  first.data = {0x07, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x87};
  second.peak = {16384};
  second.data = {0x07, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({first, second})};
  Moving font({square()}, build_gvar(spec));

  EXPECT_EQ(font.points(0, kFull),
      whole({{15, 0}, {15, 100}, {115, 100}, {115, 0}}));
  // Overlapping regions at a point in both: scalar 1 of the first and 0.5 of a
  // half-way intermediate second sum to 1.5 of a unit delta.
  Tuple wide;
  Tuple ramp;

  wide.peak = {16384};
  wide.data = {0x07, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x87};
  ramp.peak = {16384};
  ramp.start = {0};
  ramp.end = {16384};
  ramp.data = {0x07, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x87};
  spec.glyphs = {glyph_variation_data({wide, ramp})};
  Moving overlapping({square()}, build_gvar(spec));

  // At 0.5: the first at 0.5 * 16 = 8, the second at 0.5 * 32 = 16: 24.
  EXPECT_EQ(overlapping.points(0, kHalf),
      whole({{24, 0}, {24, 100}, {124, 100}, {124, 0}}));
}

TEST(Gvar, AFractionalDeltaIsRoundedToTheNearestSixtyFourthNotTruncated) {
  // 26 units at 5461/16384 is 8.6662 units - 554.64 sixty-fourths - which rounds
  // to 555, where truncating gives 554. And -25 at the same scalar is -533.3,
  // which rounds to -533 where an arithmetic shift (which floors) gives -534.
  Tuple up;
  Tuple down;

  up.peak = {16384};
  up.data = {0x07, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x87};
  down.peak = {16384};
  down.data = {0x07, 0xE7, 0xE7, 0xE7, 0xE7, 0xE7, 0xE7, 0xE7, 0xE7, 0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({up}), glyph_variation_data({down})};
  Moving font({square(), square()}, build_gvar(spec));
  const Points positive = font.points(0, {5461});
  const Points negative = font.points(1, {5461});

  ASSERT_EQ(positive.size(), 4u);
  ASSERT_EQ(negative.size(), 4u);
  EXPECT_EQ(positive[0].first, 555);
  EXPECT_EQ(negative[0].first, -533);
}

TEST(Gvar, AGlyphPastTheCountTheTableStatesIsCorruptWhateverItsOffsetsSay) {
  // The offset array has an entry for glyph 1 and the header states a count of
  // one. The entry would be read and would be valid; the count is what condemns the
  // glyph, and trusting the array's length instead would apply data the header
  // says is not there.
  Tuple good;

  good.peak = {16384};
  good.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({good}), glyph_variation_data({good})};
  spec.glyph_count_delta = -1;
  Moving font({square(), square()}, build_gvar(spec));
  GFNT_Result result = GFNT_OK;

  EXPECT_EQ(font.points(0, kFull).size(), 4u);
  font.points(1, kFull, &result);
  EXPECT_EQ(result, GFNT_ERR_CORRUPT);
  ASSERT_NE(font.error.message, nullptr);
  EXPECT_NE(std::string(font.error.message).find("glyph count"),
      std::string::npos);
}

TEST(Gvar, ABackwardsOffsetEntryIsNamedAsOne) {
  Tuple good;

  good.peak = {16384};
  good.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({good}), glyph_variation_data({good})};
  std::vector<uint8_t> table = build_gvar(spec);
  // Glyph 0's entry runs from offset 0 to the *second* entry, at byte 22; set that
  // to 0 and glyph 1's start is 0 and its end is the original second entry... so
  // instead set the *first* entry (byte 20) to 2 words and the second to 1.
  table[20] = 0x00;
  table[21] = 0x08;
  table[22] = 0x00;
  table[23] = 0x01;
  Moving font({square(), square()}, table);
  GFNT_Result result = GFNT_OK;

  font.points(0, kFull, &result);
  EXPECT_EQ(result, GFNT_ERR_CORRUPT);
  ASSERT_NE(font.error.message, nullptr);
  EXPECT_NE(std::string(font.error.message).find("backwards"), std::string::npos);
}

TEST(Gvar, AnEmptyGlyphOnAFaceWithNoGvarStillRefusesARealLocation) {
  // The refusal is the face's and not the glyph's: a face that has an fvar and no
  // gvar cannot honour a location, so the answer is the same for a glyph with
  // nothing to move as for one with something. A `space` that succeeded here
  // would make the face's answer depend on which glyph a caller tried first.
  Moving font({gfnttest::build_glyf_glyph({})}, {});
  GFNT_Result result = GFNT_OK;

  font.points(0, kFull, &result);
  EXPECT_EQ(result, GFNT_ERR_UNSUPPORTED);
  font.points(0, {0}, &result);
  EXPECT_EQ(result, GFNT_OK);
}

TEST(Gvar, AGlyphAppendedToAnOutlineThatAlreadyHasPointsIsInferredFromItsOwn) {
  // ::gfnt_glyf_load() appends, and the contour ends it records are absolute: an
  // inference that took them as relative to the glyph would walk the *first*
  // glyph's points. Every caller today hands it an empty outline, so this is the
  // only place the difference can be seen - and it is here because point
  // matching's correctness already rests on that function not assuming it.
  Tuple tuple;

  tuple.peak = {16384};
  tuple.private_points = true;
  tuple.data = {
      0x02, 0x01, 0x00, 0x02,
      0x01, 0x14, 0x00,
      0x01, 0x00, 0x28,
  };
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple})};
  Moving font({square()}, build_gvar(spec));
  GFNT_Outline * outline = nullptr;
  const GFNT_F2Dot14 coordinate = 16384;
  const GFNT_Variation variation{&coordinate, 1};

  ASSERT_EQ(gfnt_outline_create(nullptr, &outline, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_glyf_load(font.face, 0, &variation, outline, &font.error),
      GFNT_OK);
  ASSERT_EQ(gfnt_glyf_load(font.face, 0, &variation, outline, &font.error),
      GFNT_OK);
  ASSERT_EQ(gfnt_outline_point_count(outline), 8u);
  const Points expected = whole({{20, 0}, {20, 140}, {100, 140}, {100, 0}});

  for (size_t i = 0; i < 8; ++i) {
    GFNT_Point at;

    ASSERT_EQ(gfnt_outline_point_at(outline, i, &at, nullptr), GFNT_OK);
    EXPECT_EQ(at.x, expected[i % 4].first) << "point " << i;
    EXPECT_EQ(at.y, expected[i % 4].second) << "point " << i;
  }
  gfnt_outline_destroy(outline);
}

TEST(Gvar, AMetricAtALocationIsRefusedRatherThanAnsweredWithTheDefaults) {
  // A font that has the whole design space and nothing that moves a metric: the
  // outline at weight 900 changes shape (gvar), and an advance at weight 900 would
  // be the weight 400 advance, quietly. It is refused, and a caller who draws and
  // measures at one location is told which of the two it cannot have.
  Tuple tuple;

  tuple.peak = {16384};
  tuple.data = {0x03, 0x0A, 0x0A, 0xF6, 0xF6, 0x83, 0x87};
  GvarSpec spec;
  spec.glyphs = {glyph_variation_data({tuple})};
  Moving font({square()}, build_gvar(spec));
  const GFNT_F2Dot14 moved = 16384;
  const GFNT_F2Dot14 still = 0;
  const GFNT_Variation at_location{&moved, 1};
  const GFNT_Variation at_default{&still, 1};
  const GFNT_F2Dot14 many[2] = {16384, 16384};
  const GFNT_Variation too_many{many, 2};
  int32_t value = -1;
  GFNT_LineMetrics line{};

  EXPECT_EQ(gfnt_face_glyph_advance(font.face, 0, nullptr, &value, nullptr),
      GFNT_OK);
  EXPECT_EQ(value, 500);
  value = -1;
  EXPECT_EQ(gfnt_face_glyph_advance(font.face, 0, &at_default, &value, nullptr),
      GFNT_OK) << "a variation that moves nothing is the default instance";
  EXPECT_EQ(value, 500);
  EXPECT_EQ(gfnt_face_glyph_advance(font.face, 0, &at_location, &value,
      &font.error), GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(value, 500) << "and the output is untouched by a refusal";
  EXPECT_EQ(gfnt_face_glyph_side_bearing(font.face, 0, &at_location, &value,
      &font.error), GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(gfnt_face_line_metrics(font.face, GFNT_LINE_METRICS_HHEA,
      &at_location, &line, &font.error), GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(gfnt_face_line_metrics(font.face, GFNT_LINE_METRICS_HHEA,
      &at_default, &line, &font.error), GFNT_OK);
  EXPECT_EQ(line.ascent, 800);
  EXPECT_EQ(gfnt_face_glyph_advance(font.face, 0, &too_many, &value,
      &font.error), GFNT_ERR_INVALID);
  ASSERT_NE(font.error.message, nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
