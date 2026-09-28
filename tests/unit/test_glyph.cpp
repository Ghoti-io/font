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

/**
 * A `post` format 2.0 table: the shared header, then a body written here.
 *
 * `gfnttest::build_post()` writes a correct one-glyph body, which is what a test
 * of the header's fields wants. The names are a different subject: what a reader
 * has to survive is a table that contradicts itself - a count with no array
 * behind it, an index past the last stored string, a Pascal length that runs off
 * the end - and none of those is something a builder should be able to produce
 * by accident. So `count` is written as given rather than derived from
 * `indices`, and `drop` cuts bytes off the end afterwards.
 */
std::vector<uint8_t> post_v2(uint16_t count,
    const std::vector<uint16_t> & indices,
    const std::vector<std::string> & names, size_t drop = 0) {
  const std::vector<uint8_t> whole = gfnttest::build_post(0x00020000);
  std::vector<uint8_t> out(whole.begin(), whole.begin() + 32);

  gfnttest::put_u16(out, count);
  for (uint16_t index : indices) {
    gfnttest::put_u16(out, index);
  }
  for (const std::string & name : names) {
    out.push_back(static_cast<uint8_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
  }
  out.resize(out.size() - (drop > out.size() ? out.size() : drop));
  return out;
}

/** A `maxp` and a `post`, which is every table a glyph name needs. */
std::vector<Table> named(uint16_t glyphs, std::vector<uint8_t> post) {
  return {
      {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(glyphs)},
      {GFNT_TAG('p', 'o', 's', 't'), std::move(post)},
  };
}

/** One glyph's name, or the result that came back instead. */
struct Name {
  GFNT_Result result = GFNT_ERR_INTERNAL;
  std::string text;
  size_t length = 0;
  const char * message = nullptr;

  Name(const GFNT_Face * face, uint32_t glyph,
      const GFNT_Allocator * allocator = nullptr) {
    char * name = nullptr;
    GFNT_Error error{};

    gfnt_error_clear(&error);
    result = gfnt_face_glyph_name(face, glyph, allocator, &name, &length,
        &error);
    if (result == GFNT_OK) {
      text = name;
    }
    message = error.message;
    gfnt_glyph_name_free(allocator, name);
  }
};

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
  // A `CFF ` face reports **true** as of phase 2, and this assertion is the
  // history of the question: it said true before the coverage sweep of
  // 2026-09-27, because the predicate counted the table; false after it, because
  // no interpreter could read one and the predicate answers "can asking for an
  // outline succeed"; and true again now, because one can. What stayed wrong
  // throughout the first period was that `CFF2` answered the other way for the
  // same reason - two answers to one question, which left
  // gfnt_face_strike_at() telling the caller of an OTTO font it had outlines to
  // scale and gfnt_face_glyph_outline() then refusing.
  EXPECT_TRUE(gfnt_face_has_outlines(cff.face));
  EXPECT_FALSE(gfnt_face_has_outlines(neither.face));
  EXPECT_FALSE(gfnt_face_has_outlines(nullptr));
}

