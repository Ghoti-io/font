/*
 * Tests for the TrueType virtual machine: the instructions that need no outline,
 * the limits, and the way a font's program fails.
 *
 * Programs are assembled by hand from opcodes; the expected values are worked out
 * from the specification's description of each instruction, not from running it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <cstring>
#include <initializer_list>
#include <vector>

#include <ghoti.io/font/allocator.h>

extern "C" {
#include "../../src/hint/vm.h"
}

#include <gtest/gtest.h>

namespace {

using Bytes = std::vector<uint8_t>;

enum : uint8_t {
  SVTCA_Y = 0x00, SVTCA_X = 0x01, SPVTCA_X = 0x03, SFVTCA_X = 0x05, SFVTPV = 0x0E,
  GPV = 0x0C, GFV = 0x0D, SRP0 = 0x10, SZP0 = 0x13, SZPS = 0x16, SLOOP = 0x17,
  RTG = 0x18, RTHG = 0x19, SMD = 0x1A, ELSE = 0x1B, JMPR = 0x1C, SCVTCI = 0x1D,
  SSWCI = 0x1E, SSW = 0x1F, DUP = 0x20, POP = 0x21, CLEAR = 0x22, SWAP = 0x23,
  DEPTH = 0x24, CINDEX = 0x25, MINDEX = 0x26, LOOPCALL = 0x2A, CALL = 0x2B,
  FDEF = 0x2C, ENDF = 0x2D, RTDG = 0x3D, WS = 0x42, RS = 0x43, WCVTP = 0x44,
  RCVT = 0x45, MPPEM = 0x4B, MPS = 0x4C, FLIPON = 0x4D, FLIPOFF = 0x4E,
  DEBUG_ = 0x4F, LT = 0x50, LTEQ = 0x51, GT = 0x52, GTEQ = 0x53, EQ = 0x54,
  NEQ = 0x55, ODD = 0x56, EVEN = 0x57, IF = 0x58, EIF = 0x59, AND = 0x5A,
  OR = 0x5B, NOT = 0x5C, SDB = 0x5E, SDS = 0x5F, ADD = 0x60, SUB = 0x61,
  DIV = 0x62, MUL = 0x63, ABS = 0x64, NEG = 0x65, FLOOR = 0x66, CEILING = 0x67,
  ROUND = 0x68, NROUND = 0x6C, WCVTF = 0x70, DELTAC1 = 0x73, SROUND = 0x76,
  S45ROUND = 0x77, JROT = 0x78, JROF = 0x79, ROFF = 0x7A, RUTG = 0x7C,
  RDTG = 0x7D, SCANCTRL = 0x85, GETINFO = 0x88, IDEF = 0x89, ROLL = 0x8A,
  MAX = 0x8B, MIN = 0x8C, SCANTYPE = 0x8D, INSTCTRL = 0x8E, MDAP = 0x2E,
};

/** Push these values (each as a signed word). */
Bytes push(std::initializer_list<int> values) {
  Bytes out;
  out.push_back(0x41);
  out.push_back(static_cast<uint8_t>(values.size()));
  for (int v : values) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
  }
  return out;
}

/** Concatenate programs and single opcodes. */
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes & part : parts) {
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

Bytes op(std::initializer_list<int> opcodes) {
  Bytes out;
  for (int o : opcodes) {
    out.push_back(static_cast<uint8_t>(o));
  }
  return out;
}

/** A context over the given programs, 16 ppem on a 1000-unit em unless asked. */
struct Vm {
  GFNT_HintContext * ctx = nullptr;
  Bytes fpgm;
  Bytes prep;
  GFNT_Result created = GFNT_ERR_INTERNAL;

  Vm(Bytes font_program = {}, Bytes cvt_program = {},
      std::vector<int32_t> cvt = {}, int version = 40, uint32_t ppem = 16,
      GFNT_HintShape shape = {64, 32, 16, 4, 8}, const GFNT_HintLimits * limits = nullptr)
      : fpgm(std::move(font_program)), prep(std::move(cvt_program)) {
    GFNT_HintParams params{};
    params.ppem = ppem;
    params.units_per_em = 1000;
    params.version = version;
    created = gfnt_hint_ctx_create(gfnt_allocator_default(), limits, &params,
        &shape, fpgm.data(), fpgm.size(), prep.data(), prep.size(), cvt.data(),
        cvt.size(), &ctx);
  }
  ~Vm() { gfnt_hint_ctx_destroy(ctx); }
  Vm(const Vm &) = delete;
  Vm & operator=(const Vm &) = delete;
};

/** One glyph run, owned. */
struct Prog {
  GFNT_HintRun * run = nullptr;
  GFNT_Result result = GFNT_ERR_INTERNAL;
  Bytes program;

  Prog(const Vm & vm, Bytes code) : program(std::move(code)) {
    result = gfnt_hint_run_glyph(vm.ctx, nullptr, program.data(), program.size(), &run);
  }
  ~Prog() { gfnt_hint_run_destroy(run); }
  Prog(const Prog &) = delete;
  Prog & operator=(const Prog &) = delete;

  size_t depth() const { return gfnt_hint_run_depth(run); }
  int32_t at(size_t i) const { return gfnt_hint_run_stack(run, i); }
  int32_t top() const { return at(depth() - 1); }
  GFNT_HintFailure failure() const { return gfnt_hint_run_fault(run).failure; }
};

/** Prog a program with no font programs and return its top of stack. */
int32_t eval(const Bytes & code) {
  Vm vm;
  Prog run(vm, code);
  EXPECT_EQ(run.result, GFNT_OK) << gfnt_hint_failure_name(run.failure());
  return run.depth() ? run.top() : INT32_MIN;
}

GFNT_HintFailure failure_of(const Bytes & code, const Vm & vm) {
  Prog run(vm, code);
  EXPECT_NE(run.result, GFNT_OK);
  return run.failure();
}

}  // namespace

