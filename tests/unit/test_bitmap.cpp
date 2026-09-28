/**
 * @file
 *
 * The four standalone bitmap containers: PCF, BDF, PSF 1 and 2, and `.hex`.
 *
 * documentation/design.md sections 7.1 and 7.5.
 *
 * **What this suite leans on, and why it is not just more expectations of this
 * library's own.** Four unrelated files hold one design - eight glyphs of eight by
 * sixteen pixels - and four readers with no code in common must produce identical
 * pixels from them. A wrong shift, a reversed bit order or an off-by-one row would
 * have to be made in four places, the same way, to pass. That is the same argument
 * the PFB, PFA and CFF spellings of one Type 1 program make, and it is why the
 * fixtures were built as one design in the first place.
 *
 * `bitmap.pcf` and `bitmap-lsb.pcf` are the sharper case: the same glyphs with
 * their bits reversed, their bytes reversed, a two-byte scan unit and rows padded
 * to four. On a little-endian machine no font in the world exercises that path, so
 * the fixture is the only thing that can.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/font/bitmap.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/name.h>
#include <ghoti.io/font/raster.h>

#include <gtest/gtest.h>

#include "../../src/bitmap/bitmap.h"

namespace {

/** The shared design's shape, from tools/fixtures/make_fixtures.py. */
constexpr size_t kGlyphs = 8;
constexpr uint32_t kWidth = 8;
constexpr uint32_t kRows = 16;
constexpr int32_t kAscent = 14;
constexpr int32_t kDescent = -2;

/** A fixture read from disk, with a blob and a face over it. */
struct Fixture {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  GFNT_Result result = GFNT_ERR_INTERNAL;

  explicit Fixture(const std::string & name) {
    gfnt_error_clear(&error);
    const std::string path = gfnttest::data("fonts/" + name);
    result = gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &blob,
        &error);
    EXPECT_EQ(result, GFNT_OK) << "could not read " << path;
    if (result != GFNT_OK) {
      return;
    }
    result = gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error);
    EXPECT_EQ(result, GFNT_OK) << name << ": " << error.message;
  }

  ~Fixture() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }

  Fixture(const Fixture &) = delete;
  Fixture & operator=(const Fixture &) = delete;
};

/** A face over bytes this test built, for the refusal arms. */
struct Crafted {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  GFNT_Result result = GFNT_ERR_INTERNAL;

  explicit Crafted(const std::string & bytes,
      const GFNT_Limits * limits = nullptr) {
    gfnt_error_clear(&error);
    result = gfnt_blob_create_memory(bytes.data(), bytes.size(),
        GFNT_BLOB_COPY, limits, nullptr, &blob, &error);
    if (result != GFNT_OK) {
      return;
    }
    result = gfnt_face_load(blob, 0, limits, nullptr, &face, &error);
  }

  ~Crafted() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }

  Crafted(const Crafted &) = delete;
  Crafted & operator=(const Crafted &) = delete;
};

/** One glyph's pixels as a string of rows, for comparing containers. */
std::string pixels(const GFNT_Face * face, uint32_t glyph) {
  GFNT_BitmapGlyph bitmap{};
  GFNT_Error error{};
  std::string out;

  if (gfnt_face_glyph_bitmap(face, glyph, 0, &bitmap, &error) != GFNT_OK) {
    return std::string("refused: ") + error.message;
  }
  out += std::to_string(bitmap.width) + "x" + std::to_string(bitmap.height)
      + "\n";
  for (uint32_t y = 0; y < bitmap.height; ++y) {
    for (uint32_t x = 0; x < bitmap.width; ++x) {
      out += gfnt_bitmap_pixel(&bitmap, x, y) ? '#' : '.';
    }
    out += '\n';
  }
  return out;
}

/** The containers that hold the shared design, in no particular order. */
const std::vector<std::string> & shared_design() {
  static const std::vector<std::string> names = {
      "bitmap.hex", "bitmap.psf", "bitmap.bdf", "bitmap.pcf", "bitmap-lsb.pcf",
  };
  return names;
}

/** The internal parse, for the two facts the public API deliberately hides. */
const GFNT_BitmapFont * parsed(const GFNT_Face * face) {
  const GFNT_BitmapFont * font = nullptr;
  if (gfnt_face_bitmap(face, &font, nullptr) != GFNT_OK) {
    return nullptr;
  }
  return font;
}

// --------------------------------------------------------------- one design

TEST(Bitmap, FourContainersDrawOneDesign) {
  // The whole argument of this suite. If the reference pixels below are wrong,
  // they are wrong in the fixture generator - and then four readers that share no
  // code would all have to be wrong the same way for this to pass.
  std::vector<std::vector<std::string>> each;
  for (const std::string & name : shared_design()) {
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    size_t glyphs = 0;
    ASSERT_EQ(gfnt_face_num_glyphs(fixture.face, &glyphs, nullptr), GFNT_OK)
        << name;
    ASSERT_EQ(glyphs, kGlyphs) << name;

    std::vector<std::string> mine;
    for (uint32_t glyph = 0; glyph < kGlyphs; ++glyph) {
      mine.push_back(pixels(fixture.face, glyph));
    }
    each.push_back(mine);
  }
  ASSERT_EQ(each.size(), shared_design().size());
  for (size_t i = 1; i < each.size(); ++i) {
    EXPECT_EQ(each[0], each[i])
        << shared_design()[0] << " and " << shared_design()[i]
        << " hold one design and read differently";
  }
}

TEST(Bitmap, TheTwoBitOrdersAreNotEachOther) {
  // The control for the identity above. Two glyphs of the design are a single
  // column - the leftmost and the rightmost - and under a reversed bit order each
  // reads as the other while **every other glyph still looks like a glyph**. So
  // this asserts which is which, in every container, rather than trusting that a
  // mistake would have been visible.
  for (const std::string & name : shared_design()) {
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    GFNT_BitmapGlyph left{};
    GFNT_BitmapGlyph right{};
    ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 4, 0, &left, nullptr),
        GFNT_OK) << name;
    ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 5, 0, &right, nullptr),
        GFNT_OK) << name;
    for (uint32_t y = 0; y < kRows; ++y) {
      EXPECT_EQ(gfnt_bitmap_pixel(&left, 0, y), 255u) << name << " row " << y;
      EXPECT_EQ(gfnt_bitmap_pixel(&left, kWidth - 1, y), 0u) << name;
      EXPECT_EQ(gfnt_bitmap_pixel(&right, kWidth - 1, y), 255u) << name;
      EXPECT_EQ(gfnt_bitmap_pixel(&right, 0, y), 0u) << name;
    }
  }
}

TEST(Bitmap, ABaselineIsStatedOrItIsNot) {
  // The honest half of the design. BDF states FONT_ASCENT and FONT_DESCENT and PCF
  // compiles them into its accelerators; PSF is a console cell and `.hex` is a row
  // of bits, and neither says where the baseline is. So the two pairs report
  // *different* metrics for the same pixels, and the flag says which is a
  // measurement and which is the box reported where it is.
  struct Case {
    const char * name;
    bool states;
  };
  const std::vector<Case> cases = {
      {"bitmap.bdf", true}, {"bitmap.pcf", true}, {"bitmap-lsb.pcf", true},
      {"bitmap.hex", false}, {"bitmap.psf", false},
  };
  for (const Case & entry : cases) {
    Fixture fixture(entry.name);
    ASSERT_EQ(fixture.result, GFNT_OK) << entry.name;

    const GFNT_BitmapFont * font = parsed(fixture.face);
    ASSERT_NE(font, nullptr) << entry.name;
    EXPECT_EQ(font->states_baseline, entry.states) << entry.name;

    GFNT_Strike strike{};
    ASSERT_EQ(gfnt_face_strike_at(fixture.face, 0, &strike, nullptr), GFNT_OK)
        << entry.name;
    EXPECT_EQ(strike.ppem_x, kRows) << entry.name;
    EXPECT_EQ(strike.ppem_y, kRows) << entry.name;
    EXPECT_EQ(strike.bit_depth, 1) << entry.name;
    EXPECT_EQ(strike.kind, GFNT_GLYPH_BITMAP_MONO) << entry.name;
    if (entry.states) {
      EXPECT_EQ(strike.ascent, kAscent) << entry.name;
      EXPECT_EQ(strike.descent, kDescent) << entry.name;
    }
    else {
      // The box, reported where it is: the whole height above the line, nothing
      // below it. Not Unifont's 14 and 2, which is Unifont's and not the format's.
      EXPECT_EQ(strike.ascent, static_cast<int32_t>(kRows)) << entry.name;
      EXPECT_EQ(strike.descent, 0) << entry.name;
    }

    GFNT_BitmapGlyph bitmap{};
    ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 2, 0, &bitmap, nullptr),
        GFNT_OK) << entry.name;
    EXPECT_EQ(bitmap.bearing_y, strike.ascent) << entry.name;
    EXPECT_EQ(bitmap.bearing_x, 0) << entry.name;
    EXPECT_EQ(bitmap.advance, static_cast<int32_t>(kWidth)) << entry.name;
  }
}

