/**
 * @file
 *
 * The glyph kinds and the strike policy: the API shape phase 0 owes, and the
 * one thing it must not do, which is answer "no strikes" for a font that has
 * them.
 *
 * documentation/design.md sections 5.3, 5.4 and decision 3.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <set>
#include <string>
#include <vector>

#include <ghoti.io/font/glyph.h>

using gfnttest::Table;

namespace {

struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  explicit Font(std::vector<Table> tables, GFNT_Tag flavour = GFNT_FLAVOUR_TRUETYPE)
      : bytes(gfnttest::build_sfnt(flavour, tables)) {
    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                  GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
        GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error),
        GFNT_OK);
  }

  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }

  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;
};

/** A table of the given tag, with bytes that are not meant to parse. */
Table stub(GFNT_Tag tag, size_t length = 16) {
  return Table{tag, std::vector<uint8_t>(length, 0x11)};
}

} // namespace

TEST(GlyphKind, EveryKindHasItsOwnName) {
  std::set<std::string> seen;
  for (int i = 0; i < GFNT_GLYPH_KIND_COUNT; i++) {
    const char * name = gfnt_glyph_kind_string(static_cast<GFNT_GlyphKind>(i));
    ASSERT_NE(name, nullptr) << "kind " << i;
    EXPECT_STRNE(name, "unknown glyph kind") << "kind " << i;
    EXPECT_TRUE(seen.insert(name).second) << "kind " << i << " shares a name";
  }
  EXPECT_STREQ(gfnt_glyph_kind_string(GFNT_GLYPH_KIND_COUNT),
      "unknown glyph kind");
  EXPECT_STREQ(gfnt_glyph_kind_string(static_cast<GFNT_GlyphKind>(-1)),
      "unknown glyph kind");
}

TEST(GlyphKind, TheEnumCoversEveryThingAGlyphCanBe) {
  // design.md section 5.4 names five real arms and one that is deliberately
  // absent, so that the enum is complete and adding the SVG table later is not
  // a break.
  EXPECT_EQ(GFNT_GLYPH_KIND_COUNT, 6);
}

TEST(Outlines, AreFoundWhenTheirTablesAreBothThere) {
  // glyf without loca is not indexable, so it is not outlines this library can
  // reach.
  Font truetype({stub(GFNT_TAG('g', 'l', 'y', 'f')),
      stub(GFNT_TAG('l', 'o', 'c', 'a'))});
  Font glyf_only({stub(GFNT_TAG('g', 'l', 'y', 'f'))});
  Font loca_only({stub(GFNT_TAG('l', 'o', 'c', 'a'))});
  Font cff({stub(GFNT_TAG('C', 'F', 'F', ' '))}, GFNT_FLAVOUR_CFF);
  Font neither({stub(GFNT_TAG('m', 'a', 'x', 'p'))});

  EXPECT_TRUE(gfnt_face_has_outlines(truetype.face));
  EXPECT_FALSE(gfnt_face_has_outlines(glyf_only.face));
  EXPECT_FALSE(gfnt_face_has_outlines(loca_only.face));
  EXPECT_TRUE(gfnt_face_has_outlines(cff.face));
  EXPECT_FALSE(gfnt_face_has_outlines(neither.face));
  EXPECT_FALSE(gfnt_face_has_outlines(nullptr));
}

TEST(Outlines, ACff2OnlyFaceHasNoneThisLibraryReads) {
  // Section 16 defers CFF2. What a caller needs to know is whether asking for
  // an outline can succeed, and here it cannot.
  Font font({stub(GFNT_TAG('C', 'F', 'F', '2'))}, GFNT_FLAVOUR_CFF);
  EXPECT_FALSE(gfnt_face_has_outlines(font.face));
}

TEST(Strikes, AFaceWithNoBitmapTablesHasNone) {
  Font font({stub(GFNT_TAG('g', 'l', 'y', 'f')),
      stub(GFNT_TAG('l', 'o', 'c', 'a'))});
  size_t count = 1;

  ASSERT_EQ(gfnt_face_strike_count(font.face, &count, &font.error), GFNT_OK);
  EXPECT_EQ(count, 0u);
  GFNT_Strike strike{};
  EXPECT_EQ(gfnt_face_strike_at(font.face, 0, &strike, nullptr),
      GFNT_ERR_INVALID);
}

TEST(Strikes, AFaceWithStrikesThisLibraryCannotParseSaysSoRatherThanZero) {
  // The whole point of this accessor existing before the bitmap parsers do. A
  // count of zero here would tell a caller that a bitmap font has no bitmaps,
  // and nothing downstream could tell that from the truth.
  for (GFNT_Tag tag : {GFNT_TAG('E', 'B', 'L', 'C'), GFNT_TAG('b', 'l', 'o', 'c'),
           GFNT_TAG('C', 'B', 'L', 'C'), GFNT_TAG('s', 'b', 'i', 'x')}) {
    Font font({stub(tag), stub(GFNT_TAG('g', 'l', 'y', 'f')),
        stub(GFNT_TAG('l', 'o', 'c', 'a'))});
    size_t count = 99;
    char text[5];

    EXPECT_EQ(gfnt_face_strike_count(font.face, &count, &font.error),
        GFNT_ERR_UNSUPPORTED)
        << "with a " << gfnt_tag_string(tag, text) << " table";
    EXPECT_EQ(count, 99u) << "nothing is written on failure";
    EXPECT_EQ(font.error.table, tag) << "the diagnostic names the table";
  }
}

