/**
 * @file
 *
 * Adobe Font Metrics files (afm.h): a file with every section the library offers, the
 * lookups, the sections it reads past, and each refusal. The parse of real files
 * (the fourteen standard fonts' metrics) is the oracle driver's, not a unit test's.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstdint>
#include <cstring>
#include <string>

#include <ghoti.io/font/afm.h>

namespace {

const char kGood[] =
    "StartFontMetrics 4.1\n"
    "Comment A comment, which is ignored\n"
    "FontName Sample-Roman\n"
    "FullName Sample Roman\n"
    "FamilyName Sample\n"
    "Weight Roman\n"
    "ItalicAngle -12.5\n"
    "IsFixedPitch false\n"
    "UnderlinePosition -100\n"
    "UnderlineThickness 50\n"
    "Version 001.002\n"
    "Notice (c) Somebody; all rights reserved\n"
    "EncodingScheme AdobeStandardEncoding\n"
    "FontBBox -168 -218 1000 898\n"
    "CapHeight 662\n"
    "XHeight 450\n"
    "Ascender 683\n"
    "Descender -217\n"
    "StdHW 36\n"
    "StdVW 84\n"
    "Characters 4\n"
    "StartCharMetrics 4\n"
    "C 32 ; WX 250 ; N space ; B 0 0 0 0 ;\n"
    "C 102 ; WX 333.5 ; N f ; B 20 0 371 683 ; L i fi ; L l fl ;\n"
    "C -1 ; WX 556 ; N fi ; B 14 0 525 683 ;\n"
    "CH <0041> ; WX 722 ; N A ; B 15 0 706 674 ;\n"
    "EndCharMetrics\n"
    "StartKernData\n"
    "StartTrackKern 1\n"
    "TrackKern -1 6 0 6 -1\n"
    "EndTrackKern\n"
    "StartKernPairs 4\n"
    "KPX A f -50\n"
    "KPX A A -25.5\n"
    "KP f A -10 3\n"
    "KPY A f 7\n"
    "EndKernPairs\n"
    "EndKernData\n"
    "StartComposites 1\n"
    "CC Aacute 2 ; PCC A 0 0 ; PCC acute 195 212 ;\n"
    "EndComposites\n"
    "EndFontMetrics\n";

struct Parsed {
  GFNT_Afm * afm = nullptr;
  GFNT_Result result = GFNT_ERR_INTERNAL;
  GFNT_Error error;

  explicit Parsed(const std::string & text, const GFNT_Limits * limits = nullptr) {
    gfnt_error_clear(&error);
    result = gfnt_afm_parse(text.data(), text.size(), limits, nullptr, &afm, &error);
  }
  Parsed(const Parsed &) = delete;
  Parsed & operator=(const Parsed &) = delete;
  ~Parsed() { gfnt_afm_free(afm); }
};

std::string with(const std::string & from, const std::string & to) {
  std::string text = kGood;
  const size_t at = text.find(from);
  EXPECT_NE(at, std::string::npos) << from;
  text.replace(at, from.size(), to);
  return text;
}

std::string crlf(std::string text) {
  std::string out;
  for (char c : text) {
    if (c == '\n') {
      out += "\r\n";
    }
    else {
      out += c;
    }
  }
  return out;
}

}  // namespace

TEST(Afm, TheHeaderKeysAreRead) {
  Parsed p(kGood);
  ASSERT_EQ(p.result, GFNT_OK) << p.error.message;
  const GFNT_AfmInfo info = gfnt_afm_info(p.afm);
  EXPECT_STREQ(info.font_name, "Sample-Roman");
  EXPECT_STREQ(info.full_name, "Sample Roman");
  EXPECT_STREQ(info.family_name, "Sample");
  EXPECT_STREQ(info.weight, "Roman");
  EXPECT_STREQ(info.version, "001.002");
  EXPECT_STREQ(info.notice, "(c) Somebody; all rights reserved");
  EXPECT_STREQ(info.encoding_scheme, "AdobeStandardEncoding");
  EXPECT_EQ(info.character_set, nullptr);
  EXPECT_EQ(info.italic_angle_milli, -12500);
  EXPECT_FALSE(info.is_fixed_pitch);
  EXPECT_EQ(info.underline_position_milli, -100000);
  EXPECT_EQ(info.underline_thickness_milli, 50000);
  EXPECT_TRUE(info.has_bbox);
  EXPECT_EQ(info.bbox_milli[0], -168000);
  EXPECT_EQ(info.bbox_milli[3], 898000);
  EXPECT_EQ(info.cap_height_milli, 662000);
  EXPECT_EQ(info.x_height_milli, 450000);
  EXPECT_EQ(info.ascender_milli, 683000);
  EXPECT_EQ(info.descender_milli, -217000);
  EXPECT_EQ(info.std_hw_milli, 36000);
  EXPECT_EQ(info.std_vw_milli, 84000);
}

TEST(Afm, TheCharactersAreReadInFileOrderWithEveryCommand) {
  Parsed p(kGood);
  ASSERT_EQ(p.result, GFNT_OK) << p.error.message;
  ASSERT_EQ(gfnt_afm_glyph_count(p.afm), 4u);
  GFNT_AfmGlyph g;
  ASSERT_EQ(gfnt_afm_glyph_at(p.afm, 1, &g), GFNT_OK);
  EXPECT_EQ(g.code, 102);
  EXPECT_EQ(g.width_x_milli, 333500);
  EXPECT_EQ(g.width_y_milli, 0);
  EXPECT_STREQ(g.name, "f");
  EXPECT_TRUE(g.has_bbox);
  EXPECT_EQ(g.bbox_milli[2], 371000);
  EXPECT_STREQ(g.ligatures, "i fi l fl");
  ASSERT_EQ(gfnt_afm_glyph_at(p.afm, 2, &g), GFNT_OK);
  EXPECT_EQ(g.code, -1);
  EXPECT_EQ(g.ligatures, nullptr);
  ASSERT_EQ(gfnt_afm_glyph_at(p.afm, 3, &g), GFNT_OK);
  EXPECT_EQ(g.code, 0x41);   // CH <0041>
  EXPECT_EQ(gfnt_afm_glyph_at(p.afm, 4, &g), GFNT_ERR_INVALID);
}

TEST(Afm, LookupsByNameAndByCode) {
  Parsed p(kGood);
  ASSERT_EQ(p.result, GFNT_OK) << p.error.message;
  GFNT_AfmGlyph g;
  ASSERT_TRUE(gfnt_afm_glyph_for_name(p.afm, "fi", &g));
  EXPECT_EQ(g.width_x_milli, 556000);
  EXPECT_EQ(g.code, -1);
  EXPECT_FALSE(gfnt_afm_glyph_for_name(p.afm, "nosuch", &g));
  EXPECT_FALSE(gfnt_afm_glyph_for_name(p.afm, "", &g));
  ASSERT_TRUE(gfnt_afm_glyph_for_code(p.afm, 32, &g));
  EXPECT_STREQ(g.name, "space");
  EXPECT_FALSE(gfnt_afm_glyph_for_code(p.afm, -1, &g));   // unencoded glyphs have no code
  EXPECT_FALSE(gfnt_afm_glyph_for_code(p.afm, 256, &g));
  EXPECT_FALSE(gfnt_afm_glyph_for_code(p.afm, 33, &g));
}

TEST(Afm, KerningPairsAndWhatIsNotOneToThem) {
  Parsed p(kGood);
  ASSERT_EQ(p.result, GFNT_OK) << p.error.message;
  int32_t x = 0;
  EXPECT_EQ(gfnt_afm_kern_count(p.afm), 3u);   // KPX, KPX and KP; the KPY is counted and not kept
  ASSERT_TRUE(gfnt_afm_kern(p.afm, "A", "f", &x));
  EXPECT_EQ(x, -50000);
  ASSERT_TRUE(gfnt_afm_kern(p.afm, "A", "A", &x));
  EXPECT_EQ(x, -25500);
  ASSERT_TRUE(gfnt_afm_kern(p.afm, "f", "A", &x));
  EXPECT_EQ(x, -10000);   // the x of a KP
  x = 99;
  EXPECT_FALSE(gfnt_afm_kern(p.afm, "f", "f", &x));
  EXPECT_EQ(x, 99);
}

TEST(Afm, ACarriageReturnAndLineFeedPairEndsALineLikeEitherAlone) {
  Parsed crlf_file(crlf(kGood));
  ASSERT_EQ(crlf_file.result, GFNT_OK) << crlf_file.error.message;
  EXPECT_EQ(gfnt_afm_glyph_count(crlf_file.afm), 4u);
  std::string lone = kGood;
  for (char & c : lone) {
    if (c == '\n') {
      c = '\r';
    }
  }
  Parsed cr_file(lone);
  ASSERT_EQ(cr_file.result, GFNT_OK) << cr_file.error.message;
  EXPECT_EQ(gfnt_afm_kern_count(cr_file.afm), 3u);
}

TEST(Afm, VerticalAndOtherSectionsAreReadPastNotOffered) {
  const std::string text = with("StartCharMetrics 4",
      "StartDirection 1\nStartCharMetrics 1\nC 32 ; WX 999 ; N vertical ; B 0 0 0 0 ;\nEndCharMetrics\n"
      "EndDirection\nStartCharMetrics 4");
  Parsed p(text);
  ASSERT_EQ(p.result, GFNT_OK) << p.error.message;
  GFNT_AfmGlyph g;
  EXPECT_FALSE(gfnt_afm_glyph_for_name(p.afm, "vertical", &g));
  EXPECT_EQ(gfnt_afm_glyph_count(p.afm), 4u);
}

TEST(Afm, AnAdvanceVectorAndTheOtherSpellingsOfAnAdvance) {
  Parsed p(with("C 32 ; WX 250 ;", "C 32 ; W0X 250 ; W0Y 3 ;"));
  ASSERT_EQ(p.result, GFNT_OK) << p.error.message;
  GFNT_AfmGlyph g;
  ASSERT_EQ(gfnt_afm_glyph_at(p.afm, 0, &g), GFNT_OK);
  EXPECT_EQ(g.width_x_milli, 250000);
  EXPECT_EQ(g.width_y_milli, 3000);
  Parsed v(with("C 32 ; WX 250 ;", "C 32 ; W 250 7 ;"));
  ASSERT_EQ(v.result, GFNT_OK) << v.error.message;
  ASSERT_EQ(gfnt_afm_glyph_at(v.afm, 0, &g), GFNT_OK);
  EXPECT_EQ(g.width_y_milli, 7000);
}

TEST(Afm, ANumberIsTheSameUnderEveryLocale) {
  // The parser does not call strtod, so a locale that writes a comma for a point cannot move
  // it: the number is read by hand. A comma is not a number here.
  Parsed ok(with("333.5", "333.5"));
  ASSERT_EQ(ok.result, GFNT_OK);
  Parsed comma(with("333.5", "333,5"));
  EXPECT_EQ(comma.result, GFNT_ERR_CORRUPT);
}

TEST(Afm, NumbersAreThousandthsRoundedHalfAwayFromZeroAndTooLargeOnesAreRefused) {
  struct Case { const char * text; int32_t milli; };
  const Case cases[] = {{"0.5", 500}, {"12.3456", 12346}, {"12.3454", 12345}, {"-12.3455", -12346},
      {"0.0005", 1}, {"0.0004", 0}, {"-0.0005", -1}, {"7", 7000}, {"+7.25", 7250}, {"1234.", 1234000},
      {".5", 500}, {"2147483.647", 2147483647}};
  for (const Case & c : cases) {
    Parsed p(with("WX 250", std::string("WX ") + c.text));
    ASSERT_EQ(p.result, GFNT_OK) << c.text << ": " << p.error.message;
    GFNT_AfmGlyph g;
    ASSERT_EQ(gfnt_afm_glyph_at(p.afm, 0, &g), GFNT_OK);
    EXPECT_EQ(g.width_x_milli, c.milli) << c.text;
  }
  EXPECT_EQ(Parsed(with("WX 250", "WX 2147483.648")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("WX 250", "WX 9999999999999")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("WX 250", "WX .")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("WX 250", "WX 1e3")).result, GFNT_ERR_CORRUPT);
}

TEST(Afm, EachRefusal) {
  EXPECT_EQ(Parsed("").result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed("FontName X\nEndFontMetrics\n").result, GFNT_ERR_CORRUPT);   // no start
  EXPECT_EQ(Parsed(with("EndFontMetrics\n", "")).result, GFNT_ERR_CORRUPT);     // no end
  EXPECT_EQ(Parsed(with("StartCharMetrics 4", "StartCharMetrics 5")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("StartCharMetrics 4", "StartCharMetrics 3")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("StartKernPairs 4", "StartKernPairs 3")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("StartKernPairs 4", "StartKernPairs 9")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("WX 250", "WX 2x0")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("C 32 ;", "C thirty ;")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("N space", "N sp ace")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("B 0 0 0 0", "B 0 0 0")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("L i fi", "L i")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("ItalicAngle -12.5", "ItalicAngle steep")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("IsFixedPitch false", "IsFixedPitch maybe")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("FontBBox -168 -218 1000 898", "FontBBox 1 2 3")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("KPX A f -50", "KPX A f")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("EndCharMetrics\n", "")).result, GFNT_ERR_CORRUPT);     // section never closed
  EXPECT_EQ(Parsed(with("EndTrackKern\n", "")).result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("StartKernData\n", "StartFontMetrics 4.1\nStartKernData\n")).result,
      GFNT_ERR_CORRUPT);
  EXPECT_EQ(Parsed(with("StartKernData\n", "StartCharMetrics 0\nEndCharMetrics\nStartKernData\n")).result,
      GFNT_ERR_CORRUPT);   // a second section of characters
  std::string nul = kGood;
  nul[40] = '\0';
  EXPECT_EQ(Parsed(nul).result, GFNT_ERR_CORRUPT);
}

TEST(Afm, TheLimitsApply) {
  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_glyphs = 3;
  EXPECT_EQ(Parsed(kGood, &limits).result, GFNT_ERR_LIMIT);
  gfnt_limits_default(&limits);
  limits.max_line_length = 20;
  EXPECT_EQ(Parsed(kGood, &limits).result, GFNT_ERR_LIMIT);
  gfnt_limits_default(&limits);
  limits.max_blob_bytes = 100;
  EXPECT_EQ(Parsed(kGood, &limits).result, GFNT_ERR_LIMIT);
  // A count the file cannot hold is refused before anything is allocated for it.
  EXPECT_EQ(Parsed(with("StartKernPairs 4", "StartKernPairs 2000000")).result, GFNT_ERR_LIMIT);
}

TEST(Afm, NullArgumentsAndAnEmptyAfmAnswerNothing) {
  GFNT_Afm * afm = nullptr;
  EXPECT_EQ(gfnt_afm_parse(kGood, sizeof kGood - 1, nullptr, nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_afm_parse(nullptr, 5, nullptr, nullptr, &afm, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(afm, nullptr);
  GFNT_AfmGlyph g;
  int32_t x;
  EXPECT_EQ(gfnt_afm_glyph_count(nullptr), 0u);
  EXPECT_EQ(gfnt_afm_glyph_at(nullptr, 0, &g), GFNT_ERR_INVALID);
  EXPECT_FALSE(gfnt_afm_glyph_for_name(nullptr, "A", &g));
  EXPECT_FALSE(gfnt_afm_glyph_for_code(nullptr, 65, &g));
  EXPECT_FALSE(gfnt_afm_kern(nullptr, "A", "V", &x));
  EXPECT_EQ(gfnt_afm_kern_count(nullptr), 0u);
  EXPECT_EQ(gfnt_afm_info(nullptr).font_name, nullptr);
  gfnt_afm_free(nullptr);
  Parsed empty("StartFontMetrics 4.1\nEndFontMetrics\n");
  ASSERT_EQ(empty.result, GFNT_OK);
  EXPECT_EQ(gfnt_afm_glyph_count(empty.afm), 0u);
  EXPECT_FALSE(gfnt_afm_glyph_for_name(empty.afm, "A", &g));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