TEST(HintVmStack, TheStackInstructionsMoveTheElementsTheSpecificationSays) {
  Vm vm;
  {
    Prog r(vm, cat({push({1, 2, 3}), op({DUP})}));
    ASSERT_EQ(r.depth(), 4u);
    EXPECT_EQ(r.at(2), 3);
    EXPECT_EQ(r.at(3), 3);
  }
  {
    Prog r(vm, cat({push({1, 2, 3}), op({SWAP})}));
    EXPECT_EQ(r.at(1), 3);
    EXPECT_EQ(r.at(2), 2);
  }
  {
    Prog r(vm, cat({push({1, 2, 3}), op({POP})}));
    EXPECT_EQ(r.depth(), 2u);
  }
  {
    Prog r(vm, cat({push({1, 2, 3}), op({CLEAR})}));
    EXPECT_EQ(r.depth(), 0u);
  }
  {
    Prog r(vm, cat({push({7, 8, 9}), op({DEPTH})}));
    EXPECT_EQ(r.top(), 3);
  }
  {
    // CINDEX 3 copies the third element from the top: e3 of (e1 e2 e3 e4).
    Prog r(vm, cat({push({10, 20, 30, 40, 3}), op({CINDEX})}));
    ASSERT_EQ(r.depth(), 5u);
    EXPECT_EQ(r.top(), 20);
  }
  {
    // MINDEX 3 moves it: (10 20 30 40) becomes (10 30 40 20).
    Prog r(vm, cat({push({10, 20, 30, 40, 3}), op({MINDEX})}));
    ASSERT_EQ(r.depth(), 4u);
    EXPECT_EQ(r.at(0), 10);
    EXPECT_EQ(r.at(1), 30);
    EXPECT_EQ(r.at(2), 40);
    EXPECT_EQ(r.at(3), 20);
  }
  {
    // ROLL is MINDEX 3: (c b a) becomes (b a c).
    Prog r(vm, cat({push({1, 2, 3}), op({ROLL})}));
    EXPECT_EQ(r.at(0), 2);
    EXPECT_EQ(r.at(1), 3);
    EXPECT_EQ(r.at(2), 1);
  }
}

TEST(HintVmStack, PushesExtendBytesAndSignExtendWords) {
  Vm vm;
  Prog r(vm, Bytes{0xB1, 200, 7,           // PUSHB[001]: two bytes
                  0xB8, 0xFF, 0xFE,       // PUSHW[000]: -2
                  0x40, 2, 1, 255,        // NPUSHB 2
                  0x41, 1, 0x80, 0x00});  // NPUSHW 1: -32768
  ASSERT_EQ(r.depth(), 6u);
  EXPECT_EQ(r.at(0), 200);
  EXPECT_EQ(r.at(1), 7);
  EXPECT_EQ(r.at(2), -2);
  EXPECT_EQ(r.at(4), 255);
  EXPECT_EQ(r.at(5), -32768);
}

TEST(HintVmArithmetic, ValuesAreTwentySixDotSixAndTheOrderIsTheSpecifications) {
  EXPECT_EQ(eval(cat({push({10, 3}), op({ADD})})), 13);
  EXPECT_EQ(eval(cat({push({10, 3}), op({SUB})})), 7);       // n2 - n1
  EXPECT_EQ(eval(cat({push({96, 128}), op({MUL})})), 192);   // 1.5 * 2.0 = 3.0
  EXPECT_EQ(eval(cat({push({96, 128}), op({DIV})})), 48);    // 1.5 / 2.0 = 0.75
  EXPECT_EQ(eval(cat({push({-5}), op({ABS})})), 5);
  EXPECT_EQ(eval(cat({push({5}), op({NEG})})), -5);
  EXPECT_EQ(eval(cat({push({1452}), op({FLOOR})})), 1408);   // 22.69 -> 22.0
  EXPECT_EQ(eval(cat({push({-51}), op({FLOOR})})), -64);     // -0.8 -> -1.0
  EXPECT_EQ(eval(cat({push({1429}), op({CEILING})})), 1472); // 22.33 -> 23.0
  EXPECT_EQ(eval(cat({push({-51}), op({CEILING})})), 0);     // -0.8 -> 0
  EXPECT_EQ(eval(cat({push({3, 9}), op({MAX})})), 9);
  EXPECT_EQ(eval(cat({push({3, -9}), op({MIN})})), -9);
  // Overflow wraps as 32-bit words do and does not trap.
  EXPECT_EQ(eval(cat({push({0x7FFF, 0x7FFF}), op({MUL}), push({0x7FFF, 0x7FFF}),
      op({MUL}), op({ADD})})), 2 * ((0x7FFF * 0x7FFF) / 64 + 0));
}

TEST(HintVmArithmetic, DivisionByZeroIsAnErrorAndMultiplicationRoundsToNearest) {
  Vm vm;
  EXPECT_EQ(failure_of(cat({push({64, 0}), op({DIV})}), vm),
      GFNT_HINT_FAIL_DIVIDE_BY_ZERO);
  EXPECT_EQ(eval(cat({push({2, 3}), op({DIV})})), 42);   // 128/3 = 42.67, truncated
  EXPECT_EQ(eval(cat({push({-2, 3}), op({DIV})})), -42);
  EXPECT_EQ(eval(cat({push({3, 3}), op({MUL})})), 0);    // 9/64
  EXPECT_EQ(eval(cat({push({11, 6}), op({MUL})})), 1);   // 66/64
  EXPECT_EQ(eval(cat({push({-11, 6}), op({MUL})})), -1); // symmetric
}

