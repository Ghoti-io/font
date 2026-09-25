/**
 * @file
 *
 * `cmap`: the subtable directory, the preference order, and the four formats
 * this library reads.
 *
 * documentation/design.md section 7.2 and M10. Format 4's arithmetic is the
 * single most misimplemented piece of the whole sfnt format, so the tests that
 * exercise it name what they are checking.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <string>
#include <vector>

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/metrics.h>

using gfnttest::CmapRecord;
using gfnttest::Group12;
using gfnttest::Segment4;
using gfnttest::Table;

namespace {

/** A font whose only interesting table is its `cmap`. */
struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  explicit Font(std::vector<Table> tables)
      : bytes(gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables)) {
    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                  GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
        GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error),
        GFNT_OK);
  }

  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }

  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;

  /** The glyph for a codepoint, through the preferred subtable. */
  uint32_t glyph(uint32_t codepoint) {
    uint32_t glyph = 0xFFFFFFFFu;
    EXPECT_EQ(gfnt_face_glyph_for_codepoint(face, codepoint, &glyph, &error),
        GFNT_OK)
        << "codepoint " << codepoint;
    return glyph;
  }
};

/** A `cmap` table wrapped as a directory entry. */
Table cmap_table(std::vector<uint8_t> data) {
  return Table{GFNT_TAG('c', 'm', 'a', 'p'), std::move(data)};
}

/** A one-subtable Windows BMP `cmap`. */
Table windows_bmp(std::vector<uint8_t> subtable) {
  return cmap_table(gfnttest::build_cmap({{3, 1, std::move(subtable)}}));
}

} // namespace

TEST(CmapFormat4, MapsADeltaRun) {
  // The common case: one segment covering 'A'..'Z' with a constant offset, and
  // the terminating 0xFFFF segment every conforming font carries.
  Font font({windows_bmp(gfnttest::build_cmap_format4({
      {0x41, 0x5A, static_cast<int16_t>(-0x41 + 3), {}},
      {0xFFFF, 0xFFFF, 1, {}},
  }))});

  EXPECT_EQ(font.glyph(0x41), 3u) << "'A' is the third glyph";
  EXPECT_EQ(font.glyph(0x42), 4u);
  EXPECT_EQ(font.glyph(0x5A), 28u) << "'Z'";
  EXPECT_EQ(font.glyph(0x40), 0u) << "just below the segment is unmapped";
  EXPECT_EQ(font.glyph(0x5B), 0u) << "just above it is unmapped";
}

TEST(CmapFormat4, MapsThroughTheGlyphArrayAtTheOffsetTheFormatDefines) {
  // M10. idRangeOffset is a byte offset from the address of the idRangeOffset
  // entry itself - not from the start of the subtable, and not from the start
  // of the array - and the builder is what computes it here, so this test
  // checks the parser against the format rather than against itself.
  Font font({windows_bmp(gfnttest::build_cmap_format4({
      {0x100, 0x102, 0, {10, 20, 30}},
      {0x200, 0x201, 0, {40, 50}},
      {0xFFFF, 0xFFFF, 1, {}},
  }))});

  EXPECT_EQ(font.glyph(0x100), 10u);
  EXPECT_EQ(font.glyph(0x101), 20u);
  EXPECT_EQ(font.glyph(0x102), 30u);
  EXPECT_EQ(font.glyph(0x200), 40u) << "the second segment's own array";
  EXPECT_EQ(font.glyph(0x201), 50u);
  EXPECT_EQ(font.glyph(0x103), 0u);
}

TEST(CmapFormat4, AZeroInTheGlyphArrayIsUnmappedAndTheDeltaIsNotApplied) {
  // The format says so explicitly, and a parser that adds idDelta to the zero
  // turns "no glyph here" into a real glyph - which is a wrong glyph drawn
  // rather than a missing one, and therefore much harder to notice.
  Font font({windows_bmp(gfnttest::build_cmap_format4({
      {0x100, 0x102, 5, {10, 0, 30}},
      {0xFFFF, 0xFFFF, 1, {}},
  }))});

  EXPECT_EQ(font.glyph(0x100), 15u) << "10 + the delta";
  EXPECT_EQ(font.glyph(0x101), 0u) << "the zero stays zero";
  EXPECT_EQ(font.glyph(0x102), 35u) << "30 + the delta";
}

