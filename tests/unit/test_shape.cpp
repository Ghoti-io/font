/**
 * @file
 *
 * Shaping: the lookups, the order they run in, and what the run comes back as.
 *
 * Four kinds of test, in the order of what each can find:
 *
 *   * **Hand-worked** answers on the two layout fixtures, each derived from the
 *     specification and the fixture's own tables, with the arithmetic in a
 *     comment. These are the only expectations in this file that nobody else
 *     computed.
 *   * **The golden file**, which is what HarfBuzz says every case of both
 *     fixtures shapes to (`tests/data/golden/shape.txt`, written by the
 *     differential and checked against the container by `make check-oracle-hb`).
 *     It holds this library to HarfBuzz with no container, and it is where the
 *     many cases the hand-worked tests do not cover are held.
 *   * **Corruption**: every byte of both layout tables overwritten, in turn, and
 *     every case shaped on the result. The answer may be anything *but* a crash or
 *     a read outside the table; what a corrupt table is allowed to say is
 *     ::GFNT_ERR_CORRUPT with a table and an offset.
 *   * **Allocation failure**: every allocation of a shaping call refused in turn.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <tuple>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <ghoti.io/font/shape.h>

namespace {

struct Glyph {
  uint32_t glyph, cluster;
  int32_t x_advance, y_advance, x_offset, y_offset;
  bool operator==(const Glyph & o) const {
    return glyph == o.glyph && cluster == o.cluster && x_advance == o.x_advance
        && y_advance == o.y_advance && x_offset == o.x_offset
        && y_offset == o.y_offset;
  }
};

std::ostream & operator<<(std::ostream & out, const Glyph & g) {
  return out << g.glyph << "/" << g.cluster << "/" << g.x_advance << "/"
             << g.y_advance << "/" << g.x_offset << "/" << g.y_offset;
}

using Glyphs = std::vector<Glyph>;

GFNT_Tag tag4(const std::string & s) {
  unsigned char b[4] = {' ', ' ', ' ', ' '};
  for (size_t i = 0; i < 4 && i < s.size(); ++i) {
    b[i] = static_cast<unsigned char>(s[i]);
  }
  return GFNT_TAG(b[0], b[1], b[2], b[3]);
}

/** A font loaded from a fixture, or from bytes. */
struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Result result = GFNT_ERR_INTERNAL;

  explicit Font(const std::string & name) {
    std::ifstream in(gfnttest::data("fonts/" + name), std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>());
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
    GFNT_Error error;
    gfnt_error_clear(&error);
    result = gfnt_blob_create_memory(bytes.data(), bytes.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, &error);
    if (result == GFNT_OK) {
      result = gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error);
    }
  }
};

/** HarfBuzz's feature syntax: [+-]tag[=value][[start:end]], comma separated. */
std::vector<GFNT_ShapeFeature> parse_features(const std::string & list) {
  std::vector<GFNT_ShapeFeature> out;
  if (list.empty() || list == "-") {
    return out;
  }
  std::stringstream stream(list);
  std::string item;
  while (std::getline(stream, item, ',')) {
    GFNT_ShapeFeature f{};
    f.value = 1;
    f.start = 0;
    f.end = GFNT_SHAPE_END;
    size_t i = 0;
    if (item[i] == '-') {
      f.value = 0;
      ++i;
    }
    else if (item[i] == '+') {
      ++i;
    }
    size_t j = i;
    while (j < item.size() && item[j] != '[' && item[j] != '=') {
      ++j;
    }
    f.tag = tag4(item.substr(i, j - i));
    if (j < item.size() && item[j] == '[') {
      size_t close = item.find(']', j);
      std::string range = item.substr(j + 1, close - j - 1);
      size_t colon = range.find(':');
      if (colon == std::string::npos) {
        if (!range.empty()) {
          f.start = std::stoul(range);
          f.end = f.start + 1;
        }
      }
      else {
        f.start = colon == 0 ? 0 : std::stoul(range.substr(0, colon));
        f.end = colon + 1 == range.size() ? GFNT_SHAPE_END
                                          : std::stoul(range.substr(colon + 1));
      }
      j = close + 1;
    }
    if (j < item.size() && item[j] == '=') {
      f.value = static_cast<uint32_t>(std::stoul(item.substr(j + 1)));
    }
    out.push_back(f);
  }
  return out;
}

struct Request {
  std::string script = "latn";
  std::string language;
  std::string features;
  std::string location;  ///< `tag=value,...` in user coordinates, or empty.
  bool rtl = false;
  GFNT_Direction vertical = GFNT_DIRECTION_LTR;  ///< TTB or BTT, to override rtl.
  float point_size = 0;  ///< For `trak`, or 0.
  uint32_t ppem = 0;     ///< For a hinting device table, or 0.
};

/** The normalised coordinates of a location: every axis at its default but the named. */
std::vector<GFNT_F2Dot14> normalised_location(const GFNT_Face * face,
    const std::string & location) {
  std::vector<GFNT_F2Dot14> out;
  size_t axes = 0;
  if (location.empty() || location == "-"
      || gfnt_face_axis_count(face, &axes, nullptr) != GFNT_OK) {
    return out;
  }
  std::vector<GFNT_F16Dot16> user(axes);
  std::vector<GFNT_Axis> info(axes);
  for (size_t i = 0; i < axes; ++i) {
    gfnt_face_axis_at(face, i, &info[i], nullptr);
    user[i] = info[i].def;
  }
  std::stringstream stream(location);
  std::string item;
  while (std::getline(stream, item, ',')) {
    size_t equals = item.find('=');
    GFNT_Tag t = tag4(item.substr(0, equals));
    double value = std::stod(item.substr(equals + 1));
    for (size_t i = 0; i < axes; ++i) {
      if (info[i].tag == t) {
        user[i] = static_cast<GFNT_F16Dot16>(value * 65536.0 + (value < 0 ? -0.5 : 0.5));
      }
    }
  }
  out.resize(axes);
  EXPECT_EQ(gfnt_face_normalize(face, user.data(), axes, out.data(), axes,
                nullptr), GFNT_OK);
  return out;
}

/** Shape, returning the result and (on success) the glyphs. */
GFNT_Result shape(const Font & font, const std::vector<uint32_t> & text,
    const Request & request, Glyphs * out, GFNT_Error * error = nullptr,
    const GFNT_Allocator * allocator = nullptr) {
  std::vector<GFNT_ShapeFeature> features = parse_features(request.features);
  GFNT_ShapeOptions options{};
  options.script = request.script.empty() || request.script == "-"
      ? 0 : tag4(request.script);
  options.language = request.language.empty() || request.language == "-"
      ? 0 : tag4(request.language);
  options.direction = request.vertical != GFNT_DIRECTION_LTR ? request.vertical
      : request.rtl ? GFNT_DIRECTION_RTL : GFNT_DIRECTION_LTR;
  options.features = features.empty() ? nullptr : features.data();
  options.feature_count = features.size();
  options.point_size = request.point_size;
  options.ppem = request.ppem;
  std::vector<GFNT_F2Dot14> coordinates =
      normalised_location(font.face, request.location);
  GFNT_Variation located{coordinates.data(), coordinates.size(), GFNT_DELTA_ROUND_HALF_UP };
  if (!coordinates.empty()) {
    options.variation = &located;
  }
  GFNT_ShapedRun run{};
  GFNT_Error local;
  gfnt_error_clear(&local);
  GFNT_Result result = gfnt_face_shape(font.face, text.data(), text.size(),
      &options, allocator, &run, error ? error : &local);
  if (result == GFNT_OK && out) {
    out->clear();
    for (size_t i = 0; i < run.count; ++i) {
      const GFNT_ShapedGlyph & g = run.glyphs[i];
      out->push_back({g.glyph, g.cluster, g.x_advance, g.y_advance,
          g.x_offset, g.y_offset});
    }
  }
  gfnt_shaped_run_free(&run);
  return result;
}

std::vector<uint32_t> cps(const std::string & ascii) {
  return std::vector<uint32_t>(ascii.begin(), ascii.end());
}

/** Just the glyph ids of a result. */
std::vector<uint32_t> ids(const Glyphs & g) {
  std::vector<uint32_t> out;
  for (const Glyph & x : g) {
    out.push_back(x.glyph);
  }
  return out;
}

Glyphs run_of(const Font & font, const std::vector<uint32_t> & text,
    const std::string & features = "") {
  Glyphs out;
  Request request;
  request.features = features;
  EXPECT_EQ(shape(font, text, request, &out), GFNT_OK);
  return out;
}

using V = std::vector<uint32_t>;

// layout-gsub.ttf: glyph ids are .notdef 0, space 1, a..z 2..27, a.alt1..3 28..30,
// f_i 31, f_f 32, f_f_i 33, f_l 34, o_o 35, acute 36, grave 37. Advances are
// 500 + 10 * (the glyph's index among the glyphs after space) for the letters, so
// a is 500 and f 550, with the ligatures at 900, 880, 1300, 910 and 1100.
constexpr uint32_t kA = 2, kB = 3, kC = 4, kD = 5, kF = 7, kH = 9, kI = 10,
    kL = 13, kM = 14, kO = 16, kP = 17, kQ = 18, kX = 25, kY = 26, kZ = 27,
    kAAlt2 = 29, kFI = 31, kFF = 32, kFFI = 33, kFL = 34, kOO = 35,
    kAcute = 36, kGrave = 37;

/** A `vhea` of @p count long metrics, and a `vmtx` giving each one 700 and tsb 50. */
std::vector<gfnttest::Table> vertical_tables(uint16_t count, bool vorg) {
  std::vector<uint8_t> vhea;
  gfnttest::put_u16(vhea, 1);
  gfnttest::put_u16(vhea, 0x1000);
  for (int i = 0; i < 15; ++i) {
    gfnttest::put_u16(vhea, 0);
  }
  gfnttest::put_u16(vhea, count);
  std::vector<uint8_t> vmtx;
  for (uint16_t i = 0; i < count; ++i) {
    gfnttest::put_u16(vmtx, 700);
    gfnttest::put_s16(vmtx, 50);
  }
  std::vector<gfnttest::Table> out = {{GFNT_TAG('v', 'h', 'e', 'a'), vhea},
      {GFNT_TAG('v', 'm', 't', 'x'), vmtx}};
  if (vorg) {
    std::vector<uint8_t> v;
    gfnttest::put_u16(v, 1);
    gfnttest::put_u16(v, 0);
    gfnttest::put_s16(v, 880);   // the default origin
    gfnttest::put_u16(v, 1);     // one exception
    gfnttest::put_u16(v, 2);     // glyph 2 ...
    gfnttest::put_s16(v, 760);   // ... sits higher up
    out.push_back({GFNT_TAG('V', 'O', 'R', 'G'), v});
  }
  return out;
}

Glyphs shape_vertical(std::vector<uint8_t> bytes, const std::vector<uint32_t> & text,
    GFNT_Direction direction, const std::string & features = "") {
  Font font(std::move(bytes));
  EXPECT_EQ(font.result, GFNT_OK);
  Request request;
  request.vertical = direction;
  request.features = features;
  Glyphs g;
  EXPECT_EQ(shape(font, text, request, &g), GFNT_OK);
  return g;
}

}  // namespace

// --- ligatures -------------------------------------------------------------

TEST(Shape, ALigatureTakesTheClusterOfItsFirstComponentAndItsWidth) {
  Font font("layout-gsub.ttf");
  ASSERT_EQ(font.result, GFNT_OK);
  // liga: f i -> f_i, whose advance is 900 (not 550 + 580).
  EXPECT_EQ(run_of(font, cps("fi")), (Glyphs{{kFI, 0, 900, 0, 0, 0}}));
}

TEST(Shape, TheLongestLigatureTheFontListsFirstWins) {
  Font font("layout-gsub.ttf");
  // f f i could be f_f + i or f f_i; the font lists f_f_i first, so it is one glyph.
  EXPECT_EQ(run_of(font, cps("ffi")), (Glyphs{{kFFI, 0, 1300, 0, 0, 0}}));
  // f f then l: there is no f f l ligature, so the f_f ligature forms and l stays.
  EXPECT_EQ(run_of(font, cps("ffl")),
      (Glyphs{{kFF, 0, 880, 0, 0, 0}, {kL, 2, 610, 0, 0, 0}}));
}

TEST(Shape, ALigatureTurnedOffLeavesTheLetters) {
  Font font("layout-gsub.ttf");
  EXPECT_EQ(ids(run_of(font, cps("fi"), "-liga")), (V{kF, kI}));
}

// --- multiple, single, alternate ---------------------------------------------

TEST(Shape, AMultipleSubstitutionKeepsEveryOutputOnTheOriginalCluster) {
  Font font("layout-gsub.ttf");
  // ccmp is on by default: h -> h i, and m -> m m m.
  Glyphs g = run_of(font, cps("hm"));
  EXPECT_EQ(ids(g), (V{kH, kI, kM, kM, kM}));
  EXPECT_EQ(g[0].cluster, 0u);
  EXPECT_EQ(g[1].cluster, 0u);
  EXPECT_EQ(g[2].cluster, 1u);
  EXPECT_EQ(g[4].cluster, 1u);
}

TEST(Shape, ASingleSubstitutionOfADeltaAndOfAList) {
  Font font("layout-gsub.ttf");
  // ss01: [a c e] -> [b d f], one delta of +1 (format 1).
  EXPECT_EQ(ids(run_of(font, cps("aceb"), "+ss01")), (V{3, 5, 7, 3}));
  // ss02: [g k] -> [x y], which no single delta describes (format 2).
  EXPECT_EQ(ids(run_of(font, cps("gk"), "+ss02")), (V{kX, kY}));
}

TEST(Shape, AFeatureOnARangeOnlyReachesThatRange) {
  Font font("layout-gsub.ttf");
  // ss01[1:3]: only the code points at 1 and 2 are substituted.
  EXPECT_EQ(ids(run_of(font, cps("aaaa"), "+ss01[1:3]")), (V{kA, kB, kB, kA}));
}

TEST(Shape, AnAlternateIsChosenByTheFeaturesValue) {
  Font font("layout-gsub.ttf");
  EXPECT_EQ(ids(run_of(font, cps("aba"), "salt=2")), (V{kAAlt2, kB, kAAlt2}));
  EXPECT_EQ(ids(run_of(font, cps("a"), "salt=1")), (V{28}));
  // The font has three alternates; a fourth is no alternate at all.
  EXPECT_EQ(ids(run_of(font, cps("a"), "salt=4")), (V{kA}));
  // Turned on without a value it is the first.
  EXPECT_EQ(ids(run_of(font, cps("a"), "+salt")), (V{28}));
}

// --- lookup flags -----------------------------------------------------------

TEST(Shape, IgnoreMarksLetsALigatureForm_OverAMark) {
  Font font("layout-gsub.ttf");
  // o acute o with IgnoreMarks: the acute sits between the components and is
  // skipped, so o_o forms and the acute stays, in the ligature's cluster.
  Glyphs g = run_of(font, V{'o', 0x301, 'o'}, "+dlig");
  ASSERT_EQ(g.size(), 2u);
  EXPECT_EQ(g[0].glyph, kOO);
  EXPECT_EQ(g[1].glyph, kAcute);
  EXPECT_EQ(g[0].cluster, 0u);
  EXPECT_EQ(g[1].cluster, 0u);
  // With no flags the same mark stops it.
  EXPECT_EQ(ids(run_of(font, V{'o', 0x301, 'o'}, "+hlig")),
      (V{kO, kAcute, kO}));
}

TEST(Shape, AMarkFilteringSetSkipsTheMarksOutsideIt) {
  Font font("layout-gsub.ttf");
  // The set is {acute}: grave is outside it and is skipped, acute is inside and
  // stops the ligature.
  EXPECT_EQ(ids(run_of(font, V{'o', 0x300, 'o'}, "+cswh")), (V{kOO, kGrave}));
  EXPECT_EQ(ids(run_of(font, V{'o', 0x301, 'o'}, "+cswh")),
      (V{kO, kAcute, kO}));
}

TEST(Shape, AMarkAttachmentTypeSkipsTheMarksOfOtherTypes) {
  Font font("layout-gsub.ttf");
  // The lookup keeps marks of the attachment class of grave; acute is another class.
  EXPECT_EQ(ids(run_of(font, V{'o', 0x301, 'o'}, "+cv01")),
      (V{kOO, kAcute}));
  EXPECT_EQ(ids(run_of(font, V{'o', 0x300, 'o'}, "+cv01")),
      (V{kO, kGrave, kO}));
}

// --- contexts and nested lookups ---------------------------------------------

TEST(Shape, ANestedLookupsPositionsFollowTheRunAsItGrows) {
  Font font("layout-gsub.ttf");
  // ss13 is a context over a b c whose records are indexed into the *current*
  // sequence: a -> a a at 0; then b, now at index 2, -> x; then c, now at 3,
  // -> c c c. So a a x c c c.
  EXPECT_EQ(ids(run_of(font, cps("abc"), "+ss13")),
      (V{kA, kA, kX, kC, kC, kC}));
  // The feature file's own ss03 names the original indices, so after the first
  // lookup grows the run its second and third reach the wrong glyphs, and only
  // the first has an effect.
  EXPECT_EQ(ids(run_of(font, cps("abc"), "+ss03")), (V{kA, kA, kB, kC}));
}

TEST(Shape, ANestedLookupThatDeletesStopsLaterOnesFromReachingPastTheEnd) {
  Font font("layout-gsub.ttf");
  // ss14: d e f, delete e at index 1, then f -> x at index 1. After the deletion
  // index 1 is the f, so d x.
  EXPECT_EQ(ids(run_of(font, cps("def"), "+ss14")), (V{kD, kX}));
}

TEST(Shape, AChainNeedsItsBacktrackAndItsLookahead) {
  Font font("layout-gsub.ttf");
  // ss04: [a b] c' [d e], the c tripled.
  EXPECT_EQ(ids(run_of(font, cps("acd"), "+ss04")), (V{kA, kC, kC, kC, kD}));
  EXPECT_EQ(ids(run_of(font, cps("cd"), "+ss04")), (V{kC, kD}));
  // ss05, glyph rules: p x' q, and r x' q; x -> y.
  EXPECT_EQ(ids(run_of(font, cps("pxq"), "+ss05")), (V{kP, kY, kQ}));
  EXPECT_EQ(ids(run_of(font, cps("rxq"), "+ss05")), (V{19, kY, kQ}));
  EXPECT_EQ(ids(run_of(font, cps("sxq"), "+ss05")), (V{20, kX, kQ}));
}

TEST(Shape, AContextTriesItsRulesInTheOrderTheFontGivesThem) {
  Font font("layout-gsub.ttf");
  // ss09, format 1: j k l has the longer rule first (l -> x at index 2) and j k
  // the shorter (k -> b at index 1).
  EXPECT_EQ(ids(run_of(font, cps("jkl"), "+ss09")), (V{11, 12, kX}));
  EXPECT_EQ(ids(run_of(font, cps("jk"), "+ss09")), (V{11, kB}));
  // ss10, format 2: a class-1 glyph then a class-2 one doubles the second.
  EXPECT_EQ(ids(run_of(font, cps("ac"), "+ss10")), (V{kA, kC, kC}));
  EXPECT_EQ(ids(run_of(font, cps("ab"), "+ss10")), (V{kA, kB}));
  EXPECT_EQ(ids(run_of(font, cps("ad"), "+ss10")), (V{kA, kD, kD}));
}

TEST(Shape, AReverseChainSeesWhatItHasAlreadyReplacedToItsRight) {
  Font font("layout-gsub.ttf");
  // ss06: rsub p' q by z.
  EXPECT_EQ(ids(run_of(font, cps("pq"), "+ss06")), (V{kZ, kQ}));
  // ss07: rsub [p q]' [q r] by [x y], run from the end: in p p q r the q (before
  // an r) becomes y first; the second p's right neighbour is now y, which is
  // not in [q r], so it stays.
  EXPECT_EQ(ids(run_of(font, cps("ppqr"), "+ss07")),
      (V{kP, kP, kY, 19}));
}

TEST(Shape, ALookupBehindAnExtensionAppliesLikeAnyOther) {
  Font font("layout-gsub.ttf");
  // ss08: [s t] -> [u v].
  EXPECT_EQ(ids(run_of(font, cps("stst"), "+ss08")), (V{22, 23, 22, 23}));
  // ss24: reverse chaining behind an extension is still run from the end, so in
  // p p q r the q before the r goes first and the p before it then sees a y.
  EXPECT_EQ(ids(run_of(font, cps("ppqr"), "+ss24")), (V{kP, kP, kY, 19}));
}

TEST(Shape, AMultipleSubstitutionToNothingDeletesTheGlyph) {
  Font font("layout-gsub.ttf");
  // ss11, chain format 2: s before, t u as the input, v after; the u goes.
  EXPECT_EQ(ids(run_of(font, cps("stuv"), "+ss11")), (V{20, 21, 23}));
  EXPECT_EQ(ids(run_of(font, cps("ktuv"), "+ss11")), (V{12, 21, 22, 23}));
  // ss12: e -> nothing.
  EXPECT_EQ(ids(run_of(font, cps("aeb"), "+ss12")), (V{kA, kB}));
  // A deleted glyph's cluster is not lost: the neighbour that remains takes the
  // smaller of the two.
  Glyphs g = run_of(font, cps("aeb"), "+ss12");
  EXPECT_EQ(g[1].cluster, 2u);
}

// --- GPOS ---------------------------------------------------------------------

// layout-gpos.ttf: A 2 (700), V 3 (700), T 4 (600), a 5 (500), o 6 (520),
// x 7 (480), y 8 (480), f 9 (330), i 10 (260), f_i 11 (560), c1 12, c2 13, c3 14
// (600 each), acute 15, grave 16, dotbelow 17 (zero width).

TEST(Shape, APairValueMovesTheFirstGlyphsAdvance) {
  Font font("layout-gpos.ttf");
  ASSERT_EQ(font.result, GFNT_OK);
  // pos A V -80: A's advance is 700 - 80.
  EXPECT_EQ(run_of(font, cps("AV")),
      (Glyphs{{2, 0, 620, 0, 0, 0}, {3, 1, 700, 0, 0, 0}}));
  // pos T o <-30 0 -60 0>: T is placed 30 left and loses 60 of advance.
  EXPECT_EQ(run_of(font, cps("To")),
      (Glyphs{{4, 0, 540, 0, -30, 0}, {6, 1, 520, 0, 0, 0}}));
  // A pair that is not listed is not adjusted.
  EXPECT_EQ(run_of(font, cps("TA")),
      (Glyphs{{4, 0, 600, 0, 0, 0}, {2, 1, 700, 0, 0, 0}}));
}

TEST(Shape, APairWithAValueForEachGlyphAdjustsBoth) {
  Font font("layout-gpos.ttf");
  // pos A <10 0 -50 0> T <0 5 5 0>: A is 10 right and 50 narrower; T is 5 up and
  // 5 wider.
  EXPECT_EQ(run_of(font, cps("AT")),
      (Glyphs{{2, 0, 650, 0, 10, 0}, {4, 1, 605, 0, 0, 5}}));
}

TEST(Shape, AClassPairAppliesToEveryMemberOfBothClasses) {
  Font font("layout-gpos.ttf");
  // pos @C1 @C2 -40 with C1 = [a o], C2 = [x y]; and pos [a] [y] -10 beside it,
  // which a glyph pair takes precedence for: a y is -10, o x is -40.
  EXPECT_EQ(run_of(font, cps("ay"))[0].x_advance, 490);
  EXPECT_EQ(run_of(font, cps("ox"))[0].x_advance, 480);
  EXPECT_EQ(run_of(font, cps("oy"))[0].x_advance, 480);
}

TEST(Shape, KerningTurnedOffIsNotApplied) {
  Font font("layout-gpos.ttf");
  EXPECT_EQ(run_of(font, cps("AV"), "-kern")[0].x_advance, 700);
  // Off on a range: a pair is kerned only if both its glyphs have the feature, so
  // A V A V with the last two off keeps the first pair and loses the second.
  Glyphs g = run_of(font, cps("AVAV"), "kern[2:4]=0");
  EXPECT_EQ(g[0].x_advance, 620);
  EXPECT_EQ(g[2].x_advance, 700);
  // Turned *on* over a range of a feature that is on already changes nothing.
  g = run_of(font, cps("AVAV"), "kern[1:3]");
  EXPECT_EQ(g[0].x_advance, 620);
  EXPECT_EQ(g[2].x_advance, 620);
}

TEST(Shape, CursiveGlyphsAreJoinedAndTheChildRidesItsParent) {
  Font font("layout-gpos.ttf");
  // c d e: c1 exits at (100, 20), c2 enters at (0, -10) and exits at (120, 10),
  // c3 enters at (-20, 0). The first advances by its exit x (100); the second by
  // its own exit x (120); the third takes the 20 its entry x sticks out. Each
  // child sits at its parent's exit height minus its own entry height: c2 30
  // above the baseline, c3 10 above c2, so 40.
  EXPECT_EQ(run_of(font, cps("cde")),
      (Glyphs{{12, 0, 100, 0, 0, 0}, {13, 1, 120, 0, 0, 30},
              {14, 2, 620, 0, 20, 40}}));
}

TEST(Shape, AMarkIsPlacedByTheAnchorOfItsBaseMinusItsOwn) {
  Font font("layout-gpos.ttf");
  // a has its top anchor at (250, 520) and acute's own is (0, 650): the mark is
  // moved (250, -130) from the base's origin, which is (250 - 500, -130) once the
  // base's advance - the pen has moved on - is taken back.
  EXPECT_EQ(run_of(font, V{'a', 0x301}),
      (Glyphs{{5, 0, 500, 0, 0, 0}, {15, 0, 0, 0, -250, -130}}));
  // Below: o's bottom anchor (250, -10) against dotbelow's own (50, 0): (200, -10),
  // less o's advance of 520.
  EXPECT_EQ(run_of(font, V{'o', 0x323}),
      (Glyphs{{6, 0, 520, 0, 0, 0}, {17, 0, 0, 0, -320, -10}}));
}

TEST(Shape, AMarkOnAMarkRidesTheMarkItIsOn) {
  Font font("layout-gpos.ttf");
  // grave on the acute that is on a: the acute is at (-250, -130); grave's anchor
  // on it is (0, 140) against its own (0, 0), so the grave is 140 higher.
  EXPECT_EQ(run_of(font, V{'a', 0x301, 0x300}),
      (Glyphs{{5, 0, 500, 0, 0, 0}, {15, 0, 0, 0, -250, -130},
              {16, 0, 0, 0, -250, 10}}));
}

TEST(Shape, AMarkOnALigatureGoesToTheComponentItBelongedTo) {
  Font font("layout-gpos.ttf");
  // f acute i ligates (IgnoreMarks), leaving the acute as the first component's:
  // anchor (200, 700) against its own (0, 650): (200, 50), less the ligature's 560.
  EXPECT_EQ(run_of(font, V{'f', 0x301, 'i'}),
      (Glyphs{{11, 0, 560, 0, 0, 0}, {15, 0, 0, 0, -360, 50}}));
  // After the ligature the mark is the last component's, whose anchor is (450, 700).
  EXPECT_EQ(run_of(font, V{'f', 'i', 0x301}),
      (Glyphs{{11, 0, 560, 0, 0, 0}, {15, 0, 0, 0, -110, 50}}));
}

TEST(Shape, ASingleAdjustmentIsAddedToTheGlyphsOwnMetrics) {
  Font font("layout-gpos.ttf");
  // pos a <10 20 30 40>: 10 right, 20 up, 30 wider. The 40 is a *vertical*
  // advance and horizontal text does not use it.
  EXPECT_EQ(run_of(font, cps("a"), "+ss01"),
      (Glyphs{{5, 0, 530, 0, 10, 20}}));
}

TEST(Shape, ALookupsOwnAdjustmentsAddToTheKerning) {
  Font font("layout-gpos.ttf");
  // ss02: A -10 wider, V 20 wider and 5 right. With kerning (A V -80): A is
  // 700 - 80 - 10, and V 700 + 20 at offset 5.
  EXPECT_EQ(run_of(font, cps("AV"), "+ss02"),
      (Glyphs{{2, 0, 610, 0, 0, 0}, {3, 1, 720, 0, 5, 0}}));
}

TEST(Shape, AContextAppliesItsNestedLookupToTheGlyphItNames) {
  Font font("layout-gpos.ttf");
  // ss09, format 1: T a o has the longer rule first (o loosened by 25 at index
  // 2); T a the shorter (a tightened by 35 at index 1).
  EXPECT_EQ(run_of(font, cps("Tao"), "+ss09")[2].x_advance, 545);
  EXPECT_EQ(run_of(font, cps("Ta"), "+ss09")[1].x_advance, 465);
  // ss10, format 2: A then a: the a is loosened.
  EXPECT_EQ(run_of(font, cps("Aa"), "+ss10")[1].x_advance, 525);
}

TEST(Shape, ALookupBehindAnExtensionAppliesLikeAnyOtherPosition) {
  Font font("layout-gpos.ttf");
  // ss05: pos A T -25 behind an extension, on top of the kern table's A T pair.
  EXPECT_EQ(run_of(font, cps("AT"), "+ss05")[0].x_advance, 625);
}

// --- direction ---------------------------------------------------------------