TEST(HintVmLogic, ComparisonsTakeTheOperandsInTheSpecifiedOrder) {
  // "GT a,b is b > a": the element nearer the top is e2.
  EXPECT_EQ(eval(cat({push({15, 20}), op({LT})})), 1);    // 15 < 20
  EXPECT_EQ(eval(cat({push({20, 15}), op({LT})})), 0);
  EXPECT_EQ(eval(cat({push({20, 20}), op({LTEQ})})), 1);
  EXPECT_EQ(eval(cat({push({20, 20}), op({GT})})), 0);
  EXPECT_EQ(eval(cat({push({20, 15}), op({GTEQ})})), 1);
  EXPECT_EQ(eval(cat({push({20, -12}), op({EQ})})), 0);
  EXPECT_EQ(eval(cat({push({20, -12}), op({NEQ})})), 1);
  EXPECT_EQ(eval(cat({push({-3, 2}), op({LT})})), 1);     // signed
  EXPECT_EQ(eval(cat({push({1, 33}), op({AND})})), 1);
  EXPECT_EQ(eval(cat({push({1, 0}), op({AND})})), 0);
  EXPECT_EQ(eval(cat({push({44, 0}), op({OR})})), 1);
  EXPECT_EQ(eval(cat({push({0, 0}), op({OR})})), 0);
  EXPECT_EQ(eval(cat({push({42}), op({NOT})})), 0);
  EXPECT_EQ(eval(cat({push({0}), op({NOT})})), 1);
}

TEST(HintVmRounding, EachModeRoundsAsItsNameSaysAndNegativesAreSymmetric) {
  struct Case { Bytes mode; int in; int out; };
  const std::vector<Case> cases = {
      {op({RTG}), 0, 0},    {op({RTG}), 31, 0},   {op({RTG}), 32, 64},
      {op({RTG}), 95, 64},  {op({RTG}), 96, 128}, {op({RTG}), -31, 0},
      {op({RTG}), -32, -64}, {op({RTG}), -96, -128},
      {op({RTHG}), 0, 32},  {op({RTHG}), 63, 32}, {op({RTHG}), 64, 96},
      {op({RTHG}), -1, -32}, {op({RTHG}), -33, -32},
      {op({RTDG}), 0, 0},   {op({RTDG}), 15, 0},  {op({RTDG}), 16, 32},
      {op({RTDG}), 47, 32}, {op({RTDG}), 48, 64},
      {op({RDTG}), 63, 0},  {op({RDTG}), 64, 64}, {op({RDTG}), -1, 0},
      {op({RUTG}), 0, 0},   {op({RUTG}), 1, 64},  {op({RUTG}), 64, 64},
      {op({RUTG}), 65, 128},
      {op({ROFF}), 37, 37}, {op({ROFF}), -37, -37},
  };
  for (const Case & c : cases) {
    EXPECT_EQ(eval(cat({c.mode, push({c.in}), op({ROUND})})), c.out)
        << "mode " << int(c.mode[0]) << " value " << c.in;
  }
  // NROUND compensates for the engine and does not round; there is none here.
  EXPECT_EQ(eval(cat({op({RTG}), push({37}), op({NROUND})})), 37);
}

TEST(HintVmRounding, SuperRoundWithTheRightOperandIsTheNamedMode) {
  // period 1 pixel (01), threshold field 8 = half a period: RTG.
  // The same with phase half a period (10): RTHG. Threshold field 0 means
  // period - 1: RUTG. Period 1/2 pixel (00) with threshold 8: RTDG.
  struct Case { int operand; uint8_t mode; };
  for (const Case & c : {Case{0x48, RTG}, Case{0x68, RTHG}, Case{0x40, RUTG},
                         Case{0x08, RTDG}}) {
    for (int v = -200; v <= 200; ++v) {
      EXPECT_EQ(eval(cat({push({c.operand}), op({SROUND}), push({v}), op({ROUND})})),
          eval(cat({op({c.mode}), push({v}), op({ROUND})})))
          << std::hex << c.operand << std::dec << " value " << v;
    }
  }
  // Two pixels (10), no phase, threshold 8: multiples of two pixels.
  EXPECT_EQ(eval(cat({push({0x88}), op({SROUND}), push({63}), op({ROUND})})), 0);
  EXPECT_EQ(eval(cat({push({0x88}), op({SROUND}), push({64}), op({ROUND})})), 128);
  EXPECT_EQ(eval(cat({push({0x88}), op({SROUND}), push({191}), op({ROUND})})), 128);
  // Only the low eight bits of the operand count.
  EXPECT_EQ(eval(cat({push({0x1148}), op({SROUND}), push({37}), op({ROUND})})), 64);
}

TEST(HintVmRounding, OddAndEvenRoundFirst) {
  EXPECT_EQ(eval(cat({op({RTG}), push({1453}), op({ODD})})), 1);   // 22.7 -> 23
  EXPECT_EQ(eval(cat({op({RTG}), push({1453}), op({EVEN})})), 0);
  EXPECT_EQ(eval(cat({op({RTG}), push({1408}), op({EVEN})})), 1);  // 22.0
}

TEST(HintVmStorage, StorageAndTheControlValueTableReadAndWrite) {
  Vm vm({}, {}, {500, -250});     // 8 and -4 pixels at 16 ppem on a 1000 em
  EXPECT_EQ(gfnt_hint_ctx_cvt(vm.ctx, 0), 512);
  EXPECT_EQ(gfnt_hint_ctx_cvt(vm.ctx, 1), -256);
  Prog r(vm, cat({push({3, 99}), op({WS}), push({3}), op({RS}),
                 push({0}), op({RCVT}),
                 push({1, 77}), op({WCVTP}), push({1}), op({RCVT}),
                 push({0, 100}), op({WCVTF}), push({0}), op({RCVT})}));
  ASSERT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(r.at(0), 99);
  EXPECT_EQ(r.at(1), 512);
  EXPECT_EQ(r.at(2), 77);
  EXPECT_EQ(r.at(3), 102);      // 100 font units at 16/1000 = 1.6 px = 102.4
  // A glyph's writes belong to that glyph: the context and the next glyph do not
  // see them.
  EXPECT_EQ(gfnt_hint_ctx_cvt(vm.ctx, 1), -256);
  Prog again(vm, cat({push({1}), op({RCVT}), push({3}), op({RS})}));
  EXPECT_EQ(again.at(0), -256);
  EXPECT_EQ(again.at(1), 0);
}