TEST(CmapFormat4, TheDeltaArithmeticIsModuloSixtyFiveThousandFiveHundredThirtySix) {
  // Fonts rely on the wrap: a segment covering the private-use range with a
  // large positive delta is how some map 0xF000 upwards to low glyph indices.
  Font font({windows_bmp(gfnttest::build_cmap_format4({
      {0xF000, 0xF00F, 0x2000, {}},
      {0xFFFF, 0xFFFF, 1, {}},
  }))});

  EXPECT_EQ(font.glyph(0xF000), 0x1000u) << "(0xF000 + 0x2000) mod 65536";
  EXPECT_EQ(font.glyph(0xF00F), 0x100Fu);
}

TEST(CmapFormat4, CannotExpressAnythingAboveTheBasicMultilingualPlane) {
  // Not corrupt: the format is 16-bit by construction, and a caller asking
  // about U+10000 is asking a reasonable question of a table that cannot
  // answer it.
  Font font({windows_bmp(gfnttest::build_cmap_format4({
      {0x41, 0x5A, static_cast<int16_t>(-0x41 + 3), {}},
      {0xFFFF, 0xFFFF, 1, {}},
  }))});

  EXPECT_EQ(font.glyph(0x10000), 0u);
  EXPECT_EQ(font.glyph(0x10FFFF), 0u);
}

TEST(CmapFormat4, AnOddSegCountX2IsCorrupt) {
  std::vector<uint8_t> subtable = gfnttest::build_cmap_format4({
      {0x41, 0x5A, 0, {}},
      {0xFFFF, 0xFFFF, 1, {}},
  });
  gfnttest::patch_u16(subtable, 6, 5);
  Font font({windows_bmp(subtable)});

  uint32_t glyph = 0;
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(font.face, 0x41, &glyph,
                &font.error),
      GFNT_ERR_CORRUPT);
  EXPECT_EQ(font.error.table, GFNT_TAG('c', 'm', 'a', 'p'));
}

TEST(CmapFormat4, AGlyphArrayOffsetThatLeavesTheTableIsCorrupt) {
  // The arithmetic is done in size_t and bounds-checked by the reader, so this
  // is a diagnostic rather than a read of whatever follows the cmap.
  std::vector<uint8_t> subtable = gfnttest::build_cmap_format4({
      {0x100, 0x102, 0, {10, 20, 30}},
      {0xFFFF, 0xFFFF, 1, {}},
  });
  gfnttest::patch_u16(subtable, gfnttest::format4_id_range_offsets(2), 0xFFF0);
  Font font({windows_bmp(subtable)});

  uint32_t glyph = 0;
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(font.face, 0x100, &glyph,
                &font.error),
      GFNT_ERR_CORRUPT);
}

TEST(CmapFormat12, MapsTheWholeOfUnicode) {
  Font font({windows_bmp(gfnttest::build_cmap_format12({
      {0x41, 0x5A, 3},
      {0x1F600, 0x1F64F, 100},
  }))});

  EXPECT_EQ(font.glyph(0x41), 3u);
  EXPECT_EQ(font.glyph(0x5A), 28u);
  EXPECT_EQ(font.glyph(0x1F600), 100u) << "an astral codepoint";
  EXPECT_EQ(font.glyph(0x1F64F), 179u);
  EXPECT_EQ(font.glyph(0x1F650), 0u);
  EXPECT_EQ(font.glyph(0x40), 0u);
}

TEST(CmapFormat12, AGroupThatEndsBeforeItStartsIsCorrupt) {
  std::vector<uint8_t> subtable = gfnttest::build_cmap_format12({
      {0x100, 0x200, 5},
  });
  gfnttest::patch_u32(subtable, 16 + 4, 0x50);  // endCharCode below the start
  Font font({windows_bmp(subtable)});

  uint32_t glyph = 0;
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(font.face, 0x150, &glyph,
                &font.error),
      GFNT_ERR_CORRUPT);
  EXPECT_NE(font.error.message, nullptr);
}

