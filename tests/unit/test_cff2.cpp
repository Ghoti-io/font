/**
 * @file
 *
 * `CFF2`: the container, the two operators its charstrings add (`vsindex` and
 * `blend`), and an outline at a location.
 *
 * documentation/design.md section 7.7 and 16. Every number here is worked by hand
 * from the specification, because this file's encoder and decoder are both this
 * library's and one that shared a misreading would agree with itself about
 * everything. The independent reading is fontTools': `variable-cff2.otf` is its
 * work, `Cantarell-VF.otf` is Debian's, and `tools/oracle/cff2_var_diff.py`
 * compares this library against fontTools and FreeType on both.
 *
 * The fonts built here have one axis (weight, 100..400..900) and a variation store
 * of up to three regions: R0 ramps up from the default to the maximum, R1 ramps
 * down from the default to the minimum, and R2 peaks half way up. At weight 650 -
 * a normalised 0.5 - R0's scalar is 0.5 and R1's and R2's are 0 and 1.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <array>
#include <string>
#include <vector>

#include <ghoti.io/font/charstring.h>
#include <ghoti.io/font/font.h>
#include <ghoti.io/font/outline.h>

#include <gtest/gtest.h>

namespace {

using gfnttest::put_s16;
using gfnttest::put_u16;
using gfnttest::put_u32;
using gfnttest::put_u8;

using Bytes = std::vector<uint8_t>;
using Points = std::vector<std::pair<int, int>>;

// Charstring operators, by the number the format gives them.
constexpr uint8_t kRlineto = 5;
constexpr uint8_t kHlineto = 6;
constexpr uint8_t kVlineto = 7;
constexpr uint8_t kCallsubr = 10;
constexpr uint8_t kReturn = 11;
constexpr uint8_t kEndchar = 14;
constexpr uint8_t kVsindex = 15;
constexpr uint8_t kBlend = 16;
constexpr uint8_t kRmoveto = 21;
constexpr uint8_t kCallgsubr = 29;

/** A charstring, assembled from numbers and operators. */
struct Cs {
  Bytes bytes;

  /** An operand, as the three-byte 16-bit form. */
  Cs & n(int value) {
    put_u8(bytes, 28);
    put_s16(bytes, static_cast<int16_t>(value));
    return *this;
  }
  Cs & op(uint8_t value) {
    put_u8(bytes, value);
    return *this;
  }
  /** `values` then, for each, its `deltas`, then the count, then blend. */
  Cs & blend(const std::vector<int> & values, const std::vector<int> & deltas) {
    for (int v : values) {
      n(v);
    }
    for (int d : deltas) {
      n(d);
    }
    n(static_cast<int>(values.size()));
    return op(kBlend);
  }
};

/** A DICT integer, always the five-byte form so that sizes do not depend on values. */
Bytes dict_int(int32_t value) {
  Bytes out;

  put_u8(out, 29);
  put_u32(out, static_cast<uint32_t>(value));
  return out;
}

/** An INDEX with a four-byte count and two-byte offsets: CFF2's. */
Bytes index_of(const std::vector<Bytes> & elements) {
  Bytes out;
  size_t at = 1;

  put_u32(out, static_cast<uint32_t>(elements.size()));
  if (elements.empty()) {
    return out;
  }
  put_u8(out, 2);
  put_u16(out, 1);
  for (const Bytes & element : elements) {
    at += element.size();
    put_u16(out, static_cast<uint16_t>(at));
  }
  for (const Bytes & element : elements) {
    out.insert(out.end(), element.begin(), element.end());
  }
  return out;
}

/** One axis-one-triple region: start, peak, end in 2.14. */
using Region = std::array<int16_t, 3>;

/** A variation store of one axis: `regions`, and per data the regions it uses. */
Bytes variation_store(const std::vector<Region> & regions,
    const std::vector<std::vector<uint16_t>> & datas) {
  Bytes list;
  Bytes out;
  size_t at = 8 + 4 * datas.size();
  std::vector<Bytes> blocks;

  put_u16(list, 1);
  put_u16(list, static_cast<uint16_t>(regions.size()));
  for (const Region & region : regions) {
    for (int16_t value : region) {
      put_s16(list, value);
    }
  }
  for (const auto & indices : datas) {
    Bytes block;

    put_u16(block, 0);   // itemCount: CFF2 keeps no items here
    put_u16(block, 0);   // wordDeltaCount
    put_u16(block, static_cast<uint16_t>(indices.size()));
    for (uint16_t index : indices) {
      put_u16(block, index);
    }
    blocks.push_back(block);
  }
  put_u16(out, 1);
  put_u32(out, static_cast<uint32_t>(at));
  put_u16(out, static_cast<uint16_t>(datas.size()));
  at += list.size();
  for (const Bytes & block : blocks) {
    put_u32(out, static_cast<uint32_t>(at));
    at += block.size();
  }
  out.insert(out.end(), list.begin(), list.end());
  for (const Bytes & block : blocks) {
    out.insert(out.end(), block.begin(), block.end());
  }
  return out;
}

