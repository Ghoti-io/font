/**
 * @file
 *
 * The Type 1 font program as a container: PFB and PFA framing, `eexec`, the
 * PostScript, and the per-charstring cipher.
 *
 * documentation/design.md section 7.1. The *language* is `test_charstring.cpp`'s
 * and needs none of this. What is here is the container: a file whose private
 * half is encrypted, which has no table directory and no `maxp`, `hmtx`, `cmap`
 * or `name`, and which has to answer for all of that out of its own PostScript.
 *
 * `tests/data/fonts/type1.pfb` and `type1.pfa` hold the same eight charstrings
 * as `cff-type1.otf`, so the strongest statement here is that three containers
 * draw one set of programs identically.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/font/charstring.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/name.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>

#include <gtest/gtest.h>

#include "../../src/type1/type1.h"

namespace {

/** A face loaded from a committed fixture. */
struct Fixture {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  GFNT_Result result = GFNT_ERR_INTERNAL;

  explicit Fixture(const std::string & name) {
    gfnt_error_clear(&error);
    const std::string path = gfnttest::data("fonts/" + name);
    result = gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &blob,
        &error);
    EXPECT_EQ(result, GFNT_OK) << "could not read " << path;
    if (result != GFNT_OK) {
      return;
    }
    result = gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error);
    EXPECT_EQ(result, GFNT_OK) << name << ": "
        << (error.message ? error.message : "");
  }
  ~Fixture() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Fixture(const Fixture &) = delete;
  Fixture & operator=(const Fixture &) = delete;
  operator const GFNT_Face *() const { return face; }
};

/** A face loaded from bytes, which may legitimately fail. */
struct FromBytes {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  GFNT_Result result = GFNT_ERR_INTERNAL;

  explicit FromBytes(std::vector<uint8_t> data) : bytes(std::move(data)) {
    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
    result = gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error);
  }
  ~FromBytes() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  FromBytes(const FromBytes &) = delete;
  FromBytes & operator=(const FromBytes &) = delete;
};

/** Whether a message names something. */
void names(const GFNT_Error & error, const char * fragment) {
  ASSERT_NE(error.message, nullptr);
  EXPECT_NE(std::string(error.message).find(fragment), std::string::npos)
      << error.message;
}

/** A fixture's bytes. */
std::vector<uint8_t> bytes_of(const std::string & name) {
  const std::string path = gfnttest::data("fonts/" + name);
  std::vector<uint8_t> out;
  FILE * handle = fopen(path.c_str(), "rb");
  EXPECT_NE(handle, nullptr) << path;
  if (!handle) {
    return out;
  }
  uint8_t buffer[4096];
  size_t got;
  while ((got = fread(buffer, 1, sizeof buffer, handle)) > 0) {
    out.insert(out.end(), buffer, buffer + got);
  }
  fclose(handle);
  return out;
}

/** Every point of every contour of one glyph, as a comparable string. */
std::string path_of(const GFNT_Face * face, uint32_t glyph) {
  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  if (gfnt_face_glyph_outline(face, glyph, nullptr, nullptr, &outline, &error)
      != GFNT_OK) {
    return std::string("refused: ")
        + (error.message ? error.message : "no reason");
  }
  std::string out;
  const size_t contours = gfnt_outline_contour_count(outline);
  for (size_t c = 0; c < contours; ++c) {
    size_t first = 0;
    size_t count = 0;
    EXPECT_EQ(gfnt_outline_contour_at(outline, c, &first, &count), GFNT_OK);
    out += "[";
    for (size_t i = first; i < first + count; ++i) {
      GFNT_Point point{};
      GFNT_PointTag tag = GFNT_POINT_ON;
      EXPECT_EQ(gfnt_outline_point_at(outline, i, &point, &tag), GFNT_OK);
      out += std::to_string(point.x) + "," + std::to_string(point.y) + ":"
          + gfnt_point_tag_string(tag) + " ";
    }
    out += "]";
  }
  gfnt_outline_destroy(outline);
  return out;
}

/** One glyph's name. */
std::string name_of(const GFNT_Face * face, uint32_t glyph) {
  char * text = nullptr;
  if (gfnt_face_glyph_name(face, glyph, nullptr, &text, nullptr, nullptr)
      != GFNT_OK) {
    return std::string();
  }
  const std::string out = text;
  gfnt_name_free(nullptr, text);
  return out;
}

/** One name of a face. */
std::string face_name(const GFNT_Face * face, uint16_t id) {
  char * text = nullptr;
  if (gfnt_face_name(face, id, GFNT_LANGUAGE_ANY, nullptr, &text, nullptr,
      nullptr) != GFNT_OK) {
    return std::string();
  }
  const std::string out = text;
  gfnt_name_free(nullptr, text);
  return out;
}

const char * const kGlyphNames[] = {".notdef", "space", "A", "acute", "Aacute",
    "flex", "hints", "current"};

// ------------------------------------------------------- the two framings

TEST(Type1, BothFramingsLoadAsAFaceWithOneSyntheticTable) {
  for (const char * name : {"type1.pfb", "type1.pfa"}) {
    Fixture font(name);
    ASSERT_EQ(font.result, GFNT_OK) << name;

    // Not GFNT_FLAVOUR_APPLE_TYPE1, which is the `typ1` version of an *sfnt*
    // whose tables hold Type 1 data. This is the font program on its own.
    EXPECT_EQ(gfnt_face_flavour(font), GFNT_FLAVOUR_TYPE1) << name;
    EXPECT_EQ(gfnt_face_table_count(font), 1u) << name;
    GFNT_Tag tag = 0;
    ASSERT_EQ(gfnt_face_table_tag_at(font, 0, &tag), GFNT_OK) << name;
    EXPECT_EQ(tag, GFNT_TAG_TYPE1) << name;
    EXPECT_TRUE(gfnt_face_has_outlines(font)) << name;

    // The table is the *derived* program, not the file: a PFB's segment headers
    // are gone, a PFA's hex is bytes, and the private half is decrypted - so the
    // extent has no reason to match the file's size and must not be compared to
    // it.
    size_t offset = 1;
    size_t length = 0;
    ASSERT_EQ(gfnt_face_table_range(font, GFNT_TAG_TYPE1, &offset, &length),
        GFNT_OK) << name;
    EXPECT_EQ(offset, 0u) << name;
    EXPECT_GT(length, 0u) << name;
  }
}

