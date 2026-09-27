/**
 * @file
 *
 * The sfnt container: the offset table, the directory, collections, and what a
 * face answers before any table has been parsed.
 *
 * documentation/design.md sections 7.1 and 7.8.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <string>
#include <vector>

using gfnttest::Table;

namespace {

/** Bytes that are not any table this library parses; only their size matters. */
std::vector<uint8_t> filler(size_t n, uint8_t seed = 0x40) {
  std::vector<uint8_t> data(n);
  for (size_t i = 0; i < n; i++) {
    data[i] = static_cast<uint8_t>(seed + i);
  }
  return data;
}

/** A `head`-shaped table with a non-zero checkSumAdjustment. */
std::vector<uint8_t> head_with_adjustment(uint32_t adjustment) {
  std::vector<uint8_t> head;
  gfnttest::put_u16(head, 1);            // majorVersion
  gfnttest::put_u16(head, 0);            // minorVersion
  gfnttest::put_u32(head, 0x00010000);   // fontRevision
  gfnttest::put_u32(head, adjustment);   // checkSumAdjustment
  gfnttest::put_u32(head, 0x5F0F3CF5);   // magicNumber
  head.resize(54, 0);
  return head;
}

/** A blob over bytes the test keeps alive, with the face loaded from it. */
struct Loaded {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  explicit Loaded(std::vector<uint8_t> data, size_t index = 0,
      const GFNT_Limits * limits = nullptr)
      : bytes(std::move(data)) {
    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                  GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
        GFNT_OK);
    result = gfnt_face_load(blob, index, limits, nullptr, &face, &error);
  }

  ~Loaded() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }

  Loaded(const Loaded &) = delete;
  Loaded & operator=(const Loaded &) = delete;

  GFNT_Result result = GFNT_ERR_INTERNAL;
};

} // namespace

TEST(Face, LoadsTheOffsetTableAndTheDirectory) {
  Loaded font(gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {{GFNT_TAG('h', 'e', 'a', 'd'), head_with_adjustment(0)},
          {GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}}));

  ASSERT_EQ(font.result, GFNT_OK);
  EXPECT_EQ(gfnt_face_flavour(font.face), GFNT_FLAVOUR_TRUETYPE);
  EXPECT_EQ(gfnt_face_table_count(font.face), 2u);
  EXPECT_EQ(gfnt_face_index(font.face), 0u);

  GFNT_Tag tag = 0;
  ASSERT_EQ(gfnt_face_table_tag_at(font.face, 0, &tag), GFNT_OK);
  EXPECT_EQ(tag, GFNT_TAG('h', 'e', 'a', 'd'));
  ASSERT_EQ(gfnt_face_table_tag_at(font.face, 1, &tag), GFNT_OK);
  EXPECT_EQ(tag, GFNT_TAG('m', 'a', 'x', 'p'));
  EXPECT_EQ(gfnt_face_table_tag_at(font.face, 2, &tag), GFNT_ERR_INVALID);

  EXPECT_TRUE(gfnt_face_has_table(font.face, GFNT_TAG('m', 'a', 'x', 'p')));
  EXPECT_FALSE(gfnt_face_has_table(font.face, GFNT_TAG('c', 'm', 'a', 'p')));
}

TEST(Face, ReadsEverySfntVersionItNames) {
  // The four an sfnt can carry. Which outline format to expect is all the
  // version says; what the face can do is decided by its tables.
  for (GFNT_Tag flavour : {GFNT_FLAVOUR_TRUETYPE, GFNT_FLAVOUR_CFF,
           GFNT_FLAVOUR_APPLE_TRUE, GFNT_FLAVOUR_APPLE_TYPE1}) {
    Loaded font(gfnttest::build_sfnt(flavour,
        {{GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}}));
    char text[5];
    ASSERT_EQ(font.result, GFNT_OK)
        << "flavour " << gfnt_tag_string(flavour, text);
    EXPECT_EQ(gfnt_face_flavour(font.face), flavour);
  }
}