TEST(Outlines, ACff2OnlyFaceHasNoneThisLibraryReads) {
  // Section 16 defers CFF2, and this is now the *only* reason a charstring face
  // answers no: `CFF ` says yes, so a test that could not tell "deferred" from
  // "not implemented yet" is one this pair of tests no longer has.
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



TEST(GlyphNames, FormatOnePointZeroIsTheStandardOrderAndStopsAtItsEnd) {
  // Format 1.0 has no index array and no strings: the font is *stating* that its
  // glyphs are the 258 standard ones, in order. A font that says so and then
  // claims more glyphs than that has contradicted itself, and the 259th glyph
  // has no name rather than a guessed one.
  Font font(named(300, gfnttest::build_post(0x00010000)));
  const Name first(font.face, 0);
  const Name last(font.face, 257);
  const Name past(font.face, 258);

  EXPECT_EQ(first.result, GFNT_OK) << first.message;
  EXPECT_EQ(first.text, ".notdef");
  EXPECT_EQ(last.result, GFNT_OK) << last.message;
  EXPECT_EQ(last.text, "dcroat");
  EXPECT_EQ(past.result, GFNT_ERR_INVALID);
  ASSERT_NE(past.message, nullptr);
  EXPECT_NE(std::string(past.message).find("258 standard glyphs"),
      std::string::npos) << past.message;
}

TEST(GlyphNames, TwoKindsOfUnsupportedAreTwoDifferentSentences) {
  // A font that says it has no names, and a font whose names are in a format
  // this library does not read, are both UNSUPPORTED - and a caller deciding
  // whether to look elsewhere needs to tell them apart (design.md section 5.6).
  Font stated(named(4, gfnttest::build_post(0x00030000)));
  Font unread(named(4, gfnttest::build_post(0x00040000)));
  const Name none(stated.face, 1);
  const Name other(unread.face, 1);

  EXPECT_EQ(none.result, GFNT_ERR_UNSUPPORTED);
  ASSERT_NE(none.message, nullptr);
  EXPECT_NE(std::string(none.message).find("format 3.0"), std::string::npos)
      << none.message;
  EXPECT_EQ(other.result, GFNT_ERR_UNSUPPORTED);
  ASSERT_NE(other.message, nullptr);
  EXPECT_NE(std::string(other.message).find("does not read"),
      std::string::npos) << other.message;
}

TEST(GlyphNames, AStoredNameIsFoundByWalkingTheOnesBeforeIt) {
  // Format 2.0 gives no offset table: the nth stored name is found by stepping
  // over the n - 1 before it, one Pascal length at a time. A test that asked
  // only for the first would never take a step.
  Font font(named(4, post_v2(4, {0, 258, 259, 260},
      {"alpha", "beta", "gamma"})));

  EXPECT_EQ(Name(font.face, 0).text, ".notdef") << "a standard index";
  EXPECT_EQ(Name(font.face, 1).text, "alpha");
  EXPECT_EQ(Name(font.face, 2).text, "beta");
  const Name third(font.face, 3);
  EXPECT_EQ(third.result, GFNT_OK) << third.message;
  EXPECT_EQ(third.text, "gamma");
  EXPECT_EQ(third.length, 5u);
}

TEST(GlyphNames, EveryWayAFormatTwoTableCanContradictItselfIsRefused) {
  // Each of these is a different read and a different arm, and each would
  // otherwise hand back whatever bytes happened to follow the table.
  Font no_count(named(4, post_v2(4, {}, {}, /*drop=*/2)));
  // Two indices where the count promises four, and nothing after them: the
  // array's own bytes run out. With a stored name behind it the read would
  // succeed and land in the string area instead, which is a different arm.
  Font short_array(named(4, post_v2(4, {0, 258}, {})));
  Font past_strings(named(4, post_v2(4, {0, 300, 258, 258}, {"alpha"})));
  Font long_name(named(4, post_v2(4, {0, 258, 0, 0}, {"alpha"}, /*drop=*/3)));

  const Name counted(no_count.face, 1);
  EXPECT_EQ(counted.result, GFNT_ERR_CORRUPT);
  ASSERT_NE(counted.message, nullptr);
  EXPECT_NE(std::string(counted.message).find("no glyph count"),
      std::string::npos) << counted.message;

  const Name beyond(short_array.face, 3);
  EXPECT_EQ(beyond.result, GFNT_ERR_CORRUPT);
  ASSERT_NE(beyond.message, nullptr);
  EXPECT_NE(std::string(beyond.message).find("shorter than its own glyph "
      "count"), std::string::npos) << beyond.message;

  const Name missing(past_strings.face, 1);
  EXPECT_EQ(missing.result, GFNT_ERR_CORRUPT);
  ASSERT_NE(missing.message, nullptr);
  EXPECT_NE(std::string(missing.message).find("past the last stored name"),
      std::string::npos) << missing.message;

  // A stored name whose length reaches past the table. The length query is what
  // catches this one, because gfnt_face_glyph_name() asks for the size before it
  // allocates - so the bound is checked on a pass that reads nothing out.
  const Name cut(long_name.face, 1);
  EXPECT_EQ(cut.result, GFNT_ERR_CORRUPT);
  ASSERT_NE(cut.message, nullptr);
  EXPECT_NE(std::string(cut.message).find("ends past the table"),
      std::string::npos) << cut.message;

  // And the reverse lookup reaches the same table with a buffer rather than a
  // counting pass, which is the other arm of the same check.
  uint32_t glyph = 0;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_for_name(long_name.face, "alpha", &glyph, &error),
      GFNT_ERR_INVALID) << "a name nobody can read is not a match";
}

TEST(GlyphNames, AGlyphPastThePostTablesOwnCountIsACallerError) {
  // `maxp` says six glyphs and `post` names four. The glyph exists; its name
  // does not, and that is INVALID rather than CORRUPT because nothing in the
  // file is malformed - the caller asked about a glyph this table does not
  // cover.
  Font font(named(6, post_v2(4, {0, 1, 2, 3}, {})));
  const Name named_glyph(font.face, 3);
  const Name past(font.face, 4);

  EXPECT_EQ(named_glyph.result, GFNT_OK) << named_glyph.message;
  EXPECT_EQ(past.result, GFNT_ERR_INVALID);
  ASSERT_NE(past.message, nullptr);
  EXPECT_NE(std::string(past.message).find("past the post table's own"),
      std::string::npos) << past.message;
}