TEST(Shape, ARightToLeftRunInALeftToRightScriptIsShapedInReadingOrder) {
  Font font("layout-gpos.ttf");
  Request rtl;
  rtl.rtl = true;
  Glyphs g;
  // The glyphs come back in visual order. Latin reads left to right, so the
  // text is turned round first and the lookups see V A: no pair, no kerning.
  ASSERT_EQ(shape(font, cps("AV"), rtl, &g), GFNT_OK);
  EXPECT_EQ(g, (Glyphs{{3, 1, 700, 0, 0, 0}, {2, 0, 700, 0, 0, 0}}));
  // And V A, turned round, is the kerned pair A V.
  ASSERT_EQ(shape(font, cps("VA"), rtl, &g), GFNT_OK);
  EXPECT_EQ(g, (Glyphs{{2, 1, 620, 0, 0, 0}, {3, 0, 700, 0, 0, 0}}));
}

// --- the run -------------------------------------------------------------------

TEST(Shape, AnEmptyTextIsAnEmptyRun) {
  Font font("layout-gpos.ttf");
  GFNT_ShapedRun run{};
  GFNT_Error error;
  ASSERT_EQ(gfnt_face_shape(font.face, nullptr, 0, nullptr, nullptr, &run,
                &error), GFNT_OK);
  EXPECT_EQ(run.count, 0u);
  gfnt_shaped_run_free(&run);
  gfnt_shaped_run_free(&run);  // twice is harmless
  gfnt_shaped_run_free(nullptr);
}

TEST(Shape, ACharacterTheFontLacksIsNotdefAndStaysInTheRun) {
  Font font("layout-gpos.ttf");
  Glyphs g = run_of(font, V{'A', 0x4E2D, 'V'});
  ASSERT_EQ(g.size(), 3u);
  EXPECT_EQ(g[1].glyph, 0u);
  EXPECT_EQ(g[1].cluster, 1u);
}

TEST(Shape, TheRunSaysWhichScriptAndHowManyLookupsRan) {
  Font font("layout-gpos.ttf");
  GFNT_ShapeOptions options{};
  options.script = tag4("latn");
  GFNT_ShapedRun run{};
  GFNT_Error error;
  std::vector<uint32_t> text = cps("AV");
  ASSERT_EQ(gfnt_face_shape(font.face, text.data(), text.size(), &options,
                nullptr, &run, &error), GFNT_OK);
  EXPECT_EQ(run.script, tag4("latn"));
  EXPECT_GT(run.gpos_lookups, 0u);
  gfnt_shaped_run_free(&run);
}

TEST(Shape, ABadArgumentIsRefusedAndAStrayRangeIsRefused) {
  Font font("layout-gpos.ttf");
  GFNT_ShapedRun run{};
  GFNT_Error error;
  std::vector<uint32_t> text = cps("A");
  EXPECT_EQ(gfnt_face_shape(nullptr, text.data(), 1, nullptr, nullptr, &run,
                &error), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_shape(font.face, text.data(), 1, nullptr, nullptr,
                nullptr, &error), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_shape(font.face, nullptr, 1, nullptr, nullptr, &run,
                &error), GFNT_ERR_INVALID);
  GFNT_ShapeFeature backwards{tag4("kern"), 1, 3, 1};
  GFNT_ShapeOptions options{};
  options.features = &backwards;
  options.feature_count = 1;
  EXPECT_EQ(gfnt_face_shape(font.face, text.data(), 1, &options, nullptr, &run,
                &error), GFNT_ERR_INVALID);
}

// --- FeatureVariations ---------------------------------------------------------

// variable-featurevars.ttf: glyphs .notdef 0, space 1, tri 2, bar 3, ring 4, dot 5,
// acc 6, acc2 7. 'A' is tri and 'B' is bar. Its GSUB 1.1 has three records, tried in
// order: wght >= 0.5 turns bar into dot; wght <= -0.5 with wdth >= 0.25 turns tri
// into ring; wdth <= -0.5 turns bar into acc and tri into acc2. fontTools writes a
// record of its own for a place where two rules overlap, overlaying them in the
// order given, so there the later rule wins where both name a glyph. Conditions are
// on *normalised* coordinates, after avar, which bends wght: 575 is 0.35 of the way
// up and avar maps that to 0.525.
TEST(ShapeVariation, TheFirstRecordWhoseConditionsHoldSubstitutesItsFeature) {
  Font font("variable-featurevars.ttf");
  ASSERT_EQ(font.result, GFNT_OK);
  auto at = [&](const char * location) {
    Request request;
    request.script = "";
    request.location = location;
    Glyphs g;
    EXPECT_EQ(shape(font, cps("AB"), request, &g), GFNT_OK) << location;
    return ids(g);
  };
  EXPECT_EQ(at(""), (V{2, 3}));                  // no record holds
  EXPECT_EQ(at("wght=900"), (V{2, 5}));          // record 1: bar -> dot
  EXPECT_EQ(at("wght=100,wdth=125"), (V{4, 3})); // record 2: tri -> ring
  EXPECT_EQ(at("wdth=75"), (V{7, 6}));           // record 3: acc2, acc
  // Rules 1 and 3 overlap here, and the overlap's record has both: bar is named by
  // each and the later (acc) wins, and tri comes from rule 3 alone.
  EXPECT_EQ(at("wght=900,wdth=75"), (V{7, 6}));
  EXPECT_EQ(at("wght=560"), (V{2, 3}));          // avar: 0.48, just short
  EXPECT_EQ(at("wght=575"), (V{2, 5}));          // avar: 0.525
}

// --- the kern table, default ignorables, and marks without a GPOS ----------------

namespace {

/** A font of glyphs 0..N whose cmap maps 'A'+i to glyph i+1, every advance 500. */
std::vector<uint8_t> small_font(const std::vector<gfnttest::Table> & extra,
    uint16_t glyphs = 8,
    const std::vector<std::pair<uint32_t, uint16_t>> & more_cmap = {}) {
  std::vector<gfnttest::Segment4> segments;
  segments.push_back({'A', static_cast<uint16_t>('A' + glyphs - 2),
      static_cast<int16_t>(1 - 'A'), {}});
  for (const auto & m : more_cmap) {
    segments.push_back({static_cast<uint16_t>(m.first),
        static_cast<uint16_t>(m.first),
        static_cast<int16_t>(m.second - m.first), {}});
  }
  std::sort(segments.begin(), segments.end(),
      [](const gfnttest::Segment4 & a, const gfnttest::Segment4 & b) {
        return a.start < b.start;
      });
  segments.push_back({0xFFFF, 0xFFFF, 1, {}});
  std::vector<std::pair<uint16_t, int16_t>> metrics(glyphs, {500, 0});
  std::vector<gfnttest::Table> tables = {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, glyphs)},
      {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics)},
      {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(glyphs)},
      {GFNT_TAG('c', 'm', 'a', 'p'),
          gfnttest::build_cmap({{3, 1, gfnttest::build_cmap_format4(segments)}})},
  };
  tables.insert(tables.end(), extra.begin(), extra.end());
  return gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
}

/** A `kern` table, Microsoft header, one horizontal format 0 subtable. */
std::vector<uint8_t> kern_format0(
    const std::vector<std::tuple<uint16_t, uint16_t, int16_t>> & pairs,
    uint16_t coverage = 0x0001) {
  std::vector<uint8_t> out;
  gfnttest::put_u16(out, 0);  // version
  gfnttest::put_u16(out, 1);  // one subtable
  gfnttest::put_u16(out, 0);  // subtable version
  gfnttest::put_u16(out, static_cast<uint16_t>(14 + 6 * pairs.size()));
  gfnttest::put_u16(out, coverage);
  gfnttest::put_u16(out, static_cast<uint16_t>(pairs.size()));
  gfnttest::put_u16(out, 0);  // searchRange, entrySelector, rangeShift: not read
  gfnttest::put_u16(out, 0);
  gfnttest::put_u16(out, 0);
  for (const auto & p : pairs) {
    gfnttest::put_u16(out, std::get<0>(p));
    gfnttest::put_u16(out, std::get<1>(p));
    gfnttest::put_s16(out, std::get<2>(p));
  }
  return out;
}

Glyphs shape_bytes(std::vector<uint8_t> bytes, const std::vector<uint32_t> & text,
    const std::string & features = "", const std::string & script = "latn") {
  Font font(std::move(bytes));
  EXPECT_EQ(font.result, GFNT_OK);
  Request request;
  request.script = script;
  request.features = features;
  Glyphs g;
  EXPECT_EQ(shape(font, text, request, &g), GFNT_OK);
  return g;
}

}  // namespace

TEST(ShapeKern, APairIsSharedBetweenTheTwoGlyphsAndTheSecondMovesWithItsShare) {
  // Glyphs A 1, B 2, C 3, D 4. Pairs A B -50, B C +20, C D -51. A kern value is
  // split in two with an arithmetic shift, so -51 is -26 for the first glyph and
  // -25 for the second, and the second glyph's offset takes its share too. B is
  // second in the first pair and first in the second, so it gets both.
  Glyphs g = shape_bytes(
      small_font({{GFNT_TAG('k', 'e', 'r', 'n'),
          kern_format0({{1, 2, -50}, {2, 3, 20}, {3, 4, -51}})}}),
      cps("ABCD"));
  ASSERT_EQ(g.size(), 4u);
  EXPECT_EQ(g[0].x_advance, 475);            // 500 - 25
  EXPECT_EQ(g[1].x_advance, 485);            // 500 - 25 + 10
  EXPECT_EQ(g[1].x_offset, -25);
  EXPECT_EQ(g[2].x_advance, 484);            // 500 + 10 - 26
  EXPECT_EQ(g[2].x_offset, 10);
  EXPECT_EQ(g[3].x_advance, 475);            // 500 - 25
  EXPECT_EQ(g[3].x_offset, -25);
}

TEST(ShapeKern, KerningOffLeavesTheTableAlone) {
  Glyphs g = shape_bytes(
      small_font({{GFNT_TAG('k', 'e', 'r', 'n'), kern_format0({{1, 2, -50}})}}),
      cps("AB"), "-kern");
  EXPECT_EQ(g[0].x_advance, 500);
  EXPECT_EQ(g[1].x_offset, 0);
}

TEST(ShapeKern, ACrossStreamOrVerticalSubtableIsNotAppliedToHorizontalText) {
  // Coverage bit 0 is horizontal, bit 2 cross-stream: only horizontal and not
  // cross-stream moves the pen.
  EXPECT_EQ(shape_bytes(small_font({{GFNT_TAG('k', 'e', 'r', 'n'),
                kern_format0({{1, 2, -50}}, 0x0000)}}), cps("AB"))[0].x_advance,
      500);
  EXPECT_EQ(shape_bytes(small_font({{GFNT_TAG('k', 'e', 'r', 'n'),
                kern_format0({{1, 2, -50}}, 0x0005)}}), cps("AB"))[0].x_advance,
      500);
}

TEST(ShapeKern, AGposWithNoKernFeatureLeavesKerningToTheTable) {
  // A stub GPOS: a header whose script, feature and lookup lists are empty. The
  // font has nothing to say about kerning there, so the kern table does.
  std::vector<uint8_t> gpos;
  gfnttest::put_u16(gpos, 1);
  gfnttest::put_u16(gpos, 0);
  gfnttest::put_u16(gpos, 10);  // scripts
  gfnttest::put_u16(gpos, 12);  // features
  gfnttest::put_u16(gpos, 14);  // lookups
  gfnttest::put_u16(gpos, 0);
  gfnttest::put_u16(gpos, 0);
  gfnttest::put_u16(gpos, 0);
  Glyphs g = shape_bytes(
      small_font({{GFNT_TAG('G', 'P', 'O', 'S'), gpos},
                  {GFNT_TAG('k', 'e', 'r', 'n'), kern_format0({{1, 2, -50}})}}),
      cps("AB"));
  EXPECT_EQ(g[0].x_advance, 475);
}

TEST(ShapeKern, AnAppleHeaderIsReadToo) {
  std::vector<uint8_t> kern;
  gfnttest::put_u32(kern, 0x00010000);
  gfnttest::put_u32(kern, 1);
  gfnttest::put_u32(kern, 8 + 8 + 6);   // subtable length
  gfnttest::put_u16(kern, 0x0000);      // horizontal, format 0
  gfnttest::put_u16(kern, 0);           // tuple index
  gfnttest::put_u16(kern, 1);
  gfnttest::put_u16(kern, 0);
  gfnttest::put_u16(kern, 0);
  gfnttest::put_u16(kern, 0);
  gfnttest::put_u16(kern, 1);
  gfnttest::put_u16(kern, 2);
  gfnttest::put_s16(kern, -50);
  Glyphs g = shape_bytes(small_font({{GFNT_TAG('k', 'e', 'r', 'n'), kern}}),
      cps("AB"));
  EXPECT_EQ(g[0].x_advance, 475);
  EXPECT_EQ(g[1].x_offset, -25);
}

TEST(ShapeIgnorables, AJoinerIsDrawnAsTheSpaceGlyphWhenTheFontHasOneAndDroppedWhenItDoesNot) {
  // U+200D is default-ignorable: it has no ink and no width. A font with a space
  // glyph (here 7, from U+0020) keeps the run's length by showing that, with no
  // advance; a font without one drops it, and the cluster it was in goes with it.
  Glyphs with_space = shape_bytes(
      small_font({}, 8, {{0x20, 7}, {0x200D, 6}}), V{'A', 0x200D, 'B'});
  ASSERT_EQ(with_space.size(), 3u);
  EXPECT_EQ(with_space[1].glyph, 7u);
  EXPECT_EQ(with_space[1].x_advance, 0);
  // A joiner is part of the grapheme before it, so it shares its cluster.
  EXPECT_EQ(with_space[1].cluster, 0u);
  EXPECT_EQ(with_space[2].cluster, 2u);
  Glyphs without = shape_bytes(small_font({}, 8, {{0x200D, 6}}),
      V{'A', 0x200D, 'B'});
  ASSERT_EQ(without.size(), 2u);
  EXPECT_EQ(without[0].cluster, 0u);
  EXPECT_EQ(without[1].cluster, 2u);
}

TEST(ShapeMarks, AMarkWithNoGposIsZeroWidthAndPulledBackOverItsBase) {
  // GDEF with a glyph class definition giving glyph 6 the class mark. Without a
  // GPOS nothing says where it goes: its advance is taken away and it is moved
  // back over the base by that much (HarfBuzz then places it from the glyph's
  // extents, which this library does not do - hb_diff.py names the gap).
  std::vector<uint8_t> gdef;
  gfnttest::put_u16(gdef, 1);
  gfnttest::put_u16(gdef, 0);
  gfnttest::put_u16(gdef, 12);  // glyph class def
  gfnttest::put_u16(gdef, 0);
  gfnttest::put_u16(gdef, 0);
  gfnttest::put_u16(gdef, 0);
  gfnttest::put_u16(gdef, 2);   // class def format 2
  gfnttest::put_u16(gdef, 1);
  gfnttest::put_u16(gdef, 6);
  gfnttest::put_u16(gdef, 6);
  gfnttest::put_u16(gdef, 3);   // mark
  Glyphs g = shape_bytes(small_font({{GFNT_TAG('G', 'D', 'E', 'F'), gdef}}, 8,
                             {{0x301, 6}}),
      V{'A', 0x301});
  ASSERT_EQ(g.size(), 2u);
  EXPECT_EQ(g[0].x_advance, 500);
  EXPECT_EQ(g[1].x_advance, 0);
  EXPECT_EQ(g[1].x_offset, -500);
  // A mark belongs to its base's cluster: a grapheme is never split.
  EXPECT_EQ(g[1].cluster, 0u);
}

TEST(ShapeDirection, ARightToLeftScriptIsShapedAsItIsReadAndComesBackInVisualOrder) {
  std::vector<uint8_t> bytes = small_font({});
  Glyphs rtl;
  {
    Font font(bytes);
    Request request;
    request.script = "hebr";
    request.rtl = true;
    EXPECT_EQ(shape(font, cps("AB"), request, &rtl), GFNT_OK);
  }
  ASSERT_EQ(rtl.size(), 2u);
  EXPECT_EQ(rtl[0].glyph, 2u);   // B first: the last letter is the leftmost
  EXPECT_EQ(rtl[0].cluster, 1u);
  EXPECT_EQ(rtl[1].glyph, 1u);
  EXPECT_EQ(rtl[1].cluster, 0u);
  // The same script given left-to-right text comes back in text order.
  Glyphs ltr;
  {
    Font font(bytes);
    Request request;
    request.script = "hebr";
    EXPECT_EQ(shape(font, cps("AB"), request, &ltr), GFNT_OK);
  }
  EXPECT_EQ(ids(ltr), (V{1, 2}));
}

// --- more of what the engine decides ---------------------------------------------

// layout-gsub.ttf gained a mark ligature glyph "ag" (38), and layout-gpos.ttf pair
// sets and cases of its own; see tools/fixtures/layout_fixtures.py.

TEST(Shape, ARuleOverMarksMayNotReachAcrossALigatureTheyBelongTo) {
  Font font("layout-gsub.ttf");
  // o acute o grave: dlig (IgnoreMarks) ligates the o's with the acute between
  // them, so the acute is the ligature's first component's and the grave its
  // second's. A rule over acute grave sees marks of two different components and
  // does not match; one that skips ligatures may look past the ligature to them.
  constexpr uint32_t kAG = 38;
  V text{'o', 0x301, 'o', 0x300};
  EXPECT_EQ(ids(run_of(font, text, "+dlig,+ss15")), (V{kOO, kAcute, kGrave}));
  EXPECT_EQ(ids(run_of(font, text, "+dlig,+ss16")), (V{kOO, kAG}));
  // Two marks that were never in a ligature do ligate.
  EXPECT_EQ(ids(run_of(font, V{0x301, 0x300}, "+ss15")), (V{kAG}));
  // A lookup that skips marks cannot match a rule made of them, whether the rule
  // starts on a mark (ss17) or has one as its second glyph (ss25: o acute -> x).
  EXPECT_EQ(ids(run_of(font, V{0x301, 0x300}, "+ss17")), (V{kAcute, kGrave}));
  EXPECT_EQ(ids(run_of(font, V{'o', 0x301}, "+ss25")), (V{kO, kAcute}));
}

TEST(Shape, AMarkReplacedByABaseIsABaseAndTakesRoom) {
  Font font("layout-gsub.ttf");
  // ss18: acute -> a. The substituted glyph has the class its new glyph has, so
  // it is no longer a mark to the lookups. The font has no GPOS, though, so the
  // shaper places marks itself, and it goes by the character: U+0301 is still a
  // combining mark, its advance is taken away and it is set over the o.
  Glyphs g = run_of(font, V{'o', 0x301}, "+ss18");
  ASSERT_EQ(g.size(), 2u);
  EXPECT_EQ(g[1].glyph, kA);
  EXPECT_EQ(g[1].x_advance, 0);
  EXPECT_EQ(g[1].x_offset, -510);
  EXPECT_EQ(g[1].y_offset, 662);
}

TEST(Shape, AChainOnlyAppliesToGlyphsItsFirstInputCoverageHolds) {
  Font font("layout-gsub.ttf");
  // ss19: [a b] c' [d e], with a nested lookup that turns x into y. The glyph after
  // the a is an x, not a c, so the rule does not apply to it and the x stays.
  EXPECT_EQ(ids(run_of(font, cps("axd"), "+ss19")), (V{kA, kX, kD}));
}

TEST(Shape, JoinersAreSkippedWhereAContextLooksAndNotInsideAMatch) {
  Font font("layout-gsub.ttf");
  // A ZWNJ between the f and the i stops the ligature, and a ZWJ does not. The
  // joiner itself is drawn as the space glyph, 1, with no advance.
  EXPECT_EQ(ids(run_of(font, V{'f', 0x200C, 'i'})), (V{kF, 1, kI}));
  EXPECT_EQ(ids(run_of(font, V{'f', 0x200D, 'i'})), (V{kFI, 1}));
  // In a chain's lookahead and backtrack a ZWNJ is skipped, so the rule applies.
  EXPECT_EQ(ids(run_of(font, V{'a', 'c', 0x200C, 'd'}, "+ss04")),
      (V{kA, kC, kC, kC, 1, kD}));
  EXPECT_EQ(ids(run_of(font, V{'a', 0x200C, 'c', 'd'}, "+ss04")),
      (V{kA, 1, kC, kC, kC, kD}));
}

TEST(Shape, NestedRecordsMayRewindAndMayAimAtWhatAnEarlierOneInserted) {
  Font font("layout-gsub.ttf");
  // ss20: records (2: c -> c c c) then (1: b -> x). The second rewinds over the
  // first's output to the b, which is now in the middle of it: a x c c c.
  EXPECT_EQ(ids(run_of(font, cps("abc"), "+ss20")), (V{kA, kX, kC, kC, kC}));
  EXPECT_EQ(ids(run_of(font, cps("abcabc"), "+ss20")),
      (V{kA, kX, kC, kC, kC, kA, kX, kC, kC, kC}));
  // ss21: records (0: a -> a a) then (1: a -> x), and index 1 is now the a the
  // first just inserted: a x b c.
  EXPECT_EQ(ids(run_of(font, cps("abc"), "+ss21")), (V{kA, kX, kB, kC}));
}

TEST(Shape, ALigatureOfOneComponentIsAReplacementAndAReverseLookupCannotBeCalled) {
  Font font("layout-gsub.ttf");
  // ss22: one-component ligature j -> k.
  EXPECT_EQ(ids(run_of(font, cps("j"), "+ss22")), (V{12}));
  // ss23: a context whose record names the reverse chaining lookup (rsub p' q by
  // z). A reverse lookup cannot be nested, so nothing happens.
  EXPECT_EQ(ids(run_of(font, cps("pq"), "+ss23")), (V{kP, kQ}));
}

TEST(Shape, AFeatureOffOnTheSecondGlyphOfAPairStopsThePair) {
  Font font("layout-gpos.ttf");
  // Both glyphs have to carry the feature: with it off on the V there is no A V.
  EXPECT_EQ(run_of(font, cps("AV"), "kern[1:2]=0")[0].x_advance, 700);
  EXPECT_EQ(run_of(font, cps("AV"))[0].x_advance, 620);
}

TEST(Shape, APairSetIsSearchedForItsSecondGlyph) {
  Font font("layout-gpos.ttf");
  // f has five partners (x -11, y -12, a -13, o -14, T -15) in one pair set.
  EXPECT_EQ(run_of(font, cps("fx"))[0].x_advance, 319);
  EXPECT_EQ(run_of(font, cps("fy"))[0].x_advance, 318);
  EXPECT_EQ(run_of(font, cps("fa"))[0].x_advance, 317);
  EXPECT_EQ(run_of(font, cps("fo"))[0].x_advance, 316);
  EXPECT_EQ(run_of(font, cps("fT"))[0].x_advance, 315);
  // A is not one of them.
  EXPECT_EQ(run_of(font, cps("fA"))[0].x_advance, 330);
}

TEST(Shape, AGlyphGivenAValueAsTheSecondOfAPairIsNotTheFirstOfTheNext) {
  Font font("layout-gpos.ttf");
  // A T carries a value for the T, so T is consumed; T o, which would shrink the T
  // by another 60 and shift it 30, is not tried. T keeps 605 (600 + 5 from A T).
  Glyphs g = run_of(font, cps("ATo"));
  ASSERT_EQ(g.size(), 3u);
  EXPECT_EQ(g[1].x_advance, 605);
  EXPECT_EQ(g[1].x_offset, 0);
  // Without the A in front, T o is a pair like any other.
  EXPECT_EQ(run_of(font, cps("To"))[0].x_advance, 540);
}

TEST(Shape, MarksOnTwoComponentsOfOneLigatureAreNotOnEachOther) {
  Font font("layout-gpos.ttf");
  // f acute i grave: the ligature takes both, the acute on its first component
  // (200, 700) and the grave on its last (450, 700); the mark-to-mark lookup does
  // not hang the grave on the acute, because they belong to different components.
  Glyphs g = run_of(font, V{'f', 0x301, 'i', 0x300});
  ASSERT_EQ(g.size(), 3u);
  EXPECT_EQ(g[1].x_offset, -360);   // acute: 200 - 560
  EXPECT_EQ(g[2].x_offset, -110);   // grave: 450 - 560
  EXPECT_EQ(g[2].y_offset, 50);
}

TEST(Shape, LigatureIdsAreReusedAndMarksStillFindTheirComponent) {
  Font font("layout-gpos.ttf");
  // Nine of f acute i. A ligature id is three bits, and the eighth would be 0,
  // which means "no ligature", so it takes the next one instead; every acute must
  // be on its ligature's first component.
  V text;
  for (int i = 0; i < 9; ++i) {
    text.insert(text.end(), {'f', 0x301, 'i'});
  }
  Glyphs g = run_of(font, text);
  ASSERT_EQ(g.size(), 18u);
  for (size_t i = 1; i < g.size(); i += 2) {
    EXPECT_EQ(g[i].glyph, 15u) << i;
    EXPECT_EQ(g[i].x_offset, -360) << "ligature " << i / 2;
  }
}

TEST(Shape, AMarkToMarkLookupThatSkipsLigaturesStillStopsAtTheLigature) {
  Font font("layout-gpos.ttf");
  // ss08: mark to mark, skipping ligatures. acute f_i grave: the grave's
  // predecessor is the ligature, which is not a mark, so it does not attach to
  // the acute on the far side of it.
  Glyphs g = run_of(font, V{0x301, 'f', 'i', 0x300}, "+ss08");
  ASSERT_EQ(g.size(), 3u);
  EXPECT_EQ(g[2].glyph, 16u);
}

TEST(ShapeDirection, AnIgnorableDeletedFromAReversedRunMergesItsClusterBackward) {
  // A Latin run read right to left is turned round first: C Z B A with Z a
  // zero-width space. The font has no space glyph, so Z is dropped; its cluster
  // (2) is less than the C before it (3), which then takes it, as it would if the
  // run had been left to right and C had come after.
  std::vector<uint8_t> bytes = small_font({}, 8, {{0x200B, 6}});
  Font font(bytes);
  Request request;
  request.rtl = true;
  Glyphs g;
  ASSERT_EQ(shape(font, V{'A', 'B', 0x200B, 'C'}, request, &g), GFNT_OK);
  ASSERT_EQ(g.size(), 3u);
  EXPECT_EQ(g[0].glyph, 3u);
  EXPECT_EQ(g[0].cluster, 2u);
  EXPECT_EQ(g[1].cluster, 1u);
  EXPECT_EQ(g[2].cluster, 0u);
}

// --- hand-built layout tables: the plan --------------------------------------------

namespace {

struct HandLookup {
  uint16_t type;
  uint16_t flag;
  std::vector<uint8_t> subtable;
};

struct HandFeature {
  const char * tag;
  std::vector<uint16_t> lookups;
};

/** A single substitution, format 1: each glyph in @p glyphs gets @p delta added. */
std::vector<uint8_t> single_delta(const std::vector<uint16_t> & glyphs,
    int16_t delta) {
  std::vector<uint8_t> out;
  gfnttest::put_u16(out, 1);
  gfnttest::put_u16(out, 6);
  gfnttest::put_s16(out, delta);
  gfnttest::put_u16(out, 1);
  gfnttest::put_u16(out, static_cast<uint16_t>(glyphs.size()));
  for (uint16_t g : glyphs) {
    gfnttest::put_u16(out, g);
  }
  return out;
}

/**
 * A `GSUB` or `GPOS` with one script, one language system that lists every feature
 * (and names @p required, if it is not -1, as its required feature), and the given
 * lookups. Offsets are computed here so that a test names what it wants and not
 * where it goes.
 */
std::vector<uint8_t> layout_table(const char * script, int required,
    const std::vector<HandFeature> & features,
    const std::vector<HandLookup> & lookups) {
  std::vector<uint8_t> scripts;
  gfnttest::put_u16(scripts, 1);
  for (int i = 0; i < 4; ++i) {
    scripts.push_back(static_cast<uint8_t>(script[i]));
  }
  gfnttest::put_u16(scripts, 8);       // script table
  gfnttest::put_u16(scripts, 4);       // default language system
  gfnttest::put_u16(scripts, 0);       // no others
  gfnttest::put_u16(scripts, 0);       // lookup order
  gfnttest::put_u16(scripts, required < 0 ? 0xFFFF : static_cast<uint16_t>(required));
  gfnttest::put_u16(scripts, static_cast<uint16_t>(features.size()));
  for (size_t i = 0; i < features.size(); ++i) {
    gfnttest::put_u16(scripts, static_cast<uint16_t>(i));
  }

  std::vector<uint8_t> feature_list;
  gfnttest::put_u16(feature_list, static_cast<uint16_t>(features.size()));
  size_t at = 2 + 6 * features.size();
  std::vector<uint8_t> feature_tables;
  for (const HandFeature & f : features) {
    for (int i = 0; i < 4; ++i) {
      feature_list.push_back(static_cast<uint8_t>(f.tag[i]));
    }
    gfnttest::put_u16(feature_list, static_cast<uint16_t>(at + feature_tables.size()));
    gfnttest::put_u16(feature_tables, 0);
    gfnttest::put_u16(feature_tables, static_cast<uint16_t>(f.lookups.size()));
    for (uint16_t l : f.lookups) {
      gfnttest::put_u16(feature_tables, l);
    }
  }
  feature_list.insert(feature_list.end(), feature_tables.begin(),
      feature_tables.end());

  std::vector<uint8_t> lookup_list;
  gfnttest::put_u16(lookup_list, static_cast<uint16_t>(lookups.size()));
  std::vector<uint8_t> bodies;
  size_t base = 2 + 2 * lookups.size();
  for (const HandLookup & l : lookups) {
    gfnttest::put_u16(lookup_list, static_cast<uint16_t>(base + bodies.size()));
    gfnttest::put_u16(bodies, l.type);
    gfnttest::put_u16(bodies, l.flag);
    gfnttest::put_u16(bodies, 1);
    gfnttest::put_u16(bodies, 8);
    bodies.insert(bodies.end(), l.subtable.begin(), l.subtable.end());
  }
  lookup_list.insert(lookup_list.end(), bodies.begin(), bodies.end());

  std::vector<uint8_t> out;
  gfnttest::put_u16(out, 1);
  gfnttest::put_u16(out, 0);
  gfnttest::put_u16(out, 10);
  gfnttest::put_u16(out, static_cast<uint16_t>(10 + scripts.size()));
  gfnttest::put_u16(out, static_cast<uint16_t>(10 + scripts.size() + feature_list.size()));
  out.insert(out.end(), scripts.begin(), scripts.end());
  out.insert(out.end(), feature_list.begin(), feature_list.end());
  out.insert(out.end(), lookup_list.begin(), lookup_list.end());
  return out;
}

const GFNT_Tag kGSUB = GFNT_TAG('G', 'S', 'U', 'B');
const GFNT_Tag kGPOS = GFNT_TAG('G', 'P', 'O', 'S');

}  // namespace

