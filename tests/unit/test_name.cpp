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

TEST(Name, AMacintoshRecordAboveAsciiIsUnsupportedNotWrong) {
  // Decoding it needs the Mac Roman table, and section 14's rule is that a
  // vector comes from an oracle rather than from memory. Guessing Latin-1 here
  // would put a wrong character in a family name, which is worse than saying
  // so.
  Font font({name_table(gfnttest::build_name(
      {{1, 0, 0, GFNT_NAME_FAMILY, {'G', 'h', 0xA5, 'i'}}}))});
  GFNT_NameRecord record{};
  ASSERT_EQ(gfnt_face_name_at(font.face, 0, &record, nullptr), GFNT_OK);
  char * out = nullptr;
  EXPECT_EQ(gfnt_face_name_decode(font.face, &record, nullptr, &out, nullptr,
                &font.error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(font.error.message, nullptr);
}

TEST(Name, AnEncodingThisLibraryDoesNotDecodeIsUnsupported) {
  Font font({name_table(gfnttest::build_name(
      {{1, 6, 0, GFNT_NAME_FAMILY, gfnttest::raw_bytes("whatever")}}))});
  GFNT_NameRecord record{};
  ASSERT_EQ(gfnt_face_name_at(font.face, 0, &record, nullptr), GFNT_OK);
  char * out = nullptr;
  EXPECT_EQ(gfnt_face_name_decode(font.face, &record, nullptr, &out, nullptr,
                nullptr),
      GFNT_ERR_UNSUPPORTED);
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
      {1, 6, 0, GFNT_NAME_FAMILY, gfnttest::raw_bytes("whatever")},
  }))});

  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  ASSERT_EQ(gfnt_face_name_dump(font.face, out.get()), GFNT_OK);
  std::string text = out.finish();

  EXPECT_NE(text.find("2 records"), std::string::npos) << text;
  EXPECT_NE(text.find("'Ghoti Sans'"), std::string::npos) << text;
  EXPECT_NE(text.find("not decodable"), std::string::npos) << text;
}

TEST(NameDump, RefusesNullsAndReportsEveryWriteFailure) {
  Font font({name_table(gfnttest::build_name({
      {3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti Sans")},
      {3, 1, 0x409, GFNT_NAME_SUBFAMILY, gfnttest::utf16be("Regular")},
  }))});

  EXPECT_EQ(gfnt_face_name_dump(nullptr, nullptr), GFNT_ERR_INVALID);

  size_t failures = 0;
  for (size_t allow = 0; allow < 5; allow++) {
    gfnttest::FailingSink sink(allow);
    ASSERT_NE(sink.get(), nullptr);
    if (gfnt_face_name_dump(font.face, sink.get()) == GFNT_ERR_IO) {
      failures++;
    }
  }
  EXPECT_EQ(failures, 3u) << "a header line and one line per record";
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