TEST(HintVmStorage, OutOfRangeIndicesAreErrors) {
  Vm vm({}, {}, {1, 2});
  EXPECT_EQ(failure_of(cat({push({32}), op({RS})}), vm), GFNT_HINT_FAIL_BAD_INDEX);
  EXPECT_EQ(failure_of(cat({push({-1}), op({RS})}), vm), GFNT_HINT_FAIL_BAD_INDEX);
  EXPECT_EQ(failure_of(cat({push({2, 5}), op({WS}), push({32, 1}), op({WS})}), vm),
      GFNT_HINT_FAIL_BAD_INDEX);
  EXPECT_EQ(failure_of(cat({push({2}), op({RCVT})}), vm), GFNT_HINT_FAIL_BAD_INDEX);
  EXPECT_EQ(failure_of(cat({push({2, 1}), op({WCVTP})}), vm), GFNT_HINT_FAIL_BAD_INDEX);
}

TEST(HintVmControl, IfElseEifTakeTheBranchTheValueSelects) {
  // IF a ELSE b EIF, with the pushes marking which ran.
  const Bytes body = cat({op({IF}), push({1}), op({ELSE}), push({2}), op({EIF}),
                          push({9})});
  EXPECT_EQ(eval(cat({push({1}), body})), 9);
  {
    Vm vm;
    Prog t(vm, cat({push({1}), body}));
    ASSERT_EQ(t.depth(), 2u);
    EXPECT_EQ(t.at(0), 1);
    Prog f(vm, cat({push({0}), body}));
    ASSERT_EQ(f.depth(), 2u);
    EXPECT_EQ(f.at(0), 2);
  }
  // Nested: an inner IF in the skipped branch does not end the outer.
  Vm vm;
  Prog n(vm, cat({push({0}), op({IF}), push({1}), op({IF}), push({2}), op({ELSE}),
                 push({3}), op({EIF}), push({4}), op({ELSE}), push({5}), op({EIF}),
                 push({6})}));
  ASSERT_EQ(n.result, GFNT_OK);
  ASSERT_EQ(n.depth(), 2u);
  EXPECT_EQ(n.at(0), 5);
  EXPECT_EQ(n.at(1), 6);
  // A branch with push data that looks like opcodes is skipped by its length.
  Prog d(vm, cat({push({0}), op({IF}), Bytes{0xB1, 0x59, 0x1B}, op({EIF}), push({8})}));
  ASSERT_EQ(d.result, GFNT_OK);
  EXPECT_EQ(d.top(), 8);
  // An IF never closed is an error, not a run off the end.
  EXPECT_EQ(failure_of(cat({push({0}), op({IF}), push({1})}), vm),
      GFNT_HINT_FAIL_TRUNCATED);
}

TEST(HintVmControl, JumpsAreRelativeToTheInstructionAndLandOnInstructions) {
  Vm vm;
  // JMPR over a push: offset = 1 (JMPR itself) + 3 (PUSHB 7 is 2 bytes; use PUSHW).
  Bytes skip = {0xB8, 0x00, 0x07};            // 3 bytes
  Prog a(vm, cat({push({4}), op({JMPR}), skip, push({5})}));
  // offset 4 from JMPR: JMPR(1) + skip(3) -> lands on the NPUSHW.
  ASSERT_EQ(a.result, GFNT_OK);
  ASSERT_EQ(a.depth(), 1u);
  EXPECT_EQ(a.at(0), 5);
  // JROT taken / not taken, JROF taken / not taken. Stack: offset, condition.
  Prog t(vm, cat({push({4, 1}), op({JROT}), skip, push({5})}));
  EXPECT_EQ(t.depth(), 1u);
  Prog n(vm, cat({push({4, 0}), op({JROT}), skip, push({5})}));
  EXPECT_EQ(n.depth(), 2u);
  Prog f(vm, cat({push({4, 0}), op({JROF}), skip, push({5})}));
  EXPECT_EQ(f.depth(), 1u);
  Prog g(vm, cat({push({4, 1}), op({JROF}), skip, push({5})}));
  EXPECT_EQ(g.depth(), 2u);
  // Into the middle of an operand, before the start, to itself: errors.
  EXPECT_EQ(failure_of(cat({push({2}), op({JMPR}), skip}), vm), GFNT_HINT_FAIL_BAD_JUMP);
  EXPECT_EQ(failure_of(cat({push({-200}), op({JMPR})}), vm), GFNT_HINT_FAIL_BAD_JUMP);
  EXPECT_EQ(failure_of(cat({push({0}), op({JMPR})}), vm), GFNT_HINT_FAIL_BAD_JUMP);
  EXPECT_EQ(failure_of(cat({push({1000}), op({JMPR})}), vm), GFNT_HINT_FAIL_BAD_JUMP);
}

TEST(HintVmControl, ALoopWithoutAnEndStopsAtTheBudget) {
  GFNT_HintLimits limits;
  gfnt_hint_limits_default(&limits);
  limits.max_instructions = 1000;
  Vm vm({}, {}, {}, 40, 16, {64, 32, 16, 4, 8}, &limits);
  // Push -N and jump back to the push: JMPR with a negative offset to the start.
  Bytes loop = cat({push({-5}), op({JMPR})});   // NPUSHW(4) + JMPR at 4 -> offset -4 would be start
  Prog r(vm, cat({push({-4}), op({JMPR})}));      // jumps to the NPUSHW at 0
  EXPECT_EQ(r.result, GFNT_ERR_LIMIT);
  EXPECT_EQ(r.failure(), GFNT_HINT_FAIL_BUDGET);
}