TEST(Type1, TheDerivedProgramIsPlainPostScriptWithItsPrivateHalfDecrypted) {
  // What deriving is for: the file's private half is ciphertext, and after this
  // the same reader that walks the cleartext walks the rest.
  Fixture font("type1.pfb");
  ASSERT_EQ(font.result, GFNT_OK);
  const GFNT_Type1 * type1 = nullptr;
  ASSERT_EQ(gfnt_face_type1(font, &type1, nullptr), GFNT_OK);

  const std::vector<uint8_t> file = bytes_of("type1.pfb");
  ASSERT_FALSE(file.empty());
  const std::string raw(reinterpret_cast<const char *>(file.data()),
      file.size());
  // The file does not contain these words; the derived program does.
  EXPECT_EQ(raw.find("/CharStrings"), std::string::npos)
      << "the fixture's private half is not encrypted, so nothing is being "
         "tested here";
  EXPECT_GT(type1->glyph_count, 0u);
  EXPECT_EQ(type1->subr_count, 2u);
  EXPECT_EQ(type1->len_iv, 4);
}

TEST(Type1, APfaStatesTheOtherSpellingOfEverything) {
  // `-|`, `|-`, `|` for RD, ND and NP; `/lenIV 0`; a `/CharStrings` count
  // smaller than the glyphs it defines; and StandardEncoding rather than an
  // array. A reader that knew only one spelling of each would refuse it.
  Fixture font("type1.pfa");
  ASSERT_EQ(font.result, GFNT_OK);
  const GFNT_Type1 * type1 = nullptr;
  ASSERT_EQ(gfnt_face_type1(font, &type1, nullptr), GFNT_OK);

  EXPECT_EQ(type1->len_iv, 0) << "the PFA fixture states /lenIV 0";
  EXPECT_TRUE(type1->encoding_is_standard);
  // Eight glyphs from a dictionary that declared two: a PostScript `dict` count
  // is a minimum, and a reader that treated it as a bound would lose six.
  EXPECT_EQ(type1->glyph_count, 8u);
  EXPECT_EQ(type1->subr_count, 2u);
}

TEST(Type1, ThreeContainersDrawOneSetOfProgramsIdentically) {
  // The whole point of the fixtures holding the same charstrings: PFB framing
  // with a binary private half, PFA framing with a hex one, and a CFF that says
  // `CharstringType 1`. What differs is eexec, the per-charstring cipher, the
  // segment headers and a PostScript dictionary instead of INDEXes - and none of
  // that may reach the outline.
  Fixture pfb("type1.pfb");
  Fixture pfa("type1.pfa");
  Fixture cff("cff-type1.otf");
  ASSERT_EQ(pfb.result, GFNT_OK);
  ASSERT_EQ(pfa.result, GFNT_OK);
  ASSERT_EQ(cff.result, GFNT_OK);

  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(pfb, &glyphs, nullptr), GFNT_OK);
  ASSERT_EQ(glyphs, 8u);
  size_t drawn = 0;
  for (uint32_t glyph = 0; glyph < glyphs; ++glyph) {
    const std::string from_pfb = path_of(pfb, glyph);

    EXPECT_EQ(from_pfb, path_of(pfa, glyph)) << "glyph " << glyph;
    EXPECT_EQ(from_pfb, path_of(cff, glyph)) << "glyph " << glyph;
    if (from_pfb.find("refused") == std::string::npos && !from_pfb.empty()) {
      ++drawn;
    }
  }
  // Not vacuous: eight refusals would also have compared equal.
  EXPECT_GE(drawn, 7u) << "the glyphs have to have drawn for this to mean "
                          "anything";
}

TEST(Type1, ItRendersThePixelsTheCffOfTheSameProgramsRenders) {
  Fixture pfb("type1.pfb");
  Fixture cff("cff-type1.otf");
  ASSERT_EQ(pfb.result, GFNT_OK);
  ASSERT_EQ(cff.result, GFNT_OK);

  for (uint32_t ppem : {8u, 16u, 48u}) {
    for (uint32_t glyph = 2; glyph < 8; ++glyph) {
      GFNT_Coverage a{};
      GFNT_Coverage b{};
      GFNT_Error error{};
      ASSERT_EQ(gfnt_face_render_glyph(pfb, glyph, ppem, nullptr, nullptr, &a,
          &error), GFNT_OK) << error.message;
      ASSERT_EQ(gfnt_face_render_glyph(cff, glyph, ppem, nullptr, nullptr, &b,
          nullptr), GFNT_OK);
      EXPECT_EQ(gfnt_coverage_hash(&a), gfnt_coverage_hash(&b))
          << "glyph " << glyph << " at " << ppem << " ppem";
      gfnt_coverage_destroy(&a);
      gfnt_coverage_destroy(&b);
    }
  }
}

// --------------------------------------------- what it answers for itself

TEST(Type1, TheGlyphsAreNamedAndNotdefLeads) {
  // Type 1 is name-keyed: nothing in the font says which glyph is number three.
  // The index is this library's invention, and `.notdef` leads because glyph 0
  // means "no glyph" everywhere else it reads.
  for (const char * file : {"type1.pfb", "type1.pfa"}) {
    Fixture font(file);
    ASSERT_EQ(font.result, GFNT_OK) << file;

    for (uint32_t glyph = 0; glyph < 8; ++glyph) {
      EXPECT_EQ(name_of(font, glyph), kGlyphNames[glyph])
          << file << " glyph " << glyph;
    }
  }
}

TEST(Type1, TheEmAndTheGlyphCountComeFromTheProgram) {
  Fixture font("type1.pfb");
  ASSERT_EQ(font.result, GFNT_OK);

  uint16_t upem = 0;
  ASSERT_EQ(gfnt_face_units_per_em(font, &upem, nullptr), GFNT_OK);
  EXPECT_EQ(upem, 1000) << "from /FontMatrix, there being no head";

  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(font, &glyphs, nullptr), GFNT_OK);
  EXPECT_EQ(glyphs, 8u) << "from /CharStrings, there being no maxp";
  size_t claimed = 0xFFFFFFFFu;
  EXPECT_FALSE(gfnt_face_num_glyphs_disagreement(font, &claimed));
  EXPECT_EQ(claimed, 0u) << "nothing claimed a count for this to disagree with";
}

