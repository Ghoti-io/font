/**
 * @file
 *
 * The committed fixtures: every one of them loads, and contains what
 * `tests/data/fonts/MANIFEST` says it contains.
 *
 * **Why this suite is not more of the same.** Every other expectation in
 * `tests/` is one this library wrote for itself - `tests/sfnt_builder.h` says
 * so in its own header, and a corpus a library generates from its own
 * understanding of a format cannot find the place where that understanding is
 * wrong. These fixtures are bytes **fontTools** wrote, from
 * `tools/fixtures/make_fixtures.py` in the pinned image, so this is the one
 * part of the unit suite whose input did not come from here. The differentials
 * under `tools/oracle/` are the other half of that and need a container; this
 * needs only the repository.
 *
 * It is also what makes `make test` cover the fixtures on a machine with no
 * container engine: `check-fixtures` proves they can be *regenerated*, and this
 * proves they are still readable and still say what they are for. A fixture
 * quietly corrupted in a checkout fails here.
 *
 * documentation/design.md sections 14.5 and 14.7.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/name.h>
#include <ghoti.io/font/outline.h>

namespace {

/** The em, the advances and the glyph count every fixture is built with. */
constexpr uint16_t kUpem = 1000;
constexpr size_t kNumGlyphs = 5;
constexpr int32_t kAdvanceA = 640;
constexpr int32_t kAdvanceB = 620;

/** The outline fixtures have a repertoire of their own; see MANIFEST. */
constexpr size_t kOutlineGlyphs = 13;
constexpr size_t kCompositeGlyphs = 27;

/** A fixture read from disk, with a blob and a face over it. */
struct Fixture {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  GFNT_Result result = GFNT_ERR_INTERNAL;

  explicit Fixture(const std::string & name, size_t index = 0) {
    gfnt_error_clear(&error);
    const std::string path = gfnttest::data("fonts/" + name);
    result = gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &blob,
        &error);
    // A missing fixture is a failure of this suite and not of the loader, so
    // say which file rather than letting a null face fail six lines later.
    EXPECT_EQ(result, GFNT_OK) << "could not read " << path << ": "
                               << error.message;
    if (result != GFNT_OK) {
      return;
    }
    result = gfnt_face_load(blob, index, nullptr, nullptr, &face, &error);
    EXPECT_EQ(result, GFNT_OK) << name << ": " << error.message;
  }

  ~Fixture() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }

  Fixture(const Fixture &) = delete;
  Fixture & operator=(const Fixture &) = delete;
};

/**
 * Every font the generator writes, in MANIFEST order, with its glyph count.
 *
 * The count is per fixture rather than shared: `post-v1.ttf` has all 258
 * standard glyphs because that is what its format means, and `post-v2.ttf` has
 * six because one of them sits on the standard/stored boundary. One shared
 * constant would have had to be relaxed until it asserted nothing.
 */
struct Entry {
  const char * name;
  size_t glyphs;
  /**
   * Whether this fixture states an em this library can report.
   *
   * True for every font that has a `head`, and for a bare CFF whose FontMatrix
   * reduces to one. False for exactly one fixture, which exists to be refused -
   * and it is a field rather than a relaxed assertion because "every fixture
   * shares one em" is what catches a fixture that has drifted from the
   * generator, and a check that tolerated both answers would catch nothing.
   */
  bool states_em = true;
  /**
   * Whether this fixture can state its own name and licence.
   *
   * True for every font with a `name` table, a CFF's Top DICT, a Type 1
   * `/FontInfo` or a BDF's properties. **False for four**, and this is the one
   * place section 14.5's rule meets a format that cannot carry it: a `.hex` file
   * is a codepoint, a colon and a row of bits, and a PSF is a header and cells.
   * Neither has anywhere to put a string at all - no comment syntax, no free
   * field - so for those four the statement is `tests/data/fonts/MANIFEST` and
   * this repository's own licence, and nothing in the bytes.
   *
   * It is a field rather than a loosened assertion because the other thirty-seven
   * fixtures do state it, and an assertion that tolerated both answers would stop
   * catching a fixture whose generator dropped its `name` table.
   */
  bool states_own_name = true;
};