TEST(Face, WhatIsNotAnSfntIsAFormatErrorNotACorruptOne) {
  // ERR_FORMAT is "not a font this library recognises" and ERR_CORRUPT is "a
  // font whose bytes are wrong"; the PCF, BDF and Type 1 readers get their
  // turn at an ERR_FORMAT, and would never see it if this were CORRUPT.
  const std::vector<std::vector<uint8_t>> cases = {
      {},
      {0x00},
      {'%', '!', 'P', 'S'},
      {0x00, 0x01, 0x00},
      {0x00, 0x02, 0x00, 0x00},
  };

  for (const auto & bytes : cases) {
    Loaded font(bytes);
    EXPECT_EQ(font.result, GFNT_ERR_FORMAT) << "with " << bytes.size()
                                            << " bytes";
    EXPECT_EQ(font.face, nullptr);
    EXPECT_NE(font.error.message, nullptr);
  }
}

TEST(Face, MoreTablesThanTheLimitIsRefusedNotTruncated) {
  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_tables = 1;

  Loaded font(gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
                  {{GFNT_TAG('h', 'e', 'a', 'd'), filler(54)},
                      {GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}}),
      0, &limits);

  EXPECT_EQ(font.result, GFNT_ERR_LIMIT);
  EXPECT_EQ(font.face, nullptr);
}

TEST(Face, ADirectoryThatEndsMidEntryIsCorrupt) {
  std::vector<uint8_t> bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {{GFNT_TAG('h', 'e', 'a', 'd'), filler(54)},
          {GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}});
  // Cut the file in the middle of the second directory entry. The count still
  // says two, which is the case a parser that sizes from the count and reads
  // without checking walks straight out of.
  bytes.resize(gfnttest::entry_offset(0, 1) + 8);

  Loaded font(bytes);
  EXPECT_EQ(font.result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(font.face, nullptr);
  EXPECT_NE(font.error.message, nullptr);
}

TEST(Face, ATableOutsideTheFontIsCorruptAndNamed) {
  std::vector<uint8_t> bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {{GFNT_TAG('h', 'e', 'a', 'd'), filler(54)},
          {GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}});
  gfnttest::patch_u32(bytes, gfnttest::entry_offset(0, 1) + 8, 0x7FFFFFFF);

  Loaded font(bytes);
  EXPECT_EQ(font.result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(font.error.table, GFNT_TAG('m', 'a', 'x', 'p'))
      << "the diagnostic names the table whose entry is wrong";
}

TEST(Face, ATableLengthThatWouldOverflowIsCorrupt) {
  std::vector<uint8_t> bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {{GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}});
  // offset + length computed with safemath: a length near UINT32_MAX must not
  // wrap into a range that looks like it fits.
  gfnttest::patch_u32(bytes, gfnttest::entry_offset(0, 0) + 12, 0xFFFFFFFF);

  Loaded font(bytes);
  EXPECT_EQ(font.result, GFNT_ERR_CORRUPT);
}

TEST(Face, OverlappingTablesAreAccepted) {
  // Fonts in the wild share bytes between tables deliberately. Refusing them
  // would refuse fonts every other implementation reads.
  std::vector<uint8_t> bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {{GFNT_TAG('h', 'e', 'a', 'd'), filler(54)},
          {GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}});
  uint32_t head_offset = 0;
  for (int i = 0; i < 4; i++) {
    head_offset = (head_offset << 8) | bytes[gfnttest::entry_offset(0, 0) + 8 + i];
  }
  gfnttest::patch_u32(bytes, gfnttest::entry_offset(0, 1) + 8, head_offset);

  Loaded font(bytes);
  EXPECT_EQ(font.result, GFNT_OK);
  size_t head_at = 0;
  size_t maxp_at = 0;
  ASSERT_EQ(gfnt_face_table_range(font.face, GFNT_TAG('h', 'e', 'a', 'd'),
                &head_at, nullptr),
      GFNT_OK);
  ASSERT_EQ(gfnt_face_table_range(font.face, GFNT_TAG('m', 'a', 'x', 'p'),
                &maxp_at, nullptr),
      GFNT_OK);
  EXPECT_EQ(head_at, maxp_at);
}