const std::vector<Region> kRegions = {
    {0, 16384, 16384},        // R0: up from the default to the maximum
    {-16384, -16384, 0},      // R1: down from the default to the minimum
    {0, 8192, 16384},         // R2: peaks half way up
};

struct FontDictSpec {
  std::vector<Cs> subrs{};
  int vsindex = -1;           ///< The Private DICT's `vsindex`, or -1 for none.
};

struct Cff2Spec {
  std::vector<Cs> glyphs{};
  std::vector<Cs> global_subrs{};
  std::vector<FontDictSpec> fonts = {FontDictSpec{}};
  Bytes fdselect{};
  Bytes store{};              ///< The variation store, without its length.
  bool no_charstrings = false;
  bool no_fdarray = false;
  uint8_t major = 2;
  size_t top_extra = 0;       ///< Bytes to add to the header's topDictLength.
};

std::vector<Bytes> bytes_of(const std::vector<Cs> & list) {
  std::vector<Bytes> out;

  for (const Cs & cs : list) {
    out.push_back(cs.bytes);
  }
  return out;
}

/** A CFF2 table, laid out in one pass because every offset is five bytes wide. */
Bytes build_cff2(const Cff2Spec & spec) {
  const bool has_select = !spec.fdselect.empty();
  const bool has_store = !spec.store.empty();
  size_t top_size = (spec.no_charstrings ? 0 : 6) + (spec.no_fdarray ? 0 : 7)
      + (has_select ? 7 : 0) + (has_store ? 6 : 0);
  const Bytes gsubrs = index_of(bytes_of(spec.global_subrs));
  const Bytes charstrings = index_of(bytes_of(spec.glyphs));
  size_t at = 5 + top_size + gsubrs.size();
  const size_t charstrings_at = at;
  size_t store_at = 0;
  size_t select_at = 0;
  size_t fdarray_at;
  Bytes out;
  Bytes fdarray_bytes;
  Bytes privates;
  std::vector<Bytes> font_dicts;

  at += charstrings.size();
  if (has_store) {
    store_at = at;
    at += 2 + spec.store.size();
  }
  if (has_select) {
    select_at = at;
    at += spec.fdselect.size();
  }
  fdarray_at = at;
  // The FDArray's size depends on the Font DICTs, which are eleven bytes each.
  at += index_of(std::vector<Bytes>(spec.fonts.size(), Bytes(11))).size();
  for (const FontDictSpec & font : spec.fonts) {
    Bytes dict;
    Bytes subrs = font.subrs.empty() ? Bytes{} : index_of(bytes_of(font.subrs));
    Bytes private_dict;
    Bytes font_dict;

    if (font.vsindex >= 0) {
      const Bytes number = dict_int(font.vsindex);

      private_dict.insert(private_dict.end(), number.begin(), number.end());
      put_u8(private_dict, 22);
    }
    if (!font.subrs.empty()) {
      const Bytes number = dict_int(static_cast<int32_t>(private_dict.size() + 6));

      private_dict.insert(private_dict.end(), number.begin(), number.end());
      put_u8(private_dict, 19);
    }
    {
      const Bytes size = dict_int(static_cast<int32_t>(private_dict.size()));
      const Bytes where = dict_int(static_cast<int32_t>(at));

      font_dict.insert(font_dict.end(), size.begin(), size.end());
      font_dict.insert(font_dict.end(), where.begin(), where.end());
      put_u8(font_dict, 18);
    }
    font_dicts.push_back(font_dict);
    privates.insert(privates.end(), private_dict.begin(), private_dict.end());
    privates.insert(privates.end(), subrs.begin(), subrs.end());
    at += private_dict.size() + subrs.size();
  }

  put_u8(out, spec.major);
  put_u8(out, 0);
  put_u8(out, 5);
  put_u16(out, static_cast<uint16_t>(top_size + spec.top_extra));
  if (!spec.no_charstrings) {
    const Bytes number = dict_int(static_cast<int32_t>(charstrings_at));

    out.insert(out.end(), number.begin(), number.end());
    put_u8(out, 17);
  }
  if (!spec.no_fdarray) {
    const Bytes number = dict_int(static_cast<int32_t>(fdarray_at));

    out.insert(out.end(), number.begin(), number.end());
    put_u8(out, 12);
    put_u8(out, 36);
  }
  if (has_select) {
    const Bytes number = dict_int(static_cast<int32_t>(select_at));

    out.insert(out.end(), number.begin(), number.end());
    put_u8(out, 12);
    put_u8(out, 37);
  }
  if (has_store) {
    const Bytes number = dict_int(static_cast<int32_t>(store_at));

    out.insert(out.end(), number.begin(), number.end());
    put_u8(out, 24);
  }
  out.insert(out.end(), gsubrs.begin(), gsubrs.end());
  out.insert(out.end(), charstrings.begin(), charstrings.end());
  if (has_store) {
    put_u16(out, static_cast<uint16_t>(spec.store.size()));
    out.insert(out.end(), spec.store.begin(), spec.store.end());
  }
  out.insert(out.end(), spec.fdselect.begin(), spec.fdselect.end());
  {
    const Bytes array = index_of(font_dicts);

    out.insert(out.end(), array.begin(), array.end());
  }
  out.insert(out.end(), privates.begin(), privates.end());
  return out;
}

