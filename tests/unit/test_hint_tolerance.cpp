/*
 * Tests for what a tolerant hinting context does with a program that goes wrong: carry on
 * past an operand it does not have or an index that is not there, and keep what it had done
 * when it cannot, which is what FreeType returns. A strict context fails at the first.
 *
 * Every face is on a 640-unit em, hinted at 10 pixels per em, so a font unit is one 26.6
 * unit. The square's corners are at y = 13 and 213; rounding to the grid takes them to 0
 * and 192.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "hint_fixture.h"

namespace {

/** A face with one square whose program is @p program, tolerant or strict. */
struct Tolerant {
  Fixture face;
  explicit Tolerant(const Bytes & program, bool tolerant = true)
      : face({gfnttest::build_glyf_glyph({kSquare}, program)}, {{300, 40}}, {}, 40, 10, {}, {},
            {}, tolerant) {}
};

Bytes y_axis(const Bytes & rest) {
  return cat({op({0x00}), rest});                 // SVTCA[y]
}

Bytes mdap(int point) {
  return cat({push({point}), op({0x2F})});        // MDAP[1]: round it to the grid
}

}  // namespace

TEST(HintTolerance, AMissingOperandIsAZeroAndTheProgramGoesOn) {
  // ADD with nothing on the stack adds two zeros; then point 0 is rounded as usual.
  Tolerant t(y_axis(cat({op({0x60}), op({0x21}), mdap(0)})));      // ADD; POP; MDAP[0]
  Result r(t.face, 0);

  ASSERT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(r.status, GFNT_HINT_STATUS_HINTED_WITH_ERRORS);
  EXPECT_EQ(r.fault.failure, GFNT_HINT_FAIL_STACK_UNDERFLOW);
  EXPECT_EQ(r.at(0).y, 0);                          // the rest of the program ran
}

TEST(HintTolerance, ZerosReplaceWhatWasThere) {
  // One value where ADD wants two: both become zero, so the sum is 0 and not 5.
  Tolerant t(cat({op({0x00}), push({5}), op({0x60}), push({0}), op({0x23}), op({0x48})}));
  Result r(t.face, 0);

  EXPECT_EQ(r.status, GFNT_HINT_STATUS_HINTED_WITH_ERRORS);
  EXPECT_EQ(r.at(0).y, 0);                          // SCFS put point 0 at (0 + 0)
}

TEST(HintTolerance, AStrictContextFailsAtTheFirst) {
  Tolerant t(y_axis(cat({op({0x60}), op({0x21}), mdap(0)})), false);
  Result r(t.face, 0);

  EXPECT_EQ(r.status, GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED);
  EXPECT_EQ(r.fault.failure, GFNT_HINT_FAIL_STACK_UNDERFLOW);
  EXPECT_EQ(r.at(0).y, 13);                         // as drawn
}

TEST(HintTolerance, AnIndexThatIsNotThereReadsAsZeroAndWritesNothing) {
  // RCVT of control value 999 pushes 0; the sum with 40 is 40; SCFS puts point 0 there.
  Tolerant t(cat({op({0x00}), push({40, 999}), op({0x45}), op({0x60}), push({0}), op({0x23}),
      op({0x48})}));
  Result r(t.face, 0);

  EXPECT_EQ(r.status, GFNT_HINT_STATUS_HINTED_WITH_ERRORS);
  EXPECT_EQ(r.at(0).y, 40);
  // Reading storage that is not there is a zero too, and writing it is ignored.
  Tolerant s(cat({op({0x00}), push({99, 7}), op({0x42}), push({99}), op({0x43}), push({0}),
      op({0x23}), op({0x48})}));
  Result q(s.face, 0);

  EXPECT_EQ(q.at(0).y, 0);
}

TEST(HintTolerance, APointThatIsNotThereIsSkippedAndTheRestAreStillMoved) {
  // MDAP on points 99 and then 2: the first is skipped, the second rounded.
  Tolerant t(y_axis(cat({mdap(99), mdap(2)})));
  Result r(t.face, 0);

  EXPECT_EQ(r.status, GFNT_HINT_STATUS_HINTED_WITH_ERRORS);
  EXPECT_EQ(r.at(2).y, 192);
}

TEST(HintTolerance, AFatalErrorKeepsWhatWasDone) {
  // Point 0 is rounded, then a division by zero stops the program; point 2 is never reached.
  Tolerant t(y_axis(cat({mdap(0), push({5, 0}), op({0x62}), mdap(2)})));
  Result r(t.face, 0);

  ASSERT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(r.status, GFNT_HINT_STATUS_HINTED_WITH_ERRORS);
  EXPECT_EQ(r.fault.failure, GFNT_HINT_FAIL_DIVIDE_BY_ZERO);
  EXPECT_EQ(r.at(0).y, 0);
  EXPECT_EQ(r.at(2).y, 213);
  Tolerant strict(y_axis(cat({mdap(0), push({5, 0}), op({0x62}), mdap(2)})), false);
  Result s(strict.face, 0);

  EXPECT_EQ(s.status, GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED);
  EXPECT_EQ(s.at(0).y, 13);                         // a strict failure is as drawn
}

TEST(HintTolerance, AMoveToAPointThatIsNotThereStillSetsTheReferencePoints) {
  // MDRP[rp0] of point 99 makes 99 the first reference point, so the MDRP of point 2 that
  // follows has nothing to measure from; without it, rp0 is point 0 and point 2 is rounded
  // to a whole distance from it (200 -> 192).
  Bytes round_two = cat({push({2}), op({0xC4})});                     // MDRP[00100]
  Tolerant bad(y_axis(cat({push({99}), op({0xD0}), round_two})));     // MDRP[10000]
  Tolerant fine(y_axis(round_two));
  Result a(bad.face, 0);
  Result b(fine.face, 0);

  EXPECT_EQ(a.at(2).y, 213);
  EXPECT_EQ(b.at(2).y, 13 + 192);
}

TEST(HintTolerance, TheFirstThingToleratedIsRecorded) {
  Tolerant t(y_axis(cat({op({0x60}), op({0x60}), op({0x60}), mdap(99)})));
  Result r(t.face, 0);

  EXPECT_EQ(r.status, GFNT_HINT_STATUS_HINTED_WITH_ERRORS);
  EXPECT_EQ(r.fault.failure, GFNT_HINT_FAIL_STACK_UNDERFLOW);   // the first of them
}

TEST(HintTolerance, AProgramThatNeverEndsIsStoppedAndKeepsWhatItDid) {
  // Round point 0, then jump back to the start for ever: JMPR with an offset of minus the
  // program's length. The instruction budget ends it.
  Bytes body = y_axis(mdap(0));
  Bytes loop = cat({body, push({-static_cast<int>(body.size() + 4)}), op({0x1C})});   // JMPR
  Tolerant t(loop);
  Result r(t.face, 0);

  ASSERT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(r.status, GFNT_HINT_STATUS_HINTED_WITH_ERRORS);
  EXPECT_EQ(r.fault.failure, GFNT_HINT_FAIL_BUDGET);
  EXPECT_EQ(r.at(0).y, 0);
  Tolerant strict(loop, false);
  Result s(strict.face, 0);

  EXPECT_EQ(s.status, GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