TEST(Face, AFontWithNoTablesLoads) {
  // Legal, and what a stripped font can look like. Every operation then
  // reports its own missing table rather than the load failing for all of them.
  Loaded font(gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {}));

  ASSERT_EQ(font.result, GFNT_OK);
  EXPECT_EQ(gfnt_face_table_count(font.face), 0u);
  EXPECT_FALSE(gfnt_face_has_table(font.face, GFNT_TAG('h', 'e', 'a', 'd')));
  EXPECT_EQ(gfnt_face_table_range(font.face, GFNT_TAG('h', 'e', 'a', 'd'),
                nullptr, nullptr),
      GFNT_ERR_UNSUPPORTED)
      << "a missing table is UNSUPPORTED, not CORRUPT: design.md section 7.8";
}

TEST(Face, ADirectoryThatIsNotSortedStillFindsEveryTable) {
  // The specification requires a sorted directory and fonts exist that are
  // not; a binary search would answer "absent" for a table the font has, so
  // the lookup is a scan.
  Loaded font(gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {{GFNT_TAG('m', 'a', 'x', 'p'), filler(6)},
          {GFNT_TAG('h', 'e', 'a', 'd'), filler(54)},
          {GFNT_TAG('c', 'm', 'a', 'p'), filler(4)}}));

  ASSERT_EQ(font.result, GFNT_OK);
  EXPECT_TRUE(gfnt_face_has_table(font.face, GFNT_TAG('h', 'e', 'a', 'd')));
  EXPECT_TRUE(gfnt_face_has_table(font.face, GFNT_TAG('c', 'm', 'a', 'p')));
  EXPECT_TRUE(gfnt_face_has_table(font.face, GFNT_TAG('m', 'a', 'x', 'p')));
}

TEST(Face, TableRangeNamesWhereTheBytesActuallyAre) {
  std::vector<uint8_t> table = filler(13, 0x70);
  Loaded font(gfnttest::build_sfnt(GFNT_FLAVOUR_CFF,
      {{GFNT_TAG('C', 'F', 'F', ' '), table}}));
  ASSERT_EQ(font.result, GFNT_OK);

  size_t offset = 0;
  size_t length = 0;
  ASSERT_EQ(gfnt_face_table_range(font.face, GFNT_TAG('C', 'F', 'F', ' '),
                &offset, &length),
      GFNT_OK);
  EXPECT_EQ(length, table.size());
  ASSERT_LE(offset + length, font.bytes.size());
  EXPECT_EQ(std::memcmp(font.bytes.data() + offset, table.data(), length), 0)
      << "the range a PDF writer would embed has to be the table's own bytes";
}

TEST(Collection, CountsEveryFaceAndAPlainFontIsOne) {
  std::vector<uint8_t> collection = gfnttest::build_collection(
      {{{GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}},
          {{GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}},
          {{GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}}});
  std::vector<uint8_t> single = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {{GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}});

  for (const auto & pair : std::vector<std::pair<std::vector<uint8_t>, size_t>>{
           {collection, 3}, {single, 1}}) {
    GFNT_Blob * blob = nullptr;
    ASSERT_EQ(gfnt_blob_create_memory(pair.first.data(), pair.first.size(),
                  GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
        GFNT_OK);
    size_t count = 0;
    EXPECT_EQ(gfnt_face_count(blob, nullptr, &count, nullptr), GFNT_OK);
    EXPECT_EQ(count, pair.second);
    gfnt_blob_destroy(blob);
  }
}