TEST(GlyphNames, ThePostTableItselfMayBeUnreadable) {
  // Shorter than the header, so the parse fails rather than the lookup, and the
  // failure has to come back from the accessor rather than be turned into "this
  // font has no names".
  Font font(named(4, std::vector<uint8_t>(8, 0)));
  const Name name(font.face, 1);

  EXPECT_NE(name.result, GFNT_OK);
  EXPECT_NE(name.result, GFNT_ERR_UNSUPPORTED)
      << "a truncated table is not a font stating it has no names";
}

TEST(GlyphNames, TheReverseLookupScansPastGlyphsItCannotRead) {
  // Glyph 1's name is a Pascal string that runs past the table and glyph 2's is
  // fine. A scan that stopped at the first unreadable glyph would answer "no
  // glyph has that name" about a font that has it.
  std::vector<uint8_t> post = post_v2(3, {0, 258, 259}, {"alpha", "beta"});
  // Reach into the stored names and make the first one claim more bytes than
  // the table holds, leaving the second where it is.
  const size_t first_length = post.size() - (1 + 5 + 1 + 4);
  ASSERT_LT(first_length, post.size());
  post[first_length] = 250;

  Font font(named(3, post));
  uint32_t glyph = 0;
  GFNT_Error error{};

  EXPECT_NE(Name(font.face, 1).result, GFNT_OK) << "the planted name is intact";
  EXPECT_EQ(gfnt_face_glyph_for_name(font.face, ".notdef", &glyph, &error),
      GFNT_OK);
  EXPECT_EQ(glyph, 0u);
  EXPECT_EQ(gfnt_face_glyph_for_name(font.face, "nothing here", &glyph, &error),
      GFNT_ERR_INVALID);
  ASSERT_NE(error.message, nullptr);
  EXPECT_NE(std::string(error.message).find("no glyph in this font has that "
      "name"), std::string::npos) << error.message;

  // A font with no names at all says so once, rather than per glyph.
  Font none(named(3, gfnttest::build_post(0x00030000)));
  EXPECT_EQ(gfnt_face_glyph_for_name(none.face, "alpha", &glyph, &error),
      GFNT_ERR_UNSUPPORTED);
  ASSERT_NE(error.message, nullptr);
  EXPECT_NE(std::string(error.message).find("no glyph names to search"),
      std::string::npos) << error.message;

  EXPECT_EQ(gfnt_face_glyph_for_name(nullptr, "alpha", &glyph, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_for_name(font.face, nullptr, &glyph, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_for_name(font.face, "alpha", nullptr, &error),
      GFNT_ERR_INVALID);
}

TEST(GlyphNames, AllocatingTheNameCanFailAndIsReported) {
  Font font(named(4, post_v2(4, {0, 258, 0, 0}, {"alpha"})));

  for (size_t fail_at = 0; fail_at < 4; ++fail_at) {
    gfnttest::FailingAllocator allocator(fail_at, 0);
    const Name name(font.face, 1, allocator.get());

    if (name.result != GFNT_OK) {
      EXPECT_EQ(name.result, GFNT_ERR_OOM) << "at request " << fail_at;
      EXPECT_NE(name.message, nullptr);
    }
    allocator.stop_failing();
    EXPECT_EQ(allocator.live(), 0u) << "at request " << fail_at;
  }
  gfnt_glyph_name_free(nullptr, nullptr);
}

TEST(GlyphNames, TheDumpNamesEveryGlyphAndSaysWhenThereAreNone) {
  // `ttx_diff` compares this against fontTools' glyph order, so the shape of
  // every line here is part of that comparison - and until this test existed the
  // whole function ran only inside the container that does it.
  Font font(named(3, post_v2(3, {0, 258, 259}, {"alpha", "beta"})));
  gfnttest::CapturedOutput out;

  ASSERT_NE(out.get(), nullptr);
  EXPECT_EQ(gfnt_face_glyph_names_dump(font.face, out.get()), GFNT_OK);
  const std::string text = out.finish();
  EXPECT_NE(text.find("glyph name 0: '.notdef'"), std::string::npos) << text;
  EXPECT_NE(text.find("glyph name 1: 'alpha'"), std::string::npos) << text;
  EXPECT_NE(text.find("glyph name 2: 'beta'"), std::string::npos) << text;

  // A font that states it has no names says so on one line and stops: a
  // differential reads nothing at all as agreement.
  Font none(named(3, gfnttest::build_post(0x00030000)));
  gfnttest::CapturedOutput empty;
  ASSERT_NE(empty.get(), nullptr);
  EXPECT_EQ(gfnt_face_glyph_names_dump(none.face, empty.get()), GFNT_OK);
  EXPECT_NE(empty.finish().find("glyph names: none in this font"),
      std::string::npos);

  // And a glyph whose own name is unreadable is one line, not the end of the
  // dump: the other glyphs' names are still worth comparing.
  Font broken(named(3, post_v2(3, {0, 300, 258}, {"alpha"})));
  gfnttest::CapturedOutput partial;
  ASSERT_NE(partial.get(), nullptr);
  EXPECT_EQ(gfnt_face_glyph_names_dump(broken.face, partial.get()), GFNT_OK);
  const std::string mixed = partial.finish();
  EXPECT_NE(mixed.find("glyph name 1: unreadable"), std::string::npos)
      << mixed;
  EXPECT_NE(mixed.find("glyph name 2: 'alpha'"), std::string::npos) << mixed;

  EXPECT_EQ(gfnt_face_glyph_names_dump(nullptr, out.get()), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_names_dump(font.face, nullptr), GFNT_ERR_INVALID);
}

TEST(GlyphNames, TheDumpReportsEveryWriteFailure) {
  Font font(named(3, post_v2(3, {0, 258, 259}, {"alpha", "beta"})));
  size_t failures = 0;

  for (size_t allow = 0; allow < 6; allow++) {
    gfnttest::FailingSink sink(allow);
    ASSERT_NE(sink.get(), nullptr);
    if (gfnt_face_glyph_names_dump(font.face, sink.get()) == GFNT_ERR_IO) {
      failures++;
    }
  }
  EXPECT_EQ(failures, 3u) << "one line per glyph";

  Font none(named(3, gfnttest::build_post(0x00030000)));
  gfnttest::FailingSink sink(0);
  ASSERT_NE(sink.get(), nullptr);
  EXPECT_EQ(gfnt_face_glyph_names_dump(none.face, sink.get()), GFNT_ERR_IO);

  Font broken(named(3, post_v2(3, {0, 300, 258}, {"alpha"})));
  size_t unreadable = 0;
  for (size_t allow = 0; allow < 6; allow++) {
    gfnttest::FailingSink partial(allow);
    ASSERT_NE(partial.get(), nullptr);
    if (gfnt_face_glyph_names_dump(broken.face, partial.get())
        == GFNT_ERR_IO) {
      unreadable++;
    }
  }
  EXPECT_EQ(unreadable, 3u) << "the unreadable line is a write like any other";
}

TEST(GlyphNames, AFontWithNoGlyphCountCannotBeAskedAboutNames) {
  // Every name path asks how many glyphs there are first, and `maxp` is where
  // that comes from: without it the question has no answer, and neither the
  // lookup nor the dump may invent one.
  Font font({{GFNT_TAG('p', 'o', 's', 't'),
      gfnttest::build_post(0x00020000)}});
  uint32_t glyph = 0;
  GFNT_Error error{};

  EXPECT_NE(gfnt_face_glyph_for_name(font.face, ".notdef", &glyph, &error),
      GFNT_OK);
  EXPECT_NE(gfnt_face_glyph_names_dump(font.face, stdout), GFNT_OK);

  // And the null arguments, which are a caller error rather than a font's.
  char * name = nullptr;
  EXPECT_EQ(gfnt_face_glyph_name(nullptr, 0, nullptr, &name, nullptr, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_name(font.face, 0, nullptr, nullptr, nullptr,
      &error), GFNT_ERR_INVALID);
}

TEST(GlyphNames, AFontWithNoPostTableHasNoNamesAndSaysWhichIsMissing) {
  // `post` is optional, so its absence is not corruption - and the accessor has
  // to pass the table reader's own answer back rather than turning "there is no
  // such table" into "this glyph has no name".
  Font font({{GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(3)}});
  const Name name(font.face, 0);

  EXPECT_EQ(name.result, GFNT_ERR_UNSUPPORTED);
  ASSERT_NE(name.message, nullptr);
  EXPECT_NE(std::string(name.message).find("no such table"), std::string::npos)
      << name.message;
}

TEST(Strikes, AStrikeListThisLibraryCannotCountIsReportedByTheAccessorToo) {
  // gfnt_face_strike_count() refuses an EBLC it cannot parse, and
  // gfnt_face_strike_at() has to pass that on rather than fall through to "no
  // strikes, so use the outlines" - which would scale an outline for a caller
  // that asked for a bitmap.
  Font font({stub(GFNT_TAG('E', 'B', 'L', 'C')),
      stub(GFNT_TAG('E', 'B', 'D', 'T'))});
  GFNT_Strike strike{};
  GFNT_Error error{};

  EXPECT_NE(gfnt_face_strike_at(font.face, 0, &strike, &error), GFNT_OK);
  ASSERT_NE(error.message, nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
