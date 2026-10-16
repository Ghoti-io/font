/*
 * Tests for TrueType instruction programs: the opcode table, the decoder, the
 * structural check and the disassembly, and reaching a glyph's own program.
 *
 * Nothing here runs a program. documentation/design.md section 8.5.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <cstdio>
#include <string>
#include <vector>

#include <ghoti.io/font/hint.h>
#include <ghoti.io/font/glyph.h>

extern "C" {
#include "../../src/hint/hint_internal.h"
}

#include <gtest/gtest.h>

namespace {

using Bytes = std::vector<uint8_t>;

/** The disassembly of a program, as text. */
std::string disassemble(const Bytes & program) {
  char * buffer = nullptr;
  size_t size = 0;
  FILE * stream = open_memstream(&buffer, &size);
  EXPECT_NE(stream, nullptr);
  EXPECT_EQ(gfnt_program_disassemble(program.data(), program.size(), stream),
      GFNT_OK);
  fclose(stream);
  std::string text(buffer, size);
  free(buffer);
  return text;
}

GFNT_ProgramReport validate(const Bytes & program) {
  GFNT_ProgramReport report{};
  EXPECT_EQ(gfnt_program_validate(program.data(), program.size(), &report),
      GFNT_OK);
  return report;
}

}  // namespace

TEST(HintOps, EveryOpcodeHasARowAndTheDefinedOnesAreTheSpecifications) {
  size_t defined = 0;

  for (unsigned op = 0; op < 256; ++op) {
    const GFNT_HintOp * row = gfnt_hint_op((uint8_t)op);
    ASSERT_NE(row, nullptr);
    ASSERT_NE(row->name, nullptr);
    if (row->defined) {
      ++defined;
    }
  }
  // 0x00-0x27, 0x29-0x7A, 0x7C-0x82, 0x85-0x8E, 0x91, 0xB0-0xFF.
  EXPECT_EQ(defined, 40u + 82u + 7u + 10u + 1u + 80u);
  EXPECT_FALSE(gfnt_hint_op(0x28)->defined);
  EXPECT_FALSE(gfnt_hint_op(0x7B)->defined);
  EXPECT_FALSE(gfnt_hint_op(0xAF)->defined);
  EXPECT_FALSE(gfnt_hint_op(0x92)->defined);  // not in the specification
  EXPECT_STREQ(gfnt_hint_op(0xC1)->name, "MDRP");
  EXPECT_EQ(gfnt_hint_op(0xC1)->flag_bits, 5);
  EXPECT_STREQ(gfnt_hint_op(0x6B)->name, "ROUND");
  EXPECT_EQ(gfnt_hint_op(0x6B)->flag_bits, 2);
  EXPECT_EQ(gfnt_hint_op(0x2B)->pops, 1);   // CALL
  EXPECT_EQ(gfnt_hint_op(0x0F)->pops, 5);   // ISECT
  EXPECT_EQ(gfnt_hint_op(0x8A)->pushes, 3); // ROLL
  // Flags count from the row's first opcode: MD[0] is 0x49, not 0x48.
  EXPECT_EQ(gfnt_hint_op(0x4A)->first, 0x49);
}