TEST(ShapePlan, TheFeatureThatRunsBeforeTheOthersRunsBeforeThemWhateverItsLookupIndex) {
  // calt -> lookup 0 (A -> B), rvrn -> lookup 1 (B -> C). rvrn is the one feature
  // that has a pass to itself, ahead of everything: so lookup 1 runs first, finds no
  // B, and then lookup 0 makes the A a B. Run in index order it would be a C.
  Glyphs g = shape_bytes(small_font({{kGSUB,
          layout_table("latn", -1, {{"calt", {0}}, {"rvrn", {1}}},
              {{1, 0, single_delta({1}, 1)}, {1, 0, single_delta({2}, 1)}})}}),
      cps("A"));
  ASSERT_EQ(g.size(), 1u);
  EXPECT_EQ(g[0].glyph, 2u);
}

TEST(ShapePlan, ALookupSharedByTwoFeaturesRunsWhereEitherAppliesIt) {
  // ss01 and ss02 both name lookup 0 (A -> B). ss01 is on for the first code point
  // and ss02 for the second: the lookup's mask is the union of the two, so both are
  // substituted.
  Glyphs g = shape_bytes(small_font({{kGSUB,
          layout_table("latn", -1, {{"ss01", {0}}, {"ss02", {0}}},
              {{1, 0, single_delta({1}, 1)}})}}),
      cps("AA"), "ss01[0:1],ss02[1:2]");
  ASSERT_EQ(g.size(), 2u);
  EXPECT_EQ(g[0].glyph, 2u);
  EXPECT_EQ(g[1].glyph, 2u);
}

TEST(ShapePlan, TheFeatureALanguageSystemRequiresRunsWithoutBeingAsked) {
  Glyphs g = shape_bytes(small_font({{kGSUB,
          layout_table("latn", 0, {{"xxxx", {0}}},
              {{1, 0, single_delta({1}, 1)}})}}),
      cps("A"));
  ASSERT_EQ(g.size(), 1u);
  EXPECT_EQ(g[0].glyph, 2u);
}

TEST(ShapePlan, AScriptTheFontLacksFallsBackToLatinWhenThatIsAllItHas) {
  std::vector<uint8_t> font = small_font({{kGSUB,
      layout_table("latn", -1, {{"liga", {0}}}, {{1, 0, single_delta({1}, 1)}})}});
  // No script asked for, and one the font does not have: both reach latn.
  EXPECT_EQ(shape_bytes(font, cps("A"), "", "")[0].glyph, 2u);
  EXPECT_EQ(shape_bytes(font, cps("A"), "", "cyrl")[0].glyph, 2u);
}

TEST(ShapePlan, AGposThatKernsLeavesTheKernTableAlone) {
  // The GPOS has a kern feature, with no lookups: the font says it has taken over
  // kerning, so the kern table is not applied on top.
  Glyphs g = shape_bytes(small_font({{kGPOS, layout_table("latn", -1,
                                           {{"kern", {}}}, {})},
                                     {GFNT_TAG('k', 'e', 'r', 'n'),
                                      kern_format0({{1, 2, -50}})}}),
      cps("AB"));
  EXPECT_EQ(g[0].x_advance, 500);
}

TEST(ShapeSubstitution, AGlyphNumberThatWrapsPastTheEndOfTheRangeWraps) {
  // Format 1 adds a delta to the glyph modulo 65,536: glyph 1 plus -3 is 65534.
  Glyphs g = shape_bytes(small_font({{kGSUB, layout_table("latn", -1,
                                           {{"liga", {0}}},
                                           {{1, 0, single_delta({1}, -3)}})}}),
      cps("A"));
  ASSERT_EQ(g.size(), 1u);
  EXPECT_EQ(g[0].glyph, 65534u);
}

TEST(ShapeKern, AMarkBetweenTheTwoGlyphsOfAPairIsSkipped) {
  // A, a mark (glyph 6, class mark through GDEF), then B: the pair is A B across it.
  std::vector<uint8_t> gdef;
  gfnttest::put_u16(gdef, 1);
  gfnttest::put_u16(gdef, 0);
  gfnttest::put_u16(gdef, 12);
  gfnttest::put_u16(gdef, 0);
  gfnttest::put_u16(gdef, 0);
  gfnttest::put_u16(gdef, 0);
  gfnttest::put_u16(gdef, 2);
  gfnttest::put_u16(gdef, 1);
  gfnttest::put_u16(gdef, 6);
  gfnttest::put_u16(gdef, 6);
  gfnttest::put_u16(gdef, 3);
  Glyphs g = shape_bytes(small_font({{GFNT_TAG('G', 'D', 'E', 'F'), gdef},
                                     {GFNT_TAG('k', 'e', 'r', 'n'),
                                      kern_format0({{1, 2, -50}})}},
                             8, {{0x301, 6}}),
      V{'A', 0x301, 'B'});
  ASSERT_EQ(g.size(), 3u);
  EXPECT_EQ(g[0].x_advance, 475);
  EXPECT_EQ(g[2].x_offset, -25);
}

TEST(ShapeMarks, AMarkInAFontWithNoGlyphClassesIsAMarkByItsCharacter) {
  // No GDEF at all: which glyphs are marks has to come from the characters, and
  // U+0301 is a combining mark.
  Glyphs g = shape_bytes(small_font({}, 8, {{0x301, 6}}), V{'A', 0x301});
  ASSERT_EQ(g.size(), 2u);
  EXPECT_EQ(g[1].x_advance, 0);
  EXPECT_EQ(g[1].x_offset, -500);
}

// --- HarfBuzz's recorded answers ----------------------------------------------

namespace {

struct GoldenRow {
  std::string font, script, language, features, location, flag;
  std::vector<uint32_t> text;
  Glyphs want;
  std::string line;
};

std::vector<GoldenRow> golden_rows() {
  std::vector<GoldenRow> rows;
  std::ifstream in(gfnttest::data("golden/shape.txt"));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::vector<std::string> f;
    std::stringstream stream(line);
    std::string cell;
    while (std::getline(stream, cell, '\t')) {
      f.push_back(cell);
    }
    if (f.size() < 8) {
      f.resize(8);
    }
    GoldenRow row;
    row.line = line;
    row.font = f[0];
    row.script = f[1];
    row.language = f[2];
    row.features = f[3];
    row.location = f[4];
    row.flag = f[6];
    std::stringstream points(f[5]);
    std::string hex;
    while (points >> hex) {
      row.text.push_back(static_cast<uint32_t>(std::stoul(hex, nullptr, 16)));
    }
    if (f[7] != "-") {
      std::stringstream glyphs(f[7]);
      std::string one;
      while (glyphs >> one) {
        Glyph g{};
        long v[6];
        std::stringstream parts(one);
        std::string number;
        size_t i = 0;
        while (std::getline(parts, number, '/') && i < 6) {
          v[i++] = std::stol(number);
        }
        g = {static_cast<uint32_t>(v[0]), static_cast<uint32_t>(v[1]),
            static_cast<int32_t>(v[2]), static_cast<int32_t>(v[3]),
            static_cast<int32_t>(v[4]), static_cast<int32_t>(v[5])};
        row.want.push_back(g);
      }
    }
    rows.push_back(row);
  }
  return rows;
}

}  // namespace

TEST(ShapeGolden, EveryCaseShapesToWhatHarfBuzzSays) {
  std::vector<GoldenRow> rows = golden_rows();
  ASSERT_GT(rows.size(), 100u) << "the golden file is missing or empty";
  std::map<std::string, std::unique_ptr<Font>> fonts;
  size_t compared = 0;
  for (const GoldenRow & row : rows) {
    if (!fonts.count(row.font)) {
      fonts[row.font].reset(new Font(row.font));
      ASSERT_EQ(fonts[row.font]->result, GFNT_OK) << row.font;
    }
    Request request;
    request.script = row.script;
    request.language = row.language;
    request.features = row.features;
    request.location = row.location;
    Glyphs got;
    GFNT_Error error;
    ASSERT_EQ(shape(*fonts[row.font], row.text, request, &got, &error), GFNT_OK)
        << row.line;
    ++compared;
    EXPECT_EQ(got, row.want) << row.line;
  }
  EXPECT_EQ(compared, rows.size());
}

// --- corrupt layout tables ---------------------------------------------------

namespace {

/** Shape every golden case of a font and report the worst result seen. */
void shape_all(const Font & font, const std::vector<GoldenRow> & rows,
    const std::string & name) {
  for (const GoldenRow & row : rows) {
    if (row.font != name) {
      continue;
    }
    Request request;
    request.script = row.script;
    request.language = row.language;
    request.features = row.features;
    request.location = row.location;
    GFNT_Error error;
    Glyphs got;
    GFNT_Result result = shape(font, row.text, request, &got, &error);
    // A corrupt table may be shaped to anything the bytes happen to say, or
    // refused; it may not be anything else.
    EXPECT_TRUE(result == GFNT_OK || result == GFNT_ERR_CORRUPT
        || result == GFNT_ERR_UNSUPPORTED || result == GFNT_ERR_LIMIT)
        << gfnt_result_string(result) << " on " << row.line;
    if (result == GFNT_ERR_CORRUPT) {
      EXPECT_NE(error.table, 0u) << "a corrupt table is named";
    }
  }
}

}  // namespace

TEST(ShapeCorruption, EveryByteOfTheLayoutTablesOverwrittenInTurn) {
  std::vector<GoldenRow> rows = golden_rows();
  for (const char * name : {"layout-gsub.ttf", "layout-gpos.ttf",
           "variable-featurevars.ttf"}) {
    Font clean(name);
    ASSERT_EQ(clean.result, GFNT_OK);
    for (const char * table : {"GSUB", "GPOS"}) {
      size_t offset = 0;
      size_t length = 0;
      if (gfnt_face_table_range(clean.face, tag4(table), &offset, &length)
          != GFNT_OK) {
        continue;
      }
      for (size_t at = offset; at < offset + length; ++at) {
        for (uint8_t value : {uint8_t(0xFF), uint8_t(0x00), uint8_t(0x80)}) {
          std::vector<uint8_t> bytes = clean.bytes;
          if (bytes[at] == value) {
            continue;
          }
          bytes[at] = value;
          Font mutated(std::move(bytes));
          if (mutated.result != GFNT_OK) {
            continue;
          }
          shape_all(mutated, rows, name);
          if (HasFailure()) {
            FAIL() << "after setting byte " << (at - offset) << " of " << table
                   << " in " << name << " to " << int(value);
          }
        }
      }
    }
  }
}

TEST(ShapeCorruption, ATableShortenedAtEveryLengthIsRefusedOrShaped) {
  std::vector<GoldenRow> rows = golden_rows();
  for (const char * name : {"layout-gsub.ttf", "layout-gpos.ttf"}) {
    Font clean(name);
    ASSERT_EQ(clean.result, GFNT_OK);
    // Overwrite the tail of the file's layout tables with zeros from each point:
    // a table whose offsets point into emptiness.
    for (const char * table : {"GSUB", "GPOS"}) {
      size_t offset = 0;
      size_t length = 0;
      if (gfnt_face_table_range(clean.face, tag4(table), &offset, &length)
          != GFNT_OK) {
        continue;
      }
      for (size_t keep = 0; keep < length; keep += 3) {
        std::vector<uint8_t> bytes = clean.bytes;
        std::memset(bytes.data() + offset + keep, 0, length - keep);
        Font mutated(std::move(bytes));
        if (mutated.result != GFNT_OK) {
          continue;
        }
        shape_all(mutated, rows, name);
        if (HasFailure()) {
          FAIL() << "after zeroing " << table << " from " << keep << " in " << name;
        }
      }
    }
  }
}

// --- allocation failure ------------------------------------------------------

TEST(ShapeAllocation, EveryAllocationRefusedInTurnIsRefusedCleanly) {
  Font font("layout-gsub.ttf");
  ASSERT_EQ(font.result, GFNT_OK);
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"-", "ffi hm"}, {"+ss13", "abc"}, {"+ss12", "aeb"}, {"+ss07", "ppqr"},
      {"salt=2", "aba"}};
  for (const auto & c : cases) {
    Request request;
    request.features = c.first == "-" ? "" : c.first;
    std::vector<uint32_t> text = cps(c.second);
    size_t requests = 0;
    {
      gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
      Glyphs g;
      ASSERT_EQ(shape(font, text, request, &g, nullptr, counting.get()),
          GFNT_OK);
      requests = counting.requests();
      EXPECT_EQ(counting.live(), 0u) << "the run is freed by the caller's free";
    }
    ASSERT_GT(requests, 0u);
    for (size_t fail = 0; fail < requests; ++fail) {
      gfnttest::FailingAllocator failing(fail);
      Glyphs g;
      GFNT_Result result = shape(font, text, request, &g, nullptr,
          failing.get());
      EXPECT_TRUE(result == GFNT_OK || result == GFNT_ERR_OOM)
          << gfnt_result_string(result) << " failing request " << fail;
      EXPECT_EQ(failing.live(), 0u) << "request " << fail << " leaked";
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}


// --- normalisation, space fallback and the order of a reversed run -----------------

namespace {

/**
 * A font with 'A' (glyph 1), 'B' (2), a combining acute U+0301 (6), a combining
 * dot below U+0323 (5), and optionally the precomposed U+00C1 (7) and a space (3).
 */
std::vector<uint8_t> accent_font(bool with_composite, bool with_space = true) {
  std::vector<std::pair<uint32_t, uint16_t>> more = {{0x301, 6}, {0x323, 5}};
  if (with_composite) {
    more.push_back({0xC1, 7});
  }
  if (with_space) {
    more.push_back({0x20, 3});
  }
  return small_font({}, 8, more);
}

}  // namespace

TEST(ShapeNormalise, ABaseAndAMarkBecomeTheCompositeTheFontHas) {
  Font font(accent_font(true));
  Glyphs g = run_of(font, V{'A', 0x301});
  ASSERT_EQ(g.size(), 1u);
  EXPECT_EQ(g[0].glyph, 7u);
  EXPECT_EQ(g[0].cluster, 0u);
}

TEST(ShapeNormalise, ACompositeTheFontLacksIsTakenApart) {
  Font font(accent_font(false));
  Glyphs g = run_of(font, V{0xC1});
  ASSERT_EQ(g.size(), 2u);
  EXPECT_EQ(g[0].glyph, 1u);
  EXPECT_EQ(g[1].glyph, 6u);
  EXPECT_EQ(g[0].cluster, 0u);
  EXPECT_EQ(g[1].cluster, 0u);
}

TEST(ShapeNormalise, ACompositeTheFontHasIsKept) {
  Font font(accent_font(true));
  Glyphs g = run_of(font, V{0xC1});
  ASSERT_EQ(g.size(), 1u);
  EXPECT_EQ(g[0].glyph, 7u);
}

TEST(ShapeNormalise, MarksAreSortedByCombiningClassWhateverOrderTheyCameIn) {
  Font font(accent_font(false));
  // The dot below (class 220) goes before the acute (230) either way round.
  EXPECT_EQ(ids(run_of(font, V{'A', 0x301, 0x323})), (V{1, 5, 6}));
  EXPECT_EQ(ids(run_of(font, V{'A', 0x323, 0x301})), (V{1, 5, 6}));
}

TEST(ShapeNormalise, AMarkThatWillNotComposeKeepsItsPlace) {
  Font font(accent_font(true));
  // The dot below blocks nothing that matters here: A + acute composes past it.
  Glyphs g = run_of(font, V{'A', 0x323, 0x301});
  ASSERT_EQ(g.size(), 2u);
  EXPECT_EQ(g[0].glyph, 7u);
  EXPECT_EQ(g[1].glyph, 5u);
}

TEST(ShapeSpaces, ASpaceTheFontLacksIsSizedAsAFractionOfTheEm) {
  Font font(accent_font(false));
  // U+2003 EM SPACE is the whole em (1000 units), U+2002 EN SPACE half of it, U+2009
  // THIN SPACE a fifth, U+200A HAIR SPACE a sixteenth. All are the font's space glyph.
  Glyphs g = run_of(font, V{0x2003, 0x2002, 0x2009, 0x200A});
  ASSERT_EQ(g.size(), 4u);
  for (const Glyph & x : g) {
    EXPECT_EQ(x.glyph, 3u);
  }
  EXPECT_EQ(g[0].x_advance, 1000);
  EXPECT_EQ(g[1].x_advance, 500);
  EXPECT_EQ(g[2].x_advance, 200);
  EXPECT_EQ(g[3].x_advance, 63);
}

TEST(ShapeSpaces, ANoBreakSpaceTheFontLacksIsTheSpacesOwnWidth) {
  Font font(accent_font(false));
  Glyphs g = run_of(font, V{0xA0});
  ASSERT_EQ(g.size(), 1u);
  EXPECT_EQ(g[0].glyph, 3u);
  EXPECT_EQ(g[0].x_advance, 500);
}

TEST(ShapeDirection, AReversedRunKeepsEachMarkAfterItsBase) {
  Font font(accent_font(false));
  Glyphs g;
  Request request;
  request.rtl = true;
  // Text A, acute, B shaped right to left in a left to right script: the run is
  // turned round a grapheme at a time, so B comes first and the mark still follows
  // the A it belongs to.
  ASSERT_EQ(shape(font, V{'A', 0x301, 'B'}, request, &g), GFNT_OK);
  ASSERT_EQ(g.size(), 3u);
  EXPECT_EQ(ids(g), (V{2, 1, 6}));
  EXPECT_EQ(g[0].cluster, 2u);
  EXPECT_EQ(g[1].cluster, 0u);
  EXPECT_EQ(g[2].cluster, 0u);
}


// --- script shapers ----------------------------------------------------------------

TEST(ShapeScript, TheScriptOfATextIsItsFirstCharacterThatBelongsToOne) {
  const uint32_t latin[] = {'1', ' ', 'a'};
  const uint32_t arabic[] = {'(', 0x627, 'a'};
  const uint32_t none[] = {'1', ' ', 0x301};

  EXPECT_EQ(gfnt_shape_script_of(latin, 3), GFNT_TAG('l', 'a', 't', 'n'));
  EXPECT_EQ(gfnt_shape_script_of(arabic, 3), GFNT_TAG('a', 'r', 'a', 'b'));
  EXPECT_EQ(gfnt_shape_script_of(none, 3), 0u);
  EXPECT_EQ(gfnt_shape_script_direction(GFNT_TAG('a', 'r', 'a', 'b')),
      GFNT_DIRECTION_RTL);
  EXPECT_EQ(gfnt_shape_script_direction(GFNT_TAG('l', 'a', 't', 'n')),
      GFNT_DIRECTION_LTR);
  EXPECT_EQ(gfnt_shape_script_direction(0), GFNT_DIRECTION_LTR);
}

namespace {

/**
 * A font that knows Arabic only as its pre-OpenType self does: BEH (glyph 4) and
 * its four presentation forms, isolated 5, final 6, initial 7, medial 8, and no
 * layout table at all.
 */
std::vector<uint8_t> beh_font() {
  return small_font({}, 10, {{0x628, 4}, {0xFE8F, 5}, {0xFE90, 6}, {0xFE91, 7},
      {0xFE92, 8}});
}

}  // namespace

TEST(ShapeArabic, AFontWithNoLayoutIsJoinedFromItsPresentationForms) {
  Font font(beh_font());
  Glyphs g;
  Request request;
  request.script = "arab";
  request.rtl = true;
  // One letter alone is its isolated form.
  ASSERT_EQ(shape(font, V{0x628}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{5}));
  // Three in a row: initial, medial, final in reading order - which, as the glyphs
  // come back in visual order, is last to first.
  ASSERT_EQ(shape(font, V{0x628, 0x628, 0x628}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{6, 8, 7}));
  EXPECT_EQ(g[0].cluster, 2u);
  EXPECT_EQ(g[2].cluster, 0u);
}

TEST(ShapeArabic, AZeroWidthNonJoinerStopsTheJoin) {
  Font font(beh_font());
  Glyphs g;
  Request request;
  request.script = "arab";
  request.rtl = true;
  // beh, ZWNJ, beh: each is alone, and the joiner draws as nothing (the font has no
  // space either, so it is dropped from the run).
  ASSERT_EQ(shape(font, V{0x628, 0x200C, 0x628}, request, &g), GFNT_OK);
  std::vector<uint32_t> out = ids(g);
  EXPECT_EQ(std::count(out.begin(), out.end(), 5u), 2);
}

TEST(ShapeArabic, ADualJoiningLetterBeforeAJoinerTakesItsInitialForm) {
  Font font(beh_font());
  Glyphs g;
  Request request;
  request.script = "arab";
  request.rtl = true;
  // beh, ZWJ: the joiner joins to its right in reading order, so beh is initial.
  ASSERT_EQ(shape(font, V{0x628, 0x200D}, request, &g), GFNT_OK);
  std::vector<uint32_t> out = ids(g);
  EXPECT_NE(std::find(out.begin(), out.end(), 7u), out.end());
}

TEST(ShapeHebrew, ALetterAndItsPointBecomeThePresentationFormTheFontHas) {
  Font composed(small_font({}, 6, {{0x5D1, 2}, {0x5BC, 3}, {0xFB31, 4}}));
  Font bare(small_font({}, 6, {{0x5D1, 2}, {0x5BC, 3}}));
  Glyphs g;
  Request request;
  request.script = "hebr";
  request.rtl = true;
  ASSERT_EQ(shape(composed, V{0x5D1, 0x5BC}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{4}));
  // A font without the form keeps the two: the point follows its letter in
  // reading order, so it comes first in the visual order the glyphs are returned in.
  ASSERT_EQ(shape(bare, V{0x5D1, 0x5BC}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{3, 2}));
}

TEST(ShapeThai, SaraAmIsSplitAndItsNikhahitMovesBeforeAToneMark) {
  // KO KAI 2, MAI EK 3, NIKHAHIT 4, SARA AA 5.
  Font font(small_font({}, 8, {{0xE01, 2}, {0xE48, 3}, {0xE4D, 4}, {0xE32, 5}}));
  Glyphs g;
  Request request;
  request.script = "thai";
  ASSERT_EQ(shape(font, V{0xE01, 0xE33}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2, 4, 5}));
  // The tone mark that came before the vowel stays between the nikhahit and the
  // SARA AA it was written ahead of; one written after it stays after.
  ASSERT_EQ(shape(font, V{0xE01, 0xE48, 0xE33}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2, 4, 3, 5}));
  ASSERT_EQ(shape(font, V{0xE01, 0xE33, 0xE48}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2, 4, 5, 3}));
}

TEST(ShapeThai, LaoMaiKonGoesAfterTheNikhahitOfASaraAmToo) {
  // KO 2, MAI KON 3, NIGGAHITA 4, SARA AA 5.
  Font font(small_font({}, 8, {{0xE81, 2}, {0xEBB, 3}, {0xECD, 4}, {0xEB2, 5}}));
  Glyphs g;
  Request request;
  request.script = "lao ";
  ASSERT_EQ(shape(font, V{0xE81, 0xEBB, 0xEB3}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2, 4, 3, 5}));
}

namespace {

/**
 * A font that knows a few Javanese characters and nothing of layout: the letter
 * A98F is glyph 2, the vowel sign TALING (written before its letter) 3, the vowel
 * sign TARUNG (written after) 4, the dotted circle 5, the space 6.
 */
std::vector<uint8_t> javanese_font() {
  return small_font({}, 8, {{0xA98F, 2}, {0xA9BA, 3}, {0xA9B4, 4}, {0x25CC, 5},
      {0x20, 6}});
}

}  // namespace

TEST(ShapeUniversal, AMarkWithNoBaseIsGivenTheDottedCircle) {
  Font font(javanese_font());
  Glyphs g;
  Request request;
  request.script = "java";
  ASSERT_EQ(shape(font, V{0xA9B4}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{5, 4}));
  EXPECT_EQ(g[0].cluster, 0u);
  // On its base it needs none.
  ASSERT_EQ(shape(font, V{0xA98F, 0xA9B4}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2, 4}));
}

TEST(ShapeUniversal, AVowelSignWrittenBeforeItsLetterIsMovedAheadOfIt) {
  Font font(javanese_font());
  Glyphs g;
  Request request;
  request.script = "java";
  ASSERT_EQ(shape(font, V{0xA98F, 0xA9BA}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{3, 2}));
}

TEST(ShapeUniversal, AJoinerBetweenALetterAndItsMarkDoesNotBreakTheSyllable) {
  Font font(javanese_font());
  Glyphs g;
  Request request;
  request.script = "java";
  // The joiner is drawn as the space glyph, and no dotted circle appears.
  ASSERT_EQ(shape(font, V{0xA98F, 0x200C, 0xA9B4}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2, 6, 4}));
  // On its own a non-joiner is a broken syllable, as HarfBuzz has it.
  ASSERT_EQ(shape(font, V{0x200C}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{5, 6}));
}

TEST(ShapeUniversal, AMarkAfterASpaceIsLeftWithoutACircle) {
  Font font(javanese_font());
  Glyphs g;
  Request request;
  request.script = "java";
  ASSERT_EQ(shape(font, V{0x20, 0xA9B4}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{6, 4}));
}

namespace {

/**
 * A font that knows a few Devanagari characters and nothing of layout: KA is glyph
 * 2, the vowel sign I (written before its consonant) 3, the vowel sign AA 4, the
 * virama 5, the dotted circle 6.
 */
std::vector<uint8_t> devanagari_font() {
  return small_font({}, 8, {{0x915, 2}, {0x93F, 3}, {0x93E, 4}, {0x94D, 5},
      {0x25CC, 6}});
}

}  // namespace

TEST(ShapeIndic, ALeftVowelSignIsDrawnBeforeItsConsonant) {
  Font font(devanagari_font());
  Glyphs g;
  Request request;
  request.script = "deva";
  ASSERT_EQ(shape(font, V{0x915, 0x93F}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{3, 2}));
  // The right-hand one stays where it is.
  ASSERT_EQ(shape(font, V{0x915, 0x93E}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2, 4}));
}

TEST(ShapeIndic, AVowelSignWithNoConsonantIsGivenTheDottedCircle) {
  Font font(devanagari_font());
  Glyphs g;
  Request request;
  request.script = "deva";
  ASSERT_EQ(shape(font, V{0x93E}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{6, 4}));
  // A left-hand one is moved ahead of the circle it stands on.
  ASSERT_EQ(shape(font, V{0x93F}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{3, 6}));
}

TEST(ShapeIndic, TwoLeftVowelSignsComeBeforeTheirConsonantInTheOppositeOrder) {
  // KA 2, vowel sign E 3, vowel sign EE 4: HarfBuzz puts the one typed second
  // first.
  Font font(small_font({}, 8, {{0xB95, 2}, {0xBC6, 3}, {0xBC7, 4}, {0x25CC, 5}}));
  Glyphs g;
  Request request;
  request.script = "taml";
  ASSERT_EQ(shape(font, V{0xB95, 0xBC6, 0xBC7}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{4, 3, 2}));
}

TEST(ShapeIndic, ANuktaStaysWithTheLeftVowelSignItFollows) {
  // KA 2, vowel sign I 3, nukta 4 (Bengali): the nukta follows its vowel sign
  // to the front, but a nukta that followed the consonant stays behind it.
  Font font(small_font({}, 8, {{0x995, 2}, {0x9BF, 3}, {0x9BC, 4}, {0x25CC, 5}}));
  Glyphs g;
  Request request;
  request.script = "beng";
  ASSERT_EQ(shape(font, V{0x995, 0x9BF, 0x9BC}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{3, 4, 2}));
  ASSERT_EQ(shape(font, V{0x995, 0x9BC, 0x9BF}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{3, 2, 4}));
}

TEST(ShapeKhmer, TheLeftVowelAndTheCoengRaGoToTheFrontOfTheSyllable) {
  // KA 2, the left-hand vowel SRA E 3, the coeng 4, RA 5, the dotted circle 6.
  Font font(small_font({}, 8, {{0x1780, 2}, {0x17C1, 3}, {0x17D2, 4},
      {0x179A, 5}, {0x25CC, 6}}));
  Glyphs g;
  Request request;
  request.script = "khmr";
  ASSERT_EQ(shape(font, V{0x1780, 0x17C1}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{3, 2}));
  ASSERT_EQ(shape(font, V{0x1780, 0x17D2, 0x179A}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{4, 5, 2}));
  // A sign with no base is given the circle.
  ASSERT_EQ(shape(font, V{0x17C1}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{3, 6}));
}