TEST(Collection, EachFaceHasItsOwnDirectory) {
  std::vector<uint8_t> bytes = gfnttest::build_collection(
      {{{GFNT_TAG('h', 'e', 'a', 'd'), filler(54)}},
          {{GFNT_TAG('c', 'm', 'a', 'p'), filler(8)},
              {GFNT_TAG('h', 'e', 'a', 'd'), filler(54)}}});

  Loaded first(bytes, 0);
  ASSERT_EQ(first.result, GFNT_OK);
  EXPECT_EQ(gfnt_face_table_count(first.face), 1u);
  EXPECT_EQ(gfnt_face_index(first.face), 0u);
  EXPECT_FALSE(gfnt_face_has_table(first.face, GFNT_TAG('c', 'm', 'a', 'p')));

  Loaded second(bytes, 1);
  ASSERT_EQ(second.result, GFNT_OK);
  EXPECT_EQ(gfnt_face_table_count(second.face), 2u);
  EXPECT_EQ(gfnt_face_index(second.face), 1u);
  EXPECT_TRUE(gfnt_face_has_table(second.face, GFNT_TAG('c', 'm', 'a', 'p')));
}

TEST(Collection, VersionTwoReadsLikeVersionOne) {
  // The two differ only in the DSIG fields after the offset array, which this
  // library skips.
  std::vector<uint8_t> bytes = gfnttest::build_collection(
      {{{GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}}}, 2);
  Loaded font(bytes, 0);
  EXPECT_EQ(font.result, GFNT_OK);
}

TEST(Collection, AVersionThisLibraryDoesNotReadIsUnsupported) {
  // Not CORRUPT: the file is a well-formed collection of a version nobody has
  // written yet, and the distinction is what tells a caller whether to file a
  // bug against the font or against this library.
  std::vector<uint8_t> bytes = gfnttest::build_collection(
      {{{GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}}}, 3);
  Loaded font(bytes, 0);
  EXPECT_EQ(font.result, GFNT_ERR_UNSUPPORTED);
}

TEST(Collection, AnIndexNoFaceHasIsACallerError) {
  std::vector<uint8_t> collection = gfnttest::build_collection(
      {{{GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}},
          {{GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}}});
  Loaded past_end(collection, 2);
  EXPECT_EQ(past_end.result, GFNT_ERR_INVALID);

  Loaded not_a_collection(
      gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
          {{GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}}),
      1);
  EXPECT_EQ(not_a_collection.result, GFNT_ERR_INVALID);
}

TEST(Collection, MoreFacesThanTheHeaderCanHoldIsCorrupt) {
  // numFonts is a count from the file. Without checking that its offset array
  // is actually there, a sixteen-byte file can claim four billion faces.
  std::vector<uint8_t> bytes = gfnttest::build_collection(
      {{{GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}}});
  gfnttest::patch_u32(bytes, 8, 0xFFFFFFFFu);

  GFNT_Blob * blob = nullptr;
  ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
      GFNT_OK);
  size_t count = 0;
  GFNT_Error error;
  EXPECT_EQ(gfnt_face_count(blob, nullptr, &count, &error), GFNT_ERR_CORRUPT);
  EXPECT_EQ(error.table, GFNT_FLAVOUR_COLLECTION);
  gfnt_blob_destroy(blob);

  Loaded font(bytes, 0);
  EXPECT_EQ(font.result, GFNT_ERR_CORRUPT);
}

TEST(Collection, FacesShareOneBlobAndOutliveEachOther) {
  // The one lifetime rule this API has: the blob is the caller's, and freeing
  // a face must not take the bytes the other faces are still reading.
  std::vector<uint8_t> bytes = gfnttest::build_collection(
      {{{GFNT_TAG('h', 'e', 'a', 'd'), filler(54)}},
          {{GFNT_TAG('c', 'm', 'a', 'p'), filler(8)}}});
  GFNT_Blob * blob = nullptr;
  ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                GFNT_BLOB_COPY, nullptr, nullptr, &blob, nullptr),
      GFNT_OK);

  GFNT_Face * first = nullptr;
  GFNT_Face * second = nullptr;
  ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &first, nullptr),
      GFNT_OK);
  ASSERT_EQ(gfnt_face_load(blob, 1, nullptr, nullptr, &second, nullptr),
      GFNT_OK);

  gfnt_face_free(first);
  EXPECT_TRUE(gfnt_face_has_table(second, GFNT_TAG('c', 'm', 'a', 'p')))
      << "freeing one face must not disturb another's view of the blob";
  gfnt_face_free(second);
  gfnt_blob_destroy(blob);
}