const std::vector<Entry> & every_fixture() {
  static const std::vector<Entry> names = {
      // The two bare CFFs, whose glyph counts come from `CharStrings` because
      // neither has a `maxp` to carry one. `bare.cff` holds the same thirteen
      // glyphs as cff-curves.otf, whose CFF table its bytes are; the other holds
      // the same thirteen behind a FontMatrix that states no em, so it counts
      // and names its glyphs and refuses to draw them.
      // The eight standalone bitmap containers. None of them states an em - a
      // strike was drawn at a pixel size and there is nothing to scale from (M9)
      // - so every one of them is a `states_em` of false, and there are eight of
      // them rather than one relaxed assertion because each container counts its
      // glyphs a different way.
      // The two gzipped fixtures. Each must answer exactly as the uncompressed one
      // does, which is asserted glyph by glyph in test_bitmap.cpp and test_type1.cpp;
      // here they are simply two more fixtures that have to load and count.
      {"bitmap-gz.pcf.gz", 8, false},
      {"type1-gz.pfb.gz", 8},
      {"bitmap-ink.bdf", 5, false},
      {"bitmap-lsb.pcf", 8, false},
      {"bitmap-v1.psf", 512, false, false},
      {"bitmap-wide.hex", 3, false, false},
      {"bitmap.bdf", 8, false},
      {"bitmap.hex", 8, false, false},
      {"bitmap.pcf", 8, false},
      {"bitmap.psf", 8, false, false},
      {"bitmap-swap.pcf", 8, false},
      {"bare-matrix.cff", 13, false},
      // The two Type 1 font programs, which hold the same eight charstrings as
      // cff-type1.otf in a container that has no table directory, no `maxp` and
      // no `hmtx` - the glyph count comes from `/CharStrings` and each advance
      // from that glyph's own `hsbw`.
      {"type1-big.pfb", 80},
      {"type1.pfa", 8},
      {"type1.pfb", 8},
      {"bare.cff", 13},
      {"basic.ttf", kNumGlyphs},
      {"cff.otf", kNumGlyphs},
      // The seven phase 2 fixtures, each with its own repertoire: what they
      // exercise is a *program*, and a program needs its own glyphs.
      {"cff-arith.otf", 7},
      {"cff-cid.otf", 4},
      {"cff-curves.otf", 13},
      {"cff-hints.otf", 8},
      {"cff-seac.otf", 6},
      {"cff-subrs.otf", 10},
      {"cff-type1.otf", 8},
      {"cmap-format0.ttf", kNumGlyphs},
      {"cmap-format12.ttf", kNumGlyphs},
      {"cmap-format6.ttf", kNumGlyphs},
      {"cmap-symbol.ttf", kNumGlyphs},
      {"collection.ttc", kNumGlyphs},
      {"name-mac-encodings.ttf", kNumGlyphs},
      {"name-macroman.ttf", kNumGlyphs},
      {"os2-v0.ttf", kNumGlyphs},
      {"os2-v1.ttf", kNumGlyphs},
      {"os2-v2.ttf", kNumGlyphs},
      {"os2-v3.ttf", kNumGlyphs},
      {"os2-v4.ttf", kNumGlyphs},
      {"os2-v5.ttf", kNumGlyphs},
      {"outline-broken-loca.ttf", kOutlineGlyphs},
      {"outline-composite.ttf", kCompositeGlyphs},
      {"outline-cubic-flag.ttf", kOutlineGlyphs},
      {"outline-cubic.ttf", kOutlineGlyphs},
      {"outline-loca-long.ttf", kOutlineGlyphs},
      {"outline-simple.ttf", kOutlineGlyphs},
      {"post-v1.ttf", 258},
      // The first fixture with more than one strike. An ordinary TrueType face -
      // five glyphs, an em, a `name` - with three EBLC strikes beside its
      // outlines, so it is the one fixture here that is both, and `states_em`
      // stays true because the *face* has one even though its strikes do not.
      {"strikes.ttf", kNumGlyphs},
      {"post-v2.ttf", kNumGlyphs + 1},
      {"post-v3.ttf", kNumGlyphs},
  };
  return names;
}

