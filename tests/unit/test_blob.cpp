/**
 * @file
 *
 * GFNT_Blob: the bytes, their length, and who owns them.
 *
 * documentation/design.md section 5.1.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <string>
#include <vector>

namespace {

/** Bytes that are not a font; the blob does not care what they are. */
std::vector<uint8_t> bytes(size_t n) {
  std::vector<uint8_t> data(n);
  for (size_t i = 0; i < n; i++) {
    data[i] = static_cast<uint8_t>(i * 7 + 1);
  }
  return data;
}

} // namespace

TEST(Blob, CopyIsIndependentOfTheCaller) {
  std::vector<uint8_t> source = bytes(64);
  GFNT_Blob * blob = nullptr;
  GFNT_Error error;

  ASSERT_EQ(gfnt_blob_create_memory(source.data(), source.size(),
                GFNT_BLOB_COPY, nullptr, nullptr, &blob, &error),
      GFNT_OK);
  ASSERT_NE(blob, nullptr);
  EXPECT_NE(gfnt_blob_data(blob), source.data()) << "a copy is not a borrow";
  EXPECT_EQ(gfnt_blob_size(blob), source.size());
  EXPECT_EQ(std::memcmp(gfnt_blob_data(blob), source.data(), source.size()), 0);

  source[0] = 0xFF;
  EXPECT_NE(gfnt_blob_data(blob)[0], 0xFF)
      << "the copy still reflects the caller's buffer";
  gfnt_blob_destroy(blob);
}

