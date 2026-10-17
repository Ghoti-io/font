/*
 * Allocation failure while hinting: every allocation the hinter makes is refused in turn,
 * on its own, from the first to the last, and from there on. A refusal is reported as one
 * (or the glyph comes back as well as the allocator allowed), nothing is left allocated,
 * and nothing crashes.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "hint_fixture.h"
#include "failing_allocator.h"

namespace {

Bytes program() {
  return cat({op({0x00}), push({0}), op({0x2F}), push({1}), op({0x2F}), push({2}), op({0x2F}),
      push({3}), op({0x2F}), op({0x31}), op({0x30})});     // MDAP[1] x4, IUP[x], IUP[y]
}

/** Load, make a context, hint the simple glyph and the composite; free everything. */
void run_once(const Bytes & bytes, const GFNT_Allocator * allocator, GFNT_Result * out_first_failure) {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_HintContext * context = nullptr;
  GFNT_Error error{};
  GFNT_Result r = gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED, nullptr,
      allocator, &blob, &error);

  *out_first_failure = GFNT_OK;
  if (r == GFNT_OK) r = gfnt_face_load(blob, 0, nullptr, allocator, &face, &error);
  if (r == GFNT_OK) {
    r = gfnt_hint_context_create(face, 10, GFNT_HINT_VERSION_40, GFNT_HINT_MODE_FONT, nullptr,
        &context, &error);
  }
  for (uint32_t glyph = 0; r == GFNT_OK && glyph < 2; ++glyph) {
    GFNT_Outline * outline = nullptr;
    GFNT_HintStatus status = GFNT_HINT_STATUS_HINTED;

    r = gfnt_face_hinted_outline(face, context, glyph, allocator, &outline, &status, nullptr, &error);
    gfnt_outline_destroy(outline);
  }
  *out_first_failure = r;
  gfnt_hint_context_destroy(context);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
}

}  // namespace

TEST(HintOom, EveryAllocationRefusedInTurnIsSurvivedAndNothingLeaks) {
  Bytes simple = gfnttest::build_glyf_glyph({kSquare}, program());
  Bytes composite = gfnttest::build_glyf_composite({{0, XY | 0x1, 100, 50, {}},
      {0, XY | ROUND | 0x1, 300, 17, {}}}, program());
  Fixture f({simple, composite}, {{300, 40}, {500, 0}}, program(), 40, 10);
  size_t requests = 0;
  GFNT_Result first = GFNT_OK;

  {
    gfnttest::FailingAllocator counter(static_cast<size_t>(-1));

    run_once(f.bytes, counter.get(), &first);
    ASSERT_EQ(first, GFNT_OK);
    requests = counter.requests();
    EXPECT_EQ(counter.live(), 0u);
  }
  ASSERT_GT(requests, 20u);
  for (size_t run : {static_cast<size_t>(0), static_cast<size_t>(1), static_cast<size_t>(2)}) {
    for (size_t fail_at = 0; fail_at <= requests; ++fail_at) {
      gfnttest::FailingAllocator allocator(fail_at, run);

      run_once(f.bytes, allocator.get(), &first);
      EXPECT_TRUE(first == GFNT_OK || first == GFNT_ERR_OOM)
          << "at request " << fail_at << " run " << run << ": " << first;
      EXPECT_EQ(allocator.live(), 0u) << "at request " << fail_at << " run " << run;
    }
  }
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