/** Every name MANIFEST lists, which is what the generator actually wrote. */
std::vector<std::string> manifest_names() {
  std::vector<std::string> out;
  FILE * handle = fopen(gfnttest::data("fonts/MANIFEST").c_str(), "r");
  if (!handle) {
    return out;
  }
  char line[1024];
  while (fgets(line, sizeof line, handle)) {
    if (line[0] == '#' || line[0] == '\n') {
      continue;
    }
    const char * tab = strchr(line, '\t');
    if (!tab) {
      continue;
    }
    out.push_back(std::string(line, static_cast<size_t>(tab - line)));
  }
  fclose(handle);
  return out;
}

/** The subtable formats a fixture's `cmap` lists, as (platform,encoding,format). */
std::vector<std::string> cmap_shape(const GFNT_Face * face) {
  std::vector<std::string> out;
  size_t count = 0;
  if (gfnt_face_cmap_count(face, &count, nullptr) != GFNT_OK) {
    return out;
  }
  for (size_t i = 0; i < count; ++i) {
    GFNT_CmapSubtable subtable{};
    if (gfnt_face_cmap_at(face, i, &subtable, nullptr) != GFNT_OK) {
      out.push_back("unreadable");
      continue;
    }
    out.push_back("(" + std::to_string(subtable.platform_id) + ","
        + std::to_string(subtable.encoding_id) + ")f"
        + std::to_string(subtable.format));
  }
  return out;
}

TEST(Fixtures, ThisSuitesListIsTheGeneratorsList) {
  // Without this, a fixture added to the generator is covered by
  // `check-fixtures` - which walks the directory - and by nothing here, because
  // every test below iterates a list written by hand. The list and MANIFEST are
  // two spellings of one set, and the set is the generator's.
  std::vector<std::string> mine;
  for (const Entry & entry : every_fixture()) {
    mine.push_back(entry.name);
  }
  std::vector<std::string> theirs = manifest_names();
  ASSERT_FALSE(theirs.empty()) << "MANIFEST is missing or unreadable";
  std::sort(mine.begin(), mine.end());
  std::sort(theirs.begin(), theirs.end());
  EXPECT_EQ(mine, theirs);
}

TEST(Fixtures, EveryCommittedFixtureLoads) {
  for (const Entry & entry : every_fixture()) {
    const std::string name = entry.name;
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    // The em is shared by construction. A fixture that has drifted from the
    // generator fails here rather than in whichever suite uses it next.
    uint16_t upem = 0;
    if (entry.states_em) {
      EXPECT_EQ(gfnt_face_units_per_em(fixture.face, &upem, nullptr), GFNT_OK)
          << name;
      EXPECT_EQ(upem, kUpem) << name;
    }
    else {
      EXPECT_EQ(gfnt_face_units_per_em(fixture.face, &upem, nullptr),
          GFNT_ERR_UNSUPPORTED) << name;
    }

    size_t glyphs = 0;
    EXPECT_EQ(gfnt_face_num_glyphs(fixture.face, &glyphs, nullptr), GFNT_OK)
        << name;
    EXPECT_EQ(glyphs, entry.glyphs) << name;
    EXPECT_FALSE(gfnt_face_num_glyphs_disagreement(fixture.face, nullptr))
        << name;
  }
}

TEST(Fixtures, EveryFixtureNamesItselfAndItsLicence) {
  // A fixture that escapes into a font directory should answer for itself, and
  // the licence question this repository does not have is one the fixture
  // states rather than one a reader infers from its absence (section 14.5).
  for (const Entry & entry : every_fixture()) {
    const std::string name = entry.name;
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    char * family = nullptr;
    if (!entry.states_own_name) {
      // The format has nowhere to put one. Asserting the refusal rather than
      // skipping the fixture, so that a container which grows a name field is
      // noticed here.
      EXPECT_EQ(gfnt_face_name(fixture.face, GFNT_NAME_FAMILY, GFNT_LANGUAGE_ANY,
                    nullptr, &family, nullptr, nullptr),
          GFNT_ERR_UNSUPPORTED) << name;
      continue;
    }
    ASSERT_EQ(gfnt_face_name(fixture.face, GFNT_NAME_FAMILY,
                  GFNT_LANGUAGE_ANY, nullptr, &family, nullptr, nullptr),
        GFNT_OK) << name;
    EXPECT_EQ(std::string(family).rfind("Ghoti Fixture", 0), 0u)
        << name << " family is " << family;
    gfnt_name_free(nullptr, family);

    char * copyright = nullptr;
    ASSERT_EQ(gfnt_face_name(fixture.face, GFNT_NAME_COPYRIGHT,
                  GFNT_LANGUAGE_ANY, nullptr, &copyright, nullptr, nullptr),
        GFNT_OK) << name;
    EXPECT_NE(std::string(copyright).find("LGPL-3.0-only"), std::string::npos)
        << name << " copyright is " << copyright;
    gfnt_name_free(nullptr, copyright);
  }
}

