/**
 * @file
 *
 * The checked reader: bounds, byte order, signedness, and sub-extents.
 *
 * documentation/design.md section 6. These are the tests that stand behind
 * every parser in the library, because a parser's own tests cannot tell a
 * correct parse from one that read two bytes past a table and found a zero.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstdint>
#include <vector>

#include "../../src/reader/reader.h"

namespace {

/** A reader over a fixed extent, with a cleared diagnostic beside it. */
struct Fixture {
  std::vector<uint8_t> bytes;
  GFNT_Error error{};
  GFNT_Reader reader{};

  explicit Fixture(std::vector<uint8_t> data,
      GFNT_Tag table = GFNT_TAG('t', 'e', 's', 't'))
      : bytes(std::move(data)) {
    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_reader_init(&reader, bytes.data(), bytes.size(), table,
                  &error),
        GFNT_OK);
  }
};

} // namespace

TEST(Reader, InitRefusesBytesThatAreNotThere) {
  GFNT_Reader reader;
  GFNT_Error error;
  gfnt_error_clear(&error);

  EXPECT_EQ(gfnt_reader_init(&reader, nullptr, 4, 0, &error), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_reader_init(nullptr, nullptr, 0, 0, &error), GFNT_ERR_INVALID);
}

TEST(Reader, InitOverNothingIsAnEmptyExtent) {
  // A zero-length table is legal in an sfnt directory, so a reader over one
  // must exist and refuse every read rather than being an error to create.
  GFNT_Reader reader;
  uint8_t value;

  ASSERT_EQ(gfnt_reader_init(&reader, nullptr, 0, 0, nullptr), GFNT_OK);
  EXPECT_EQ(gfnt_reader_remaining(&reader), 0u);
  EXPECT_EQ(gfnt_read_u8(&reader, &value), GFNT_ERR_CORRUPT);
}

TEST(Reader, ReadsBigEndianWhateverTheHostIs) {
  // Every sfnt-derived format is big-endian, and the reader assembles bytes
  // with shifts so that this test passes unchanged in the cross container.
  Fixture f({0x12, 0x34, 0x56, 0x78});
  uint16_t u16 = 0;
  uint32_t u32 = 0;

  ASSERT_EQ(gfnt_read_u16(&f.reader, &u16), GFNT_OK);
  EXPECT_EQ(u16, 0x1234u);
  ASSERT_EQ(gfnt_reader_seek(&f.reader, 0), GFNT_OK);
  ASSERT_EQ(gfnt_read_u32(&f.reader, &u32), GFNT_OK);
  EXPECT_EQ(u32, 0x12345678u);
  ASSERT_EQ(gfnt_reader_seek(&f.reader, 0), GFNT_OK);
  ASSERT_EQ(gfnt_read_u24(&f.reader, &u32), GFNT_OK);
  EXPECT_EQ(u32, 0x123456u) << "the CFF and EBDT offset size";
  EXPECT_EQ(gfnt_reader_tell(&f.reader), 3u);
}

TEST(Reader, TagReadsAsTheBytesInFileOrder) {
  Fixture f({'c', 'm', 'a', 'p'});
  GFNT_Tag tag = 0;

  ASSERT_EQ(gfnt_read_tag(&f.reader, &tag), GFNT_OK);
  EXPECT_EQ(tag, GFNT_TAG('c', 'm', 'a', 'p'))
      << "GFNT_TAG and a big-endian read must agree, or every table lookup "
         "compares two different numbers";
}