TEST(Checksum, TheDirectorysNumbersMatchTheBytes) {
  Loaded font(gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {{GFNT_TAG('h', 'e', 'a', 'd'), head_with_adjustment(0xDEADBEEF)},
          {GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}}));
  ASSERT_EQ(font.result, GFNT_OK);

  for (GFNT_Tag tag :
      {GFNT_TAG('h', 'e', 'a', 'd'), GFNT_TAG('m', 'a', 'x', 'p')}) {
    uint32_t stored = 0;
    uint32_t computed = 1;
    char text[5];
    ASSERT_EQ(gfnt_face_table_checksum(font.face, tag, &stored, &computed),
        GFNT_OK);
    EXPECT_EQ(stored, computed) << "table " << gfnt_tag_string(tag, text);
  }
}

TEST(Checksum, HeadIsCheckedWithItsAdjustmentZeroed) {
  // head carries the checksum of the whole font, so its own directory entry is
  // computed with that field zeroed - otherwise the number would depend on
  // itself. A checksum that did not do this would disagree for every font ever
  // written, which is indistinguishable from "checksums are not enforced".
  std::vector<uint8_t> quiet = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {{GFNT_TAG('h', 'e', 'a', 'd'), head_with_adjustment(0)}});
  std::vector<uint8_t> loud = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {{GFNT_TAG('h', 'e', 'a', 'd'), head_with_adjustment(0x11223344)}});

  uint32_t quiet_sum = 0;
  uint32_t loud_sum = 0;
  {
    Loaded font(quiet);
    ASSERT_EQ(font.result, GFNT_OK);
    ASSERT_EQ(gfnt_face_table_checksum(font.face, GFNT_TAG('h', 'e', 'a', 'd'),
                  nullptr, &quiet_sum),
        GFNT_OK);
  }
  {
    Loaded font(loud);
    ASSERT_EQ(font.result, GFNT_OK);
    ASSERT_EQ(gfnt_face_table_checksum(font.face, GFNT_TAG('h', 'e', 'a', 'd'),
                  nullptr, &loud_sum),
        GFNT_OK);
  }
  EXPECT_EQ(quiet_sum, loud_sum)
      << "checkSumAdjustment must not take part in head's own checksum";
}

TEST(Checksum, ABadOneIsReportedAndNeverEnforced) {
  // A font with a wrong checksum renders everywhere else; refusing it here
  // would make this the only library that cannot read a font the user can see.
  std::vector<uint8_t> bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {{GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}});
  gfnttest::patch_u32(bytes, gfnttest::entry_offset(0, 0) + 4, 0x00000000);

  Loaded font(bytes);
  ASSERT_EQ(font.result, GFNT_OK) << "a bad checksum is not a load failure";
  uint32_t stored = 1;
  uint32_t computed = 1;
  ASSERT_EQ(gfnt_face_table_checksum(font.face, GFNT_TAG('m', 'a', 'x', 'p'),
                &stored, &computed),
      GFNT_OK);
  EXPECT_EQ(stored, 0u);
  EXPECT_NE(computed, stored) << "and the caller can see that it is bad";
}

TEST(Checksum, AnAbsentTableHasNone) {
  Loaded font(gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {}));
  ASSERT_EQ(font.result, GFNT_OK);
  EXPECT_EQ(gfnt_face_table_checksum(font.face, GFNT_TAG('h', 'e', 'a', 'd'),
                nullptr, nullptr),
      GFNT_ERR_UNSUPPORTED);
}

TEST(FaceDump, NamesTheFlavourAndEveryTable) {
  Loaded font(gfnttest::build_sfnt(GFNT_FLAVOUR_CFF,
      {{GFNT_TAG('C', 'F', 'F', ' '), filler(9)},
          {GFNT_TAG('h', 'e', 'a', 'd'), filler(54)}}));
  ASSERT_EQ(font.result, GFNT_OK);

  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  ASSERT_EQ(gfnt_face_dump(font.face, out.get()), GFNT_OK);
  std::string text = out.finish();

  EXPECT_NE(text.find("flavour 'OTTO'"), std::string::npos) << text;
  EXPECT_NE(text.find("2 tables"), std::string::npos) << text;
  EXPECT_NE(text.find("table 'CFF '"), std::string::npos) << text;
  EXPECT_NE(text.find("table 'head'"), std::string::npos) << text;
  EXPECT_NE(text.find("length 9"), std::string::npos) << text;
}