TEST(Bitmap, EveryContainerIsItsOwnFlavourAndHasNoOutlines) {
  struct Case {
    const char * name;
    GFNT_Tag flavour;
  };
  const std::vector<Case> cases = {
      {"bitmap.hex", GFNT_FLAVOUR_HEX},
      {"bitmap-wide.hex", GFNT_FLAVOUR_HEX},
      {"bitmap.psf", GFNT_FLAVOUR_PSF},
      {"bitmap-v1.psf", GFNT_FLAVOUR_PSF},
      {"bitmap.bdf", GFNT_FLAVOUR_BDF},
      {"bitmap-ink.bdf", GFNT_FLAVOUR_BDF},
      {"bitmap.pcf", GFNT_FLAVOUR_PCF},
      {"bitmap-lsb.pcf", GFNT_FLAVOUR_PCF},
  };
  for (const Case & entry : cases) {
    Fixture fixture(entry.name);
    ASSERT_EQ(fixture.result, GFNT_OK) << entry.name;

    EXPECT_EQ(gfnt_face_flavour(fixture.face), entry.flavour) << entry.name;
    EXPECT_FALSE(gfnt_face_has_outlines(fixture.face)) << entry.name;

    size_t strikes = 0;
    EXPECT_EQ(gfnt_face_strike_count(fixture.face, &strikes, nullptr), GFNT_OK)
        << entry.name;
    EXPECT_EQ(strikes, 1u) << entry.name;
  }
}

// ------------------------------------------------------------ what each says

TEST(Bitmap, PcfCarriesOneEntryPerTableItCanRead) {
  // The multi-entry synthetic directory (design.md section 7.1): a PCF's table of
  // contents is a list of extents, so each becomes a directory entry and each
  // table parse gets a reader over itself. A single entry spanning the file would
  // have worked and would have given every table the whole file to read.
  Fixture fixture("bitmap.pcf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  EXPECT_TRUE(gfnt_face_has_table(fixture.face, GFNT_TAG('T', 'O', 'C', ' ')));
  EXPECT_TRUE(gfnt_face_has_table(fixture.face, GFNT_TAG('M', 'T', 'R', 'C')));
  EXPECT_TRUE(gfnt_face_has_table(fixture.face, GFNT_TAG('B', 'M', 'A', 'P')));
  EXPECT_TRUE(gfnt_face_has_table(fixture.face, GFNT_TAG('E', 'N', 'C', 'O')));
  EXPECT_TRUE(gfnt_face_has_table(fixture.face, GFNT_TAG('G', 'N', 'A', 'M')));
  EXPECT_TRUE(gfnt_face_has_table(fixture.face, GFNT_TAG('P', 'R', 'O', 'P')));
  // Absent from this one and present in the other, which is how the fixture pair
  // shows that a table the reader never asks for is skipped rather than stumbled
  // over.
  EXPECT_FALSE(gfnt_face_has_table(fixture.face, GFNT_TAG('I', 'M', 'T', 'R')));

  Fixture other("bitmap-lsb.pcf");
  ASSERT_EQ(other.result, GFNT_OK);
  EXPECT_TRUE(gfnt_face_has_table(other.face, GFNT_TAG('I', 'M', 'T', 'R')));

  // Every entry is bounded by the file, which is what the directory is for. A
  // table whose extent left the blob would have been refused at load.
  size_t offset = 0;
  size_t length = 0;
  ASSERT_EQ(gfnt_face_table_range(fixture.face, GFNT_TAG('B', 'M', 'A', 'P'),
      &offset, &length), GFNT_OK);
  EXPECT_GT(length, 0u);
}

TEST(Bitmap, HexWidthComesFromTheDigitCount) {
  // Nothing in a `.hex` line states a width: the number of digits is the width,
  // and a reader that assumed eight pixels would read a CJK page as rubbish.
  Fixture fixture("bitmap-wide.hex");
  ASSERT_EQ(fixture.result, GFNT_OK);

  const std::vector<uint32_t> widths = {8, 16, 32};
  for (uint32_t glyph = 0; glyph < widths.size(); ++glyph) {
    GFNT_BitmapGlyph bitmap{};
    ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, glyph, 0, &bitmap, nullptr),
        GFNT_OK) << glyph;
    EXPECT_EQ(bitmap.width, widths[glyph]) << glyph;
    EXPECT_EQ(bitmap.height, kRows) << glyph;
    EXPECT_EQ(bitmap.advance, static_cast<int32_t>(widths[glyph])) << glyph;
    EXPECT_EQ(bitmap.stride, widths[glyph] / 8u) << glyph;
  }
  // A six-digit codepoint, which is how every astral-plane glyph is written.
  uint32_t glyph = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 0x20000, &glyph,
      nullptr), GFNT_OK);
  EXPECT_EQ(glyph, 2u);
  // The 16- and 32-pixel glyphs set their outermost columns and nothing between,
  // so a scan that lost a byte would show here.
  GFNT_BitmapGlyph wide{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 1, 0, &wide, nullptr), GFNT_OK);
  for (uint32_t y = 0; y < kRows; ++y) {
    EXPECT_EQ(gfnt_bitmap_pixel(&wide, 0, y), 255u) << y;
    EXPECT_EQ(gfnt_bitmap_pixel(&wide, 15, y), 255u) << y;
    EXPECT_EQ(gfnt_bitmap_pixel(&wide, 7, y), 0u) << y;
    EXPECT_EQ(gfnt_bitmap_pixel(&wide, 8, y), 0u) << y;
  }
}

TEST(Bitmap, PsfMapsSeveralCodepointsToOneCellAndWalksPastSequences) {
  // One cell can stand for several characters, and for a *sequence* of them. A
  // sequence is not a codepoint and cannot be in a codepoint map, so it has to be
  // read past - refusing the font over one would refuse fonts the console draws.
  for (const char * name : {"bitmap.psf", "bitmap-v1.psf"}) {
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    uint32_t glyph = GFNT_GLYPH_NONE;
    ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 'A', &glyph, nullptr),
        GFNT_OK) << name;
    EXPECT_EQ(glyph, 2u) << name;
    // The second character of the same cell: a Greek capital alpha, which looks
    // like an A and in this font is one.
    ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 0x0391, &glyph,
        nullptr), GFNT_OK) << name;
    EXPECT_EQ(glyph, 2u) << name;
    // The combining acute of the sequence is **not** a mapping of its own.
    ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 0x0301, &glyph,
        nullptr), GFNT_OK) << name;
    EXPECT_EQ(glyph, 0u) << name << ": a sequence's members were mapped singly";
  }
}

