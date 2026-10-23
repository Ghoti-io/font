/**
 * @file
 *
 * Where a `glyf` glyph's origin is: `outline-placement.ttf` and its two variable
 * twins, whose left side bearings are not their `xMin`.
 *
 * The points a `glyf` glyph stores are in the glyph's own design coordinates,
 * whose origin is the first phantom point, `xMin - lsb`. A renderer draws the glyph
 * at the pen with that point on it, so the outline is moved by `lsb - xMin`
 * (design.md section 7.3, "Where a glyph's outline sits", has the rules, which of
 * them the specification states, and the measured table of what FreeType, HarfBuzz
 * and fontTools do). Every number here was read off FreeType's outline for the same
 * font, and `make check-oracle-glyf` and `check-oracle-var` hold the library to it
 * glyph by glyph.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <cstdio>
#include <string>
#include <vector>

#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>

#include "../../src/glyf/glyf.h"
#include "../../src/outline/outline.h"

namespace {

/** The glyphs of the three fixtures, in file order. */
enum Glyph : uint32_t {
  kNotdef = 0,
  kSpace = 1,
  kBox = 2,          // bearing == xMin: the control
  kMoved = 3,        // bearing 160, xMin 100
  kBack = 4,         // bearing 50, xMin 200
  kComp = 5,         // bearing 20, xMin 150
  kUmm = 6,          // USE_MY_METRICS on `moved`
  kUmmFirst = 7,     // flagged component first, an unflagged one after
  kUmmLast = 8,      // both flagged: the last wins
  kUmmNested = 9,    // flagged, and the component is itself flagged
  kCompEq = 10,      // bearing == xMin: the control for composites
  kUmmEmpty = 11,    // the flagged component is empty
};

struct Font {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  explicit Font(const std::string & name) {
    const std::string path = gfnttest::data("fonts/" + name);
    EXPECT_EQ(gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &blob,
        &error), GFNT_OK) << "could not read " << path;
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK)
        << name;
  }
  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;
};

/** The left edge of a glyph's outline, in whole font units, or INT32_MIN if it has none. */
int32_t left_edge(const Font & font, uint32_t glyph,
    const GFNT_Variation * variation = nullptr) {
  GFNT_Outline * outline = nullptr;
  GFNT_Box box{};
  int32_t edge = INT32_MIN;

  EXPECT_EQ(gfnt_face_glyph_outline(font.face, glyph, variation, nullptr,
      &outline, nullptr), GFNT_OK) << "glyph " << glyph;
  if (outline && gfnt_outline_control_box(outline, &box) == GFNT_OK
      && !gfnt_box_is_empty(&box)) {
    EXPECT_EQ(box.x_min % 64, 0) << "glyph " << glyph;
    edge = box.x_min / 64;
  }
  gfnt_outline_destroy(outline);
  return edge;
}

/** Every glyph's left edge, in file order. */
std::vector<int32_t> left_edges(const Font & font,
    const GFNT_Variation * variation = nullptr) {
  std::vector<int32_t> out;

  for (uint32_t glyph = kBox; glyph <= kUmmEmpty; ++glyph) {
    out.push_back(left_edge(font, glyph, variation));
  }
  return out;
}

}  // namespace

// The expected edges are `xMin` moved by `lsb - xMin`, worked by hand:
//   box   100 (bearing 100)                    -> 100, the control
//   moved 100 (bearing 160)                    -> 160
//   back  200 (bearing 50)                     -> 50
//   comp  150 (bearing 20)                     -> 20
//   umm   140, lends `moved`'s point (100-160) -> 140 + 60 = 200
//   first 100, lends `moved`'s                 -> 100 + 60 = 160
//   last  100, the last flag is `box`'s (0)    -> 100
//   nest  150, lends umm's final (-60)         -> 150 + 60 = 210
//   eq    150 (bearing 150)                    -> 150, the control
//   empty 100, lends `space`'s (0 - 25)        -> 100 + 25 = 125
TEST(Placement, ASimpleGlyphIsMovedByItsBearingLessItsXMin) {
  Font font("outline-placement.ttf");

  EXPECT_EQ(left_edge(font, kBox), 100);
  EXPECT_EQ(left_edge(font, kMoved), 160);
  EXPECT_EQ(left_edge(font, kBack), 50);
}