TEST(HintDecode, PushesCarryTheirValuesAndLengths) {
  const Bytes program = {0x40, 3, 1, 2, 255,       // NPUSHB 3
                         0x41, 2, 0xFF, 0xFE, 0, 7, // NPUSHW 2: -2, 7
                         0xB1, 9, 8,                // PUSHB[001]: two bytes
                         0xB8, 0x80, 0x00,          // PUSHW[000]: -32768
                         0x2B};                     // CALL
  size_t at = 0;
  GFNT_HintInsn insn;

  ASSERT_EQ(gfnt_hint_decode(program.data(), program.size(), at, &insn), GFNT_OK);
  EXPECT_EQ(insn.count, 3u);
  EXPECT_EQ(insn.length, 5u);
  EXPECT_EQ(gfnt_hint_push_value(program.data(), &insn, 2), 255);
  at += insn.length;
  ASSERT_EQ(gfnt_hint_decode(program.data(), program.size(), at, &insn), GFNT_OK);
  EXPECT_EQ(insn.count, 2u);
  EXPECT_TRUE(insn.word);
  EXPECT_EQ(gfnt_hint_push_value(program.data(), &insn, 0), -2);
  EXPECT_EQ(gfnt_hint_push_value(program.data(), &insn, 1), 7);
  at += insn.length;
  ASSERT_EQ(gfnt_hint_decode(program.data(), program.size(), at, &insn), GFNT_OK);
  EXPECT_EQ(insn.count, 2u);
  at += insn.length;
  ASSERT_EQ(gfnt_hint_decode(program.data(), program.size(), at, &insn), GFNT_OK);
  EXPECT_EQ(gfnt_hint_push_value(program.data(), &insn, 0), -32768);
  at += insn.length;
  ASSERT_EQ(gfnt_hint_decode(program.data(), program.size(), at, &insn), GFNT_OK);
  EXPECT_EQ(insn.length, 1u);
  EXPECT_EQ(at + insn.length, program.size());
}

TEST(HintDecode, AnEmptyPushAndTheLargestOneAreBothWhole) {
  Bytes zero = {0x40, 0};
  Bytes big = {0x41, 255};
  GFNT_HintInsn insn;

  ASSERT_EQ(gfnt_hint_decode(zero.data(), zero.size(), 0, &insn), GFNT_OK);
  EXPECT_EQ(insn.count, 0u);
  EXPECT_EQ(insn.length, 2u);
  big.resize(2 + 255 * 2, 0);
  ASSERT_EQ(gfnt_hint_decode(big.data(), big.size(), 0, &insn), GFNT_OK);
  EXPECT_EQ(insn.length, 512u);
}

TEST(HintDecode, EveryCutOfAPushIsRefusedAndNeverReadPast) {
  // Each prefix is copied to an allocation of exactly its size, so a read past
  // the end is a sanitizer finding and not a lucky zero.
  const Bytes whole = {0x41, 2, 0, 1, 0, 2, 0xB2, 1, 2, 3};

  for (size_t cut = 0; cut <= whole.size(); ++cut) {
    Bytes prefix(whole.begin(), whole.begin() + (long)cut);
    size_t at = 0;
    bool refused = false;

    while (at < prefix.size()) {
      GFNT_HintInsn insn;
      GFNT_Result result = gfnt_hint_decode(prefix.data(), prefix.size(), at, &insn);

      if (result != GFNT_OK) {
        EXPECT_EQ(result, GFNT_ERR_CORRUPT);
        refused = true;
        break;
      }
      ASSERT_GT(insn.length, 0u);
      ASSERT_LE(insn.offset + insn.length, prefix.size());
      at += insn.length;
    }
    // The program is whole at 0, 6 (after the NPUSHW), and 10 (the end).
    EXPECT_EQ(refused, cut != 0 && cut != 6 && cut != 10) << "cut at " << cut;
  }
}

TEST(HintDecode, BadArgumentsAreRefused) {
  GFNT_HintInsn insn;
  const Bytes program = {0x20};

  EXPECT_EQ(gfnt_hint_decode(nullptr, 1, 0, &insn), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_hint_decode(program.data(), 1, 1, &insn), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_hint_decode(program.data(), 1, 0, nullptr), GFNT_ERR_INVALID);
}