TEST(Bitmap, Psf1CountComesFromOneBitOfTheModeByte) {
  // Version 1 states no glyph count anywhere else, and for a height of eight the
  // file would be the same length either way.
  Fixture fixture("bitmap-v1.psf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(fixture.face, &glyphs, nullptr), GFNT_OK);
  EXPECT_EQ(glyphs, 512u);

  // And the glyphs past the shared design are there, which is what says the count
  // was believed rather than clamped to what the first 256 cells hold.
  GFNT_BitmapGlyph bitmap{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 511, 0, &bitmap, nullptr),
      GFNT_OK);
  EXPECT_EQ(bitmap.width, kWidth);
  EXPECT_EQ(bitmap.height, kRows);
  EXPECT_EQ(gfnt_face_glyph_bitmap(fixture.face, 512, 0, &bitmap, nullptr),
      GFNT_ERR_INVALID);
}

TEST(Bitmap, BdfStatesABoxAndAnAdvancePerGlyph) {
  // What only the two per-glyph formats can say. Each of these is a real file
  // somewhere and a different way to be wrong.
  Fixture fixture("bitmap-ink.bdf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(fixture.face, &glyphs, nullptr), GFNT_OK);
  ASSERT_EQ(glyphs, 5u);

  struct Expected {
    uint32_t glyph;
    uint32_t width;
    uint32_t height;
    int32_t bearing_x;
    int32_t bearing_y;
    int32_t advance;
  };
  const std::vector<Expected> cases = {
      // An empty box with no BITMAP rows at all: a space, and its advance is
      // still its own.
      {0, 0, 0, 0, 0, 8},
      // Hangs two pixels left of the pen, and is wider than one byte.
      {1, 12, 14, -2, 10, 10},
      // A descender: BBX y is -4, so the box's *top* is at 8 and its bottom is
      // four rows below the baseline. Reading BBX y as the top would report 12.
      {2, 8, 12, 1, 8, 9},
      // An advance wider than the glyph, which is what a spacing accent has.
      {3, 4, 4, 0, 14, 12},
      {4, 8, 8, 0, 8, 8},
  };
  for (const Expected & entry : cases) {
    GFNT_BitmapGlyph bitmap{};
    ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, entry.glyph, 0, &bitmap,
        nullptr), GFNT_OK) << entry.glyph;
    EXPECT_EQ(bitmap.width, entry.width) << entry.glyph;
    EXPECT_EQ(bitmap.height, entry.height) << entry.glyph;
    EXPECT_EQ(bitmap.bearing_x, entry.bearing_x) << entry.glyph;
    EXPECT_EQ(bitmap.bearing_y, entry.bearing_y) << entry.glyph;
    EXPECT_EQ(bitmap.advance, entry.advance) << entry.glyph;
    if (entry.height == 0) {
      EXPECT_EQ(bitmap.bits, nullptr) << entry.glyph;
      EXPECT_EQ(bitmap.stride, 0u) << entry.glyph;
    }
  }

  // ENCODING -1: a glyph with no character. It must be a glyph - dropping it
  // renumbers every glyph after it - and it must not be in the map.
  size_t mappings = 0;
  ASSERT_EQ(gfnt_face_bitmap_encoding_count(fixture.face, &mappings, nullptr),
      GFNT_OK);
  EXPECT_EQ(mappings, 4u) << "the unencoded glyph was given a codepoint";
  for (size_t i = 0; i < mappings; ++i) {
    uint32_t glyph = GFNT_GLYPH_NONE;
    ASSERT_EQ(gfnt_face_bitmap_encoding_at(fixture.face, i, nullptr, &glyph,
        nullptr), GFNT_OK);
    EXPECT_NE(glyph, 4u);
  }
  char * name = nullptr;
  ASSERT_EQ(gfnt_face_glyph_name(fixture.face, 4, nullptr, &name, nullptr,
      nullptr), GFNT_OK);
  EXPECT_STREQ(name, "unencoded");
  gfnt_glyph_name_free(nullptr, name);
}

TEST(Bitmap, TheEncodingIsEnumerableAndSortedByCodepoint) {
  // A `cmap` can be enumerated and these formats otherwise could not be, which is
  // the one thing bitmap.h adds beyond the pixels.
  Fixture fixture("bitmap.pcf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  size_t count = 0;
  ASSERT_EQ(gfnt_face_bitmap_encoding_count(fixture.face, &count, nullptr),
      GFNT_OK);
  ASSERT_EQ(count, kGlyphs);

  uint32_t previous = 0;
  for (size_t i = 0; i < count; ++i) {
    uint32_t codepoint = 0;
    uint32_t glyph = GFNT_GLYPH_NONE;
    ASSERT_EQ(gfnt_face_bitmap_encoding_at(fixture.face, i, &codepoint, &glyph,
        nullptr), GFNT_OK) << i;
    if (i > 0) {
      EXPECT_GT(codepoint, previous) << i;
    }
    previous = codepoint;
    // And every pair round-trips through the lookup, which is the other half of
    // the same table.
    uint32_t looked_up = GFNT_GLYPH_NONE;
    ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, codepoint, &looked_up,
        nullptr), GFNT_OK) << i;
    EXPECT_EQ(looked_up, glyph) << i;
  }
  EXPECT_EQ(gfnt_face_bitmap_encoding_at(fixture.face, count, nullptr, nullptr,
      nullptr), GFNT_ERR_INVALID);

  // A codepoint the font does not have is glyph 0, as every cmap subtable
  // answers: a caller looping over a string cannot be asked to treat one
  // container's misses as errors and another's as zero.
  uint32_t missing = 99;
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 0x4E00, &missing,
      nullptr), GFNT_OK);
  EXPECT_EQ(missing, 0u);
}

TEST(Bitmap, NamesComeFromTheContainersThatStateThem) {
  // BDF names every character and PCF compiles those names into a table; PSF and
  // `.hex` state none at all, and the refusal says which of the two it is.
  for (const char * name : {"bitmap.bdf", "bitmap.pcf", "bitmap-lsb.pcf"}) {
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    char * text = nullptr;
    ASSERT_EQ(gfnt_face_glyph_name(fixture.face, 2, nullptr, &text, nullptr,
        nullptr), GFNT_OK) << name;
    EXPECT_STREQ(text, "A") << name;
    gfnt_glyph_name_free(nullptr, text);

    uint32_t glyph = GFNT_GLYPH_NONE;
    ASSERT_EQ(gfnt_face_glyph_for_name(fixture.face, "checker", &glyph, nullptr),
        GFNT_OK) << name;
    EXPECT_EQ(glyph, 7u) << name;
    EXPECT_EQ(gfnt_face_glyph_for_name(fixture.face, "nosuchglyph", &glyph,
        nullptr), GFNT_ERR_INVALID) << name;
  }
  for (const char * name : {"bitmap.hex", "bitmap.psf"}) {
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    char * text = nullptr;
    GFNT_Error error{};
    EXPECT_EQ(gfnt_face_glyph_name(fixture.face, 2, nullptr, &text, nullptr,
        &error), GFNT_ERR_UNSUPPORTED) << name;
    EXPECT_NE(std::string(error.message).find("states no glyph names"),
        std::string::npos) << name << ": " << error.message;
  }
}