TEST(HintVmFunctions, FdefCallLoopcallAndNestingWork) {
  // fpgm: function 0 adds 1; function 1 calls function 0 twice.
  Bytes fpgm = cat({push({0}), op({FDEF}), push({1}), op({ADD, ENDF}),
                    push({1}), op({FDEF}), push({0}), op({CALL}), push({0}),
                    op({CALL, ENDF})});
  Vm vm(fpgm);
  ASSERT_EQ(vm.created, GFNT_OK);
  ASSERT_TRUE(gfnt_hint_ctx_usable(vm.ctx));
  Prog c(vm, cat({push({10, 1}), op({CALL})}));
  ASSERT_EQ(c.result, GFNT_OK);
  EXPECT_EQ(c.top(), 12);
  Prog l(vm, cat({push({100, 5, 0}), op({LOOPCALL})}));
  ASSERT_EQ(l.result, GFNT_OK);
  EXPECT_EQ(l.top(), 105);
  Prog z(vm, cat({push({100, 0, 0}), op({LOOPCALL})}));   // zero times
  EXPECT_EQ(z.top(), 100);
  EXPECT_EQ(failure_of(cat({push({2}), op({CALL})}), vm), GFNT_HINT_FAIL_UNDEFINED_CALL);
  EXPECT_EQ(failure_of(cat({push({16}), op({CALL})}), vm), GFNT_HINT_FAIL_BAD_INDEX);
  EXPECT_EQ(failure_of(cat({push({-1}), op({CALL})}), vm), GFNT_HINT_FAIL_BAD_INDEX);
  EXPECT_EQ(failure_of(op({ENDF}), vm), GFNT_HINT_FAIL_BAD_STRUCTURE);
}

TEST(HintVmFunctions, DefinitionsBelongToTheFontProgramsOnly) {
  Vm vm(cat({push({0}), op({FDEF, ENDF})}));
  EXPECT_EQ(failure_of(cat({push({0}), op({FDEF, ENDF})}), vm),
      GFNT_HINT_FAIL_BAD_STRUCTURE);
  EXPECT_EQ(failure_of(cat({push({200}), op({IDEF, ENDF})}), vm),
      GFNT_HINT_FAIL_BAD_STRUCTURE);
  // Nested definitions are refused when the font program is built.
  Vm nested(cat({push({0}), op({FDEF}), push({1}), op({FDEF, ENDF, ENDF})}));
  EXPECT_FALSE(gfnt_hint_ctx_usable(nested.ctx));
  EXPECT_EQ(gfnt_hint_ctx_fault(nested.ctx).failure, GFNT_HINT_FAIL_BAD_STRUCTURE);
}

TEST(HintVmFunctions, InstructionDefinitionsGiveMeaningToUndefinedOpcodes) {
  // IDEF 0x91 (GETVARIATION, which the specification asks fonts to define).
  Vm vm(cat({push({0x91}), op({IDEF}), push({7, 8}), op({ENDF})}));
  ASSERT_TRUE(gfnt_hint_ctx_usable(vm.ctx));
  Prog r(vm, op({0x91}));
  ASSERT_EQ(r.result, GFNT_OK);
  ASSERT_EQ(r.depth(), 2u);
  EXPECT_EQ(r.at(1), 8);
  // An opcode that is already defined ignores an IDEF; 0x28 has none.
  Vm over(cat({push({0x60}), op({IDEF}), push({1}), op({ENDF})}));
  Prog add(over, cat({push({1, 2}), op({ADD})}));
  EXPECT_EQ(add.top(), 3);
  EXPECT_EQ(failure_of(op({0x28}), over), GFNT_HINT_FAIL_BAD_OPCODE);
  // The last definition of an opcode is the one that runs.
  Vm twice(cat({push({0x28}), op({IDEF}), push({1}), op({ENDF}),
                push({0x28}), op({IDEF}), push({2}), op({ENDF})}));
  Prog t(twice, op({0x28}));
  EXPECT_EQ(t.top(), 2);
}

TEST(HintVmFunctions, RecursionAndHugeLoopsAreBounded) {
  Vm vm(cat({push({0}), op({FDEF}), push({0}), op({CALL, ENDF})}));
  ASSERT_TRUE(gfnt_hint_ctx_usable(vm.ctx));
  Prog r(vm, cat({push({0}), op({CALL})}));
  EXPECT_EQ(r.result, GFNT_ERR_LIMIT);
  EXPECT_EQ(r.failure(), GFNT_HINT_FAIL_CALL_DEPTH);
  // A function that does almost nothing, called as many times as a word allows,
  // twice nested: 32767 * 32767 is a billion calls from a few bytes. The budget
  // ends it.
  Vm big(cat({push({0}), op({FDEF, ENDF}),
              push({1}), op({FDEF}), push({32767, 0}), op({LOOPCALL, ENDF})}));
  ASSERT_TRUE(gfnt_hint_ctx_usable(big.ctx));
  Prog b(big, cat({push({32767, 1}), op({LOOPCALL})}));
  EXPECT_EQ(b.result, GFNT_ERR_LIMIT);
  EXPECT_EQ(b.failure(), GFNT_HINT_FAIL_BUDGET);
}