/** An `fvar` of one axis, weight 100 to 900, default 400. */
Bytes weight_fvar() {
  Bytes out;

  put_u16(out, 1);
  put_u16(out, 0);
  put_u16(out, 16);
  put_u16(out, 2);
  put_u16(out, 1);
  put_u16(out, 20);
  put_u16(out, 0);
  put_u16(out, 8);
  out.push_back('w');
  out.push_back('g');
  out.push_back('h');
  out.push_back('t');
  put_u32(out, 100u << 16);
  put_u32(out, 400u << 16);
  put_u32(out, 900u << 16);
  put_u16(out, 0);
  put_u16(out, 256);
  return out;
}

/** A face over a CFF2 table, an `fvar`, and the tables a face needs. */
struct Cff2Face {
  Bytes bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  GFNT_Result load = GFNT_OK;

  explicit Cff2Face(const Cff2Spec & spec, bool with_fvar = true) {
    const size_t glyphs = spec.glyphs.size();
    std::vector<std::pair<uint16_t, int16_t>> metrics(glyphs, {500, 0});
    std::vector<gfnttest::Table> tables = {
        {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
        {GFNT_TAG('h', 'h', 'e', 'a'),
            gfnttest::build_hhea(800, -200, 0, static_cast<uint16_t>(glyphs))},
        {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics, {})},
        {GFNT_TAG('m', 'a', 'x', 'p'),
            gfnttest::build_maxp(static_cast<uint16_t>(glyphs))},
        {GFNT_TAG('C', 'F', 'F', '2'), build_cff2(spec)},
    };
    if (with_fvar) {
      tables.push_back({GFNT_TAG('f', 'v', 'a', 'r'), weight_fvar()});
    }
    bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_CFF, tables);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
    load = gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error);
  }
  ~Cff2Face() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Cff2Face(const Cff2Face &) = delete;
  Cff2Face & operator=(const Cff2Face &) = delete;

  /** The outline's points in whole units, or the failure through @p result. */
  Points points(uint32_t glyph, const std::vector<GFNT_F2Dot14> & coordinates = {},
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

  GFNT_Result outline_result(uint32_t glyph,
      const std::vector<GFNT_F2Dot14> & coordinates = {}) {
    GFNT_Result result = GFNT_OK;

    points(glyph, coordinates, &result);
    return result;
  }
};

/** Points given in whole font units, as the 26.6 an outline holds. */
Points whole(std::initializer_list<std::pair<int, int>> units) {
  Points out;

  for (const auto & unit : units) {
    out.push_back({unit.first * 64, unit.second * 64});
  }
  return out;
}

const std::vector<GFNT_F2Dot14> kMax = {16384};
const std::vector<GFNT_F2Dot14> kHalf = {8192};
const std::vector<GFNT_F2Dot14> kMin = {-16384};

