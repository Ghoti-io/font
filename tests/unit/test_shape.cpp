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
  options.direction = request.rtl ? GFNT_DIRECTION_RTL : GFNT_DIRECTION_LTR;
  options.features = features.empty() ? nullptr : features.data();
  options.feature_count = features.size();
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