TEST(Bitmap, TheFontsOwnNamesComeFromItsProperties) {
  // BDF states them as properties and PCF compiles the same properties into a
  // table, so both answer `name.h` - and the bytes are Latin-1, which is what
  // XLFD says a property string is.
  for (const char * name : {"bitmap.bdf", "bitmap.pcf"}) {
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    char * family = nullptr;
    ASSERT_EQ(gfnt_face_name(fixture.face, GFNT_NAME_FAMILY, GFNT_LANGUAGE_ANY,
        nullptr, &family, nullptr, nullptr), GFNT_OK) << name;
    EXPECT_STREQ(family, "Ghoti Fixture Bitmap") << name;
    gfnt_name_free(nullptr, family);

    char * copyright = nullptr;
    ASSERT_EQ(gfnt_face_name(fixture.face, GFNT_NAME_COPYRIGHT,
        GFNT_LANGUAGE_ANY, nullptr, &copyright, nullptr, nullptr), GFNT_OK)
        << name;
    EXPECT_NE(std::string(copyright).find("LGPL-3.0-only"), std::string::npos)
        << name;
    gfnt_name_free(nullptr, copyright);

    // The XLFD name answers both "the full name" and "the PostScript name": it is
    // the one string that names the whole font, and a caller asking for the name
    // of this font should not be refused because the font is not PostScript.
    for (uint16_t id : {GFNT_NAME_FULL, GFNT_NAME_POSTSCRIPT}) {
      char * full = nullptr;
      ASSERT_EQ(gfnt_face_name(fixture.face, id, GFNT_LANGUAGE_ANY, nullptr,
          &full, nullptr, nullptr), GFNT_OK) << name << " id " << id;
      EXPECT_EQ(std::string(full).rfind("-Ghoti.io-", 0), 0u) << full;
      gfnt_name_free(nullptr, full);
    }

    // A name id neither format has a property for.
    char * version = nullptr;
    GFNT_Error error{};
    EXPECT_EQ(gfnt_face_name(fixture.face, GFNT_NAME_VERSION, GFNT_LANGUAGE_ANY,
        nullptr, &version, nullptr, &error), GFNT_ERR_UNSUPPORTED) << name;
  }
  // The two formats with nowhere to put a name at all.
  for (const char * name : {"bitmap.hex", "bitmap.psf"}) {
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    char * family = nullptr;
    EXPECT_EQ(gfnt_face_name(fixture.face, GFNT_NAME_FAMILY, GFNT_LANGUAGE_ANY,
        nullptr, &family, nullptr, nullptr), GFNT_ERR_UNSUPPORTED) << name;
  }
}

// ------------------------------------------------------------- the refusals