/** `0 0 rmoveto`, then three lines: a square whose sides can be blended. */
Cs square(int a, int b, int c) {
  return Cs().n(0).n(0).op(kRmoveto).n(a).op(kHlineto).n(b).op(kVlineto).n(c)
      .op(kHlineto);
}

}  // namespace

TEST(Cff2, AFontWithNoBlendDrawsTheSameAtEveryLocation) {
  Cff2Spec spec;

  spec.glyphs = {Cs(), square(100, 100, -100)};
  spec.store = variation_store(kRegions, {{0}});
  Cff2Face font(spec);
  const Points square_points = whole({{0, 0}, {100, 0}, {100, 100}, {0, 100}});

  ASSERT_EQ(font.load, GFNT_OK);
  EXPECT_EQ(font.points(1), square_points);
  EXPECT_EQ(font.points(1, kMax), square_points);
  EXPECT_EQ(font.points(1, kMin), square_points);
  // A glyph with an empty charstring draws nothing, which is not an error.
  EXPECT_EQ(font.points(0, kMax), Points{});
}

TEST(Cff2, ABlendAddsEachDeltaScaledByItsRegionsScalar) {
  // `100 [20] 1 blend hlineto`: 100 plus 20 times R0's scalar, which is the
  // coordinate itself on the upper half and 0 on the lower.
  Cff2Spec spec;
  Cs glyph = Cs().n(0).n(0).op(kRmoveto).blend({100}, {20}).op(kHlineto)
      .n(100).op(kVlineto).blend({-100}, {20}).op(kHlineto);

  spec.glyphs = {Cs(), glyph};
  spec.store = variation_store(kRegions, {{0}});
  Cff2Face font(spec);

  ASSERT_EQ(font.load, GFNT_OK);
  // The default location: the blend's own values.
  EXPECT_EQ(font.points(1), whole({{0, 0}, {100, 0}, {100, 100}, {0, 100}}));
  // At the maximum R0's scalar is 1: 120, then -100 + 20 = -80 back from there.
  EXPECT_EQ(font.points(1, kMax), whole({{0, 0}, {120, 0}, {120, 100}, {40, 100}}));
  // Half way: 110, and -90 back.
  EXPECT_EQ(font.points(1, kHalf), whole({{0, 0}, {110, 0}, {110, 100}, {20, 100}}));
  // On the lower half R0 does not reach.
  EXPECT_EQ(font.points(1, kMin), whole({{0, 0}, {100, 0}, {100, 100}, {0, 100}}));
}

TEST(Cff2, ABlendOfSeveralValuesAndRegionsSumsEachValuesDeltas) {
  // `100 50  10 20  4 -6  2 blend` with the data's regions R0 and R1: each value
  // takes its own pair of deltas, one per region. At 0.5 R0 is 0.5 and R1 is 0:
  // 100 + 5 = 105 and 50 + 2 = 52. At -1 R0 is 0 and R1 is 1: 100 + 20 = 120 and
  // 50 - 6 = 44. They move the pen, so they are the first point.
  Cff2Spec spec;
  Cs glyph = Cs().blend({100, 50}, {10, 20, 4, -6}).op(kRmoveto).n(10)
      .op(kHlineto);

  spec.glyphs = {Cs(), glyph};
  spec.store = variation_store(kRegions, {{0, 1}});
  Cff2Face font(spec);

  ASSERT_EQ(font.load, GFNT_OK);
  EXPECT_EQ(font.points(1), whole({{100, 50}, {110, 50}}));
  EXPECT_EQ(font.points(1, kHalf), whole({{105, 52}, {115, 52}}));
  EXPECT_EQ(font.points(1, kMin), whole({{120, 44}, {130, 44}}));
}

TEST(Cff2, VsindexChoosesWhichRegionsABlendUses) {
  // Data 0 uses R0 and data 1 uses R1. `1 vsindex` makes the charstring's blends
  // use data 1, so the same `100 [20] 1 blend` is 120 at the minimum and 100 at
  // the half-way point, which is the other way round from data 0.
  Cff2Spec spec;
  Cs plain = Cs().n(0).n(0).op(kRmoveto).blend({100}, {20}).op(kHlineto);
  Cs chosen = Cs().n(1).op(kVsindex).n(0).n(0).op(kRmoveto).blend({100}, {20})
      .op(kHlineto);

  spec.glyphs = {Cs(), plain, chosen};
  spec.store = variation_store(kRegions, {{0}, {1}});
  Cff2Face font(spec);

  ASSERT_EQ(font.load, GFNT_OK);
  EXPECT_EQ(font.points(1, kHalf), whole({{0, 0}, {110, 0}}));
  EXPECT_EQ(font.points(1, kMin), whole({{0, 0}, {100, 0}}));
  EXPECT_EQ(font.points(2, kHalf), whole({{0, 0}, {100, 0}}));
  EXPECT_EQ(font.points(2, kMin), whole({{0, 0}, {120, 0}}));
}

