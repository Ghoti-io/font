/**
 * @file
 *
 * The charstring interpreters, with no container anywhere near them.
 *
 * documentation/design.md section 7.4. Every program here is written byte by
 * byte, which is the point: a CFF font cannot carry a charstring that underflows
 * its stack, calls a subroutine it does not have, or ends in the middle of an
 * operand, and those arms are most of what an interpreter is. The fonts that
 * *can* be built are in `tests/data/fonts/cff-*.otf` and are compared against
 * fontTools by `tools/oracle/cff_diff.py`; this suite is the other half.
 *
 * `tests/unit/test_cff.cpp` covers the container. What is here needs none of it:
 * ::gfnt_charstring_run() takes bytes and two subroutine accessors, so a test is
 * a program and a pair of arrays.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <ghoti.io/font/charstring.h>
#include <ghoti.io/font/outline.h>

#include <gtest/gtest.h>

namespace {

/** Type 2 one-byte operators, by the name the tests spell them with. */
enum : uint8_t {
  kHstem = 1, kVstem = 3, kVmoveto = 4, kRlineto = 5, kHlineto = 6,
  kVlineto = 7, kRrcurveto = 8, kClosepath = 9, kCallsubr = 10, kReturn = 11,
  kEscape = 12, kHsbw = 13, kEndchar = 14, kHstemhm = 18, kHintmask = 19,
  kCntrmask = 20, kRmoveto = 21, kHmoveto = 22, kVstemhm = 23,
  kRcurveline = 24, kRlinecurve = 25, kVvcurveto = 26, kHhcurveto = 27,
  kShortint = 28, kCallgsubr = 29, kVhcurveto = 30, kHvcurveto = 31,
  kFixed = 255,
};

/** Two-byte operators, after the escape. */
enum : uint8_t {
  kDotsection = 0, kVstem3 = 1, kHstem3 = 2, kAnd = 3, kOr = 4, kNot = 5,
  kSeac = 6, kSbw = 7, kAbs = 9, kAdd = 10, kSub = 11, kDiv = 12, kNeg = 14,
  kEq = 15, kCallothersubr = 16, kPop = 17, kDrop = 18, kPut = 20, kGet = 21,
  kIfelse = 22, kRandom = 23, kMul = 24, kSqrt = 26, kDup = 27, kExch = 28,
  kIndex = 29, kRoll = 30, kSetcurrentpoint = 33, kHflex = 34, kFlex = 35,
  kHflex1 = 36, kFlex1 = 37,
};

/**
 * A charstring under construction.
 *
 * `num()` writes the shortest form that holds the value, which is what a real
 * font does; `wide()` and `fixed()` write the longer ones on purpose, because
 * which form an operand is written in is a property of the program and a reader
 * can be wrong about exactly one of them.
 */
struct Program {
  std::vector<uint8_t> bytes;

  Program & num(int32_t value) {
    if (value >= -107 && value <= 107) {
      bytes.push_back(static_cast<uint8_t>(value + 139));
    }
    else if (value >= 108 && value <= 1131) {
      const int32_t shifted = value - 108;
      bytes.push_back(static_cast<uint8_t>(247 + (shifted >> 8)));
      bytes.push_back(static_cast<uint8_t>(shifted & 0xFF));
    }
    else if (value <= -108 && value >= -1131) {
      const int32_t shifted = -value - 108;
      bytes.push_back(static_cast<uint8_t>(251 + (shifted >> 8)));
      bytes.push_back(static_cast<uint8_t>(shifted & 0xFF));
    }
    else {
      wide(static_cast<int16_t>(value));
    }
    return *this;
  }

  /** The 28 form: a 16-bit integer, whatever its magnitude. */
  Program & wide(int16_t value) {
    bytes.push_back(kShortint);
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<uint8_t>(value & 0xFF));
    return *this;
  }

  /** The 255 form: 16.16 in Type 2, a plain integer in Type 1. */
  Program & fixed(int32_t raw) {
    bytes.push_back(kFixed);
    for (int shift = 24; shift >= 0; shift -= 8) {
      bytes.push_back(static_cast<uint8_t>((raw >> shift) & 0xFF));
    }
    return *this;
  }

  Program & op(uint8_t which) {
    bytes.push_back(which);
    return *this;
  }

  Program & op2(uint8_t which) {
    bytes.push_back(kEscape);
    bytes.push_back(which);
    return *this;
  }

  Program & raw(uint8_t byte) {
    bytes.push_back(byte);
    return *this;
  }
};

/** An INDEX of subroutines, as an accessor the interpreter can call. */
struct Subrs {
  std::vector<std::vector<uint8_t>> items;

  static GFNT_Result at(void * user, uint32_t index, const uint8_t ** bytes,
      size_t * length) {
    Subrs * self = static_cast<Subrs *>(user);
    if (index >= self->items.size()) {
      return GFNT_ERR_CORRUPT;
    }
    *bytes = self->items[index].empty() ? nullptr : self->items[index].data();
    *length = self->items[index].size();
    return GFNT_OK;
  }

  void fill(GFNT_CharstringSubrs * out) {
    out->at = &Subrs::at;
    out->user = this;
    out->count = items.size();
  }
};

/** Charstrings a `seac` can name, by Standard Encoding code. */
struct StandardGlyphs {
  std::vector<std::pair<uint8_t, std::vector<uint8_t>>> items;

  static GFNT_Result at(void * user, uint8_t code, const uint8_t ** bytes,
      size_t * length) {
    StandardGlyphs * self = static_cast<StandardGlyphs *>(user);
    for (const auto & entry : self->items) {
      if (entry.first == code) {
        *bytes = entry.second.data();
        *length = entry.second.size();
        return GFNT_OK;
      }
    }
    return GFNT_ERR_CORRUPT;
  }
};

/** One run of one program, with its outline and what it reported. */
struct Drawn {
  GFNT_Outline * outline = nullptr;
  GFNT_CharstringMetrics metrics{};
  GFNT_Error error{};
  GFNT_Result result = GFNT_ERR_INTERNAL;

  Drawn() {
    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_outline_create(nullptr, &outline, nullptr), GFNT_OK);
  }
  ~Drawn() { gfnt_outline_destroy(outline); }
  Drawn(const Drawn &) = delete;
  Drawn & operator=(const Drawn &) = delete;

  GFNT_Result go(const Program & program, GFNT_CharstringContext * context,
      GFNT_CharstringType type = GFNT_CHARSTRING_TYPE2) {
    result = gfnt_charstring_run(type, program.bytes.data(),
        program.bytes.size(), context, outline, &metrics, &error);
    return result;
  }
};

/** A context with no subroutines and no accented characters. */
GFNT_CharstringContext bare(const GFNT_Limits * limits = nullptr) {
  GFNT_CharstringContext context{};
  context.limits = limits;
  return context;
}

/** The path as one string per segment, in whole font units. */
std::vector<std::string> path_of(const GFNT_Outline * outline) {
  std::vector<std::string> lines;
  GFNT_OutlineSink sink{};
  static auto * emit = +[](std::vector<std::string> * out, const char * verb,
      const GFNT_Point * points, size_t count) {
    std::string line = verb;
    for (size_t i = 0; i < count; ++i) {
      line += " " + std::to_string(points[i].x / GFNT_F26DOT6_ONE) + " "
          + std::to_string(points[i].y / GFNT_F26DOT6_ONE);
    }
    out->push_back(line);
  };
  sink.move_to = [](void * user, GFNT_Point to) -> GFNT_Result {
    emit(static_cast<std::vector<std::string> *>(user), "move", &to, 1);
    return GFNT_OK;
  };
  sink.line_to = [](void * user, GFNT_Point to) -> GFNT_Result {
    emit(static_cast<std::vector<std::string> *>(user), "line", &to, 1);
    return GFNT_OK;
  };
  sink.quad_to = [](void * user, GFNT_Point control,
      GFNT_Point to) -> GFNT_Result {
    GFNT_Point pair[2] = {control, to};
    emit(static_cast<std::vector<std::string> *>(user), "quad", pair, 2);
    return GFNT_OK;
  };
  sink.cubic_to = [](void * user, GFNT_Point c1, GFNT_Point c2,
      GFNT_Point to) -> GFNT_Result {
    GFNT_Point triple[3] = {c1, c2, to};
    emit(static_cast<std::vector<std::string> *>(user), "cubic", triple, 3);
    return GFNT_OK;
  };
  sink.close = [](void * user) -> GFNT_Result {
    emit(static_cast<std::vector<std::string> *>(user), "close", nullptr, 0);
    return GFNT_OK;
  };
  EXPECT_EQ(gfnt_outline_decompose(outline, &sink, &lines, nullptr), GFNT_OK);
  return lines;
}

/** How many times a dump says `reserved`. */
size_t reserved_count(const std::string & text) {
  size_t at = 0;
  size_t found = 0;
  while ((at = text.find("reserved", at)) != std::string::npos) {
    ++found;
    at += 1;
  }
  return found;
}

/** Whether a message names something, for the refusal tests. */
void names(const GFNT_Error & error, const char * fragment) {
  ASSERT_NE(error.message, nullptr);
  EXPECT_NE(std::string(error.message).find(fragment), std::string::npos)
      << error.message;
}

// ---------------------------------------------------------------- Type 2

TEST(Charstring, ALineIsDrawnInFontUnitsTimesSixtyFour) {
  Program program;
  program.num(100).num(200).op(kRmoveto).num(300).num(0).op(kRlineto)
      .op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_EQ(gfnt_outline_point_count(run.outline), 2u);
  EXPECT_EQ(gfnt_outline_contour_count(run.outline), 1u);
  GFNT_Point point{};
  ASSERT_EQ(gfnt_outline_point_at(run.outline, 0, &point, nullptr), GFNT_OK);
  EXPECT_EQ(point.x, 100 * GFNT_F26DOT6_ONE);
  EXPECT_EQ(point.y, 200 * GFNT_F26DOT6_ONE);
  EXPECT_EQ(path_of(run.outline), (std::vector<std::string>{
      "move 100 200", "line 400 200", "close"}));
}

