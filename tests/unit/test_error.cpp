/**
 * @file
 *
 * Diagnostics: the table, the offset and the glyph that make a malformed font
 * actionable, and the dump that prints them.
 *
 * documentation/design.md section 5.6: "corrupt" is not a diagnostic.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <string>

TEST(Tag, MacroMatchesTheFileByteOrder) {
  // A tag is four bytes in the order they appear in the file, so that
  // GFNT_TAG and a big-endian read produce the same number everywhere.
  EXPECT_EQ(GFNT_TAG('c', 'm', 'a', 'p'), 0x636D6170u);
  EXPECT_EQ(GFNT_TAG('h', 'e', 'a', 'd'), 0x68656164u);
  // The space in "CFF " and the slash in "OS/2" are part of the tag.
  EXPECT_EQ(GFNT_TAG('C', 'F', 'F', ' '), 0x43464620u);
  EXPECT_EQ(GFNT_TAG('O', 'S', '/', '2'), 0x4F532F32u);
}

TEST(Tag, StringSpellsPrintableAscii) {
  char text[5];
  EXPECT_STREQ(gfnt_tag_string(GFNT_TAG('c', 'm', 'a', 'p'), text), "cmap");
  EXPECT_STREQ(gfnt_tag_string(GFNT_TAG('C', 'F', 'F', ' '), text), "CFF ");
  EXPECT_STREQ(gfnt_tag_string(GFNT_TAG('O', 'S', '/', '2'), text), "OS/2");
}

TEST(Tag, StringReplacesWhatATerminalWouldNotSurvive) {
  char text[5];
  // The sfnt version 1.0 "tag" is four non-printable bytes, and a font can
  // put any byte in a directory entry.
  EXPECT_STREQ(gfnt_tag_string(0x00010000u, text), "....");
  EXPECT_STREQ(gfnt_tag_string(0x7F80FF09u, text), "....");
}

TEST(Tag, StringRefusesNullRatherThanWriting) {
  EXPECT_EQ(gfnt_tag_string(GFNT_TAG('c', 'm', 'a', 'p'), nullptr), nullptr);
}

TEST(Error, ClearSaysNothingIsRecorded) {
  GFNT_Error error;
  std::memset(&error, 0xA5, sizeof error);
  gfnt_error_clear(&error);
  EXPECT_EQ(error.result, GFNT_OK);
  EXPECT_EQ(error.table, 0u);
  EXPECT_EQ(error.offset, 0u);
  EXPECT_EQ(error.glyph, GFNT_GLYPH_NONE) << "glyph 0 is .notdef, a real glyph";
  EXPECT_EQ(error.message, nullptr);
}

TEST(Error, ClearAcceptsNull) {
  gfnt_error_clear(nullptr);
}

TEST(Error, SetRecordsEveryFieldAndReturnsTheResult) {
  GFNT_Error error;
  gfnt_error_clear(&error);
  GFNT_Result result = gfnt_error_set(&error, GFNT_ERR_CORRUPT,
      GFNT_TAG('l', 'o', 'c', 'a'), 12, 7, "entry runs backwards");
  EXPECT_EQ(result, GFNT_ERR_CORRUPT)
      << "the result is returned so that a parser can report and return in "
         "one statement";
  EXPECT_EQ(error.result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(error.table, GFNT_TAG('l', 'o', 'c', 'a'));
  EXPECT_EQ(error.offset, 12u);
  EXPECT_EQ(error.glyph, 7u);
  EXPECT_STREQ(error.message, "entry runs backwards");
}

TEST(Error, SetWithNoDiagnosticStillReturnsTheResult) {
  // Every parser takes an optional GFNT_Error *; NULL means the caller does
  // not want one, and the result code is the same either way.
  EXPECT_EQ(gfnt_error_set(nullptr, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
                "too many tables"),
      GFNT_ERR_LIMIT);
}

TEST(ErrorDump, ClearedDiagnosticSaysSo) {
  GFNT_Error error;
  gfnt_error_clear(&error);
  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  EXPECT_EQ(gfnt_error_dump(&error, out.get()), GFNT_OK);
  EXPECT_EQ(out.finish(), "no error recorded\n");
}

TEST(ErrorDump, NamesTheTableTheOffsetTheGlyphAndTheMessage) {
  GFNT_Error error;
  gfnt_error_set(&error, GFNT_ERR_CORRUPT, GFNT_TAG('c', 'm', 'a', 'p'), 42,
      9, "idRangeOffset leaves the subtable");
  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  EXPECT_EQ(gfnt_error_dump(&error, out.get()), GFNT_OK);
  std::string text = out.finish();
  EXPECT_NE(text.find("cmap"), std::string::npos) << text;
  EXPECT_NE(text.find("42"), std::string::npos) << text;
  EXPECT_NE(text.find("glyph 9"), std::string::npos) << text;
  EXPECT_NE(text.find("idRangeOffset leaves the subtable"), std::string::npos)
      << text;
  EXPECT_NE(text.find(gfnt_result_string(GFNT_ERR_CORRUPT)),
      std::string::npos)
      << text;
}

TEST(ErrorDump, OmitsWhatWasNotRecorded) {
  GFNT_Error error;
  gfnt_error_set(&error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE, nullptr);
  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  EXPECT_EQ(gfnt_error_dump(&error, out.get()), GFNT_OK);
  std::string text = out.finish();
  EXPECT_EQ(text.find("table"), std::string::npos) << text;
  EXPECT_EQ(text.find("glyph"), std::string::npos) << text;
}

TEST(ErrorDump, RefusesNullArguments) {
  GFNT_Error error;
  gfnt_error_clear(&error);
  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  EXPECT_EQ(gfnt_error_dump(nullptr, out.get()), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_error_dump(&error, nullptr), GFNT_ERR_INVALID);
}

// Every fprintf in the dumper is checked, and until this sweep existed not one
// of those arms had ever run. Walking the failure through the dump reaches
// each of them in turn; the floor at the end is what keeps the sweep from
// going quietly vacuous if the dumper ever stops writing through fprintf.
TEST(ErrorDump, ReportsAWriteFailureAtEveryPoint) {
  GFNT_Error error;
  gfnt_error_set(&error, GFNT_ERR_CORRUPT, GFNT_TAG('g', 'l', 'y', 'f'), 8, 3,
      "flag stream ends mid-glyph");
  size_t failures = 0;

  for (size_t allow = 0; allow < 8; ++allow) {
    gfnttest::FailingSink sink(allow);
    ASSERT_NE(sink.get(), nullptr);
    GFNT_Result result = gfnt_error_dump(&error, sink.get());
    if (sink.failed()) {
      EXPECT_EQ(result, GFNT_ERR_IO) << "with " << allow << " writes allowed";
      failures++;
    }
    else {
      EXPECT_EQ(result, GFNT_OK) << "with " << allow << " writes allowed";
    }
  }
  EXPECT_GE(failures, 5u) << "the sweep found no write to fail; the dumper is "
                             "not writing through fprintf any more";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