TEST(ShapeMyanmar, TheLeftVowelAndTheMedialRaGoBeforeTheConsonant) {
  // KA 2, vowel sign E 3, medial RA 4, the dotted circle 5.
  Font font(small_font({}, 8, {{0x1000, 2}, {0x1031, 3}, {0x103C, 4},
      {0x25CC, 5}}));
  Glyphs g;
  Request request;
  request.script = "mymr";
  ASSERT_EQ(shape(font, V{0x1000, 0x1031}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{3, 2}));
  ASSERT_EQ(shape(font, V{0x1000, 0x103C, 0x1031}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{3, 4, 2}));
  // A joiner by itself is not a broken syllable.
  ASSERT_EQ(shape(font, V{0x200C}, request, &g), GFNT_OK);
  std::vector<uint32_t> out = ids(g);
  EXPECT_EQ(std::count(out.begin(), out.end(), 5u), 0);
}

TEST(ShapeMyanmar, TwoLeftVowelsComeOutInTheOppositeOrderToTheOneWritten) {
  // VOWEL SIGN E 2, SHAN E 3, the dotted circle 4.
  Font font(small_font({}, 8, {{0x1031, 2}, {0x1084, 3}, {0x25CC, 4}}));
  Glyphs g;
  Request request;
  request.script = "mym2";
  ASSERT_EQ(shape(font, V{0x1031, 0x1084}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{3, 2, 4}));
  ASSERT_EQ(shape(font, V{0x1084, 0x1031}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2, 3, 4}));
}

TEST(ShapeMyanmar, UuStaysTheTwoLettersItWasMadeIntoUnlessAMarkFollows) {
  // LETTER U 2, LETTER UU 3, VOWEL SIGN II 4, KA 5.
  Font font(small_font({}, 8, {{0x1025, 2}, {0x1026, 3}, {0x102E, 4}, {0x1000, 5}}));
  Glyphs g;
  Request request;
  request.script = "mym2";
  ASSERT_EQ(shape(font, V{0x1026}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2, 4}));
  ASSERT_EQ(shape(font, V{0x1026, 0x1000}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2, 4, 5}));
  ASSERT_EQ(shape(font, V{0x1026, 0x102E}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{3, 4}));
}

TEST(ShapeHangul, ATrailingJamoJoinsTheClusterOfASyllableTheFontHasButNotOtherwise) {
  // HIEUH 3, A 4, NIEUN 5, and the syllable HA 2 in one font and not in the other.
  Font with(small_font({}, 8, {{0xD558, 2}, {0x1112, 3}, {0x1161, 4},
      {0x11AB, 5}}));
  Font without(small_font({}, 8, {{0x1112, 3}, {0x1161, 4}, {0x11AB, 5}}));
  Glyphs g;
  Request request;
  request.script = "hang";
  // HarfBuzz: the font has HA but nothing for HA+NIEUN, so HA is taken apart and
  // the jamo that followed is part of the syllable.
  ASSERT_EQ(shape(with, V{0xD558, 0x11AB}, request, &g), GFNT_OK);
  ASSERT_EQ(g.size(), 3u);
  EXPECT_EQ(g[2].cluster, 0u);
  // A font without HA only takes it apart: the NIEUN after it stands alone.
  ASSERT_EQ(shape(without, V{0xD558, 0x11AB}, request, &g), GFNT_OK);
  ASSERT_EQ(g.size(), 3u);
  EXPECT_EQ(ids(g), (V{3, 4, 5}));
  EXPECT_EQ(g[1].cluster, 0u);
  EXPECT_EQ(g[2].cluster, 1u);
}

TEST(ShapeHangul, AJoinerAfterASyllableOfJamoIsInItsCluster) {
  Font font(small_font({}, 8, {{0x1112, 3}, {0x1161, 4}, {0x11AB, 5}, {0x20, 7}}));
  Glyphs g;
  Request request;
  request.script = "hang";
  ASSERT_EQ(shape(font, V{0x1112, 0x1161, 0x11AB, 0x200D}, request, &g), GFNT_OK);
  ASSERT_EQ(g.size(), 4u);
  for (const Glyph & x : g) {
    EXPECT_EQ(x.cluster, 0u);
  }
}

TEST(ShapeHangul, TheFillersAreDrawnWithTheFontsOwnGlyphsNotHidden) {
  // U+115F, U+1160, U+3164 and U+FFA0 are default-ignorable in Unicode and are
  // not so for HarfBuzz: it keeps the glyph the font has for them.
  Font font(small_font({}, 8, {{0x115F, 5}, {0x1160, 6}, {0x3164, 4}, {0xFFA0, 3},
      {0x20, 7}}));
  Glyphs g;
  Request request;
  request.script = "hang";
  ASSERT_EQ(shape(font, V{0x115F, 0x1160, 0x3164, 0xFFA0}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{5, 6, 4, 3}));
}

TEST(ShapeHangul, JamoBecomeTheSyllableTheFontHasAndStayJamoWhenItHasNone) {
  // HAN 2, the jamo HIEUH 3, A 4, NIEUN 5, the tone mark 6.
  Font composed(small_font({}, 8, {{0xD55C, 2}, {0x1112, 3}, {0x1161, 4},
      {0x11AB, 5}, {0x302E, 6}}));
  Font jamo_only(small_font({}, 8, {{0x1112, 3}, {0x1161, 4}, {0x11AB, 5}}));
  Glyphs g;
  Request request;
  request.script = "hang";
  ASSERT_EQ(shape(composed, V{0x1112, 0x1161, 0x11AB}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2}));
  EXPECT_EQ(g[0].cluster, 0u);
  // The syllable and a trailing jamo make the syllable with it.
  ASSERT_EQ(shape(composed, V{0xD558, 0x11AB}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2}));
  // A font with only the jamo keeps them, and a syllable it lacks is taken apart.
  ASSERT_EQ(shape(jamo_only, V{0x1112, 0x1161, 0x11AB}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{3, 4, 5}));
  ASSERT_EQ(shape(jamo_only, V{0xD55C}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{3, 4, 5}));
  // A tone mark with width goes in front of its syllable.
  ASSERT_EQ(shape(composed, V{0xD55C, 0x302E}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{6, 2}));
  // And in a run written right to left, where the tone mark came first, it and
  // the syllable are one cluster, as HarfBuzz has them.
  request.rtl = true;
  ASSERT_EQ(shape(composed, V{0x302F, 0xD55C}, request, &g), GFNT_OK);
  ASSERT_EQ(g.size(), 2u);
  EXPECT_EQ(g[0].cluster, g[1].cluster);
}

TEST(ShapeAllocation, EveryAllocationRefusedInTurnInTheScriptShapersToo) {
  struct Case {
    const char * script;
    std::vector<std::pair<uint32_t, uint16_t>> cmap;
    std::vector<uint32_t> text;
  };
  const std::vector<Case> cases = {
      {"deva", {{0x915, 2}, {0x93F, 3}, {0x94D, 4}, {0x930, 5}, {0x25CC, 6},
          {0x200D, 7}}, {0x930, 0x94D, 0x915, 0x93F, 0x94D, 0x200D, 0x93F, 0x93F}},
      {"java", {{0xA98F, 2}, {0xA9BA, 3}, {0xA9B4, 4}, {0xA9C0, 5}, {0x25CC, 6}},
          {0xA9B4, 0xA98F, 0xA9BA, 0xA9C0, 0xA9C0, 0x200C}},
      {"khmr", {{0x1780, 2}, {0x17C1, 3}, {0x17D2, 4}, {0x179A, 5}, {0x25CC, 6}},
          {0x1780, 0x17D2, 0x179A, 0x17C1, 0x17C1, 0x17D2}},
      {"mymr", {{0x1000, 2}, {0x1031, 3}, {0x103C, 4}, {0x1039, 5}, {0x25CC, 6}},
          {0x1000, 0x103C, 0x1031, 0x1039, 0x1000, 0x103C}},
      {"hang", {{0xD55C, 2}, {0x1112, 3}, {0x1161, 4}, {0x11AB, 5}, {0x302E, 6}},
          {0x1112, 0x1161, 0x11AB, 0xD558, 0x11AB, 0x302E}},
      {"thai", {{0xE01, 2}, {0xE33, 3}, {0xE4D, 4}, {0xE32, 5}, {0xE48, 6}},
          {0xE01, 0xE48, 0xE33, 0xE01, 0xE33}},
  };
  for (const auto & c : cases) {
    Font font(small_font({}, 8, c.cmap));
    Request request;
    size_t requests = 0;
    request.script = c.script;
    {
      gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
      Glyphs g;
      ASSERT_EQ(shape(font, c.text, request, &g, nullptr, counting.get()),
          GFNT_OK) << c.script;
      requests = counting.requests();
      EXPECT_EQ(counting.live(), 0u) << c.script;
    }
    ASSERT_GT(requests, 0u) << c.script;
    for (size_t fail = 0; fail < requests; ++fail) {
      gfnttest::FailingAllocator failing(fail);
      Glyphs g;
      GFNT_Result result = shape(font, c.text, request, &g, nullptr,
          failing.get());
      EXPECT_TRUE(result == GFNT_OK || result == GFNT_ERR_OOM)
          << c.script << ": " << gfnt_result_string(result)
          << " failing request " << fail;
      EXPECT_EQ(failing.live(), 0u) << c.script << ": request " << fail
          << " leaked";
    }
  }
}

// --- vertical text and mirroring ---------------------------------------------------

TEST(ShapeVertical, AdvancesComeFromVmtxAndTheOriginFromVorg) {
  Glyphs g = shape_vertical(small_font(vertical_tables(1, true)), cps("AB"),
      GFNT_DIRECTION_TTB);
  // Down is negative. The pen sits half the glyph's width across and at VORG's
  // height, so the glyph moves left by 250 and down by that height.
  EXPECT_EQ(g, (Glyphs{{1, 0, 0, -700, -250, -880}, {2, 1, 0, -700, -250, -760}}));
}

TEST(ShapeVertical, WithoutVorgTheOriginIsTheAscender) {
  Glyphs g = shape_vertical(small_font(vertical_tables(1, false)), cps("A"),
      GFNT_DIRECTION_TTB);
  EXPECT_EQ(g, (Glyphs{{1, 0, 0, -700, -250, -800}}));
}

TEST(ShapeVertical, WithoutVmtxAnAdvanceIsTheHeightOfTheLine) {
  Glyphs g = shape_vertical(small_font({}), cps("A"), GFNT_DIRECTION_TTB);
  EXPECT_EQ(g[0].y_advance, -1000);
  EXPECT_EQ(g[0].x_advance, 0);
}

TEST(ShapeVertical, BottomToTopComesBackInVisualOrder) {
  Glyphs g = shape_vertical(small_font(vertical_tables(1, true)), cps("AB"),
      GFNT_DIRECTION_BTT);
  EXPECT_EQ(g, (Glyphs{{2, 1, 0, -700, -250, -760}, {1, 0, 0, -700, -250, -880}}));
}

TEST(ShapeVertical, TheVertFeatureReplacesTheHorizontalOnes) {
  Font font("layout-gpos.ttf");
  Request request;
  request.vertical = GFNT_DIRECTION_TTB;
  Glyphs vertical, horizontal;
  ASSERT_EQ(shape(font, cps("AV"), request, &vertical), GFNT_OK);
  ASSERT_EQ(shape(font, cps("AV"), Request{}, &horizontal), GFNT_OK);
  // The font kerns A V; vertical text does not ask for `kern`.
  EXPECT_NE(horizontal[0].x_advance, vertical[0].x_advance);
  EXPECT_EQ(vertical[0].x_advance, 0);
}

TEST(ShapeVertical, ALocationInTheDesignSpaceIsRefused) {
  Font font("variable-featurevars.ttf");
  Request request;
  request.vertical = GFNT_DIRECTION_TTB;
  request.location = "wght=900";
  request.script = "";
  Glyphs g;
  EXPECT_EQ(shape(font, cps("AB"), request, &g), GFNT_ERR_UNSUPPORTED);
}

TEST(ShapeVertical, AVerticalFormStandsInWhereTheFontHasNoVertFeature) {
  Glyphs g = shape_vertical(small_font(vertical_tables(1, false), 8,
                                {{0xFF08, 6}, {0xFE35, 7}}),
      {0xFF08}, GFNT_DIRECTION_TTB);
  EXPECT_EQ(g[0].glyph, 7u);
}

TEST(ShapeMirror, ABracketTurnsRoundInABackwardsRunIfTheFontHasItsMirror) {
  auto font = small_font({}, 8, {{'(', 6}, {')', 7}});
  Font f(font);
  Request request;
  request.rtl = true;
  Glyphs g;
  ASSERT_EQ(shape(f, cps("(A"), request, &g), GFNT_OK);
  EXPECT_EQ(g[0].glyph, 1u);
  EXPECT_EQ(g[1].glyph, 7u);
  ASSERT_EQ(shape(f, cps("(A"), Request{}, &g), GFNT_OK);
  EXPECT_EQ(g[0].glyph, 6u);
}

// --- Apple's morx ------------------------------------------------------------------
//
// Every expected value below is what HarfBuzz 10.2.0 gives for the same bytes,
// from a font built with the same tables: A is glyph 1, B glyph 2, and so on, and
// the glyphs the tables make are 30 and up.

namespace {

using Bytes = std::vector<uint8_t>;

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes & p : parts) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

Bytes words(std::initializer_list<uint32_t> values) {
  Bytes out;
  for (uint32_t v : values) {
    gfnttest::put_u16(out, static_cast<uint16_t>(v));
  }
  return out;
}

Bytes longs(std::initializer_list<uint32_t> values) {
  Bytes out;
  for (uint32_t v : values) {
    gfnttest::put_u32(out, v);
  }
  return out;
}

/** An AAT lookup table, format 6: glyph and value pairs. */
Bytes lookup6(std::vector<std::pair<uint16_t, uint16_t>> pairs) {
  std::sort(pairs.begin(), pairs.end());
  Bytes out = words({6, 4, static_cast<uint32_t>(pairs.size()), 0, 0, 0});
  for (const auto & p : pairs) {
    gfnttest::put_u16(out, p.first);
    gfnttest::put_u16(out, p.second);
  }
  gfnttest::put_u16(out, 0xFFFF);
  gfnttest::put_u16(out, 0);
  return out;
}

Bytes pad4(Bytes b) {
  while (b.size() % 4) {
    b.push_back(0);
  }
  return b;
}

/**
 * A state table: the header, the class lookup (class of glyph g = classes[g-1]
 * for the first glyphs), the states and the entries. @p extra is the subtable's
 * own offsets (4 bytes each), which the caller patches afterwards.
 */
Bytes state_table(uint32_t classes, const std::vector<uint16_t> & class_of,
    const std::vector<std::vector<uint16_t>> & states,
    const std::vector<Bytes> & entries, size_t extra_longs) {
  Bytes lookup = words({8, 1, static_cast<uint32_t>(class_of.size())});
  for (uint16_t c : class_of) {
    gfnttest::put_u16(lookup, c);
  }
  lookup = pad4(lookup);
  Bytes array;
  for (const auto & row : states) {
    for (uint16_t e : row) {
      gfnttest::put_u16(array, e);
    }
  }
  array = pad4(array);
  Bytes table;
  for (const Bytes & e : entries) {
    table.insert(table.end(), e.begin(), e.end());
  }
  size_t header = 16 + 4 * extra_longs;
  size_t at_classes = header;
  size_t at_states = at_classes + lookup.size();
  size_t at_entries = at_states + array.size();
  Bytes out = longs({classes, static_cast<uint32_t>(at_classes),
      static_cast<uint32_t>(at_states), static_cast<uint32_t>(at_entries)});
  for (size_t i = 0; i < extra_longs; ++i) {
    gfnttest::put_u32(out, 0);
  }
  return cat({out, lookup, array, table});
}

void patch32(Bytes & b, size_t at, uint32_t v) {
  b[at] = static_cast<uint8_t>(v >> 24);
  b[at + 1] = static_cast<uint8_t>(v >> 16);
  b[at + 2] = static_cast<uint8_t>(v >> 8);
  b[at + 3] = static_cast<uint8_t>(v);
}

Bytes morx_subtable(uint8_t type, uint8_t coverage, uint32_t flags, Bytes body) {
  Bytes out;
  uint32_t length = static_cast<uint32_t>(12 + body.size());
  length += (4 - length % 4) % 4;
  gfnttest::put_u32(out, length);
  gfnttest::put_u32(out, (static_cast<uint32_t>(coverage) << 24) | type);
  gfnttest::put_u32(out, flags);
  out.insert(out.end(), body.begin(), body.end());
  out.resize(length);
  return out;
}

struct MorxFeature {
  uint16_t type, setting;
  uint32_t enable, disable;
};

Bytes morx_table(uint32_t defaults, const std::vector<MorxFeature> & features,
    const std::vector<Bytes> & subtables) {
  Bytes chain;
  for (const MorxFeature & f : features) {
    gfnttest::put_u16(chain, f.type);
    gfnttest::put_u16(chain, f.setting);
    gfnttest::put_u32(chain, f.enable);
    gfnttest::put_u32(chain, f.disable);
  }
  for (const Bytes & s : subtables) {
    chain.insert(chain.end(), s.begin(), s.end());
  }
  Bytes out = words({2, 0});
  gfnttest::put_u32(out, 1);
  gfnttest::put_u32(out, defaults);
  gfnttest::put_u32(out, static_cast<uint32_t>(16 + chain.size()));
  gfnttest::put_u32(out, static_cast<uint32_t>(features.size()));
  gfnttest::put_u32(out, static_cast<uint32_t>(subtables.size()));
  out.insert(out.end(), chain.begin(), chain.end());
  return out;
}

/** The `feat` table naming the feature types a test uses, sorted, non-exclusive. */
Bytes feat_table(const std::vector<std::pair<uint16_t, std::vector<uint16_t>>> & types) {
  Bytes out;
  gfnttest::put_u32(out, 0x00010000);
  gfnttest::put_u16(out, static_cast<uint16_t>(types.size()));
  gfnttest::put_u16(out, 0);
  gfnttest::put_u32(out, 0);
  Bytes settings;
  size_t base = 12 + 12 * types.size();
  for (const auto & t : types) {
    gfnttest::put_u16(out, t.first);
    gfnttest::put_u16(out, static_cast<uint16_t>(t.second.size()));
    gfnttest::put_u32(out, static_cast<uint32_t>(base + settings.size()));
    gfnttest::put_u16(out, 0);
    gfnttest::put_u16(out, 0);
    for (uint16_t s : t.second) {
      gfnttest::put_u16(settings, s);
      gfnttest::put_u16(settings, 0);
    }
  }
  out.insert(out.end(), settings.begin(), settings.end());
  return out;
}

Bytes morx_font(const Bytes & morx, const Bytes & feat = {}) {
  std::vector<gfnttest::Table> tables = {{GFNT_TAG('m', 'o', 'r', 'x'), morx}};
  if (!feat.empty()) {
    tables.push_back({GFNT_TAG('f', 'e', 'a', 't'), feat});
  }
  return small_font(tables, 40);
}

struct Placed {
  uint32_t glyph, cluster;
  bool operator==(const Placed & o) const {
    return glyph == o.glyph && cluster == o.cluster;
  }
};

std::ostream & operator<<(std::ostream & out, const Placed & p) {
  return out << p.glyph << "/" << p.cluster;
}

std::vector<Placed> morx_run(const Bytes & font_bytes, const std::string & text,
    const std::string & features = "", bool rtl = false,
    const std::string & script = "latn") {
  Font font(font_bytes);
  EXPECT_EQ(font.result, GFNT_OK);
  Request request;
  request.script = script;
  request.features = features;
  request.rtl = rtl;
  Glyphs g;
  EXPECT_EQ(shape(font, cps(text), request, &g), GFNT_OK);
  std::vector<Placed> out;
  for (const Glyph & x : g) {
    out.push_back({x.glyph, x.cluster});
  }
  return out;
}

using P = std::vector<Placed>;

}  // namespace

TEST(ShapeMorx, ANoncontextualSubtableMapsGlyphToGlyph) {
  Bytes sub = morx_subtable(4, 0, 1, lookup6({{1, 30}, {5, 32}}));
  Bytes font = morx_font(morx_table(1, {}, {sub}));
  EXPECT_EQ(morx_run(font, "ABE"), (P{{30, 0}, {2, 1}, {32, 2}}));
}

TEST(ShapeMorx, ALigatureFormsAndItsComponentsAreRemovedWithTheirClusters) {
  // A then B: push A, push B and perform; actions pop B then A, A storing.
  Bytes body = state_table(8, {4, 5}, {{0, 0, 0, 0, 1, 0, 0, 0},
                                       {0, 0, 0, 0, 0, 2, 0, 0}},
      {words({0, 0, 0}), words({1, 0x8000, 0}), words({0, 0xA000, 0})}, 3);
  Bytes actions = longs({0, 0xC0000000u});
  Bytes components = words({0, 0, 1, 2});
  Bytes ligatures = words({0, 30, 0, 31, 0, 0});
  size_t at_actions = body.size();
  size_t at_components = at_actions + actions.size();
  size_t at_ligatures = at_components + components.size();
  patch32(body, 16, static_cast<uint32_t>(at_actions));
  patch32(body, 20, static_cast<uint32_t>(at_components));
  patch32(body, 24, static_cast<uint32_t>(at_ligatures));
  body = cat({body, actions, components, ligatures});
  Bytes font = morx_font(morx_table(1, {}, {morx_subtable(2, 0, 1, body)}));
  EXPECT_EQ(morx_run(font, "XABY"), (P{{24, 0}, {30, 1}, {25, 3}}));
  EXPECT_EQ(morx_run(font, "ABABC"), (P{{30, 0}, {30, 2}, {3, 4}}));
  // A subtable that wants to run against the text runs on the reversed run: for
  // a right-to-left run that is the way the text is read.
  Bytes backwards = morx_subtable(2, 0x40, 1, body);
  Bytes font2 = morx_font(morx_table(1, {}, {backwards}));
  EXPECT_EQ(morx_run(font2, "AB", "", true, "arab"), (P{{30, 0}}));
  EXPECT_EQ(morx_run(font2, "BA", "", true, "arab"), (P{{1, 1}, {2, 0}}));
}

TEST(ShapeMorx, ARearrangementMovesGlyphsAndMergesTheirClusters) {
  // From an A, mark first; at a D, mark last and rearrange: ABxCD -> DCxBA.
  Bytes body = state_table(8, {4, 5, 6, 7}, {{0, 0, 0, 0, 1, 0, 0, 0},
                                             {2, 2, 2, 2, 2, 2, 2, 3}},
      {words({0, 0}), words({1, 0x8000}), words({1, 0}),
          words({0, 0x2000 | 15})}, 0);
  Bytes font = morx_font(morx_table(1, {}, {morx_subtable(0, 0, 1, body)}));
  EXPECT_EQ(morx_run(font, "ABEFCD"),
      (P{{4, 0}, {3, 0}, {5, 0}, {6, 0}, {2, 0}, {1, 0}}));
  EXPECT_EQ(morx_run(font, "AEFD"), (P{{4, 0}, {6, 0}, {5, 0}, {1, 0}}));
}

TEST(ShapeMorx, AnInsertionPutsGlyphsAfterTheCurrentGlyphInItsCluster) {
  // An A inserts two glyphs, 30 and 31, behind itself.
  Bytes body = state_table(6, {4, 5}, {{0, 0, 0, 0, 1, 0}},
      {words({0, 0, 0xFFFF, 0xFFFF}), words({0, 0x0040, 0, 0xFFFF})}, 1);
  patch32(body, 16, static_cast<uint32_t>(body.size()));
  body = cat({body, words({30, 31, 32, 33})});
  Bytes font = morx_font(morx_table(1, {}, {morx_subtable(5, 0, 1, body)}));
  EXPECT_EQ(morx_run(font, "XAY"),
      (P{{24, 0}, {1, 1}, {30, 1}, {31, 1}, {25, 2}}));
}

TEST(ShapeMorx, AContextualSubtableSubstitutesTheMarkedAndTheCurrentGlyph) {
  // An A sets the mark; a B or C after it substitutes the A through lookup 0 and
  // itself through lookup 1.
  Bytes body = state_table(8, {4, 5, 6}, {{0, 0, 0, 0, 1, 0, 0, 0},
                                          {0, 0, 0, 0, 1, 2, 3, 0}},
      {words({0, 0, 0xFFFF, 0xFFFF}), words({1, 0x8000, 0xFFFF, 0xFFFF}),
          words({0, 0, 0, 1}), words({0, 0, 0xFFFF, 1})}, 1);
  Bytes lookup0 = lookup6({{1, 30}, {2, 34}});
  Bytes lookup1 = lookup6({{2, 31}, {3, 32}});
  patch32(body, 16, static_cast<uint32_t>(body.size()));
  body = cat({body, longs({8, static_cast<uint32_t>(8 + lookup0.size())}),
      lookup0, lookup1});
  Bytes font = morx_font(morx_table(1, {}, {morx_subtable(1, 0, 1, body)}));
  EXPECT_EQ(morx_run(font, "AB"), (P{{30, 0}, {31, 1}}));
  EXPECT_EQ(morx_run(font, "XAC"), (P{{24, 0}, {1, 1}, {32, 2}}));
}

TEST(ShapeMorx, FeaturesSelectSubtablesFirstRequestWinsAndRangesAreHonoured) {
  // Subtable 1 (flag 1, on by default) maps A; subtable 2 (flag 2) maps B and is
  // turned on by the common-ligatures selector of the ligature feature.
  std::vector<MorxFeature> features = {{1, 2, 0x2, 0xFFFFFFFF},
      {1, 3, 0, 0xFFFFFFFD}};
  Bytes first = morx_subtable(4, 0, 1, lookup6({{1, 30}}));
  Bytes second = morx_subtable(4, 0, 2, lookup6({{2, 31}}));
  Bytes font = morx_font(morx_table(1, features, {first, second}),
      feat_table({{1, {2, 3}}}));
  EXPECT_EQ(morx_run(font, "AB"), (P{{30, 0}, {2, 1}}));
  EXPECT_EQ(morx_run(font, "AB", "liga"), (P{{30, 0}, {31, 1}}));
  EXPECT_EQ(morx_run(font, "AB", "-liga"), (P{{30, 0}, {2, 1}}));
  // Whichever of two requests for one setting came first is the one that holds.
  EXPECT_EQ(morx_run(font, "AB", "liga,-liga"), (P{{30, 0}, {31, 1}}));
  EXPECT_EQ(morx_run(font, "AB", "-liga,liga"), (P{{30, 0}, {2, 1}}));
  // A range reaches the glyphs whose clusters it holds.
  EXPECT_EQ(morx_run(font, "BB", "liga[1:2]"), (P{{2, 0}, {31, 1}}));
}

TEST(ShapeMorx, FeaturesWithNoOpenTypeTwinMapToTheirSettingsAndSmcpToTwo) {
  // hist is setting 0 of type 40; smcp is setting 1 of type 37 and also setting
  // 3 of type 3. Flag 1 is on by default; the others each turn one on.
  std::vector<MorxFeature> features = {{40, 0, 0x2, 0xFFFFFFFF},
      {37, 1, 0x4, 0xFFFFFFFF}, {3, 3, 0x8, 0xFFFFFFFF}};
  Bytes font = morx_font(morx_table(1, features,
      {morx_subtable(4, 0, 1, lookup6({{1, 30}})),
       morx_subtable(4, 0, 2, lookup6({{2, 31}})),
       morx_subtable(4, 0, 4, lookup6({{3, 32}})),
       morx_subtable(4, 0, 8, lookup6({{4, 33}}))}),
      feat_table({{3, {3}}, {37, {1}}, {40, {0}}}));
  EXPECT_EQ(morx_run(font, "BCD", "hist"), (P{{31, 0}, {3, 1}, {4, 2}}));
  EXPECT_EQ(morx_run(font, "BCD", "smcp"), (P{{2, 0}, {32, 1}, {33, 2}}));
}

TEST(ShapeMorx, ItReplacesTheScriptsOwnShaperAndNoClassIsMadeUpForAMark) {
  // A mark of an Arabic run keeps its advance: no class is made up for it and no
  // script shaper reorders or joins; the font's tables do it all.
  Bytes sub = morx_subtable(4, 0, 1, lookup6({{1, 30}}));
  Bytes font = morx_font(morx_table(1, {}, {sub}));
  EXPECT_EQ(morx_run(font, "A", "", true, "arab"), (P{{30, 0}}));
}

TEST(ShapeMorx, AContextualSubstitutionIsNotMadeAtTheEndOfTheTextWithNoMark) {
  // After an A the machine reaches the end of the text with a substitution of
  // the last glyph in its entry: with no mark ever set, HarfBuzz makes none.
  Bytes font = morx_font(Bytes {
    0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x6C, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x1C,
    0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x48, 0x00, 0x08, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x04, 0x00, 0x06, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x1E, 0xFF, 0xFF, 0x00, 0x00});
  EXPECT_EQ(morx_run(font, "A"), (P{{1, 0}}));
  EXPECT_EQ(morx_run(font, "AAA"), (P{{1, 0}, {1, 1}, {1, 2}}));
}