TEST(Fixtures, BasicIsTrueTypeWithFormat4OnTwoPlatforms) {
  Fixture fixture("basic.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  EXPECT_EQ(gfnt_face_flavour(fixture.face), GFNT_FLAVOUR_TRUETYPE);
  EXPECT_TRUE(gfnt_face_has_outlines(fixture.face));
  EXPECT_TRUE(gfnt_face_has_table(fixture.face, GFNT_TAG('g', 'l', 'y', 'f')));
  EXPECT_FALSE(gfnt_face_has_table(fixture.face, GFNT_TAG('C', 'F', 'F', ' ')));

  EXPECT_EQ(cmap_shape(fixture.face),
      (std::vector<std::string>{"(0,3)f4", "(3,1)f4"}));

  // The outlines are drawn in make_fixtures.py, so the advances are known
  // rather than read off whatever the library reports.
  uint32_t glyph = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 'A', &glyph, nullptr),
      GFNT_OK);
  EXPECT_NE(glyph, 0u);
  int32_t advance = 0;
  ASSERT_EQ(gfnt_face_glyph_advance(fixture.face, glyph, nullptr, &advance, nullptr),
      GFNT_OK);
  EXPECT_EQ(advance, kAdvanceA);

  ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 'B', &glyph, nullptr),
      GFNT_OK);
  ASSERT_EQ(gfnt_face_glyph_advance(fixture.face, glyph, nullptr, &advance, nullptr),
      GFNT_OK);
  EXPECT_EQ(advance, kAdvanceB);
}

TEST(Fixtures, CffIsTheOttoFlavourWithNoGlyf) {
  Fixture fixture("cff.otf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  EXPECT_EQ(gfnt_face_flavour(fixture.face), GFNT_FLAVOUR_CFF);
  EXPECT_TRUE(gfnt_face_has_table(fixture.face, GFNT_TAG('C', 'F', 'F', ' ')));
  EXPECT_FALSE(gfnt_face_has_table(fixture.face, GFNT_TAG('g', 'l', 'y', 'f')));
  // **This assertion was `EXPECT_FALSE` until phase 2**, and it was right then:
  // the predicate answers "can asking for an outline succeed", and until a
  // charstring interpreter existed the answer for an OTTO face was no. It is
  // kept and flipped rather than deleted, because an assertion that something
  // is unsupported goes on passing after the support lands and says nothing.
  EXPECT_TRUE(gfnt_face_has_outlines(fixture.face));

  // And the outline is there to be had, which is the whole of what phase 2
  // changed about this font.
  GFNT_Outline * outline = nullptr;
  ASSERT_EQ(gfnt_face_glyph_outline(fixture.face, 2, nullptr, nullptr, &outline,
      nullptr), GFNT_OK);
  EXPECT_GT(gfnt_outline_point_count(outline), 0u);
  gfnt_outline_destroy(outline);

  // Every metric table is the same question over a different container.
  uint32_t glyph = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 'A', &glyph, nullptr),
      GFNT_OK);
  int32_t advance = 0;
  ASSERT_EQ(gfnt_face_glyph_advance(fixture.face, glyph, nullptr, &advance, nullptr),
      GFNT_OK);
  EXPECT_EQ(advance, kAdvanceA);
}