TEST(Type1, EachAdvanceComesFromThatGlyphsOwnHsbw) {
  // There is no `hmtx` and never will be: a Type 1 glyph states its advance in
  // its own charstring, which means reading it is running it.
  Fixture font("type1.pfb");
  ASSERT_EQ(font.result, GFNT_OK);

  int32_t advance = -1;
  ASSERT_EQ(gfnt_face_glyph_advance(font, 2, nullptr, &advance, nullptr),
      GFNT_OK);
  EXPECT_EQ(advance, 600);
  // `space` states a different one, so this is not one number for every glyph.
  ASSERT_EQ(gfnt_face_glyph_advance(font, 1, nullptr, &advance, nullptr),
      GFNT_OK);
  EXPECT_EQ(advance, 300);

  // And the side bearing, which Type 2 has no operator for at all.
  GFNT_CharstringMetrics metrics{};
  ASSERT_EQ(gfnt_face_glyph_charstring_metrics(font, 2, &metrics, nullptr),
      GFNT_OK);
  EXPECT_EQ(metrics.side_bearing, 50 * 65536);
  EXPECT_TRUE(metrics.width_stated);
}

TEST(Type1, ItNamesItselfOutOfFontInfoAndFontName) {
  Fixture font("type1.pfb");
  ASSERT_EQ(font.result, GFNT_OK);

  EXPECT_EQ(face_name(font, GFNT_NAME_FAMILY), "Ghoti Fixture Type1 PFB");
  EXPECT_EQ(face_name(font, GFNT_NAME_FULL),
      "Ghoti Fixture Type1 PFB Regular");
  EXPECT_EQ(face_name(font, GFNT_NAME_POSTSCRIPT),
      "GhotiFixtureType1PFB-Regular");
  EXPECT_EQ(face_name(font, GFNT_NAME_SUBFAMILY), "Regular");
  EXPECT_EQ(face_name(font, GFNT_NAME_VERSION), "001.000");
  // The licence, which every committed fixture has to state and which this
  // container states in `/Notice` because it has no `name` table (section 14.5).
  EXPECT_EQ(face_name(font, GFNT_NAME_COPYRIGHT),
      "Copyright 2026 Corey Pennycuff. LGPL-3.0-only.");
}

TEST(Type1, ANameTheProgramDoesNotStateIsUnsupportedAndSaysWhich) {
  Fixture font("type1.pfb");
  ASSERT_EQ(font.result, GFNT_OK);

  for (uint16_t id : {GFNT_NAME_MANUFACTURER, GFNT_NAME_DESIGNER,
      GFNT_NAME_LICENSE, GFNT_NAME_UNIQUE_ID}) {
    char * text = nullptr;
    GFNT_Error error{};
    EXPECT_EQ(gfnt_face_name(font, id, GFNT_LANGUAGE_ANY, nullptr, &text,
        nullptr, &error), GFNT_ERR_UNSUPPORTED) << "name id " << id;
    EXPECT_EQ(text, nullptr);
    names(error, "does not state");
  }
}

TEST(Type1, ItsOwnEncodingMapsCodesAndIsNotACmap) {
  // A Type 1 `/Encoding` is 256 entries of the font's own choosing and says
  // nothing about Unicode, so a codepoint lookup is refused rather than guessed
  // at - turning a code into a codepoint needs the Adobe Glyph List.
  Fixture font("type1.pfb");
  ASSERT_EQ(font.result, GFNT_OK);
  const GFNT_Type1 * type1 = nullptr;
  ASSERT_EQ(gfnt_face_type1(font, &type1, nullptr), GFNT_OK);

  EXPECT_FALSE(type1->encoding_is_standard);
  EXPECT_EQ(gfnt_type1_glyph_for_code(type1, 0x20), 1u);   // space
  EXPECT_EQ(gfnt_type1_glyph_for_code(type1, 0x41), 2u);   // A
  EXPECT_EQ(gfnt_type1_glyph_for_code(type1, 0xC1), 4u);   // Aacute
  EXPECT_EQ(gfnt_type1_glyph_for_code(type1, 0x42), GFNT_GLYPH_NONE);
  EXPECT_EQ(gfnt_type1_glyph_for_code(nullptr, 0x41), GFNT_GLYPH_NONE);

  uint32_t glyph = 0;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(font, 0x41, &glyph, &error),
      GFNT_ERR_UNSUPPORTED);
}

TEST(Type1, AnAccentedCharacterResolvesItsPartsByStandardEncodingCode) {
  // `seac` names its components by Standard Encoding code whatever the font's
  // own /Encoding says. This font's own encoding puts Aacute at 0xC1; the accent
  // is named by code 194 in the *standard* table, and going through the font's
  // encoding instead would build it out of whichever glyph that font puts there.
  Fixture font("type1.pfb");
  ASSERT_EQ(font.result, GFNT_OK);

  bool composite = false;
  ASSERT_EQ(gfnt_face_glyph_is_composite(font, 4, &composite, nullptr), GFNT_OK);
  EXPECT_TRUE(composite) << "Aacute is an accented character";
  ASSERT_EQ(gfnt_face_glyph_is_composite(font, 2, &composite, nullptr), GFNT_OK);
  EXPECT_FALSE(composite) << "A is not";
  // Two contours: the base and the accent.
  GFNT_Outline * outline = nullptr;
  ASSERT_EQ(gfnt_face_glyph_outline(font, 4, nullptr, nullptr, &outline,
      nullptr), GFNT_OK);
  EXPECT_EQ(gfnt_outline_contour_count(outline), 2u);
  gfnt_outline_destroy(outline);
}