TEST(FaceDump, RefusesNullsAndReportsEveryWriteFailure) {
  Loaded font(gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {{GFNT_TAG('h', 'e', 'a', 'd'), filler(54)},
          {GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}}));
  ASSERT_EQ(font.result, GFNT_OK);

  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  EXPECT_EQ(gfnt_face_dump(nullptr, out.get()), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_dump(font.face, nullptr), GFNT_ERR_INVALID);

  size_t failures = 0;
  for (size_t allow = 0; allow < 4; allow++) {
    gfnttest::FailingSink sink(allow);
    ASSERT_NE(sink.get(), nullptr);
    GFNT_Result result = gfnt_face_dump(font.face, sink.get());
    if (sink.failed()) {
      EXPECT_EQ(result, GFNT_ERR_IO) << "with " << allow << " writes allowed";
      failures++;
    }
    else {
      EXPECT_EQ(result, GFNT_OK) << "with " << allow << " writes allowed";
    }
  }
  EXPECT_EQ(failures, 3u)
      << "one header line and one line per table, each checked";
}

TEST(Face, AccessorsAndFreeAcceptNull) {
  EXPECT_EQ(gfnt_face_index(nullptr), 0u);
  EXPECT_EQ(gfnt_face_flavour(nullptr), 0u);
  EXPECT_EQ(gfnt_face_table_count(nullptr), 0u);
  EXPECT_FALSE(gfnt_face_has_table(nullptr, 0));
  EXPECT_EQ(gfnt_face_table_range(nullptr, 0, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_table_checksum(nullptr, 0, nullptr, nullptr),
      GFNT_ERR_INVALID);
  GFNT_Tag tag = 0;
  EXPECT_EQ(gfnt_face_table_tag_at(nullptr, 0, &tag), GFNT_ERR_INVALID);
  gfnt_face_free(nullptr);
}

TEST(Face, LoadAndCountRefuseNullOutputs) {
  std::vector<uint8_t> bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {});
  GFNT_Blob * blob = nullptr;
  ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
      GFNT_OK);

  EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_count(blob, nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
  GFNT_Face * face = nullptr;
  size_t count = 0;
  EXPECT_EQ(gfnt_face_load(nullptr, 0, nullptr, nullptr, &face, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_count(nullptr, nullptr, &count, nullptr),
      GFNT_ERR_INVALID);
  gfnt_blob_destroy(blob);
}

TEST(Face, EveryAllocationFailureIsReportedAndLeaksNothing) {
  std::vector<uint8_t> bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {{GFNT_TAG('h', 'e', 'a', 'd'), filler(54)},
          {GFNT_TAG('m', 'a', 'x', 'p'), filler(6)}});
  size_t failures = 0;

  for (size_t n = 0; n < 6; n++) {
    gfnttest::FailingAllocator allocator(n);
    GFNT_Blob * blob = nullptr;
    // The blob takes the default allocator: this sweep walks the face's own
    // allocations, and a refused blob would stop it before the first of them.
    ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                  GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
        GFNT_OK);

    GFNT_Face * face = nullptr;
    GFNT_Error error;
    GFNT_Result result = gfnt_face_load(blob, 0, nullptr, allocator.get(),
        &face, &error);
    if (result == GFNT_OK) {
      ASSERT_NE(face, nullptr);
    }
    else {
      EXPECT_EQ(result, GFNT_ERR_OOM) << "at refusal " << n;
      EXPECT_EQ(face, nullptr) << "at refusal " << n;
      failures++;
    }
    allocator.stop_failing();
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    EXPECT_EQ(allocator.live(), 0u) << "at refusal " << n;
  }
  EXPECT_EQ(failures, 2u)
      << "a face load makes exactly two allocations: the face and its table "
         "array";
}