TEST(Fixtures, TheCollectionHoldsTwoFacesThatShareTables) {
  size_t count = 0;
  {
    Fixture first("collection.ttc", 0);
    ASSERT_EQ(first.result, GFNT_OK);
    EXPECT_EQ(gfnt_face_flavour(first.face), GFNT_FLAVOUR_TRUETYPE);
    EXPECT_EQ(gfnt_face_index(first.face), 0u);
    ASSERT_EQ(gfnt_face_count(first.blob, nullptr, &count, nullptr), GFNT_OK);
    EXPECT_EQ(count, 2u);
  }

  // The two faces carry different OS/2 versions, which is how a reader that
  // answered from the wrong face would be caught: they are otherwise alike.
  Fixture first("collection.ttc", 0);
  Fixture second("collection.ttc", 1);
  ASSERT_EQ(first.result, GFNT_OK);
  ASSERT_EQ(second.result, GFNT_OK);
  EXPECT_EQ(gfnt_face_index(second.face), 1u);

  const GFNT_Os2 * one = nullptr;
  const GFNT_Os2 * two = nullptr;
  ASSERT_EQ(gfnt_face_os2(first.face, &one, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_os2(second.face, &two, nullptr), GFNT_OK);
  EXPECT_EQ(one->version, 4u);
  EXPECT_EQ(two->version, 1u);

  // Shared tables are the point of a collection: the second face's directory
  // entry for `glyf` names the same bytes as the first's. A reader that treated
  // an offset as relative to its own face would read garbage, and a generator
  // that wrote two concatenated fonts would make this pass for the wrong reason.
  size_t offset_one = 0;
  size_t length_one = 0;
  size_t offset_two = 0;
  size_t length_two = 0;
  ASSERT_EQ(gfnt_face_table_range(first.face, GFNT_TAG('g', 'l', 'y', 'f'),
                &offset_one, &length_one),
      GFNT_OK);
  ASSERT_EQ(gfnt_face_table_range(second.face, GFNT_TAG('g', 'l', 'y', 'f'),
                &offset_two, &length_two),
      GFNT_OK);
  EXPECT_EQ(offset_one, offset_two);
  EXPECT_EQ(length_one, length_two);
}

TEST(Fixtures, TheOs2VersionDecidesWhichFieldsExist) {
  // The six fixtures carry the same requested values and differ only in the
  // version, which decides which of them the file actually holds. The oracle
  // corpus has versions 1, 3 and 4 only - no 0, no 2, no 5 - and a planted
  // defect that read version 2's fields out of a version 1 table survived a
  // four-font sample of it (design.md section 14.7). These close that axis.
  //
  // Every gated value below is non-zero in the generator, so "the field was
  // read" and "the field was absent, so reported zero" are distinguishable.
  struct Expectation {
    const char * name;
    uint16_t version;
    bool code_pages; ///< Version 1 and later.
    bool version_2;  ///< x-height through max context.
    bool optical;    ///< Version 5 only.
  };
  const Expectation expectations[] = {
      {"os2-v0.ttf", 0, false, false, false},
      {"os2-v1.ttf", 1, true, false, false},
      {"os2-v2.ttf", 2, true, true, false},
      {"os2-v3.ttf", 3, true, true, false},
      {"os2-v4.ttf", 4, true, true, false},
      {"os2-v5.ttf", 5, true, true, true},
  };

  for (const Expectation & expectation : expectations) {
    Fixture fixture(expectation.name);
    ASSERT_EQ(fixture.result, GFNT_OK) << expectation.name;

    const GFNT_Os2 * os2 = nullptr;
    ASSERT_EQ(gfnt_face_os2(fixture.face, &os2, nullptr), GFNT_OK)
        << expectation.name;
    ASSERT_NE(os2, nullptr) << expectation.name;
    EXPECT_EQ(os2->version, expectation.version) << expectation.name;

    // Present at every version, so a fixture whose OS/2 is simply unread does
    // not pass the absences below by accident.
    EXPECT_EQ(os2->typo_ascender, 800) << expectation.name;
    EXPECT_EQ(os2->typo_descender, -200) << expectation.name;
    EXPECT_EQ(os2->win_ascent, 800u) << expectation.name;
    EXPECT_EQ(os2->win_descent, 200u) << expectation.name;
    EXPECT_STREQ(os2->vendor_id, "GHTI") << expectation.name;

    if (expectation.code_pages) {
      EXPECT_EQ(os2->code_page_range[0], 0x00000651u) << expectation.name;
      EXPECT_EQ(os2->code_page_range[1], 0x10000001u) << expectation.name;
    }
    else {
      EXPECT_EQ(os2->code_page_range[0], 0u) << expectation.name;
      EXPECT_EQ(os2->code_page_range[1], 0u) << expectation.name;
    }

    if (expectation.version_2) {
      EXPECT_EQ(os2->x_height, 520) << expectation.name;
      EXPECT_EQ(os2->cap_height, 720) << expectation.name;
      EXPECT_EQ(os2->default_char, 0x003Fu) << expectation.name;
      EXPECT_EQ(os2->break_char, 0x0020u) << expectation.name;
      EXPECT_EQ(os2->max_context, 3u) << expectation.name;
    }
    else {
      EXPECT_EQ(os2->x_height, 0) << expectation.name;
      EXPECT_EQ(os2->cap_height, 0) << expectation.name;
      EXPECT_EQ(os2->default_char, 0u) << expectation.name;
      EXPECT_EQ(os2->break_char, 0u) << expectation.name;
      EXPECT_EQ(os2->max_context, 0u) << expectation.name;
    }

    if (expectation.optical) {
      // The file holds twentieths of a point, and this library reports what the
      // file holds: 8pt and 72pt as fontTools was asked for them.
      EXPECT_EQ(os2->lower_optical_size, 160u) << expectation.name;
      EXPECT_EQ(os2->upper_optical_size, 1440u) << expectation.name;
    }
    else {
      EXPECT_EQ(os2->lower_optical_size, 0u) << expectation.name;
      EXPECT_EQ(os2->upper_optical_size, 0u) << expectation.name;
    }
  }
}

TEST(Fixtures, EachCmapFormatIsReadableOnItsOwn) {
  struct Case {
    const char * name;
    std::vector<std::string> shape;
    uint32_t codepoint;
  };
  const std::vector<Case> cases = {
      {"cmap-format0.ttf", {"(1,0)f0"}, 'A'},
      {"cmap-format6.ttf", {"(1,0)f6"}, 'A'},
      {"cmap-format12.ttf", {"(3,1)f4", "(3,10)f12"}, 'A'},
  };

  for (const Case & one : cases) {
    Fixture fixture(one.name);
    ASSERT_EQ(fixture.result, GFNT_OK) << one.name;
    EXPECT_EQ(cmap_shape(fixture.face), one.shape) << one.name;

    uint32_t glyph = GFNT_GLYPH_NONE;
    ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, one.codepoint, &glyph,
                  nullptr),
        GFNT_OK) << one.name;
    EXPECT_NE(glyph, 0u) << one.name;
    int32_t advance = 0;
    ASSERT_EQ(gfnt_face_glyph_advance(fixture.face, glyph, nullptr, &advance, nullptr),
        GFNT_OK) << one.name;
    EXPECT_EQ(advance, kAdvanceA) << one.name;
  }
}