TEST(CmapFormat12, AGroupThatMapsPastTheGlyphSpaceIsCorrupt) {
  // startGlyphID + (codepoint - start) is computed in 64 bits, so a group
  // claiming a start near UINT32_MAX is reported rather than wrapping into a
  // low glyph index.
  Font font({windows_bmp(gfnttest::build_cmap_format12({
      {0x100, 0x200, 0xFFFFFFF0u},
  }))});

  uint32_t glyph = 0;
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(font.face, 0x1FF, &glyph,
                &font.error),
      GFNT_ERR_CORRUPT);
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(font.face, 0x100, &glyph, nullptr),
      GFNT_OK)
      << "the group is usable where it does not overflow";
  EXPECT_EQ(glyph, 0xFFFFFFF0u);
}

TEST(CmapFormat12, AGroupListThatEndsEarlyIsCorrupt) {
  std::vector<uint8_t> subtable = gfnttest::build_cmap_format12({
      {0x100, 0x200, 5},
      {0x300, 0x400, 300},
  });
  subtable.resize(subtable.size() - 6);
  Font font({windows_bmp(subtable)});

  uint32_t glyph = 0;
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(font.face, 0x350, &glyph, nullptr),
      GFNT_ERR_CORRUPT);
}

TEST(CmapFormat0, MapsSingleBytes) {
  Font font({cmap_table(gfnttest::build_cmap(
      {{1, 0, gfnttest::build_cmap_format0({{0x41, 7}, {0xFF, 9}})}}))});

  EXPECT_EQ(font.glyph(0x41), 7u);
  EXPECT_EQ(font.glyph(0xFF), 9u);
  EXPECT_EQ(font.glyph(0x42), 0u);
  EXPECT_EQ(font.glyph(0x100), 0u) << "the format cannot express it";
}

TEST(CmapFormat6, MapsATrimmedArray) {
  Font font({windows_bmp(gfnttest::build_cmap_format6(0x30, {5, 6, 7}))});

  EXPECT_EQ(font.glyph(0x30), 5u);
  EXPECT_EQ(font.glyph(0x32), 7u);
  EXPECT_EQ(font.glyph(0x2F), 0u) << "below firstCode";
  EXPECT_EQ(font.glyph(0x33), 0u) << "past entryCount";
}

TEST(Cmap, TheSubtableListIsReadableInTheOrderTheFontWroteIt) {
  Font font({cmap_table(gfnttest::build_cmap({
      {1, 0, gfnttest::build_cmap_format0({{0x41, 7}})},
      {3, 1, gfnttest::build_cmap_format4({{0x41, 0x41, 0, {11}}})},
      {3, 10, gfnttest::build_cmap_format12({{0x41, 0x41, 22}})},
  }))});

  size_t count = 0;
  ASSERT_EQ(gfnt_face_cmap_count(font.face, &count, nullptr), GFNT_OK);
  ASSERT_EQ(count, 3u);

  const uint16_t platforms[] = {1, 3, 3};
  const uint16_t encodings[] = {0, 1, 10};
  const uint16_t formats[] = {0, 4, 12};
  for (size_t i = 0; i < 3; i++) {
    GFNT_CmapSubtable subtable{};
    ASSERT_EQ(gfnt_face_cmap_at(font.face, i, &subtable, nullptr), GFNT_OK);
    EXPECT_EQ(subtable.platform_id, platforms[i]);
    EXPECT_EQ(subtable.encoding_id, encodings[i]);
    EXPECT_EQ(subtable.format, formats[i]) << "read from the subtable itself";
  }
  EXPECT_EQ(gfnt_face_cmap_at(font.face, 3, nullptr, nullptr),
      GFNT_ERR_INVALID);
}

TEST(Cmap, ThePreferenceOrderTakesFullUnicodeFirst) {
  // design.md section 7.2: (3,10) beats (3,1) beats (1,0). The font's own
  // order is deliberately the reverse, so that a parser taking the first
  // subtable it meets fails here.
  Font font({cmap_table(gfnttest::build_cmap({
      {1, 0, gfnttest::build_cmap_format0({{0x41, 7}})},
      {3, 1, gfnttest::build_cmap_format4({{0x41, 0x41, 0, {11}}})},
      {3, 10, gfnttest::build_cmap_format12({{0x41, 0x41, 22}})},
  }))});

  GFNT_CmapSubtable best{};
  ASSERT_EQ(gfnt_face_cmap_best(font.face, &best, nullptr), GFNT_OK);
  EXPECT_EQ(best.platform_id, 3u);
  EXPECT_EQ(best.encoding_id, 10u);
  EXPECT_EQ(font.glyph(0x41), 22u) << "the chosen subtable is the one used";
}