TEST(Type1, AProgramTooLargeForOneAllocationGrowsEveryArray) {
  // Three reallocation paths, and no other fixture reaches any of them: the
  // buffer the derived program is built in doubles past 4 KB, the glyph array
  // doubles past sixteen, and the subroutine array grows to fit a *number*
  // rather than a count.
  Fixture font("type1-big.pfb");
  ASSERT_EQ(font.result, GFNT_OK);
  const GFNT_Type1 * type1 = nullptr;
  ASSERT_EQ(gfnt_face_type1(font, &type1, nullptr), GFNT_OK);

  EXPECT_GT(type1->length, 4096u) << "the derived program has to exceed the "
                                    "buffer's first allocation, or the doubling "
                                    "is not being reached";
  EXPECT_EQ(type1->glyph_count, 80u);
  // Numbers 0, 1 and 40 with a gap between, so the array is sized by the highest
  // number used and not by how many there are.
  EXPECT_EQ(type1->subr_count, 41u);

  const uint8_t * bytes = nullptr;
  size_t length = 0;
  EXPECT_EQ(gfnt_type1_subr(type1, 40, &bytes, &length), GFNT_OK);
  EXPECT_GT(length, 0u);
  // A gap reads as "not there" rather than as whatever was in that memory.
  EXPECT_EQ(gfnt_type1_subr(type1, 20, &bytes, &length), GFNT_ERR_CORRUPT);
  EXPECT_EQ(gfnt_type1_subr(type1, 41, &bytes, &length), GFNT_ERR_CORRUPT);

  // And every glyph draws, including the ones that call the sparse high number.
  size_t drawn = 0;
  for (uint32_t glyph = 0; glyph < 80; ++glyph) {
    if (path_of(font, glyph).find("refused") == std::string::npos) {
      ++drawn;
    }
  }
  EXPECT_EQ(drawn, 80u);
}

// ------------------------------------------- the arms only a broken font meets

/**
 * Adobe's cipher, in the encrypting direction.
 *
 * The *inverse* of what this module does rather than a copy of it, which is what
 * makes it usable as a test input: get either direction wrong and the round trip
 * fails loudly on every test here, where a copy of the decryptor would agree with
 * a decryptor that was wrong. The committed fixtures are encrypted by fontTools
 * instead, so there are two independent encryptors behind these tests.
 */
std::vector<uint8_t> encrypt(const std::vector<uint8_t> & plain, unsigned r,
    size_t pad) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> padded(pad, 'G');

  padded.insert(padded.end(), plain.begin(), plain.end());
  for (uint8_t byte : padded) {
    const uint8_t cipher = static_cast<uint8_t>(byte ^ (r >> 8));

    out.push_back(cipher);
    r = ((cipher + r) * 52845u + 22719u) & 0xFFFFu;
  }
  return out;
}

std::vector<uint8_t> as_bytes(const std::string & text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

/** A Type 1 font program, assembled so that any part of it can be broken. */
struct Program {
  std::string clear =
      "%!PS-AdobeFont-1.0: Probe 001.000\n"
      "/FontName /Probe def\n"
      "/FontMatrix [0.001 0 0 0.001 0 0] readonly def\n"
      "/Encoding StandardEncoding def\n"
      "currentdict end\ncurrentfile eexec\n";
  std::string private_part =
      "dup /Private 8 dict dup begin\n/lenIV 4 def\n";
  std::vector<uint8_t> charstring{0x8B, 0x8B, 0x0D, 0x0E}; // 0 0 hsbw endchar
  std::string glyph_name = ".notdef";
  bool with_charstrings = true;
  /**
   * How many padding bytes each charstring is encrypted with.
   *
   * Has to match whatever the private portion's `/lenIV` says, because that is
   * how many the reader throws away - a fixture that pads four and declares zero
   * has four cipher bytes of rubbish at the head of every program.
   */
  size_t pad = 4;

  /** The whole file, as a PFB. */
  std::vector<uint8_t> pfb() const {
    std::string body = private_part;

    if (with_charstrings) {
      char head[128];
      snprintf(head, sizeof head, "2 index /CharStrings 1 dict dup begin\n/%s %zu RD ",
          glyph_name.c_str(), encrypt(charstring, 4330u, pad).size());
      body += head;
      const std::vector<uint8_t> cipher = encrypt(charstring, 4330u, pad);
      body.append(reinterpret_cast<const char *>(cipher.data()), cipher.size());
      body += " ND\nend\nend\n";
    }
    const std::vector<uint8_t> encrypted =
        encrypt(as_bytes(body), 55665u, 4);
    std::vector<uint8_t> out;
    const std::vector<uint8_t> parts[2] = {as_bytes(clear), encrypted};
    const uint8_t kinds[2] = {1, 2};

    for (size_t which = 0; which < 2; ++which) {
      out.push_back(0x80);
      out.push_back(kinds[which]);
      for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>(parts[which].size() >> shift));
      }
      out.insert(out.end(), parts[which].begin(), parts[which].end());
    }
    out.push_back(0x80);
    out.push_back(3);
    return out;
  }
};

TEST(Type1, AProbeProgramLoads) {
  // The control for every test below: they break one thing each, and if the
  // unbroken one did not load they would all be passing for the wrong reason.
  Program program;
  FromBytes font(program.pfb());
  ASSERT_EQ(font.result, GFNT_OK)
      << (font.error.message ? font.error.message : "");
  size_t glyphs = 0;
  EXPECT_EQ(gfnt_face_num_glyphs(font.face, &glyphs, nullptr), GFNT_OK);
  EXPECT_EQ(glyphs, 1u);
}