TEST(ShapeMorx, ALigatureSumIsNotClearedBetweenStores) {
  // Two B's wait on the stack and an A runs a storing action, then the last one.
  // The second store adds its component to the first one's sum: 2 + 2 selects 24.
  Bytes font = morx_font(Bytes {
    0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x8C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x00, 0x00, 0x28,
    0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x52, 0x00, 0x00, 0x00, 0x5A,
    0x00, 0x00, 0x00, 0x66, 0x00, 0x08, 0x00, 0x01, 0x00, 0x02, 0x00, 0x04,
    0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x15, 0x00, 0x16,
    0x00, 0x17, 0x00, 0x18});
  EXPECT_EQ(morx_run(font, "BBA"), (P{{24, 0}, {1, 2}}));
  EXPECT_EQ(morx_run(font, "BA"), (P{{22, 0}, {1, 1}}));
}

// --- Hinting device tables ----------------------------------------------------------

TEST(ShapeDevice, APixelDeltaIsScaledToFontUnitsAndTruncatedTowardZero) {
  // A's advance of 500 plus 10, and a device table for sizes 8 to 13 (8-bit
  // deltas 1 -1 2 -2 3 -3). Every expected advance is what HarfBuzz 10.2.0 gives
  // for the same bytes at that --font-ppem: delta * 1000 / ppem, cut toward zero.
  std::vector<uint8_t> font_bytes = small_font({{GFNT_TAG('G', 'P', 'O', 'S'),
      {0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x6B, 0x65, 0x72, 0x6E,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x0A,
       0x00, 0x44, 0x00, 0x0A, 0x00, 0x10, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x08, 0x00, 0x0D, 0x00, 0x03, 0x01, 0xFF, 0x02, 0xFE, 0x03, 0xFD}}});
  Font font(font_bytes);
  ASSERT_EQ(font.result, GFNT_OK);
  auto advance_at = [&](uint32_t ppem) {
    Request request;
    request.ppem = ppem;
    Glyphs g;
    EXPECT_EQ(shape(font, cps("A"), request, &g), GFNT_OK);
    return g.size() == 1 ? g[0].x_advance : -1;
  };
  EXPECT_EQ(advance_at(0), 510);
  EXPECT_EQ(advance_at(7), 510);
  EXPECT_EQ(advance_at(8), 635);
  EXPECT_EQ(advance_at(9), 399);
  EXPECT_EQ(advance_at(11), 329);
  EXPECT_EQ(advance_at(13), 280);
  EXPECT_EQ(advance_at(14), 510);
}

// --- Ligature carets ----------------------------------------------------------------

TEST(ShapeCarets, AGdefListsWhereTheCursorGoesInsideALigature) {
  // Glyph 5 has three carets: the coordinate 250; a contour point, which this
  // face has no outline to read and so leaves out; and 500 with a hinting device
  // table for sizes 8 and 9 (deltas 1 and -1).
  std::vector<uint8_t> font_bytes = small_font({{GFNT_TAG('G', 'D', 'E', 'F'),
      {0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00,
       0x00, 0x06, 0x00, 0x01, 0x00, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x05,
       0x00, 0x03, 0x00, 0x08, 0x00, 0x0C, 0x00, 0x10, 0x00, 0x01, 0x00, 0xFA,
       0x00, 0x02, 0x00, 0x01, 0x00, 0x03, 0x01, 0xF4, 0x00, 0x06, 0x00, 0x08,
       0x00, 0x09, 0x00, 0x03, 0x01, 0xFF}}});
  Font font(font_bytes);
  ASSERT_EQ(font.result, GFNT_OK);
  int32_t carets[4] = {0, 0, 0, 0};

  EXPECT_EQ(gfnt_face_ligature_carets(font.face, 5, 0, nullptr, 0, nullptr, 0), 2u);
  EXPECT_EQ(gfnt_face_ligature_carets(font.face, 5, 0, nullptr, 0, carets, 4), 2u);
  EXPECT_EQ(carets[0], 250);
  EXPECT_EQ(carets[1], 500);
  EXPECT_EQ(gfnt_face_ligature_carets(font.face, 5, 0, nullptr, 8, carets, 4), 2u);
  EXPECT_EQ(carets[1], 625);
  EXPECT_EQ(gfnt_face_ligature_carets(font.face, 5, 0, nullptr, 9, carets, 4), 2u);
  EXPECT_EQ(carets[1], 389);
  EXPECT_EQ(gfnt_face_ligature_carets(font.face, 6, 0, nullptr, 0, carets, 4), 0u);
  // Counting does not stop at the capacity.
  EXPECT_EQ(gfnt_face_ligature_carets(font.face, 5, 0, nullptr, 0, carets, 1), 2u);
}

// --- BASE ---------------------------------------------------------------------------

TEST(ShapeBase, ABaselineIsReadByScriptAndTagAndScaledByItsDeviceTable) {
  // latn has ideo at -120 and romn at 0 with a hinting device table for sizes 8
  // and 9 (deltas 1 and -1). Nothing here is checked against HarfBuzz, whose
  // command line does not print baselines: the bytes follow the specification.
  std::vector<uint8_t> font_bytes = small_font({{GFNT_TAG('B', 'A', 'S', 'E'),
      {0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x0E,
       0x00, 0x02, 0x69, 0x64, 0x65, 0x6F, 0x72, 0x6F, 0x6D, 0x6E, 0x00, 0x01,
       0x6C, 0x61, 0x74, 0x6E, 0x00, 0x08, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x08, 0x00, 0x0C, 0x00, 0x01, 0xFF, 0x88,
       0x00, 0x03, 0x00, 0x00, 0x00, 0x06, 0x00, 0x08, 0x00, 0x09, 0x00, 0x03,
       0x01, 0xFF}}});
  Font font(font_bytes);
  ASSERT_EQ(font.result, GFNT_OK);
  int32_t value = 99;

  EXPECT_TRUE(gfnt_face_baseline(font.face, GFNT_TAG('i', 'd', 'e', 'o'), 0,
      GFNT_TAG('l', 'a', 't', 'n'), nullptr, 0, &value));
  EXPECT_EQ(value, -120);
  EXPECT_TRUE(gfnt_face_baseline(font.face, GFNT_TAG('r', 'o', 'm', 'n'), 0,
      GFNT_TAG('l', 'a', 't', 'n'), nullptr, 8, &value));
  EXPECT_EQ(value, 125);
  EXPECT_FALSE(gfnt_face_baseline(font.face, GFNT_TAG('h', 'a', 'n', 'g'), 0,
      GFNT_TAG('l', 'a', 't', 'n'), nullptr, 0, &value));
  EXPECT_FALSE(gfnt_face_baseline(font.face, GFNT_TAG('i', 'd', 'e', 'o'), 0,
      GFNT_TAG('g', 'r', 'e', 'k'), nullptr, 0, &value));
  EXPECT_FALSE(gfnt_face_baseline(font.face, GFNT_TAG('i', 'd', 'e', 'o'), 1,
      GFNT_TAG('l', 'a', 't', 'n'), nullptr, 0, &value));
}

// --- a mark and the glyphs of a multiple substitution ---------------------------------

namespace {

/** 'A' (glyph 1) becomes glyphs 2, 3, 4 under ccmp; 'E' (glyph 5) is a mark that a
 *  mark-to-base lookup attaches to the one glyph its base coverage holds. */
Glyphs shape_mark_after_multiple(const std::vector<uint8_t> & gpos) {
  static const std::vector<uint8_t> gsub = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x63, 0x63, 0x6D, 0x70,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x03,
       0x00, 0x02, 0x00, 0x03, 0x00, 0x04};
  static const std::vector<uint8_t> gdef = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x05, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x03};
  Font font(small_font({{GFNT_TAG('G', 'S', 'U', 'B'), gsub},
      {GFNT_TAG('G', 'P', 'O', 'S'), gpos}, {GFNT_TAG('G', 'D', 'E', 'F'), gdef}}));
  Glyphs g;
  Request request;
  EXPECT_EQ(shape(font, V{'A', 'E'}, request, &g), GFNT_OK);
  return g;
}

}  // namespace

TEST(ShapeMark, AMarkAttachesToTheNearestGlyphOfAMultipleSubstitutionTheCoverageHolds) {
  // Every expected offset is what HarfBuzz 10.2.0 gives for the same bytes: the
  // base anchor (100) less the advances of the glyphs between the base and the mark;
  // glyph 3 has no class in the GDEF, which does not make it skippable.
  static const std::vector<uint8_t> first = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x6D, 0x61, 0x72, 0x6B,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x0C,
       0x00, 0x12, 0x00, 0x01, 0x00, 0x18, 0x00, 0x24, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x05, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00,
       0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x64, 0x00, 0x32};
  static const std::vector<uint8_t> middle = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x6D, 0x61, 0x72, 0x6B,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x0C,
       0x00, 0x12, 0x00, 0x01, 0x00, 0x18, 0x00, 0x24, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x05, 0x00, 0x01, 0x00, 0x01, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00,
       0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x64, 0x00, 0x32};
  static const std::vector<uint8_t> last = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x6D, 0x61, 0x72, 0x6B,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x0C,
       0x00, 0x12, 0x00, 0x01, 0x00, 0x18, 0x00, 0x24, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x05, 0x00, 0x01, 0x00, 0x01, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00,
       0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x64, 0x00, 0x32};
  Glyphs g = shape_mark_after_multiple(first);
  ASSERT_EQ(g.size(), 4u);
  EXPECT_EQ(g[3].x_offset, 100 - 1500);
  g = shape_mark_after_multiple(middle);
  ASSERT_EQ(g.size(), 4u);
  EXPECT_EQ(g[3].x_offset, 100 - 1000);
  g = shape_mark_after_multiple(last);
  ASSERT_EQ(g.size(), 4u);
  EXPECT_EQ(g[3].x_offset, 100 - 500);
}

// --- Indic, Khmer and Myanmar feature plans -----------------------------------------
//
// Each font here has one lookup under one feature in its script's tables, and every
// expected value is what HarfBuzz 10.2.0 gives for the same bytes.

namespace {

std::vector<uint32_t> run_in(const std::vector<uint8_t> & table_bytes,
    const std::vector<std::pair<uint32_t, uint16_t>> & cmap, const char * script,
    const std::vector<uint32_t> & text, const std::string & features = "") {
  Font font(small_font({{GFNT_TAG('G', 'S', 'U', 'B'), table_bytes}}, 40, cmap));
  EXPECT_EQ(font.result, GFNT_OK);
  Request request;
  request.script = script;
  request.features = features;
  Glyphs g;
  EXPECT_EQ(shape(font, text, request, &g), GFNT_OK);
  return ids(g);
}

}  // namespace

static const std::vector<uint8_t> k_liga = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x64, 0x65, 0x76, 0x32, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x6C, 0x69, 0x67, 0x61,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x02, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x05, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01};

static const std::vector<uint8_t> k_ligakhmr = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x6B, 0x68, 0x6D, 0x72, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x6C, 0x69, 0x67, 0x61,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x02, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x05, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01};

static const std::vector<uint8_t> k_ccmpdev = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x64, 0x65, 0x76, 0x32, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x63, 0x63, 0x6D, 0x70,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x04, 0x00, 0x05, 0x00, 0x02, 0x00, 0x02};

static const std::vector<uint8_t> k_ccmpkhmr = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x6B, 0x68, 0x6D, 0x72, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x63, 0x63, 0x6D, 0x70,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x04, 0x00, 0x05, 0x00, 0x02, 0x00, 0x02};

static const std::vector<uint8_t> k_ccmpmym = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x6D, 0x79, 0x6D, 0x32, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x63, 0x63, 0x6D, 0x70,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x04, 0x00, 0x05, 0x00, 0x02, 0x00, 0x02};

static const std::vector<uint8_t> k_ccmplatn = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x6C, 0x61, 0x74, 0x6E, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x63, 0x63, 0x6D, 0x70,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x04, 0x00, 0x05, 0x00, 0x02, 0x00, 0x02};

static const std::vector<uint8_t> k_half = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x64, 0x65, 0x76, 0x32, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x68, 0x61, 0x6C, 0x66,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x02, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x18, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0E};

static const std::vector<uint8_t> k_abvf = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x64, 0x65, 0x76, 0x32, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x61, 0x62, 0x76, 0x66,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x02, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x18, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0E};

TEST(ShapeIndic, LigaIsOffForTheIndicAndKhmerShapersAndACallerCannotTurnItOn) {
  const std::vector<std::pair<uint32_t, uint16_t>> dev = {{0x915, 1}, {0x916, 2}};
  const std::vector<std::pair<uint32_t, uint16_t>> khmer = {{0x1780, 1}, {0x1781, 2}};

  EXPECT_EQ(run_in(k_liga, dev, "dev2", {0x915, 0x916}), (V{1, 2}));
  EXPECT_EQ(run_in(k_liga, dev, "dev2", {0x915, 0x916}, "+liga"), (V{1, 2}));
  EXPECT_EQ(run_in(k_ligakhmr, khmer, "khmr", {0x1780, 0x1781}), (V{1, 2}));
  EXPECT_EQ(run_in(k_ligakhmr, khmer, "khmr", {0x1780, 0x1781}, "+liga"), (V{1, 2}));
}

TEST(ShapeIndic, LoclAndCcmpDoNotLigateAcrossASyllable) {
  // Two consonants are two syllables in each of these scripts; the same lookup
  // ligates them in the default shaper.
  EXPECT_EQ(run_in(k_ccmpdev, {{0x915, 1}, {0x916, 2}}, "dev2", {0x915, 0x916}), (V{1, 2}));
  EXPECT_EQ(run_in(k_ccmpkhmr, {{0x1780, 1}, {0x1781, 2}}, "khmr", {0x1780, 0x1781}), (V{1, 2}));
  EXPECT_EQ(run_in(k_ccmpmym, {{0x1000, 1}, {0x1001, 2}}, "mym2", {0x1000, 0x1001}), (V{1, 2}));
  EXPECT_EQ(run_in(k_ccmplatn, {{'A', 1}, {'B', 2}}, "latn", {'A', 'B'}), (V{5}));
}

TEST(ShapeIndic, ALeftHandMatraGetsThePreBaseFeaturesNotThePostBaseOnes) {
  // KA then the I matra: the matra is after the base in the text and goes in front
  // of it, and takes `half` and not `abvf`.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {{0x915, 11}, {0x93F, 14}};

  EXPECT_EQ(run_in(k_half, cmap, "dev2", {0x915, 0x93F}), (V{24, 11}));
  EXPECT_EQ(run_in(k_abvf, cmap, "dev2", {0x915, 0x93F}), (V{14, 11}));
}

static const std::vector<uint8_t> k_blwf = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x64, 0x65, 0x76, 0x32, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x62, 0x6C, 0x77, 0x66,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x02, 0x00, 0x0E,
       0x00, 0x04, 0x00, 0x15, 0x00, 0x14, 0x00, 0x16, 0x00, 0x19, 0x00, 0x01,
       0x00, 0x04, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0E};

TEST(ShapeIndic, BlwfReachesEveryGlyphBeforeTheBaseAndALeftHandMatraToo) {
  // KA 11, RA 9, the halant 10 and the I matra 14, which the lookup sends to 22, 21,
  // 20 and 25. Every expected value is what HarfBuzz 10.2.0 gives for the same bytes.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0x915, 11}, {0x930, 9}, {0x94D, 10}, {0x93F, 14}};

  EXPECT_EQ(run_in(k_blwf, cmap, "dev2", {0x930, 0x94D, 0x915}), (V{21, 20, 11}));
  EXPECT_EQ(run_in(k_blwf, cmap, "dev2", {0x915, 0x94D, 0x915}), (V{22, 20, 11}));
  EXPECT_EQ(run_in(k_blwf, cmap, "dev2", {0x915, 0x94D, 0x930, 0x94D, 0x915}),
      (V{22, 20, 21, 20, 11}));
  // The matra, with a consonant or without, and the halant before one.
  EXPECT_EQ(run_in(k_blwf, cmap, "dev2", {0x915, 0x93F}), (V{25, 11}));
  EXPECT_EQ(run_in(k_blwf, cmap, "dev2", {0x93F}), (V{25}));
  EXPECT_EQ(run_in(k_blwf, cmap, "dev2", {0x915, 0x94D, 0x93F}), (V{11, 20, 25}));
}

TEST(ShapeIndic, AnOldSpecFontGetsBlwfOnlyOnARaAndTheHalantsBesideOneBeforeTheBase) {
  // The same lookup under the script tag 'deva' alone, which makes the font old-spec.
  std::vector<uint8_t> old_font = k_blwf;
  const uint8_t dev2[] = {0x64, 0x65, 0x76, 0x32};
  auto at = std::search(old_font.begin(), old_font.end(), dev2, dev2 + 4);
  ASSERT_NE(at, old_font.end());
  *(at + 3) = 0x61;
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0x915, 11}, {0x930, 9}, {0x94D, 10}, {0x93F, 14}};

  EXPECT_EQ(run_in(old_font, cmap, "deva", {0x930, 0x94D, 0x915}), (V{21, 20, 11}));
  EXPECT_EQ(run_in(old_font, cmap, "deva", {0x915, 0x94D, 0x915}), (V{11, 10, 11}));
  EXPECT_EQ(run_in(old_font, cmap, "deva", {0x915, 0x94D, 0x930, 0x94D, 0x915}),
      (V{11, 10, 21, 20, 11}));
  // A left-hand matra does not get it, with a consonant or without.
  EXPECT_EQ(run_in(old_font, cmap, "deva", {0x915, 0x93F}), (V{14, 11}));
  EXPECT_EQ(run_in(old_font, cmap, "deva", {0x93F}), (V{14}));
  EXPECT_EQ(run_in(old_font, cmap, "deva", {0x915, 0x94D, 0x93F}), (V{11, 20, 14}));
}

static const std::vector<uint8_t> k_wouldchain3 = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x64, 0x65, 0x76, 0x61, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x70, 0x72, 0x65, 0x66,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x03, 0x00, 0x00,
       0x00, 0x02, 0x00, 0x0E, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x04, 0x00, 0x01, 0x00, 0x04, 0x00, 0x08, 0x00, 0x0A, 0x00, 0x01,
       0x00, 0x04, 0x00, 0x05, 0x00, 0x0B, 0x00, 0x0D, 0x00, 0x0E};

static const std::vector<uint8_t> k_wouldctx2 = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x64, 0x65, 0x76, 0x61, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x70, 0x72, 0x65, 0x66,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x02, 0x00, 0x0E,
       0x00, 0x1A, 0x00, 0x03, 0x00, 0x3A, 0x00, 0x4C, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x04, 0x00, 0x04, 0x00, 0x05, 0x00, 0x07, 0x00, 0x08, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x0D, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x02, 0x00, 0x06, 0x00, 0x0C,
       0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x04, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02};

TEST(ShapeIndic, AWouldSubstituteTestOfAContextLookupNeedsTheFirstGlyphInItsCoverage) {
  // Old-spec Devanagari asks the font whether `pref` would act on a halant and a
  // consonant; with the answer wrong, a halant that should stay moves behind the Ra.
  // Both fonts have one lookup under `pref` that does nothing to these strings: a
  // chained format 3 whose input coverages do not hold the halant, and a plain
  // format 2 likewise. Every expected value is what HarfBuzz 10.2.0 gives.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0x915, 11}, {0x930, 9}, {0x94D, 10}};

  EXPECT_EQ(run_in(k_wouldchain3, cmap, "deva", {0x915, 0x94D, 0x930, 0x915}),
      (V{11, 10, 9, 11}));
  EXPECT_EQ(run_in(k_wouldctx2, cmap, "deva", {0x915, 0x94D, 0x930, 0x915}),
      (V{11, 10, 9, 11}));
}

static const std::vector<uint8_t> k_blwfdigest = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0xEC, 0x01, 0xB8, 0x00, 0x04,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x1A, 0x64, 0x65, 0x76, 0x32, 0x00, 0x4C,
       0x64, 0x65, 0x76, 0x61, 0x00, 0x7E, 0x6C, 0x61, 0x74, 0x6E, 0x00, 0xB0,
       0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x14, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06,
       0x00, 0x07, 0x00, 0x08, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C,
       0x00, 0x0D, 0x00, 0x0E, 0x00, 0x0F, 0x00, 0x10, 0x00, 0x11, 0x00, 0x12,
       0x00, 0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x14,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05,
       0x00, 0x06, 0x00, 0x07, 0x00, 0x08, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0B,
       0x00, 0x0C, 0x00, 0x0D, 0x00, 0x0E, 0x00, 0x0F, 0x00, 0x10, 0x00, 0x11,
       0x00, 0x12, 0x00, 0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
       0x00, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04,
       0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00, 0x08, 0x00, 0x09, 0x00, 0x0A,
       0x00, 0x0B, 0x00, 0x0C, 0x00, 0x0D, 0x00, 0x0E, 0x00, 0x0F, 0x00, 0x10,
       0x00, 0x11, 0x00, 0x12, 0x00, 0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03,
       0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00, 0x08, 0x00, 0x09,
       0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C, 0x00, 0x0D, 0x00, 0x0E, 0x00, 0x0F,
       0x00, 0x10, 0x00, 0x11, 0x00, 0x12, 0x00, 0x13, 0x00, 0x14, 0x61, 0x62,
       0x76, 0x73, 0x00, 0x7A, 0x61, 0x6B, 0x68, 0x6E, 0x00, 0x7E, 0x62, 0x6C,
       0x77, 0x66, 0x00, 0x82, 0x62, 0x6C, 0x77, 0x73, 0x00, 0x88, 0x63, 0x61,
       0x6C, 0x74, 0x00, 0x8C, 0x63, 0x63, 0x6D, 0x70, 0x00, 0x90, 0x63, 0x6A,
       0x63, 0x74, 0x00, 0x94, 0x63, 0x6C, 0x69, 0x67, 0x00, 0x98, 0x68, 0x61,
       0x6C, 0x66, 0x00, 0x9C, 0x68, 0x61, 0x6C, 0x6E, 0x00, 0xA0, 0x6C, 0x69,
       0x67, 0x61, 0x00, 0xA4, 0x6C, 0x6F, 0x63, 0x6C, 0x00, 0xA8, 0x6E, 0x75,
       0x6B, 0x74, 0x00, 0xAC, 0x70, 0x72, 0x65, 0x66, 0x00, 0xB0, 0x70, 0x72,
       0x65, 0x73, 0x00, 0xB4, 0x70, 0x73, 0x74, 0x66, 0x00, 0xB8, 0x70, 0x73,
       0x74, 0x73, 0x00, 0xBC, 0x72, 0x6C, 0x69, 0x67, 0x00, 0xC0, 0x72, 0x70,
       0x68, 0x66, 0x00, 0xC4, 0x76, 0x61, 0x74, 0x75, 0x00, 0xC8, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x0E,
       0x00, 0x2A, 0x00, 0x44, 0x00, 0x60, 0x00, 0xAA, 0x00, 0xD0, 0x00, 0x03,
       0x00, 0x04, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01,
       0x00, 0x0E, 0x00, 0x01, 0x00, 0x01, 0x00, 0x09, 0x00, 0x02, 0x00, 0x02,
       0x00, 0x08, 0x00, 0x02, 0x00, 0x08, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01,
       0x00, 0x08, 0x00, 0x01, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0D,
       0x00, 0x01, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x04, 0x00, 0x02, 0x00, 0x03, 0x00, 0x09, 0x00, 0x05, 0x00, 0x00,
       0x00, 0x02, 0x00, 0x0A, 0x00, 0x2E, 0x00, 0x03, 0x00, 0x02, 0x00, 0x02,
       0x00, 0x12, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x0C, 0x00, 0x0D, 0x00, 0x01, 0x00, 0x03,
       0x00, 0x03, 0x00, 0x0A, 0x00, 0x0B, 0x00, 0x03, 0x00, 0x02, 0x00, 0x01,
       0x00, 0x0E, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x0A, 0x00, 0x01, 0x00, 0x02, 0x00, 0x04, 0x00, 0x08, 0x00, 0x08,
       0x00, 0x08, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x0E, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x14, 0x00, 0x01, 0x00, 0x0B, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x01, 0x00, 0x03, 0x00, 0x01, 0x00, 0x05, 0x00, 0x0B,
       0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x02, 0x00, 0x12,
       0x00, 0x1C, 0x00, 0x3E, 0x00, 0x60, 0x00, 0x03, 0x00, 0x00, 0x00, 0x80,
       0x00, 0xA0, 0x00, 0x01, 0x00, 0x03, 0x00, 0x08, 0x00, 0x09, 0x00, 0x0C,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02,
       0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x0E, 0x00, 0x02, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02,
       0x00, 0x0D, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02,
       0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x02, 0x00, 0x06, 0x00, 0x0E, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x03,
       0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x06, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};

TEST(ShapeIndic, AWouldSubstituteTestChecksTheFirstGlyphAgainstTheWholeLookupNotEachSubtable) {
  // `blwf` holds one lookup of two context subtables. The glyph pair halant, KA
  // passes the lookup's coverage through the second subtable's halant, and the first
  // subtable then matches it on its second glyph alone, so KA counts as taking a
  // below-base form and the matra goes in front of the Ra. With only the first
  // subtable the lookup would not cover the halant and nothing changes. The
  // expected values are what HarfBuzz 10.2.0 gives for these bytes.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0x915, 11}, {0x930, 9}, {0x93F, 14}, {0x94D, 10}};

  EXPECT_EQ(run_in(k_blwfdigest, cmap, "dev2", {0x94D, 0x930, 0x94D, 0x915, 0x93F}),
      (V{10, 14, 9, 10, 11}));
}

static const std::vector<uint8_t> k_sharedcalt_psts = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0xEC, 0x01, 0xBA, 0x00, 0x04,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x1A, 0x64, 0x65, 0x76, 0x32, 0x00, 0x4C,
       0x64, 0x65, 0x76, 0x61, 0x00, 0x7E, 0x6C, 0x61, 0x74, 0x6E, 0x00, 0xB0,
       0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x14, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06,
       0x00, 0x07, 0x00, 0x08, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C,
       0x00, 0x0D, 0x00, 0x0E, 0x00, 0x0F, 0x00, 0x10, 0x00, 0x11, 0x00, 0x12,
       0x00, 0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x14,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05,
       0x00, 0x06, 0x00, 0x07, 0x00, 0x08, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0B,
       0x00, 0x0C, 0x00, 0x0D, 0x00, 0x0E, 0x00, 0x0F, 0x00, 0x10, 0x00, 0x11,
       0x00, 0x12, 0x00, 0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
       0x00, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04,
       0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00, 0x08, 0x00, 0x09, 0x00, 0x0A,
       0x00, 0x0B, 0x00, 0x0C, 0x00, 0x0D, 0x00, 0x0E, 0x00, 0x0F, 0x00, 0x10,
       0x00, 0x11, 0x00, 0x12, 0x00, 0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03,
       0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00, 0x08, 0x00, 0x09,
       0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C, 0x00, 0x0D, 0x00, 0x0E, 0x00, 0x0F,
       0x00, 0x10, 0x00, 0x11, 0x00, 0x12, 0x00, 0x13, 0x00, 0x14, 0x61, 0x62,
       0x76, 0x73, 0x00, 0x7A, 0x61, 0x6B, 0x68, 0x6E, 0x00, 0x7E, 0x62, 0x6C,
       0x77, 0x66, 0x00, 0x82, 0x62, 0x6C, 0x77, 0x73, 0x00, 0x86, 0x63, 0x61,
       0x6C, 0x74, 0x00, 0x8A, 0x63, 0x63, 0x6D, 0x70, 0x00, 0x90, 0x63, 0x6A,
       0x63, 0x74, 0x00, 0x94, 0x63, 0x6C, 0x69, 0x67, 0x00, 0x98, 0x68, 0x61,
       0x6C, 0x66, 0x00, 0x9C, 0x68, 0x61, 0x6C, 0x6E, 0x00, 0xA0, 0x6C, 0x69,
       0x67, 0x61, 0x00, 0xA4, 0x6C, 0x6F, 0x63, 0x6C, 0x00, 0xA8, 0x6E, 0x75,
       0x6B, 0x74, 0x00, 0xAC, 0x70, 0x72, 0x65, 0x66, 0x00, 0xB0, 0x70, 0x72,
       0x65, 0x73, 0x00, 0xB4, 0x70, 0x73, 0x74, 0x66, 0x00, 0xB8, 0x70, 0x73,
       0x74, 0x73, 0x00, 0xBC, 0x72, 0x6C, 0x69, 0x67, 0x00, 0xC2, 0x72, 0x70,
       0x68, 0x66, 0x00, 0xC6, 0x76, 0x61, 0x74, 0x75, 0x00, 0xCA, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,
       0x00, 0x10, 0x00, 0x42, 0x00, 0x6E, 0x00, 0x82, 0x00, 0xEE, 0x01, 0xDE,
       0x02, 0x18, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x0A, 0x00, 0x18,
       0x00, 0x02, 0x00, 0x08, 0x00, 0x01, 0x00, 0x0D, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x0D, 0x00, 0x02, 0x00, 0x0E, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0A,
       0x00, 0x08, 0x00, 0x03, 0x00, 0x01, 0x00, 0x04, 0x00, 0x05, 0x00, 0x08,
       0x00, 0x0A, 0x00, 0x0D, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x0A, 0x00, 0x02, 0x00, 0x12, 0x00, 0x1A, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x04, 0x00, 0x0C, 0x00, 0x01, 0x00, 0x04, 0x00, 0x05,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x04, 0x00, 0x05, 0x00, 0x02, 0x00, 0x09,
       0x00, 0x01, 0x00, 0x04, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x06,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0D, 0x00, 0x04, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x0C, 0x00, 0x03, 0x00, 0x16,
       0x00, 0x34, 0x00, 0x48, 0x00, 0x01, 0x00, 0x03, 0x00, 0x02, 0x00, 0x08,
       0x00, 0x0D, 0x00, 0x03, 0x00, 0x08, 0x00, 0x10, 0x00, 0x1A, 0x00, 0x09,
       0x00, 0x03, 0x00, 0x09, 0x00, 0x06, 0x00, 0x08, 0x00, 0x04, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x07, 0x00, 0x07, 0x00, 0x01, 0x00, 0x02, 0x00, 0x06,
       0x00, 0x0A, 0x00, 0x04, 0x00, 0x01, 0x00, 0x04, 0x00, 0x04, 0x00, 0x03,
       0x00, 0x0E, 0x00, 0x0C, 0x00, 0x03, 0x00, 0x08, 0x00, 0x0C, 0x00, 0x14,
       0x00, 0x05, 0x00, 0x01, 0x00, 0x05, 0x00, 0x03, 0x00, 0x02, 0x00, 0x0C,
       0x00, 0x0A, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x04,
       0x00, 0x02, 0x00, 0x0A, 0x00, 0xAE, 0x00, 0x02, 0x00, 0x12, 0x00, 0x1C,
       0x00, 0x3C, 0x00, 0x5E, 0x00, 0x03, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x90,
       0x00, 0x01, 0x00, 0x03, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0E, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x0D, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0E,
       0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02, 0x00, 0x0D, 0x00, 0x02,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02,
       0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x04, 0x00, 0x02,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x1A, 0x00, 0x24,
       0x00, 0x02, 0x00, 0x30, 0x00, 0x3A, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03, 0x00, 0x02, 0x00, 0x03,
       0x00, 0x0C, 0x00, 0x01, 0x00, 0x04, 0x00, 0x06, 0x00, 0x07, 0x00, 0x09,
       0x00, 0x0A, 0x00, 0x01, 0x00, 0x03, 0x00, 0x05, 0x00, 0x0C, 0x00, 0x0E,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x06, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x0E,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x09, 0x00, 0x02, 0x00, 0x06, 0x00, 0x12,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x0A, 0x00, 0x0D, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0B, 0x00, 0x02, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x04, 0x00, 0x01,
       0x00, 0x08, 0x00, 0x03, 0x00, 0x03, 0x00, 0x01, 0x00, 0x10, 0x00, 0x1C,
       0x00, 0x26, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x04, 0x00, 0x04,
       0x00, 0x06, 0x00, 0x0B, 0x00, 0x0C, 0x00, 0x01, 0x00, 0x03, 0x00, 0x06,
       0x00, 0x08, 0x00, 0x0B, 0x00, 0x01, 0x00, 0x04, 0x00, 0x06, 0x00, 0x0B,
       0x00, 0x0C, 0x00, 0x0E};