TEST(Cmap, ThePreferenceOrderPassesOverAFormatItCannotRead) {
  // A format 13 subtable at the top preference would otherwise select a
  // subtable that refuses every lookup, leaving a caller with a font that
  // every other implementation maps.
  std::vector<uint8_t> format13;
  gfnttest::put_u16(format13, 13);
  gfnttest::put_u16(format13, 0);
  gfnttest::put_u32(format13, 16);
  gfnttest::put_u32(format13, 0);
  gfnttest::put_u32(format13, 0);

  Font font({cmap_table(gfnttest::build_cmap({
      {3, 10, format13},
      {3, 1, gfnttest::build_cmap_format4({{0x41, 0x41, 0, {11}}})},
  }))});

  GFNT_CmapSubtable best{};
  ASSERT_EQ(gfnt_face_cmap_best(font.face, &best, nullptr), GFNT_OK);
  EXPECT_EQ(best.encoding_id, 1u) << "the next preference answered";
  EXPECT_EQ(best.format, 4u);
  EXPECT_EQ(font.glyph(0x41), 11u);
}

TEST(Cmap, AFormatItCannotReadIsUnsupportedWhenAskedForDirectly) {
  // The distinction the preference order hides is still available: asking that
  // subtable directly says "this library does not read that yet", which is not
  // the same answer as "that font is broken".
  std::vector<uint8_t> format13;
  gfnttest::put_u16(format13, 13);
  gfnttest::put_u16(format13, 0);
  gfnttest::put_u32(format13, 16);
  gfnttest::put_u32(format13, 0);
  gfnttest::put_u32(format13, 0);

  Font font({cmap_table(gfnttest::build_cmap({{3, 10, format13}}))});
  GFNT_CmapSubtable subtable{};
  ASSERT_EQ(gfnt_face_cmap_at(font.face, 0, &subtable, nullptr), GFNT_OK);
  uint32_t glyph = 0;
  EXPECT_EQ(gfnt_cmap_lookup(font.face, &subtable, 0x41, &glyph, &font.error),
      GFNT_ERR_UNSUPPORTED);

  GFNT_CmapSubtable best{};
  EXPECT_EQ(gfnt_face_cmap_best(font.face, &best, nullptr),
      GFNT_ERR_UNSUPPORTED)
      << "and a font with nothing else is a font with no usable cmap";
}

TEST(Cmap, AnEncodingTheOrderDoesNotNameIsNotUsedByDefault) {
  Font font({cmap_table(gfnttest::build_cmap(
      {{7, 7, gfnttest::build_cmap_format4({{0x41, 0x41, 0, {11}}})}}))});

  GFNT_CmapSubtable best{};
  EXPECT_EQ(gfnt_face_cmap_best(font.face, &best, &font.error),
      GFNT_ERR_UNSUPPORTED);

  // It is still reachable by name, which is what a tool inspecting a font
  // needs.
  GFNT_CmapSubtable subtable{};
  ASSERT_EQ(gfnt_face_cmap_at(font.face, 0, &subtable, nullptr), GFNT_OK);
  uint32_t glyph = 0;
  ASSERT_EQ(gfnt_cmap_lookup(font.face, &subtable, 0x41, &glyph, nullptr),
      GFNT_OK);
  EXPECT_EQ(glyph, 11u);
}