TEST(Face, TheFirstFourBytesDecideWhetherItIsAFontAtAll) {
  // Below the directory and below the collection header there is one question:
  // is this an sfnt. It is answered from the flavour tag, and the answers have
  // to be told apart - "not a font" is FORMAT and a caller should try another
  // parser; "a collection whose header ends early" is CORRUPT and it should not.
  const std::vector<uint8_t> tiny = {0x00, 0x01};
  const std::vector<uint8_t> junk = {'j', 'u', 'n', 'k', 0, 0, 0, 0};
  std::vector<uint8_t> collection = {'t', 't', 'c', 'f', 0x00, 0x01};
  size_t count = 0;
  GFNT_Error error{};

  {
    GFNT_Blob * blob = nullptr;
    ASSERT_EQ(gfnt_blob_create_memory(tiny.data(), tiny.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_count(blob, nullptr, &count, &error), GFNT_ERR_FORMAT);
    ASSERT_NE(error.message, nullptr);
    EXPECT_NE(std::string(error.message).find("too short to be a font"),
        std::string::npos) << error.message;
    gfnt_blob_destroy(blob);
  }
  {
    GFNT_Blob * blob = nullptr;
    ASSERT_EQ(gfnt_blob_create_memory(junk.data(), junk.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_count(blob, nullptr, &count, &error), GFNT_ERR_FORMAT);
    ASSERT_NE(error.message, nullptr);
    EXPECT_NE(std::string(error.message).find("not an sfnt version"),
        std::string::npos) << error.message;
    gfnt_blob_destroy(blob);
  }
  {
    GFNT_Blob * blob = nullptr;
    ASSERT_EQ(gfnt_blob_create_memory(collection.data(), collection.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_count(blob, nullptr, &count, &error), GFNT_ERR_CORRUPT);
    ASSERT_NE(error.message, nullptr);
    EXPECT_NE(std::string(error.message).find("collection header ends early"),
        std::string::npos) << error.message;
    gfnt_blob_destroy(blob);
  }
}

TEST(Face, ACollectionWhoseOffsetArrayIsMissingOrWrongIsRefusedPerFace) {
  // The header says how many faces there are and the array says where each one
  // is. A header that promises faces the array does not describe, and an entry
  // that points outside the file, are two different reads and neither may be
  // answered with face 0.
  std::vector<uint8_t> promised = {'t', 't', 'c', 'f'};
  gfnttest::put_u16(promised, 1);
  gfnttest::put_u16(promised, 0);
  gfnttest::put_u32(promised, 3);       // three faces, and no offsets at all
  std::vector<uint8_t> outside = promised;
  gfnttest::put_u32(outside, 0);        // face 0, at the start of the file
  gfnttest::put_u32(outside, 100000);   // face 1, past the end of it
  gfnttest::put_u32(outside, 22);       // face 2, two bytes from the end
  size_t count = 0;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  {
    // The count itself is refused: the header's own check requires room for the
    // offset array, so "three faces" and "no array" never becomes a face load
    // that reads a missing offset.
    GFNT_Blob * blob = nullptr;
    ASSERT_EQ(gfnt_blob_create_memory(promised.data(), promised.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_count(blob, nullptr, &count, &error), GFNT_ERR_CORRUPT);
    EXPECT_NE(gfnt_face_load(blob, 2, nullptr, nullptr, &face, &error),
        GFNT_OK);
    gfnt_face_free(face);
    face = nullptr;
    gfnt_blob_destroy(blob);
  }
  {
    GFNT_Blob * blob = nullptr;
    ASSERT_EQ(gfnt_blob_create_memory(outside.data(), outside.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_NE(gfnt_face_load(blob, 1, nullptr, nullptr, &face, &error),
        GFNT_OK) << "an offset past the end of the blob";
    gfnt_face_free(face);
    face = nullptr;
    EXPECT_NE(gfnt_face_load(blob, 2, nullptr, nullptr, &face, &error),
        GFNT_OK) << "an offset with no room for an offset table behind it";
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