TEST(Reader, SignedValuesAreTwosComplementOnEveryPlatform) {
  // Casting 0xFFFF down to an int16_t is implementation-defined before C23;
  // the reader converts arithmetically, so these answers are the
  // specification's on every compiler.
  Fixture f({0xFF, 0xFF, 0x80, 0x00, 0x7F, 0xFF, 0x80, 0x00});
  int16_t s16 = 0;
  int8_t s8 = 0;
  int32_t s32 = 0;

  ASSERT_EQ(gfnt_read_s16(&f.reader, &s16), GFNT_OK);
  EXPECT_EQ(s16, -1);
  ASSERT_EQ(gfnt_read_s16(&f.reader, &s16), GFNT_OK);
  EXPECT_EQ(s16, -32768);
  ASSERT_EQ(gfnt_read_s16(&f.reader, &s16), GFNT_OK);
  EXPECT_EQ(s16, 32767);

  ASSERT_EQ(gfnt_reader_seek(&f.reader, 2), GFNT_OK);
  ASSERT_EQ(gfnt_read_s8(&f.reader, &s8), GFNT_OK);
  EXPECT_EQ(s8, -128);

  ASSERT_EQ(gfnt_reader_seek(&f.reader, 0), GFNT_OK);
  ASSERT_EQ(gfnt_read_s32(&f.reader, &s32), GFNT_OK);
  EXPECT_EQ(s32, -32768) << "0xFFFF8000";
}

