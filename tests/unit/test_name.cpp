/**
 * @file
 *
 * `name`: the records, the UTF-8 decode, and the preference order.
 *
 * documentation/design.md section 7.2 and M15.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <algorithm>
#include <string>
#include <vector>

#include <ghoti.io/font/name.h>

using gfnttest::NameRecord;
using gfnttest::Table;

namespace {

struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  explicit Font(std::vector<Table> tables)
      : bytes(gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables)) {
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

Table name_table(std::vector<uint8_t> data) {
  return Table{GFNT_TAG('n', 'a', 'm', 'e'), std::move(data)};
}

/** The decoded name, or "" with the result reported through `result`. */
std::string name_of(GFNT_Face * face, uint16_t name_id, GFNT_Result * result,
    uint16_t language = GFNT_LANGUAGE_ANY) {
  char * text = nullptr;
  size_t length = 0;
  GFNT_Result got = gfnt_face_name(face, name_id, language, nullptr, &text,
      &length, nullptr);
  if (result) {
    *result = got;
  }
  if (got != GFNT_OK) {
    return std::string();
  }
  std::string copy(text, length);
  EXPECT_EQ(std::strlen(text), length) << "the string is NUL-terminated";
  gfnt_name_free(nullptr, text);
  return copy;
}

} // namespace