static const std::vector<uint8_t> k_sharedcalt_blws = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0xEC, 0x01, 0xBA, 0x00, 0x04,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x1A, 0x64, 0x65, 0x76, 0x32, 0x00, 0x4C,
       0x64, 0x65, 0x76, 0x61, 0x00, 0x7E, 0x6C, 0x61, 0x74, 0x6E, 0x00, 0xB0,
       0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x14, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06,
       0x00, 0x07, 0x00, 0x08, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C,
       0x00, 0x0D, 0x00, 0x0E, 0x00, 0x0F, 0x00, 0x10, 0x00, 0x11, 0x00, 0x12,
       0x00, 0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x14,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05,
       0x00, 0x06, 0x00, 0x07, 0x00, 0x08, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0B,
       0x00, 0x0C, 0x00, 0x0D, 0x00, 0x0E, 0x00, 0x0F, 0x00, 0x10, 0x00, 0x11,
       0x00, 0x12, 0x00, 0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
       0x00, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04,
       0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00, 0x08, 0x00, 0x09, 0x00, 0x0A,
       0x00, 0x0B, 0x00, 0x0C, 0x00, 0x0D, 0x00, 0x0E, 0x00, 0x0F, 0x00, 0x10,
       0x00, 0x11, 0x00, 0x12, 0x00, 0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03,
       0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00, 0x08, 0x00, 0x09,
       0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C, 0x00, 0x0D, 0x00, 0x0E, 0x00, 0x0F,
       0x00, 0x10, 0x00, 0x11, 0x00, 0x12, 0x00, 0x13, 0x00, 0x14, 0x61, 0x62,
       0x76, 0x73, 0x00, 0x7A, 0x61, 0x6B, 0x68, 0x6E, 0x00, 0x7E, 0x62, 0x6C,
       0x77, 0x66, 0x00, 0x82, 0x62, 0x6C, 0x77, 0x73, 0x00, 0x86, 0x63, 0x61,
       0x6C, 0x74, 0x00, 0x8C, 0x63, 0x63, 0x6D, 0x70, 0x00, 0x92, 0x63, 0x6A,
       0x63, 0x74, 0x00, 0x96, 0x63, 0x6C, 0x69, 0x67, 0x00, 0x9A, 0x68, 0x61,
       0x6C, 0x66, 0x00, 0x9E, 0x68, 0x61, 0x6C, 0x6E, 0x00, 0xA2, 0x6C, 0x69,
       0x67, 0x61, 0x00, 0xA6, 0x6C, 0x6F, 0x63, 0x6C, 0x00, 0xAA, 0x6E, 0x75,
       0x6B, 0x74, 0x00, 0xAE, 0x70, 0x72, 0x65, 0x66, 0x00, 0xB2, 0x70, 0x72,
       0x65, 0x73, 0x00, 0xB6, 0x70, 0x73, 0x74, 0x66, 0x00, 0xBA, 0x70, 0x73,
       0x74, 0x73, 0x00, 0xBE, 0x72, 0x6C, 0x69, 0x67, 0x00, 0xC2, 0x72, 0x70,
       0x68, 0x66, 0x00, 0xC6, 0x76, 0x61, 0x74, 0x75, 0x00, 0xCA, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,
       0x00, 0x10, 0x00, 0x42, 0x00, 0x6E, 0x00, 0x82, 0x00, 0xEE, 0x01, 0xDE,
       0x02, 0x18, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x0A, 0x00, 0x18,
       0x00, 0x02, 0x00, 0x08, 0x00, 0x01, 0x00, 0x0D, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x0D, 0x00, 0x02, 0x00, 0x0E, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0A,
       0x00, 0x08, 0x00, 0x03, 0x00, 0x01, 0x00, 0x04, 0x00, 0x05, 0x00, 0x08,
       0x00, 0x0A, 0x00, 0x0D, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x0A, 0x00, 0x02, 0x00, 0x12, 0x00, 0x1A, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x04, 0x00, 0x0C, 0x00, 0x01, 0x00, 0x04, 0x00, 0x05,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x04, 0x00, 0x05, 0x00, 0x02, 0x00, 0x09,
       0x00, 0x01, 0x00, 0x04, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x06,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0D, 0x00, 0x04, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x0C, 0x00, 0x03, 0x00, 0x16,
       0x00, 0x34, 0x00, 0x48, 0x00, 0x01, 0x00, 0x03, 0x00, 0x02, 0x00, 0x08,
       0x00, 0x0D, 0x00, 0x03, 0x00, 0x08, 0x00, 0x10, 0x00, 0x1A, 0x00, 0x09,
       0x00, 0x03, 0x00, 0x09, 0x00, 0x06, 0x00, 0x08, 0x00, 0x04, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x07, 0x00, 0x07, 0x00, 0x01, 0x00, 0x02, 0x00, 0x06,
       0x00, 0x0A, 0x00, 0x04, 0x00, 0x01, 0x00, 0x04, 0x00, 0x04, 0x00, 0x03,
       0x00, 0x0E, 0x00, 0x0C, 0x00, 0x03, 0x00, 0x08, 0x00, 0x0C, 0x00, 0x14,
       0x00, 0x05, 0x00, 0x01, 0x00, 0x05, 0x00, 0x03, 0x00, 0x02, 0x00, 0x0C,
       0x00, 0x0A, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x04,
       0x00, 0x02, 0x00, 0x0A, 0x00, 0xAE, 0x00, 0x02, 0x00, 0x12, 0x00, 0x1C,
       0x00, 0x3C, 0x00, 0x5E, 0x00, 0x03, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x90,
       0x00, 0x01, 0x00, 0x03, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0E, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x0D, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0E,
       0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02, 0x00, 0x0D, 0x00, 0x02,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02,
       0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x04, 0x00, 0x02,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x1A, 0x00, 0x24,
       0x00, 0x02, 0x00, 0x30, 0x00, 0x3A, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03, 0x00, 0x02, 0x00, 0x03,
       0x00, 0x0C, 0x00, 0x01, 0x00, 0x04, 0x00, 0x06, 0x00, 0x07, 0x00, 0x09,
       0x00, 0x0A, 0x00, 0x01, 0x00, 0x03, 0x00, 0x05, 0x00, 0x0C, 0x00, 0x0E,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x06, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x0E,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x09, 0x00, 0x02, 0x00, 0x06, 0x00, 0x12,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x0A, 0x00, 0x0D, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0B, 0x00, 0x02, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x04, 0x00, 0x01,
       0x00, 0x08, 0x00, 0x03, 0x00, 0x03, 0x00, 0x01, 0x00, 0x10, 0x00, 0x1C,
       0x00, 0x26, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x04, 0x00, 0x04,
       0x00, 0x06, 0x00, 0x0B, 0x00, 0x0C, 0x00, 0x01, 0x00, 0x03, 0x00, 0x06,
       0x00, 0x08, 0x00, 0x0B, 0x00, 0x01, 0x00, 0x04, 0x00, 0x06, 0x00, 0x0B,
       0x00, 0x0C, 0x00, 0x0E};

TEST(ShapeIndic, ASharedLookupTakesItsPerSyllableSettingFromTheFirstFeatureByTag) {
  // One ligature lookup under `calt` and under another feature of the last stage.
  // The features are taken in tag order and the merged lookup keeps the first one's
  // setting: `calt` runs it across the whole text, so KHA and RA join when `psts`
  // shares it; `blws` comes first and its setting wins, and the ligature does not
  // form. Every expected value is HarfBuzz 10.2.0's.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0x916, 12}, {0x930, 9}, {0x93E, 13}};

  EXPECT_EQ(run_in(k_sharedcalt_psts, cmap, "dev2", {0x916, 0x930, 0x93E}),
      (V{5, 13}));
  EXPECT_EQ(run_in(k_sharedcalt_blws, cmap, "dev2", {0x916, 0x930, 0x93E}),
      (V{12, 9, 13}));
}

TEST(ShapeIndic, AnOldSpecSyllableMergesTheClustersAfterTheBaseWhereTheSortLeftIt) {
  // KA, halant, KHA, I matra under 'deva': the matra moves in front of KHA, and the
  // clusters merged are those of the base the sort left, not of where it began.
  // (HarfBuzz 10.2.0: KA 0, halant 0, I matra 2, KHA 2.)
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0x915, 11}, {0x94D, 10}, {0x916, 12}, {0x93F, 14}};
  Font font(small_font({{GFNT_TAG('G', 'S', 'U', 'B'), k_wouldctx2}}, 40, cmap));
  Glyphs g;
  Request request;

  ASSERT_EQ(font.result, GFNT_OK);
  request.script = "deva";
  ASSERT_EQ(shape(font, V{0x915, 0x94D, 0x916, 0x93F}, request, &g), GFNT_OK);
  ASSERT_EQ(g.size(), 4u);
  EXPECT_EQ(ids(g), (V{11, 10, 14, 12}));
  EXPECT_EQ(g[0].cluster, 0u);
  EXPECT_EQ(g[1].cluster, 0u);
  EXPECT_EQ(g[2].cluster, 2u);
  EXPECT_EQ(g[3].cluster, 2u);
}

static const std::vector<uint8_t> k_vatulig = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x64, 0x65, 0x76, 0x61, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x76, 0x61, 0x74, 0x75,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x01,
       0x00, 0x04, 0x00, 0x1B, 0x00, 0x02, 0x00, 0x0B};

TEST(ShapeIndic, AnOldSpecFontPlacesAConsonantBehindAHalantAVatuLigatureCouldFormWith) {
  // One ligature, halant + KA to glyph 27, under `vatu` in an old-spec font. It
  // makes the second KA a post-base consonant, so the halant moves behind it and
  // nothing ligates (HarfBuzz 10.2.0 gives KA, KA, halant); a new-spec font forms it.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {{0x915, 11}, {0x94D, 10}};

  EXPECT_EQ(run_in(k_vatulig, cmap, "deva", {0x915, 0x94D, 0x915}), (V{11, 11, 10}));
}

static const std::vector<uint8_t> k_rligpsts = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x20, 0x00, 0x3A, 0x00, 0x01,
       0x64, 0x65, 0x76, 0x32, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x70, 0x73,
       0x74, 0x73, 0x00, 0x0E, 0x72, 0x6C, 0x69, 0x67, 0x00, 0x14, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
       0x00, 0x06, 0x00, 0x1C, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08,
       0x00, 0x02, 0x00, 0x08, 0x00, 0x01, 0x00, 0x1C, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x0B, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x02,
       0x00, 0x08, 0x00, 0x01, 0x00, 0x1D, 0x00, 0x01, 0x00, 0x01, 0x00, 0x1C};

TEST(ShapeIndic, TheCommonFeaturesAreInTheStageOfTheShapersLastFeatures) {
  // `rlig` (glyph 11 to 28, lookup 0) and `psts` (28 to 29, lookup 1): in one stage
  // they run in the order of the lookups, so a Ka comes out as 29. HarfBuzz 10.2.0
  // gives {29}; with the common features a stage behind, it was 28.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {{0x915, 11}};

  EXPECT_EQ(run_in(k_rligpsts, cmap, "dev2", {0x915}), (V{29}));
}

static const std::vector<uint8_t> k_presmym2 = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x6D, 0x79, 0x6D, 0x32, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x70, 0x72, 0x65, 0x73,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x02, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x1C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0B};

static const std::vector<uint8_t> k_presmymr = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x1E, 0x00, 0x2C, 0x00, 0x01,
       0x6D, 0x79, 0x6D, 0x72, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x70, 0x72, 0x65, 0x73,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x02, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x1C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0B};

TEST(ShapeMyanmar, OnlyAFontWithTheNewScriptTagIsShapedAsMyanmar) {
  // A lookup under `pres`, which the Myanmar shaper asks for and the default one
  // does not: a 'mym2' font applies it, a font with the old 'mymr' alone does not.
  // (HarfBuzz 10.2.0.)
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {{0x1000, 11}};
  Font fresh(small_font({{GFNT_TAG('G', 'S', 'U', 'B'), k_presmym2}}, 40, cmap));
  Font old(small_font({{GFNT_TAG('G', 'S', 'U', 'B'), k_presmymr}}, 40, cmap));
  Glyphs g;
  Request request;

  request.script = "mym2";
  ASSERT_EQ(shape(fresh, V{0x1000}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{28}));
  request.script = "mymr";
  ASSERT_EQ(shape(old, V{0x1000}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{11}));
}

static const std::vector<uint8_t> k_mymprefsyl = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x90, 0x01, 0x20, 0x00, 0x03,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x14, 0x6C, 0x61, 0x74, 0x6E, 0x00, 0x3A,
       0x6D, 0x79, 0x6D, 0x32, 0x00, 0x60, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03,
       0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00, 0x08, 0x00, 0x09,
       0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C, 0x00, 0x0D, 0x00, 0x04, 0x00, 0x00,
       0x00, 0x00, 0xFF, 0xFF, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02,
       0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00, 0x08,
       0x00, 0x09, 0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C, 0x00, 0x0D, 0x00, 0x04,
       0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x07,
       0x00, 0x08, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C, 0x00, 0x0D,
       0x00, 0x0E, 0x61, 0x62, 0x76, 0x73, 0x00, 0x56, 0x62, 0x6C, 0x77, 0x66,
       0x00, 0x5A, 0x62, 0x6C, 0x77, 0x73, 0x00, 0x5E, 0x63, 0x61, 0x6C, 0x74,
       0x00, 0x62, 0x63, 0x63, 0x6D, 0x70, 0x00, 0x66, 0x63, 0x6C, 0x69, 0x67,
       0x00, 0x6A, 0x6C, 0x69, 0x67, 0x61, 0x00, 0x6E, 0x6C, 0x6F, 0x63, 0x6C,
       0x00, 0x72, 0x70, 0x72, 0x65, 0x66, 0x00, 0x76, 0x70, 0x72, 0x65, 0x73,
       0x00, 0x7C, 0x70, 0x73, 0x74, 0x66, 0x00, 0x80, 0x70, 0x73, 0x74, 0x73,
       0x00, 0x84, 0x72, 0x6C, 0x69, 0x67, 0x00, 0x88, 0x72, 0x70, 0x68, 0x66,
       0x00, 0x8C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x06, 0x00, 0x0E, 0x00, 0xC6, 0x01, 0x92, 0x01, 0xBC, 0x01, 0xD6,
       0x02, 0x46, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x0A, 0x00, 0x64,
       0x00, 0x01, 0x00, 0x0C, 0x00, 0x03, 0x00, 0x16, 0x00, 0x30, 0x00, 0x48,
       0x00, 0x01, 0x00, 0x03, 0x00, 0x07, 0x00, 0x09, 0x00, 0x0D, 0x00, 0x03,
       0x00, 0x08, 0x00, 0x0E, 0x00, 0x12, 0x00, 0x06, 0x00, 0x02, 0x00, 0x02,
       0x00, 0x06, 0x00, 0x01, 0x00, 0x01, 0x00, 0x03, 0x00, 0x0A, 0x00, 0x04,
       0x00, 0x03, 0x00, 0x08, 0x00, 0x0E, 0x00, 0x14, 0x00, 0x0A, 0x00, 0x02,
       0x00, 0x01, 0x00, 0x0B, 0x00, 0x02, 0x00, 0x07, 0x00, 0x03, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x06, 0x00, 0x0C, 0x00, 0x02, 0x00, 0x02, 0x00, 0x03,
       0x00, 0x04, 0x00, 0x02, 0x00, 0x09, 0x00, 0x01, 0x00, 0x0C, 0x00, 0x03,
       0x00, 0x16, 0x00, 0x2E, 0x00, 0x3C, 0x00, 0x01, 0x00, 0x03, 0x00, 0x03,
       0x00, 0x07, 0x00, 0x0C, 0x00, 0x03, 0x00, 0x08, 0x00, 0x0C, 0x00, 0x12,
       0x00, 0x0D, 0x00, 0x01, 0x00, 0x0B, 0x00, 0x02, 0x00, 0x09, 0x00, 0x03,
       0x00, 0x02, 0x00, 0x04, 0x00, 0x01, 0x00, 0x04, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x0C, 0x00, 0x0B, 0x00, 0x06, 0x00, 0x03, 0x00, 0x08, 0x00, 0x0C,
       0x00, 0x14, 0x00, 0x0A, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x03, 0x00, 0x01,
       0x00, 0x0E, 0x00, 0x09, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02,
       0x00, 0x0A, 0x00, 0x64, 0x00, 0x01, 0x00, 0x0E, 0x00, 0x04, 0x00, 0x1A,
       0x00, 0x30, 0x00, 0x3C, 0x00, 0x44, 0x00, 0x01, 0x00, 0x04, 0x00, 0x06,
       0x00, 0x08, 0x00, 0x0B, 0x00, 0x0D, 0x00, 0x02, 0x00, 0x06, 0x00, 0x0C,
       0x00, 0x08, 0x00, 0x02, 0x00, 0x09, 0x00, 0x04, 0x00, 0x04, 0x00, 0x0B,
       0x00, 0x01, 0x00, 0x0B, 0x00, 0x01, 0x00, 0x04, 0x00, 0x08, 0x00, 0x03,
       0x00, 0x0D, 0x00, 0x0C, 0x00, 0x01, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x06, 0x00, 0x0C, 0x00, 0x05, 0x00, 0x02, 0x00, 0x0A,
       0x00, 0x0A, 0x00, 0x04, 0x00, 0x01, 0x00, 0x03, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x0C, 0x00, 0x03, 0x00, 0x16, 0x00, 0x30, 0x00, 0x52, 0x00, 0x01,
       0x00, 0x03, 0x00, 0x03, 0x00, 0x09, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x08,
       0x00, 0x0C, 0x00, 0x14, 0x00, 0x01, 0x00, 0x01, 0x00, 0x07, 0x00, 0x03,
       0x00, 0x0A, 0x00, 0x0D, 0x00, 0x01, 0x00, 0x02, 0x00, 0x0C, 0x00, 0x03,
       0x00, 0x08, 0x00, 0x12, 0x00, 0x18, 0x00, 0x05, 0x00, 0x04, 0x00, 0x0B,
       0x00, 0x0C, 0x00, 0x09, 0x00, 0x0B, 0x00, 0x02, 0x00, 0x01, 0x00, 0x0A,
       0x00, 0x04, 0x00, 0x03, 0x00, 0x03, 0x00, 0x0B, 0x00, 0x03, 0x00, 0x08,
       0x00, 0x0E, 0x00, 0x12, 0x00, 0x0B, 0x00, 0x02, 0x00, 0x09, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x0B, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x08, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x02, 0x00, 0x12, 0x00, 0x1A,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x05, 0x00, 0x03, 0x00, 0x03,
       0x00, 0x0D, 0x00, 0x0A, 0x00, 0x03, 0x00, 0x0D, 0x00, 0x0C, 0x00, 0x02,
       0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x08, 0x00, 0x02, 0x00, 0x0A,
       0x00, 0x02, 0x00, 0x06, 0x00, 0x03, 0x00, 0x01, 0x00, 0x02, 0x00, 0x04,
       0x00, 0x0A, 0x00, 0x06, 0x00, 0x08, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01,
       0x00, 0x0C, 0x00, 0x03, 0x00, 0x16, 0x00, 0x28, 0x00, 0x3A, 0x00, 0x01,
       0x00, 0x03, 0x00, 0x02, 0x00, 0x07, 0x00, 0x0C, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x0B, 0x00, 0x02,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x03, 0x00, 0x07,
       0x00, 0x09, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x02, 0x00, 0x06,
       0x00, 0x1C, 0x00, 0x01, 0x00, 0x0D, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01,
       0x00, 0x0D, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x0D, 0x00, 0x02, 0x00, 0x0A, 0x00, 0x01, 0x00, 0x02,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x08, 0x00, 0x02,
       0x00, 0x0A, 0x00, 0xAE, 0x00, 0x02, 0x00, 0x12, 0x00, 0x1A, 0x00, 0x3A,
       0x00, 0x5C, 0x00, 0x03, 0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x01, 0x00, 0x02, 0x00, 0x0D,
       0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0E, 0x00, 0x02, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02,
       0x00, 0x02, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x0D, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x06,
       0x00, 0x1E, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x02,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x02, 0x00, 0x12, 0x00, 0x1A, 0x00, 0x3C, 0x00, 0x5A, 0x00, 0x03,
       0x00, 0x7C, 0x00, 0x94, 0x00, 0xA4, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x0C, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02,
       0x00, 0x02, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0E,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x02, 0x00, 0x02, 0x00, 0x01, 0x00, 0x04, 0x00, 0x02, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x03,
       0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01,
       0x00, 0x00, 0x00, 0x01};

TEST(ShapeMyanmar, TheBasicFeaturesStayInOneSyllableAndAMarkTakesNoWidth) {
  // A ligature lookup under `pref` over a vowel sign E and a consonant, with a virama
  // in front and a sign after: the E belongs to the syllable of the virama, so it does
  // not join the consonant that starts the next one, and the single substitution of
  // the consonant still applies. The virama, a mark with no GDEF to say otherwise, has
  // no advance. Every expected value is what HarfBuzz 10.2.0 gives for these bytes.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0x1000, 9}, {0x1001, 10}, {0x1039, 11}, {0x103B, 12}, {0x1031, 13},
      {0x1036, 14}};
  Font font(small_font({{GFNT_TAG('G', 'S', 'U', 'B'), k_mymprefsyl}}, 40, cmap));
  Glyphs g;
  Request request;

  ASSERT_EQ(font.result, GFNT_OK);
  request.script = "mym2";
  ASSERT_EQ(shape(font, V{0x1039, 0x1031, 0x1000, 0x1036}, request, &g), GFNT_OK);
  ASSERT_EQ(g.size(), 4u);
  EXPECT_EQ(ids(g), (V{11, 13, 3, 14}));
  EXPECT_EQ(g[0].cluster, 0u);
  EXPECT_EQ(g[1].cluster, 0u);
  EXPECT_EQ(g[2].cluster, 2u);
  EXPECT_EQ(g[3].cluster, 2u);
  EXPECT_EQ(g[0].x_advance, 0);
}

static const std::vector<uint8_t> k_altshared = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x48, 0x00, 0x62, 0x00, 0x03,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x14, 0x6C, 0x61, 0x74, 0x6E, 0x00, 0x22,
       0x6C, 0x61, 0x74, 0x6E, 0x00, 0x30, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00,
       0x00, 0x00, 0xFF, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x02, 0x63, 0x32, 0x73, 0x63, 0x00, 0x0E, 0x73, 0x6D, 0x63, 0x70,
       0x00, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x08, 0x00, 0x01, 0x00, 0x0E, 0x00, 0x04, 0x00, 0x1A, 0x00, 0x22,
       0x00, 0x2A, 0x00, 0x32, 0x00, 0x01, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02,
       0x00, 0x03, 0x00, 0x04, 0x00, 0x03, 0x00, 0x64, 0x00, 0x6E, 0x00, 0x78,
       0x00, 0x03, 0x00, 0x65, 0x00, 0x6F, 0x00, 0x79, 0x00, 0x03, 0x00, 0x66,
       0x00, 0x70, 0x00, 0x7A, 0x00, 0x03, 0x00, 0x67, 0x00, 0x71, 0x00, 0x7B};

TEST(Shape, TwoFeaturesSharingAnAlternateLookupAreCountedFromTheFirstMaskBit) {
  // One alternate lookup under `c2sc` and `smcp`, each over part of the text. The
  // lookup's mask holds both features' bits and the alternate is the glyph's bits
  // above the lowest, so a glyph in `smcp` alone takes the second alternate and one
  // in both the third. That counts from `c2sc` only if the features the font lacks
  // (`frac`, `numr`, `dnom` and the rest) were given no bit between the two.
  // Every expected value is what HarfBuzz 10.2.0 gives for these bytes.
  Font font(small_font({{GFNT_TAG('G', 'S', 'U', 'B'), k_altshared}}, 300,
      {{'A', 1}, {'B', 2}, {'C', 3}, {'D', 4}}));
  Glyphs g;
  Request request;

  ASSERT_EQ(font.result, GFNT_OK);
  request.features = "smcp[0:2],c2sc[1:3]";
  ASSERT_EQ(shape(font, V{'A', 'B', 'C', 'D'}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{110, 121, 102, 4}));
}

