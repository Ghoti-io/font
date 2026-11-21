/**
 * @file
 *
 * Glyph names to text by the Adobe Glyph List specification: the specification's
 * own examples, the rules it gives for what is not a name, and every entry of the
 * generated tables answering for itself (which proves the binary search reaches the
 * first and the last and every one between, and that the table is sorted).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/font/glyph.h>

#include "../../src/glyph/agl_data.h"

namespace {

std::vector<uint32_t> text(const char * name, bool zapf = false) {
  size_t count = 99;
  const GFNT_Result sized = gfnt_glyph_name_to_unicode(name, zapf, nullptr, 0, &count, nullptr);

  EXPECT_EQ(sized, count == 0 ? GFNT_OK : GFNT_ERR_LIMIT) << name;
  std::vector<uint32_t> out(count);
  size_t again = 0;

  EXPECT_EQ(gfnt_glyph_name_to_unicode(name, zapf, out.data(), out.size(), &again, nullptr),
      GFNT_OK) << name;
  EXPECT_EQ(again, count) << name;
  return out;
}

using V = std::vector<uint32_t>;

}  // namespace

TEST(Agl, TheSpecificationsOwnExamples) {
  EXPECT_EQ(text("Lcommaaccent"), V({0x013B}));
  EXPECT_EQ(text("uni20AC0308"), V({0x20AC, 0x0308}));
  EXPECT_EQ(text("u1040C"), V({0x1040C}));
  EXPECT_EQ(text("uniD801DC0C"), V());
  EXPECT_EQ(text("uni20ac"), V());
  EXPECT_EQ(text("Lcommaaccent_uni20AC0308_u1040C.alternate"), V({0x013B, 0x20AC, 0x0308, 0x1040C}));
  EXPECT_EQ(text("uni013B"), V({0x013B}));
  EXPECT_EQ(text("foo"), V());
  EXPECT_EQ(text(".notdef"), V());
  EXPECT_EQ(text("A_"), V({0x41}));
  EXPECT_EQ(text("A.sc"), V({0x41}));
  EXPECT_EQ(text("A__B"), V({0x41, 0x42}));
  EXPECT_EQ(text(""), V());
}

TEST(Agl, TheFormsThatAreNotNames) {
  EXPECT_EQ(text("uni"), V());
  EXPECT_EQ(text("uni013"), V());         // not a multiple of four digits
  EXPECT_EQ(text("uni013B0"), V());
  EXPECT_EQ(text("uniD7FF"), V({0xD7FF}));
  EXPECT_EQ(text("uniD800"), V());        // a surrogate
  EXPECT_EQ(text("uniE000"), V({0xE000}));
  EXPECT_EQ(text("uni0041D800"), V());    // one bad group rejects the component
  EXPECT_EQ(text("u0041"), V({0x41}));
  EXPECT_EQ(text("u10FFFF"), V({0x10FFFF}));
  EXPECT_EQ(text("u110000"), V());
  EXPECT_EQ(text("u00041"), V({0x41}));
  EXPECT_EQ(text("u041"), V());           // too few digits
  EXPECT_EQ(text("u1234567"), V());       // too many
  EXPECT_EQ(text("uD800"), V());
  EXPECT_EQ(text("g23"), V());
  EXPECT_EQ(text("cid42"), V());
}

TEST(Agl, ANameThatStandsForSeveralCodepoints) {
  EXPECT_EQ(text("dalethatafpatah"), V({0x05D3, 0x05B2}));
}

TEST(Agl, ZapfDingbatsHasItsOwnListAndOnlyWhenAsked) {
  EXPECT_EQ(text("a100", true), V({0x275E}));
  EXPECT_EQ(text("a100", false), V());
  EXPECT_EQ(text("A", true), V());
  EXPECT_EQ(text("uni0041", true), V({0x41}));  // the algebraic forms hold in either
}

TEST(Agl, ABufferTooSmallGetsWhatFitsAndTheFullCount) {
  uint32_t out[2] = {0, 0};
  size_t count = 0;

  EXPECT_EQ(gfnt_glyph_name_to_unicode("A_B_C", false, out, 2, &count, nullptr), GFNT_ERR_LIMIT);
  EXPECT_EQ(count, 3u);
  EXPECT_EQ(out[0], 0x41u);
  EXPECT_EQ(out[1], 0x42u);
  EXPECT_EQ(gfnt_glyph_name_to_unicode(nullptr, false, out, 2, &count, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_glyph_name_to_unicode("A", false, out, 2, nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_glyph_name_to_unicode("A", false, nullptr, 2, &count, nullptr), GFNT_ERR_INVALID);
}

TEST(Agl, EveryEntryOfEveryTableAnswersForItselfAndTheTablesAreSorted) {
  const size_t agl_count = sizeof gfnt_agl_entries / sizeof gfnt_agl_entries[0];
  const size_t zapf_count = sizeof gfnt_zapf_entries / sizeof gfnt_zapf_entries[0];

  EXPECT_GT(agl_count, 4000u);
  EXPECT_GT(zapf_count, 200u);
  for (int zapf = 0; zapf < 2; zapf++) {
    const GFNT_AglEntry * entries = zapf ? gfnt_zapf_entries : gfnt_agl_entries;
    const uint32_t * codepoints = zapf ? gfnt_zapf_codepoints : gfnt_agl_codepoints;
    const size_t count = zapf ? zapf_count : agl_count;

    for (size_t i = 0; i < count; i++) {
      if (i) {
        ASSERT_LT(std::strcmp(entries[i - 1].name, entries[i].name), 0) << entries[i].name;
      }
      ASSERT_GE(entries[i].count, 1);
      V expected(codepoints + entries[i].first, codepoints + entries[i].first + entries[i].count);
      ASSERT_EQ(text(entries[i].name, zapf != 0), expected) << entries[i].name;
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