TEST(Cff2, APrivateDictsVsindexIsTheDefaultAndTheCharstringsOverridesIt) {
  Cff2Spec spec;
  Cs plain = Cs().n(0).n(0).op(kRmoveto).blend({100}, {20}).op(kHlineto);
  Cs back = Cs().n(0).op(kVsindex).n(0).n(0).op(kRmoveto).blend({100}, {20})
      .op(kHlineto);

  spec.glyphs = {Cs(), plain, back};
  spec.fonts = {FontDictSpec{{}, 1}};
  spec.store = variation_store(kRegions, {{0}, {1}});
  Cff2Face font(spec);

  ASSERT_EQ(font.load, GFNT_OK);
  // Data 1 by default: R1, so 120 at the minimum.
  EXPECT_EQ(font.points(1, kMin), whole({{0, 0}, {120, 0}}));
  EXPECT_EQ(font.points(1, kHalf), whole({{0, 0}, {100, 0}}));
  // Data 0 by the charstring's own word: R0, so 110 at the half-way point.
  EXPECT_EQ(font.points(2, kHalf), whole({{0, 0}, {110, 0}}));
}

TEST(Cff2, ARegionThatPeaksInTheMiddleScalesFromItsOwnEdges) {
  // R2 starts at 0, peaks at 0.5 and ends at 1. At 0.25 the scalar is a half and
  // at 0.75 it is a half again; at the peak it is one, at 1 it is zero.
  Cff2Spec spec;

  spec.glyphs = {Cs(), Cs().n(0).n(0).op(kRmoveto).blend({100}, {40}).op(kHlineto)};
  spec.store = variation_store(kRegions, {{2}});
  Cff2Face font(spec);

  ASSERT_EQ(font.load, GFNT_OK);
  EXPECT_EQ(font.points(1, {8192}), whole({{0, 0}, {140, 0}}));
  EXPECT_EQ(font.points(1, {4096}), whole({{0, 0}, {120, 0}}));
  EXPECT_EQ(font.points(1, {12288}), whole({{0, 0}, {120, 0}}));
  EXPECT_EQ(font.points(1, {16384}), whole({{0, 0}, {100, 0}}));
}

TEST(Cff2, ADeltaThatIsNotAWholeNumberOfUnitsKeepsItsFraction) {
  // 16.16 operands: the delta 20.5 is `20.5` as a 255 operand. At 0.5 it adds
  // 10.25, which is 656 sixty-fourths on the point's own 26.6.
  Cff2Spec spec;
  Cs glyph;

  glyph.n(0).n(0).op(kRmoveto).n(100);
  glyph.bytes.push_back(255);
  gfnttest::put_u32(glyph.bytes, 0x00148000u);   // 20.5
  glyph.n(1).op(kBlend).op(kHlineto);
  spec.glyphs = {Cs(), glyph};
  spec.store = variation_store(kRegions, {{0}});
  Cff2Face font(spec);

  ASSERT_EQ(font.load, GFNT_OK);
  EXPECT_EQ(font.points(1, kHalf), (Points{{0, 0}, {110 * 64 + 16, 0}}));
}

TEST(Cff2, ABlendRoundsOnceToTheStacksOwnPrecisionHalfAwayFromZero) {
  // A delta of 1023/65536 at a scalar of a half is 511.5 of the stack's units;
  // rounded half away from zero that is 512, which is exactly half a 64th on top
  // of 100 and so rounds up to 6401 in 26.6 - where a floor would be 511 and 6400.
  Cff2Spec spec;
  Cs glyph = Cs().n(0).n(0).op(kRmoveto).n(100);

  glyph.bytes.push_back(255);
  gfnttest::put_u32(glyph.bytes, 0x000003FFu);
  glyph.n(1).op(kBlend).op(kHlineto);
  spec.glyphs = {Cs(), glyph};
  spec.store = variation_store(kRegions, {{0}});
  Cff2Face font(spec);

  ASSERT_EQ(font.load, GFNT_OK);
  EXPECT_EQ(font.points(1, kHalf), (Points{{0, 0}, {6401, 0}}));
}