static const std::vector<uint8_t> k_khmerperlig = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x96, 0x01, 0x34, 0x00, 0x03,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x14, 0x6B, 0x68, 0x6D, 0x72, 0x00, 0x3C,
       0x6C, 0x61, 0x74, 0x6E, 0x00, 0x64, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03,
       0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00, 0x08, 0x00, 0x09,
       0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C, 0x00, 0x0D, 0x00, 0x0E, 0x00, 0x04,
       0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x07,
       0x00, 0x08, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C, 0x00, 0x0D,
       0x00, 0x0E, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x0F,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05,
       0x00, 0x06, 0x00, 0x07, 0x00, 0x08, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0B,
       0x00, 0x0C, 0x00, 0x0D, 0x00, 0x0E, 0x00, 0x0F, 0x61, 0x62, 0x76, 0x66,
       0x00, 0x5C, 0x61, 0x62, 0x76, 0x73, 0x00, 0x60, 0x62, 0x6C, 0x77, 0x66,
       0x00, 0x64, 0x62, 0x6C, 0x77, 0x73, 0x00, 0x68, 0x63, 0x61, 0x6C, 0x74,
       0x00, 0x6C, 0x63, 0x63, 0x6D, 0x70, 0x00, 0x70, 0x63, 0x66, 0x61, 0x72,
       0x00, 0x76, 0x63, 0x6C, 0x69, 0x67, 0x00, 0x7C, 0x6C, 0x69, 0x67, 0x61,
       0x00, 0x80, 0x6C, 0x6F, 0x63, 0x6C, 0x00, 0x84, 0x70, 0x72, 0x65, 0x66,
       0x00, 0x8A, 0x70, 0x72, 0x65, 0x73, 0x00, 0x8E, 0x70, 0x73, 0x74, 0x66,
       0x00, 0x92, 0x70, 0x73, 0x74, 0x73, 0x00, 0x96, 0x72, 0x6C, 0x69, 0x67,
       0x00, 0x9A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x0E,
       0x00, 0x26, 0x00, 0x5C, 0x00, 0x70, 0x00, 0xB8, 0x01, 0x06, 0x00, 0x01,
       0x00, 0x08, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x06, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x03, 0x00, 0x08, 0x00, 0x0A, 0x00, 0x0B, 0x00, 0x01,
       0x00, 0x04, 0x00, 0x02, 0x00, 0x0A, 0x00, 0x1C, 0x00, 0x02, 0x00, 0x0A,
       0x00, 0x02, 0x00, 0x0E, 0x00, 0x09, 0x00, 0x01, 0x00, 0x02, 0x00, 0x04,
       0x00, 0x0C, 0x00, 0x02, 0x00, 0x0E, 0x00, 0x04, 0x00, 0x03, 0x00, 0x0E,
       0x00, 0x09, 0x00, 0x07, 0x00, 0x01, 0x00, 0x04, 0x00, 0x03, 0x00, 0x07,
       0x00, 0x0B, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x06, 0x00, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0x05,
       0x00, 0x08, 0x00, 0x04, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x16,
       0x00, 0x02, 0x00, 0x20, 0x00, 0x2C, 0x00, 0x01, 0x00, 0x34, 0x00, 0x03,
       0x00, 0x07, 0x00, 0x09, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x03, 0x00, 0x07,
       0x00, 0x0C, 0x00, 0x0D, 0x00, 0x01, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02,
       0x00, 0x03, 0x00, 0x06, 0x00, 0x01, 0x00, 0x02, 0x00, 0x04, 0x00, 0x08,
       0x00, 0x01, 0x00, 0x04, 0x00, 0x05, 0x00, 0x0B, 0x00, 0x0D, 0x00, 0x0E,
       0x00, 0x04, 0x00, 0x08, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x0C,
       0x00, 0x03, 0x00, 0x16, 0x00, 0x30, 0x00, 0x3C, 0x00, 0x01, 0x00, 0x03,
       0x00, 0x07, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x03, 0x00, 0x08, 0x00, 0x0E,
       0x00, 0x12, 0x00, 0x0B, 0x00, 0x02, 0x00, 0x06, 0x00, 0x0E, 0x00, 0x01,
       0x00, 0x0C, 0x00, 0x03, 0x00, 0x0A, 0x00, 0x0B, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x0A, 0x00, 0x03, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x0E, 0x00, 0x02, 0x00, 0x0B, 0x00, 0x06, 0x00, 0x00, 0x00, 0x02,
       0x00, 0x0A, 0x00, 0x38, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x0E,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x08, 0x00, 0x02, 0x00, 0x06, 0x00, 0x14,
       0x00, 0x00, 0x00, 0x02, 0x00, 0x0D, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x07, 0x00, 0x02, 0x00, 0x0E, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x10, 0x00, 0x16, 0x00, 0x01,
       0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x07,
       0x00, 0x01, 0x00, 0x02, 0x00, 0x05, 0x00, 0x09, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x05};

TEST(ShapeKhmer, TheBasicFeaturesNeverJoinGlyphsOfTwoSyllables) {
  // A ligature lookup under `locl` and `cfar` over a vowel sign AA, KA and KHA: the
  // sign is a syllable of its own, so the lookup cannot take it with the KA after
  // it. Every expected value is what HarfBuzz 10.2.0 gives for these bytes.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0x1780, 9}, {0x1781, 10}, {0x17D2, 11}, {0x17B6, 12}, {0x17C1, 13},
      {0x17C9, 14}};
  Font font(small_font({{GFNT_TAG('G', 'S', 'U', 'B'), k_khmerperlig}}, 40, cmap));
  Glyphs g;
  Request request;

  ASSERT_EQ(font.result, GFNT_OK);
  request.script = "khmr";
  ASSERT_EQ(shape(font, V{0x17B6, 0x1780, 0x1781, 0x1781, 0x1781, 0x17C9}, request,
                &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{9, 9, 10, 10, 10, 7}));
}

static const std::vector<uint8_t> k_altglobal = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x48, 0x00, 0x62, 0x00, 0x03,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x14, 0x6C, 0x61, 0x74, 0x6E, 0x00, 0x22,
       0x6C, 0x61, 0x74, 0x6E, 0x00, 0x30, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00,
       0x00, 0x00, 0xFF, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x02, 0x6C, 0x69, 0x67, 0x61, 0x00, 0x0E, 0x73, 0x6D, 0x63, 0x70,
       0x00, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01,
       0x00, 0x08, 0x00, 0x01, 0x00, 0x0E, 0x00, 0x04, 0x00, 0x1A, 0x00, 0x22,
       0x00, 0x2A, 0x00, 0x32, 0x00, 0x01, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02,
       0x00, 0x03, 0x00, 0x04, 0x00, 0x03, 0x00, 0x64, 0x00, 0x6E, 0x00, 0x78,
       0x00, 0x03, 0x00, 0x65, 0x00, 0x6F, 0x00, 0x79, 0x00, 0x03, 0x00, 0x66,
       0x00, 0x70, 0x00, 0x7A, 0x00, 0x03, 0x00, 0x67, 0x00, 0x71, 0x00, 0x7B};

TEST(Shape, AnAlternateLookupAGlobalFeatureSharesWithAnotherTakesNoAlternate) {
  // One alternate lookup under `liga`, which is on for the whole text, and `smcp`.
  // With `smcp` on for the whole text the two read the one shared bit and the first
  // alternate is taken. With it on for part of the text, HarfBuzz reads the shared
  // bit above the others as an index no set reaches, so nothing is taken anywhere.
  // Every expected value is what HarfBuzz 10.2.0 gives for these bytes.
  Font font(small_font({{GFNT_TAG('G', 'S', 'U', 'B'), k_altglobal}}, 300,
      {{'A', 1}, {'B', 2}, {'C', 3}, {'D', 4}}));
  Glyphs g;
  Request request;

  ASSERT_EQ(font.result, GFNT_OK);
  request.features = "smcp";
  ASSERT_EQ(shape(font, V{'A', 'B', 'C', 'D'}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{100, 101, 102, 103}));
  request.features = "smcp[0:2]";
  ASSERT_EQ(shape(font, V{'A', 'B', 'C', 'D'}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{1, 2, 3, 4}));
}

TEST(ShapeKhmer, TwoRegisterShiftersStraightAfterTheBaseAreOneSyllable) {
  // KA, MUUSIKATOAN twice: HarfBuzz 10.2.0 puts no dotted circle between them. After
  // a subscript's consonant the second one still starts a syllable of its own.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0x1780, 9}, {0x1781, 10}, {0x17D2, 11}, {0x17C9, 14}, {0x25CC, 20}};
  Font font(small_font({}, 40, cmap));
  Glyphs g;
  Request request;

  ASSERT_EQ(font.result, GFNT_OK);
  request.script = "khmr";
  ASSERT_EQ(shape(font, V{0x1780, 0x17C9, 0x17C9}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{9, 14, 14}));
  ASSERT_EQ(shape(font, V{0x1780, 0x17C9, 0x17C9, 0x17C9}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{9, 14, 14, 20, 14}));
  ASSERT_EQ(shape(font, V{0x1780, 0x17D2, 0x1781, 0x17C9, 0x17C9}, request, &g),
      GFNT_OK);
  EXPECT_EQ(ids(g), (V{9, 11, 10, 14, 20, 14}));
}

static const std::vector<uint8_t> k_sinhrphf = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x42, 0x00, 0x50, 0x00, 0x03,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x14, 0x6C, 0x61, 0x74, 0x6E, 0x00, 0x20,
       0x73, 0x69, 0x6E, 0x68, 0x00, 0x2C, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x72, 0x70, 0x68, 0x66,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x02, 0x00, 0x12,
       0x00, 0x06, 0x00, 0x14, 0x00, 0x15, 0x00, 0x16, 0x00, 0x17, 0x00, 0x18,
       0x00, 0x19, 0x00, 0x01, 0x00, 0x06, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0B,
       0x00, 0x0C, 0x00, 0x0D, 0x00, 0x0E};

TEST(ShapeUse, ASinhalaAlLakunaIsAHalantToTheRephReorder) {
  // A single substitution under `rphf` marks the first glyph of a syllable as a
  // reph, which then moves to just before the first post-base glyph. The al-lakuna
  // counts as one, so a reph straight in front of it stays where it is.
  // Every expected value is what HarfBuzz 10.2.0 gives for these bytes.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0xD9A, 9}, {0xD9B, 10}, {0xDCA, 11}, {0xDCF, 12}, {0xDD9, 13}, {0xDD2, 14}};
  Font font(small_font({{GFNT_TAG('G', 'S', 'U', 'B'), k_sinhrphf}}, 40, cmap));
  Glyphs g;
  Request request;

  ASSERT_EQ(font.result, GFNT_OK);
  request.script = "sinh";
  ASSERT_EQ(shape(font, V{0xD9A, 0xDCA}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{20, 22}));
  ASSERT_EQ(shape(font, V{0xDD2, 0xDCA, 0xD9A}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{25, 22, 20}));
  ASSERT_EQ(shape(font, V{0xD9A, 0xDCF}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{20, 23}));
}

static const std::vector<uint8_t> k_sinhlig = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x42, 0x00, 0x50, 0x00, 0x03,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x14, 0x6C, 0x61, 0x74, 0x6E, 0x00, 0x20,
       0x73, 0x69, 0x6E, 0x68, 0x00, 0x2C, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x6C, 0x6F, 0x63, 0x6C,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x14,
       0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x04, 0x00, 0x1E, 0x00, 0x03,
       0x00, 0x0B, 0x00, 0x09, 0x00, 0x01, 0x00, 0x01, 0x00, 0x09};

TEST(ShapeUse, ASinhalaConsonantAlLakunaConsonantIsOneSyllable) {
  // A ligature of KA, al-lakuna, KA under `locl`, which does not cross a syllable:
  // the al-lakuna stacks the second KA, as any halant does, so the three are one
  // syllable. Every expected value is what HarfBuzz 10.2.0 gives for these bytes.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0xD9A, 9}, {0xDCA, 11}};
  Font font(small_font({{GFNT_TAG('G', 'S', 'U', 'B'), k_sinhlig}}, 40, cmap));
  Glyphs g;
  Request request;

  ASSERT_EQ(font.result, GFNT_OK);
  request.script = "sinh";
  ASSERT_EQ(shape(font, V{0xD9A, 0xDCA, 0xD9A}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{30}));
  ASSERT_EQ(shape(font, V{0xD9A, 0xDCA, 0xD9A, 0xD9A}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{30, 9}));
}

static const std::vector<uint8_t> k_arablig = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x42, 0x00, 0x50, 0x00, 0x03,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x14, 0x61, 0x72, 0x61, 0x62, 0x00, 0x20,
       0x6C, 0x61, 0x74, 0x6E, 0x00, 0x2C, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x72, 0x6C, 0x69, 0x67,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x12,
       0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x04, 0x00, 0x1E, 0x00, 0x02,
       0x00, 0x0B, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0D};

TEST(ShapeArabic, AMarkAndALetterLigatedAreNoMarkToThePlacementOfMarks) {
  // FATHA and LAM made into one glyph by `rlig`, after a BEH. With no GPOS the
  // marks are placed and their advances zeroed by the character's category; the
  // ligature, which is a mark only at its first component, keeps its advance as
  // HarfBuzz 10.2.0 gives it for these bytes (the glyph's own, 800 in its font).
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0x628, 9}, {0x644, 11}, {0x64E, 13}};
  Font font(small_font({{GFNT_TAG('G', 'S', 'U', 'B'), k_arablig}}, 40, cmap));
  Glyphs g;
  Request request;

  ASSERT_EQ(font.result, GFNT_OK);
  request.script = "arab";
  request.rtl = true;
  ASSERT_EQ(shape(font, V{0x628}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{9}));
  ASSERT_EQ(shape(font, V{0x64E, 0x644}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{30}));
  ASSERT_EQ(shape(font, V{0x628, 0x64E, 0x644}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{30, 9}));
  ASSERT_EQ(g.size(), 2u);
  EXPECT_NE(g[0].x_advance, 0);
}

TEST(ShapeKhmer, ASubscriptMayFollowTheVowelsOfTheBaseButOnlyOne) {
  // KA, VOWEL SIGN AA, COENG, KHA is one syllable in HarfBuzz 10.2.0 and gets no
  // dotted circle; a second subscript after it, or a coeng with no consonant, does.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0x1780, 9}, {0x1781, 10}, {0x1782, 14}, {0x17D2, 11}, {0x17B6, 12},
      {0x25CC, 20}};
  Font font(small_font({}, 40, cmap));
  Glyphs g;
  Request request;

  ASSERT_EQ(font.result, GFNT_OK);
  request.script = "khmr";
  ASSERT_EQ(shape(font, V{0x1780, 0x17B6, 0x17D2, 0x1781}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{9, 12, 11, 10}));
  ASSERT_EQ(shape(font, V{0x1780, 0x17B6, 0x17D2, 0x1781, 0x17D2, 0x1782}, request,
                &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{9, 12, 11, 10, 20, 11, 14}));
  ASSERT_EQ(shape(font, V{0x1780, 0x17B6, 0x17D2}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{9, 12, 20, 11}));
}

TEST(ShapeKhmer, ASubscriptAfterTheVowelsTakesNoRegisterShifterOfItsOwn) {
  // In a broken syllable that starts at a vowel, the consonant under a coeng is just
  // that: the MUUSIKATOAN after it starts a syllable of its own and gets a dotted
  // circle. HarfBuzz 10.2.0 gives the glyphs below.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0x1781, 10}, {0x17D2, 11}, {0x17C1, 13}, {0x17C9, 14}, {0x25CC, 20}};
  Font font(small_font({}, 40, cmap));
  Glyphs g;
  Request request;

  ASSERT_EQ(font.result, GFNT_OK);
  request.script = "khmr";
  ASSERT_EQ(shape(font, V{0x17C1, 0x17D2, 0x1781, 0x17C9}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{13, 20, 11, 10, 20, 14}));
}

static const std::vector<uint8_t> k_prefpair = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x42, 0x00, 0x52, 0x00, 0x03,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x14, 0x64, 0x65, 0x76, 0x61, 0x00, 0x20,
       0x6C, 0x61, 0x74, 0x6E, 0x00, 0x2C, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x70, 0x72, 0x65, 0x66,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02,
       0x00, 0x06, 0x00, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08,
       0x00, 0x02, 0x00, 0x12, 0x00, 0x06, 0x00, 0x14, 0x00, 0x15, 0x00, 0x16,
       0x00, 0x17, 0x00, 0x18, 0x00, 0x19, 0x00, 0x01, 0x00, 0x06, 0x00, 0x09,
       0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C, 0x00, 0x0D, 0x00, 0x0E, 0x00, 0x06,
       0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02,
       0x00, 0x0E, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x0D, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0B};

TEST(ShapeIndic, APrefPairIsFoundAfterTheSortHasMovedAPreBaseMatra) {
  // Under `pref`, a single substitution of every glyph and a chain that accepts a
  // pre-base matra followed by the virama. The pair is looked for after the base,
  // and the syllable is sorted before that: KA, vowel sign I, virama has the matra
  // in front of the KA by then, so there is no pair and nothing is substituted.
  // Every expected value is what HarfBuzz 10.2.0 gives for these bytes.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0x915, 9}, {0x930, 10}, {0x94D, 11}, {0x93E, 12}, {0x93F, 13}};
  Font font(small_font({{GFNT_TAG('G', 'S', 'U', 'B'), k_prefpair}}, 40, cmap));
  Glyphs g;
  Request request;

  ASSERT_EQ(font.result, GFNT_OK);
  request.script = "deva";
  ASSERT_EQ(shape(font, V{0x915, 0x93F, 0x94D}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{13, 9, 11}));
  ASSERT_EQ(shape(font, V{0x915, 0x93E, 0x94D}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{9, 12, 11}));
}

static const std::vector<uint8_t> k_blwfbeng = {
       0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x42, 0x00, 0x50, 0x00, 0x03,
       0x44, 0x46, 0x4C, 0x54, 0x00, 0x14, 0x62, 0x65, 0x6E, 0x67, 0x00, 0x20,
       0x6C, 0x61, 0x74, 0x6E, 0x00, 0x2C, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x62, 0x6C, 0x77, 0x66,
       0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04,
       0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x02, 0x00, 0x0A,
       0x00, 0x02, 0x00, 0x15, 0x00, 0x16, 0x00, 0x01, 0x00, 0x02, 0x00, 0x0A,
       0x00, 0x0B};

TEST(ShapeIndic, OnlyAnOldSpecDevanagariFontGivesTheRaBlwf) {
  // Under `blwf`, a single substitution of RA and of the virama, in a font with the
  // script tag 'beng' alone (old-spec). HarfBuzz 10.2.0 leaves a Bengali RA at the
  // start of a syllable alone, and the virama after it, where a Devanagari font of
  // the same shape has both substituted.
  const std::vector<std::pair<uint32_t, uint16_t>> cmap = {
      {0x995, 9}, {0x9B0, 10}, {0x9CD, 11}};
  Font font(small_font({{GFNT_TAG('G', 'S', 'U', 'B'), k_blwfbeng}}, 40, cmap));
  Glyphs g;
  Request request;

  ASSERT_EQ(font.result, GFNT_OK);
  request.script = "beng";
  ASSERT_EQ(shape(font, V{0x9B0, 0x9CD, 0x995}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{10, 11, 9}));
}

// --- Apple's mort ------------------------------------------------------------------
//
// The older 16-bit form of morx. The tables are built byte for byte, and every
// expected value is what HarfBuzz 10.2.0 gives for the same bytes.

namespace {

Bytes mort_font(const Bytes & mort) {
  return small_font({{GFNT_TAG('m', 'o', 'r', 't'), mort}}, 40);
}

}  // namespace

TEST(ShapeMort, ANoncontextualSubtableMapsGlyphToGlyph) {
  Bytes font = mort_font(Bytes {
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x00, 0x12, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x05, 0x00, 0x03, 0x00, 0x09,
    0x00, 0x09, 0x00, 0x09});
  EXPECT_EQ(morx_run(font, "ABCDE"), (P{{1, 0}, {2, 1}, {3, 2}, {4, 3}, {9, 4}}));
}

TEST(ShapeMort, AContextualSubtableSubstitutesAtAMarkThatWasNeverSet) {
  // Offsets count words from the start of the state table, and the glyph is added
  // to them. An A substitutes the glyph at the mark, the first one, though no
  // entry set it; a substitution made at the end of the text needs a mark.
  Bytes table = Bytes {
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0xD0, 0x00, 0x00, 0x00, 0x01, 0x00, 0xC2, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x0A, 0x00, 0x10, 0x00, 0x1C,
    0x00, 0x3C, 0x00, 0x01, 0x00, 0x02, 0x04, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x10, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x16, 0x00, 0x00, 0x00, 0x3B, 0xFF, 0xFF,
    0x00, 0x10, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x45, 0x00, 0x10, 0x00, 0x00,
    0xFF, 0xFF, 0x00, 0x45};
  for (uint32_t g = 0; g < 64; g++) {
    gfnttest::put_u16(table, g < 2 ? 0 : static_cast<uint16_t>(g));
  }
  Bytes font = mort_font(table);
  EXPECT_EQ(morx_run(font, "BA"), (P{{31, 0}, {1, 1}}));
  EXPECT_EQ(morx_run(font, "AB"), (P{{30, 0}, {41, 1}}));
  EXPECT_EQ(morx_run(font, "A"), (P{{30, 0}}));
  EXPECT_EQ(morx_run(font, "B"), (P{{2, 0}}));
}

TEST(ShapeMort, ALigatureSumIsNotClearedBetweenStores) {
  // Two B's wait on the stack and an A runs a storing action then the last one:
  // the second store adds to the first one's sum.
  Bytes font = mort_font(Bytes {
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x6C, 0x00, 0x00, 0x00, 0x01, 0x00, 0x5E, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x0E, 0x00, 0x14, 0x00, 0x26,
    0x00, 0x36, 0x00, 0x3E, 0x00, 0x4E, 0x00, 0x01, 0x00, 0x02, 0x04, 0x05,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x1A,
    0x80, 0x00, 0x00, 0x14, 0x00, 0x36, 0x00, 0x14, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x1F, 0x80, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14,
    0x00, 0x15, 0x00, 0x16, 0x00, 0x17, 0x00, 0x18});
  EXPECT_EQ(morx_run(font, "BBA"), (P{{23, 0}, {1, 2}}));
  EXPECT_EQ(morx_run(font, "BA"), (P{{22, 0}, {1, 1}}));
}

TEST(ShapeMort, AnInsertionListIsAtTwiceItsOffsetPlusOne) {
  Bytes font = mort_font(Bytes {
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x00, 0x01, 0x00, 0x30, 0x00, 0x05,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x05, 0x00, 0x08, 0x00, 0x0E, 0x00, 0x14,
    0x00, 0x01, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x0E, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x0E, 0x00, 0x40,
    0x00, 0x12, 0xFF, 0xFF, 0x00, 0x00, 0x1E, 0x00, 0x1F, 0x00});
  EXPECT_EQ(morx_run(font, "AB"), (P{{1, 0}, {30, 0}, {31, 0}, {2, 1}}));
}

// ---- Bidirectional text ----

namespace {

/** What one letter of a bidi test font is drawn as: A-D are glyphs 1-4, Hebrew
 * alef, bet and gimel are glyphs 20, 21 and 22, and a space is 30. */
Bytes bidi_font() {
  return small_font({}, 40,
      {{0x05D0, 20}, {0x05D1, 21}, {0x05D2, 22}, {0x0020, 30}, {'(', 31},
          {')', 32}});
}

std::vector<Placed> bidi_run(const Bytes & font_bytes,
    const std::u32string & text, GFNT_BidiDirection paragraph) {
  Font font(font_bytes);
  EXPECT_EQ(font.result, GFNT_OK);
  std::vector<uint32_t> text_cps(text.begin(), text.end());
  const GFNT_Face * faces[1] = {font.face};
  GFNT_FaceRuns runs;
  memset(&runs, 0, sizeof runs);
  EXPECT_EQ(gfnt_faces_shape_bidi(faces, 1, text_cps.data(), text_cps.size(),
                paragraph, nullptr, nullptr, &runs, nullptr),
      GFNT_OK);
  std::vector<Placed> out;
  for (size_t r = 0; r < runs.count; r++) {
    for (size_t g = 0; g < runs.runs[r].run.count; g++) {
      out.push_back({runs.runs[r].run.glyphs[g].glyph,
          runs.runs[r].run.glyphs[g].cluster});
    }
  }
  gfnt_face_runs_free(&runs);
  return out;
}

}  // namespace

TEST(ShapeBidi, ARightToLeftRunInsideLeftToRightTextIsReversedInPlace) {
  // "AB " then alef bet gimel then " CD": the Hebrew reads from the right.
  Bytes font = bidi_font();
  EXPECT_EQ(bidi_run(font, U"AB \u05D0\u05D1\u05D2 CD", GFNT_BIDI_LTR),
      (P{{1, 0}, {2, 1}, {30, 2}, {22, 5}, {21, 4}, {20, 3}, {30, 6}, {3, 7},
          {4, 8}}));
}

TEST(ShapeBidi, ARightToLeftParagraphPutsItsFirstRunOnTheRight) {
  // Alef bet, a space, then AB: in a right-to-left paragraph the Latin sits to the
  // left of the Hebrew, and the space between the two follows the paragraph.
  Bytes font = bidi_font();
  EXPECT_EQ(bidi_run(font, U"\u05D0\u05D1 AB", GFNT_BIDI_RTL),
      (P{{1, 3}, {2, 4}, {30, 2}, {21, 1}, {20, 0}}));
  // With no direction given the first strong character decides: here, the same.
  EXPECT_EQ(bidi_run(font, U"\u05D0\u05D1 AB", GFNT_BIDI_AUTO),
      (P{{1, 3}, {2, 4}, {30, 2}, {21, 1}, {20, 0}}));
  // And plain Latin in an automatic paragraph stays left to right.
  EXPECT_EQ(bidi_run(font, U"AB", GFNT_BIDI_AUTO), (P{{1, 0}, {2, 1}}));
}

TEST(ShapeBidi, ABracketPairIsMirroredWithTheRunItIsIn) {
  // In a right-to-left paragraph the parentheses around AB are on its level and
  // drawn as each other's mirror image: the glyph of ")" stands left of AB and
  // that of "(" right of it, and AB itself, one level deeper, reads left to right.
  Bytes font = bidi_font();
  EXPECT_EQ(bidi_run(font, U"\u05D0 (AB) \u05D1", GFNT_BIDI_RTL),
      (P{{21, 7}, {30, 6}, {31, 5}, {1, 3}, {2, 4}, {32, 2}, {30, 1}, {20, 0}}));
}

// ---- Font fallback ----

namespace {

struct Faces {
  std::vector<std::unique_ptr<Font>> fonts;
  std::vector<const GFNT_Face *> faces;
  void add(std::vector<uint8_t> bytes) {
    fonts.push_back(std::make_unique<Font>(std::move(bytes)));
    EXPECT_EQ(fonts.back()->result, GFNT_OK);
    faces.push_back(fonts.back()->face);
  }
};

}  // namespace

TEST(ShapeFallback, EachCharacterGoesToTheFirstFaceThatHasIt) {
  Faces f;
  f.add(small_font({}, 4));  // A B C
  f.add(small_font({}, 8));  // A to G
  std::vector<uint32_t> text = cps("ABEC");
  GFNT_FaceRuns out{};
  ASSERT_EQ(gfnt_faces_shape(f.faces.data(), f.faces.size(), text.data(),
                text.size(), nullptr, nullptr, &out, nullptr), GFNT_OK);
  ASSERT_EQ(out.count, 3u);
  EXPECT_EQ(out.runs[0].face, 0u);
  EXPECT_EQ(out.runs[0].length, 2u);
  EXPECT_EQ(out.runs[1].face, 1u);
  EXPECT_EQ(out.runs[1].start, 2u);
  EXPECT_EQ(out.runs[1].run.glyphs[0].cluster, 2u);  // a cluster of the whole text
  EXPECT_EQ(out.runs[2].face, 0u);
  EXPECT_EQ(out.runs[2].run.glyphs[0].cluster, 3u);
  gfnt_face_runs_free(&out);
}

TEST(ShapeFallback, AClusterStaysInOneFaceWhenOneHasAllOfIt) {
  Faces f;
  f.add(small_font({}, 3));                 // A B
  f.add(small_font({}, 2, {{0x301, 1}}));   // A and the acute
  std::vector<uint32_t> text = {'A', 0x301};
  GFNT_FaceRuns out{};
  ASSERT_EQ(gfnt_faces_shape(f.faces.data(), 2, text.data(), 2, nullptr, nullptr,
                &out, nullptr), GFNT_OK);
  ASSERT_EQ(out.count, 1u);
  EXPECT_EQ(out.runs[0].face, 1u);
  gfnt_face_runs_free(&out);
}

TEST(ShapeFallback, AMarkTheBaseFaceLacksIsBorrowedAndCentredOverTheBase) {
  Faces f;
  f.add(small_font({}, 3));                 // A B
  f.add(small_font({}, 2, {{0x301, 1}}));   // A and the acute; no B
  std::vector<uint32_t> text = {'B', 0x301};
  GFNT_FaceRuns out{};
  ASSERT_EQ(gfnt_faces_shape(f.faces.data(), 2, text.data(), 2, nullptr, nullptr,
                &out, nullptr), GFNT_OK);
  ASSERT_EQ(out.count, 2u);
  EXPECT_EQ(out.runs[0].face, 0u);
  EXPECT_EQ(out.runs[1].face, 1u);
  const GFNT_ShapedGlyph & mark = out.runs[1].run.glyphs[0];
  EXPECT_EQ(mark.cluster, 1u);
  EXPECT_EQ(mark.x_advance, 0);
  // Half the base's advance back; the font has no outlines, so the mark's ink is
  // taken to be centred on its origin.
  EXPECT_EQ(mark.x_offset, -250);
  gfnt_face_runs_free(&out);
}

TEST(ShapeFallback, ARightToLeftTextComesBackLastStretchFirst) {
  Faces f;
  f.add(small_font({}, 4));
  f.add(small_font({}, 8));
  std::vector<uint32_t> text = cps("ABE");
  GFNT_ShapeOptions o{};
  o.direction = GFNT_DIRECTION_RTL;
  GFNT_FaceRuns out{};
  ASSERT_EQ(gfnt_faces_shape(f.faces.data(), 2, text.data(), 3, &o, nullptr,
                &out, nullptr), GFNT_OK);
  ASSERT_EQ(out.count, 2u);
  EXPECT_EQ(out.runs[0].start, 2u);
  EXPECT_EQ(out.runs[1].start, 0u);
  gfnt_face_runs_free(&out);
}