TEST(HintValidate, WellFormedProgramsPass) {
  EXPECT_EQ(validate({}).problem, GFNT_PROGRAM_OK);
  // FDEF 0 ... IF ... ELSE ... EIF ... ENDF, then a top-level IF/EIF.
  EXPECT_EQ(validate({0xB0, 0, 0x2C, 0xB0, 1, 0x58, 0x1B, 0x59, 0x2D,
                      0xB0, 1, 0x58, 0x59}).problem, GFNT_PROGRAM_OK);
  // IDEF and a nested IF.
  EXPECT_EQ(validate({0xB0, 0x91, 0x89, 0x58, 0x58, 0x59, 0x59, 0x2D}).problem,
      GFNT_PROGRAM_OK);
  // Bytes that look like opcodes inside push data are data.
  EXPECT_EQ(validate({0xB1, 0x58, 0x2D}).problem, GFNT_PROGRAM_OK);
}

TEST(HintValidate, EachFlawIsNamedAtTheRightOffset) {
  GFNT_ProgramReport r;

  r = validate({0x20, 0x40, 5, 1});
  EXPECT_EQ(r.problem, GFNT_PROGRAM_TRUNCATED_PUSH);
  EXPECT_EQ(r.offset, 1u);
  r = validate({0x20, 0x59});
  EXPECT_EQ(r.problem, GFNT_PROGRAM_UNBALANCED_IF);
  EXPECT_EQ(r.offset, 1u);
  r = validate({0x1B});
  EXPECT_EQ(r.problem, GFNT_PROGRAM_UNBALANCED_IF);
  r = validate({0x20, 0x58, 0x58, 0x59});
  EXPECT_EQ(r.problem, GFNT_PROGRAM_UNBALANCED_IF);
  EXPECT_EQ(r.offset, 1u);                       // the IF that was never closed
  r = validate({0x2D});
  EXPECT_EQ(r.problem, GFNT_PROGRAM_UNBALANCED_DEF);
  r = validate({0x2C, 0x2C, 0x2D});
  EXPECT_EQ(r.problem, GFNT_PROGRAM_UNBALANCED_DEF);
  EXPECT_EQ(r.offset, 1u);                       // a definition inside one
  r = validate({0x20, 0x2C, 0x20});
  EXPECT_EQ(r.problem, GFNT_PROGRAM_UNBALANCED_DEF);
  EXPECT_EQ(r.offset, 1u);                       // the one never ended
  // An IF opened outside a function and closed inside it, and the reverse.
  EXPECT_EQ(validate({0x58, 0x2C, 0x59, 0x2D}).problem, GFNT_PROGRAM_UNBALANCED_IF);
  EXPECT_EQ(validate({0x2C, 0x58, 0x2D, 0x59}).problem, GFNT_PROGRAM_UNBALANCED_IF);
}

TEST(HintValidate, ArgumentsAreChecked) {
  GFNT_ProgramReport report;

  EXPECT_EQ(gfnt_program_validate(nullptr, 1, &report), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_program_validate(nullptr, 0, &report), GFNT_OK);
  EXPECT_EQ(gfnt_program_validate(nullptr, 0, nullptr), GFNT_ERR_INVALID);
}

TEST(HintDisassemble, UsesTheSpecificationsMnemonics) {
  EXPECT_EQ(disassemble({0xB1, 9, 255, 0xC1, 0xFF, 0x28, 0x6B, 0x00}),
      "PUSHB[ ]\n9\n255\n"
      "MDRP[00001]\n"
      "MIRP[11111]\n"
      "INSTR40[ ]\n"
      "ROUND[11]\n"
      "SVTCA[0]\n");
  EXPECT_EQ(disassemble({0x49, 0x4A}), "MD[0]\nMD[1]\n");
  EXPECT_EQ(disassemble({0x41, 2, 0xFF, 0xFE, 0, 7}),
      "NPUSHW[ ]\n-2\n7\n");
  EXPECT_EQ(disassemble({0x40, 4, 1}), "TRUNCATED[0]\n");
}

TEST(HintDisassemble, BadArgumentsAreRefused) {
  EXPECT_EQ(gfnt_program_disassemble(nullptr, 1, stdout), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_program_disassemble(nullptr, 0, nullptr), GFNT_ERR_INVALID);
}