TEST(Bitmap, FontUnitMetricsAreRefusedRatherThanAnsweredInPixels) {
  // The unit error this refusal exists to prevent: these functions' unit is font
  // units, a strike's advance is pixels, and a caller handed one for the other
  // lays out text at whatever ratio the em happened to be.
  Fixture fixture("bitmap.pcf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  uint16_t upem = 0;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_units_per_em(fixture.face, &upem, &error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::string(error.message).find("no em"), std::string::npos)
      << error.message;

  int32_t advance = 0;
  EXPECT_EQ(gfnt_face_glyph_advance(fixture.face, 2, nullptr, &advance, &error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::string(error.message).find("pixels"), std::string::npos)
      << error.message;

  int32_t bearing = 0;
  EXPECT_EQ(gfnt_face_glyph_side_bearing(fixture.face, 2, nullptr, &bearing,
      &error), GFNT_ERR_UNSUPPORTED);

  // And the pixels the refusal points at are there.
  GFNT_BitmapGlyph bitmap{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 2, 0, &bitmap, nullptr),
      GFNT_OK);
  EXPECT_EQ(bitmap.advance, static_cast<int32_t>(kWidth));
}

TEST(Bitmap, StrikePoliciesResolveAgainstTheOneStrike) {
  Fixture fixture("bitmap.pcf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  GFNT_Strike strike{};
  bool from_outlines = true;
  GFNT_Error error{};

  // OUTLINES_ONLY is the policy that cannot surprise a caller who never heard of
  // strikes, and on a font that is nothing but a strike it has to fail.
  EXPECT_EQ(gfnt_face_select_strike(fixture.face, 16, GFNT_STRIKE_OUTLINES_ONLY,
      &strike, &from_outlines, &error), GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::string(error.message).find("outlines only"), std::string::npos)
      << error.message;

  for (GFNT_StrikePolicy policy : {GFNT_STRIKE_EXACT, GFNT_STRIKE_NEAREST,
      GFNT_STRIKE_PREFER_STRIKE}) {
    from_outlines = true;
    ASSERT_EQ(gfnt_face_select_strike(fixture.face, 16, policy, &strike,
        &from_outlines, nullptr), GFNT_OK) << policy;
    EXPECT_FALSE(from_outlines) << policy;
    EXPECT_EQ(strike.ppem_y, kRows) << policy;
  }

  // EXACT is the one policy that can refuse a strike this face has, which is the
  // whole reason it exists beside NEAREST: a font drawn at sixteen pixels cannot
  // answer for thirteen, and M9 is what pretending otherwise looks like.
  EXPECT_EQ(gfnt_face_select_strike(fixture.face, 13, GFNT_STRIKE_EXACT, &strike,
      &from_outlines, nullptr), GFNT_ERR_UNSUPPORTED);
  ASSERT_EQ(gfnt_face_select_strike(fixture.face, 13, GFNT_STRIKE_NEAREST,
      &strike, &from_outlines, nullptr), GFNT_OK);
  EXPECT_EQ(strike.ppem_y, kRows);

  EXPECT_EQ(gfnt_face_strike_at(fixture.face, 1, &strike, nullptr),
      GFNT_ERR_INVALID);
  // A strike index a one-strike font does not have, which is every index but 0.
  GFNT_BitmapGlyph second{};
  EXPECT_EQ(gfnt_face_glyph_bitmap(fixture.face, 2, 1, &second, &error),
      GFNT_ERR_INVALID);
  EXPECT_NE(std::string(error.message).find("exactly one strike"),
      std::string::npos) << error.message;
}

TEST(Bitmap, AnOutlineFaceHasNoBitmapsAndSaysSo) {
  // The distinction glyph.h exists to keep: "this font has no strikes" and "this
  // library cannot read this font's strikes" are both UNSUPPORTED and the
  // diagnostic separates them.
  Fixture fixture("basic.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  GFNT_BitmapGlyph bitmap{};
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_bitmap(fixture.face, 1, 0, &bitmap, &error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::string(error.message).find("no bitmap strikes at all"),
      std::string::npos) << error.message;

  size_t count = 0;
  EXPECT_EQ(gfnt_face_bitmap_encoding_count(fixture.face, &count, &error),
      GFNT_ERR_UNSUPPORTED);
}

// --------------------------------------------------------- the coverage bridge

TEST(Bitmap, AStrikeBecomesCoverageWithoutBeingScaled) {
  // So that a caller compositing a run needs one code path. The bearings are the
  // coverage's origin, which is the whole reason the conversion is three lines.
  Fixture fixture("bitmap.bdf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  GFNT_BitmapGlyph bitmap{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 2, 0, &bitmap, nullptr),
      GFNT_OK);

  GFNT_Coverage coverage{};
  ASSERT_EQ(gfnt_coverage_from_bitmap(&bitmap, nullptr, &coverage, nullptr),
      GFNT_OK);
  EXPECT_EQ(coverage.width, bitmap.width);
  EXPECT_EQ(coverage.height, bitmap.height);
  EXPECT_EQ(coverage.left, bitmap.bearing_x);
  EXPECT_EQ(coverage.top, bitmap.bearing_y);

  size_t set = 0;
  for (uint32_t y = 0; y < bitmap.height; ++y) {
    for (uint32_t x = 0; x < bitmap.width; ++x) {
      const uint8_t pixel = gfnt_bitmap_pixel(&bitmap, x, y);

      EXPECT_EQ(gfnt_coverage_at(&coverage, x, y), pixel) << x << "," << y;
      if (pixel) {
        ++set;
      }
    }
  }
  // A 1-bit strike is either 0 or 255, so the total is the pixel count times 255
  // exactly - no partial coverage anywhere, which is the difference from a
  // rasterised outline.
  EXPECT_EQ(gfnt_coverage_total(&coverage), static_cast<uint64_t>(set) * 255u);
  gfnt_coverage_destroy(&coverage);

  // And the containers' coverages hash alike *within* a baseline group. The
  // hash covers `left` and `top`, so the two groups deliberately differ: BDF and
  // PCF put the box's top at 14 and PSF and `.hex` at 16, because the second pair
  // states no baseline. That is the documented consequence of the silence and not
  // a disagreement about pixels - the pixel comparison above is what says the
  // rows are identical.
  struct Group {
    std::vector<std::string> names;
    bool states_baseline;
  };
  const std::vector<Group> groups = {
      {{"bitmap.bdf", "bitmap.pcf", "bitmap-lsb.pcf"}, true},
      {{"bitmap.hex", "bitmap.psf"}, false},
  };
  std::vector<uint64_t> per_group;
  for (const Group & group : groups) {
    std::vector<uint64_t> hashes;
    for (const std::string & name : group.names) {
      Fixture each(name);
      ASSERT_EQ(each.result, GFNT_OK) << name;
      uint64_t combined = 0;
      for (uint32_t glyph = 0; glyph < kGlyphs; ++glyph) {
        GFNT_BitmapGlyph one{};
        GFNT_Coverage converted{};
        ASSERT_EQ(gfnt_face_glyph_bitmap(each.face, glyph, 0, &one, nullptr),
            GFNT_OK) << name;
        ASSERT_EQ(gfnt_coverage_from_bitmap(&one, nullptr, &converted, nullptr),
            GFNT_OK) << name;
        EXPECT_EQ(converted.top, group.states_baseline ? kAscent
            : static_cast<int32_t>(kRows)) << name << " glyph " << glyph;
        combined = combined * 31u + gfnt_coverage_hash(&converted);
        gfnt_coverage_destroy(&converted);
      }
      hashes.push_back(combined);
    }
    for (size_t i = 1; i < hashes.size(); ++i) {
      EXPECT_EQ(hashes[0], hashes[i]) << group.names[i];
    }
    per_group.push_back(hashes[0]);
  }
  // And the two groups are *not* equal, which is the control: if this passed as
  // well, the position would not be in the hash and the check above would be
  // comparing pixels twice.
  EXPECT_NE(per_group[0], per_group[1]);
}


// ------------------------------------------------- crafted refusals: builders

/** A little-endian 32-bit number, which is what PCF's own integers are. */
void put32(std::string & out, uint32_t value, bool lsb = true) {
  for (unsigned i = 0; i < 4; ++i) {
    const unsigned shift = lsb ? (8 * i) : (8 * (3 - i));

    out += static_cast<char>((value >> shift) & 0xFF);
  }
}

void put16(std::string & out, uint16_t value, bool lsb = true) {
  for (unsigned i = 0; i < 2; ++i) {
    const unsigned shift = lsb ? (8 * i) : (8 * (1 - i));

    out += static_cast<char>((value >> shift) & 0xFF);
  }
}

/**
 * The smallest PCF this library will read, with each table separately editable.
 *
 * Hand-built rather than generated, for the reason `tests/sfnt_builder.h` gives:
 * the refusal arms are bytes no writer emits, so a fixture generator cannot make
 * them. One glyph, 8 by 8, most-significant bit and byte first.
 */
struct PcfBuilder {
  // Bits most-significant first, bytes **least**, one-byte rows, a one-byte scan
  // unit. Little-endian content is what `put32` below writes, and this is the
  // combination that needs no byte swap - the swapping cases are the fixture
  // pair's, where the bytes come from the generator rather than from here.
  uint32_t format = 0x08;
  uint32_t metrics_format = 0x08;
  uint32_t metrics_count = 1;
  uint32_t bitmaps_count = 1;
  uint32_t names_count = 1;
  bool with_names = true;
  bool with_metrics = true;
  bool with_bitmaps = true;
  bool with_encodings = true;
  uint32_t toc_format_override = 0;  // 0: use each table's own
  uint32_t bitmap_offset = 0;
  uint32_t block_size_override = 0;
  uint16_t encoding_glyph = 0;
  uint16_t encoding_min = 0x41;
  uint16_t encoding_max = 0x41;
  int16_t left = 0;
  int16_t right = 8;
  int16_t ascent = 8;
  int16_t descent = 0;
  uint32_t toc_count_override = 0;

  std::string build() const {
    std::vector<std::pair<uint32_t, std::string>> tables;

    if (with_metrics) {
      std::string payload;
      put32(payload, metrics_format);
      put32(payload, metrics_count);
      for (uint32_t i = 0; i < metrics_count; ++i) {
        put16(payload, static_cast<uint16_t>(left));
        put16(payload, static_cast<uint16_t>(right));
        put16(payload, 8);
        put16(payload, static_cast<uint16_t>(ascent));
        put16(payload, static_cast<uint16_t>(descent));
        put16(payload, 0);
      }
      tables.push_back({4, payload});
    }
    if (with_bitmaps) {
      std::string payload;
      const uint32_t rows = static_cast<uint32_t>(ascent + descent);
      put32(payload, format);
      put32(payload, bitmaps_count);
      for (uint32_t i = 0; i < bitmaps_count; ++i) {
        put32(payload, bitmap_offset);
      }
      const uint32_t block = block_size_override ? block_size_override
          : rows * bitmaps_count;
      for (unsigned pad = 0; pad < 4; ++pad) {
        put32(payload, block * (1u << pad));
      }
      payload.append(block, '\x81');
      tables.push_back({8, payload});
    }
    if (with_encodings) {
      std::string payload;
      put32(payload, format);
      put16(payload, encoding_min);
      put16(payload, encoding_max);
      put16(payload, 0);
      put16(payload, 0);
      put16(payload, 0xFFFF);
      for (uint32_t code = encoding_min; code <= encoding_max; ++code) {
        put16(payload, encoding_glyph);
      }
      tables.push_back({32, payload});
    }
    if (with_names) {
      std::string payload;
      put32(payload, format);
      put32(payload, names_count);
      for (uint32_t i = 0; i < names_count; ++i) {
        put32(payload, 0);
      }
      put32(payload, 2);
      payload += std::string("A", 1) + std::string(1, '\0');
      tables.push_back({128, payload});
    }

    std::string out = "\x01" "fcp";
    put32(out, toc_count_override ? toc_count_override
        : static_cast<uint32_t>(tables.size()));
    uint32_t at = static_cast<uint32_t>(8 + 16 * tables.size());
    std::string body;
    for (const auto & table : tables) {
      std::string payload = table.second;
      payload.append((4 - payload.size() % 4) % 4, '\0');
      put32(out, table.first);
      put32(out, toc_format_override ? toc_format_override
          : (table.first == 4 ? metrics_format : format));
      put32(out, static_cast<uint32_t>(payload.size()));
      put32(out, at);
      at += static_cast<uint32_t>(payload.size());
      body += payload;
    }
    return out + body;
  }
};

/** A BDF with every keyword a valid one needs, so a test can spoil just one. */
struct BdfBuilder {
  std::string font = "-ghoti-test-medium-r-normal--8-80-75-75-c-80-iso10646-1";
  std::string box = "8 8 0 0";
  bool with_box = true;
  bool with_endfont = true;
  std::string chars = "1";
  bool with_chars = true;
  std::string glyph_name = "A";
  std::string encoding = "65";
  std::string bbx = "8 8 0 0";
  bool with_bbx = true;
  bool with_dwidth = true;
  bool with_bitmap = true;
  std::vector<std::string> rows = {"81", "42", "24", "18", "18", "24", "42",
      "81"};
  std::string extra;

  std::string build() const {
    std::string out = "STARTFONT 2.1\nFONT " + font + "\nSIZE 8 75 75\n";
    if (with_box) {
      out += "FONTBOUNDINGBOX " + box + "\n";
    }
    out += "STARTPROPERTIES 2\nFONT_ASCENT 8\nFONT_DESCENT 0\n"
           "ENDPROPERTIES\n";
    if (with_chars) {
      out += "CHARS " + chars + "\n";
    }
    out += "STARTCHAR " + glyph_name + "\nENCODING " + encoding + "\n";
    if (with_dwidth) {
      out += "DWIDTH 8 0\n";
    }
    if (with_bbx) {
      out += "BBX " + bbx + "\n";
    }
    out += extra;
    if (with_bitmap) {
      out += "BITMAP\n";
      for (const std::string & row : rows) {
        out += row + "\n";
      }
    }
    out += "ENDCHAR\n";
    if (with_endfont) {
      out += "ENDFONT\n";
    }
    return out;
  }
};

/** A PSF 2 with one 8x8 glyph. */
std::string psf2(uint32_t flags = 0, uint32_t version = 0,
    uint32_t header_size = 32, uint32_t count = 1, uint32_t charsize = 8,
    uint32_t height = 8, uint32_t width = 8,
    const std::string & table = std::string()) {
  std::string out;
  put32(out, 0x864AB572);
  put32(out, version);
  put32(out, header_size);
  put32(out, flags);
  put32(out, count);
  put32(out, charsize);
  put32(out, height);
  put32(out, width);
  out.append(header_size > 32 ? header_size - 32 : 0, '\0');
  out.append(static_cast<size_t>(count) * charsize, '\x5A');
  out += table;
  return out;
}

/** Every refusal, as (bytes, what the message must mention). */
struct Refusal {
  std::string bytes;
  const char * because;
  GFNT_Result result = GFNT_ERR_CORRUPT;
};

void expect_refusals(const std::vector<Refusal> & cases,
    const GFNT_Limits * limits = nullptr) {
  for (const Refusal & entry : cases) {
    Crafted crafted(entry.bytes, limits);

    EXPECT_EQ(crafted.result, entry.result) << entry.because;
    if (crafted.result != GFNT_OK) {
      EXPECT_NE(std::string(crafted.error.message).find(entry.because),
          std::string::npos)
          << "expected a message mentioning \"" << entry.because
          << "\", got \"" << crafted.error.message << "\"";
    }
  }
}

// --------------------------------------------------------- crafted refusals

TEST(Bitmap, PaddingBitsPastTheWidthAreCleared) {
  // Two faces of one design have to compare equal, and a container is entitled to
  // leave rubbish in the bits past the last pixel of a row - BDF calls them
  // undefined. Eight pixels is a whole byte, so the shared design cannot show
  // this; the ink fixture's 12-pixel glyph has a first row that sets all four.
  //
  // **This test could not fail until that row existed.** Deleting the masking in
  // `bitmap.c` left all twenty-eight tests passing, because every row of every
  // fixture already had zeros there. The fixture is the control.
  Fixture fixture("bitmap-ink.bdf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  GFNT_BitmapGlyph bitmap{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 1, 0, &bitmap, nullptr),
      GFNT_OK);
  ASSERT_EQ(bitmap.width, 12u);
  ASSERT_EQ(bitmap.stride, 2u);
  // The row that carries them, first: twelve pixels set and four bits discarded.
  for (uint32_t x = 0; x < 12; ++x) {
    EXPECT_EQ(gfnt_bitmap_pixel(&bitmap, x, 0), 255u) << x;
  }
  for (uint32_t y = 0; y < bitmap.height; ++y) {
    EXPECT_EQ(bitmap.bits[y * bitmap.stride + 1] & 0x0F, 0)
        << "row " << y << " has bits set past the glyph's width";
  }

  // And a row written the same way by hand, so the assertion does not rest on one
  // fixture staying as it is.
  BdfBuilder builder;
  builder.bbx = "12 2 0 0";
  builder.box = "12 2 0 0";
  builder.rows = {"FFFF", "FFFF"};
  Crafted crafted(builder.build());
  ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;
  GFNT_BitmapGlyph crafted_glyph{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(crafted.face, 0, 0, &crafted_glyph, nullptr),
      GFNT_OK);
  EXPECT_EQ(crafted_glyph.bits[1] & 0x0F, 0);
  EXPECT_EQ(crafted_glyph.bits[3] & 0x0F, 0);
}

TEST(Bitmap, HexRefusesWhatIsNotItsGrammar) {
  // The probe is the grammar, so most malformations are "not a font at all"
  // rather than a corrupt one: a file whose first line is not a record was never
  // claimed by this container. The ones that reach ERR_CORRUPT are files whose
  // *first* line is a record and whose later ones are not - which is the only way
  // to be a broken `.hex` rather than something else entirely.
  const std::string good = "0041:00000000000000000000000000000000\n";

  expect_refusals({
      {good + "0042:0000\n", "sixteen rows of hexadecimal"},
      {good + "0043:0000000000000000000000000000000G\n",
          "sixteen rows of hexadecimal"},
      {good + "00430000000000000000000000000000000\n",
          "sixteen rows of hexadecimal"},
      // Ten rows' worth of digits: a multiple of two, not a multiple of 32.
      {good + "0044:00000000000000000000\n", "sixteen rows of hexadecimal"},
      // Wider than the format's widest, which is where a reader that trusted the
      // digit count would allocate whatever it was told.
      {good + "0045:" + std::string(160, '0') + "\n",
          "sixteen rows of hexadecimal"},
  });

  // And these are not `.hex` at all, so the face load reports the format rather
  // than a corruption inside one.
  for (const std::string & bytes : {std::string("hello world\n"),
      std::string("0041\n"), std::string("41:0000\n"), std::string("\n\n\n")}) {
    Crafted crafted(bytes);
    EXPECT_EQ(crafted.result, GFNT_ERR_FORMAT) << bytes;
  }
}

TEST(Bitmap, HexRefusesALineLongerThanTheLimit) {
  // A limit rather than a corruption: a long line is a legal thing for a text
  // file to contain and GFNT_Limits is what says this reader will not walk it.
  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_line_length = 40;

  Crafted crafted("0041:" + std::string(64, '0') + "\n", &limits);
  EXPECT_EQ(crafted.result, GFNT_ERR_LIMIT);
  EXPECT_NE(std::string(crafted.error.message).find("max_line_length"),
      std::string::npos) << crafted.error.message;
}

TEST(Bitmap, PsfRefusesAHeaderThatContradictsItself) {
  expect_refusals({
      // Version 1's mode byte, with a bit the format does not define.
      {std::string("\x36\x04\x40\x08", 4), "bits the format reserves"},
      {std::string("\x36\x04\x00\x00", 4), "no pixels in one direction"},
      // Version 2: a version number, a header that cannot hold the fields, and
      // the size that has to agree with the height and width.
      {psf2(0, 1), "version this library does not read",
          GFNT_ERR_UNSUPPORTED},
      {psf2(0, 0, 16), "smaller than the fields"},
      {psf2(0, 0, 32, 1, 9), "height times its width"},
      {psf2(0, 0, 32, 1, 0, 0), "no pixels in one direction"},
      // The count is believed and then checked against what the file holds.
      {psf2(0, 0, 32, 4).substr(0, 32 + 8), "ends before the glyph"},
  });
}

TEST(Bitmap, PsfRefusesAUnicodeTableItCannotRead) {
  expect_refusals({
      // An entry with no terminator at all: the table ends mid-glyph.
      {psf2(1, 0, 32, 1, 8, 8, 8, std::string("A")),
          "ends before its terminator"},
      // A continuation byte with no lead byte.
      {psf2(1, 0, 32, 1, 8, 8, 8, std::string("\x80\xff", 2)),
          "bytes that are not UTF-8"},
      // U+0041 written in two bytes. An overlong form maps a second sequence to
      // one character, and then which glyph a lookup finds depends on which was
      // written.
      {psf2(1, 0, 32, 1, 8, 8, 8, std::string("\xc1\x81\xff", 3)),
          "bytes that are not UTF-8"},
      // A surrogate, which is not a character.
      {psf2(1, 0, 32, 1, 8, 8, 8, std::string("\xed\xa0\x80\xff", 4)),
          "bytes that are not UTF-8"},
  });

  // And the version 1 spelling of the same defect.
  Crafted unterminated(std::string("\x36\x04\x02\x08", 4)
      + std::string(256 * 8, '\0') + std::string("\x41\x00", 2));
  EXPECT_EQ(unterminated.result, GFNT_ERR_CORRUPT);
  EXPECT_NE(std::string(unterminated.error.message).find("terminator"),
      std::string::npos) << unterminated.error.message;
}

TEST(Bitmap, PsfWithoutAUnicodeTableStatesNoCharacters) {
  // Its glyph indices are positions in a console's character generator. Mapping
  // index to codepoint would invent a font that claims to hold U+0001.
  Crafted crafted(psf2());
  ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;

  size_t count = 0;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_bitmap_encoding_count(crafted.face, &count, &error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::string(error.message).find("states no mapping"),
      std::string::npos) << error.message;

  uint32_t glyph = GFNT_GLYPH_NONE;
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(crafted.face, 'A', &glyph, &error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::string(error.message).find("console"), std::string::npos)
      << error.message;

  // The glyph itself is still there: an unmapped font is readable by index, which
  // is how `setfont` uses one.
  GFNT_BitmapGlyph bitmap{};
  EXPECT_EQ(gfnt_face_glyph_bitmap(crafted.face, 0, 0, &bitmap, nullptr),
      GFNT_OK);
}

TEST(Bitmap, BdfRefusesEachMissingPieceByName) {
  {
    BdfBuilder builder;
    ASSERT_EQ(Crafted(builder.build()).result, GFNT_OK)
        << "the control does not parse, so every case below proves nothing";
  }
  std::vector<Refusal> cases;
  {
    BdfBuilder builder;
    builder.with_endfont = false;
    cases.push_back({builder.build(), "without ENDFONT"});
  }
  {
    BdfBuilder builder;
    builder.with_box = false;
    cases.push_back({builder.build(), "no FONTBOUNDINGBOX"});
  }
  {
    BdfBuilder builder;
    builder.chars = "4";
    cases.push_back({builder.build(), "than its CHARS states"});
  }
  {
    BdfBuilder builder;
    builder.with_bbx = false;
    builder.with_bitmap = false;
    cases.push_back({builder.build(), "no BBX"});
  }
  {
    BdfBuilder builder;
    builder.with_dwidth = false;
    cases.push_back({builder.build(), "no DWIDTH"});
  }
  {
    BdfBuilder builder;
    builder.bbx = "8 -8 0 0";
    cases.push_back({builder.build(), "negative size"});
  }
  {
    BdfBuilder builder;
    builder.bbx = "8 8";
    cases.push_back({builder.build(), "four numbers"});
  }
  {
    BdfBuilder builder;
    builder.rows.pop_back();
    cases.push_back({builder.build(), "fewer rows than its BBX"});
  }
  {
    BdfBuilder builder;
    builder.rows.push_back("FF");
    cases.push_back({builder.build(), "more rows than its BBX"});
  }
  {
    BdfBuilder builder;
    builder.rows[3] = "ZZ";
    cases.push_back({builder.build(), "not hexadecimal"});
  }
  {
    BdfBuilder builder;
    builder.rows[3] = "8";
    cases.push_back({builder.build(), "fewer digits than the glyph is wide"});
  }
  {
    BdfBuilder builder;
    builder.encoding = "";
    cases.push_back({builder.build(), "an ENCODING with no number"});
  }
  {
    BdfBuilder builder;
    builder.extra = "STARTCHAR B\n";
    cases.push_back({builder.build(), "never ended"});
  }
  {
    BdfBuilder builder;
    builder.with_bbx = false;
    cases.push_back({builder.build(), "before its BBX"});
  }
  expect_refusals(cases);

  // Not a BDF at all: the keyword has to be the first thing in the file, because
  // a scan that found STARTFONT further down would claim a shell script that
  // mentions one.
  EXPECT_EQ(Crafted("# a comment\nSTARTFONT 2.1\n").result, GFNT_ERR_FORMAT);
}

TEST(Bitmap, BdfAcceptsWhatTheSpecificationAllows) {
  // The other half of the refusals: three things that look like defects and are
  // not, each of which a stricter reader would refuse a real font over.
  {
    // A row padded to the font's bounding box rather than the glyph's, which is
    // what several writers emit.
    BdfBuilder builder;
    for (std::string & row : builder.rows) {
      row += "00";
    }
    Crafted crafted(builder.build());
    ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;
    GFNT_BitmapGlyph bitmap{};
    ASSERT_EQ(gfnt_face_glyph_bitmap(crafted.face, 0, 0, &bitmap, nullptr),
        GFNT_OK);
    EXPECT_EQ(bitmap.width, 8u);
  }
  {
    // CRLF line endings, and a lone CR, both of which are files that exist.
    BdfBuilder builder;
    std::string text = builder.build();
    std::string crlf;
    for (char c : text) {
      if (c == '\n') {
        crlf += '\r';
      }
      crlf += c;
    }
    EXPECT_EQ(Crafted(crlf).result, GFNT_OK) << "CRLF";
    std::string cr = text;
    for (char & c : cr) {
      if (c == '\n') {
        c = '\r';
      }
    }
    EXPECT_EQ(Crafted(cr).result, GFNT_OK) << "a lone CR";
  }
  {
    // A comment, and a property this library does not read.
    BdfBuilder builder;
    builder.extra = "COMMENT this glyph is a test\nSWIDTH 1000 0\n";
    EXPECT_EQ(Crafted(builder.build()).result, GFNT_OK);
  }
}

TEST(Bitmap, PcfRefusesATableThatDisagreesWithItsDirectory) {
  {
    PcfBuilder builder;
    Crafted control(builder.build());
    ASSERT_EQ(control.result, GFNT_OK)
        << "the control does not parse: " << control.error.message;
  }
  std::vector<Refusal> cases;
  {
    // The format word is written twice - once in the table of contents and once
    // at the head of the table - and it decides how every number after it is read.
    // A file where the two disagree is one where a reader would have to choose.
    PcfBuilder builder;
    builder.toc_format_override = 0x0C;
    cases.push_back({builder.build(), "disagrees with its directory entry"});
  }
  {
    PcfBuilder builder;
    builder.format = 0x08 | 0x30;  // an eight-byte scan unit
    builder.metrics_format = 0x08 | 0x30;
    cases.push_back({builder.build(), "scan unit of eight bytes"});
  }
  {
    PcfBuilder builder;
    builder.format = 0x08 | 0x00000400;
    builder.metrics_format = builder.format;
    cases.push_back({builder.build(), "format this library does not read",
        GFNT_ERR_UNSUPPORTED});
  }
  {
    PcfBuilder builder;
    builder.metrics_count = 0;
    cases.push_back({builder.build(), "no glyphs in it"});
  }
  {
    PcfBuilder builder;
    builder.bitmaps_count = 2;
    cases.push_back({builder.build(), "disagree about how many glyphs"});
  }
  {
    PcfBuilder builder;
    builder.names_count = 2;
    cases.push_back({builder.build(), "different number of glyphs than it has"});
  }
  {
    PcfBuilder builder;
    builder.encoding_glyph = 7;
    cases.push_back({builder.build(), "naming a glyph the font does not have"});
  }
  {
    PcfBuilder builder;
    builder.encoding_min = 0x50;
    builder.encoding_max = 0x41;
    cases.push_back({builder.build(), "range that runs backwards"});
  }
  {
    // An offset into the bitmap block, past its end. The block's own length is
    // the bound, and it is the one of four stated sizes that this format names.
    PcfBuilder builder;
    builder.bitmap_offset = 100;
    cases.push_back({builder.build(), "past the bitmap block"});
  }
  {
    PcfBuilder builder;
    builder.block_size_override = 4;
    cases.push_back({builder.build(), "past the bitmap block"});
  }
  {
    PcfBuilder builder;
    builder.left = 8;
    builder.right = 0;
    cases.push_back({builder.build(), "run backwards"});
  }
  {
    PcfBuilder builder;
    builder.toc_count_override = 1000;
    cases.push_back({builder.build(), "more tables than its own file can hold"});
  }
  expect_refusals(cases);

  // Without metrics there is no box for any bitmap, and without bitmaps there is
  // nothing to draw: both are "the font has no such table", which section 7.8
  // spells UNSUPPORTED.
  {
    PcfBuilder builder;
    builder.with_metrics = false;
    Crafted crafted(builder.build());
    EXPECT_EQ(crafted.result, GFNT_ERR_UNSUPPORTED) << crafted.error.message;
  }
  {
    PcfBuilder builder;
    builder.with_bitmaps = false;
    Crafted crafted(builder.build());
    EXPECT_EQ(crafted.result, GFNT_ERR_UNSUPPORTED) << crafted.error.message;
  }
  // A PCF with no encodings table is legal and is a font reached by index.
  {
    PcfBuilder builder;
    builder.with_encodings = false;
    Crafted crafted(builder.build());
    ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;
    size_t count = 0;
    EXPECT_EQ(gfnt_face_bitmap_encoding_count(crafted.face, &count, nullptr),
        GFNT_ERR_UNSUPPORTED);
  }
  // And one with no glyph names is legal too, and then names are refused per
  // glyph rather than the font being refused.
  {
    PcfBuilder builder;
    builder.with_names = false;
    Crafted crafted(builder.build());
    ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;
    char * name = nullptr;
    EXPECT_EQ(gfnt_face_glyph_name(crafted.face, 0, nullptr, &name, nullptr,
        nullptr), GFNT_ERR_UNSUPPORTED);
  }
  // Truncated at every length: a file that ends anywhere must be refused rather
  // than read past. This is section 14.3's idea applied to a container whose
  // tables are its own.
  {
    const std::string whole = PcfBuilder().build();
    for (size_t length = 1; length < whole.size(); ++length) {
      Crafted crafted(whole.substr(0, length));

      EXPECT_NE(crafted.result, GFNT_OK) << "accepted a PCF cut to " << length
                                        << " bytes";
    }
  }
}

TEST(Bitmap, LimitsAreEnforcedPerContainer) {
  GFNT_Limits limits;

  gfnt_limits_default(&limits);
  limits.max_glyphs = 4;
  {
    // PSF states its count in its header, so the limit is checked before an
    // allocation is made with the number.
    Crafted crafted(psf2(0, 0, 32, 8), &limits);
    EXPECT_EQ(crafted.result, GFNT_ERR_LIMIT);
    EXPECT_NE(std::string(crafted.error.message).find("max_glyphs"),
        std::string::npos) << crafted.error.message;
  }
  {
    // `.hex` states no count anywhere, so its limit is per glyph.
    std::string many;
    for (unsigned i = 0; i < 8; ++i) {
      many += "004" + std::to_string(i) + ":"
          + std::string(32, '0') + "\n";
    }
    Crafted crafted(many, &limits);
    EXPECT_EQ(crafted.result, GFNT_ERR_LIMIT);
  }
  {
    PcfBuilder builder;
    builder.metrics_count = 8;
    builder.bitmaps_count = 8;
    builder.names_count = 8;
    Crafted crafted(builder.build(), &limits);
    EXPECT_EQ(crafted.result, GFNT_ERR_LIMIT);
  }

  gfnt_limits_default(&limits);
  limits.max_ppem = 8;
  {
    // A glyph taller than the largest size this library will rasterise. The cap
    // is what keeps a claimed box from sizing an allocation.
    BdfBuilder builder;
    builder.bbx = "8 40 0 0";
    builder.rows.assign(40, "81");
    Crafted crafted(builder.build(), &limits);
    EXPECT_EQ(crafted.result, GFNT_ERR_LIMIT);
    EXPECT_NE(std::string(crafted.error.message).find("max_ppem"),
        std::string::npos) << crafted.error.message;
  }

  gfnt_limits_default(&limits);
  limits.max_raster_bytes = 4;
  {
    BdfBuilder builder;
    Crafted crafted(builder.build(), &limits);
    EXPECT_EQ(crafted.result, GFNT_ERR_LIMIT);
    EXPECT_NE(std::string(crafted.error.message).find("max_raster_bytes"),
        std::string::npos) << crafted.error.message;
  }
}

TEST(Bitmap, EveryAllocationCanFail) {
  // design.md section 15.1: an allocation failure is a code path, and the only
  // way to walk it is to refuse each request in turn. The four parsers allocate
  // different numbers of things - PCF alone allocates its directory, its metrics,
  // its name offsets and three arenas - so the sweep is per container and the
  // requirement is the same: no crash, no leak, and a refusal that says OOM.
  for (const char * name : {"bitmap.hex", "bitmap.psf", "bitmap.bdf",
      "bitmap.pcf", "bitmap-lsb.pcf", "bitmap-ink.bdf", "bitmap-v1.psf"}) {
    const std::string path = gfnttest::data(std::string("fonts/") + name);
    GFNT_Blob * blob = nullptr;
    GFNT_Error error{};

    ASSERT_EQ(gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &blob,
        &error), GFNT_OK) << name;

    size_t requests = 0;
    {
      gfnttest::FailingAllocator counter(static_cast<size_t>(-1));
      GFNT_Face * face = nullptr;

      ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, counter.get(), &face, &error),
          GFNT_OK) << name;
      gfnt_face_free(face);
      requests = counter.requests();
      EXPECT_EQ(counter.live(), 0u) << name;
    }
    ASSERT_GT(requests, 1u) << name;

    for (size_t at = 0; at < requests; ++at) {
      gfnttest::FailingAllocator failing(at);
      GFNT_Face * face = nullptr;
      GFNT_Result result;

      gfnt_error_clear(&error);
      result = gfnt_face_load(blob, 0, nullptr, failing.get(), &face, &error);
      if (result == GFNT_OK) {
        // A refused request the parse did not need: the reallocation that was
        // going to be asked for anyway, served from the capacity already there.
        gfnt_face_free(face);
        continue;
      }
      EXPECT_EQ(result, GFNT_ERR_OOM) << name << " request " << at << ": "
                                      << error.message;
      EXPECT_EQ(failing.live(), 0u) << name << " request " << at;
    }
    gfnt_blob_destroy(blob);
  }
}