TEST(Name, ReadsEveryRecordTheFontLists) {
  Font font({name_table(gfnttest::build_name({
      {3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti Sans")},
      {3, 1, 0x409, GFNT_NAME_SUBFAMILY, gfnttest::utf16be("Regular")},
      {1, 0, 0, GFNT_NAME_FAMILY, gfnttest::raw_bytes("Ghoti Sans")},
  }))});

  size_t count = 0;
  ASSERT_EQ(gfnt_face_name_count(font.face, &count, nullptr), GFNT_OK);
  ASSERT_EQ(count, 3u);

  GFNT_NameRecord record{};
  ASSERT_EQ(gfnt_face_name_at(font.face, 1, &record, nullptr), GFNT_OK);
  EXPECT_EQ(record.platform_id, 3u);
  EXPECT_EQ(record.encoding_id, 1u);
  EXPECT_EQ(record.language_id, 0x409u);
  EXPECT_EQ(record.name_id, GFNT_NAME_SUBFAMILY);
  EXPECT_EQ(record.length, 14u) << "seven characters of UTF-16";

  EXPECT_EQ(gfnt_face_name_at(font.face, 3, &record, nullptr),
      GFNT_ERR_INVALID);
}

TEST(Name, DecodesUtf16BeToUtf8) {
  // M15: the same bytes read as Latin-1 give "G\0h\0o\0t\0i\0", which is what a
  // parser that ignores the encoding IDs produces.
  Font font({name_table(gfnttest::build_name(
      {{3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti Sans")}}))});

  GFNT_Result result = GFNT_ERR_INTERNAL;
  EXPECT_EQ(name_of(font.face, GFNT_NAME_FAMILY, &result), "Ghoti Sans");
  EXPECT_EQ(result, GFNT_OK);
}

TEST(Name, DecodesBeyondTheBasicMultilingualPlaneThroughSurrogates) {
  // U+1F600, as a surrogate pair, is four UTF-8 bytes. A decoder that copies
  // code units straight through produces two replacement characters or two
  // three-byte sequences in the surrogate range, neither of which is UTF-8.
  std::vector<uint8_t> text;
  gfnttest::put_u16(text, 0x0041);  // 'A'
  gfnttest::put_u16(text, 0xD83D);  // high surrogate
  gfnttest::put_u16(text, 0xDE00);  // low surrogate
  gfnttest::put_u16(text, 0x00E9);  // 'é', two UTF-8 bytes
  gfnttest::put_u16(text, 0x4E2D);  // a Han character, three UTF-8 bytes

  Font font({name_table(gfnttest::build_name(
      {{3, 1, 0x409, GFNT_NAME_FAMILY, text}}))});

  GFNT_Result result = GFNT_ERR_INTERNAL;
  std::string decoded = name_of(font.face, GFNT_NAME_FAMILY, &result);
  ASSERT_EQ(result, GFNT_OK);
  EXPECT_EQ(decoded, "A\xF0\x9F\x98\x80\xC3\xA9\xE4\xB8\xAD");
  EXPECT_EQ(decoded.size(), 1u + 4u + 2u + 3u)
      << "one, four, two and three bytes";
}

TEST(Name, AnUnpairedSurrogateIsCorrupt) {
  std::vector<uint8_t> high_alone;
  gfnttest::put_u16(high_alone, 0xD83D);
  gfnttest::put_u16(high_alone, 0x0041);
  std::vector<uint8_t> low_alone;
  gfnttest::put_u16(low_alone, 0xDE00);

  for (const auto & text : {high_alone, low_alone}) {
    Font font({name_table(gfnttest::build_name(
        {{3, 1, 0x409, GFNT_NAME_FAMILY, text}}))});
    GFNT_NameRecord record{};
    ASSERT_EQ(gfnt_face_name_at(font.face, 0, &record, nullptr), GFNT_OK);
    char * out = nullptr;
    EXPECT_EQ(gfnt_face_name_decode(font.face, &record, nullptr, &out, nullptr,
                  &font.error),
        GFNT_ERR_CORRUPT);
    EXPECT_EQ(out, nullptr);
    EXPECT_NE(font.error.message, nullptr);
  }
}

TEST(Name, AnOddLengthUtf16RecordIsCorrupt) {
  Font font({name_table(gfnttest::build_name(
      {{3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::raw_bytes("abc")}}))});
  GFNT_NameRecord record{};
  ASSERT_EQ(gfnt_face_name_at(font.face, 0, &record, nullptr), GFNT_OK);
  char * out = nullptr;
  EXPECT_EQ(gfnt_face_name_decode(font.face, &record, nullptr, &out, nullptr,
                nullptr),
      GFNT_ERR_CORRUPT);
}

TEST(Name, AMacintoshRecordDecodesAsFarAsAscii) {
  Font font({name_table(gfnttest::build_name(
      {{1, 0, 0, GFNT_NAME_FAMILY, gfnttest::raw_bytes("Ghoti Sans")}}))});

  GFNT_Result result = GFNT_ERR_INTERNAL;
  EXPECT_EQ(name_of(font.face, GFNT_NAME_FAMILY, &result), "Ghoti Sans");
  EXPECT_EQ(result, GFNT_OK);
}

TEST(Name, AMacintoshRecordAboveAsciiIsDecodedThroughItsTable) {
  // This test asserted a *refusal* until the vectors were generated: decoding
  // needed the 128-entry Mac Roman table, and section 14's rule is that a vector
  // comes from an oracle rather than from memory, so guessing Latin-1 - which
  // would have put a wrong character in a family name - was worse than saying
  // so. The table is generated now, and 0xA5 in Mac Roman is U+2022 BULLET,
  // which Latin-1 would have made U+00A5 YEN SIGN. The two readings are the
  // whole reason the table was not written from memory.
  Font font({name_table(gfnttest::build_name(
      {{1, 0, 0, GFNT_NAME_FAMILY, {'G', 'h', 0xA5, 'i'}}}))});
  GFNT_NameRecord record{};
  ASSERT_EQ(gfnt_face_name_at(font.face, 0, &record, nullptr), GFNT_OK);
  char * out = nullptr;
  ASSERT_EQ(gfnt_face_name_decode(font.face, &record, nullptr, &out, nullptr,
                &font.error),
      GFNT_OK);
  EXPECT_STREQ(out, "Gh\xE2\x80\xA2i");
  gfnt_name_free(nullptr, out);
}

TEST(Name, AnEncodingThisLibraryDoesNotDecodeIsUnsupported) {
  // Macintosh encoding 1 is Japanese: multi-byte, a data set of its own, and
  // refused rather than guessed a byte at a time. Encoding 6 used to be the
  // subject here and is Macintosh Greek, which is now tabulated - a test whose
  // subject has become supported is a test that would have started asserting
  // the wrong thing.
  Font font({name_table(gfnttest::build_name(
      {{1, 1, 0, GFNT_NAME_FAMILY, gfnttest::raw_bytes("whatever")}}))});
  GFNT_NameRecord record{};
  ASSERT_EQ(gfnt_face_name_at(font.face, 0, &record, nullptr), GFNT_OK);
  char * out = nullptr;
  EXPECT_EQ(gfnt_face_name_decode(font.face, &record, nullptr, &out, nullptr,
                &font.error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(font.error.message, nullptr);
}

TEST(Name, TheMacintoshLanguageDecidesWhichTableDecodesEncodingZero) {
  // platEncID 0 is Mac Roman for most languages and five other encodings for
  // thirteen of them. 0xDB is U+20AC EURO SIGN in Mac Roman and U+20AC in
  // Icelandic too, so the byte that separates them is 0xA0: U+2020 DAGGER in
  // Roman and U+00DD LATIN CAPITAL LETTER Y WITH ACUTE in Icelandic.
  //
  // Two records that differ only in their language ID must therefore decode
  // differently. A library that read encoding 0 as Mac Roman throughout would
  // pass every other test in this file.
  Font font({name_table(gfnttest::build_name({
      {1, 0, 0, GFNT_NAME_FAMILY, {'x', 0xA0}},
      {1, 0, 15, GFNT_NAME_FAMILY, {'x', 0xA0}},
  }))});

  GFNT_NameRecord roman{};
  GFNT_NameRecord iceland{};
  ASSERT_EQ(gfnt_face_name_at(font.face, 0, &roman, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_name_at(font.face, 1, &iceland, nullptr), GFNT_OK);
  ASSERT_EQ(roman.language_id, 0u);
  ASSERT_EQ(iceland.language_id, 15u);

  char * first = nullptr;
  char * second = nullptr;
  ASSERT_EQ(gfnt_face_name_decode(font.face, &roman, nullptr, &first, nullptr,
                nullptr),
      GFNT_OK);
  ASSERT_EQ(gfnt_face_name_decode(font.face, &iceland, nullptr, &second,
                nullptr, nullptr),
      GFNT_OK);
  EXPECT_STREQ(first, "x\xE2\x80\xA0");  // U+2020 DAGGER
  EXPECT_STREQ(second, "x\xC3\x9D");     // U+00DD Y WITH ACUTE
  EXPECT_STRNE(first, second);
  gfnt_name_free(nullptr, first);
  gfnt_name_free(nullptr, second);
}

TEST(Name, ThePreferenceOrderTakesWindowsEnglishFirst) {
  // The font lists them in the reverse of the preference order, so a parser
  // taking the first matching record fails here.
  Font font({name_table(gfnttest::build_name({
      {1, 0, 0, GFNT_NAME_FAMILY, gfnttest::raw_bytes("Macintosh")},
      {0, 3, 0, GFNT_NAME_FAMILY, gfnttest::utf16be("Unicode")},
      {3, 1, 0x40C, GFNT_NAME_FAMILY, gfnttest::utf16be("Windows French")},
      {3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Windows English")},
  }))});

  GFNT_Result result = GFNT_ERR_INTERNAL;
  EXPECT_EQ(name_of(font.face, GFNT_NAME_FAMILY, &result), "Windows English");
  EXPECT_EQ(result, GFNT_OK);
}

TEST(Name, ThePreferenceOrderFallsThroughPlatformByPlatform) {
  struct Case {
    std::vector<NameRecord> records;
    const char * expected;
  };
  const std::vector<Case> cases = {
      {{{3, 1, 0x40C, GFNT_NAME_FAMILY, gfnttest::utf16be("Windows French")},
           {0, 3, 0, GFNT_NAME_FAMILY, gfnttest::utf16be("Unicode")}},
          "Windows French"},
      {{{0, 3, 0, GFNT_NAME_FAMILY, gfnttest::utf16be("Unicode")},
           {1, 0, 0, GFNT_NAME_FAMILY, gfnttest::raw_bytes("Macintosh")}},
          "Unicode"},
      {{{1, 0, 5, GFNT_NAME_FAMILY, gfnttest::raw_bytes("Mac Italian")},
           {1, 0, 0, GFNT_NAME_FAMILY, gfnttest::raw_bytes("Mac English")}},
          "Mac English"},
  };

  for (const auto & test : cases) {
    Font font({name_table(gfnttest::build_name(test.records))});
    GFNT_Result result = GFNT_ERR_INTERNAL;
    EXPECT_EQ(name_of(font.face, GFNT_NAME_FAMILY, &result), test.expected);
    EXPECT_EQ(result, GFNT_OK);
  }
}

TEST(Name, ARequestedLanguageNarrowsToThatLanguage) {
  Font font({name_table(gfnttest::build_name({
      {3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Windows English")},
      {3, 1, 0x40C, GFNT_NAME_FAMILY, gfnttest::utf16be("Windows French")},
  }))});

  GFNT_Result result = GFNT_ERR_INTERNAL;
  EXPECT_EQ(name_of(font.face, GFNT_NAME_FAMILY, &result, 0x40C),
      "Windows French");
  EXPECT_EQ(result, GFNT_OK);

  // A language no record carries is not a fallback to another language: a
  // caller that asked for Japanese would rather know it is absent.
  EXPECT_EQ(name_of(font.face, GFNT_NAME_FAMILY, &result, 0x411), "");
  EXPECT_EQ(result, GFNT_ERR_UNSUPPORTED);
}

TEST(Name, ARecordThisLibraryCannotDecodeIsPassedOverForOneItCan) {
  Font font({name_table(gfnttest::build_name({
      {3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::raw_bytes("odd")},
      {0, 3, 0, GFNT_NAME_FAMILY, gfnttest::utf16be("Unicode")},
  }))});

  // The Windows record wins the preference order and then fails to decode, so
  // this is the case where the order and the decode disagree. The Windows
  // record is the one chosen, and its corruption is reported rather than being
  // papered over with the Unicode record - the alternative hides a broken font.
  GFNT_Result result = GFNT_ERR_INTERNAL;
  name_of(font.face, GFNT_NAME_FAMILY, &result);
  EXPECT_EQ(result, GFNT_ERR_CORRUPT);
}

TEST(Name, ANameTheFontDoesNotCarryIsUnsupported) {
  Font font({name_table(gfnttest::build_name(
      {{3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti Sans")}}))});

  GFNT_Result result = GFNT_ERR_INTERNAL;
  EXPECT_EQ(name_of(font.face, GFNT_NAME_POSTSCRIPT, &result), "");
  EXPECT_EQ(result, GFNT_ERR_UNSUPPORTED);
}

TEST(Name, AFontWithNoNameTableIsUnsupported) {
  // Which is what a font embedded in a PDF looks like.
  Font font({Table{GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(3)}});
  size_t count = 0;
  EXPECT_EQ(gfnt_face_name_count(font.face, &count, nullptr),
      GFNT_ERR_UNSUPPORTED);
  GFNT_Result result = GFNT_ERR_INTERNAL;
  EXPECT_EQ(name_of(font.face, GFNT_NAME_FAMILY, &result), "");
  EXPECT_EQ(result, GFNT_ERR_UNSUPPORTED);
}

TEST(Name, FormatOneReadsLikeFormatZero) {
  // Format 1 adds language-tag records between the name records and the string
  // storage; the storage is found by its own offset, so nothing here depends on
  // them.
  Font font({name_table(gfnttest::build_name(
      {{3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti Sans")}}, 1))});
  GFNT_Result result = GFNT_ERR_INTERNAL;
  EXPECT_EQ(name_of(font.face, GFNT_NAME_FAMILY, &result), "Ghoti Sans");
}

TEST(Name, AFormatThisLibraryDoesNotReadIsUnsupported) {
  Font font({name_table(gfnttest::build_name(
      {{3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("x")}}, 2))});
  size_t count = 0;
  EXPECT_EQ(gfnt_face_name_count(font.face, &count, &font.error),
      GFNT_ERR_UNSUPPORTED);
}

TEST(Name, MoreRecordsThanTheLimitIsRefused) {
  std::vector<NameRecord> records;
  for (int i = 0; i < 4; i++) {
    records.push_back({3, 1, 0x409, static_cast<uint16_t>(i),
        gfnttest::utf16be("x")});
  }
  std::vector<uint8_t> bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {name_table(gfnttest::build_name(records))});

  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_name_records = 3;

  GFNT_Blob * blob = nullptr;
  ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
      GFNT_OK);
  GFNT_Face * face = nullptr;
  ASSERT_EQ(gfnt_face_load(blob, 0, &limits, nullptr, &face, nullptr), GFNT_OK);

  size_t count = 0;
  GFNT_Error error;
  EXPECT_EQ(gfnt_face_name_count(face, &count, &error), GFNT_ERR_LIMIT);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
}

TEST(Name, AStringStorageOffsetPastTheTableIsCorrupt) {
  std::vector<uint8_t> name = gfnttest::build_name(
      {{3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti")}});
  gfnttest::patch_u16(name, 4, 0xFFFF);
  Font font({name_table(name)});

  size_t count = 0;
  EXPECT_EQ(gfnt_face_name_count(font.face, &count, &font.error),
      GFNT_ERR_CORRUPT);
}

TEST(Name, ARecordWhoseTextLeavesTheTableIsCorrupt) {
  std::vector<uint8_t> name = gfnttest::build_name(
      {{3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti")}});
  gfnttest::patch_u16(name, gfnttest::name_record_offset(0) + 8, 0xFF00);
  Font font({name_table(name)});

  GFNT_NameRecord record{};
  ASSERT_EQ(gfnt_face_name_at(font.face, 0, &record, nullptr), GFNT_OK);
  char * out = nullptr;
  EXPECT_EQ(gfnt_face_name_decode(font.face, &record, nullptr, &out, nullptr,
                nullptr),
      GFNT_ERR_CORRUPT);
  EXPECT_EQ(out, nullptr);
}

TEST(Name, AnEmptyRecordDecodesToAnEmptyString) {
  Font font({name_table(gfnttest::build_name(
      {{3, 1, 0x409, GFNT_NAME_FAMILY, {}}}))});

  char * text = nullptr;
  size_t length = 1;
  GFNT_NameRecord record{};
  ASSERT_EQ(gfnt_face_name_at(font.face, 0, &record, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_name_decode(font.face, &record, nullptr, &text, &length,
                nullptr),
      GFNT_OK);
  ASSERT_NE(text, nullptr);
  EXPECT_EQ(length, 0u);
  EXPECT_STREQ(text, "");
  gfnt_name_free(nullptr, text);
}

TEST(Name, EveryAllocationFailureIsReportedAndLeaksNothing) {
  std::vector<uint8_t> bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {name_table(gfnttest::build_name(
          {{3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti Sans")}}))});
  size_t failures = 0;

  for (size_t n = 0; n < 4; n++) {
    GFNT_Blob * blob = nullptr;
    ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                  GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
        GFNT_OK);
    GFNT_Face * face = nullptr;
    ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr),
        GFNT_OK);

    gfnttest::FailingAllocator allocator(n);
    char * text = nullptr;
    GFNT_Result result = gfnt_face_name(face, GFNT_NAME_FAMILY,
        GFNT_LANGUAGE_ANY, allocator.get(), &text, nullptr, nullptr);
    if (result == GFNT_OK) {
      ASSERT_NE(text, nullptr);
      gfnt_name_free(allocator.get(), text);
    }
    else {
      EXPECT_EQ(result, GFNT_ERR_OOM) << "at refusal " << n;
      EXPECT_EQ(text, nullptr);
      failures++;
    }
    EXPECT_EQ(allocator.live(), 0u) << "at refusal " << n;
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  EXPECT_EQ(failures, 1u) << "a decode makes exactly one allocation";
}

TEST(NameDump, NamesEveryRecordAndSaysWhenOneCannotBeDecoded) {
  Font font({name_table(gfnttest::build_name({
      {3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti Sans")},
      // Macintosh Japanese: multi-byte, and the only kind of record left that
      // this library declines. Encoding 6 stood here and is Greek, which is now
      // tabulated - so the dump would have decoded it and the assertion below
      // would have been testing nothing.
      {1, 1, 0, GFNT_NAME_FAMILY, gfnttest::raw_bytes("whatever")},
  }))});

  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  ASSERT_EQ(gfnt_face_name_dump(font.face, out.get()), GFNT_OK);
  std::string text = out.finish();

  EXPECT_NE(text.find("2 records"), std::string::npos) << text;
  EXPECT_NE(text.find("'Ghoti Sans'"), std::string::npos) << text;
  EXPECT_NE(text.find("not decodable"), std::string::npos) << text;
}

TEST(NameDump, EscapesTheControlCharactersARealNameCarries) {
  // Every multi-line copyright notice has newlines in it, and a dump whose
  // records can run over several lines is one no tool can read back.
  // tools/oracle/ttx_diff.py parses these lines, which is what makes this the
  // difference between a dump and an interface.
  Font font({name_table(gfnttest::build_name({
      {3, 1, 0x409, GFNT_NAME_COPYRIGHT,
          gfnttest::utf16be("Line one\nLine two\tand\\back")},
  }))});

  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  ASSERT_EQ(gfnt_face_name_dump(font.face, out.get()), GFNT_OK);
  std::string text = out.finish();

  EXPECT_NE(text.find("'Line one\\nLine two\\tand\\\\back'"),
      std::string::npos)
      << text;
  EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 2)
      << "a header line and one record line, whatever the record contains:\n"
      << text;
}

TEST(NameDump, LeavesRealTextAlone) {
  // Only C0 and DEL are escaped. The decoder's output is UTF-8 by
  // construction, and mangling it here would make the dump a worse record than
  // the font.
  std::vector<uint8_t> utf16;
  gfnttest::put_u16(utf16, 0x00E9);  // 'é'
  gfnttest::put_u16(utf16, 0x4E2D);  // a Han character
  Font font({name_table(gfnttest::build_name(
      {{3, 1, 0x409, GFNT_NAME_FAMILY, utf16}}))});

  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  ASSERT_EQ(gfnt_face_name_dump(font.face, out.get()), GFNT_OK);
  EXPECT_NE(out.finish().find("'\xC3\xA9\xE4\xB8\xAD'"), std::string::npos);
}

TEST(NameDump, RefusesNullsAndReportsEveryWriteFailure) {
  Font font({name_table(gfnttest::build_name({
      {3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti Sans")},
      {3, 1, 0x409, GFNT_NAME_SUBFAMILY, gfnttest::utf16be("Regular")},
  }))});

  EXPECT_EQ(gfnt_face_name_dump(nullptr, nullptr), GFNT_ERR_INVALID);

  size_t failures = 0;
  for (size_t allow = 0; allow < 9; allow++) {
    gfnttest::FailingSink sink(allow);
    ASSERT_NE(sink.get(), nullptr);
    if (gfnt_face_name_dump(font.face, sink.get()) == GFNT_ERR_IO) {
      failures++;
    }
  }
  // One header line, then three writes per record: the prefix, the text, and
  // the closing quote. The text is one write because neither of these names has
  // anything to escape - which is the property the escaping is written in runs
  // for, and the reason this count is derivable rather than guessed.
  EXPECT_EQ(failures, 7u) << "one header and three writes for each of two "
                             "records";
}

TEST(Name, EveryEntryPointRefusesNullArguments) {
  Font font({name_table(gfnttest::build_name(
      {{3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti")}}))});
  GFNT_NameRecord record{};
  ASSERT_EQ(gfnt_face_name_at(font.face, 0, &record, nullptr), GFNT_OK);
  size_t count = 0;
  char * text = nullptr;

  EXPECT_EQ(gfnt_face_name_count(nullptr, &count, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_name_count(font.face, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_name_at(nullptr, 0, &record, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_name_at(font.face, 0, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_name_decode(nullptr, &record, nullptr, &text, nullptr,
                nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_name_decode(font.face, nullptr, nullptr, &text, nullptr,
                nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_name_decode(font.face, &record, nullptr, nullptr, nullptr,
                nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_name(nullptr, GFNT_NAME_FAMILY, GFNT_LANGUAGE_ANY,
                nullptr, &text, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_name(font.face, GFNT_NAME_FAMILY, GFNT_LANGUAGE_ANY,
                nullptr, nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
  gfnt_name_free(nullptr, nullptr);
}

TEST(Name, ATableCutToEveryLengthIsRefusedRatherThanPartlyRead) {
  // Two reads sit behind every record - the header that says how many there are,
  // and the twelve bytes of the record itself - and a cut between them is the
  // case where a count promises what the table cannot hold.
  const std::vector<uint8_t> whole = gfnttest::build_name({
      {3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti Sans")},
      {3, 1, 0x409, GFNT_NAME_SUBFAMILY, gfnttest::utf16be("Regular")},
  });
  size_t refusals = 0;

  for (size_t cut = 0; cut < whole.size(); cut++) {
    Font font({name_table(std::vector<uint8_t>(whole.begin(),
        whole.begin() + cut))});
    size_t count = 0;
    GFNT_NameRecord record{};
    GFNT_Error error{};
    bool refused = gfnt_face_name_count(font.face, &count, &error) != GFNT_OK;

    // Asked whatever the count said, because the record accessor opens the table
    // itself: guarding these calls on the count having worked would leave that
    // path unwalked for every cut short enough to lose the header.
    for (size_t i = 0; i < 2; i++) {
      refused = gfnt_face_name_at(font.face, i, &record, &error) != GFNT_OK
          || refused;
    }
    // The strings live after the records, so a cut that takes the storage leaves
    // the records readable and only the *text* missing. Asking for the text is
    // what makes those cuts visible: without it, half of this sweep would report
    // a font behaving perfectly while its names had gone.
    GFNT_Result family_result = GFNT_OK;
    GFNT_Result sub_result = GFNT_OK;
    const std::string family = name_of(font.face, GFNT_NAME_FAMILY,
        &family_result);
    const std::string sub = name_of(font.face, GFNT_NAME_SUBFAMILY,
        &sub_result);
    // Both names, because each one's bytes are its own: the last record's string
    // sits at the end of the table, so the cuts that take only it leave the first
    // name intact - and a sweep that asked for one name would report those
    // fourteen cuts as a table that lost nothing.
    if (refused || family_result != GFNT_OK || sub_result != GFNT_OK) {
      refusals++;
      continue;
    }
    // Property one: if it still answers, it answers what the whole table did.
    EXPECT_EQ(count, 2u) << "name cut to " << cut;
    EXPECT_EQ(family, "Ghoti Sans") << "name cut to " << cut;
    EXPECT_EQ(sub, "Regular") << "name cut to " << cut;
  }
  EXPECT_EQ(refusals, whole.size()) << "every short table loses something";
}

TEST(Name, ARecordOnAPlatformThisLibraryCannotDecodeIsPassedOver) {
  // Platform 7 is not a platform: it is what a font written by a tool nobody
  // maintains looks like. The scan must skip it rather than rank it, because the
  // rank would put it somewhere and the decode would then fail.
  Font mixed({name_table(gfnttest::build_name({
      {7, 0, 0, GFNT_NAME_FAMILY, gfnttest::raw_bytes("nonsense")},
      {3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti Sans")},
  }))});
  Font only({name_table(gfnttest::build_name({
      {7, 0, 0, GFNT_NAME_FAMILY, gfnttest::raw_bytes("nonsense")},
  }))});
  GFNT_Result result = GFNT_OK;

  EXPECT_EQ(name_of(mixed.face, GFNT_NAME_FAMILY, &result), "Ghoti Sans");
  EXPECT_EQ(result, GFNT_OK);
  EXPECT_EQ(name_of(only.face, GFNT_NAME_FAMILY, &result), "");
  EXPECT_EQ(result, GFNT_ERR_UNSUPPORTED)
      << "a record nobody can decode is not a name this font has";
}

TEST(Name, DecodingOneRecordDirectlyReportsWhatTheScanWouldHaveSkipped) {
  // The scan passes over an undecodable record; a caller that walks the records
  // itself and decodes one is entitled to be told *why* it cannot be decoded,
  // which is a different answer from "this font has no such name".
  Font font({name_table(gfnttest::build_name({
      {1, 1, 11, GFNT_NAME_FAMILY, gfnttest::raw_bytes("\x82\xa0")},
      {3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti")},
  }))});
  GFNT_NameRecord record{};
  char * text = nullptr;
  GFNT_Error error{};

  ASSERT_EQ(gfnt_face_name_at(font.face, 0, &record, &error), GFNT_OK);
  EXPECT_EQ(record.platform_id, 1);
  EXPECT_EQ(gfnt_face_name_decode(font.face, &record, nullptr, &text, nullptr,
      &error), GFNT_ERR_UNSUPPORTED);
  ASSERT_NE(error.message, nullptr);
  // And it names *which* encoding it cannot decode. The specific sentence used
  // to live one function further in, where the decodability check in front of it
  // meant nothing ever reached it, so every caller got the generic one.
  EXPECT_NE(std::string(error.message).find("multi-byte"), std::string::npos)
      << error.message;
  EXPECT_EQ(text, nullptr);

  // A platform nobody defines gets the general sentence, which is the other half
  // of the same refusal and the reason it cannot simply be reworded.
  GFNT_NameRecord unknown{};
  unknown.platform_id = 7;
  EXPECT_EQ(gfnt_face_name_decode(font.face, &unknown, nullptr, &text, nullptr,
      &error), GFNT_ERR_UNSUPPORTED);
  ASSERT_NE(error.message, nullptr);
  EXPECT_EQ(std::string(error.message).find("multi-byte"), std::string::npos)
      << error.message;
}

TEST(Name, ARecordWhoseBytesRunPastTheStorageIsCorrupt) {
  // The record's length and offset are read from the table and the bytes they
  // name are not there. A Macintosh record is decoded a byte at a time, so this
  // is the bound inside that loop rather than the one on the record.
  std::vector<uint8_t> name = gfnttest::build_name({
      {1, 0, 0, GFNT_NAME_FAMILY, gfnttest::raw_bytes("Ghoti")},
  });
  // The length field of record 0, made longer than the storage that follows it.
  gfnttest::patch_u16(name, gfnttest::name_record_offset(0) + 8, 200);
  Font font({name_table(name)});
  GFNT_NameRecord record{};
  char * text = nullptr;
  GFNT_Error error{};

  ASSERT_EQ(gfnt_face_name_at(font.face, 0, &record, &error), GFNT_OK);
  EXPECT_EQ(gfnt_face_name_decode(font.face, &record, nullptr, &text, nullptr,
      &error), GFNT_ERR_CORRUPT);
  EXPECT_EQ(text, nullptr);
}

TEST(Name, DecodingCanRunOutOfMemoryAndSaysSoWithoutLeaking) {
  Font font({name_table(gfnttest::build_name({
      {3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti Sans")},
  }))});
  GFNT_NameRecord record{};
  ASSERT_EQ(gfnt_face_name_at(font.face, 0, &record, nullptr), GFNT_OK);

  for (size_t n = 0; n < 3; n++) {
    gfnttest::FailingAllocator allocator(n);
    char * text = nullptr;
    GFNT_Error error{};
    const GFNT_Result result = gfnt_face_name_decode(font.face, &record,
        allocator.get(), &text, nullptr, &error);

    if (result == GFNT_OK) {
      allocator.stop_failing();
      gfnt_name_free(allocator.get(), text);
    }
    else {
      EXPECT_EQ(result, GFNT_ERR_OOM) << "at refusal " << n;
      EXPECT_EQ(text, nullptr) << "at refusal " << n;
      EXPECT_NE(error.message, nullptr);
    }
    EXPECT_EQ(allocator.live(), 0u) << "at refusal " << n;
  }
}

TEST(NameDump, EscapesTheControlBytesItSaysItDoesAndNothingElse) {
  // The dump is read by people and by `ttx_diff`, so a byte that would end a
  // line has to be spelled rather than written. Only C0 and DEL are escaped:
  // everything else is UTF-8 the decoder produced, and mangling it here would
  // make the dump a worse record than the font.
  Font font({name_table(gfnttest::build_name({
      // "\x01f" would be one hex escape of three digits rather than a byte and
      // a letter, which is why the string is split here.
      {3, 1, 0x409, GFNT_NAME_FAMILY,
          gfnttest::utf16be("a\rb\nc\td\\e\x01" "f\x7f" "z")},
      {3, 1, 0x409, GFNT_NAME_COPYRIGHT, gfnttest::utf16be("\xe9")},
  }))});
  gfnttest::CapturedOutput out;

  ASSERT_NE(out.get(), nullptr);
  ASSERT_EQ(gfnt_face_name_dump(font.face, out.get()), GFNT_OK);
  const std::string text = out.finish();

  // The trailing 'z' is what makes the last run a run: without a plain byte
  // after the final escape there is nothing left to write when the loop ends.
  EXPECT_NE(text.find("a\\rb\\nc\\td\\\\e\\x01" "f\\x7F" "z"),
      std::string::npos) << text;

  // The escaped text is written in runs - the bytes before an escape, the escape
  // itself, and whatever is left after the last one - so each is a write of its
  // own and each can fail.
  size_t failures = 0;
  for (size_t allow = 0; allow < 32; allow++) {
    gfnttest::FailingSink sink(allow);
    ASSERT_NE(sink.get(), nullptr);
    if (gfnt_face_name_dump(font.face, sink.get()) == GFNT_ERR_IO) {
      failures++;
    }
  }
  EXPECT_GT(failures, 6u) << "one write per run and per escape";
  // U+00E9 is two UTF-8 bytes out of the decoder and neither is escaped.
  EXPECT_NE(text.find("\xc3\xa9"), std::string::npos) << text;
}

TEST(NameDump, ReportsEveryWriteFailureAndEveryRecordItCannotRead) {
  // Three kinds of line - a readable record, one whose bytes are unreadable, and
  // one whose encoding this library does not decode - and every one of them is a
  // write that can fail.
  std::vector<uint8_t> name = gfnttest::build_name({
      {3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti")},
      {1, 1, 11, GFNT_NAME_SUBFAMILY, gfnttest::raw_bytes("\x82\xa0")},
      {1, 0, 0, GFNT_NAME_COPYRIGHT, gfnttest::raw_bytes("Mac")},
  });
  gfnttest::patch_u16(name, gfnttest::name_record_offset(2) + 8, 400);
  Font font({name_table(name)});
  gfnttest::CapturedOutput out;

  ASSERT_NE(out.get(), nullptr);
  ASSERT_EQ(gfnt_face_name_dump(font.face, out.get()), GFNT_OK);
  const std::string text = out.finish();
  EXPECT_NE(text.find("name: 3 records"), std::string::npos) << text;
  EXPECT_NE(text.find("name record 1: platform 1, encoding 1"),
      std::string::npos) << text;
  EXPECT_NE(text.find("not decodable"), std::string::npos) << text;

  size_t failures = 0;
  for (size_t allow = 0; allow < 12; allow++) {
    gfnttest::FailingSink sink(allow);
    ASSERT_NE(sink.get(), nullptr);
    if (gfnt_face_name_dump(font.face, sink.get()) == GFNT_ERR_IO) {
      failures++;
    }
  }
  // The header, then for the readable record its prefix, its text and its
  // closing quote; for the undecodable one a single line; and for the one whose
  // bytes are missing, the same.
  EXPECT_EQ(failures, 6u);

  Font broken({name_table(std::vector<uint8_t>(4, 0))});
  gfnttest::CapturedOutput other;
  ASSERT_NE(other.get(), nullptr);
  EXPECT_NE(gfnt_face_name_dump(broken.face, other.get()), GFNT_OK)
      << "a table whose count cannot be read has no records to print";

  // A count that promises records the table does not hold: the record itself is
  // unreadable, which is its own line and its own write.
  std::vector<uint8_t> promised = gfnttest::build_name({
      {3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti")},
  });
  gfnttest::patch_u16(promised, 2, 40);
  Font ragged({name_table(promised)});
  gfnttest::CapturedOutput third;
  ASSERT_NE(third.get(), nullptr);
  EXPECT_EQ(gfnt_face_name_dump(ragged.face, third.get()), GFNT_OK);
  EXPECT_NE(third.finish().find("unreadable"), std::string::npos);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