TEST(Type1, EveryShapeOfBrokenFramingIsRefusedByName) {
  Program program;
  const std::vector<uint8_t> good = program.pfb();

  struct Case {
    const char * what;
    std::vector<uint8_t> bytes;
    GFNT_Result result;
    const char * says;
  };
  std::vector<Case> cases;

  // A segment that does not start with the marker: the second header is broken.
  {
    std::vector<uint8_t> bytes = good;
    bytes[as_bytes(program.clear).size() + 6] = 0x7F;
    cases.push_back({"a segment without its marker", bytes, GFNT_ERR_CORRUPT,
        "does not start with 0x80"});
  }
  // A segment kind the format does not define - in the *second* header, because
  // breaking the first one stops the file being recognised as Type 1 at all,
  // which is a different and also correct answer.
  {
    std::vector<uint8_t> bytes = good;
    bytes[as_bytes(program.clear).size() + 7] = 0x09;
    cases.push_back({"an undefined segment kind", bytes, GFNT_ERR_CORRUPT,
        "a kind the format does not define"});
  }
  // A header cut short.
  cases.push_back({"a header cut short",
      std::vector<uint8_t>(good.begin(), good.begin() + 4), GFNT_ERR_CORRUPT,
      "header cut short"});
  // A length longer than the file.
  {
    std::vector<uint8_t> bytes = good;
    bytes[2] = 0xFF;
    bytes[3] = 0xFF;
    cases.push_back({"a segment longer than the file", bytes, GFNT_ERR_CORRUPT,
        "longer than the file"});
  }
  // No binary segment at all, so no private portion.
  {
    std::vector<uint8_t> bytes;
    const std::vector<uint8_t> text = as_bytes(program.clear);

    bytes.push_back(0x80);
    bytes.push_back(1);
    for (int shift = 0; shift < 32; shift += 8) {
      bytes.push_back(static_cast<uint8_t>(text.size() >> shift));
    }
    bytes.insert(bytes.end(), text.begin(), text.end());
    bytes.push_back(0x80);
    bytes.push_back(3);
    cases.push_back({"no binary segment", bytes, GFNT_ERR_CORRUPT,
        "no binary segment"});
  }
  // A PostScript program with no eexec in it.
  cases.push_back({"no eexec", as_bytes("%!PS-AdobeFont-1.0\n/FontName /X def\n"),
      GFNT_ERR_FORMAT, "no eexec"});
  // An odd number of hex digits.
  cases.push_back({"odd hex",
      as_bytes("%!PS\ncurrentfile eexec\nabcde\n"), GFNT_ERR_CORRUPT,
      "odd number of hex digits"});
  // Nothing after eexec but its four random bytes.
  cases.push_back({"nothing after eexec",
      as_bytes("%!PS\ncurrentfile eexec\n"), GFNT_ERR_CORRUPT,
      "nothing after eexec"});

  for (const Case & item : cases) {
    FromBytes font(item.bytes);
    EXPECT_EQ(font.result, item.result) << item.what;
    EXPECT_EQ(font.face, nullptr) << item.what;
    if (font.error.message) {
      EXPECT_NE(std::string(font.error.message).find(item.says),
          std::string::npos) << item.what << ": " << font.error.message;
    }
  }
}

TEST(Type1, EveryShapeOfBrokenPostScriptIsRefusedByName) {
  struct Case {
    const char * what;
    const char * clear;
    const char * privately;
    bool charstrings;
    const char * says;
  };
  const std::vector<Case> cases = {
      {"a /FontMatrix that is not an array",
       "%!PS\n/FontMatrix 0.001 def\ncurrentfile eexec\n", nullptr, true,
       "not an array"},
      {"a /FontMatrix of the wrong length",
       "%!PS\n/FontMatrix [0.001 0 0] readonly def\ncurrentfile eexec\n",
       nullptr, true, "not six numbers"},
      {"a /FontMatrix holding something else",
       "%!PS\n/FontMatrix [0.001 0 0 /x 0 0] def\ncurrentfile eexec\n",
       nullptr, true, "not a number"},
      {"no /CharStrings at all", nullptr,
       "dup /Private 8 dict dup begin\n/lenIV 4 def\n", false,
       "no /CharStrings"},
      {"a /CharStrings entry with no length", nullptr,
       "dup /Private begin\n2 index /CharStrings 1 dict dup begin\n/A RD x ND\n",
       false, "no length"},
      {"a /CharStrings entry that is not RD", nullptr,
       "dup /Private begin\n2 index /CharStrings 1 dict dup begin\n/A 2 XX xx ND\n",
       false, "is not `/<name> <length> RD <bytes>`"},
      // Two different refusals, and the boundary between them is worth having
      // both of: a length larger than the whole program is not a length at all,
      // while one that fits the program and not the bytes left is a string that
      // runs off the end.
      {"a binary length larger than the program", nullptr,
       "dup /Private begin\n2 index /CharStrings 1 dict dup begin\n/A 9999 RD x\n",
       false, "not a length"},
      {"a binary string past the end of the program", nullptr,
       "dup /Private begin\n2 index /CharStrings 1 dict dup begin\n/A 60 RD x\n",
       false, "past the end of the program"},
      {"a /Subrs without a count", nullptr,
       "dup /Private begin\n/Subrs array\n", false, "without a count"},
      {"a /Subrs entry that is malformed", nullptr,
       "dup /Private begin\n/Subrs 1 array\ndup 0 XX RD x NP\n", false,
       "is not `dup <index> <length> RD <bytes>`"},
      {"more subroutines than the reader allocates for", nullptr,
       "dup /Private begin\n/Subrs 99999999 array\n", false,
       "more subroutines than this reader allocates for"},
      {"a subroutine number the reader does not allocate for", nullptr,
       "dup /Private begin\n/Subrs 1 array\ndup 99999999 2 RD xx NP\n", false,
       "a subroutine number this reader does not allocate for"},
      {"an /Encoding with nothing after it",
       "%!PS\n/FontName /Probe def\ncurrentfile eexec\n",
       "dup /Private begin\n/Encoding", false, "nothing after it"},
      {"an /Encoding entry that is not dup code /name put",
       "%!PS\n/FontName /Probe def\n/Encoding 256 array\ndup /x 3 put\n"
       "currentfile eexec\n", nullptr, true,
       "not `dup <code> /<name> put`"},
      {"a /FontMatrix with a translation, written with an exponent",
       "%!PS\n/FontName /Probe def\n"
       "/FontMatrix [1e-3 0 0 1e-3 1e2 0] readonly def\n"
       "currentdict end\ncurrentfile eexec\n", nullptr, true, nullptr},
  };
  for (const Case & item : cases) {
    Program program;

    if (item.clear) {
      program.clear = item.clear;
    }
    if (item.privately) {
      program.private_part = item.privately;
    }
    program.with_charstrings = item.charstrings;
    FromBytes font(program.pfb());
    if (!item.says) {
      // A program the *parse* accepts and that is refused where it is used: a
      // FontMatrix with a translation states no em, so the face exists and its
      // glyphs do not draw.
      ASSERT_EQ(font.result, GFNT_OK) << item.what;
      uint16_t upem = 0;
      EXPECT_EQ(gfnt_face_units_per_em(font.face, &upem, nullptr),
          GFNT_ERR_UNSUPPORTED) << item.what;
      continue;
    }
    EXPECT_NE(font.result, GFNT_OK) << item.what;
    EXPECT_EQ(font.face, nullptr) << item.what;
    if (font.error.message) {
      EXPECT_NE(std::string(font.error.message).find(item.says),
          std::string::npos) << item.what << ": " << font.error.message;
    }
  }
}