TEST(HintVmGraphicsState, TheDefaultsAreTheSpecificationsAndPrepChangesThem) {
  Vm plain;
  const GFNT_HintGState * gs = gfnt_hint_ctx_gstate(plain.ctx);
  EXPECT_TRUE(gs->auto_flip);
  EXPECT_EQ(gs->cvt_cut_in, 68);          // 17/16 pixel
  EXPECT_EQ(gs->delta_base, 9);
  EXPECT_EQ(gs->delta_shift, 3);
  EXPECT_EQ(gs->loop, 1);
  EXPECT_EQ(gs->min_distance, 64);
  EXPECT_EQ(gs->sw_cut_in, 0);
  EXPECT_EQ(gs->sw_value, 0);
  EXPECT_EQ(gs->round, GFNT_HINT_ROUND_GRID);
  EXPECT_EQ(gs->pv[0], 0x4000);
  EXPECT_EQ(gs->fv[0], 0x4000);
  EXPECT_EQ(gs->zp[0], 1);
  EXPECT_EQ(gs->rp[1], 0u);

  // prep: cut-in 3 px, min distance half a pixel, delta base 6, flip off,
  // single width value 500 font units (= 8 px), round to half grid.
  Vm prepped({}, cat({push({192}), op({SCVTCI}), push({32}), op({SMD}),
                      push({6}), op({SDB}), op({FLIPOFF}), push({500}), op({SSW}),
                      op({RTHG}), push({40}), op({SSWCI}), push({5}), op({SDS}),
                      push({0x0A10}), op({SCANCTRL}), push({4}), op({SCANTYPE})}));
  ASSERT_TRUE(gfnt_hint_ctx_usable(prepped.ctx));
  gs = gfnt_hint_ctx_gstate(prepped.ctx);
  EXPECT_FALSE(gs->auto_flip);
  EXPECT_EQ(gs->cvt_cut_in, 192);
  EXPECT_EQ(gs->min_distance, 32);
  EXPECT_EQ(gs->delta_base, 6);
  EXPECT_EQ(gs->delta_shift, 5);
  EXPECT_EQ(gs->sw_value, 512);
  EXPECT_EQ(gs->sw_cut_in, 40);
  // The rounding state is the one thing prep sets that a glyph does not start
  // from (FreeType resets it, and a glyph that wants half-grid says so).
  EXPECT_EQ(gs->round, GFNT_HINT_ROUND_GRID);
  EXPECT_EQ(gs->scan_control, 0x0A10);
  EXPECT_EQ(gs->scan_type, 4);
  // A glyph starts from what prep left, and its own changes stay its own.
  Prog r(prepped, cat({push({77}), op({SMD}), push({1}), op({SLOOP})}));
  EXPECT_EQ(gfnt_hint_run_gstate(r.run)->min_distance, 77);
  EXPECT_EQ(gfnt_hint_run_gstate(r.run)->loop, 1);
  EXPECT_EQ(gfnt_hint_ctx_gstate(prepped.ctx)->min_distance, 32);
  Prog again(prepped, op({DEPTH}));
  EXPECT_EQ(gfnt_hint_run_gstate(again.run)->min_distance, 32);
}

TEST(HintVmGraphicsState, BadStateOperandsAreErrors) {
  Vm vm;
  EXPECT_EQ(failure_of(cat({push({-1}), op({SLOOP})}), vm), GFNT_HINT_FAIL_BAD_ARGUMENT);
  EXPECT_EQ(failure_of(cat({push({7}), op({SDS})}), vm), GFNT_HINT_FAIL_BAD_ARGUMENT);
  EXPECT_EQ(failure_of(cat({push({2}), op({SZP0})}), vm), GFNT_HINT_FAIL_BAD_ARGUMENT);
  Prog ok(vm, cat({push({0}), op({SZPS})}));
  EXPECT_EQ(gfnt_hint_run_gstate(ok.run)->zp[2], 0);
}

TEST(HintVmGraphicsState, MppemAndGetinfoReportTheSizeAndTheVersion) {
  Vm v40({}, {}, {}, 40, 22);
  EXPECT_EQ(Prog(v40, op({MPPEM})).top(), 22);
  EXPECT_EQ(Prog(v40, cat({push({1}), op({GETINFO})})).top() & 0xFF, 40);
  Vm v35({}, {}, {}, 35, 9);
  EXPECT_EQ(Prog(v35, cat({push({1}), op({GETINFO})})).top() & 0xFF, 35);
  EXPECT_EQ(Prog(v35, cat({push({0}), op({GETINFO})})).top(), 0);
}

TEST(HintVmGraphicsState, VectorsAreSetByAxisAndReportedBack) {
  Vm vm;
  Prog r(vm, op({SVTCA_Y, GPV, GFV, SVTCA_X, GPV}));
  ASSERT_EQ(r.depth(), 6u);
  EXPECT_EQ(r.at(0), 0);        // pv x
  EXPECT_EQ(r.at(1), 0x4000);   // pv y
  EXPECT_EQ(r.at(3), 0x4000);   // fv y
  EXPECT_EQ(r.at(4), 0x4000);   // pv x again
  Prog s(vm, op({SPVTCA_X, SVTCA_Y, SFVTPV, GFV}));
  EXPECT_EQ(s.at(0), 0);
  EXPECT_EQ(s.at(1), 0x4000);
}

TEST(HintVmGraphicsState, InstctrlSetsItsFlagsAndTheSelectorIsOnTop) {
  // Only the control value program may set selectors 1 and 2.
  Vm in_prep({}, cat({push({1, 1}), op({INSTCTRL}), push({4, 3}), op({INSTCTRL})}));
  EXPECT_EQ(gfnt_hint_ctx_instruct_control(in_prep.ctx), 5u);
  Vm vm;
  Prog r(vm, cat({push({1, 1}), op({INSTCTRL})}));
  EXPECT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(gfnt_hint_run_instruct_control(r.run), 0u);   // ignored from a glyph
  Prog native(vm, cat({push({4, 3}), op({INSTCTRL})}));
  EXPECT_EQ(gfnt_hint_run_instruct_control(native.run), 4u);
  Vm off({}, cat({push({1, 1}), op({INSTCTRL}), push({0, 1}), op({INSTCTRL})}));
  EXPECT_EQ(gfnt_hint_ctx_instruct_control(off.ctx), 0u);
}

TEST(HintVmDelta, DeltacMovesAControlValueOnlyAtItsSize) {
  // Relative ppem 3 (high nibble) with delta_base 9: 12 ppem. Magnitude 8 is +1
  // step; delta_shift 3 makes a step 1/8 pixel = 8 units.
  const int arg = (3 << 4) | 8;
  Vm at12({}, {}, {64}, 40, 12);
  Prog r(at12, cat({push({arg, 0, 1}), op({DELTAC1}), push({0}), op({RCVT})}));
  ASSERT_EQ(r.result, GFNT_OK);
  EXPECT_EQ(r.top(), 768 / 1000 * 0 + gfnt_hint_ctx_cvt(at12.ctx, 0) + 8);
  Vm at13({}, {}, {64}, 40, 13);
  Prog q(at13, cat({push({arg, 0, 1}), op({DELTAC1}), push({0}), op({RCVT})}));
  EXPECT_EQ(q.top(), gfnt_hint_ctx_cvt(at13.ctx, 0));
  // Magnitude 7 is -1 step; magnitude 0 is -8 steps; 15 is +8.
  Vm m({}, {}, {64}, 40, 12);
  Prog neg(m, cat({push({(3 << 4) | 0, 0, 1}), op({DELTAC1}), push({0}), op({RCVT})}));
  EXPECT_EQ(neg.top(), gfnt_hint_ctx_cvt(m.ctx, 0) - 64);
}

