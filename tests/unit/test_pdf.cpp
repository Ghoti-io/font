/**
 * @file
 *
 * The `W` array and the `ToUnicode` CMap (pdf.h): the exact text for faces whose
 * advances are chosen here, the rounding at the thousandth, the run and range
 * forms, the PDF specification's own limits, and every refusal.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/pdf.h>

namespace {

struct Face {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  Face(uint16_t upem, const std::vector<uint16_t> & advances) {
    std::vector<std::pair<uint16_t, int16_t>> metrics;
    for (uint16_t a : advances) {
      metrics.push_back({a, 0});
    }
    const uint16_t n = static_cast<uint16_t>(advances.size());
    bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {
        {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(upem, 0)},
        {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, n)},
        {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics)},
        {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(n)},
    });
    GFNT_Error error;
    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED, nullptr,
        nullptr, &blob, &error), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
  }
  Face(const Face &) = delete;
  Face & operator=(const Face &) = delete;
  ~Face() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
};

std::string take(GFNT_Blob * blob) {
  std::string text(reinterpret_cast<const char *>(gfnt_blob_data(blob)), gfnt_blob_size(blob));
  gfnt_blob_destroy(blob);
  return text;
}

std::string widths(const Face & f, const std::vector<uint32_t> & glyphs) {
  GFNT_Blob * blob = nullptr;
  GFNT_Error error;
  gfnt_error_clear(&error);
  EXPECT_EQ(gfnt_pdf_widths(f.face, glyphs.data(), glyphs.size(), nullptr, nullptr, nullptr, &blob,
      &error), GFNT_OK);
  return blob ? take(blob) : "";
}

GFNT_Result widths_result(const Face & f, const std::vector<uint32_t> & glyphs) {
  GFNT_Blob * blob = nullptr;
  GFNT_Result r = gfnt_pdf_widths(f.face, glyphs.data(), glyphs.size(), nullptr, nullptr, nullptr,
      &blob, nullptr);
  gfnt_blob_destroy(blob);
  return r;
}

using Entry = std::pair<uint32_t, std::vector<uint32_t>>;

GFNT_Result tounicode(const std::vector<Entry> & in, unsigned bytes, std::string * out) {
  std::vector<GFNT_PdfToUnicodeEntry> entries;
  for (const Entry & e : in) {
    entries.push_back({e.first, e.second.data(), e.second.size()});
  }
  GFNT_Blob * blob = nullptr;
  GFNT_Result r = gfnt_pdf_tounicode(entries.data(), entries.size(), bytes, nullptr, nullptr, &blob,
      nullptr);
  if (r == GFNT_OK && out) {
    *out = take(blob);
  }
  else {
    EXPECT_EQ(blob, nullptr);
  }
  return r;
}

const std::string kHead =
    "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
    "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
    "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n";
const std::string kTail = "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";

}  // namespace

TEST(PdfWidths, ARunOfOneWidthIsARangeAndTheRestAreLists) {
  Face f(1000, {500, 500, 500, 500, 500, 500, 600, 700, 250, 250, 250});
  EXPECT_EQ(widths(f, {1, 2, 3, 4, 5}), "1 5 500");
  EXPECT_EQ(widths(f, {1, 2, 3, 4, 5, 6}), "1 5 500 6 [600]");
  EXPECT_EQ(widths(f, {6, 7, 8}), "6 [600 700 250]");
  EXPECT_EQ(widths(f, {0, 1, 2, 3, 6, 7, 8, 9, 10}), "0 3 500 6 [600 700 250 250 250]");
  EXPECT_EQ(widths(f, {8, 9, 10}), "8 [250 250 250]");
  EXPECT_EQ(widths(f, {0, 2}), "0 [500] 2 [500]");
}

TEST(PdfWidths, GlyphsComeInAnyOrderAndRepeat) {
  Face f(1000, {500, 600, 700, 800});
  EXPECT_EQ(widths(f, {3, 1, 2, 1, 0, 3}), "0 [500 600 700 800]");
}

TEST(PdfWidths, AFaceOf2048UnitsIsScaledToTheThousandthAndRoundedHalfAwayFromZero) {
  // advance * 1000 / 2048, exactly: 1024 -> 500, 1025 -> 500.488..., 2 -> 0.9765625 * 8 = 0.976..
  Face f(2048, {1024, 1025, 2, 1, 2047, 2048});
  EXPECT_EQ(widths(f, {0}), "0 [500]");
  EXPECT_EQ(widths(f, {1}), "1 [500.488]");
  EXPECT_EQ(widths(f, {2}), "2 [0.977]");   // 0.9765625
  EXPECT_EQ(widths(f, {3}), "3 [0.488]");   // 0.48828125
  EXPECT_EQ(widths(f, {4}), "4 [999.512]"); // 999.51171875
  EXPECT_EQ(widths(f, {5}), "5 [1000]");
}

TEST(PdfWidths, ARoundingTieGoesAwayFromZero) {
  // 1e6 * advance / 640 has a fraction of exactly one half for an advance of 1 (1562.5), so
  // the thousandth is decided by the rule and not by the arithmetic.
  Face f(640, {1, 2, 3});
  EXPECT_EQ(widths(f, {0}), "0 [1.563]");   // 1.5625
  EXPECT_EQ(widths(f, {1}), "1 [3.125]");   // 3.125, no tie
  EXPECT_EQ(widths(f, {2}), "2 [4.688]");   // 4.6875
}

TEST(PdfWidths, Refusals) {
  Face f(1000, {500, 500});
  EXPECT_EQ(widths_result(f, {}), GFNT_ERR_INVALID);
  EXPECT_EQ(widths_result(f, {0, 2}), GFNT_ERR_INVALID);   // no glyph 2
  GFNT_Blob * blob = nullptr;
  uint32_t g = 0;
  EXPECT_EQ(gfnt_pdf_widths(nullptr, &g, 1, nullptr, nullptr, nullptr, &blob, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_pdf_widths(f.face, &g, 1, nullptr, nullptr, nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_pdf_widths(f.face, nullptr, 1, nullptr, nullptr, nullptr, &blob, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(blob, nullptr);
}

TEST(PdfToUnicode, ACMapWithNoEntriesIsStillACMap) {
  std::string text;
  ASSERT_EQ(tounicode({}, 2, &text), GFNT_OK);
  EXPECT_EQ(text, kHead + "<0000> <FFFF>\nendcodespacerange\n" + kTail);
}

TEST(PdfToUnicode, OneByteCodesUseOneByteSpaces) {
  std::string text;
  ASSERT_EQ(tounicode({{0x41, {0x41}}}, 1, &text), GFNT_OK);
  EXPECT_EQ(text, kHead + "<00> <FF>\nendcodespacerange\n1 beginbfchar\n<41> <0041>\nendbfchar\n" + kTail);
}

TEST(PdfToUnicode, ConsecutiveCodesOfConsecutiveCodepointsAreARangeAndTheRestAreChars) {
  std::string text;
  ASSERT_EQ(tounicode({{12, {0x42}}, {10, {0x41}}, {11, {0x42}}, {20, {0x5A}},
      {30, {0x20AC, 0x0308}}}, 2, &text), GFNT_OK);
  // 10 -> A, 11 -> B, 12 -> B (so 10..11 is a range and 12 is not part of it).
  EXPECT_EQ(text, kHead + "<0000> <FFFF>\nendcodespacerange\n"
      "3 beginbfchar\n<000C> <0042>\n<0014> <005A>\n<001E> <20AC0308>\nendbfchar\n"
      "1 beginbfrange\n<000A> <000B> <0041>\nendbfrange\n" + kTail);
}

TEST(PdfToUnicode, ARangeNeverWrapsTheLastByteOfItsDestinationOrItsCode) {
  std::string text;
  // Destinations 0x00FE, 0x00FF, 0x0100: the third would carry out of the last byte.
  ASSERT_EQ(tounicode({{1, {0xFE}}, {2, {0xFF}}, {3, {0x100}}}, 2, &text), GFNT_OK);
  EXPECT_EQ(text, kHead + "<0000> <FFFF>\nendcodespacerange\n"
      "1 beginbfchar\n<0003> <0100>\nendbfchar\n"
      "1 beginbfrange\n<0001> <0002> <00FE>\nendbfrange\n" + kTail);
  // Codes 0x00FF and 0x0100 differ in their high byte.
  ASSERT_EQ(tounicode({{0xFF, {0x41}}, {0x100, {0x42}}}, 2, &text), GFNT_OK);
  EXPECT_EQ(text, kHead + "<0000> <FFFF>\nendcodespacerange\n"
      "2 beginbfchar\n<00FF> <0041>\n<0100> <0042>\nendbfchar\n" + kTail);
}

TEST(PdfToUnicode, ACodepointPastTheBasicPlaneIsASurrogatePair) {
  std::string text;
  ASSERT_EQ(tounicode({{1, {0x1F600}}, {2, {0x10FFFF}}}, 2, &text), GFNT_OK);
  EXPECT_EQ(text, kHead + "<0000> <FFFF>\nendcodespacerange\n"
      "2 beginbfchar\n<0001> <D83DDE00>\n<0002> <DBFFDFFF>\nendbfchar\n" + kTail);
}

TEST(PdfToUnicode, AnEntryWithNoCodepointsSaysNothingAndIsLeftOut) {
  std::string text;
  ASSERT_EQ(tounicode({{1, {}}, {2, {0x41}}}, 2, &text), GFNT_OK);
  EXPECT_EQ(text, kHead + "<0000> <FFFF>\nendcodespacerange\n1 beginbfchar\n<0002> <0041>\nendbfchar\n" + kTail);
}

TEST(PdfToUnicode, ABlockHoldsAtMostAHundredEntries) {
  std::vector<Entry> in;
  for (uint32_t i = 0; i < 150; i++) {
    in.push_back({i * 2, {0x4E00 + i * 2}});   // codes and codepoints skip, so no range
  }
  std::string text;
  ASSERT_EQ(tounicode(in, 2, &text), GFNT_OK);
  const size_t first = text.find("100 beginbfchar\n");
  const size_t second = text.find("50 beginbfchar\n");
  EXPECT_NE(first, std::string::npos);
  EXPECT_NE(second, std::string::npos);
  EXPECT_LT(first, second);
  EXPECT_EQ(text.find("beginbfrange"), std::string::npos);
}

TEST(PdfToUnicode, Refusals) {
  std::string text;
  EXPECT_EQ(tounicode({{1, {0x41}}}, 0, &text), GFNT_ERR_INVALID);
  EXPECT_EQ(tounicode({{1, {0x41}}}, 3, &text), GFNT_ERR_INVALID);
  EXPECT_EQ(tounicode({{0x100, {0x41}}}, 1, &text), GFNT_ERR_INVALID);   // does not fit a byte
  EXPECT_EQ(tounicode({{0x10000, {0x41}}}, 2, &text), GFNT_ERR_INVALID);
  EXPECT_EQ(tounicode({{1, {0x41}}, {1, {0x42}}}, 2, &text), GFNT_ERR_INVALID);   // repeats
  EXPECT_EQ(tounicode({{1, {0xD800}}}, 2, &text), GFNT_ERR_INVALID);   // a surrogate
  EXPECT_EQ(tounicode({{1, {0x110000}}}, 2, &text), GFNT_ERR_INVALID);
  EXPECT_EQ(tounicode({{1, std::vector<uint32_t>(GFNT_PDF_TOUNICODE_MAX_CODEPOINTS + 1, 0x41)}}, 2,
      &text), GFNT_ERR_INVALID);
  EXPECT_EQ(tounicode({{1, std::vector<uint32_t>(GFNT_PDF_TOUNICODE_MAX_CODEPOINTS, 0x41)}}, 2,
      &text), GFNT_OK);
  GFNT_Blob * blob = nullptr;
  GFNT_PdfToUnicodeEntry bad = {1, nullptr, 1};
  EXPECT_EQ(gfnt_pdf_tounicode(&bad, 1, 2, nullptr, nullptr, &blob, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_pdf_tounicode(nullptr, 1, 2, nullptr, nullptr, &blob, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_pdf_tounicode(&bad, 0, 2, nullptr, nullptr, nullptr, nullptr), GFNT_ERR_INVALID);
}

namespace {

std::string name_of(GFNT_PdfEncoding e, unsigned code) {
  const char * name = gfnt_pdf_encoding_glyph_name(e, static_cast<uint8_t>(code));
  return name ? name : "(none)";
}

}  // namespace

TEST(PdfEncodings, TheCodesTheReferenceNamesAreTheNamesItGives) {
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_STANDARD, 0x41), "A");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_STANDARD, 0xE1), "AE");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_STANDARD, 0x27), "quoteright");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_STANDARD, 0x60), "quoteleft");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_STANDARD, 0x00), "(none)");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_WIN_ANSI, 0x27), "quotesingle");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_WIN_ANSI, 0x80), "Euro");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_WIN_ANSI, 0xA0), "space");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_WIN_ANSI, 0x00), "(none)");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_MAC_ROMAN, 0x80), "Adieresis");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_MAC_ROMAN, 0xCA), "space");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_MAC_EXPERT, 0x21), "exclamsmall");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_SYMBOL, 0x61), "alpha");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_SYMBOL, 0xA0), "Euro");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_ZAPF_DINGBATS, 0x21), "a1");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_ZAPF_DINGBATS, 0x20), "space");
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_ZAPF_DINGBATS, 0x00), "(none)");
}

TEST(PdfEncodings, TheFootnotesOfWinAnsiAreHonoured) {
  for (unsigned code : {127u, 129u, 141u, 143u, 144u, 157u}) {
    EXPECT_EQ(name_of(GFNT_PDF_ENCODING_WIN_ANSI, code), "bullet") << code;
  }
  EXPECT_EQ(name_of(GFNT_PDF_ENCODING_WIN_ANSI, 173), "hyphen");
}

TEST(PdfEncodings, EveryNameOfEveryEncodingReadsAsText) {
  for (int e = 0; e < GFNT_PDF_ENCODING_COUNT; e++) {
    const auto enc = static_cast<GFNT_PdfEncoding>(e);
    unsigned defined = 0;

    for (unsigned code = 0; code < 256; code++) {
      const char * name = gfnt_pdf_encoding_glyph_name(enc, static_cast<uint8_t>(code));
      if (!name) {
        continue;
      }
      defined++;
      uint32_t out[8];
      size_t count = 0;
      const bool zapf = enc == GFNT_PDF_ENCODING_ZAPF_DINGBATS && std::string(name) != "space";
      ASSERT_EQ(gfnt_glyph_name_to_unicode(name, zapf, out, 8, &count, nullptr), GFNT_OK) << name;
      EXPECT_GE(count, 1u) << "encoding " << e << " code " << code << " " << name;
      uint8_t back = 0;
      ASSERT_TRUE(gfnt_pdf_encoding_code_for_name(enc, name, &back)) << name;
      EXPECT_LE(back, code) << name;
      EXPECT_STREQ(gfnt_pdf_encoding_glyph_name(enc, back), name);
    }
    // How many codes each encoding defines, as the generator's two sources agree on them (a cut or
    // shifted listing would move one of these).
    switch (enc) {
      case GFNT_PDF_ENCODING_STANDARD: EXPECT_EQ(defined, 149u); break;
      case GFNT_PDF_ENCODING_WIN_ANSI: EXPECT_EQ(defined, 224u); break;
      case GFNT_PDF_ENCODING_MAC_ROMAN: EXPECT_EQ(defined, 208u); break;
      case GFNT_PDF_ENCODING_MAC_EXPERT: EXPECT_EQ(defined, 165u); break;
      case GFNT_PDF_ENCODING_SYMBOL: EXPECT_EQ(defined, 189u); break;
      case GFNT_PDF_ENCODING_ZAPF_DINGBATS: EXPECT_EQ(defined, 202u); break;
      default: break;
    }
  }
}

TEST(PdfEncodings, NoSuchEncodingOrNameOrCodeIsNoAnswer) {
  uint8_t code = 77;
  EXPECT_EQ(gfnt_pdf_encoding_glyph_name(GFNT_PDF_ENCODING_COUNT, 65), nullptr);
  EXPECT_EQ(gfnt_pdf_encoding_glyph_name(static_cast<GFNT_PdfEncoding>(-1), 65), nullptr);
  EXPECT_FALSE(gfnt_pdf_encoding_code_for_name(GFNT_PDF_ENCODING_STANDARD, "nosuchglyph", &code));
  EXPECT_EQ(code, 77);
  EXPECT_FALSE(gfnt_pdf_encoding_code_for_name(GFNT_PDF_ENCODING_STANDARD, nullptr, &code));
  EXPECT_FALSE(gfnt_pdf_encoding_code_for_name(GFNT_PDF_ENCODING_STANDARD, "A", nullptr));
  EXPECT_FALSE(gfnt_pdf_encoding_code_for_name(GFNT_PDF_ENCODING_COUNT, "A", &code));
  EXPECT_TRUE(gfnt_pdf_encoding_code_for_name(GFNT_PDF_ENCODING_WIN_ANSI, "bullet", &code));
  EXPECT_EQ(code, 127);   // the lowest of the several
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