TEST(Cff2, AVsindexInTheMiddleOfACharstringChangesTheScalarsOfTheBlendsAfterIt) {
  // The first blend uses data 0 (R0: 110 at the half-way point); `1 vsindex` then
  // switches to data 1 (R1: no reach there), so the second is 100 - and a reader
  // that kept the first blend's scalars would draw 110 again.
  Cff2Spec spec;
  Cs glyph = Cs().n(0).n(0).op(kRmoveto).blend({100}, {20}).op(kHlineto)
      .n(1).op(kVsindex).blend({100}, {20}).op(kHlineto);

  spec.glyphs = {Cs(), glyph};
  spec.store = variation_store(kRegions, {{0}, {1}});
  Cff2Face font(spec);

  ASSERT_EQ(font.load, GFNT_OK);
  EXPECT_EQ(font.points(1, kHalf), whole({{0, 0}, {110, 0}, {210, 0}}));
}

TEST(Cff2, NoLeadingOperandIsTakenForAWidth) {
  // A CFF's first stack-clearing operator may carry a width, found by an operand
  // count that is one too many; a CFF2 states none, so `5 0 0 rmoveto` is a move
  // by (5, 0) with a stray operand and not a width of 5 and a move by (0, 0).
  Cff2Spec spec;

  spec.glyphs = {Cs(), Cs().n(5).n(0).n(0).op(kRmoveto).n(10).op(kHlineto)};
  Cff2Face font(spec);

  ASSERT_EQ(font.load, GFNT_OK);
  EXPECT_EQ(font.points(1), whole({{5, 0}, {15, 0}}));
}

TEST(Cff2, SubroutinesAreCalledWithTheirBiasAndMayBlend) {
  // Local subroutine 0 draws a blended 100 and returns; global subroutine 0 draws
  // a vertical 50. Both are numbered -107, the bias for fewer than 1240.
  Cff2Spec spec;
  Cs local = Cs().blend({100}, {20}).op(kHlineto).op(kReturn);
  Cs global = Cs().n(50).op(kVlineto).op(kReturn);
  Cs glyph = Cs().n(0).n(0).op(kRmoveto).n(-107).op(kCallsubr).n(-107)
      .op(kCallgsubr);

  spec.glyphs = {Cs(), glyph};
  spec.fonts = {FontDictSpec{{local}, -1}};
  spec.global_subrs = {global};
  spec.store = variation_store(kRegions, {{0}});
  Cff2Face font(spec);

  ASSERT_EQ(font.load, GFNT_OK);
  EXPECT_EQ(font.points(1), whole({{0, 0}, {100, 0}, {100, 50}}));
  EXPECT_EQ(font.points(1, kMax), whole({{0, 0}, {120, 0}, {120, 50}}));
}

TEST(Cff2, FdselectChoosesTheFontDictWhoseSubroutinesAGlyphCalls) {
  // Two Font DICTs, each with one local subroutine that draws a line of a different
  // length; glyph 1 is in the first and glyph 2 in the second. Formats 0, 3 and 4
  // are the same selection written three ways.
  const Bytes format0 = {0, 0, 0, 1};
  Bytes format3;
  Bytes format4;

  put_u8(format3, 3);
  put_u16(format3, 2);
  put_u16(format3, 0);
  put_u8(format3, 0);
  put_u16(format3, 2);
  put_u8(format3, 1);
  put_u16(format3, 3);
  put_u8(format4, 4);
  put_u32(format4, 2);
  put_u32(format4, 0);
  put_u16(format4, 0);
  put_u32(format4, 2);
  put_u16(format4, 1);
  put_u32(format4, 3);
  for (const Bytes & select : {format0, format3, format4}) {
    Cff2Spec spec;
    Cs call = Cs().n(0).n(0).op(kRmoveto).n(-107).op(kCallsubr);

    spec.glyphs = {Cs(), call, call};
    spec.fonts = {
        FontDictSpec{{Cs().n(10).op(kHlineto).op(kReturn)}, -1},
        FontDictSpec{{Cs().n(30).op(kHlineto).op(kReturn)}, -1}};
    spec.fdselect = select;
    Cff2Face font(spec);

    ASSERT_EQ(font.load, GFNT_OK) << select.size();
    EXPECT_EQ(font.points(1), whole({{0, 0}, {10, 0}})) << "format "
        << static_cast<int>(select[0]);
    EXPECT_EQ(font.points(2), whole({{0, 0}, {30, 0}})) << "format "
        << static_cast<int>(select[0]);
  }
}

