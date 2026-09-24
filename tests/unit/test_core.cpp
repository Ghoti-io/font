/**
 * @file
 *
 * The core module: the result vocabulary, the limits, and the version.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstring>
#include <set>
#include <string>

#include <ghoti.io/font/libver.h>

// CONVENTIONS.md section 5: GFNT_RESULT_COUNT closes the enum so that a test
// can check the string table is complete. A new result that arrives without a
// string falls into the default arm and is caught here.
TEST(Result, EveryCodeHasItsOwnString) {
  std::set<std::string> seen;
  for (int i = 0; i < GFNT_RESULT_COUNT; i++) {
    const char * s = gfnt_result_string(static_cast<GFNT_Result>(i));
    ASSERT_NE(s, nullptr) << "result " << i;
    EXPECT_STRNE(s, "Unknown error") << "result " << i << " has no string";
    EXPECT_TRUE(seen.insert(s).second) << "result " << i << " shares a string";
  }
}

TEST(Result, OutOfRangeIsUnknownNotUndefined) {
  EXPECT_STREQ(gfnt_result_string(GFNT_RESULT_COUNT), "Unknown error");
  EXPECT_STREQ(gfnt_result_string(static_cast<GFNT_Result>(-1)), "Unknown error");
}

TEST(Result, ZeroIsSuccess) {
  EXPECT_EQ(GFNT_OK, 0);
  EXPECT_STREQ(gfnt_result_string(GFNT_OK), "No error");
}

TEST(Limits, DefaultsAreTheDocumentedOnes) {
  GFNT_Limits limits;
  std::memset(&limits, 0xA5, sizeof limits);
  gfnt_limits_default(&limits);
  // documentation/design.md section 15.2, and section 6.2 for the recursion
  // budgets.
  EXPECT_EQ(limits.max_blob_bytes, (size_t)256 * 1024 * 1024);
  EXPECT_EQ(limits.max_tables, 512u);
  EXPECT_EQ(limits.max_glyphs, 65535u) << "the format's own bound";
  EXPECT_EQ(limits.max_composite_depth, 16u);
  EXPECT_EQ(limits.max_outline_points, 65536u);
  EXPECT_EQ(limits.max_contours, 4096u);
  EXPECT_EQ(limits.max_ppem, 4096u);
  EXPECT_EQ(limits.max_raster_bytes, (size_t)64 * 1024 * 1024);
  EXPECT_EQ(limits.max_strikes, 256u);
  EXPECT_EQ(limits.max_name_records, 4096u);
  EXPECT_EQ(limits.max_axes, 64u);
  EXPECT_EQ(limits.max_lookup_depth, 6u) << "matches HarfBuzz";
  EXPECT_EQ(limits.max_ops_per_glyph, 64u);
  EXPECT_EQ(limits.max_paint_depth, 64u);
  EXPECT_EQ(limits.max_run_bytes, (size_t)16 * 1024 * 1024);
  EXPECT_EQ(limits.max_line_length, 4096u);
}

TEST(Limits, NullIsIgnored) {
  gfnt_limits_default(nullptr);
}

// The linked library's version is the one that matters when it differs from
// the header's; here they are the same build, so they must agree.
TEST(Version, LibraryAgreesWithHeader) {
  EXPECT_STREQ(gfnt_version_string(), GFNT_VERSION_STRING);
  EXPECT_EQ(gfnt_version_number(), GFNT_VERSION_NUMBER);
  EXPECT_EQ(gfnt_version_number(),
      GFNT_MAKE_VERSION(GFNT_VERSION_MAJOR, GFNT_VERSION_MINOR, GFNT_VERSION_PATCH));
}

TEST(Version, PackingIsOneBytePerComponent) {
  // libcurl's LIBCURL_VERSION_NUM layout: 1.2.3 reads as 0x010203, so a
  // plain < compares two versions correctly. CONVENTIONS.md section 4.
  EXPECT_EQ(GFNT_MAKE_VERSION(1, 2, 3), 0x010203u);
  EXPECT_LT(GFNT_MAKE_VERSION(1, 9, 9), GFNT_MAKE_VERSION(2, 0, 0));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