TEST(Cmap, TheWindowsSymbolSubtableIsReadThroughItsPrivateUseRange) {
  // A symbol font maps its glyphs at 0xF000..0xF0FF and expects a caller
  // asking for 'A' to be answered from 0xF041. Every other implementation
  // reads one this way.
  Font font({cmap_table(gfnttest::build_cmap({{3, 0,
      gfnttest::build_cmap_format4({
          {0xF041, 0xF042, 0, {5, 6}},
          {0xFFFF, 0xFFFF, 1, {}},
      })}}))});

  EXPECT_EQ(font.glyph(0x41), 5u) << "'A' through 0xF041";
  EXPECT_EQ(font.glyph(0x42), 6u);
  EXPECT_EQ(font.glyph(0xF041), 5u) << "and the codepoint itself still works";
  EXPECT_EQ(font.glyph(0x43), 0u);
}

TEST(Cmap, AnUnmappedCodepointIsGlyphZeroRatherThanAnError) {
  // M14 and section 5.4: glyph 0 is .notdef, every font has one, and this
  // library will draw it. Asking about a codepoint a font does not cover is an
  // ordinary question with an ordinary answer.
  Font font({windows_bmp(gfnttest::build_cmap_format4({
      {0x41, 0x41, 0, {11}},
      {0xFFFF, 0xFFFF, 1, {}},
  }))});

  uint32_t glyph = 0xFFFFu;
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(font.face, 0x2603, &glyph, nullptr),
      GFNT_OK);
  EXPECT_EQ(glyph, 0u);
}

TEST(Cmap, ASubtableOffsetThatLeavesTheTableIsCorrupt) {
  std::vector<uint8_t> cmap = gfnttest::build_cmap(
      {{3, 1, gfnttest::build_cmap_format4({{0x41, 0x41, 0, {11}}})}});
  gfnttest::patch_u32(cmap, gfnttest::cmap_record_offset(0) + 4, 0xFFFF);

  Font font({cmap_table(cmap)});
  GFNT_CmapSubtable subtable{};
  EXPECT_EQ(gfnt_face_cmap_at(font.face, 0, &subtable, &font.error),
      GFNT_ERR_CORRUPT);
  GFNT_CmapSubtable best{};
  EXPECT_EQ(gfnt_face_cmap_best(font.face, &best, nullptr),
      GFNT_ERR_UNSUPPORTED)
      << "an unreadable subtable is passed over, and there is nothing else";
}