TEST(HintVmFaults, AFailingFontProgramMakesTheContextUnusable) {
  Vm bad_fpgm(op({ADD}));        // underflow
  EXPECT_EQ(bad_fpgm.created, GFNT_OK);
  EXPECT_FALSE(gfnt_hint_ctx_usable(bad_fpgm.ctx));
  EXPECT_EQ(gfnt_hint_ctx_fault(bad_fpgm.ctx).failure, GFNT_HINT_FAIL_STACK_UNDERFLOW);
  EXPECT_EQ(gfnt_hint_ctx_fault(bad_fpgm.ctx).program, GFNT_HINT_PROGRAM_FONT);
  GFNT_HintRun * run = nullptr;
  Bytes none;
  EXPECT_EQ(gfnt_hint_run_glyph(bad_fpgm.ctx, nullptr, none.data(), 0, &run), GFNT_ERR_INVALID);
  Vm bad_prep({}, op({ADD}));
  EXPECT_FALSE(gfnt_hint_ctx_usable(bad_prep.ctx));
  EXPECT_EQ(gfnt_hint_ctx_fault(bad_prep.ctx).program, GFNT_HINT_PROGRAM_CVT);
}

TEST(HintVmFaults, EveryPopFailsCleanlyOnAnEmptyStack) {
  Vm vm;
  // Every defined opcode whose table says it pops a fixed number, given too few.
  for (unsigned o = 0; o < 256; ++o) {
    if (o == 0x40 || o == 0x41 || (o >= 0xB0 && o <= 0xBF)) continue;
    Bytes code = {static_cast<uint8_t>(o)};
    Prog r(vm, code);
    // Whatever happens, it is a named outcome and not a crash.
    EXPECT_TRUE(r.result == GFNT_OK || r.failure() != GFNT_HINT_OK) << o;
  }
}

TEST(HintVmFaults, ThePushesAndTheStackCapacityAreChecked) {
  Vm small({}, {}, {}, 40, 16, {0, 8, 4, 4, 8});   // capacity 64 (minimum)
  // 65 values on a stack of 64.
  Bytes many(2 + 255, 1);
  many[0] = 0x40;
  many[1] = 255;
  EXPECT_EQ(failure_of(many, small), GFNT_HINT_FAIL_STACK_OVERFLOW);
  EXPECT_EQ(failure_of(Bytes{0x40, 5, 1, 2}, small), GFNT_HINT_FAIL_TRUNCATED);
  EXPECT_EQ(failure_of(Bytes{0x41, 1, 0}, small), GFNT_HINT_FAIL_TRUNCATED);
}

TEST(HintVmFaults, EveryPrefixAndEveryByteChangeOfAProgramEndsCleanly) {
  const Bytes rich = cat({push({3, 4}), op({ADD, DUP, MUL}), push({2}), op({IF}),
                          push({1}), op({ELSE}), push({2}), op({EIF}),
                          push({0}), op({FDEF}), op({POP, ENDF}),
                          push({5, 1}), op({WS}), push({1}), op({RS}),
                          op({RTG}), push({33}), op({ROUND, DEPTH, CLEAR})});
  Vm vm(cat({push({0}), op({FDEF}), push({1}), op({ADD, ENDF})}));
  ASSERT_TRUE(gfnt_hint_ctx_usable(vm.ctx));
  for (size_t cut = 0; cut <= rich.size(); ++cut) {
    Bytes prefix(rich.begin(), rich.begin() + static_cast<long>(cut));
    Prog r(vm, prefix);
    EXPECT_NE(r.run, nullptr);
  }
  for (size_t at = 0; at < rich.size(); ++at) {
    for (int delta : {1, 0x40, 0x7F, 0xFF}) {
      Bytes mutated = rich;
      mutated[at] = static_cast<uint8_t>(mutated[at] + delta);
      Prog r(vm, mutated);
      EXPECT_NE(r.run, nullptr);
    }
  }
}

TEST(HintVmContext, BadArgumentsAndLimitsAreRefused) {
  GFNT_HintContext * ctx = nullptr;
  GFNT_HintParams params{};
  params.ppem = 16;
  params.units_per_em = 1000;
  params.version = 40;
  GFNT_HintShape shape{64, 32, 16, 4, 8};

  EXPECT_EQ(gfnt_hint_ctx_create(nullptr, nullptr, &params, &shape, nullptr, 0,
      nullptr, 0, nullptr, 0, &ctx), GFNT_ERR_INVALID);
  params.version = 38;
  EXPECT_EQ(gfnt_hint_ctx_create(gfnt_allocator_default(), nullptr, &params, &shape,
      nullptr, 0, nullptr, 0, nullptr, 0, &ctx), GFNT_ERR_INVALID);
  params.version = 40;
  params.ppem = 0;
  EXPECT_EQ(gfnt_hint_ctx_create(gfnt_allocator_default(), nullptr, &params, &shape,
      nullptr, 0, nullptr, 0, nullptr, 0, &ctx), GFNT_ERR_INVALID);
  params.ppem = 16;
  GFNT_HintShape huge{64, 1u << 24, 16, 4, 8};
  EXPECT_EQ(gfnt_hint_ctx_create(gfnt_allocator_default(), nullptr, &params, &huge,
      nullptr, 0, nullptr, 0, nullptr, 0, &ctx), GFNT_ERR_LIMIT);
  EXPECT_EQ(ctx, nullptr);
  gfnt_hint_ctx_destroy(nullptr);
  gfnt_hint_run_destroy(nullptr);
}