TEST(Cff2, TheOperandStackHoldsFiveHundredAndThirteen) {
  // Sixty operands to `rlineto` is thirty segments, past Type 2's 48 and inside
  // CFF2's 513. 512 is the most a program can push and still call an operator.
  Cff2Spec spec;
  Cs many = Cs().n(0).n(0).op(kRmoveto);
  Cs too_many = Cs().n(0).n(0).op(kRmoveto);

  for (int i = 0; i < 60; ++i) {
    many.n(1);
  }
  many.op(kRlineto);
  for (int i = 0; i < 514; ++i) {
    too_many.n(1);
  }
  too_many.op(kRlineto);
  spec.glyphs = {Cs(), many, too_many};
  Cff2Face font(spec);

  ASSERT_EQ(font.load, GFNT_OK);
  EXPECT_EQ(font.points(1).size(), 31u);
  EXPECT_EQ(font.outline_result(2), GFNT_ERR_CORRUPT);
}

TEST(Cff2, AnEndcharIsRefusedBecauseTheFormatRemovedIt) {
  Cff2Spec spec;

  spec.glyphs = {Cs(), square(10, 10, -10).op(kEndchar)};
  Cff2Face font(spec);

  ASSERT_EQ(font.load, GFNT_OK);
  EXPECT_EQ(font.outline_result(1), GFNT_ERR_CORRUPT);
}

TEST(Cff2, ABlendWithNothingToBlendWithIsRefused) {
  Cff2Spec no_store;
  Cff2Spec too_few;
  Cff2Spec bad_index;

  no_store.glyphs = {Cs(), Cs().blend({1}, {1}).op(kRmoveto)};
  too_few.glyphs = {Cs(), Cs().n(1).n(5).op(kBlend)};
  too_few.store = variation_store(kRegions, {{0}});
  bad_index.glyphs = {Cs(), Cs().n(3).op(kVsindex).blend({1}, {1}).op(kRmoveto)};
  bad_index.store = variation_store(kRegions, {{0}});
  // Operands enough for the count but not for its deltas: two values and one
  // region need four numbers and there are three. The count alone fits the stack,
  // which is what a check of the count alone would pass.
  Cff2Spec short_deltas;

  short_deltas.glyphs = {Cs(), Cs().n(7).n(8).n(9).n(2).op(kBlend)};
  short_deltas.store = variation_store(kRegions, {{0}});
  Cff2Face a(no_store);
  Cff2Face b(too_few);
  Cff2Face c(bad_index);
  Cff2Face d(short_deltas);

  ASSERT_EQ(a.load, GFNT_OK);
  EXPECT_EQ(a.outline_result(1), GFNT_ERR_CORRUPT);
  EXPECT_EQ(b.outline_result(1), GFNT_ERR_CORRUPT);
  EXPECT_EQ(c.outline_result(1), GFNT_ERR_CORRUPT);
  EXPECT_EQ(d.outline_result(1), GFNT_ERR_CORRUPT);
}

TEST(Cff2, ALocationWithMoreCoordinatesThanAxesIsInvalid) {
  Cff2Spec spec;

  spec.glyphs = {Cs(), square(10, 10, -10)};
  Cff2Face font(spec);
  GFNT_Result result = GFNT_OK;

  font.points(1, {1, 1}, &result);
  EXPECT_EQ(result, GFNT_ERR_INVALID);
}

TEST(Cff2, ATableThatContradictsItselfIsRefused) {
  struct Case {
    const char * why;
    Cff2Spec spec;
    GFNT_Result expected;
  };
  Cff2Spec base;
  Cff2Spec no_charstrings;
  Cff2Spec no_fdarray;
  Cff2Spec version_three;
  Cff2Spec long_top;
  Cff2Spec bad_select;

  base.glyphs = {Cs(), square(10, 10, -10)};
  no_charstrings = base;
  no_charstrings.no_charstrings = true;
  no_fdarray = base;
  no_fdarray.no_fdarray = true;
  version_three = base;
  version_three.major = 3;
  long_top = base;
  long_top.top_extra = 60000;
  bad_select = base;
  bad_select.fdselect = {0, 5, 5};
  const Case cases[] = {
      {"no CharStrings", no_charstrings, GFNT_ERR_CORRUPT},
      {"no FDArray", no_fdarray, GFNT_ERR_CORRUPT},
      {"a top DICT longer than the table", long_top, GFNT_ERR_CORRUPT},
      {"a major version of 3", version_three, GFNT_ERR_UNSUPPORTED},
      {"an FDSelect naming a Font DICT that is not there", bad_select,
          GFNT_ERR_CORRUPT},
  };
  for (const Case & test : cases) {
    Cff2Face font(test.spec);

    ASSERT_EQ(font.load, GFNT_OK) << test.why;
    EXPECT_EQ(font.outline_result(1), test.expected) << test.why;
    EXPECT_EQ(font.error.table, GFNT_TAG('C', 'F', 'F', '2')) << test.why;
  }
}