TEST(Fixtures, Format12ReachesBeyondTheBasicMultilingualPlane) {
  Fixture fixture("cmap-format12.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  // U+10041 and U+1F600 cannot be held by the format 4 beside it, so a lookup
  // that succeeds proves the format 12 was chosen and its groups were walked.
  uint32_t bmp = GFNT_GLYPH_NONE;
  uint32_t wide = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 'A', &bmp, nullptr),
      GFNT_OK);
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 0x10041, &wide,
                nullptr),
      GFNT_OK);
  EXPECT_NE(wide, 0u);
  EXPECT_EQ(wide, bmp) << "U+10041 and U+0041 are mapped to the same glyph";

  uint32_t emoji = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 0x1F600, &emoji,
                nullptr),
      GFNT_OK);
  EXPECT_NE(emoji, 0u);
  EXPECT_NE(emoji, bmp);
}

TEST(Fixtures, TheSymbolFixtureIsReachableOnlyThroughTheF0xxRule) {
  Fixture fixture("cmap-symbol.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);
  EXPECT_EQ(cmap_shape(fixture.face), (std::vector<std::string>{"(3,0)f4"}));

  // The subtable maps 0xF041 and not 0x41, so U+0041 resolves only via the
  // symbol rule in section 7.2. No font in the oracle corpus is shaped this
  // way, which is why the fixture exists.
  // Asked of the subtable directly, rather than of the face, so that the
  // fallback is not what answers: gfnt_cmap_lookup takes the subtable by name
  // and applies no rule of its own.
  GFNT_CmapSubtable symbol{};
  ASSERT_EQ(gfnt_face_cmap_at(fixture.face, 0, &symbol, nullptr), GFNT_OK);

  uint32_t direct = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_cmap_lookup(fixture.face, &symbol, 'A', &direct, nullptr),
      GFNT_OK);
  EXPECT_EQ(direct, 0u) << "the subtable itself must not map U+0041";

  uint32_t mapped = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_cmap_lookup(fixture.face, &symbol, 0xF041, &mapped, nullptr),
      GFNT_OK);
  EXPECT_NE(mapped, 0u);

  uint32_t glyph = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 'A', &glyph, nullptr),
      GFNT_OK);
  EXPECT_EQ(glyph, mapped) << "the 0xF0xx fallback must find the same glyph";
}