namespace {

/** A face with a font program, a control value program, control values and maxp sizes. */
struct Hinted {
  Bytes bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  Hinted(const Bytes & fpgm, const Bytes & prep, const std::vector<int16_t> & cvt,
         uint16_t storage = 8, uint16_t functions = 4, uint16_t stack = 32,
         uint32_t maxp_version = 0x00010000) {
    Bytes glyf;
    Bytes loca;
    bool is_long = false;
    Bytes maxp = gfnttest::build_maxp(1, maxp_version);
    Bytes cvt_table;

    gfnttest::build_glyf_and_loca({gfnttest::build_glyf_glyph(
        {{{0, 0, true}, {100, 0, true}, {100, 100, true}}})}, &glyf, &loca, &is_long);
    if (maxp_version == 0x00010000) {
      gfnttest::patch_u16(maxp, 18, storage);
      gfnttest::patch_u16(maxp, 20, functions);
      gfnttest::patch_u16(maxp, 22, 2);
      gfnttest::patch_u16(maxp, 24, stack);
    }
    for (int16_t v : cvt) {
      gfnttest::put_s16(cvt_table, v);
    }
    std::vector<gfnttest::Table> tables = {
        {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, is_long ? 1 : 0)},
        {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 100, 1)},
        {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx({{500, 0}}, {})},
        {GFNT_TAG('m', 'a', 'x', 'p'), maxp},
        {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
        {GFNT_TAG('l', 'o', 'c', 'a'), loca},
    };
    if (!fpgm.empty()) tables.push_back({GFNT_TAG('f', 'p', 'g', 'm'), fpgm});
    if (!prep.empty()) tables.push_back({GFNT_TAG('p', 'r', 'e', 'p'), prep});
    if (!cvt.empty()) tables.push_back({GFNT_TAG('c', 'v', 't', ' '), cvt_table});
    bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED,
        nullptr, nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
  }
  ~Hinted() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Hinted(const Hinted &) = delete;
  Hinted & operator=(const Hinted &) = delete;
};

}  // namespace

TEST(HintVmFace, AContextIsBuiltFromAFacesTablesAndSizes) {
  // fpgm: function 0 adds one to the top of the stack.
  // prep: storage[2] = f0(5), and a control value is changed.
  Hinted font(cat({push({0}), op({FDEF}), push({1}), op({ADD, ENDF})}),
              cat({push({2, 5, 0}), op({CALL, WS}), push({1, 40}), op({WCVTP})}),
              {500, 250});
  GFNT_HintContext * ctx = nullptr;

  ASSERT_EQ(gfnt_hint_ctx_from_face(font.face, 16, 40, false, nullptr, &ctx,
      &font.error), GFNT_OK);
  ASSERT_TRUE(gfnt_hint_ctx_usable(ctx));
  EXPECT_EQ(gfnt_hint_ctx_storage(ctx, 2), 6);
  EXPECT_EQ(gfnt_hint_ctx_cvt(ctx, 0), 512);     // 500/1000 em at 16 ppem = 8 px
  EXPECT_EQ(gfnt_hint_ctx_cvt(ctx, 1), 40);      // prep wrote it
  GFNT_HintRun * run = nullptr;
  Bytes glyph = cat({push({2}), op({RS}), push({0}), op({RCVT})});
  ASSERT_EQ(gfnt_hint_run_glyph(ctx, nullptr, glyph.data(), glyph.size(), &run), GFNT_OK);
  EXPECT_EQ(gfnt_hint_run_stack(run, 0), 6);
  EXPECT_EQ(gfnt_hint_run_stack(run, 1), 512);
  gfnt_hint_run_destroy(run);
  gfnt_hint_ctx_destroy(ctx);

  // A different size scales the same font units differently.
  ASSERT_EQ(gfnt_hint_ctx_from_face(font.face, 32, 40, false, nullptr, &ctx,
      &font.error), GFNT_OK);
  EXPECT_EQ(gfnt_hint_ctx_cvt(ctx, 0), 1024);
  gfnt_hint_ctx_destroy(ctx);
}

TEST(HintVmFace, AFaceWithoutProgramsHasAnEmptyUsableContext) {
  Hinted font({}, {}, {});
  GFNT_HintContext * ctx = nullptr;

  ASSERT_EQ(gfnt_hint_ctx_from_face(font.face, 12, 35, true, nullptr, &ctx,
      &font.error), GFNT_OK);
  EXPECT_TRUE(gfnt_hint_ctx_usable(ctx));
  EXPECT_EQ(gfnt_hint_ctx_cvt(ctx, 0), 0);
  gfnt_hint_ctx_destroy(ctx);
}

TEST(HintVmFace, AMaxpWithoutTheTrueTypeSizesIsRefused) {
  Hinted font({}, {}, {}, 8, 4, 32, 0x00005000);
  GFNT_HintContext * ctx = nullptr;

  EXPECT_EQ(gfnt_hint_ctx_from_face(font.face, 16, 40, false, nullptr, &ctx,
      &font.error), GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(ctx, nullptr);
  EXPECT_EQ(gfnt_hint_ctx_from_face(nullptr, 16, 40, false, nullptr, &ctx, nullptr),
      GFNT_ERR_INVALID);
}

TEST(HintVmFace, AProgramThatFailsLeavesTheFaceUnhintedAndSaysWhy) {
  Hinted font({}, op({ADD}), {});
  GFNT_HintContext * ctx = nullptr;

  ASSERT_EQ(gfnt_hint_ctx_from_face(font.face, 16, 40, false, nullptr, &ctx,
      &font.error), GFNT_OK);
  EXPECT_FALSE(gfnt_hint_ctx_usable(ctx));
  EXPECT_EQ(gfnt_hint_ctx_fault(ctx).failure, GFNT_HINT_FAIL_STACK_UNDERFLOW);
  gfnt_hint_ctx_destroy(ctx);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