namespace {

/** A face over glyphs built by hand, and the program of each. */
struct Built {
  Bytes bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  explicit Built(const std::vector<Bytes> & glyphs) {
    Bytes glyf;
    Bytes loca;
    bool is_long = false;
    std::vector<std::pair<uint16_t, int16_t>> metrics;

    gfnttest::build_glyf_and_loca(glyphs, &glyf, &loca, &is_long);
    for (size_t i = 0; i < glyphs.size(); ++i) {
      metrics.push_back({500, 0});
    }
    bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {
        {GFNT_TAG('h', 'e', 'a', 'd'),
            gfnttest::build_head(1000, is_long ? 1 : 0)},
        {GFNT_TAG('h', 'h', 'e', 'a'),
            gfnttest::build_hhea(800, -200, 100, (uint16_t)glyphs.size())},
        {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics, {})},
        {GFNT_TAG('m', 'a', 'x', 'p'),
            gfnttest::build_maxp((uint16_t)glyphs.size())},
        {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
        {GFNT_TAG('l', 'o', 'c', 'a'), loca},
    });
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
  }
  ~Built() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Built(const Built &) = delete;
  Built & operator=(const Built &) = delete;

  Bytes program(uint32_t glyph, GFNT_Result expected = GFNT_OK) {
    const uint8_t * data = reinterpret_cast<const uint8_t *>(1);
    size_t length = 99;
    GFNT_Result result = gfnt_face_glyph_program(face, glyph, &data, &length,
        &error);

    EXPECT_EQ(result, expected);
    if (result != GFNT_OK) {
      return {};
    }
    if (length == 0) {
      EXPECT_EQ(data, nullptr);
      return {};
    }
    return Bytes(data, data + length);
  }
};

}  // namespace

TEST(HintGlyphProgram, ASimpleGlyphsProgramIsFound) {
  const Bytes program = {0xB0, 0, 0x2E, 0x30};
  Built font({
      gfnttest::build_glyf_glyph({{{0, 0, true}, {100, 0, true}, {100, 100, true}}},
          program),
      gfnttest::build_glyf_glyph({{{0, 0, true}, {100, 0, true}, {100, 100, true}}}),
      {},
  });

  EXPECT_EQ(font.program(0), program);
  EXPECT_TRUE(font.program(1).empty());   // a glyph with none
  EXPECT_TRUE(font.program(2).empty());   // an empty glyph
  font.program(3, GFNT_ERR_INVALID);      // past numGlyphs
}

TEST(HintGlyphProgram, ACompositesProgramFollowsItsLastComponent) {
  const Bytes program = {0xB0, 7, 0x2B};
  Built font({
      gfnttest::build_glyf_glyph({{{0, 0, true}, {100, 0, true}, {100, 100, true}}}),
      // Two components, one with a two-by-two transform and word arguments.
      gfnttest::build_glyf_composite({
          {0, 0x0003, 10, 20, {}},
          {0, 0x0003 | 0x0080, 1, 2, {0x4000, 0, 0, 0x4000}},
      }, program),
      gfnttest::build_glyf_composite({{0, 0x0002, 1, 2, {}}}),
  });

  EXPECT_EQ(font.program(1), program);
  EXPECT_TRUE(font.program(2).empty());
}

TEST(HintGlyphProgram, AFaceWithoutGlyfAndBadArgumentsAreRefused) {
  const uint8_t * data = nullptr;
  size_t length = 0;

  EXPECT_EQ(gfnt_face_glyph_program(nullptr, 0, &data, &length, nullptr),
      GFNT_ERR_INVALID);
  Built font({gfnttest::build_glyf_glyph({{{0, 0, true}}})});
  EXPECT_EQ(gfnt_face_glyph_program(font.face, 0, nullptr, &length, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_program(font.face, 0, &data, nullptr, nullptr),
      GFNT_ERR_INVALID);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