TEST(ShapeFallback, RefusesNoFacesAndEmptyTextIsEmpty) {
  Faces f;
  f.add(small_font({}, 4));
  GFNT_FaceRuns out{};
  uint32_t a = 'A';
  EXPECT_EQ(gfnt_faces_shape(f.faces.data(), 0, &a, 1, nullptr, nullptr, &out,
                nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_faces_shape(f.faces.data(), 1, nullptr, 0, nullptr, nullptr,
                &out, nullptr), GFNT_OK);
  EXPECT_EQ(out.count, 0u);
  gfnt_face_runs_free(&out);
}

namespace {

/** A font of boxes: glyph n is box n, mapped from the given code points. */
std::vector<uint8_t> box_font(
    const std::vector<std::tuple<uint16_t, int16_t, int16_t, int16_t, int16_t,
        uint16_t>> & boxes) {  // code point, x0, y0, x1, y1, advance
  std::vector<std::vector<uint8_t>> glyphs = {{}};
  std::vector<std::pair<uint16_t, int16_t>> metrics = {{500, 0}};
  std::vector<gfnttest::Segment4> segments;
  uint16_t n = 1;
  for (const auto & b : boxes) {
    auto [cp, x0, y0, x1, y1, adv] = b;
    glyphs.push_back(gfnttest::build_glyf_glyph(
        {{{x0, y0, true}, {x1, y0, true}, {x1, y1, true}, {x0, y1, true}}}));
    metrics.push_back({adv, x0});
    segments.push_back({cp, cp, static_cast<int16_t>(n - cp), {}});
    n++;
  }
  std::sort(segments.begin(), segments.end(),
      [](const gfnttest::Segment4 & a, const gfnttest::Segment4 & c) {
        return a.start < c.start;
      });
  segments.push_back({0xFFFF, 0xFFFF, 1, {}});
  std::vector<uint8_t> glyf, loca;
  bool long_loca = false;
  gfnttest::build_glyf_and_loca(glyphs, &glyf, &loca, &long_loca);
  return gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {
      {GFNT_TAG('h', 'e', 'a', 'd'),
          gfnttest::build_head(1000, long_loca ? 1 : 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'),
          gfnttest::build_hhea(800, -200, 0, static_cast<uint16_t>(glyphs.size()))},
      {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics, {})},
      {GFNT_TAG('m', 'a', 'x', 'p'),
          gfnttest::build_maxp(static_cast<uint16_t>(glyphs.size()))},
      {GFNT_TAG('c', 'm', 'a', 'p'),
          gfnttest::build_cmap({{3, 1, gfnttest::build_cmap_format4(segments)}})},
      {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
      {GFNT_TAG('l', 'o', 'c', 'a'), loca},
  });
}

}  // namespace

TEST(ShapeFallback, BorrowedMarksStackAboveAndBelowTheBaseInChains) {
  Faces f;
  f.add(box_font({{'B', 0, 0, 500, 600, 600}}));
  // Two marks above (ink 700 to 800) and one below (-250 to -150), no advance.
  f.add(box_font({{0x301, -400, 700, -100, 800, 0},
      {0x316, -400, -250, -100, -150, 0}}));
  std::vector<uint32_t> text = {'B', 0x301, 0x301, 0x316};
  GFNT_FaceRuns out{};
  ASSERT_EQ(gfnt_faces_shape(f.faces.data(), 2, text.data(), 4, nullptr, nullptr,
                &out, nullptr), GFNT_OK);
  ASSERT_EQ(out.count, 2u);
  ASSERT_EQ(out.runs[1].run.count, 3u);
  const GFNT_ShapedGlyph * m = out.runs[1].run.glyphs;
  // Canonical order puts the mark below first; it hangs from the base's bottom.
  EXPECT_EQ(m[0].y_offset, 130);
  // The first above rests just over the base (top 600), the next over that.
  EXPECT_EQ(m[1].y_offset, -80);
  EXPECT_EQ(m[2].y_offset, 40);
  for (int k = 0; k < 3; k++) {
    EXPECT_EQ(m[k].x_advance, 0);
    EXPECT_EQ(m[k].x_offset, -50);  // centred over a 600-wide base
  }
  gfnt_face_runs_free(&out);
}

// ---- Apple's kerx ----

namespace {

/** A kerx table of the given version over finished subtables. */
Bytes kerx_table(const std::vector<Bytes> & subtables, uint16_t version = 2) {
  Bytes out;
  gfnttest::put_u16(out, version);
  gfnttest::put_u16(out, 0);
  gfnttest::put_u32(out, static_cast<uint32_t>(subtables.size()));
  for (const Bytes & s : subtables) {
    out.insert(out.end(), s.begin(), s.end());
  }
  return out;
}

Bytes kerx_subtable(uint32_t flags, uint8_t format, const Bytes & body) {
  Bytes out;
  gfnttest::put_u32(out, static_cast<uint32_t>(12 + body.size()));
  gfnttest::put_u32(out, flags | format);
  gfnttest::put_u32(out, 0);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

Bytes kerx_pairs(std::vector<std::tuple<uint16_t, uint16_t, int16_t>> pairs,
    uint32_t flags = 0) {
  std::sort(pairs.begin(), pairs.end());
  Bytes body;
  gfnttest::put_u32(body, static_cast<uint32_t>(pairs.size()));
  gfnttest::put_u32(body, 0);
  gfnttest::put_u32(body, 0);
  gfnttest::put_u32(body, 0);
  for (const auto & p : pairs) {
    gfnttest::put_u16(body, std::get<0>(p));
    gfnttest::put_u16(body, std::get<1>(p));
    gfnttest::put_u16(body, static_cast<uint16_t>(std::get<2>(p)));
  }
  return kerx_subtable(flags, 0, body);
}

Glyphs kerx_shape(const Bytes & kerx, const std::string & text,
    const std::string & features = "", const Bytes & kern = {}) {
  std::vector<gfnttest::Table> tables = {{GFNT_TAG('k', 'e', 'r', 'x'), kerx}};
  if (!kern.empty()) {
    tables.push_back({GFNT_TAG('k', 'e', 'r', 'n'), kern});
  }
  return shape_bytes(small_font(tables), cps(text), features);
}

}  // namespace

TEST(ShapeKerx, APairIsSharedBetweenTheTwoGlyphsAndSubtablesAddUp) {
  Glyphs g = kerx_shape(kerx_table({kerx_pairs({{1, 2, -100}}),
      kerx_pairs({{1, 2, -30}, {2, 3, -40}})}), "ABC");
  ASSERT_EQ(g.size(), 3u);
  EXPECT_EQ(g[0].x_advance, 435);            // 500 - 65
  EXPECT_EQ(g[1].x_advance, 415);            // 500 - 65 - 20
  EXPECT_EQ(g[1].x_offset, -65);
  EXPECT_EQ(g[2].x_offset, -20);
  EXPECT_EQ(shape_bytes(small_font({{GFNT_TAG('k', 'e', 'r', 'x'),
                kerx_table({kerx_pairs({{1, 2, -100}})})}}), cps("AB"), "-kern")[0]
                .x_advance, 500);
}

TEST(ShapeKerx, ItReplacesTheKernTableAndOnlyAVersionTwoTableIsRead) {
  Bytes kern = kern_format0({{1, 2, -20}});
  EXPECT_EQ(kerx_shape(kerx_table({kerx_pairs({{1, 2, -100}})}), "AB", "", kern)[0]
                .x_advance, 450);
  // Any other version is as good as none, and `kern` is used as it would be.
  EXPECT_EQ(kerx_shape(kerx_table({kerx_pairs({{1, 2, -100}})}, 3), "AB", "",
                kern)[0].x_advance, 490);
}

TEST(ShapeKerx, VerticalAndDescendingSubtablesKernNothingInHorizontalText) {
  EXPECT_EQ(kerx_shape(kerx_table({kerx_pairs({{1, 2, -100}}, 0x80000000u)}),
                "AB")[0].x_advance, 500);
  EXPECT_EQ(kerx_shape(kerx_table({kerx_pairs({{1, 2, -100}}, 0x10000000u)}),
                "AB")[0].x_advance, 500);
}

TEST(ShapeKerx, ACrossStreamShiftCarriesOnToEveryGlyphAfterIt) {
  Glyphs g = kerx_shape(kerx_table({kerx_pairs({{1, 2, -100}}, 0x40000000u)}),
      "ABAB");
  ASSERT_EQ(g.size(), 4u);
  EXPECT_EQ(g[0].y_offset, 0);
  EXPECT_EQ(g[1].y_offset, -100);
  EXPECT_EQ(g[2].y_offset, -100);
  EXPECT_EQ(g[3].y_offset, -200);
  EXPECT_EQ(g[1].x_advance, 500);
}

TEST(ShapeKerx, ClassSubtablesAddTheTwoClassesToAnIndexIntoTheValues) {
  // Format 2: left A = 0, B = 2; right B = 0, C = 1: the value at index l + r.
  Bytes left = lookup6({{1, 0}, {2, 2}});
  Bytes right = lookup6({{2, 0}, {3, 1}});
  Bytes values;
  for (int16_t v : {-100, -60, -20, 40}) {
    gfnttest::put_u16(values, static_cast<uint16_t>(v));
  }
  Bytes body;
  gfnttest::put_u32(body, 4);
  gfnttest::put_u32(body, 28);
  gfnttest::put_u32(body, static_cast<uint32_t>(28 + left.size()));
  gfnttest::put_u32(body, static_cast<uint32_t>(28 + left.size() + right.size()));
  body.insert(body.end(), left.begin(), left.end());
  body.insert(body.end(), right.begin(), right.end());
  body.insert(body.end(), values.begin(), values.end());
  Bytes kerx = kerx_table({kerx_subtable(0, 2, body)});
  EXPECT_EQ(kerx_shape(kerx, "AB")[1].x_offset, -50);   // index 0: -100
  EXPECT_EQ(kerx_shape(kerx, "BB")[1].x_offset, -10);   // index 2: -20
  EXPECT_EQ(kerx_shape(kerx, "BC")[1].x_offset, 20);    // index 3: 40
  // A glyph with no class on either side has no kerning.
  EXPECT_EQ(kerx_shape(kerx, "CB")[1].x_offset, 0);
  EXPECT_EQ(kerx_shape(kerx, "AA")[1].x_offset, 0);
}

namespace {

/**
 * A contextual kerning subtable: classes A = 4 and B = 5; A pushes itself and
 * stays in state 1, B in state 1 applies the values at @p action (a byte offset
 * into @p values) and returns to state 0.
 */
Bytes kerx_contextual(const std::vector<int16_t> & values, uint16_t action,
    uint32_t flags = 0, uint16_t b_flags = 0) {
  Bytes classes;
  for (uint16_t v : std::initializer_list<uint16_t>{8, 1, 2, 4, 5}) {  // format 8, first glyph 1, two glyphs
    gfnttest::put_u16(classes, v);
  }
  Bytes states;
  for (uint16_t e : std::initializer_list<uint16_t>{0, 0, 0, 0, 1, 0,  // state 0: A pushes
                      0, 0, 0, 0, 1, 2}) {  // state 1: A pushes, B applies
    gfnttest::put_u16(states, e);
  }
  Bytes entries;
  for (uint16_t e : std::initializer_list<uint16_t>{0, 0, 0xFFFF,  // nothing
                      1, 0x8000, 0xFFFF,     // push
                      0, b_flags, action}) { // apply
    gfnttest::put_u16(entries, e);
  }
  Bytes vals;
  for (int16_t v : values) {
    gfnttest::put_u16(vals, static_cast<uint16_t>(v));
  }
  while (classes.size() % 4) {
    classes.push_back(0);
  }
  while (states.size() % 4) {
    states.push_back(0);
  }
  while (entries.size() % 4) {
    entries.push_back(0);
  }
  const uint32_t at_classes = 20;
  const uint32_t at_states = at_classes + static_cast<uint32_t>(classes.size());
  const uint32_t at_entries = at_states + static_cast<uint32_t>(states.size());
  const uint32_t at_values = at_entries + static_cast<uint32_t>(entries.size());
  Bytes body;
  for (uint32_t v : {6u, at_classes, at_states, at_entries, at_values}) {
    gfnttest::put_u32(body, v);
  }
  for (const Bytes * part : {&classes, &states, &entries, &vals}) {
    body.insert(body.end(), part->begin(), part->end());
  }
  return kerx_subtable(flags, 1, body);
}

}  // namespace

namespace {

/**
 * An attachment subtable: classes A = 4 and B = 5; A is marked and B hangs from
 * it by the action at index 0. A coordinate action (kind 2) puts point
 * (@p mark_x, @p mark_y) of the mark on point (@p curr_x, @p curr_y) of B; an
 * anchor action (kind 1) names anchor @p mark_x of the mark and @p mark_y of B.
 */
Bytes kerx_attachment(int16_t mark_x, int16_t mark_y, int16_t curr_x,
    int16_t curr_y, uint32_t flags = 0, uint32_t kind = 2) {
  Bytes classes;
  for (uint16_t v : std::initializer_list<uint16_t>{8, 1, 2, 4, 5}) {
    gfnttest::put_u16(classes, v);
  }
  Bytes states;
  for (uint16_t e : std::initializer_list<uint16_t>{0, 0, 0, 0, 1, 0,
                      0, 0, 0, 0, 1, 2}) {
    gfnttest::put_u16(states, e);
  }
  Bytes entries;
  for (uint16_t e : std::initializer_list<uint16_t>{0, 0, 0xFFFF,
                      1, 0x8000, 0xFFFF, 0, 0, 0}) {
    gfnttest::put_u16(entries, e);
  }
  // Coordinates are four numbers; anchor points are two point numbers.
  Bytes actions;
  const std::vector<int16_t> numbers = kind == 2
      ? std::vector<int16_t>{mark_x, mark_y, curr_x, curr_y}
      : std::vector<int16_t>{mark_x, mark_y};
  for (int16_t v : numbers) {
    gfnttest::put_u16(actions, static_cast<uint16_t>(v));
  }
  for (Bytes * part : {&classes, &states, &entries}) {
    while (part->size() % 4) {
      part->push_back(0);
    }
  }
  const uint32_t at_classes = 20;
  const uint32_t at_states = at_classes + static_cast<uint32_t>(classes.size());
  const uint32_t at_entries = at_states + static_cast<uint32_t>(states.size());
  const uint32_t at_actions = at_entries + static_cast<uint32_t>(entries.size());
  Bytes body;
  for (uint32_t v : {6u, at_classes, at_states, at_entries,
                     (kind << 30) | at_actions}) {
    gfnttest::put_u32(body, v);
  }
  for (const Bytes * part : {&classes, &states, &entries, &actions}) {
    body.insert(body.end(), part->begin(), part->end());
  }
  return kerx_subtable(flags, 4, body);
}

}  // namespace

TEST(ShapeKerx, AnAttachmentSubtableHangsAGlyphFromTheMarkedOne) {
  Bytes kerx = kerx_table({kerx_attachment(100, 50, 10, 20)});
  Glyphs g = kerx_shape(kerx, "AB");
  ASSERT_EQ(g.size(), 2u);
  // Point (100, 50) of A on point (10, 20) of B: B moves by the difference, less
  // the advance of A, which the pen has passed.
  EXPECT_EQ(g[1].x_offset, 90 - g[0].x_advance);
  EXPECT_EQ(g[1].y_offset, 30);
  // With nothing marked first, nothing hangs.
  EXPECT_EQ(kerx_shape(kerx, "BA")[1].y_offset, 0);
  // Cross-stream, each glyph then rides on those before it.
  Glyphs c = kerx_shape(kerx_table({kerx_attachment(100, 50, 10, 20,
      0x40000000u)}), "ABAB");
  EXPECT_EQ(c[1].y_offset, 30);
  EXPECT_EQ(c[2].y_offset, 30);
  EXPECT_EQ(c[3].y_offset, 60);
}

TEST(ShapeKerx, AnAttachmentCanNameAnchorsOfTheAnkrTable) {
  // Glyph 1 has anchors (10, 20) and (30, 40); glyph 2 has (5, 6) and (7, 8). The
  // lookup is a format 8 array from glyph 1, each value an offset into the data.
  Bytes lookup;
  for (uint16_t v : std::initializer_list<uint16_t>{8, 1, 2, 0, 12}) {
    gfnttest::put_u16(lookup, v);
  }
  Bytes data;
  for (uint32_t v : {2u, 0x000A0014u, 0x001E0028u, 2u, 0x00050006u, 0x00070008u}) {
    gfnttest::put_u32(data, v);
  }
  Bytes ankr;
  gfnttest::put_u16(ankr, 0);
  gfnttest::put_u16(ankr, 0);
  gfnttest::put_u32(ankr, 12);
  gfnttest::put_u32(ankr, static_cast<uint32_t>(12 + lookup.size()));
  ankr.insert(ankr.end(), lookup.begin(), lookup.end());
  ankr.insert(ankr.end(), data.begin(), data.end());
  // Anchor 1 of the mark on anchor 0 of B: (30, 40) less (5, 6).
  std::vector<gfnttest::Table> tables = {
      {GFNT_TAG('k', 'e', 'r', 'x'), kerx_table({kerx_attachment(1, 0, 0, 0, 0, 1)})},
      {GFNT_TAG('a', 'n', 'k', 'r'), ankr}};
  Glyphs g = shape_bytes(small_font(tables), cps("AB"), "");
  ASSERT_EQ(g.size(), 2u);
  EXPECT_EQ(g[1].x_offset, 25 - g[0].x_advance);
  EXPECT_EQ(g[1].y_offset, 34);
}

TEST(ShapeTrak, AdvancesGrowByTheTrackingForThePointSizeAndMoveHalfAsFar) {
  // Version 1.0, a horizontal table at 12 with one track, zero, for sizes 10, 20
  // and 40 whose values are 0, 100 and 300.
  Bytes trak;
  gfnttest::put_u32(trak, 0x00010000);
  gfnttest::put_u16(trak, 0);
  gfnttest::put_u16(trak, 12);
  gfnttest::put_u16(trak, 0);
  gfnttest::put_u16(trak, 0);
  gfnttest::put_u16(trak, 1);
  gfnttest::put_u16(trak, 3);
  gfnttest::put_u32(trak, 12 + 8 + 8);   // the size table
  gfnttest::put_u32(trak, 0);            // track 0.0
  gfnttest::put_u16(trak, 256);
  gfnttest::put_u16(trak, 12 + 8 + 8 + 12);   // its values
  for (uint32_t size : {10u, 20u, 40u}) {
    gfnttest::put_u32(trak, size << 16);
  }
  for (int16_t v : {0, 100, 300}) {
    gfnttest::put_u16(trak, static_cast<uint16_t>(v));
  }
  Font font(small_font({{GFNT_TAG('t', 'r', 'a', 'k'), trak}}));
  Glyphs g;
  Request request;
  EXPECT_EQ(shape(font, cps("A"), request, &g), GFNT_OK);
  const int32_t plain = g[0].x_advance;
  // With no point size nothing is added.
  request.point_size = 15;
  ASSERT_EQ(shape(font, cps("A"), request, &g), GFNT_OK);
  EXPECT_EQ(g[0].x_advance, plain + 50);
  EXPECT_EQ(g[0].x_offset, 25);
  // A size below the first goes on along the first two.
  request.point_size = 5;
  ASSERT_EQ(shape(font, cps("A"), request, &g), GFNT_OK);
  EXPECT_EQ(g[0].x_advance, plain - 50);
  request.features = "-trak";
  ASSERT_EQ(shape(font, cps("A"), request, &g), GFNT_OK);
  EXPECT_EQ(g[0].x_advance, plain);
}

TEST(ShapeKerx, WithTuplesAValueIsTheOffsetOfTheFirstOfAListOfThem) {
  // One pair, A B, whose value is the offset of two tuples, -100 and -10.
  Bytes body;
  gfnttest::put_u32(body, 1);
  gfnttest::put_u32(body, 0);
  gfnttest::put_u32(body, 0);
  gfnttest::put_u32(body, 0);
  gfnttest::put_u16(body, 1);
  gfnttest::put_u16(body, 2);
  gfnttest::put_u16(body, 12 + 16 + 6);  // from the start of the subtable
  gfnttest::put_u16(body, static_cast<uint16_t>(-100));
  gfnttest::put_u16(body, static_cast<uint16_t>(-10));
  Bytes sub;
  gfnttest::put_u32(sub, static_cast<uint32_t>(12 + body.size()));
  gfnttest::put_u32(sub, 0);
  gfnttest::put_u32(sub, 2);  // two tuples
  sub.insert(sub.end(), body.begin(), body.end());
  Glyphs g = kerx_shape(kerx_table({sub}), "AB");
  EXPECT_EQ(g[1].x_offset, -50);
  Glyphs plain = kerx_shape(kerx_table({kerx_pairs({{1, 2, -100}})}), "AB");
  EXPECT_EQ(g[0].x_advance, plain[0].x_advance);
}

TEST(ShapeKerx, AStateMachineAppliesItsValuesFromTheTopOfTheStack) {
  // The first value is for the glyph on top; one with its low bit set is the last.
  Bytes kerx = kerx_table({kerx_contextual({-10, -21, -30}, 0)});
  Glyphs g = shape_bytes(small_font({{GFNT_TAG('k', 'e', 'r', 'x'), kerx}}),
      cps("AAB"));
  ASSERT_EQ(g.size(), 3u);
  EXPECT_EQ(g[0].x_advance, 500 - 22);
  EXPECT_EQ(g[0].x_offset, -22);
  EXPECT_EQ(g[1].x_advance, 500 - 10);
  EXPECT_EQ(g[2].x_advance, 500);
  // The glyph the values did not reach stays on the stack for the next B.
  g = shape_bytes(small_font({{GFNT_TAG('k', 'e', 'r', 'x'),
      kerx_table({kerx_contextual({-10, -21, -30}, 0)})}}), cps("AAAB"));
  EXPECT_EQ(g[0].x_advance, 500);
  EXPECT_EQ(g[1].x_advance, 500 - 22);
  EXPECT_EQ(g[2].x_advance, 500 - 10);
  // A value offset is in bytes.
  g = shape_bytes(small_font({{GFNT_TAG('k', 'e', 'r', 'x'),
      kerx_table({kerx_contextual({-10, -21, -30}, 2)})}}), cps("AB"));
  EXPECT_EQ(g[0].x_advance, 500 - 22);
}

TEST(ShapeKerx, AStateMachinesStackHoldsEightAndAnotherPushEmptiesIt) {
  Bytes kerx = kerx_table({kerx_contextual({-10, -20, -30, -40, -50, -60, -70, -80,
      -90}, 0)});
  Bytes font = small_font({{GFNT_TAG('k', 'e', 'r', 'x'), kerx}});
  EXPECT_EQ(shape_bytes(font, cps("AAAAAAAAB"))[7].x_advance, 500 - 10);
  // The ninth A empties the stack: nothing is left for B.
  EXPECT_EQ(shape_bytes(font, cps("AAAAAAAAAB"))[0].x_advance, 500);
  EXPECT_EQ(shape_bytes(font, cps("AAAAAAAAAB"))[8].x_advance, 500);
}

TEST(ShapeKerx, AStateMachinesCrossStreamShiftsCarryOnAndAMarkedValueSetsApart) {
  Bytes font = small_font({{GFNT_TAG('k', 'e', 'r', 'x'), kerx_table({
      kerx_contextual({-10, -20, -31}, 0, 0x40000000u)})}});
  Glyphs g = shape_bytes(font, cps("AAAB"));
  ASSERT_EQ(g.size(), 4u);
  EXPECT_EQ(g[0].y_offset, -32);
  EXPECT_EQ(g[1].y_offset, -52);
  EXPECT_EQ(g[2].y_offset, -62);
  EXPECT_EQ(g[3].y_offset, -62);
  // -0x8000 puts a glyph back to nothing, and the glyphs after it start from there.
  font = small_font({{GFNT_TAG('k', 'e', 'r', 'x'), kerx_table({
      kerx_contextual({-10, -32768, -30}, 0, 0x40000000u)})}});
  g = shape_bytes(font, cps("AAAB"));
  EXPECT_EQ(g[0].y_offset, -30);
  EXPECT_EQ(g[1].y_offset, 0);
  EXPECT_EQ(g[2].y_offset, -10);
}

// ---- Indic vowel constraints and the characters HarfBuzz sorts differently ----

TEST(ShapeIndic, AVowelAndTheSignThatMakesAnotherVowelAreSetOffByADottedCircle) {
  // Gujarati A and the sign for AA add up to the letter AA: HarfBuzz puts a dotted
  // circle between them, and the circle shares the sign's cluster.
  Font font(small_font({}, 8, {{0x0A85, 2}, {0x0ABE, 3}, {0x0A86, 5},
      {0x25CC, 4}}));
  Glyphs g;
  Request request;
  request.script = "gjr2";
  ASSERT_EQ(shape(font, V{0x0A85, 0x0ABE}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2, 4, 3}));
  EXPECT_EQ(g[1].cluster, 0u);
  // Two letters that no one letter stands for are left alone.
  ASSERT_EQ(shape(font, V{0x0A86, 0x0ABE}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{5, 3}));
}

TEST(ShapeIndic, TheDevanagariAccentsAreMarksThatWantABase) {
  // U+0953 stands where a syllable modifier does, so alone it gets a circle.
  Font font(small_font({}, 8, {{0x0953, 2}, {0x25CC, 4}}));
  Glyphs g;
  Request request;
  request.script = "dev2";
  ASSERT_EQ(shape(font, V{0x0953}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{4, 2}));
}

TEST(ShapeIndic, GurmukhiIIFollowsABinduWithNoCircleOfItsOwn) {
  // BINDU 2, VOWEL SIGN II 3, VOWEL SIGN AA 5, the dotted circle 4.
  Font font(small_font({}, 8, {{0x0A02, 2}, {0x0A40, 3}, {0x0A3E, 5},
      {0x25CC, 4}}));
  Glyphs g;
  Request request;
  request.script = "gur2";
  ASSERT_EQ(shape(font, V{0x0A02, 0x0A40}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{4, 2, 3}));
  // Another vowel sign after the bindu is a syllable of its own.
  ASSERT_EQ(shape(font, V{0x0A02, 0x0A3E}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{4, 2, 4, 5}));
}

TEST(ShapeIndic, TheTeluguLengthMarksSortBeforeTheNuktaAndTheFirstBeforeTheSecond) {
  Font font(small_font({}, 8, {{0x0C55, 2}, {0x0C56, 3}, {0x25CC, 4}}));
  Glyphs g;
  Request request;
  request.script = "tel2";
  ASSERT_EQ(shape(font, V{0x0C56, 0x0C55}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{4, 2, 3}));
}

// ---- Variation sequences ----

namespace {

/** A font whose cmap has a format 4 subtable and a format 14 one. */
std::vector<uint8_t> uvs_font() {
  std::vector<gfnttest::Segment4> segments;
  segments.push_back({'A', 'C', static_cast<int16_t>(1 - 'A'), {}});  // A B C = 1 2 3
  segments.push_back({0xFFFF, 0xFFFF, 1, {}});
  // Format 14: selector U+FE00 with a default range 'B', and non-default A -> 5.
  auto u24 = [](std::vector<uint8_t> & out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
  };
  std::vector<uint8_t> f14;
  gfnttest::put_u16(f14, 14);
  gfnttest::put_u32(f14, 10 + 11 + 8 + 9);   // length
  gfnttest::put_u32(f14, 1);                 // one selector
  u24(f14, 0xFE00);
  gfnttest::put_u32(f14, 21);                // default UVS
  gfnttest::put_u32(f14, 29);                // non-default UVS
  gfnttest::put_u32(f14, 1);                 // one default range
  u24(f14, 'B');
  f14.push_back(0);
  gfnttest::put_u32(f14, 1);                 // one non-default mapping
  u24(f14, 'A');
  gfnttest::put_u16(f14, 5);
  std::vector<std::pair<uint16_t, int16_t>> metrics(8, {500, 0});
  return gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, 8)},
      {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics)},
      {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(8)},
      {GFNT_TAG('c', 'm', 'a', 'p'), gfnttest::build_cmap({
          {3, 1, gfnttest::build_cmap_format4(segments)}, {0, 5, f14}})},
  });
}

}  // namespace

TEST(ShapeVariation, ACharacterAndASelectorTheFontHasASequenceForShapeAsTheGlyphItNames) {
  Font font(uvs_font());
  ASSERT_EQ(font.result, GFNT_OK);
  Glyphs g;
  Request request;
  ASSERT_EQ(shape(font, V{'A', 0xFE00}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{5}));
  // A default sequence uses the character's own glyph, and the selector is spent.
  ASSERT_EQ(shape(font, V{'B', 0xFE00}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2}));
  // No entry: the character, and the selector after it hidden as before.
  ASSERT_EQ(shape(font, V{'C', 0xFE00}, request, &g), GFNT_OK);
  EXPECT_EQ(g[0].glyph, 3u);
  // The selector has to follow the character directly.
  ASSERT_EQ(shape(font, V{'A', 0x0301, 0xFE00}, request, &g), GFNT_OK);
  EXPECT_EQ(g[0].glyph, 1u);
}

TEST(ShapeKhmer, ACoengWithNoConsonantAfterItIsABrokenClusterUnlessItFollowsOne) {
  // KA 2, COENG 3, AA 4, the dotted circle 5.
  Font font(small_font({}, 8, {{0x1780, 2}, {0x17D2, 3}, {0x17B6, 4},
      {0x25CC, 5}}));
  Glyphs g;
  Request request;
  request.script = "khmr";
  // Straight after a consonant it is left alone.
  ASSERT_EQ(shape(font, V{0x1780, 0x17D2}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2, 3}));
  // After a vowel sign, or alone, it stands where a base would, and the circle
  // is put in front of it.
  ASSERT_EQ(shape(font, V{0x17D2}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{5, 3}));
  ASSERT_EQ(shape(font, V{0x1780, 0x17B6, 0x17D2}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2, 4, 5, 3}));
  // The trailing coeng ends the syllable, so a vowel after it starts a broken
  // one and is given its own circle.
  ASSERT_EQ(shape(font, V{0x1780, 0x17D2, 0x17B6}, request, &g), GFNT_OK);
  EXPECT_EQ(ids(g), (V{2, 3, 5, 4}));
}