TEST(Cff2, TheCharstringAccessorReturnsTheBytesAndTheDumpNamesTheNewOperators) {
  Cff2Spec spec;
  Cs glyph = Cs().n(0).n(0).op(kRmoveto).n(1).op(kVsindex).blend({100}, {20})
      .op(kHlineto);

  spec.glyphs = {Cs(), glyph};
  spec.store = variation_store(kRegions, {{0}, {1}});
  Cff2Face font(spec);
  GFNT_CharstringType type = GFNT_CHARSTRING_TYPE2;
  const uint8_t * bytes = nullptr;
  size_t length = 0;
  char * text = nullptr;
  size_t size = 0;

  ASSERT_EQ(font.load, GFNT_OK);
  ASSERT_EQ(gfnt_face_glyph_charstring(font.face, 1, &type, &bytes, &length,
      &font.error), GFNT_OK);
  EXPECT_EQ(type, GFNT_CHARSTRING_CFF2);
  EXPECT_EQ(Bytes(bytes, bytes + length), glyph.bytes);
  FILE * stream = open_memstream(&text, &size);

  ASSERT_NE(stream, nullptr);
  ASSERT_EQ(gfnt_charstring_dump(type, bytes, length, nullptr, stream), GFNT_OK);
  fclose(stream);
  const std::string dump(text, size);
  free(text);
  EXPECT_NE(dump.find("vsindex"), std::string::npos) << dump;
  EXPECT_NE(dump.find("blend"), std::string::npos) << dump;
}

TEST(Cff2, ACff2FaceHasOutlinesAndItsGlyphCountIsTheCharStringsCount) {
  Cff2Spec spec;

  spec.glyphs = {Cs(), square(10, 10, -10), Cs()};
  Cff2Face font(spec);
  size_t glyphs = 0;

  ASSERT_EQ(font.load, GFNT_OK);
  EXPECT_TRUE(gfnt_face_has_outlines(font.face));
  ASSERT_EQ(gfnt_face_num_glyphs(font.face, &glyphs, &font.error), GFNT_OK);
  EXPECT_EQ(glyphs, 3u);
  EXPECT_EQ(font.outline_result(3), GFNT_ERR_INVALID);
}

TEST(Cff2, WithNoHvarAnAdvanceDoesNotMoveBecauseNothingElseCouldMoveIt) {
  // Every glyph's `hmtx` advance is 500. A CFF2 has no phantom points to move it
  // and no width in its charstrings, so a font without an `HVAR` states that its
  // advances are the same at every location - which is an answer, where a `glyf`
  // font with neither `HVAR` nor `gvar` has stated nothing at all.
  Cff2Spec spec;

  spec.glyphs = {Cs(), square(10, 10, -10)};
  Cff2Face font(spec);
  int32_t advance = 0;
  const GFNT_Variation at{kMax.data(), kMax.size()};

  ASSERT_EQ(font.load, GFNT_OK);
  ASSERT_EQ(gfnt_face_glyph_advance(font.face, 1, &at, &advance, &font.error),
      GFNT_OK);
  EXPECT_EQ(advance, 500);
}

TEST(Cff2, AFaceWithNoFvarDrawsTheDefaultAndRefusesALocation) {
  Cff2Spec spec;

  spec.glyphs = {Cs(), square(10, 10, -10)};
  Cff2Face font(spec, false);
  GFNT_Result result = GFNT_OK;

  ASSERT_EQ(font.load, GFNT_OK);
  EXPECT_EQ(font.points(1), whole({{0, 0}, {10, 0}, {10, 10}, {0, 10}}));
  font.points(1, {1}, &result);
  EXPECT_EQ(result, GFNT_ERR_INVALID);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