TEST(Cmap, AFontWithNoCmapIsUnsupportedNotCorrupt) {
  // A font embedded in a PDF typically has no cmap at all; glyph-id access is
  // first-class and codepoint access is the thing that needs the table.
  Font font({Table{GFNT_TAG('m', 'a', 'x', 'p'),
      gfnttest::build_maxp(3)}});
  size_t count = 0;
  uint32_t glyph = 0;
  GFNT_CmapSubtable best{};

  EXPECT_EQ(gfnt_face_cmap_count(font.face, &count, nullptr),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(gfnt_face_cmap_best(font.face, &best, nullptr),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(font.face, 0x41, &glyph, nullptr),
      GFNT_ERR_UNSUPPORTED);
}

TEST(Cmap, ATruncatedHeaderIsCorrupt) {
  Font font({cmap_table({0x00, 0x00})});
  size_t count = 0;
  EXPECT_EQ(gfnt_face_cmap_count(font.face, &count, nullptr), GFNT_ERR_CORRUPT);
}

TEST(Cmap, AMappingPastTheGlyphCountIsReportedAndRefusedWhereItMatters) {
  // The cmap says what the font says; the check against numGlyphs lives in the
  // accessor that would load the glyph, which is the one place it belongs.
  Font font({windows_bmp(gfnttest::build_cmap_format4({
                 {0x41, 0x41, 0, {9}},
                 {0xFFFF, 0xFFFF, 1, {}},
             })),
      Table{GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(2)},
      Table{GFNT_TAG('h', 'h', 'e', 'a'),
          gfnttest::build_hhea(800, -200, 100, 2)},
      Table{GFNT_TAG('h', 'm', 't', 'x'),
          gfnttest::build_hmtx({{500, 10}, {600, 20}})}});

  EXPECT_EQ(font.glyph(0x41), 9u) << "the mapping is reported as the font has it";
  int32_t advance = 0;
  EXPECT_EQ(gfnt_face_glyph_advance(font.face, 9, nullptr, &advance,
                &font.error),
      GFNT_ERR_INVALID)
      << "and the glyph accessor is where that is refused";
  EXPECT_EQ(font.error.glyph, 9u);
}

TEST(Cmap, TheChosenSubtableIsMemoisedAndStable) {
  Font font({cmap_table(gfnttest::build_cmap({
      {3, 1, gfnttest::build_cmap_format4({{0x41, 0x41, 0, {11}}})},
      {3, 10, gfnttest::build_cmap_format12({{0x41, 0x41, 22}})},
  }))});

  GFNT_CmapSubtable first{};
  GFNT_CmapSubtable second{};
  ASSERT_EQ(gfnt_face_cmap_best(font.face, &first, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_cmap_best(font.face, &second, nullptr), GFNT_OK);
  EXPECT_EQ(first.platform_id, second.platform_id);
  EXPECT_EQ(first.encoding_id, second.encoding_id);
  EXPECT_EQ(first.format, second.format);
  EXPECT_EQ(first.offset, second.offset);
}

TEST(CmapDump, NamesEverySubtableAndWhichOneAnswers) {
  Font font({cmap_table(gfnttest::build_cmap({
      {1, 0, gfnttest::build_cmap_format0({{0x41, 7}})},
      {3, 10, gfnttest::build_cmap_format12({{0x41, 0x41, 22}})},
  }))});

  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  ASSERT_EQ(gfnt_face_cmap_dump(font.face, out.get()), GFNT_OK);
  std::string text = out.finish();

  EXPECT_NE(text.find("2 subtables"), std::string::npos) << text;
  EXPECT_NE(text.find("platform 1, encoding 0, format 0"), std::string::npos)
      << text;
  EXPECT_NE(text.find("platform 3, encoding 10, format 12"), std::string::npos)
      << text;
  EXPECT_NE(text.find("using platform 3, encoding 10"), std::string::npos)
      << text;
}

TEST(CmapDump, SaysWhenNothingIsUsableAndRefusesNulls) {
  Font font({cmap_table(gfnttest::build_cmap(
      {{7, 7, gfnttest::build_cmap_format4({{0x41, 0x41, 0, {11}}})}}))});

  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  ASSERT_EQ(gfnt_face_cmap_dump(font.face, out.get()), GFNT_OK);
  std::string text = out.finish();
  EXPECT_NE(text.find("no subtable this library reads"), std::string::npos)
      << text;

  EXPECT_EQ(gfnt_face_cmap_dump(nullptr, nullptr), GFNT_ERR_INVALID);
}

TEST(CmapDump, ReportsEveryWriteFailure) {
  Font font({cmap_table(gfnttest::build_cmap({
      {1, 0, gfnttest::build_cmap_format0({{0x41, 7}})},
      {3, 10, gfnttest::build_cmap_format12({{0x41, 0x41, 22}})},
  }))});

  size_t failures = 0;
  for (size_t allow = 0; allow < 6; allow++) {
    gfnttest::FailingSink sink(allow);
    ASSERT_NE(sink.get(), nullptr);
    if (gfnt_face_cmap_dump(font.face, sink.get()) == GFNT_ERR_IO) {
      failures++;
    }
  }
  EXPECT_EQ(failures, 4u)
      << "a header line, one line per subtable, and the line naming the "
         "subtable in use";
}

TEST(Cmap, EveryEntryPointRefusesNullArguments) {
  Font font({windows_bmp(gfnttest::build_cmap_format4({{0x41, 0x41, 0, {11}}}))});
  GFNT_CmapSubtable subtable{};
  ASSERT_EQ(gfnt_face_cmap_at(font.face, 0, &subtable, nullptr), GFNT_OK);
  size_t count = 0;
  uint32_t glyph = 0;

  EXPECT_EQ(gfnt_face_cmap_count(nullptr, &count, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_cmap_count(font.face, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_cmap_at(nullptr, 0, &subtable, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_cmap_best(nullptr, &subtable, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_cmap_best(font.face, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_cmap_lookup(nullptr, &subtable, 0x41, &glyph, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_cmap_lookup(font.face, nullptr, 0x41, &glyph, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_cmap_lookup(font.face, &subtable, 0x41, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(nullptr, 0x41, &glyph, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(font.face, 0x41, nullptr, nullptr),
      GFNT_ERR_INVALID);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