TEST(Reader, LongDateTimeSpansTheWholeSignedRange) {
  // head's created and modified dates are signed 64-bit seconds since 1904,
  // and fonts in the wild carry nonsense in them - including the extreme
  // values, which is where a cast-based conversion would break.
  Fixture minus_one({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  Fixture most_negative({0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  Fixture epoch({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  int64_t value = 0;

  ASSERT_EQ(gfnt_read_longdatetime(&minus_one.reader, &value), GFNT_OK);
  EXPECT_EQ(value, -1);
  ASSERT_EQ(gfnt_read_longdatetime(&most_negative.reader, &value), GFNT_OK);
  EXPECT_EQ(value, INT64_MIN);
  ASSERT_EQ(gfnt_read_longdatetime(&epoch.reader, &value), GFNT_OK);
  EXPECT_EQ(value, 0);
}

TEST(Reader, FixedAndF2Dot14AreTheFormatsOwnTypes) {
  Fixture f({0x00, 0x01, 0x00, 0x00, 0x40, 0x00, 0xC0, 0x00});
  GFNT_F16Dot16 fixed = 0;
  GFNT_F2Dot14 short_fixed = 0;

  ASSERT_EQ(gfnt_read_fixed(&f.reader, &fixed), GFNT_OK);
  EXPECT_EQ(fixed, GFNT_F16DOT16_ONE) << "0x00010000 is 1.0";
  ASSERT_EQ(gfnt_read_f2dot14(&f.reader, &short_fixed), GFNT_OK);
  EXPECT_EQ(short_fixed, GFNT_F2DOT14_ONE) << "0x4000 is 1.0";
  ASSERT_EQ(gfnt_read_f2dot14(&f.reader, &short_fixed), GFNT_OK);
  EXPECT_EQ(short_fixed, -GFNT_F2DOT14_ONE) << "0xC000 is -1.0";
}

TEST(Reader, AReadPastTheEndFailsAndLeavesTheCursorAlone) {
  // The cursor must not move on a failed read: a parser that ignores one
  // result and reads the next field would otherwise be reading from a
  // position nothing chose.
  Fixture f({0x01, 0x02, 0x03});
  uint32_t u32 = 0;

  EXPECT_EQ(gfnt_read_u32(&f.reader, &u32), GFNT_ERR_CORRUPT);
  EXPECT_EQ(gfnt_reader_tell(&f.reader), 0u);
  EXPECT_EQ(f.error.result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(f.error.table, GFNT_TAG('t', 'e', 's', 't'))
      << "the diagnostic names the table, because \"corrupt\" is not a "
         "diagnostic";
  EXPECT_EQ(f.error.offset, 0u);
  EXPECT_NE(f.error.message, nullptr);
}

TEST(Reader, TheLastByteIsReadableAndTheNextIsNot) {
  Fixture f({0x01, 0x02});
  uint8_t value = 0;

  ASSERT_EQ(gfnt_read_u8(&f.reader, &value), GFNT_OK);
  ASSERT_EQ(gfnt_read_u8(&f.reader, &value), GFNT_OK);
  EXPECT_EQ(value, 0x02u);
  EXPECT_EQ(gfnt_reader_remaining(&f.reader), 0u);
  EXPECT_EQ(gfnt_read_u8(&f.reader, &value), GFNT_ERR_CORRUPT);
  EXPECT_EQ(f.error.offset, 2u) << "the offset of the read that failed";
}

TEST(Reader, OffsetArithmeticDoesNotWrap) {
  // A length near SIZE_MAX is what turns `cursor + count > length` into a
  // read that passes its own bounds check; every sum goes through safemath.
  Fixture f({0x01, 0x02, 0x03, 0x04});
  const uint8_t * block = nullptr;
  uint16_t value = 0;

  EXPECT_EQ(gfnt_read_bytes(&f.reader, SIZE_MAX, &block), GFNT_ERR_CORRUPT);
  EXPECT_EQ(block, nullptr);
  EXPECT_EQ(gfnt_reader_skip(&f.reader, SIZE_MAX), GFNT_ERR_CORRUPT);
  EXPECT_EQ(gfnt_reader_seek(&f.reader, SIZE_MAX), GFNT_ERR_CORRUPT);
  EXPECT_EQ(gfnt_reader_u16_at(&f.reader, SIZE_MAX - 1, &value),
      GFNT_ERR_CORRUPT);
  EXPECT_FALSE(gfnt_reader_has(&f.reader, SIZE_MAX));
  EXPECT_EQ(gfnt_reader_tell(&f.reader), 0u);
}

TEST(Reader, SeekToTheEndIsLegalAndPastItIsNot) {
  Fixture f({0x01, 0x02, 0x03, 0x04});

  EXPECT_EQ(gfnt_reader_seek(&f.reader, 4), GFNT_OK)
      << "the end is a position; a table read to completion sits there";
  EXPECT_EQ(gfnt_reader_remaining(&f.reader), 0u);
  EXPECT_EQ(gfnt_reader_seek(&f.reader, 5), GFNT_ERR_CORRUPT);
  EXPECT_EQ(gfnt_reader_tell(&f.reader), 4u) << "a failed seek moves nothing";
}

TEST(Reader, HasAnswersWithoutRecordingADiagnostic) {
  // A parser sizing an array before allocating it has not failed yet, and a
  // diagnostic left behind by a question would be reported as the cause of a
  // later, unrelated failure.
  Fixture f({0x01, 0x02});

  EXPECT_TRUE(gfnt_reader_has(&f.reader, 2));
  EXPECT_FALSE(gfnt_reader_has(&f.reader, 3));
  EXPECT_EQ(f.error.result, GFNT_OK);
  EXPECT_EQ(f.error.message, nullptr);
}

TEST(Reader, TheAtFamilyLeavesTheCursorWhereItWas) {
  // cmap format 4's parallel arrays and loca's entries are indexed, not
  // walked; an indexed read that moved the cursor would make the two
  // impossible to mix.
  Fixture f({0x00, 0x01, 0x00, 0x02, 0xFF, 0xFE});
  ASSERT_EQ(gfnt_reader_seek(&f.reader, 2), GFNT_OK);

  uint8_t u8 = 0;
  uint16_t u16 = 0;
  uint32_t u32 = 0;
  int16_t s16 = 0;

  EXPECT_EQ(gfnt_reader_u8_at(&f.reader, 0, &u8), GFNT_OK);
  EXPECT_EQ(u8, 0x00u);
  EXPECT_EQ(gfnt_reader_u16_at(&f.reader, 2, &u16), GFNT_OK);
  EXPECT_EQ(u16, 0x0002u);
  EXPECT_EQ(gfnt_reader_u32_at(&f.reader, 0, &u32), GFNT_OK);
  EXPECT_EQ(u32, 0x00010002u);
  EXPECT_EQ(gfnt_reader_s16_at(&f.reader, 4, &s16), GFNT_OK);
  EXPECT_EQ(s16, -2);
  EXPECT_EQ(gfnt_reader_tell(&f.reader), 2u);

  EXPECT_EQ(gfnt_reader_u32_at(&f.reader, 4, &u32), GFNT_ERR_CORRUPT);
  EXPECT_EQ(f.error.offset, 4u);
}

TEST(Reader, BytesBorrowFromTheExtent) {
  Fixture f({0x01, 0x02, 0x03, 0x04});
  const uint8_t * block = nullptr;

  ASSERT_EQ(gfnt_reader_skip(&f.reader, 1), GFNT_OK);
  ASSERT_EQ(gfnt_read_bytes(&f.reader, 2, &block), GFNT_OK);
  EXPECT_EQ(block, f.bytes.data() + 1);
  EXPECT_EQ(gfnt_reader_tell(&f.reader), 3u);

  // A zero-length block is not a failure: an empty glyph has no bytes and
  // wants no special case at every call site.
  const uint8_t * empty = reinterpret_cast<const uint8_t *>(1);
  ASSERT_EQ(gfnt_read_bytes(&f.reader, 0, &empty), GFNT_OK);
  EXPECT_EQ(empty, nullptr);
}

TEST(SubReader, CannotLeaveItsParent) {
  // The property the whole threat model leans on: a table cannot read into
  // its neighbour, because the extent it was handed cannot be widened.
  Fixture f({0x01, 0x02, 0x03, 0x04});
  GFNT_Reader child;

  EXPECT_EQ(gfnt_reader_sub(&f.reader, 2, 2, &child), GFNT_OK);
  EXPECT_EQ(child.length, 2u);
  EXPECT_EQ(gfnt_reader_sub(&f.reader, 2, 3, &child), GFNT_ERR_CORRUPT);
  EXPECT_EQ(gfnt_reader_sub(&f.reader, 5, 0, &child), GFNT_ERR_CORRUPT);
  EXPECT_EQ(gfnt_reader_sub(&f.reader, 2, SIZE_MAX - 1, &child),
      GFNT_ERR_CORRUPT);
}

TEST(SubReader, RestMeansToTheEndOfTheParent) {
  // The tables whose subtables carry no length of their own - cmap's, among
  // others - need this, and it must not become a second way to spell "empty".
  Fixture f({0x01, 0x02, 0x03, 0x04});
  GFNT_Reader child;

  ASSERT_EQ(gfnt_reader_sub(&f.reader, 1, GFNT_READER_REST, &child), GFNT_OK);
  EXPECT_EQ(child.length, 3u);
  ASSERT_EQ(gfnt_reader_sub(&f.reader, 4, GFNT_READER_REST, &child), GFNT_OK);
  EXPECT_EQ(child.length, 0u) << "an offset at the end yields an empty extent";
  ASSERT_EQ(gfnt_reader_sub(&f.reader, 1, 0, &child), GFNT_OK);
  EXPECT_EQ(child.length, 0u) << "an explicit zero length is still empty";
}

TEST(SubReader, ReadsTheParentsBytesAtTheChildsOffsets) {
  Fixture f({0x11, 0x22, 0x33, 0x44});
  GFNT_Reader child;
  uint16_t value = 0;

  ASSERT_EQ(gfnt_reader_sub(&f.reader, 1, 2, &child), GFNT_OK);
  EXPECT_EQ(gfnt_reader_tell(&child), 0u)
      << "a child starts at its own beginning, whatever the parent's cursor";
  ASSERT_EQ(gfnt_read_u16(&child, &value), GFNT_OK);
  EXPECT_EQ(value, 0x2233u);
  EXPECT_EQ(gfnt_read_u16(&child, &value), GFNT_ERR_CORRUPT)
      << "the child cannot read the parent's fourth byte";
}

TEST(SubReader, IgnoresTheParentsCursor) {
  // Every sfnt-derived format means "from the start of the table" by an
  // offset, so a sub-reader taken mid-parse must not be relative to wherever
  // the parse had got to.
  Fixture f({0x11, 0x22, 0x33, 0x44});
  GFNT_Reader child;
  uint8_t value = 0;

  ASSERT_EQ(gfnt_reader_skip(&f.reader, 3), GFNT_OK);
  ASSERT_EQ(gfnt_reader_sub(&f.reader, 0, 1, &child), GFNT_OK);
  ASSERT_EQ(gfnt_read_u8(&child, &value), GFNT_OK);
  EXPECT_EQ(value, 0x11u);
}

TEST(SubReader, InheritsTheTableAndTheDiagnostic) {
  Fixture f({0x11, 0x22});
  GFNT_Reader child;
  uint16_t value = 0;

  ASSERT_EQ(gfnt_reader_sub(&f.reader, 1, 1, &child), GFNT_OK);
  EXPECT_EQ(child.table, GFNT_TAG('t', 'e', 's', 't'));
  EXPECT_EQ(gfnt_read_u16(&child, &value), GFNT_ERR_CORRUPT);
  EXPECT_EQ(f.error.result, GFNT_ERR_CORRUPT)
      << "a subtable's failure reaches the caller's diagnostic";
  EXPECT_EQ(f.error.table, GFNT_TAG('t', 'e', 's', 't'));
}

TEST(SubReader, ACourseOfSubReadersStillCannotEscape) {
  Fixture f({1, 2, 3, 4, 5, 6, 7, 8});
  GFNT_Reader child;
  GFNT_Reader grandchild;

  ASSERT_EQ(gfnt_reader_sub(&f.reader, 2, 4, &child), GFNT_OK);
  ASSERT_EQ(gfnt_reader_sub(&child, 2, 2, &grandchild), GFNT_OK);
  EXPECT_EQ(grandchild.length, 2u);
  EXPECT_EQ(gfnt_reader_sub(&child, 2, 3, &grandchild), GFNT_ERR_CORRUPT)
      << "a grandchild cannot reach past its parent even though the "
         "grandparent has the bytes";
}

TEST(Reader, OverABlobIsTheOnlyWayBytesEnterTheLibrary) {
  std::vector<uint8_t> data = {0x00, 0x01, 0x00, 0x00};
  GFNT_Blob * blob = nullptr;
  ASSERT_EQ(gfnt_blob_create_memory(data.data(), data.size(), GFNT_BLOB_COPY,
                nullptr, nullptr, &blob, nullptr),
      GFNT_OK);

  GFNT_Reader reader;
  GFNT_Tag tag = 0;
  ASSERT_EQ(gfnt_reader_init_blob(&reader, blob, 0, nullptr), GFNT_OK);
  EXPECT_EQ(reader.length, data.size());
  ASSERT_EQ(gfnt_read_tag(&reader, &tag), GFNT_OK);
  EXPECT_EQ(tag, 0x00010000u) << "the sfnt version of a TrueType font";

  EXPECT_EQ(gfnt_reader_init_blob(&reader, nullptr, 0, nullptr),
      GFNT_ERR_INVALID);
  gfnt_blob_destroy(blob);
}

TEST(Reader, EveryEntryPointRefusesNullArguments) {
  // A parser that passes a NULL out-pointer is a bug in this library, not a
  // corrupt font, so these are ERR_INVALID and not ERR_CORRUPT.
  Fixture f({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
  GFNT_Reader * r = &f.reader;

  EXPECT_EQ(gfnt_read_u8(r, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_read_u16(r, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_read_u24(r, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_read_u32(r, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_read_s8(r, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_read_s16(r, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_read_s32(r, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_read_tag(r, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_read_fixed(r, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_read_f2dot14(r, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_read_longdatetime(r, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_read_bytes(r, 1, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_reader_u8_at(r, 0, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_reader_u16_at(r, 0, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_reader_u32_at(r, 0, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_reader_s16_at(r, 0, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_reader_sub(r, 0, 1, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_reader_sub(nullptr, 0, 1, r), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_reader_seek(nullptr, 0), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_reader_skip(nullptr, 0), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_read_u8(nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_reader_tell(nullptr), 0u);
  EXPECT_EQ(gfnt_reader_remaining(nullptr), 0u);
  EXPECT_FALSE(gfnt_reader_has(nullptr, 0));
  EXPECT_EQ(gfnt_reader_tell(r), 0u) << "none of that moved the cursor";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