TEST(Placement, AGlyphWhoseBearingIsItsXMinDoesNotMove) {
  Font font("outline-placement.ttf");
  GFNT_Box stated{};
  GFNT_Box control{};
  GFNT_Outline * outline = nullptr;

  for (uint32_t glyph : {uint32_t(kBox), uint32_t(kCompEq)}) {
    ASSERT_EQ(gfnt_face_glyph_outline(font.face, glyph, nullptr, nullptr,
        &outline, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_glyph_stated_box(font.face, glyph, &stated, nullptr),
        GFNT_OK);
    ASSERT_EQ(gfnt_outline_control_box(outline, &control), GFNT_OK);
    EXPECT_EQ(control.x_min, stated.x_min) << "glyph " << glyph;
    EXPECT_EQ(control.x_max, stated.x_max) << "glyph " << glyph;
    gfnt_outline_destroy(outline);
    outline = nullptr;
  }
}

TEST(Placement, PlacementMovesTheShapeAndChangesNothingAboutIt) {
  // The same glyph twice, one with its bearing equal to its xMin and one not:
  // `box` and `moved` have the same points, so they differ in x by exactly the
  // distance between their origins (60) and in nothing else.
  Font font("outline-placement.ttf");
  GFNT_Outline * plain = nullptr;
  GFNT_Outline * moved = nullptr;

  ASSERT_EQ(gfnt_face_glyph_outline(font.face, kBox, nullptr, nullptr, &plain,
      nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_glyph_outline(font.face, kMoved, nullptr, nullptr,
      &moved, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_outline_point_count(plain), gfnt_outline_point_count(moved));
  for (size_t i = 0; i < gfnt_outline_point_count(plain); ++i) {
    GFNT_Point a{};
    GFNT_Point b{};

    ASSERT_EQ(gfnt_outline_point_at(plain, i, &a, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_outline_point_at(moved, i, &b, nullptr), GFNT_OK);
    EXPECT_EQ(b.x - a.x, 60 * 64) << "point " << i;
    EXPECT_EQ(b.y, a.y) << "point " << i;
  }
  gfnt_outline_destroy(plain);
  gfnt_outline_destroy(moved);
}

TEST(Placement, ACompositeIsMovedByItsOwnBearingAndNotItsComponents) {
  Font font("outline-placement.ttf");

  // `comp` is `box` offset by 50, so its xMin is 150, and its own bearing is 20.
  // `box`'s bearing is its xMin and would leave the composite at 150.
  EXPECT_EQ(left_edge(font, kComp), 20);
  EXPECT_EQ(left_edge(font, kCompEq), 150);
}

TEST(Placement, AComponentFlaggedUseMyMetricsLendsItsPoint) {
  Font font("outline-placement.ttf");

  // The composite's own bearing is 7; the flagged component's origin is used.
  EXPECT_EQ(left_edge(font, kUmm), 200);
  // The flagged component first, then an unflagged one: the second does not
  // take the point back.
  EXPECT_EQ(left_edge(font, kUmmFirst), 160);
  // Both flagged: the last one counts.
  EXPECT_EQ(left_edge(font, kUmmLast), 100);
  // A flagged component that is itself a composite lending its metrics lends
  // the final point.
  EXPECT_EQ(left_edge(font, kUmmNested), 210);
  // A flagged component with no description lends an origin of 0 - 25.
  EXPECT_EQ(left_edge(font, kUmmEmpty), 125);
}

TEST(Placement, ARequestForTheDefaultInstanceByNameIsPlacedLikeNone) {
  Font font("variable-placement.ttf");
  const GFNT_F2Dot14 zero = 0;
  const GFNT_Variation at_default{&zero, 1, GFNT_DELTA_ROUND_HALF_UP};

  EXPECT_EQ(left_edges(font, &at_default), left_edges(font));
  // And it is the static font's answer: same glyphs, same bearings.
  Font fixed("outline-placement.ttf");
  EXPECT_EQ(left_edges(font), left_edges(fixed));
}

// At wght = +1 with no HVAR, `gvar` moves the first phantom point too:
//   box    points stay, point +20      -> origin 20 right, edge 100 - 20 = 80
//   moved  points +10, point -60 + 30  -> 110 + 30 = 140
//   back   no gvar                     -> 50
//   comp   point 130 - 15 = 115        -> 150 - 115 = 35
//   umm    lends moved's -30 (its own +7 is not added): 155 + 30 = 185
//   empty  lends space's -25 + 12 = -13: 100 + 13 = 113
TEST(Placement, WithNoHvarGvarMovesTheFirstPhantomPointToo) {
  Font font("variable-placement.ttf");
  const GFNT_F2Dot14 one = 16384;
  const GFNT_Variation up{&one, 1, GFNT_DELTA_ROUND_HALF_UP};

  EXPECT_EQ(left_edge(font, kBox, &up), 80);
  EXPECT_EQ(left_edge(font, kMoved, &up), 140);
  EXPECT_EQ(left_edge(font, kBack, &up), 50);
  EXPECT_EQ(left_edge(font, kComp, &up), 35);
  EXPECT_EQ(left_edge(font, kUmm, &up), 185);
  EXPECT_EQ(left_edge(font, kUmmFirst, &up), 140);
  EXPECT_EQ(left_edge(font, kUmmLast, &up), 90);
  EXPECT_EQ(left_edge(font, kUmmNested, &up), 195);
  EXPECT_EQ(left_edge(font, kCompEq, &up), 150);
  // `space` has no description and gvar moves its origin by 12: 100 + 25 - 12.
  EXPECT_EQ(left_edge(font, kUmmEmpty, &up), 113);
}

// With an HVAR the stored point stands: gvar's phantom delta is not applied, and
// neither is HVAR's left side bearing mapping (FreeType's behaviour; HarfBuzz and
// fontTools apply gvar's delta here, which design.md section 7.3 records).
//   box    100                         moved  110 + 60 = 170
//   comp   150 - 130 = 20              umm    155 + 60 = 215
TEST(Placement, WithAnHvarTheStoredPointStands) {
  Font font("variable-placement-hvar.ttf");
  const GFNT_F2Dot14 one = 16384;
  const GFNT_Variation up{&one, 1, GFNT_DELTA_ROUND_HALF_UP};

  EXPECT_EQ(left_edge(font, kBox, &up), 100);
  EXPECT_EQ(left_edge(font, kMoved, &up), 170);
  EXPECT_EQ(left_edge(font, kBack, &up), 50);
  EXPECT_EQ(left_edge(font, kComp, &up), 20);
  EXPECT_EQ(left_edge(font, kUmm, &up), 215);
  EXPECT_EQ(left_edge(font, kUmmFirst, &up), 170);
  EXPECT_EQ(left_edge(font, kUmmLast, &up), 110);
  EXPECT_EQ(left_edge(font, kUmmNested, &up), 225);
  EXPECT_EQ(left_edge(font, kCompEq, &up), 150);
  EXPECT_EQ(left_edge(font, kUmmEmpty, &up), 125);
}

TEST(Placement, ACharstringOutlineIsNotMoved) {
  // `cff.otf` states a bearing of zero for glyphs whose outlines start at 40, 80
  // and 60. FreeType, HarfBuzz and fontTools leave a charstring where it draws
  // itself, and so does this library.
  Font font("cff.otf");

  EXPECT_EQ(left_edge(font, 2), 40);
  EXPECT_EQ(left_edge(font, 3), 80);
  EXPECT_EQ(left_edge(font, 4), 60);
}

namespace {

using Bytes = std::vector<uint8_t>;

uint32_t be32(const Bytes & b, size_t at) {
  return (uint32_t)b[at] << 24 | (uint32_t)b[at + 1] << 16 | (uint32_t)b[at + 2] << 8 | b[at + 3];
}

Bytes file_bytes(const std::string & name) {
  const std::string path = gfnttest::data("fonts/" + name);
  Bytes out;
  FILE * f = std::fopen(path.c_str(), "rb");
  EXPECT_NE(f, nullptr) << path;
  if (!f) {
    return out;
  }
  int c;
  while ((c = std::fgetc(f)) != EOF) {
    out.push_back(static_cast<uint8_t>(c));
  }
  std::fclose(f);
  return out;
}

/** @p font with the table @p tag cut to @p keep bytes, or dropped if @p keep is negative (every other table as it was). */
Bytes with_table_cut(const Bytes & font, uint32_t tag, long keep) {
  std::vector<gfnttest::Table> tables;
  const size_t count = (size_t)font[4] << 8 | font[5];
  for (size_t i = 0; i < count; i++) {
    const size_t entry = 12 + 16 * i;
    gfnttest::Table t;
    t.tag = be32(font, entry);
    const size_t offset = be32(font, entry + 8);
    const size_t length = be32(font, entry + 12);
    if (t.tag == tag && keep < 0) {
      continue;
    }
    t.data = Bytes(font.begin() + (long)offset, font.begin() + (long)(offset + length));
    if (t.tag == tag) {
      t.data.resize((size_t)keep);
    }
    tables.push_back(t);
  }
  return gfnttest::build_sfnt(be32(font, 0), tables);
}

/** A face loaded from bytes. */
struct BytesFont {
  Bytes bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  GFNT_Result loaded = GFNT_OK;

  explicit BytesFont(Bytes b) : bytes(std::move(b)) {
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, &error),
        GFNT_OK);
    loaded = gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error);
  }
  ~BytesFont() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  BytesFont(const BytesFont &) = delete;
  BytesFont & operator=(const BytesFont &) = delete;
};

}  // namespace

// The planted-defect pass found these four seams of the placement unseen.

TEST(Placement, TheUnplacedOutlineIsWhereTheFileStoresItAndThePlacedOneIsMoved) {
  // The hinter is given the outline unplaced and moves it itself afterwards; `moved` stores its points at 100 and its
  // bearing says 160.
  Font font("outline-placement.ttf");
  GFNT_Outline * unplaced = nullptr;
  GFNT_Box box{};

  ASSERT_EQ(gfnt_face_glyph_outline_unplaced(font.face, kMoved, nullptr, nullptr, &unplaced, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_outline_control_box(unplaced, &box), GFNT_OK);
  EXPECT_EQ(box.x_min, 100 * 64);
  gfnt_outline_destroy(unplaced);
  EXPECT_EQ(left_edge(font, kMoved), 160);
}

TEST(Placement, AGlyphLoadedOntoAnOutlineThatHoldsPointsMovesOnlyItsOwn) {
  Font font("outline-placement.ttf");
  GFNT_Outline * outline = nullptr;
  GFNT_Outline * first = nullptr;
  GFNT_Outline * second = nullptr;

  ASSERT_EQ(gfnt_outline_create(nullptr, &outline, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_glyf_load(font.face, kBack, nullptr, outline, &font.error), GFNT_OK);
  const size_t held = gfnt_outline_point_count(outline);
  ASSERT_GT(held, 0u);
  ASSERT_EQ(gfnt_glyf_load(font.face, kMoved, nullptr, outline, &font.error), GFNT_OK);
  ASSERT_EQ(gfnt_face_glyph_outline(font.face, kBack, nullptr, nullptr, &first, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_glyph_outline(font.face, kMoved, nullptr, nullptr, &second, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_outline_point_count(first), held);
  ASSERT_EQ(gfnt_outline_point_count(outline), held + gfnt_outline_point_count(second));
  for (size_t i = 0; i < gfnt_outline_point_count(outline); i++) {
    GFNT_Point got{}, want{};
    ASSERT_EQ(gfnt_outline_point_at(outline, i, &got, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_outline_point_at(i < held ? first : second, i < held ? i : i - held, &want, nullptr), GFNT_OK);
    EXPECT_EQ(got.x, want.x) << "point " << i << (i < held ? ": held before, not moved again" : "");
    EXPECT_EQ(got.y, want.y) << i;
  }
  gfnt_outline_destroy(outline);
  gfnt_outline_destroy(first);
  gfnt_outline_destroy(second);
}

TEST(Placement, AFaceWithNoHmtxStatesNoBearingAndItsGlyphsStayWhereTheirPointsAre) {
  BytesFont font(with_table_cut(file_bytes("outline-placement.ttf"), GFNT_TAG('h', 'm', 't', 'x'), -1));
  ASSERT_EQ(font.loaded, GFNT_OK);
  GFNT_Outline * outline = nullptr;
  GFNT_Box box{};

  ASSERT_EQ(gfnt_face_glyph_outline(font.face, kMoved, nullptr, nullptr, &outline, &font.error), GFNT_OK);
  ASSERT_EQ(gfnt_outline_control_box(outline, &box), GFNT_OK);
  EXPECT_EQ(box.x_min, 100 * 64) << "no bearing to place it by";
  gfnt_outline_destroy(outline);
}

TEST(Placement, AnHmtxThatIsThereAndCannotBeReadRefusesTheGlyphAndNeverPlacesItAnywhereElse) {
  // `hhea` says the font has no horizontal metrics at all: `hmtx` is there and no entry of it can be found.  (A table
  // cut short instead shrinks the face's glyph count, so no glyph is left to ask.)
  Bytes bytes = file_bytes("outline-placement.ttf");
  const size_t count = (size_t)bytes[4] << 8 | bytes[5];
  size_t hhea = 0;
  for (size_t i = 0; i < count; i++) {
    if (be32(bytes, 12 + 16 * i) == GFNT_TAG('h', 'h', 'e', 'a')) {
      hhea = be32(bytes, 12 + 16 * i + 8);
    }
  }
  ASSERT_NE(hhea, 0u);
  bytes[hhea + 34] = 0;
  bytes[hhea + 35] = 0;
  BytesFont font(bytes);
  ASSERT_EQ(font.loaded, GFNT_OK);
  GFNT_Outline * outline = nullptr;
  EXPECT_EQ(gfnt_face_glyph_outline(font.face, kMoved, nullptr, nullptr, &outline, &font.error), GFNT_ERR_CORRUPT)
      << "the glyph is refused, not handed back where its points are";
  EXPECT_EQ(outline, nullptr);
  gfnt_outline_destroy(outline);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