TEST(Blob, BorrowedUsesTheCallersBytes) {
  // The contract the enum name states: the bytes are the caller's, and they
  // must outlive the blob. A test can only show the first half.
  std::vector<uint8_t> source = bytes(16);
  GFNT_Blob * blob = nullptr;

  ASSERT_EQ(gfnt_blob_create_memory(source.data(), source.size(),
                GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
      GFNT_OK);
  EXPECT_EQ(gfnt_blob_data(blob), source.data());
  EXPECT_EQ(gfnt_blob_size(blob), source.size());
  gfnt_blob_destroy(blob);
}

TEST(Blob, CopyIsTheZeroOwnership) {
  // A caller who never heard of borrowing cannot select it by passing 0.
  EXPECT_EQ(GFNT_BLOB_COPY, 0);
}

TEST(Blob, EmptyIsAllowedAndCarriesNoBytes) {
  // Zero bytes is not a font, and every parser will say so; the blob itself
  // has no opinion, so that the refusal comes from one place.
  GFNT_Blob * blob = nullptr;

  ASSERT_EQ(gfnt_blob_create_memory(nullptr, 0, GFNT_BLOB_COPY, nullptr,
                nullptr, &blob, nullptr),
      GFNT_OK);
  EXPECT_EQ(gfnt_blob_data(blob), nullptr);
  EXPECT_EQ(gfnt_blob_size(blob), 0u);
  gfnt_blob_destroy(blob);
}

TEST(Blob, RefusesBytesThatAreNotThere) {
  GFNT_Blob * blob = nullptr;
  GFNT_Error error;

  EXPECT_EQ(gfnt_blob_create_memory(nullptr, 8, GFNT_BLOB_COPY, nullptr,
                nullptr, &blob, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(blob, nullptr) << "nothing is written on failure";
  EXPECT_EQ(error.result, GFNT_ERR_INVALID);
  EXPECT_NE(error.message, nullptr);
}

TEST(Blob, RefusesNowhereToPutTheBlob) {
  std::vector<uint8_t> source = bytes(4);
  EXPECT_EQ(gfnt_blob_create_memory(source.data(), source.size(),
                GFNT_BLOB_COPY, nullptr, nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
}

TEST(Blob, RefusesAnOwnershipItDoesNotDefine) {
  // A value from neither arm of the enum is a caller error, not a default:
  // guessing would make a miscast integer look like a copy.
  std::vector<uint8_t> source = bytes(4);
  GFNT_Blob * blob = nullptr;

  EXPECT_EQ(gfnt_blob_create_memory(source.data(), source.size(),
                static_cast<GFNT_BlobOwnership>(7), nullptr, nullptr, &blob,
                nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(blob, nullptr);
}

TEST(Blob, LimitRefusesRatherThanTruncating) {
  // CONVENTIONS.md section 5: ERR_LIMIT, never a silent truncation.
  std::vector<uint8_t> source = bytes(64);
  GFNT_Limits limits;
  GFNT_Blob * blob = nullptr;
  GFNT_Error error;

  gfnt_limits_default(&limits);
  limits.max_blob_bytes = 63;
  EXPECT_EQ(gfnt_blob_create_memory(source.data(), source.size(),
                GFNT_BLOB_COPY, &limits, nullptr, &blob, &error),
      GFNT_ERR_LIMIT);
  EXPECT_EQ(blob, nullptr);
  EXPECT_EQ(error.result, GFNT_ERR_LIMIT);

  limits.max_blob_bytes = 64;
  EXPECT_EQ(gfnt_blob_create_memory(source.data(), source.size(),
                GFNT_BLOB_COPY, &limits, nullptr, &blob, &error),
      GFNT_OK)
      << "the limit is a maximum, not a strict bound";
  gfnt_blob_destroy(blob);
}

TEST(Blob, FileIsReadWhole) {
  std::string contents = "not a font, but forty-two bytes of something ok";
  gfnttest::TempFile file(contents);
  ASSERT_TRUE(file.valid());
  GFNT_Blob * blob = nullptr;

  ASSERT_EQ(gfnt_blob_create_file(file.path(), nullptr, nullptr, &blob,
                nullptr),
      GFNT_OK);
  ASSERT_EQ(gfnt_blob_size(blob), contents.size());
  EXPECT_EQ(std::memcmp(gfnt_blob_data(blob), contents.data(),
                contents.size()),
      0);
  EXPECT_FALSE(gfnt_blob_is_mapped(blob))
      << "reading is the default; mapping is asked for by name";
  gfnt_blob_destroy(blob);
}

TEST(Blob, FileThatIsNotThereIsAnIoError) {
  GFNT_Blob * blob = nullptr;
  GFNT_Error error;

  EXPECT_EQ(gfnt_blob_create_file(gfnttest::missing_path(), nullptr, nullptr,
                &blob, &error),
      GFNT_ERR_IO);
  EXPECT_EQ(blob, nullptr);
  EXPECT_NE(error.message, nullptr);
}

TEST(Blob, FileLimitRefusesBeforeReading) {
  gfnttest::TempFile file("0123456789");
  ASSERT_TRUE(file.valid());
  GFNT_Limits limits;
  GFNT_Blob * blob = nullptr;

  gfnt_limits_default(&limits);
  limits.max_blob_bytes = 4;
  EXPECT_EQ(
      gfnt_blob_create_file(file.path(), &limits, nullptr, &blob, nullptr),
      GFNT_ERR_LIMIT);
  EXPECT_EQ(blob, nullptr);
}

TEST(Blob, FileRefusesNulls) {
  GFNT_Blob * blob = nullptr;
  EXPECT_EQ(gfnt_blob_create_file(nullptr, nullptr, nullptr, &blob, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_blob_create_file("anything", nullptr, nullptr, nullptr,
                nullptr),
      GFNT_ERR_INVALID);
}

TEST(Blob, MappingSeesTheSameBytesAsReading) {
  std::string contents = "mapped bytes and read bytes must agree exactly";
  gfnttest::TempFile file(contents);
  ASSERT_TRUE(file.valid());
  GFNT_Blob * read = nullptr;
  GFNT_Blob * mapped = nullptr;

  ASSERT_EQ(gfnt_blob_create_file(file.path(), nullptr, nullptr, &read,
                nullptr),
      GFNT_OK);
  ASSERT_EQ(gfnt_blob_create_mmap(file.path(), nullptr, nullptr, &mapped,
                nullptr),
      GFNT_OK);
  EXPECT_TRUE(gfnt_blob_is_mapped(mapped));
  ASSERT_EQ(gfnt_blob_size(mapped), gfnt_blob_size(read));
  EXPECT_EQ(std::memcmp(gfnt_blob_data(mapped), gfnt_blob_data(read),
                gfnt_blob_size(read)),
      0);
  gfnt_blob_destroy(mapped);
  gfnt_blob_destroy(read);
}

TEST(Blob, MappingAFileThatIsNotThereIsAnIoError) {
  GFNT_Blob * blob = nullptr;
  EXPECT_EQ(gfnt_blob_create_mmap(gfnttest::missing_path(), nullptr, nullptr,
                &blob, nullptr),
      GFNT_ERR_IO);
  EXPECT_EQ(blob, nullptr);
}

TEST(Blob, MappingLimitUnmapsAgain) {
  gfnttest::TempFile file("0123456789");
  ASSERT_TRUE(file.valid());
  GFNT_Limits limits;
  GFNT_Blob * blob = nullptr;

  gfnt_limits_default(&limits);
  limits.max_blob_bytes = 4;
  EXPECT_EQ(
      gfnt_blob_create_mmap(file.path(), &limits, nullptr, &blob, nullptr),
      GFNT_ERR_LIMIT);
  EXPECT_EQ(blob, nullptr);
}

TEST(Blob, MappingRefusesNulls) {
  GFNT_Blob * blob = nullptr;
  EXPECT_EQ(gfnt_blob_create_mmap(nullptr, nullptr, nullptr, &blob, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_blob_create_mmap("anything", nullptr, nullptr, nullptr,
                nullptr),
      GFNT_ERR_INVALID);
}

TEST(Blob, AccessorsAndDestroyAcceptNull) {
  EXPECT_EQ(gfnt_blob_data(nullptr), nullptr);
  EXPECT_EQ(gfnt_blob_size(nullptr), 0u);
  EXPECT_FALSE(gfnt_blob_is_mapped(nullptr));
  gfnt_blob_destroy(nullptr);
}

TEST(BlobDump, NamesTheSizeAndWhereTheBytesCameFrom) {
  std::vector<uint8_t> source = bytes(10);
  GFNT_Blob * copied = nullptr;
  GFNT_Blob * borrowed = nullptr;

  ASSERT_EQ(gfnt_blob_create_memory(source.data(), source.size(),
                GFNT_BLOB_COPY, nullptr, nullptr, &copied, nullptr),
      GFNT_OK);
  ASSERT_EQ(gfnt_blob_create_memory(source.data(), source.size(),
                GFNT_BLOB_BORROWED, nullptr, nullptr, &borrowed, nullptr),
      GFNT_OK);

  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  EXPECT_EQ(gfnt_blob_dump(copied, out.get()), GFNT_OK);
  EXPECT_EQ(gfnt_blob_dump(borrowed, out.get()), GFNT_OK);
  std::string text = out.finish();
  EXPECT_NE(text.find("10 bytes, owned"), std::string::npos) << text;
  EXPECT_NE(text.find("10 bytes, borrowed"), std::string::npos) << text;

  gfnt_blob_destroy(borrowed);
  gfnt_blob_destroy(copied);
}

TEST(BlobDump, RefusesNullsAndReportsAWriteFailure) {
  std::vector<uint8_t> source = bytes(4);
  GFNT_Blob * blob = nullptr;
  ASSERT_EQ(gfnt_blob_create_memory(source.data(), source.size(),
                GFNT_BLOB_COPY, nullptr, nullptr, &blob, nullptr),
      GFNT_OK);

  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  EXPECT_EQ(gfnt_blob_dump(nullptr, out.get()), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_blob_dump(blob, nullptr), GFNT_ERR_INVALID);

  gfnttest::FailingSink sink(0);
  ASSERT_NE(sink.get(), nullptr);
  EXPECT_EQ(gfnt_blob_dump(blob, sink.get()), GFNT_ERR_IO);
  EXPECT_TRUE(sink.failed());
  gfnt_blob_destroy(blob);
}

// Every allocation the blob makes has a failure arm, and an arm that has
// never run is an arm nobody has tested. The sweep walks one refusal through
// the whole of a create, and live() is what catches the other way of being
// wrong: reporting the failure and keeping what had already been built.
TEST(Blob, EveryAllocationFailureIsReportedAndLeaksNothing) {
  std::vector<uint8_t> source = bytes(32);
  size_t failures = 0;

  for (size_t n = 0; n < 6; n++) {
    gfnttest::FailingAllocator allocator(n);
    GFNT_Blob * blob = nullptr;
    GFNT_Error error;
    GFNT_Result result = gfnt_blob_create_memory(source.data(), source.size(),
        GFNT_BLOB_COPY, nullptr, allocator.get(), &blob, &error);

    if (result == GFNT_OK) {
      ASSERT_NE(blob, nullptr);
      allocator.stop_failing();
      gfnt_blob_destroy(blob);
    }
    else {
      EXPECT_EQ(result, GFNT_ERR_OOM) << "at refusal " << n;
      EXPECT_EQ(blob, nullptr) << "at refusal " << n;
      EXPECT_EQ(error.result, GFNT_ERR_OOM);
      failures++;
    }
    EXPECT_EQ(allocator.live(), 0u) << "at refusal " << n;
  }
  EXPECT_EQ(failures, 2u)
      << "a blob over memory makes exactly two allocations: the blob and the "
         "copy";
}

TEST(Blob, EveryAllocationFailureReadingOrMappingAFileIsReportedAndLeaksNothing) {
  // The memory sweep above cannot reach either of these: reading a file goes
  // through cutil, whose own refusal has to be translated into this library's
  // vocabulary, and mapping one allocates the blob while the bytes are already
  // mapped - so the failure arm has an unmap to do before it returns.
  gfnttest::TempFile file(std::string(64, 'x'));
  ASSERT_TRUE(file.valid());

  size_t read_failures = 0;
  for (size_t n = 0; n < 6; n++) {
    gfnttest::FailingAllocator allocator(n);
    GFNT_Blob * blob = nullptr;
    GFNT_Error error{};
    const GFNT_Result result = gfnt_blob_create_file(file.path(), nullptr,
        allocator.get(), &blob, &error);

    if (result == GFNT_OK) {
      allocator.stop_failing();
      gfnt_blob_destroy(blob);
    }
    else {
      EXPECT_EQ(result, GFNT_ERR_OOM) << "at refusal " << n;
      EXPECT_EQ(blob, nullptr) << "at refusal " << n;
      EXPECT_NE(error.message, nullptr) << "at refusal " << n;
      read_failures++;
    }
    EXPECT_EQ(allocator.live(), 0u) << "at refusal " << n;
  }
  EXPECT_GT(read_failures, 0u) << "reading a file allocates something";

  size_t map_failures = 0;
  for (size_t n = 0; n < 4; n++) {
    gfnttest::FailingAllocator allocator(n);
    GFNT_Blob * blob = nullptr;
    GFNT_Error error{};
    const GFNT_Result result = gfnt_blob_create_mmap(file.path(), nullptr,
        allocator.get(), &blob, &error);

    if (result == GFNT_OK) {
      allocator.stop_failing();
      gfnt_blob_destroy(blob);
    }
    else {
      EXPECT_EQ(result, GFNT_ERR_OOM) << "at refusal " << n;
      EXPECT_EQ(blob, nullptr) << "at refusal " << n;
      map_failures++;
    }
    EXPECT_EQ(allocator.live(), 0u) << "at refusal " << n;
  }
  EXPECT_EQ(map_failures, 1u) << "mapping allocates the blob and nothing else";
}

TEST(Blob, TheDumpSaysWhereTheBytesCameFrom) {
  // Three sources, three words, and the mapped one was the line no test read:
  // every other blob in the suite is memory or a file.
  gfnttest::TempFile file(std::string(48, 'y'));
  ASSERT_TRUE(file.valid());
  GFNT_Blob * mapped = nullptr;
  ASSERT_EQ(gfnt_blob_create_mmap(file.path(), nullptr, nullptr, &mapped,
      nullptr), GFNT_OK);
  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);

  EXPECT_EQ(gfnt_blob_dump(mapped, out.get()), GFNT_OK);
  const std::string text = out.finish();
  EXPECT_NE(text.find("mapped"), std::string::npos) << text;
  EXPECT_NE(text.find("48"), std::string::npos) << text;
  gfnt_blob_destroy(mapped);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