TEST(Type1, NotdefIsMovedToZeroWhereverTheDictionaryPutIt) {
  // A Type 1 font may define `.notdef` anywhere in its dictionary, and many put
  // it last. Glyph 0 means "no glyph" everywhere else in this library, so it is
  // moved - and the swap is a branch a font with `.notdef` first never takes.
  Program program;
  program.private_part =
      "dup /Private 8 dict dup begin\n/lenIV 4 def\n";
  // Two glyphs, `A` first and `.notdef` second.
  const std::vector<uint8_t> cipher = encrypt(program.charstring, 4330u, 4);
  char head[160];
  std::string body = program.private_part;
  snprintf(head, sizeof head,
      "2 index /CharStrings 2 dict dup begin\n/A %zu RD ", cipher.size());
  body += head;
  body.append(reinterpret_cast<const char *>(cipher.data()), cipher.size());
  snprintf(head, sizeof head, " ND\n/.notdef %zu RD ", cipher.size());
  body += head;
  body.append(reinterpret_cast<const char *>(cipher.data()), cipher.size());
  body += " ND\nend\nend\n";
  program.private_part = body;
  program.with_charstrings = false;

  FromBytes font(program.pfb());
  ASSERT_EQ(font.result, GFNT_OK)
      << (font.error.message ? font.error.message : "");
  EXPECT_EQ(name_of(font.face, 0), ".notdef");
  EXPECT_EQ(name_of(font.face, 1), "A");
}

TEST(Type1, AMatrixThatStatesNoEmIsRefusedByName) {
  Program program;
  program.clear =
      "%!PS\n/FontName /Probe def\n"
      "/FontMatrix [0.001 0 0 0.002 0 0] readonly def\n"
      "currentdict end\ncurrentfile eexec\n";
  FromBytes font(program.pfb());
  ASSERT_EQ(font.result, GFNT_OK)
      << (font.error.message ? font.error.message : "");

  uint16_t upem = 0;
  EXPECT_EQ(gfnt_face_units_per_em(font.face, &upem, nullptr),
      GFNT_ERR_UNSUPPORTED);
  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_outline(font.face, 0, nullptr, nullptr, &outline,
      &error), GFNT_ERR_UNSUPPORTED);
  names(error, "does not reduce to an em");
  gfnt_outline_destroy(outline);
}

TEST(Type1, AMatrixWrittenWithAnExponentIsReadAsTheSameNumber) {
  // PostScript numbers can carry an exponent, and no fixture states one. `1e-3`
  // is `0.001`, so this font's em is 1000 and its glyph draws where the other
  // probe's does.
  Program program;
  program.clear =
      "%!PS\n/FontName /Probe def\n"
      "/FontMatrix [1e-3 0 0 1E-3 0 0] readonly def\n"
      "currentdict end\ncurrentfile eexec\n";
  FromBytes font(program.pfb());
  ASSERT_EQ(font.result, GFNT_OK)
      << (font.error.message ? font.error.message : "");

  uint16_t upem = 0;
  ASSERT_EQ(gfnt_face_units_per_em(font.face, &upem, nullptr), GFNT_OK);
  EXPECT_EQ(upem, 1000);
}