TEST(Charstring, TheAdvanceIsADeltaFromNominalWidthX) {
  Program program;
  program.num(45).num(0).num(0).op(kRmoveto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  context.nominal_width = 500 * GFNT_F16DOT16_ONE;
  context.default_width = 999 * GFNT_F16DOT16_ONE;
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_TRUE(run.metrics.width_stated);
  EXPECT_EQ(run.metrics.width, 545 * GFNT_F16DOT16_ONE);
}

TEST(Charstring, AProgramThatStatesNoAdvanceGetsDefaultWidthX) {
  Program program;
  program.num(0).num(0).op(kRmoveto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  context.nominal_width = 500 * GFNT_F16DOT16_ONE;
  context.default_width = 999 * GFNT_F16DOT16_ONE;
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_FALSE(run.metrics.width_stated);
  EXPECT_EQ(run.metrics.width, 999 * GFNT_F16DOT16_ONE);
}

TEST(Charstring, EndcharCarriesTheAdvanceWhenItHasOneOperand) {
  // The one stack-clearing operator whose width cannot be recognised by "more
  // operands than it takes": endchar takes none, or the four of an accented
  // character, so one operand is a width and five are a width and four.
  Program program;
  program.num(20).op(kEndchar);
  GFNT_CharstringContext context = bare();
  context.nominal_width = 500 * GFNT_F16DOT16_ONE;
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_TRUE(run.metrics.width_stated);
  EXPECT_EQ(run.metrics.width, 520 * GFNT_F16DOT16_ONE);
  EXPECT_EQ(gfnt_outline_point_count(run.outline), 0u);
}

TEST(Charstring, AStemOperatorWithAnOddOperandCountCarriesTheAdvance) {
  Program program;
  program.num(30).num(0).num(50).num(100).num(50).op(kHstemhm)
      .num(0).num(0).op(kRmoveto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  context.nominal_width = 500 * GFNT_F16DOT16_ONE;
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_EQ(run.metrics.width, 530 * GFNT_F16DOT16_ONE);
  EXPECT_EQ(run.metrics.stems, 2u);
}

TEST(Charstring, AHintmaskSkipsOneByteForEveryEightStems) {
  // Ten stems, so two mask bytes. What proves the count is right is the
  // *operator after the mask*: a reader that skipped one byte would read the
  // second mask byte as an operator, and a reader that skipped three would take
  // the rmoveto's first operand for one.
  Program program;
  for (int i = 0; i < 5; ++i) {
    program.num(100).num(20);
  }
  program.op(kHstemhm);
  for (int i = 0; i < 5; ++i) {
    program.num(100).num(20);
  }
  program.op(kVstemhm).op(kHintmask).raw(0xAA).raw(0xC0)
      .num(10).num(20).op(kRmoveto).num(30).num(0).op(kRlineto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_EQ(run.metrics.stems, 10u);
  EXPECT_EQ(path_of(run.outline), (std::vector<std::string>{
      "move 10 20", "line 40 20", "close"}));
}

TEST(Charstring, OperandsBeforeAHintmaskAreAnImplicitVstem) {
  // The one place a stem count grows with no stem operator, and the mask's
  // width depends on it.
  Program program;
  program.num(100).num(20).num(200).num(20).op(kHintmask).raw(0xC0)
      .num(10).num(20).op(kRmoveto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_EQ(run.metrics.stems, 2u);
  EXPECT_EQ(gfnt_outline_point_count(run.outline), 1u);
}

TEST(Charstring, AMaskThatRunsPastTheEndIsCorrupt) {
  Program program;
  program.num(100).num(20).op(kHstemhm).op(kHintmask);
  GFNT_CharstringContext context = bare();
  Drawn run;

  EXPECT_EQ(run.go(program, &context), GFNT_ERR_CORRUPT);
  names(run.error, "mask bytes run off the end");
}

TEST(Charstring, CntrmaskCountsItsStemsToo) {
  Program program;
  program.num(100).num(20).op(kHstemhm).op(kCntrmask).raw(0x80)
      .num(0).num(0).op(kRmoveto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_EQ(run.metrics.stems, 1u);
}

TEST(Charstring, HvcurvetoPutsItsTrailingOperandOnTheLastGroupOnly) {
  // Two groups and a fifth operand. The trailing one belongs to the *last*
  // group, so it is recognised by "five operands remain" and not by the total
  // count's parity - a reader that gave it to every group of five draws the
  // first curve's endpoint off the axis it is supposed to leave on.
  Program program;
  program.num(0).num(0).op(kRmoveto)
      .num(10).num(20).num(30).num(40)
      .num(50).num(60).num(70).num(80).num(90)
      .op(kHvcurveto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_EQ(path_of(run.outline), (std::vector<std::string>{
      "move 0 0",
      // Horizontal out, vertical in: the first control point moves along x
      // only, and the endpoint along y only.
      "cubic 10 0 30 30 30 70",
      // Vertical out, horizontal in - and the ninth operand is this curve's
      // *y*, which is the whole of what the trailing operand is for.
      "cubic 30 120 90 190 170 280",
      "close"}));
}

TEST(Charstring, HhcurvetoAppliesItsLeadingOperandToTheFirstCurveOnly) {
  Program program;
  program.num(0).num(0).op(kRmoveto)
      .num(20)
      .num(30).num(40).num(50).num(60)
      .num(30).num(40).num(50).num(60)
      .op(kHhcurveto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_EQ(path_of(run.outline), (std::vector<std::string>{
      "move 0 0",
      // The 20 is the first control point's dy, once.
      "cubic 30 20 70 70 130 70",
      // And not again: this curve's first control point stays on the y it
      // arrived at.
      "cubic 160 70 200 120 260 120",
      "close"}));
}

TEST(Charstring, HflexReturnsToTheYTheFlexStartedAt) {
  Program program;
  program.num(0).num(100).op(kRmoveto)
      .num(30).num(40).num(30).num(50).num(60).num(40).num(30)
      .op2(kHflex).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  const std::vector<std::string> path = path_of(run.outline);
  ASSERT_EQ(path.size(), 4u);
  // Both endpoints, and the second curve's last control point, are back on the
  // y the flex began at. That is the whole of what the short form omits.
  EXPECT_EQ(path[2], "cubic 180 130 220 100 250 100");
}

TEST(Charstring, ASubroutineNumberIsBiasedByHowManyThereAre) {
  Subrs local;
  Program body;
  body.num(300).num(0).op(kRlineto).op(kReturn);
  local.items.push_back(body.bytes);

  Program program;
  program.num(10).num(20).op(kRmoveto).num(-107).op(kCallsubr).op(kEndchar);
  GFNT_CharstringContext context = bare();
  local.fill(&context.local);
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_EQ(path_of(run.outline), (std::vector<std::string>{
      "move 10 20", "line 310 20", "close"}));

  // And the unbiased number is not a subroutine this font has.
  Program unbiased;
  unbiased.num(10).num(20).op(kRmoveto).num(0).op(kCallsubr).op(kEndchar);
  Drawn second;
  EXPECT_EQ(second.go(unbiased, &context), GFNT_ERR_CORRUPT);
  names(second.error, "subroutine this font does not have");
}

TEST(Charstring, AGlobalSubroutineIsADifferentIndexSpace) {
  Subrs local;
  Subrs global;
  Program in_local;
  in_local.num(100).num(0).op(kRlineto).op(kReturn);
  Program in_global;
  in_global.num(0).num(100).op(kRlineto).op(kReturn);
  local.items.push_back(in_local.bytes);
  global.items.push_back(in_global.bytes);

  Program program;
  program.num(0).num(0).op(kRmoveto)
      .num(-107).op(kCallsubr).num(-107).op(kCallgsubr).op(kEndchar);
  GFNT_CharstringContext context = bare();
  local.fill(&context.local);
  global.fill(&context.global);
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_EQ(path_of(run.outline), (std::vector<std::string>{
      "move 0 0", "line 100 0", "line 100 100", "close"}));
}

TEST(Charstring, ASubroutineThatCallsItselfIsStoppedByTheDepthLimit) {
  Subrs local;
  Program recursive;
  recursive.num(-107).op(kCallsubr).op(kReturn);
  local.items.push_back(recursive.bytes);

  Program program;
  program.num(0).num(0).op(kRmoveto).num(-107).op(kCallsubr).op(kEndchar);
  GFNT_Limits limits{};
  gfnt_limits_default(&limits);
  GFNT_CharstringContext context = bare(&limits);
  local.fill(&context.local);
  Drawn run;

  EXPECT_EQ(run.go(program, &context), GFNT_ERR_LIMIT);
  names(run.error, "max_charstring_depth");
}

TEST(Charstring, TheOperatorCountIsWhatStopsAnExpandingProgram) {
  // Depth alone does not bound the work: a subroutine that calls two
  // subroutines, ten levels deep, is a thousandfold expansion from a few
  // hundred bytes. The cap on operators is what makes it terminate, and it is
  // in GFNT_Limits where a caller can see it.
  Subrs local;
  Program leaf;
  leaf.num(1).num(1).op(kRlineto).op(kReturn);
  Program pair;
  pair.num(-107).op(kCallsubr).num(-107).op(kCallsubr).op(kReturn);
  local.items.push_back(leaf.bytes);   // 0
  local.items.push_back(pair.bytes);   // 1, calls 0 twice

  Program program;
  program.num(0).num(0).op(kRmoveto);
  for (int i = 0; i < 20; ++i) {
    program.num(-106).op(kCallsubr);
  }
  program.op(kEndchar);

  GFNT_Limits limits{};
  gfnt_limits_default(&limits);
  limits.max_charstring_ops = 30;
  GFNT_CharstringContext context = bare(&limits);
  local.fill(&context.local);
  Drawn run;

  EXPECT_EQ(run.go(program, &context), GFNT_ERR_LIMIT);
  names(run.error, "max_charstring_ops");
}

TEST(Charstring, MoreOperandsThanTheStackHoldsIsCorrupt) {
  Program program;
  for (int i = 0; i < 49; ++i) {
    program.num(1);
  }
  program.op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  EXPECT_EQ(run.go(program, &context), GFNT_ERR_CORRUPT);
  names(run.error, "stack holds");
}

TEST(Charstring, AnOperandThatRunsOffTheEndIsCorrupt) {
  // Each of the three multi-byte forms, cut short. A reader that read past the
  // charstring would be reading another glyph's program.
  const std::vector<std::pair<Program, const char *>> cases = {
      {Program().raw(247), "two-byte operand"},
      {Program().raw(kShortint).raw(0x01), "16-bit operand"},
      {Program().raw(kFixed).raw(0x01).raw(0x02), "32-bit operand"},
  };
  for (const auto & item : cases) {
    GFNT_CharstringContext context = bare();
    Drawn run;
    EXPECT_EQ(run.go(item.first, &context), GFNT_ERR_CORRUPT) << item.second;
    names(run.error, item.second);
  }
}

TEST(Charstring, AnOperatorTheFormatDoesNotDefineIsCorrupt) {
  // 15, 16 and 17 are reserved in Type 2, and 13 is Type 1's hsbw.
  for (uint8_t reserved : {uint8_t{15}, uint8_t{16}, uint8_t{17}}) {
    Program program;
    program.op(reserved);
    GFNT_CharstringContext context = bare();
    Drawn run;
    EXPECT_EQ(run.go(program, &context), GFNT_ERR_CORRUPT)
        << "operator " << (int)reserved;
    names(run.error, "does not define");
  }
}

TEST(Charstring, RandomIsRefusedByNameRatherThanAnswered) {
  // design.md section 1 promises one font at one size gives one bitmap on every
  // platform for ever. An operator whose value is by definition unpredictable
  // cannot be part of that, and nothing in the corpus uses it.
  Program program;
  program.op2(kRandom);
  GFNT_CharstringContext context = bare();
  Drawn run;

  EXPECT_EQ(run.go(program, &context), GFNT_ERR_UNSUPPORTED);
  names(run.error, "deterministic");
}

TEST(Charstring, TheArithmeticOperatorsComputeTheCoordinatesTheyDrawWith) {
  Program program;
  // 600/5 = 120 across, then 2*150 = 300 up, then the transient array.
  program.num(0).num(0).op(kRmoveto)
      .num(600).num(5).op2(kDiv).num(0).op(kRlineto)
      .num(0).num(150).num(2).op2(kMul).op(kRlineto)
      .num(-60).op2(kAbs).num(5).op2(kPut).num(5).op2(kGet).num(0)
      .op(kRlineto)
      .op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_EQ(path_of(run.outline), (std::vector<std::string>{
      "move 0 0", "line 120 0", "line 120 300", "line 180 300", "close"}));
}

TEST(Charstring, DivideByZeroIsCorruptRatherThanAnInfinity) {
  Program program;
  program.num(1).num(0).op2(kDiv).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  EXPECT_EQ(run.go(program, &context), GFNT_ERR_CORRUPT);
  names(run.error, "divided by zero");
}

TEST(Charstring, TheTransientArrayIsBoundedAtBothEnds) {
  const std::vector<std::pair<Program, const char *>> cases = {
      {Program().num(1).num(32).op2(kPut), "stored outside"},
      // A negative index, which is the other end of the same bound: the value
      // is the first operand and the index the second.
      {Program().num(1).num(-1).op2(kPut), "stored outside"},
      {Program().num(32).op2(kGet), "read outside"},
      {Program().num(-1).op2(kGet), "read outside"},
  };
  for (const auto & item : cases) {
    GFNT_CharstringContext context = bare();
    Drawn run;
    EXPECT_EQ(run.go(item.first, &context), GFNT_ERR_CORRUPT) << item.second;
    names(run.error, item.second);
  }
}

TEST(Charstring, AnArithmeticOperatorWithoutItsOperandsIsCorrupt) {
  const std::vector<uint8_t> greedy = {kAdd, kSub, kMul, kDiv, kEq, kExch,
      kIfelse, kIndex, kRoll};
  for (uint8_t which : greedy) {
    Program program;
    program.num(1).op2(which);
    GFNT_CharstringContext context = bare();
    Drawn run;
    EXPECT_EQ(run.go(program, &context), GFNT_ERR_CORRUPT)
        << "two-byte operator " << (int)which;
  }
}

TEST(Charstring, IndexCannotReachBelowTheStack) {
  Program program;
  program.num(1).num(5).op2(kIndex).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  EXPECT_EQ(run.go(program, &context), GFNT_ERR_CORRUPT);
  names(run.error, "below the stack");
}

TEST(Charstring, AnAccentedCharacterNeedsTheContainerToResolveIt) {
  Program program;
  program.num(0).num(100).num(65).num(194).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  EXPECT_EQ(run.go(program, &context), GFNT_ERR_UNSUPPORTED);
  names(run.error, "Standard Encoding code");
}

TEST(Charstring, AnAccentedCharacterDrawsBothGlyphsAndTranslatesTheAccent) {
  StandardGlyphs glyphs;
  Program base;
  base.num(0).num(0).op(kRmoveto).num(100).num(0).op(kRlineto).op(kEndchar);
  Program accent;
  accent.num(0).num(0).op(kRmoveto).num(0).num(50).op(kRlineto).op(kEndchar);
  glyphs.items.push_back({65, base.bytes});
  glyphs.items.push_back({194, accent.bytes});

  Program program;
  program.num(200).num(700).num(65).num(194).op(kEndchar);
  GFNT_CharstringContext context = bare();
  context.standard_code = &StandardGlyphs::at;
  context.standard_user = &glyphs;
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_TRUE(run.metrics.seac);
  EXPECT_EQ(path_of(run.outline), (std::vector<std::string>{
      "move 0 0", "line 100 0", "close",
      // The accent, moved by the two operands and by nothing else.
      "move 200 700", "line 200 750", "close"}));
}

TEST(Charstring, AnAccentedCharacterWhoseAccentIsOneDoesNotRecurse) {
  StandardGlyphs glyphs;
  Program nested;
  nested.num(0).num(0).num(65).num(65).op(kEndchar);
  glyphs.items.push_back({65, nested.bytes});

  Program program;
  program.num(0).num(0).num(65).num(65).op(kEndchar);
  GFNT_CharstringContext context = bare();
  context.standard_code = &StandardGlyphs::at;
  context.standard_user = &glyphs;
  Drawn run;

  EXPECT_EQ(run.go(program, &context), GFNT_ERR_CORRUPT);
  names(run.error, "would not terminate");
}

TEST(Charstring, AnAccentedCharacterWhoseComponentsAreMissingIsRefused) {
  StandardGlyphs glyphs;  // Empty: no code resolves.
  Program program;
  program.num(0).num(0).num(65).num(194).op(kEndchar);
  GFNT_CharstringContext context = bare();
  context.standard_code = &StandardGlyphs::at;
  context.standard_user = &glyphs;
  Drawn run;

  EXPECT_EQ(run.go(program, &context), GFNT_ERR_CORRUPT);
  names(run.error, "base glyph");
}

TEST(Charstring, EndcharWithOperandsItCannotUseIsCorrupt) {
  Program program;
  program.num(1).num(2).num(3).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  EXPECT_EQ(run.go(program, &context), GFNT_ERR_CORRUPT);
  names(run.error, "neither a width nor the four");
}

TEST(Charstring, AProgramWithNoBytesDrawsNothingAndSaysSo) {
  Program program;
  GFNT_CharstringContext context = bare();
  context.default_width = 400 * GFNT_F16DOT16_ONE;
  Drawn run;

  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_EQ(gfnt_outline_point_count(run.outline), 0u);
  // Nothing ran, so nothing decided the width: the default stands.
  EXPECT_EQ(run.metrics.width, 400 * GFNT_F16DOT16_ONE);
  EXPECT_FALSE(run.metrics.width_stated);
}

TEST(Charstring, TheOutlineIsAppendedToRatherThanCleared) {
  // What `seac` relies on, and what a container placing two programs in one
  // outline relies on.
  Program first;
  first.num(0).num(0).op(kRmoveto).num(10).num(0).op(kRlineto).op(kEndchar);
  Program second;
  second.num(100).num(100).op(kRmoveto).num(10).num(0).op(kRlineto)
      .op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(first, &context), GFNT_OK);
  ASSERT_EQ(gfnt_charstring_run(GFNT_CHARSTRING_TYPE2, second.bytes.data(),
      second.bytes.size(), &context, run.outline, nullptr, nullptr), GFNT_OK);
  EXPECT_EQ(gfnt_outline_contour_count(run.outline), 2u);
}

TEST(Charstring, ThePointCapIsTheOutlinesAndItIsReported) {
  Program program;
  program.num(0).num(0).op(kRmoveto);
  for (int i = 0; i < 20; ++i) {
    program.num(10).num(0).op(kRlineto);
  }
  program.op(kEndchar);
  GFNT_Limits limits{};
  gfnt_limits_default(&limits);
  limits.max_outline_points = 5;
  GFNT_CharstringContext context = bare(&limits);
  Drawn run;

  EXPECT_EQ(run.go(program, &context), GFNT_ERR_LIMIT);
}

TEST(Charstring, NullArgumentsAreCallerErrors) {
  Program program;
  program.op(kEndchar);
  GFNT_CharstringContext context = bare();
  GFNT_Outline * outline = nullptr;
  ASSERT_EQ(gfnt_outline_create(nullptr, &outline, nullptr), GFNT_OK);

  EXPECT_EQ(gfnt_charstring_run(GFNT_CHARSTRING_TYPE2, nullptr, 4, &context,
      outline, nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_charstring_run(GFNT_CHARSTRING_TYPE2, program.bytes.data(),
      program.bytes.size(), nullptr, outline, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_charstring_run(GFNT_CHARSTRING_TYPE2, program.bytes.data(),
      program.bytes.size(), &context, nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_charstring_run(GFNT_CHARSTRING_TYPE_COUNT,
      program.bytes.data(), program.bytes.size(), &context, outline, nullptr,
      nullptr), GFNT_ERR_INVALID);
  gfnt_outline_destroy(outline);
}

TEST(Charstring, EveryLanguageHasAName) {
  std::set<std::string> seen;
  for (int i = 0; i < GFNT_CHARSTRING_TYPE_COUNT; ++i) {
    const char * name =
        gfnt_charstring_type_string(static_cast<GFNT_CharstringType>(i));
    ASSERT_NE(name, nullptr) << i;
    EXPECT_STRNE(name, "unknown") << i;
    EXPECT_TRUE(seen.insert(name).second) << i << " shares a name";
  }
  EXPECT_STREQ(gfnt_charstring_type_string(GFNT_CHARSTRING_TYPE_COUNT),
      "unknown");
  EXPECT_STREQ(
      gfnt_charstring_type_string(static_cast<GFNT_CharstringType>(-1)),
      "unknown");
}

// ---------------------------------------------------------------- Type 1

TEST(Charstring, Type1KeepsItsAdvanceAndSideBearingInHsbw) {
  // And the drawing starts at the side bearing rather than at the origin, which
  // is the difference a reader that shared Type 2's entry point gets wrong.
  Program program;
  program.num(50).num(600).op(kHsbw)
      .num(0).num(0).op(kRmoveto).num(100).num(0).op(kRlineto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(program, &context, GFNT_CHARSTRING_TYPE1), GFNT_OK)
      << run.error.message;
  EXPECT_EQ(run.metrics.width, 600 * GFNT_F16DOT16_ONE);
  EXPECT_TRUE(run.metrics.width_stated);
  EXPECT_EQ(run.metrics.side_bearing, 50 * GFNT_F16DOT16_ONE);
  EXPECT_EQ(path_of(run.outline), (std::vector<std::string>{
      "move 50 0", "line 150 0", "close"}));
}

TEST(Charstring, ATypeOneProgramWithoutHsbwStatesNoAdvanceAndIsRefused) {
  Program program;
  program.num(0).num(0).op(kRmoveto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  EXPECT_EQ(run.go(program, &context, GFNT_CHARSTRING_TYPE1), GFNT_ERR_CORRUPT);
  names(run.error, "never ran hsbw");
}

TEST(Charstring, TypeOnesTwoHundredFiftyFifthOperandIsAnInteger) {
  // The same five bytes mean 100 in Type 1 and 100/65536 in Type 2, which is a
  // factor of 65,536 between two readers that share one arm.
  Program program;
  program.num(0).num(600).op(kHsbw).num(0).num(0).op(kRmoveto)
      .fixed(100).num(0).op(kRlineto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn one;

  ASSERT_EQ(one.go(program, &context, GFNT_CHARSTRING_TYPE1), GFNT_OK)
      << one.error.message;
  EXPECT_EQ(path_of(one.outline), (std::vector<std::string>{
      "move 0 0", "line 100 0", "close"}));

  Program two;
  two.num(0).num(0).op(kRmoveto).fixed(100).num(0).op(kRlineto).op(kEndchar);
  Drawn second;
  ASSERT_EQ(second.go(two, &context), GFNT_OK) << second.error.message;
  // 100 in 16.16 is a hundred 65,536ths, which rounds to nothing in 26.6.
  EXPECT_EQ(path_of(second.outline), (std::vector<std::string>{
      "move 0 0", "line 0 0", "close"}));
}

TEST(Charstring, TypeOneSubroutineNumbersAreNotBiased) {
  Subrs local;
  Program body;
  body.num(100).num(0).op(kRlineto).op(kReturn);
  local.items.push_back(body.bytes);

  Program program;
  program.num(0).num(600).op(kHsbw).num(0).num(0).op(kRmoveto)
      .num(0).op(kCallsubr).op(kEndchar);
  GFNT_CharstringContext context = bare();
  local.fill(&context.local);
  Drawn run;

  ASSERT_EQ(run.go(program, &context, GFNT_CHARSTRING_TYPE1), GFNT_OK)
      << run.error.message;
  EXPECT_EQ(path_of(run.outline), (std::vector<std::string>{
      "move 0 0", "line 100 0", "close"}));
}

TEST(Charstring, TypeOneClosepathEndsTheContourWithoutASegment) {
  Program program;
  program.num(0).num(600).op(kHsbw)
      .num(0).num(0).op(kRmoveto).num(100).num(0).op(kRlineto).op(kClosepath)
      .num(0).num(200).op(kRmoveto).num(100).num(0).op(kRlineto).op(kClosepath)
      .op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(program, &context, GFNT_CHARSTRING_TYPE1), GFNT_OK)
      << run.error.message;
  EXPECT_EQ(gfnt_outline_contour_count(run.outline), 2u);
}

TEST(Charstring, TypeOneSbwCarriesBothAxesAndSetcurrentpointIsAbsolute) {
  Program program;
  program.num(50).num(20).num(600).num(0).op2(kSbw)
      .num(100).num(0).op(kRlineto)
      .num(300).num(400).op2(kSetcurrentpoint)
      .num(0).num(100).op(kRlineto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(program, &context, GFNT_CHARSTRING_TYPE1), GFNT_OK)
      << run.error.message;
  EXPECT_EQ(run.metrics.width, 600 * GFNT_F16DOT16_ONE);
  EXPECT_EQ(run.metrics.side_bearing, 50 * GFNT_F16DOT16_ONE);
  EXPECT_EQ(path_of(run.outline), (std::vector<std::string>{
      // The first segment leaves from (sbx, sby), which sbw set.
      "move 50 20", "line 150 20",
      // setcurrentpoint is absolute, and the only operator here that is.
      "line 300 500", "close"}));
}

TEST(Charstring, TypeOneFlexCollectsSevenPointsAndDrawsTwoCurves) {
  Program program;
  program.num(0).num(600).op(kHsbw).num(100).num(100).op(kRmoveto)
      .num(0).num(1).op2(kCallothersubr);
  const int dx[7] = {50, 50, 50, 50, 50, 50, 50};
  const int dy[7] = {50, 0, -50, 0, 50, 0, -50};
  for (int i = 0; i < 7; ++i) {
    program.num(dx[i]).num(dy[i]).op(kRmoveto).num(0).num(2)
        .op2(kCallothersubr);
  }
  program.num(50).num(450).num(100).num(3).num(0).op2(kCallothersubr)
      .op2(kPop).op2(kPop).op2(kSetcurrentpoint).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(program, &context, GFNT_CHARSTRING_TYPE1), GFNT_OK)
      << run.error.message;
  // Two cubics out of the six points that are on the curve; the first collected
  // point is the reference point and is not one of them.
  const std::vector<std::string> path = path_of(run.outline);
  ASSERT_EQ(path.size(), 4u) << path[0];
  EXPECT_EQ(path[0], "move 100 100");
  EXPECT_EQ(path[1], "cubic 200 150 250 100 300 100");
  EXPECT_EQ(path[2], "cubic 350 150 400 150 450 100");
}

TEST(Charstring, AFlexThatEndsEarlyIsCorrupt) {
  Program program;
  program.num(0).num(600).op(kHsbw).num(0).num(0).op(kRmoveto)
      .num(0).num(1).op2(kCallothersubr)
      .num(10).num(10).op(kRmoveto).num(0).num(2).op2(kCallothersubr)
      .num(0).num(0).num(0).num(3).num(0).op2(kCallothersubr).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  EXPECT_EQ(run.go(program, &context, GFNT_CHARSTRING_TYPE1), GFNT_ERR_CORRUPT);
  names(run.error, "seven points");
}

TEST(Charstring, AFlexCollectingMoreThanSevenPointsIsCorrupt) {
  Program program;
  program.num(0).num(600).op(kHsbw).num(0).num(0).op(kRmoveto)
      .num(0).num(1).op2(kCallothersubr);
  for (int i = 0; i < 8; ++i) {
    program.num(10).num(10).op(kRmoveto).num(0).num(2).op2(kCallothersubr);
  }
  program.op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  EXPECT_EQ(run.go(program, &context, GFNT_CHARSTRING_TYPE1), GFNT_ERR_CORRUPT);
  names(run.error, "more than seven points");
}

TEST(Charstring, HintReplacementHandsTheSubroutineNumberBackToPop) {
  Subrs local;
  Program body;
  body.num(100).num(0).op(kRlineto).op(kReturn);
  local.items.push_back(body.bytes);

  Program program;
  program.num(0).num(600).op(kHsbw).num(0).num(0).op(kRmoveto)
      // subr# 1 3 callothersubr pop callsubr: the number goes in and comes back.
      .num(0).num(1).num(3).op2(kCallothersubr).op2(kPop).op(kCallsubr)
      .op(kEndchar);
  GFNT_CharstringContext context = bare();
  local.fill(&context.local);
  Drawn run;

  ASSERT_EQ(run.go(program, &context, GFNT_CHARSTRING_TYPE1), GFNT_OK)
      << run.error.message;
  EXPECT_EQ(path_of(run.outline), (std::vector<std::string>{
      "move 0 0", "line 100 0", "close"}));
}

TEST(Charstring, AnUnknownOthersubrLeavesItsArgumentsForPop) {
  // The multiple-master family is the usual one. Its arguments become its
  // results, which is what lets a font using one still draw.
  Program program;
  program.num(0).num(600).op(kHsbw).num(0).num(0).op(kRmoveto)
      .num(70).num(1).num(14).op2(kCallothersubr).op2(kPop).num(0)
      .op(kRlineto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(program, &context, GFNT_CHARSTRING_TYPE1), GFNT_OK)
      << run.error.message;
  EXPECT_EQ(path_of(run.outline), (std::vector<std::string>{
      "move 0 0", "line 70 0", "close"}));
}

TEST(Charstring, APopWithNothingBehindItPushesZeroRatherThanRefusing) {
  // What the reference interpreters do. Refusing here would refuse fonts whose
  // unknown OtherSubrs this library ran without knowing how many results they
  // meant to leave.
  Program program;
  program.num(0).num(600).op(kHsbw).num(0).num(0).op(kRmoveto)
      .op2(kPop).num(100).op(kRlineto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn run;

  ASSERT_EQ(run.go(program, &context, GFNT_CHARSTRING_TYPE1), GFNT_OK)
      << run.error.message;
  EXPECT_EQ(path_of(run.outline), (std::vector<std::string>{
      "move 0 0", "line 0 100", "close"}));
}

TEST(Charstring, TypeOneSeacCorrectsTheAccentByASideBearing) {
  StandardGlyphs glyphs;
  Program base;
  base.num(50).num(600).op(kHsbw).num(0).num(0).op(kRmoveto).num(100).num(0)
      .op(kRlineto).op(kEndchar);
  Program accent;
  accent.num(20).num(600).op(kHsbw).num(0).num(0).op(kRmoveto).num(0).num(50)
      .op(kRlineto).op(kEndchar);
  glyphs.items.push_back({65, base.bytes});
  glyphs.items.push_back({194, accent.bytes});

  // asb 20, adx 200: the accent moves by sbx - asb + adx = 50 - 20 + 200.
  Program program;
  program.num(50).num(600).op(kHsbw)
      .num(20).num(200).num(700).num(65).num(194).op2(kSeac);
  GFNT_CharstringContext context = bare();
  context.standard_code = &StandardGlyphs::at;
  context.standard_user = &glyphs;
  Drawn run;

  ASSERT_EQ(run.go(program, &context, GFNT_CHARSTRING_TYPE1), GFNT_OK)
      << run.error.message;
  EXPECT_TRUE(run.metrics.seac);
  const std::vector<std::string> path = path_of(run.outline);
  ASSERT_EQ(path.size(), 6u);
  // The accent's own charstring starts at its own side bearing, 20, and the
  // correction puts it at 20 + 230.
  EXPECT_EQ(path[3], "move 250 700");
}

/** Every byte one language defines and the other reserves. */
const std::vector<uint8_t> kTypeOneOnlySingle = {kClosepath, kHsbw};
const std::vector<uint8_t> kTypeOneOnlyEscape = {kDotsection, kVstem3, kHstem3,
    kSeac, kSbw, kCallothersubr, kPop, kSetcurrentpoint};
const std::vector<uint8_t> kTypeTwoOnlySingle = {kHstemhm, kHintmask,
    kCntrmask, kVstemhm, kRcurveline, kRlinecurve, kVvcurveto, kHhcurveto,
    kCallgsubr};
/** Type 2's two-byte operators less `div`, the one both languages have. */
const std::vector<uint8_t> kTypeTwoOnlyEscape = {kAnd, kOr, kNot, kAbs, kAdd,
    kSub, kNeg, kEq, kDrop, kPut, kGet, kIfelse, kRandom, kMul, kSqrt, kDup,
    kExch, kIndex, kRoll, kHflex, kFlex, kHflex1, kFlex1};

TEST(Charstring, EachLanguageRefusesTheOthersOperators) {
  // Not a dialect: a container that guessed wrong should get a refusal from the
  // first byte it cannot read, not a plausible wrong shape.
  //
  // Every such byte is here rather than a sample of them. The sample this
  // replaced named ten and left out hstemhm and vstemhm, which reach the
  // interpreter through the case they share with the hstem and vstem Type 1
  // does have - so a Type 1 charstring could declare stems with a byte that
  // language reserves, and was drawn instead of refused.
  //
  // The Type 1 programs open with a real hsbw, and the offset is asserted. A
  // Type 1 run with no hsbw is refused at the end of the run for that, which
  // would pass a test that only looked at the result code and would prove
  // nothing at all about the operator in the middle.
  struct Case {
    Program program;
    GFNT_CharstringType type;
    size_t at;
    uint8_t op;
  };
  std::vector<Case> cases;
  for (uint8_t which : kTypeOneOnlySingle) {
    cases.push_back({Program().num(0).num(0).op(which),
        GFNT_CHARSTRING_TYPE2, 2, which});
  }
  for (uint8_t which : kTypeOneOnlyEscape) {
    cases.push_back({Program().num(0).num(0).op2(which),
        GFNT_CHARSTRING_TYPE2, 2, which});
  }
  for (uint8_t which : kTypeTwoOnlySingle) {
    cases.push_back({Program().num(0).num(500).op(kHsbw).num(0).num(0)
        .op(which), GFNT_CHARSTRING_TYPE1, 6, which});
  }
  for (uint8_t which : kTypeTwoOnlyEscape) {
    cases.push_back({Program().num(0).num(500).op(kHsbw).num(0).num(0)
        .op2(which), GFNT_CHARSTRING_TYPE1, 6, which});
  }
  ASSERT_EQ(cases.size(), kTypeOneOnlySingle.size() + kTypeOneOnlyEscape.size()
      + kTypeTwoOnlySingle.size() + kTypeTwoOnlyEscape.size());

  for (const Case & item : cases) {
    GFNT_CharstringContext context = bare();
    Drawn run;
    EXPECT_EQ(run.go(item.program, &context, item.type), GFNT_ERR_CORRUPT)
        << "operator " << static_cast<int>(item.op) << " was accepted by "
        << gfnt_charstring_type_string(item.type);
    // EXPECT and not ASSERT: an ASSERT here returns from the test, so the
    // first operator that was wrongly accepted would be the only one reported
    // and the list would stop being the exhaustive thing it is for.
    if (run.error.message == nullptr) {
      continue;
    }
    EXPECT_EQ(run.error.offset, item.at)
        << "operator " << static_cast<int>(item.op) << " was refused somewhere "
        << "other than at its own byte: " << run.error.message;
  }
}

TEST(Charstring, EveryArithmeticOperatorIsExercised) {
  // One glyph per operator would be sixteen fixtures; what matters is that each
  // one's arithmetic is the specification's, so each is checked by drawing a
  // line whose length only the right answer produces.
  struct Case {
    Program program;
    const char * expected;
  };
  const std::vector<Case> cases = {
      // 300 - 100 = 200 across.
      {Program().num(0).num(0).op(kRmoveto).num(300).num(100).op2(kSub).num(0)
           .op(kRlineto).op(kEndchar), "line 200 0"},
      // -(-200) = 200.
      {Program().num(0).num(0).op(kRmoveto).num(-200).op2(kNeg).num(0)
           .op(kRlineto).op(kEndchar), "line 200 0"},
      // |-200| = 200.
      {Program().num(0).num(0).op(kRmoveto).num(-200).op2(kAbs).num(0)
           .op(kRlineto).op(kEndchar), "line 200 0"},
      // sqrt(40000) = 200, on integers.
      {Program().num(0).num(0).op(kRmoveto).num(200).num(200).op2(kMul)
           .op2(kSqrt).num(0).op(kRlineto).op(kEndchar), "line 200 0"},
      // 1 and 1 is true, which is one unit in 16.16; 200 times that is 200.
      {Program().num(0).num(0).op(kRmoveto).num(1).num(1).op2(kAnd).num(200)
           .op2(kMul).num(0).op(kRlineto).op(kEndchar), "line 200 0"},
      // 0 or 1 is true as well.
      {Program().num(0).num(0).op(kRmoveto).num(0).num(1).op2(kOr).num(200)
           .op2(kMul).num(0).op(kRlineto).op(kEndchar), "line 200 0"},
      // not 0 is true.
      {Program().num(0).num(0).op(kRmoveto).num(0).op2(kNot).num(200)
           .op2(kMul).num(0).op(kRlineto).op(kEndchar), "line 200 0"},
      // 3 2 1 rolled by one puts the 200 where rlineto's dx goes.
      {Program().num(0).num(0).op(kRmoveto).num(0).num(200).num(2).num(1)
           .op2(kRoll).op(kRlineto).op(kEndchar), "line 200 0"},
      // A negative roll turns the other way and is the same cycle of two.
      {Program().num(0).num(0).op(kRmoveto).num(0).num(200).num(2).num(-1)
           .op2(kRoll).op(kRlineto).op(kEndchar), "line 200 0"},
      // ifelse picks the second when the comparison is false.
      {Program().num(0).num(0).op(kRmoveto).num(500).num(200).num(3).num(2)
           .op2(kIfelse).num(0).op(kRlineto).op(kEndchar), "line 200 0"},
  };
  for (size_t at = 0; at < cases.size(); ++at) {
    GFNT_CharstringContext context = bare();
    Drawn drawn;
    ASSERT_EQ(drawn.go(cases[at].program, &context), GFNT_OK)
        << "case " << at << ": " << drawn.error.message;
    const std::vector<std::string> path = path_of(drawn.outline);
    ASSERT_EQ(path.size(), 3u) << "case " << at;
    EXPECT_EQ(path[1], cases[at].expected) << "case " << at;
  }
}

TEST(Charstring, SqrtOfZeroAndOfANegativeIsZero) {
  // Not a number this can answer, and the answer is the one that keeps the
  // program running: the alternative is an interpreter that refuses a glyph
  // over an operand nothing draws with.
  for (int operand : {0, -400}) {
    Program program;
    program.num(0).num(0).op(kRmoveto).num(operand).op2(kSqrt).num(100)
        .op(kRlineto).op(kEndchar);
    GFNT_CharstringContext context = bare();
    Drawn drawn;
    ASSERT_EQ(drawn.go(program, &context), GFNT_OK) << drawn.error.message;
    EXPECT_EQ(path_of(drawn.outline), (std::vector<std::string>{
        "move 0 0", "line 0 100", "close"})) << operand;
  }
}

TEST(Charstring, RrcurvetoDrawsEveryTripleItIsGiven) {
  // The plainest curve operator, and the one the short forms are shorthand for.
  Program program;
  program.num(0).num(0).op(kRmoveto)
      .num(10).num(20).num(30).num(40).num(50).num(60)
      .num(10).num(-20).num(30).num(-40).num(50).num(-60)
      .op(kRrcurveto).op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn drawn;

  ASSERT_EQ(drawn.go(program, &context), GFNT_OK) << drawn.error.message;
  EXPECT_EQ(path_of(drawn.outline), (std::vector<std::string>{
      "move 0 0",
      "cubic 10 20 40 60 90 120",
      "cubic 100 100 130 60 180 0",
      "close"}));
}

TEST(Charstring, TheSubroutineBiasHasThreeStepsAndTheCountPicksOne) {
  // 107 below 1240 subroutines, 1131 below 33900, 32768 above. A font with that
  // many subroutines is a CJK font; what is tested here is the arithmetic, so
  // the accessor reports the count and refuses every index - which makes the
  // *number* the interpreter asked for the thing under test.
  struct Case {
    size_t count;
    int32_t number;   ///< What the program pushes.
  };
  // Each pushes the number that, correctly biased, asks for subroutine 0 - and
  // subroutine 0 is refused, so a reader using the wrong bias asks for a
  // different index and gets the same refusal. What separates them is the
  // message's *index*, so this checks the refusal and the fixture below checks
  // the arithmetic where it can be seen.
  const std::vector<Case> cases = {{1300, -1131}, {40000, -32768}};
  for (const Case & item : cases) {
    Subrs empty;
    Program program;
    program.num(0).num(0).op(kRmoveto).num(item.number).op(kCallsubr)
        .op(kEndchar);
    GFNT_CharstringContext context = bare();
    empty.fill(&context.local);
    // The count is what the bias comes from, and it is a claim the container
    // makes rather than something the accessor proves.
    context.local.count = item.count;
    Drawn drawn;
    EXPECT_EQ(drawn.go(program, &context), GFNT_ERR_CORRUPT)
        << "count " << item.count;
    // The biased number is *inside* the count the container claimed, so the
    // interpreter asks for it and the accessor is what refuses - which is the
    // arm a font with a lying subroutine count reaches.
    names(drawn.error, "the container refused to produce");
  }

  // And with the subroutine there, the biased number reaches it.
  Subrs local;
  Program body;
  body.num(100).num(0).op(kRlineto).op(kReturn);
  local.items.push_back(body.bytes);
  Program program;
  program.num(0).num(0).op(kRmoveto).num(-107).op(kCallsubr).op(kEndchar);
  GFNT_CharstringContext context = bare();
  local.fill(&context.local);
  Drawn drawn;
  ASSERT_EQ(drawn.go(program, &context), GFNT_OK) << drawn.error.message;
  EXPECT_EQ(gfnt_outline_point_count(drawn.outline), 2u);
}

TEST(Charstring, AnAllocationFailureAnywhereInARunIsReported) {
  // Every point a curve adds is an allocation that can refuse, and the arms
  // that carry the refusal out of the interpreter are unreachable from a
  // working allocator. The sweep is by request number, so each one stops the
  // run at a different operator.
  Program program;
  program.num(0).num(0).op(kRmoveto);
  for (int i = 0; i < 6; ++i) {
    program.num(10).num(20).num(30).num(40).num(50).num(60).op(kRrcurveto)
        .num(10).num(10).op(kRlineto).num(20).num(30).op(kRmoveto);
  }
  program.op(kEndchar);

  gfnttest::FailingAllocator counter((size_t)-1);
  GFNT_Outline * measured = nullptr;
  ASSERT_EQ(gfnt_outline_create(counter.get(), &measured, nullptr), GFNT_OK);
  GFNT_CharstringContext context = bare();
  ASSERT_EQ(gfnt_charstring_run(GFNT_CHARSTRING_TYPE2, program.bytes.data(),
      program.bytes.size(), &context, measured, nullptr, nullptr), GFNT_OK);
  gfnt_outline_destroy(measured);
  const size_t requests = counter.requests();
  ASSERT_GT(requests, 2u);

  size_t refusals = 0;
  for (size_t at = 0; at < requests; ++at) {
    gfnttest::FailingAllocator allocator(at);
    GFNT_Outline * outline = nullptr;
    if (gfnt_outline_create(allocator.get(), &outline, nullptr) != GFNT_OK) {
      allocator.stop_failing();
      continue;
    }
    GFNT_Error error{};
    const GFNT_Result result = gfnt_charstring_run(GFNT_CHARSTRING_TYPE2,
        program.bytes.data(), program.bytes.size(), &context, outline, nullptr,
        &error);
    if (result != GFNT_OK) {
      EXPECT_EQ(result, GFNT_ERR_OOM) << "refusing request " << at;
      ++refusals;
    }
    allocator.stop_failing();
    gfnt_outline_destroy(outline);
    EXPECT_EQ(allocator.live(), 0u) << "refusing request " << at;
  }
  // The denominator: a sweep where nothing was refused measured nothing.
  EXPECT_GT(refusals, 1u);
}

TEST(Charstring, AnAccentedCharacterCarriesTheAllocationFailureOut) {
  // `seac` builds a second outline of its own, so it has an allocation path the
  // ordinary run does not.
  StandardGlyphs glyphs;
  Program base;
  base.num(0).num(0).op(kRmoveto).num(100).num(0).op(kRlineto).op(kEndchar);
  glyphs.items.push_back({65, base.bytes});
  glyphs.items.push_back({194, base.bytes});

  Program program;
  program.num(200).num(700).num(65).num(194).op(kEndchar);
  GFNT_CharstringContext context = bare();
  context.standard_code = &StandardGlyphs::at;
  context.standard_user = &glyphs;

  size_t refusals = 0;
  for (size_t at = 0; at < 12; ++at) {
    gfnttest::FailingAllocator allocator(at);
    GFNT_Outline * outline = nullptr;
    if (gfnt_outline_create(allocator.get(), &outline, nullptr) != GFNT_OK) {
      allocator.stop_failing();
      continue;
    }
    const GFNT_Result result = gfnt_charstring_run(GFNT_CHARSTRING_TYPE2,
        program.bytes.data(), program.bytes.size(), &context, outline, nullptr,
        nullptr);
    if (result != GFNT_OK) {
      EXPECT_EQ(result, GFNT_ERR_OOM) << "refusing request " << at;
      ++refusals;
    }
    allocator.stop_failing();
    gfnt_outline_destroy(outline);
    EXPECT_EQ(allocator.live(), 0u) << "refusing request " << at;
  }
  EXPECT_GT(refusals, 1u);
}

TEST(Charstring, EveryOperatorRefusesAnOperandListItCannotUse) {
  // One table rather than thirty tests, because what is being checked is the
  // same thing thirty times: an operator given fewer operands than it takes is a
  // corrupt charstring, and the arm that says so is unreachable from a font -
  // no writer emits one. The expected result is the same for all of them; what
  // differs is which line reports it.
  struct Case {
    Program program;
    GFNT_CharstringType type;
    const char * what;
  };
  const GFNT_CharstringType two = GFNT_CHARSTRING_TYPE2;
  const GFNT_CharstringType one = GFNT_CHARSTRING_TYPE1;
  const std::vector<Case> cases = {
      {Program().op(kRmoveto), two, "rmoveto with nothing"},
      {Program().num(1).op(kRmoveto), two, "rmoveto with one operand"},
      {Program().op(kHmoveto), two, "hmoveto with nothing"},
      {Program().op(kVmoveto), two, "vmoveto with nothing"},
      {Program().num(1).num(2).op(kRcurveline), two, "rcurveline too short"},
      {Program().num(1).num(2).op(kRlinecurve), two, "rlinecurve too short"},
      {Program().num(1).op2(kFlex), two, "flex too short"},
      {Program().num(1).op2(kHflex), two, "hflex too short"},
      {Program().num(1).op2(kHflex1), two, "hflex1 too short"},
      {Program().num(1).op2(kFlex1), two, "flex1 too short"},
      {Program().op(kCallsubr), two, "callsubr with no number"},
      {Program().op(kCallgsubr), two, "callgsubr with no number"},
      // Type 1's own, and the Type 2 operators it does not have.
      {Program().num(1).op(kHsbw), one, "hsbw with one operand"},
      {Program().num(1).op2(kSbw), one, "sbw too short"},
      {Program().num(1).op2(kSeac), one, "seac too short"},
      {Program().num(1).op2(kSetcurrentpoint), one, "setcurrentpoint too short"},
      {Program().num(1).op2(kCallothersubr), one, "callothersubr too short"},
      // callothersubr claiming more arguments than the stack holds.
      {Program().num(9).num(0).op2(kCallothersubr), one, "callothersubr lying"},
      {Program().num(0).num(0).op(kVvcurveto), one, "vvcurveto in Type 1"},
      {Program().num(0).num(0).op(kHhcurveto), one, "hhcurveto in Type 1"},
      {Program().num(0).num(0).op(kRlinecurve), one, "rlinecurve in Type 1"},
      {Program().num(0).num(0).op(kCntrmask), one, "cntrmask in Type 1"},
      {Program().num(0).num(0).op2(kHflex), one, "hflex in Type 1"},
      {Program().num(0).num(0).op2(kHflex1), one, "hflex1 in Type 1"},
      {Program().num(0).num(0).op2(kFlex1), one, "flex1 in Type 1"},
      {Program().num(0).num(0).op2(kDotsection), two, "dotsection in Type 2"},
      {Program().num(0).num(0).op2(kVstem3), two, "vstem3 in Type 2"},
      {Program().num(0).num(0).op2(kSbw), two, "sbw in Type 2"},
      {Program().num(0).num(0).op2(kPop), two, "pop in Type 2"},
      {Program().num(0).num(0).op2(kSetcurrentpoint), two,
          "setcurrentpoint in Type 2"},
      {Program().num(0).num(0).op(kClosepath), two, "closepath in Type 2"},
      // A two-byte operator whose second byte is not there.
      {Program().op(kEscape), two, "an escape at the end"},
      // A two-byte operator neither format defines.
      {Program().num(1).op2(99), two, "an undefined two-byte operator"},
      {Program().num(1).op2(99), one, "an undefined two-byte operator, Type 1"},
  };
  for (const Case & item : cases) {
    GFNT_CharstringContext context = bare();
    Drawn drawn;
    EXPECT_EQ(drawn.go(item.program, &context, item.type), GFNT_ERR_CORRUPT)
        << item.what;
    EXPECT_NE(drawn.error.message, nullptr) << item.what;
  }
}

TEST(Charstring, ATypeOneProgramThatDrawsBeforeItMovesOpensAContourWhereItIs) {
  // The one place a charstring may draw with no current contour, and the arm
  // that opens one. A curve reaches it as well as a line, and both have to
  // begin where the path *is* rather than where the segment ends.
  Program lines;
  lines.num(50).num(600).op(kHsbw).num(100).num(0).op(kRlineto).op(kEndchar);
  Program curves;
  curves.num(50).num(600).op(kHsbw)
      .num(10).num(20).num(30).num(40).num(50).num(60).op(kRrcurveto)
      .op(kEndchar);
  GFNT_CharstringContext context = bare();

  Drawn first;
  ASSERT_EQ(first.go(lines, &context, GFNT_CHARSTRING_TYPE1), GFNT_OK)
      << first.error.message;
  EXPECT_EQ(path_of(first.outline), (std::vector<std::string>{
      "move 50 0", "line 150 0", "close"}));

  Drawn second;
  ASSERT_EQ(second.go(curves, &context, GFNT_CHARSTRING_TYPE1), GFNT_OK)
      << second.error.message;
  EXPECT_EQ(path_of(second.outline), (std::vector<std::string>{
      "move 50 0", "cubic 60 20 90 60 140 120", "close"}));
}

TEST(Charstring, AHintReplacementWithNoArgumentsStillLeavesAResult) {
  // `0 3 callothersubr` - no arguments at all, which a font should not write and
  // which the reference interpreters answer rather than refuse. What `pop` reads
  // back is then the subroutine number 3 itself.
  Program program;
  program.num(0).num(600).op(kHsbw).num(0).num(0).op(kRmoveto)
      .num(0).num(3).op2(kCallothersubr).op2(kPop).num(0).op(kRlineto)
      .op(kEndchar);
  GFNT_CharstringContext context = bare();
  Drawn drawn;

  ASSERT_EQ(drawn.go(program, &context, GFNT_CHARSTRING_TYPE1), GFNT_OK)
      << drawn.error.message;
  EXPECT_EQ(path_of(drawn.outline), (std::vector<std::string>{
      "move 0 0", "line 3 0", "close"}));
}

TEST(Charstring, AnAccentedCharacterNamingACodeOutsideTheByteRangeIsCorrupt) {
  StandardGlyphs glyphs;
  Program base;
  base.num(0).num(0).op(kRmoveto).num(10).num(0).op(kRlineto).op(kEndchar);
  glyphs.items.push_back({65, base.bytes});

  Program program;
  program.num(0).num(0).num(65).wide(300).op(kEndchar);
  GFNT_CharstringContext context = bare();
  context.standard_code = &StandardGlyphs::at;
  context.standard_user = &glyphs;
  Drawn drawn;

  EXPECT_EQ(drawn.go(program, &context), GFNT_ERR_CORRUPT);
  names(drawn.error, "outside 0-255");
}

TEST(Charstring, AnAccentWhoseCharstringIsMissingIsRefusedSeparately) {
  // The base resolves and the accent does not, which is a different line from
  // the base failing: a font can have one and not the other.
  StandardGlyphs glyphs;
  Program base;
  base.num(0).num(0).op(kRmoveto).num(10).num(0).op(kRlineto).op(kEndchar);
  glyphs.items.push_back({65, base.bytes});

  Program program;
  program.num(0).num(0).num(65).num(194).op(kEndchar);
  GFNT_CharstringContext context = bare();
  context.standard_code = &StandardGlyphs::at;
  context.standard_user = &glyphs;
  Drawn drawn;

  EXPECT_EQ(drawn.go(program, &context), GFNT_ERR_CORRUPT);
  names(drawn.error, "whose accent");
}

// ---------------------------------------------------------------- the dump

TEST(CharstringDump, EveryOperatorIsNamedWithItsOperands) {
  Program program;
  program.num(600).num(50).num(0).op(kRmoveto).num(500).op(kHlineto)
      .op(kEndchar);
  gfnttest::CapturedOutput out;

  ASSERT_EQ(gfnt_charstring_dump(GFNT_CHARSTRING_TYPE2, program.bytes.data(),
      program.bytes.size(), nullptr, out.get()), GFNT_OK);
  const std::string text = out.finish();
  EXPECT_NE(text.find("charstring Type 2"), std::string::npos) << text;
  EXPECT_NE(text.find("rmoveto 600 50 0"), std::string::npos) << text;
  EXPECT_NE(text.find("hlineto 500"), std::string::npos) << text;
  EXPECT_NE(text.find("endchar"), std::string::npos) << text;
}

TEST(CharstringDump, AFractionalOperandIsPrintedAsItsRawSixteenSixteen) {
  Program program;
  program.fixed(100 * GFNT_F16DOT16_ONE + 32768).op(kEndchar);
  gfnttest::CapturedOutput out;

  ASSERT_EQ(gfnt_charstring_dump(GFNT_CHARSTRING_TYPE2, program.bytes.data(),
      program.bytes.size(), nullptr, out.get()), GFNT_OK);
  // A decimal rendering would be a second rounding rule in a file whose whole
  // purpose is to show exactly what the font said.
  EXPECT_NE(out.finish().find("100+32768/65536"), std::string::npos);
}

TEST(CharstringDump, AHintmaskAfterASubroutineNeedsARunsStemCount) {
  Subrs local;
  Program declares;
  declares.num(100).num(20).num(200).num(20).op(kHstemhm).op(kReturn);
  local.items.push_back(declares.bytes);

  Program program;
  program.num(-107).op(kCallsubr).op(kHintmask).raw(0xC0)
      .num(0).num(0).op(kRmoveto).op(kEndchar);

  // Without the metrics, the dump stops and says why: the mask's width is
  // decided by stems a subroutine declared, and a walk that does not run
  // subroutines cannot know them.
  gfnttest::CapturedOutput blind;
  ASSERT_EQ(gfnt_charstring_dump(GFNT_CHARSTRING_TYPE2, program.bytes.data(),
      program.bytes.size(), nullptr, blind.get()), GFNT_OK);
  const std::string without = blind.finish();
  EXPECT_NE(without.find("mask width unknown"), std::string::npos) << without;
  EXPECT_EQ(without.find("rmoveto"), std::string::npos) << without;

  // With them, every operator after the mask is there.
  GFNT_CharstringContext context = bare();
  local.fill(&context.local);
  Drawn run;
  ASSERT_EQ(run.go(program, &context), GFNT_OK) << run.error.message;
  EXPECT_EQ(run.metrics.stems, 2u);

  gfnttest::CapturedOutput seeing;
  ASSERT_EQ(gfnt_charstring_dump(GFNT_CHARSTRING_TYPE2, program.bytes.data(),
      program.bytes.size(), &run.metrics, seeing.get()), GFNT_OK);
  const std::string with = seeing.finish();
  EXPECT_NE(with.find("hintmask mask C0"), std::string::npos) << with;
  EXPECT_NE(with.find("rmoveto 0 0"), std::string::npos) << with;
}

TEST(CharstringDump, ATruncatedProgramSaysWhatItHasLeft) {
  Program program;
  program.num(1).num(2);
  gfnttest::CapturedOutput out;

  ASSERT_EQ(gfnt_charstring_dump(GFNT_CHARSTRING_TYPE2, program.bytes.data(),
      program.bytes.size(), nullptr, out.get()), GFNT_OK);
  EXPECT_NE(out.finish().find("trailing-operands 2"), std::string::npos);
}

TEST(CharstringDump, AMaskRunningPastTheEndIsReportedAndRefused) {
  Program program;
  program.num(100).num(20).op(kHstemhm).op(kHintmask);
  gfnttest::CapturedOutput out;

  EXPECT_EQ(gfnt_charstring_dump(GFNT_CHARSTRING_TYPE2, program.bytes.data(),
      program.bytes.size(), nullptr, out.get()), GFNT_ERR_CORRUPT);
  EXPECT_NE(out.finish().find("mask runs past the end"), std::string::npos);
}

TEST(CharstringDump, EveryWriteIsCheckedForFailure) {
  // The refusal arms a working stream cannot reach. Each failure count stops
  // the dump at a different fprintf, and every one of them must be reported
  // rather than swallowed.
  Program program;
  program.num(600).num(0).num(0).op(kRmoveto).num(100).num(20).op(kHstemhm)
      .op(kHintmask).raw(0xC0).num(10).op(kHlineto).op(kEndchar)
      // A mask whose bytes are written one fprintf each, and two trailing
      // operands, so that every write in the function is on the path.
      .num(100).num(20).op(kHstemhm).op(kHintmask).raw(0x80).num(1).num(2);
  size_t reported = 0;
  for (size_t writes = 0; writes < 40; ++writes) {
    gfnttest::FailingSink sink(writes);
    const GFNT_Result result = gfnt_charstring_dump(GFNT_CHARSTRING_TYPE2,
        program.bytes.data(), program.bytes.size(), nullptr, sink.get());
    if (result == GFNT_ERR_IO) {
      ++reported;
    }
    else {
      EXPECT_EQ(result, GFNT_OK) << "after " << writes << " writes";
    }
  }
  EXPECT_GT(reported, 5u) << "a failing stream should be reported, not ignored";
}

TEST(CharstringDump, EveryOperatorTheFormatDefinesHasAName) {
  // The dump's name table is one line per operator, and a dump of a program
  // that uses six of them leaves the rest unread - which for a table whose
  // entries are *strings* means nothing checks that they are the right strings.
  // So one program uses every one-byte operator and every two-byte operator,
  // and the dump of it is required to name each.
  Program program;
  const std::vector<uint8_t> one_byte = {kHstem, kVstem, kVmoveto, kRlineto,
      kHlineto, kVlineto, kRrcurveto, kReturn, kHstemhm, kRmoveto, kHmoveto,
      kVstemhm, kRcurveline, kRlinecurve, kVvcurveto, kHhcurveto, kCallgsubr,
      kVhcurveto, kHvcurveto, kEndchar};
  for (uint8_t which : one_byte) {
    program.num(1).num(2).op(which);
  }
  const std::vector<uint8_t> two_byte = {kAnd, kOr, kNot, kAbs, kAdd, kSub,
      kDiv, kNeg, kEq, kDrop, kPut, kGet, kIfelse, kRandom, kMul, kSqrt, kDup,
      kExch, kIndex, kRoll, kHflex, kFlex, kHflex1, kFlex1};
  for (uint8_t which : two_byte) {
    program.num(3).op2(which);
  }
  // And the numeric forms, each of which the dump decodes separately.
  program.num(-500).wide(-30000).fixed(-70000).num(-40).num(700);
  program.raw(31);  // An operator to hang the trailing operands on.

  gfnttest::CapturedOutput out;
  ASSERT_EQ(gfnt_charstring_dump(GFNT_CHARSTRING_TYPE2, program.bytes.data(),
      program.bytes.size(), nullptr, out.get()), GFNT_OK);
  const std::string text = out.finish();

  for (const char * name : {"hstem", "vstem", "vmoveto", "rlineto", "hlineto",
      "vlineto", "rrcurveto", "return", "hstemhm", "rmoveto", "hmoveto",
      "vstemhm", "rcurveline", "rlinecurve", "vvcurveto", "hhcurveto",
      "callgsubr", "vhcurveto", "hvcurveto", "endchar", "and", "or", "not",
      "abs", "add", "sub", "div", "neg", "eq", "drop", "put", "get", "ifelse",
      "random", "mul", "sqrt", "dup", "exch", "index", "roll", "hflex", "flex",
      "hflex1", "flex1"}) {
    EXPECT_NE(text.find(std::string(" ") + name), std::string::npos)
        << name << " is not named in the dump";
  }
  // The negative operand forms, which are where a shift of a negative value
  // used to be - and are printed as themselves.
  EXPECT_NE(text.find("-500"), std::string::npos) << text;
  EXPECT_NE(text.find("-30000"), std::string::npos) << text;
}

TEST(CharstringDump, TypeOnesOperatorsAreNamedAndItsReservedOnesSaidSo) {
  Program program;
  for (uint8_t which : {kClosepath, kHsbw, kCallsubr}) {
    program.num(1).num(2).op(which);
  }
  for (uint8_t which : {kDotsection, kVstem3, kHstem3, kSeac, kSbw,
      kCallothersubr, kPop, kSetcurrentpoint}) {
    program.num(3).op2(which);
  }
  // A Type 2 flex operator, which Type 1 does not have: the dump says
  // `reserved` rather than naming an operator this program cannot contain.
  program.num(4).op2(kFlex);
  // Type 1's 255 is an integer, so this is -70000 and not a fraction.
  program.fixed(-70000).num(1).op(kEndchar);

  gfnttest::CapturedOutput out;
  ASSERT_EQ(gfnt_charstring_dump(GFNT_CHARSTRING_TYPE1, program.bytes.data(),
      program.bytes.size(), nullptr, out.get()), GFNT_OK);
  const std::string text = out.finish();

  EXPECT_NE(text.find("charstring Type 1"), std::string::npos) << text;
  for (const char * name : {"closepath", "hsbw", "callsubr", "dotsection",
      "vstem3", "hstem3", "seac", "sbw", "callothersubr", "pop",
      "setcurrentpoint", "reserved"}) {
    EXPECT_NE(text.find(std::string(" ") + name), std::string::npos)
        << name << " is not named in the dump";
  }
  EXPECT_NE(text.find("-70000"), std::string::npos) << text;
}

TEST(CharstringDump, AReservedOperatorIsNamedRatherThanGuessedAt) {
  Program program;
  program.raw(15).raw(16).raw(17).op(kEndchar);
  gfnttest::CapturedOutput out;

  ASSERT_EQ(gfnt_charstring_dump(GFNT_CHARSTRING_TYPE2, program.bytes.data(),
      program.bytes.size(), nullptr, out.get()), GFNT_OK);
  const std::string text = out.finish();
  // Three of them, so the dump is not stopping at the first.
  EXPECT_EQ(reserved_count(text), 3u) << text;
}

TEST(CharstringDump, ATruncatedOperandEndsTheDumpWithoutReadingPastIt) {
  // Each multi-byte form, cut short. The dump stops rather than reading the
  // byte after the charstring, and says what it had.
  const std::vector<Program> cases = {
      Program().raw(247),
      Program().raw(kShortint).raw(0x01),
      Program().raw(kFixed).raw(0x01).raw(0x02),
      Program().raw(251),
  };
  for (const Program & program : cases) {
    gfnttest::CapturedOutput out;
    EXPECT_EQ(gfnt_charstring_dump(GFNT_CHARSTRING_TYPE2, program.bytes.data(),
        program.bytes.size(), nullptr, out.get()), GFNT_OK);
    const std::string text = out.finish();
    EXPECT_NE(text.find("charstring Type 2"), std::string::npos) << text;
  }
}

TEST(CharstringDump, AnUnassignedTwoByteOperatorIsNamedReservedInEitherLanguage) {
  // Neither language assigns 8, 19, 32 or 38 after an escape, so neither dump
  // can name them - and a dump that guessed would be inventing an operator.
  const std::vector<uint8_t> unassigned = {8, 19, 32, 38};
  for (GFNT_CharstringType type : {GFNT_CHARSTRING_TYPE1,
      GFNT_CHARSTRING_TYPE2}) {
    Program program;
    for (uint8_t which : unassigned) {
      program.num(1).op2(which);
    }
    gfnttest::CapturedOutput out;
    ASSERT_EQ(gfnt_charstring_dump(type, program.bytes.data(),
        program.bytes.size(), nullptr, out.get()), GFNT_OK);
    const std::string text = out.finish();
    EXPECT_EQ(reserved_count(text), unassigned.size())
        << gfnt_charstring_type_string(type) << ": " << text;
  }
}

TEST(CharstringDump, TheDumpNamesOnlyTheOperatorsTheLanguageItIsReadingHas) {
  // The name table is a second copy of the split the interpreter enforces, and
  // it used to be one table for both languages: a Type 2 dump called byte 13
  // `hsbw` and a Type 1 dump called byte 19 `hintmask`, so a dump reported that
  // a font contained an operator the language it is written in does not have.
  // The interpreter refuses each of these by name; the dump has to agree.
  struct Case {
    GFNT_CharstringType type;
    std::vector<uint8_t> single;
    std::vector<uint8_t> escape;
    std::vector<const char *> absent;
  };
  const std::vector<Case> cases = {
      {GFNT_CHARSTRING_TYPE2, kTypeOneOnlySingle, kTypeOneOnlyEscape,
          {"closepath", "hsbw", "dotsection", "vstem3", "hstem3", "seac", "sbw",
              "callothersubr", "pop", "setcurrentpoint"}},
      {GFNT_CHARSTRING_TYPE1, kTypeTwoOnlySingle, kTypeTwoOnlyEscape,
          {"hstemhm", "hintmask", "cntrmask", "vstemhm", "rcurveline",
              "rlinecurve", "vvcurveto", "hhcurveto", "callgsubr", "and", "or",
              "abs", "add", "sub", "neg", "drop", "put", "get", "ifelse",
              "random", "mul", "sqrt", "dup", "exch", "index", "roll", "hflex",
              "flex", "hflex1", "flex1"}},
  };
  for (const Case & item : cases) {
    Program program;
    for (uint8_t which : item.single) {
      program.num(1).op(which);
    }
    for (uint8_t which : item.escape) {
      program.num(1).op2(which);
    }
    gfnttest::CapturedOutput out;
    ASSERT_EQ(gfnt_charstring_dump(item.type, program.bytes.data(),
        program.bytes.size(), nullptr, out.get()), GFNT_OK);
    const std::string text = out.finish();

    for (const char * name : item.absent) {
      EXPECT_EQ(text.find(std::string(" ") + name), std::string::npos)
          << gfnt_charstring_type_string(item.type) << " named " << name
          << ", which it does not have: " << text;
    }
    EXPECT_EQ(reserved_count(text), item.single.size() + item.escape.size())
        << gfnt_charstring_type_string(item.type) << ": " << text;
  }
}

TEST(CharstringDump, ATypeOneDumpDoesNotReadAMaskAfterAByteItReserves) {
  // Worse than a wrong name: the caller divides the bytes after a `hintmask`
  // as a mask, so naming byte 19 in a Type 1 charstring made the dump swallow
  // the operators after it and print fiction from there on. No stems have been
  // declared, so the mask it used to read was one byte wide - and the byte
  // behind it here is the endchar, which a dump that reads a mask never names.
  Program program;
  program.num(0).num(500).op(kHsbw).op(kHintmask).op(kEndchar);

  gfnttest::CapturedOutput out;
  ASSERT_EQ(gfnt_charstring_dump(GFNT_CHARSTRING_TYPE1, program.bytes.data(),
      program.bytes.size(), nullptr, out.get()), GFNT_OK);
  const std::string text = out.finish();

  EXPECT_NE(text.find(" hsbw"), std::string::npos) << text;
  EXPECT_NE(text.find(" reserved"), std::string::npos) << text;
  EXPECT_NE(text.find(" endchar"), std::string::npos)
      << "the byte behind a reserved 19 was read as a mask: " << text;
  EXPECT_EQ(text.find(" mask"), std::string::npos)
      << "a Type 1 dump read a hint mask: " << text;
}

TEST(CharstringDump, NullArgumentsAreCallerErrors) {
  Program program;
  program.op(kEndchar);
  gfnttest::CapturedOutput out;

  EXPECT_EQ(gfnt_charstring_dump(GFNT_CHARSTRING_TYPE2, nullptr, 4, nullptr,
      out.get()), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_charstring_dump(GFNT_CHARSTRING_TYPE2, program.bytes.data(),
      program.bytes.size(), nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_charstring_dump(GFNT_CHARSTRING_TYPE_COUNT,
      program.bytes.data(), program.bytes.size(), nullptr, out.get()),
      GFNT_ERR_INVALID);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