TEST(Strikes, TheZeroPolicyIgnoresStrikesEntirely) {
  // GFNT_STRIKE_OUTLINES_ONLY is zero so that a caller who never heard of
  // strikes cannot be surprised by one - including by a strike table this
  // library cannot read, which every other policy refuses over.
  Font font({stub(GFNT_TAG('E', 'B', 'L', 'C')),
      stub(GFNT_TAG('g', 'l', 'y', 'f')), stub(GFNT_TAG('l', 'o', 'c', 'a'))});
  GFNT_Strike strike{};
  bool from_outlines = false;

  EXPECT_EQ(GFNT_STRIKE_OUTLINES_ONLY, 0);
  ASSERT_EQ(gfnt_face_select_strike(font.face, 12, GFNT_STRIKE_OUTLINES_ONLY,
                &strike, &from_outlines, &font.error),
      GFNT_OK);
  EXPECT_TRUE(from_outlines);
  EXPECT_EQ(strike.kind, GFNT_GLYPH_OUTLINE);
  EXPECT_EQ(strike.ppem_x, 12u);
  EXPECT_EQ(strike.ppem_y, 12u);

  for (GFNT_StrikePolicy policy : {GFNT_STRIKE_EXACT, GFNT_STRIKE_NEAREST,
           GFNT_STRIKE_PREFER_STRIKE}) {
    EXPECT_EQ(gfnt_face_select_strike(font.face, 12, policy, &strike,
                  &from_outlines, &font.error),
        GFNT_ERR_UNSUPPORTED)
        << "policy " << policy
        << " wanted a strike, and this library cannot see the ones this face "
           "has";
  }
}

TEST(Strikes, WithNoStrikesEveryPolicyScalesTheOutlines) {
  Font font({stub(GFNT_TAG('g', 'l', 'y', 'f')),
      stub(GFNT_TAG('l', 'o', 'c', 'a'))});

  for (GFNT_StrikePolicy policy : {GFNT_STRIKE_OUTLINES_ONLY,
           GFNT_STRIKE_EXACT, GFNT_STRIKE_NEAREST, GFNT_STRIKE_PREFER_STRIKE}) {
    GFNT_Strike strike{};
    bool from_outlines = false;
    ASSERT_EQ(gfnt_face_select_strike(font.face, 16, policy, &strike,
                  &from_outlines, nullptr),
        GFNT_OK)
        << "policy " << policy;
    EXPECT_TRUE(from_outlines);
  }
}

TEST(Strikes, AFaceThatCanAnswerAtNoSizeSaysSo) {
  Font font({stub(GFNT_TAG('m', 'a', 'x', 'p'))});
  GFNT_Strike strike{};
  bool from_outlines = true;

  EXPECT_EQ(gfnt_face_select_strike(font.face, 12, GFNT_STRIKE_OUTLINES_ONLY,
                &strike, &from_outlines, &font.error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(font.error.message, nullptr);
}

TEST(Strikes, APixelSizeOfZeroOrAboveTheLimitIsRefused) {
  std::vector<uint8_t> bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {stub(GFNT_TAG('g', 'l', 'y', 'f')), stub(GFNT_TAG('l', 'o', 'c', 'a'))});
  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_ppem = 32;

  GFNT_Blob * blob = nullptr;
  ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
      GFNT_OK);
  GFNT_Face * face = nullptr;
  ASSERT_EQ(gfnt_face_load(blob, 0, &limits, nullptr, &face, nullptr), GFNT_OK);

  GFNT_Error error;
  EXPECT_EQ(gfnt_face_select_strike(face, 0, GFNT_STRIKE_OUTLINES_ONLY, nullptr,
                nullptr, &error),
      GFNT_ERR_INVALID)
      << "a ppem of zero is a caller error, not a font's";
  EXPECT_EQ(gfnt_face_select_strike(face, 33, GFNT_STRIKE_OUTLINES_ONLY,
                nullptr, nullptr, &error),
      GFNT_ERR_LIMIT)
      << "max_ppem exists because a 65,535-ppem glyph is a gigabyte (M22)";
  EXPECT_EQ(gfnt_face_select_strike(face, 32, GFNT_STRIKE_OUTLINES_ONLY,
                nullptr, nullptr, &error),
      GFNT_OK)
      << "the limit is a maximum, not a strict bound";

  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
}

TEST(Strikes, APolicyThisLibraryDoesNotDefineIsACallerError) {
  Font font({stub(GFNT_TAG('g', 'l', 'y', 'f')),
      stub(GFNT_TAG('l', 'o', 'c', 'a'))});
  EXPECT_EQ(gfnt_face_select_strike(font.face, 12,
                static_cast<GFNT_StrikePolicy>(42), nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
}

TEST(Strikes, EveryEntryPointRefusesNullArguments) {
  Font font({stub(GFNT_TAG('g', 'l', 'y', 'f')),
      stub(GFNT_TAG('l', 'o', 'c', 'a'))});
  size_t count = 0;
  GFNT_Strike strike{};

  EXPECT_EQ(gfnt_face_strike_count(nullptr, &count, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_strike_count(font.face, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_strike_at(nullptr, 0, &strike, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_strike_at(font.face, 0, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_select_strike(nullptr, 12, GFNT_STRIKE_OUTLINES_ONLY,
                nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