TEST(Type1, TheAccessorsRefuseWhatTheyCannotAnswer) {
  Fixture font("type1.pfb");
  ASSERT_EQ(font.result, GFNT_OK);
  const GFNT_Type1 * type1 = nullptr;
  ASSERT_EQ(gfnt_face_type1(font, &type1, nullptr), GFNT_OK);

  const uint8_t * bytes = nullptr;
  size_t length = 0;
  EXPECT_EQ(gfnt_type1_program(type1, 99, &bytes, &length), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_type1_program(nullptr, 0, &bytes, &length), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_type1_program(type1, 0, nullptr, &length), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_type1_subr(nullptr, 0, &bytes, &length), GFNT_ERR_CORRUPT);
  EXPECT_FALSE(gfnt_type1_units_per_em(nullptr, nullptr));
  EXPECT_EQ(gfnt_type1_glyph_for_name(type1, nullptr, "A", 1), GFNT_GLYPH_NONE);

  // A name and the two-pass contract around it.
  size_t needed = 0;
  ASSERT_EQ(gfnt_type1_name_at(font, 4, nullptr, 0, &needed, nullptr), GFNT_OK);
  EXPECT_EQ(needed, strlen("Aacute"));
  char small[3];
  GFNT_Error error{};
  EXPECT_EQ(gfnt_type1_name_at(font, 4, small, sizeof small, &needed, &error),
      GFNT_ERR_LIMIT);
  names(error, "longer than the caller's buffer");
  EXPECT_EQ(gfnt_type1_name_at(font, 99, nullptr, 0, &needed, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_type1_name_at(font, 0, nullptr, 0, nullptr, nullptr),
      GFNT_ERR_INVALID);

  GFNT_CharstringMetrics metrics{};
  EXPECT_EQ(gfnt_type1_metrics(font, 0, nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_type1_metrics(font, 99, &metrics, nullptr), GFNT_ERR_INVALID);
  bool composite = false;
  EXPECT_EQ(gfnt_type1_is_composite(font, 0, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_type1_is_composite(font, 99, &composite, nullptr),
      GFNT_ERR_INVALID);
}

TEST(Type1, AnAllocationFailureAnywhereInTheDeriveOrParseIsReported) {
  // Deriving allocates twice (the two halves) and a blob, and the parse allocates
  // the glyph array, the subroutine array and the plaintext arena. None of those
  // arms is reachable from an allocator that works.
  // The *big* fixture, and a wide sweep: it is the one whose arrays grow, so its
  // load allocates more times than the small one and each of those is an arm
  // that carries a refusal out of a different place.
  const std::vector<uint8_t> file = bytes_of("type1-big.pfb");
  ASSERT_FALSE(file.empty());

  size_t refused = 0;
  // A wide sweep: the big fixture's load allocates the two halves of the derived
  // program, a blob, the glyph array and its doublings, the subroutine array and
  // its growths, and the plaintext arena - and each realloc is a separate arm.
  // The count is the number of allocations a load makes, found by widening it
  // until the refusals stopped increasing.
  for (size_t fail_at = 0; fail_at < 150; ++fail_at) {
    gfnttest::FailingAllocator allocator(fail_at);
    GFNT_Blob * blob = nullptr;
    ASSERT_EQ(gfnt_blob_create_memory(file.data(), file.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
    GFNT_Face * face = nullptr;
    GFNT_Error error{};
    const GFNT_Result result = gfnt_face_load(blob, 0, nullptr,
        allocator.get(), &face, &error);

    if (result != GFNT_OK) {
      ++refused;
      EXPECT_EQ(result, GFNT_ERR_OOM) << "allocation " << fail_at;
      EXPECT_EQ(face, nullptr) << "allocation " << fail_at;
    }
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  EXPECT_GT(refused, 8u) << "the load has to allocate for this to mean anything";
}

TEST(Type1, TheFramingsThisAcceptsBeyondTheTwoFixtures) {
  // Shapes a writer produces that no committed fixture has, each of which the
  // reader has a branch for.
  Program program;

  // A PFB whose first segment is the binary one. Nothing forbids it, and the
  // probe has to accept `0x80 0x02` as well as `0x80 0x01`.
  {
    const std::vector<uint8_t> encrypted =
        encrypt(as_bytes("nothing here"), 55665u, 4);
    std::vector<uint8_t> bytes;

    bytes.push_back(0x80);
    bytes.push_back(2);
    for (int shift = 0; shift < 32; shift += 8) {
      bytes.push_back(static_cast<uint8_t>(encrypted.size() >> shift));
    }
    bytes.insert(bytes.end(), encrypted.begin(), encrypted.end());
    bytes.push_back(0x80);
    bytes.push_back(3);
    FromBytes font(bytes);
    // It is a Type 1 file, and it has no glyphs - which is a different answer
    // from "not a font", and the one a caller can act on.
    EXPECT_EQ(font.result, GFNT_ERR_CORRUPT);
    if (font.error.message) {
      EXPECT_NE(std::string(font.error.message).find("no /CharStrings"),
          std::string::npos) << font.error.message;
    }
  }

  // A PostScript program whose private portion is *binary* rather than hex.
  // Adobe's rule is the four bytes after `eexec`, and a writer that emits binary
  // normally emits PFB - but nothing stops this, and the branch exists.
  {
    std::vector<uint8_t> bytes = as_bytes(
        "%!PS-AdobeFont-1.0: Probe\n/FontName /Probe def\n"
        "currentdict end\ncurrentfile eexec\n");
    std::string body = program.private_part;
    const std::vector<uint8_t> cipher = encrypt(program.charstring, 4330u, 4);
    char head[128];

    snprintf(head, sizeof head,
        "2 index /CharStrings 1 dict dup begin\n/.notdef %zu RD ",
        cipher.size());
    body += head;
    body.append(reinterpret_cast<const char *>(cipher.data()), cipher.size());
    body += " ND\nend\nend\n";
    const std::vector<uint8_t> encrypted = encrypt(as_bytes(body), 55665u, 4);

    // The four bytes after `eexec` decide, and this ciphertext's first four are
    // not four hex digits - so it is read as binary. If they had been, this is
    // the input Adobe's own rule cannot tell apart from hex, and the fixture
    // would be ambiguous rather than wrong.
    ASSERT_FALSE(encrypted.size() >= 4
        && isxdigit(encrypted[0]) && isxdigit(encrypted[1])
        && isxdigit(encrypted[2]) && isxdigit(encrypted[3]))
        << "this input is ambiguous under the format's own rule";
    bytes.insert(bytes.end(), encrypted.begin(), encrypted.end());
    FromBytes font(bytes);
    ASSERT_EQ(font.result, GFNT_OK)
        << "a binary private portion outside PFB framing: "
        << (font.error.message ? font.error.message : "");
    size_t glyphs = 0;
    EXPECT_EQ(gfnt_face_num_glyphs(font.face, &glyphs, nullptr), GFNT_OK);
    EXPECT_EQ(glyphs, 1u);
  }

  // A PFA whose hex ends at a byte that is neither hex nor whitespace, with no
  // run of zeros to find first.
  {
    std::vector<uint8_t> bytes = as_bytes("%!PS\ncurrentfile eexec\n");
    const std::vector<uint8_t> encrypted =
        encrypt(as_bytes("dup /Private begin\n"), 55665u, 4);
    std::string hex;

    for (uint8_t byte : encrypted) {
      char pair[3];

      snprintf(pair, sizeof pair, "%02x", byte);
      hex += pair;
    }
    hex += "!cleartomark\n";
    const std::vector<uint8_t> tail = as_bytes(hex);
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    FromBytes font(bytes);
    // No /CharStrings in it, so it is refused for that - and not for an odd
    // digit count, which is what swallowing the `!` would have produced.
    EXPECT_EQ(font.result, GFNT_ERR_CORRUPT);
    if (font.error.message) {
      EXPECT_NE(std::string(font.error.message).find("no /CharStrings"),
          std::string::npos) << font.error.message;
    }
  }
}

TEST(Type1, AGlyphCapAppliesToTheDictionaryTheProgramDeclares) {
  Program program;
  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_glyphs = 0;

  const std::vector<uint8_t> file = program.pfb();
  GFNT_Blob * blob = nullptr;
  ASSERT_EQ(gfnt_blob_create_memory(file.data(), file.size(),
      GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  EXPECT_EQ(gfnt_face_load(blob, 0, &limits, nullptr, &face, &error),
      GFNT_ERR_LIMIT);
  names(error, "max_glyphs");
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
}

TEST(Type1, AnEncodingEntryNamingAGlyphTheFontLacksLeavesThatCodeUnmapped) {
  // `/Encoding` is resolved from names to glyph indices at parse time, and a
  // name the font never defines has no glyph - so that code maps to nothing
  // rather than to glyph 0, which is what an unresolved name would become if it
  // were left as a zero.
  Program program;
  program.clear =
      "%!PS\n/FontName /Probe def\n"
      "/FontMatrix [0.001 0 0 0.001 0 0] readonly def\n"
      "/Encoding 256 array\n"
      "0 1 255 {1 index exch /.notdef put} for\n"
      "dup 65 /nosuchglyph put\n"
      "dup 66 /.notdef put\n"
      "readonly def\n"
      "currentdict end\ncurrentfile eexec\n";
  FromBytes font(program.pfb());
  ASSERT_EQ(font.result, GFNT_OK)
      << (font.error.message ? font.error.message : "");
  const GFNT_Type1 * type1 = nullptr;
  ASSERT_EQ(gfnt_face_type1(font.face, &type1, nullptr), GFNT_OK);

  EXPECT_EQ(gfnt_type1_glyph_for_code(type1, 65), GFNT_GLYPH_NONE);
  EXPECT_EQ(gfnt_type1_glyph_for_code(type1, 66), 0u);
  EXPECT_EQ(gfnt_type1_glyph_for_code(type1, 67), GFNT_GLYPH_NONE);
}

TEST(Type1, TheDictionaryShapesAWriterVariesAndTheReaderAccepts) {
  // Each of these is a place where a writer's habit differs and the reader has to
  // find the entries anyway. None of them is malformed, and each `before` ends
  // where the `/CharStrings` entries begin.
  struct Case {
    const char * what;
    const char * before;
  };
  const std::vector<Case> cases = {
      // `/Subrs 2` with no `array`, and entries with no `NP` after their binary:
      // both are tokens this reader steps over, and either one left unconsumed
      // makes the loop stop and lose every entry after the first.
      {"a /Subrs with no array and no NP",
       "dup /Private begin\n/Subrs 2\ndup 0 5 RD ....."
       "dup 1 5 RD .....\n2 index /CharStrings 1 dict dup begin\n"},
      // A `/CharStrings` with no `dup begin` before its first entry.
      {"a /CharStrings with no begin",
       "dup /Private begin\n2 index /CharStrings 1 dict\n"},
      // An `/Encoding` array that simply stops, with no `readonly` or `def`.
      {"an /Encoding that ends without readonly",
       "dup /Private begin\n/Encoding 256 array\ndup 65 /.notdef put\n"
       "2 index /CharStrings 1 dict dup begin\n"},
  };
  for (const Case & item : cases) {
    Program program;
    const std::vector<uint8_t> cipher = encrypt(program.charstring, 4330u, 4);
    char head[64];
    std::string body = item.before;

    snprintf(head, sizeof head, "/.notdef %zu RD ", cipher.size());
    body += head;
    body.append(reinterpret_cast<const char *>(cipher.data()), cipher.size());
    body += " ND\nend\nend\n";
    program.private_part = body;
    program.with_charstrings = false;

    FromBytes font(program.pfb());
    ASSERT_EQ(font.result, GFNT_OK) << item.what << ": "
        << (font.error.message ? font.error.message : "");
    size_t glyphs = 0;
    EXPECT_EQ(gfnt_face_num_glyphs(font.face, &glyphs, nullptr), GFNT_OK)
        << item.what;
    EXPECT_EQ(glyphs, 1u) << item.what;
    EXPECT_EQ(name_of(font.face, 0), ".notdef") << item.what;
  }
}

TEST(Type1, TheArmsReachedOnlyByAProgramThatContradictsItself) {
  // A `/lenIV` larger than a charstring, a charstring of nothing at all, and a
  // PFB whose binary segment is shorter than the four bytes `eexec` opens with.
  // Each is a program that states one thing and contains another.
  {
    Program program;
    // lenIV 16, and a four-byte charstring: the padding alone is longer than the
    // whole entry.
    program.private_part = "dup /Private 8 dict dup begin\n/lenIV 16 def\n";
    FromBytes font(program.pfb());
    EXPECT_EQ(font.result, GFNT_ERR_CORRUPT);
    if (font.error.message) {
      EXPECT_NE(std::string(font.error.message).find("/lenIV"),
          std::string::npos) << font.error.message;
    }
  }
  {
    Program program;
    // A charstring of zero bytes with no padding to remove: the arena would have
    // nothing in it, which is a program with no program.
    program.private_part = "dup /Private 8 dict dup begin\n/lenIV 0 def\n";
    program.charstring.clear();
    program.pad = 0;
    FromBytes font(program.pfb());
    EXPECT_EQ(font.result, GFNT_ERR_CORRUPT);
    if (font.error.message) {
      EXPECT_NE(std::string(font.error.message).find("all empty"),
          std::string::npos) << font.error.message;
    }
  }
  {
    // A PFB whose binary segment holds fewer bytes than eexec's own padding.
    std::vector<uint8_t> bytes;
    const std::vector<uint8_t> text = as_bytes("%!PS\ncurrentfile eexec\n");
    const uint8_t stub[3] = {0xAA, 0xBB, 0xCC};

    bytes.push_back(0x80);
    bytes.push_back(1);
    for (int shift = 0; shift < 32; shift += 8) {
      bytes.push_back(static_cast<uint8_t>(text.size() >> shift));
    }
    bytes.insert(bytes.end(), text.begin(), text.end());
    bytes.push_back(0x80);
    bytes.push_back(2);
    bytes.push_back(sizeof stub);
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.insert(bytes.end(), stub, stub + sizeof stub);
    bytes.push_back(0x80);
    bytes.push_back(3);

    FromBytes font(bytes);
    EXPECT_EQ(font.result, GFNT_ERR_CORRUPT);
    if (font.error.message) {
      EXPECT_NE(std::string(font.error.message).find("no plaintext"),
          std::string::npos) << font.error.message;
    }
  }
}

TEST(Type1, TheInternalAccessorsAnswerForAFaceThatIsNotType1) {
  // `gfnt_type1_glyph_bound` is asked of every face whose `maxp` is missing, so
  // it has to answer "not mine" for a CFF rather than parsing one as PostScript.
  Fixture cff("bare.cff");
  ASSERT_EQ(cff.result, GFNT_OK);

  size_t bound = 99;
  EXPECT_FALSE(gfnt_type1_glyph_bound(cff, &bound));
  EXPECT_FALSE(gfnt_type1_glyph_bound(nullptr, &bound));
  EXPECT_EQ(bound, 99u) << "it must not write through the pointer";

  // And releasing nothing is not a crash, which is what a cache teardown does on
  // a face whose Type 1 parse never ran.
  gfnt_type1_release(nullptr, nullptr);
  GFNT_Type1 empty{};
  gfnt_type1_release(gfnt_allocator_default(), &empty);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
