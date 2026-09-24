/**
 * @file
 *
 * The allocator seam: the default is the suite's, not a second one.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <ghoti.io/cutil/allocator.h>

TEST(Allocator, DefaultIsTheSuiteDefault) {
  const GFNT_Allocator * a = gfnt_allocator_default();
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a, gcu_allocator_default())
      << "the font library shares cutil's allocator rather than defining "
         "another one";
  EXPECT_NE(a->malloc_fn, nullptr);
  EXPECT_NE(a->calloc_fn, nullptr);
  EXPECT_NE(a->realloc_fn, nullptr);
  EXPECT_NE(a->free_fn, nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