TEST(Bitmap, NullArgumentsAreCallerErrors) {
  Fixture fixture("bitmap.pcf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  GFNT_BitmapGlyph bitmap{};
  size_t count = 0;
  GFNT_Coverage coverage{};

  EXPECT_EQ(gfnt_face_glyph_bitmap(nullptr, 0, 0, &bitmap, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_bitmap(fixture.face, 0, 0, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_bitmap(fixture.face, 99, 0, &bitmap, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_bitmap_encoding_count(nullptr, &count, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_bitmap_encoding_count(fixture.face, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_coverage_from_bitmap(nullptr, nullptr, &coverage, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_coverage_from_bitmap(&bitmap, nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_bitmap_dump(nullptr, stdout), GFNT_ERR_INVALID);

  // A pixel outside the glyph is a pixel the glyph does not cover, which is zero
  // rather than an error - a caller drawing a box around a glyph reads it.
  ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 2, 0, &bitmap, nullptr),
      GFNT_OK);
  EXPECT_EQ(gfnt_bitmap_pixel(&bitmap, bitmap.width, 0), 0u);
  EXPECT_EQ(gfnt_bitmap_pixel(&bitmap, 0, bitmap.height), 0u);
  EXPECT_EQ(gfnt_bitmap_pixel(nullptr, 0, 0), 0u);

  // A deeper strike than any container here produces: the bridge refuses rather
  // than reading a 1-bit row and calling it grey.
  bitmap.bit_depth = 4;
  EXPECT_EQ(gfnt_coverage_from_bitmap(&bitmap, nullptr, &coverage, nullptr),
      GFNT_ERR_UNSUPPORTED);
}

}  // namespace

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