TEST(Fixtures, ThePostFixturesCarryTheTwoFormatsThisLibraryMeets) {
  Fixture two("post-v2.ttf");
  ASSERT_EQ(two.result, GFNT_OK);
  const GFNT_Post * post = nullptr;
  ASSERT_EQ(gfnt_face_post(two.face, &post, nullptr), GFNT_OK);
  EXPECT_EQ(post->version, 0x00020000u);

  Fixture three("post-v3.ttf");
  ASSERT_EQ(three.result, GFNT_OK);
  const GFNT_Post * header = nullptr;
  ASSERT_EQ(gfnt_face_post(three.face, &header, nullptr), GFNT_OK);
  EXPECT_EQ(header->version, 0x00030000u);

  // The header fields are the same in both, which is what makes the version the
  // only difference and the fixture pair worth having: section 7.2 reads the
  // header and defers the names, and the format 2.0 table here is what the
  // names will be read from.
  EXPECT_EQ(post->italic_angle, header->italic_angle);
  EXPECT_EQ(post->underline_position, header->underline_position);
  EXPECT_EQ(post->is_fixed_pitch, header->is_fixed_pitch);
}

TEST(Fixtures, TheMacRomanRecordsDecodeToTheSameTextAsTheirWindowsTwins) {
  Fixture fixture("name-macroman.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  // This fixture was built as the witness for a *refusal*: section 7.2 decoded
  // Macintosh Roman as far as ASCII and stopped, because the rest needed a
  // 128-codepoint table that had to come from an oracle rather than from memory.
  // The table is generated now, so the same fixture proves the opposite - and
  // proves it the strongest way this font allows, because it carries each string
  // twice: once as Mac Roman bytes and once as Windows UTF-16BE. Two encodings,
  // one text, and the comparison needs no expected value written down here.
  size_t count = 0;
  ASSERT_EQ(gfnt_face_name_count(fixture.face, &count, nullptr), GFNT_OK);
  ASSERT_GT(count, 0u);

  std::map<uint16_t, std::string> macintosh;
  std::map<uint16_t, std::string> windows;
  for (size_t i = 0; i < count; ++i) {
    GFNT_NameRecord record{};
    ASSERT_EQ(gfnt_face_name_at(fixture.face, i, &record, nullptr), GFNT_OK);

    char * text = nullptr;
    ASSERT_EQ(gfnt_face_name_decode(fixture.face, &record, nullptr, &text,
                  nullptr, nullptr),
        GFNT_OK) << "record " << i << " (platform " << record.platform_id
                 << ", encoding " << record.encoding_id << ")";
    if (record.platform_id == GFNT_PLATFORM_MACINTOSH) {
      macintosh[record.name_id] = text;
    }
    else if (record.platform_id == GFNT_PLATFORM_WINDOWS) {
      windows[record.name_id] = text;
    }
    gfnt_name_free(nullptr, text);
  }

  ASSERT_GT(macintosh.size(), 0u) << "the fixture must carry Macintosh records";
  size_t paired = 0;
  size_t above_ascii = 0;
  for (const auto & pair : macintosh) {
    if (!windows.count(pair.first)) {
      continue;
    }
    EXPECT_EQ(pair.second, windows[pair.first])
        << "name ID " << pair.first << " differs between its Mac Roman and "
        << "its UTF-16BE spelling";
    ++paired;
    for (unsigned char byte : pair.second) {
      if (byte > 0x7F) {
        ++above_ascii;
        break;
      }
    }
  }
  EXPECT_GE(paired, 8u) << "most name IDs are written in both encodings";
  // Counted, so that a fixture rebuilt without the non-ASCII records cannot
  // make this test pass by leaving it nothing to check.
  EXPECT_EQ(above_ascii, 2u)
      << "the trademark and description records are the two holding 0xAA";

  EXPECT_NE(macintosh[GFNT_NAME_TRADEMARK].find("\xE2\x84\xA2"),
      std::string::npos)
      << "0xAA in Mac Roman is U+2122, which is E2 84 A2 in UTF-8";
}

TEST(Fixtures, EveryFixturesStrikeCountIsTheOneItsContainerImplies) {
  // **This asserted zero for every fixture until phase 1b**, and it was right
  // then: no container this library read carried a strike. It was then "one or
  // none", which was right until `strikes.ttf` - and the rename is the history:
  // an assertion about a shape the fixture set has outgrown goes on passing and
  // stops saying anything, which is what `CffIsTheOttoFlavourWithNoGlyf` records
  // about the same hazard.
  //
  // What it says now: a standalone bitmap container has exactly one strike,
  // because the file *is* a strike; a face with an `EBLC` and an `EBDT` has as
  // many as the table lists; every other fixture has none; and no fixture
  // reports ::GFNT_ERR_UNSUPPORTED, which would mean it carried strikes in a
  // table this library cannot enumerate. That last answer is the one M9 needs
  // kept apart from zero, and there is still no *fixture* that produces it -
  // `bloc`, `CBLC` and `sbix` are hand-built cases in test_bitmap.cpp, because a
  // fixture is a font this library reads and those are fonts it does not.
  size_t with_strikes = 0;
  size_t with_many = 0;

  for (const Entry & entry : every_fixture()) {
    const std::string name = entry.name;
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;
    size_t strikes = 99;
    const GFNT_Result result =
        gfnt_face_strike_count(fixture.face, &strikes, nullptr);
    EXPECT_EQ(result, GFNT_OK) << name;
    // Which answer to expect comes from the flavour and the directory, not from
    // a field: both facts are the font's own, and a field here would be a second
    // place to state them that could drift from the first.
    const GFNT_Tag flavour = gfnt_face_flavour(fixture.face);
    const bool is_strike = flavour == GFNT_FLAVOUR_PCF
        || flavour == GFNT_FLAVOUR_BDF || flavour == GFNT_FLAVOUR_PSF
        || flavour == GFNT_FLAVOUR_HEX;
    const bool has_eblc = gfnt_face_has_table(fixture.face,
                              GFNT_TAG('E', 'B', 'L', 'C'))
        && gfnt_face_has_table(fixture.face, GFNT_TAG('E', 'B', 'D', 'T'));

    if (is_strike) {
      EXPECT_EQ(strikes, 1u) << name;
    } else if (has_eblc) {
      EXPECT_GT(strikes, 1u) << name << ": the one EBLC fixture there is has "
          "three, and a fixture with one would not exercise a choice";
    } else {
      EXPECT_EQ(strikes, 0u) << name;
    }
    if (strikes > 0) {
      ++with_strikes;
    }
    if (strikes > 1) {
      ++with_many;
    }
  }
  // Counted, because the three branches above are only worth anything if the
  // fixture set reaches all of them: a sweep over a set that had lost its
  // multi-strike font would pass every assertion and check nothing new.
  EXPECT_EQ(with_strikes, 11u) << "ten standalone containers and strikes.ttf";
  EXPECT_EQ(with_many, 1u) << "strikes.ttf is the only fixture with a choice";
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
