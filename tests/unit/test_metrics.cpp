/**
 * @file
 *
 * The metric tables: head, maxp, hhea, hmtx, OS/2, post, the numGlyphs
 * minimum, and which table answers "how tall is a line".
 *
 * documentation/design.md section 7.2, and M4, M12 and M18 from section 2.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <string>
#include <thread>
#include <vector>

#include <ghoti.io/font/metrics.h>

using gfnttest::Table;

namespace {

/** A font built from tables, with a blob and a face over it. */
struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  GFNT_Result result = GFNT_ERR_INTERNAL;

  explicit Font(const std::vector<Table> & tables,
      const GFNT_Limits * limits = nullptr)
      : bytes(gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables)) {
    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                  GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
        GFNT_OK);
    result = gfnt_face_load(blob, 0, limits, nullptr, &face, &error);
    EXPECT_EQ(result, GFNT_OK);
  }

  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }

  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;
};

Table head_table(std::vector<uint8_t> data) {
  return Table{GFNT_TAG('h', 'e', 'a', 'd'), std::move(data)};
}
Table maxp_table(std::vector<uint8_t> data) {
  return Table{GFNT_TAG('m', 'a', 'x', 'p'), std::move(data)};
}
Table hhea_table(std::vector<uint8_t> data) {
  return Table{GFNT_TAG('h', 'h', 'e', 'a'), std::move(data)};
}
Table hmtx_table(std::vector<uint8_t> data) {
  return Table{GFNT_TAG('h', 'm', 't', 'x'), std::move(data)};
}
Table os2_table(std::vector<uint8_t> data) {
  return Table{GFNT_TAG('O', 'S', '/', '2'), std::move(data)};
}
Table post_table(std::vector<uint8_t> data) {
  return Table{GFNT_TAG('p', 'o', 's', 't'), std::move(data)};
}

} // namespace

TEST(Head, ParsesEveryFieldTheSpecificationDefines) {
  Font font({head_table(gfnttest::build_head(2048, 1))});
  const GFNT_Head * head = nullptr;
  ASSERT_EQ(gfnt_face_head(font.face, &head, &font.error), GFNT_OK);

  EXPECT_EQ(head->major_version, 1u);
  EXPECT_EQ(head->minor_version, 0u);
  EXPECT_EQ(head->font_revision, 0x00015000);
  EXPECT_EQ(head->checksum_adjustment, 0xAABBCCDDu);
  EXPECT_EQ(head->flags, 0x000Bu);
  EXPECT_EQ(head->units_per_em, 2048u);
  EXPECT_EQ(head->created, -1) << "a LONGDATETIME is signed";
  EXPECT_EQ(head->modified, 3000000000LL) << "and wider than 32 bits";
  EXPECT_EQ(head->x_min, -100);
  EXPECT_EQ(head->y_min, -250);
  EXPECT_EQ(head->x_max, 1200);
  EXPECT_EQ(head->y_max, 900);
  EXPECT_EQ(head->mac_style, 0x0002u);
  EXPECT_EQ(head->lowest_rec_ppem, 8u);
  EXPECT_EQ(head->font_direction_hint, 2);
  EXPECT_EQ(head->index_to_loc_format, 1);
  EXPECT_EQ(head->glyph_data_format, 0);
}

TEST(Head, IsParsedOnceAndAnsweredFromTheMemo) {
  // design.md section 5.3: each table is parsed on first use and the result
  // memoised, so that opening a collection to ask one question costs one.
  Font font({head_table(gfnttest::build_head())});
  const GFNT_Head * first = nullptr;
  const GFNT_Head * second = nullptr;

  ASSERT_EQ(gfnt_face_head(font.face, &first, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_head(font.face, &second, nullptr), GFNT_OK);
  EXPECT_EQ(first, second) << "the same storage inside the face, not a copy";
}

TEST(Head, AWrongMagicNumberIsCorrupt) {
  Font font({head_table(gfnttest::build_head(1000, 0, 0xDEADBEEF))});
  const GFNT_Head * head = nullptr;

  EXPECT_EQ(gfnt_face_head(font.face, &head, &font.error), GFNT_ERR_CORRUPT);
  EXPECT_EQ(font.error.table, GFNT_TAG('h', 'e', 'a', 'd'));
  EXPECT_EQ(font.error.offset, 12u) << "the offset of magicNumber within head";
}

TEST(Head, AZeroEmIsCorruptBecauseNothingCanBeScaled) {
  // The specification's range is 16 to 16384 and this library does not enforce
  // it - fonts outside it render elsewhere - but zero is different in kind.
  Font font({head_table(gfnttest::build_head(0))});
  const GFNT_Head * head = nullptr;
  EXPECT_EQ(gfnt_face_head(font.face, &head, &font.error), GFNT_ERR_CORRUPT);
  EXPECT_EQ(font.error.offset, 18u);
}

TEST(Head, AnEmOutsideTheSpecifiedRangeIsStillRead) {
  // Deliberately permissive: refusing these would refuse fonts every other
  // implementation reads, and the caller can see the number.
  for (uint16_t upem : {(uint16_t)1, (uint16_t)15, (uint16_t)16000}) {
    Font font({head_table(gfnttest::build_head(upem))});
    uint16_t parsed = 0;
    ASSERT_EQ(gfnt_face_units_per_em(font.face, &parsed, nullptr), GFNT_OK)
        << "unitsPerEm " << upem;
    EXPECT_EQ(parsed, upem);
  }
}

TEST(Head, AnIndexToLocFormatThatNamesNoFormatIsCorrupt) {
  Font font({head_table(gfnttest::build_head(1000, 2))});
  const GFNT_Head * head = nullptr;
  EXPECT_EQ(gfnt_face_head(font.face, &head, &font.error), GFNT_ERR_CORRUPT);
  EXPECT_EQ(font.error.offset, 50u);
}

TEST(Head, ATruncatedTableIsCorruptAndTheMemoReplaysWhy) {
  std::vector<uint8_t> head = gfnttest::build_head();
  head.resize(20);
  Font font({head_table(head)});

  const GFNT_Head * parsed = nullptr;
  GFNT_Error first;
  gfnt_error_clear(&first);
  EXPECT_EQ(gfnt_face_head(font.face, &parsed, &first), GFNT_ERR_CORRUPT);
  ASSERT_NE(first.message, nullptr);

  GFNT_Error again;
  gfnt_error_clear(&again);
  EXPECT_EQ(gfnt_face_head(font.face, &parsed, &again), GFNT_ERR_CORRUPT);
  EXPECT_STREQ(again.message, first.message)
      << "a memoised failure explains itself as fully as the first attempt";
  EXPECT_EQ(again.offset, first.offset);
}

TEST(Head, AMissingTableIsUnsupportedNotCorrupt) {
  Font font({maxp_table(gfnttest::build_maxp(3))});
  const GFNT_Head * head = nullptr;
  uint16_t upem = 0;

  EXPECT_EQ(gfnt_face_head(font.face, &head, &font.error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(gfnt_face_units_per_em(font.face, &upem, nullptr),
      GFNT_ERR_UNSUPPORTED);
}

TEST(GlyphCount, ReportsWhatMaxpSaysWhenNothingContradictsIt) {
  Font font({maxp_table(gfnttest::build_maxp(42))});
  size_t count = 0;
  size_t claimed = 0;

  ASSERT_EQ(gfnt_face_num_glyphs(font.face, &count, nullptr), GFNT_OK);
  EXPECT_EQ(count, 42u);
  EXPECT_FALSE(gfnt_face_num_glyphs_disagreement(font.face, &claimed));
  EXPECT_EQ(claimed, 42u);
}

TEST(GlyphCount, VersionHalfCarriesTheSameFieldAsVersionOne) {
  // 0.5 is what a CFF font ships, and numGlyphs sits at the same offset.
  Font font({maxp_table(gfnttest::build_maxp(7, 0x00005000))});
  size_t count = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(font.face, &count, nullptr), GFNT_OK);
  EXPECT_EQ(count, 7u);
}

TEST(GlyphCount, TakesTheMinimumAcrossTheTablesThatIndexGlyphs) {
  // M12. hmtx here holds two long entries and three trailing bearings, so it
  // covers five glyphs; maxp claims ten. The minimum governs and the
  // disagreement is reported.
  Font font({maxp_table(gfnttest::build_maxp(10)),
      hhea_table(gfnttest::build_hhea(800, -200, 100, 2)),
      hmtx_table(gfnttest::build_hmtx({{500, 10}, {600, 20}}, {30, 40, 50}))});
  size_t count = 0;
  size_t claimed = 0;

  ASSERT_EQ(gfnt_face_num_glyphs(font.face, &count, nullptr), GFNT_OK);
  EXPECT_EQ(count, 5u);
  EXPECT_TRUE(gfnt_face_num_glyphs_disagreement(font.face, &claimed));
  EXPECT_EQ(claimed, 10u) << "what maxp claimed is still reportable";
}

TEST(GlyphCount, AnHmtxWithRoomToSpareDoesNotRaiseTheCount) {
  Font font({maxp_table(gfnttest::build_maxp(3)),
      hhea_table(gfnttest::build_hhea(800, -200, 100, 3)),
      hmtx_table(gfnttest::build_hmtx(
          {{500, 10}, {600, 20}, {700, 30}, {800, 40}, {900, 50}}))});
  size_t count = 0;

  ASSERT_EQ(gfnt_face_num_glyphs(font.face, &count, nullptr), GFNT_OK);
  EXPECT_EQ(count, 3u) << "maxp is a maximum as well as a claim";
  EXPECT_FALSE(gfnt_face_num_glyphs_disagreement(font.face, nullptr));
}

TEST(GlyphCount, AnHheaClaimingNoMetricsDoesNotZeroTheCount) {
  // A numberOfHMetrics of zero is a defect in hhea, and the advance accessor
  // reports it as one. If it were allowed to bound the glyph count, one broken
  // field would make every other table in the font unreachable.
  Font font({maxp_table(gfnttest::build_maxp(4)),
      hhea_table(gfnttest::build_hhea(800, -200, 100, 0)),
      hmtx_table(gfnttest::build_hmtx({}))});
  size_t count = 0;
  int32_t advance = 0;

  ASSERT_EQ(gfnt_face_num_glyphs(font.face, &count, nullptr), GFNT_OK);
  EXPECT_EQ(count, 4u);
  EXPECT_EQ(gfnt_face_glyph_advance(font.face, 0, nullptr, &advance,
                &font.error),
      GFNT_ERR_CORRUPT);
  EXPECT_EQ(font.error.table, GFNT_TAG('h', 'h', 'e', 'a'));
}

TEST(GlyphCount, MoreGlyphsThanTheLimitIsRefused) {
  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_glyphs = 5;

  Font font({maxp_table(gfnttest::build_maxp(6))}, &limits);
  size_t count = 0;
  EXPECT_EQ(gfnt_face_num_glyphs(font.face, &count, &font.error),
      GFNT_ERR_LIMIT);
}

TEST(GlyphCount, WithoutMaxpThereIsNoCount) {
  Font font({head_table(gfnttest::build_head())});
  size_t count = 0;
  EXPECT_EQ(gfnt_face_num_glyphs(font.face, &count, nullptr),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_FALSE(gfnt_face_num_glyphs_disagreement(font.face, nullptr));
}

TEST(Hmtx, AdvanceAndSideBearingComeFromTheGlyphsOwnEntry) {
  Font font({maxp_table(gfnttest::build_maxp(3)),
      hhea_table(gfnttest::build_hhea(800, -200, 100, 3)),
      hmtx_table(gfnttest::build_hmtx({{500, 10}, {600, -20}, {700, 30}}))});

  const int32_t advances[] = {500, 600, 700};
  const int32_t bearings[] = {10, -20, 30};
  for (uint32_t glyph = 0; glyph < 3; glyph++) {
    int32_t advance = 0;
    int32_t bearing = 0;
    ASSERT_EQ(gfnt_face_glyph_advance(font.face, glyph, nullptr, &advance,
                  nullptr),
        GFNT_OK)
        << "glyph " << glyph;
    ASSERT_EQ(gfnt_face_glyph_side_bearing(font.face, glyph, nullptr, &bearing,
                  nullptr),
        GFNT_OK)
        << "glyph " << glyph;
    EXPECT_EQ(advance, advances[glyph]);
    EXPECT_EQ(bearing, bearings[glyph]);
  }
}

TEST(Hmtx, TheLastAdvanceRepeatsAndTrailingBearingsAreTheirOwn) {
  // The format's compression for monospaced and CJK fonts: one advance for
  // three thousand glyphs, each with its own side bearing. A parser that
  // indexes the long array directly reads whatever follows it.
  Font font({maxp_table(gfnttest::build_maxp(5)),
      hhea_table(gfnttest::build_hhea(800, -200, 100, 2)),
      hmtx_table(gfnttest::build_hmtx({{500, 10}, {600, 20}}, {30, 40, 50}))});

  for (uint32_t glyph = 2; glyph < 5; glyph++) {
    int32_t advance = 0;
    int32_t bearing = 0;
    ASSERT_EQ(gfnt_face_glyph_advance(font.face, glyph, nullptr, &advance,
                  nullptr),
        GFNT_OK);
    ASSERT_EQ(gfnt_face_glyph_side_bearing(font.face, glyph, nullptr, &bearing,
                  nullptr),
        GFNT_OK);
    EXPECT_EQ(advance, 600) << "glyph " << glyph << " repeats the last advance";
    EXPECT_EQ(bearing, 30 + 10 * static_cast<int32_t>(glyph - 2))
        << "glyph " << glyph << " has its own bearing";
  }
}

TEST(Hmtx, AGlyphTheFaceDoesNotHaveIsACallerError) {
  Font font({maxp_table(gfnttest::build_maxp(2)),
      hhea_table(gfnttest::build_hhea(800, -200, 100, 2)),
      hmtx_table(gfnttest::build_hmtx({{500, 10}, {600, 20}}))});
  int32_t advance = 0;

  EXPECT_EQ(gfnt_face_glyph_advance(font.face, 2, nullptr, &advance,
                &font.error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(font.error.glyph, 2u) << "the diagnostic names the glyph";
}

TEST(Hmtx, AShortTableReducesTheGlyphCountRatherThanBeingReadPast) {
  // The bound and the reads come from the same number, so a truncated hmtx
  // shows up as a face with fewer glyphs - and glyph 1 is then a caller error
  // rather than a read past the table.
  Font font({maxp_table(gfnttest::build_maxp(2)),
      hhea_table(gfnttest::build_hhea(800, -200, 100, 2)),
      hmtx_table(gfnttest::build_hmtx({{500, 10}}))});
  size_t count = 0;
  int32_t advance = 0;

  ASSERT_EQ(gfnt_face_num_glyphs(font.face, &count, nullptr), GFNT_OK);
  EXPECT_EQ(count, 1u);
  EXPECT_EQ(gfnt_face_glyph_advance(font.face, 0, nullptr, &advance, nullptr),
      GFNT_OK);
  EXPECT_EQ(advance, 500);
  EXPECT_EQ(gfnt_face_glyph_advance(font.face, 1, nullptr, &advance, nullptr),
      GFNT_ERR_INVALID);
}

TEST(Hmtx, WithoutHmtxOrHheaThereAreNoAdvances) {
  Font without_hmtx({maxp_table(gfnttest::build_maxp(2)),
      hhea_table(gfnttest::build_hhea(800, -200, 100, 2))});
  Font without_hhea({maxp_table(gfnttest::build_maxp(2)),
      hmtx_table(gfnttest::build_hmtx({{500, 10}, {600, 20}}))});
  int32_t advance = 0;

  EXPECT_EQ(gfnt_face_glyph_advance(without_hmtx.face, 0, nullptr, &advance,
                nullptr),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(gfnt_face_glyph_advance(without_hhea.face, 0, nullptr, &advance,
                nullptr),
      GFNT_ERR_UNSUPPORTED);
}

TEST(Hhea, AMetricDataFormatThisLibraryCannotReadIsUnsupported) {
  Font font({hhea_table(gfnttest::build_hhea(800, -200, 100, 2, 1))});
  const GFNT_Hhea * hhea = nullptr;
  EXPECT_EQ(gfnt_face_hhea(font.face, &hhea, &font.error),
      GFNT_ERR_UNSUPPORTED);
}

TEST(Hhea, ParsesEveryField) {
  Font font({hhea_table(gfnttest::build_hhea(1900, -500, 0, 17))});
  const GFNT_Hhea * hhea = nullptr;
  ASSERT_EQ(gfnt_face_hhea(font.face, &hhea, nullptr), GFNT_OK);

  EXPECT_EQ(hhea->ascender, 1900);
  EXPECT_EQ(hhea->descender, -500) << "already negative in this table";
  EXPECT_EQ(hhea->line_gap, 0);
  EXPECT_EQ(hhea->advance_width_max, 1500u);
  EXPECT_EQ(hhea->min_left_side_bearing, -30);
  EXPECT_EQ(hhea->min_right_side_bearing, -40);
  EXPECT_EQ(hhea->x_max_extent, 1250);
  EXPECT_EQ(hhea->caret_slope_rise, 1);
  EXPECT_EQ(hhea->caret_slope_run, 0);
  EXPECT_EQ(hhea->metric_data_format, 0);
  EXPECT_EQ(hhea->number_of_h_metrics, 17u);
}

TEST(Os2, ParsesEveryVersionItReads) {
  for (uint16_t version = 0; version <= 5; version++) {
    gfnttest::Os2Spec spec;
    spec.version = version;
    Font font({os2_table(gfnttest::build_os2(spec))});
    const GFNT_Os2 * os2 = nullptr;
    ASSERT_EQ(gfnt_face_os2(font.face, &os2, &font.error), GFNT_OK)
        << "version " << version;

    EXPECT_EQ(os2->version, version);
    EXPECT_EQ(os2->weight_class, 400u);
    EXPECT_EQ(os2->typo_ascender, 800);
    EXPECT_EQ(os2->typo_descender, -200);
    EXPECT_EQ(os2->win_descent, 250u) << "positive, as the file gives it";
    EXPECT_STREQ(os2->vendor_id, "GHTI");
    EXPECT_EQ(os2->panose[9], 10u);
    EXPECT_EQ(os2->unicode_range[3], 0x40000000u);

    // Fields a version does not carry are zero, and the version says which.
    EXPECT_EQ(os2->code_page_range[0], version >= 1 ? 0x1Fu : 0u);
    EXPECT_EQ(os2->x_height, version >= 2 ? 500 : 0);
    EXPECT_EQ(os2->cap_height, version >= 2 ? 700 : 0);
    EXPECT_EQ(os2->max_context, version >= 2 ? 3u : 0u);
    EXPECT_EQ(os2->lower_optical_size, version == 5 ? 80u : 0u);
  }
}

TEST(Os2, ATableShorterThanItsVersionIsCorrupt) {
  // Reporting the fields that are there and quietly demoting the version would
  // answer a question about sCapHeight with a zero that looks like the font's
  // own value, and a caller cannot tell those apart.
  gfnttest::Os2Spec spec;
  spec.version = 4;
  std::vector<uint8_t> os2 = gfnttest::build_os2(spec);
  os2.resize(86);

  Font font({os2_table(os2)});
  const GFNT_Os2 * parsed = nullptr;
  EXPECT_EQ(gfnt_face_os2(font.face, &parsed, &font.error), GFNT_ERR_CORRUPT);
  EXPECT_EQ(font.error.table, GFNT_TAG('O', 'S', '/', '2'));
  EXPECT_NE(font.error.message, nullptr);
}

TEST(Os2, AVersionThisLibraryDoesNotReadIsUnsupported) {
  gfnttest::Os2Spec spec;
  spec.version = 4;
  std::vector<uint8_t> os2 = gfnttest::build_os2(spec);
  gfnttest::patch_u16(os2, 0, 6);

  Font font({os2_table(os2)});
  const GFNT_Os2 * parsed = nullptr;
  EXPECT_EQ(gfnt_face_os2(font.face, &parsed, &font.error),
      GFNT_ERR_UNSUPPORTED);
}

TEST(Os2, FsTypeIsReportedAndNeverEnforced) {
  // design.md section 17.9: a library that refuses to render a font over an
  // embedding bit breaks every legitimate viewer. Bits 1 and 2 here are the
  // most restrictive setting a font can carry.
  gfnttest::Os2Spec spec;
  spec.fs_type = 0x0002;
  Font font({os2_table(gfnttest::build_os2(spec)),
      head_table(gfnttest::build_head()),
      maxp_table(gfnttest::build_maxp(3))});
  const GFNT_Os2 * os2 = nullptr;
  size_t count = 0;

  ASSERT_EQ(gfnt_face_os2(font.face, &os2, nullptr), GFNT_OK);
  EXPECT_EQ(os2->fs_type, 0x0002u);
  EXPECT_EQ(gfnt_face_num_glyphs(font.face, &count, nullptr), GFNT_OK)
      << "and nothing else about the face is refused because of it";
}

TEST(Post, ParsesTheHeaderOfEveryVersion) {
  for (uint32_t version : {0x00010000u, 0x00020000u, 0x00030000u}) {
    Font font({post_table(gfnttest::build_post(version))});
    const GFNT_Post * post = nullptr;
    ASSERT_EQ(gfnt_face_post(font.face, &post, &font.error), GFNT_OK)
        << "version " << version;
    EXPECT_EQ(static_cast<uint32_t>(post->version), version);
    EXPECT_EQ(post->italic_angle, static_cast<GFNT_F16Dot16>(0xFFF40000))
        << "-12 degrees, in 16.16";
    EXPECT_EQ(post->underline_position, -75);
    EXPECT_EQ(post->underline_thickness, 50);
    EXPECT_EQ(post->is_fixed_pitch, 1u);
  }
}

TEST(Post, ATruncatedHeaderIsCorrupt) {
  std::vector<uint8_t> post = gfnttest::build_post();
  post.resize(20);
  Font font({post_table(post)});
  const GFNT_Post * parsed = nullptr;
  EXPECT_EQ(gfnt_face_post(font.face, &parsed, nullptr), GFNT_ERR_CORRUPT);
}

TEST(LineMetrics, TheZeroPolicyIsTheFontsOwnRequest) {
  // M4 and design.md section 10.3: fsSelection bit 7 is the font saying "use
  // my typographic metrics", and a font that does not set it wanted hhea's.
  gfnttest::Os2Spec typo;
  typo.fs_selection = GFNT_OS2_USE_TYPO_METRICS;
  Font asks_typo({os2_table(gfnttest::build_os2(typo)),
      hhea_table(gfnttest::build_hhea(1000, -300, 50, 2))});
  Font asks_hhea({os2_table(gfnttest::build_os2(gfnttest::Os2Spec{})),
      hhea_table(gfnttest::build_hhea(1000, -300, 50, 2))});

  GFNT_LineMetrics metrics{};
  ASSERT_EQ(gfnt_face_line_metrics(asks_typo.face, GFNT_LINE_METRICS_FONT,
                nullptr, &metrics, nullptr),
      GFNT_OK);
  EXPECT_EQ(metrics.source, GFNT_LINE_METRICS_TYPO);
  EXPECT_EQ(metrics.ascent, 800);
  EXPECT_EQ(metrics.descent, -200);
  EXPECT_EQ(metrics.line_gap, 100);

  ASSERT_EQ(gfnt_face_line_metrics(asks_hhea.face, GFNT_LINE_METRICS_FONT,
                nullptr, &metrics, nullptr),
      GFNT_OK);
  EXPECT_EQ(metrics.source, GFNT_LINE_METRICS_HHEA);
  EXPECT_EQ(metrics.ascent, 1000);
  EXPECT_EQ(metrics.descent, -300);
  EXPECT_EQ(metrics.line_gap, 50);
}

TEST(LineMetrics, EachExplicitPolicyReadsItsOwnTable) {
  gfnttest::Os2Spec spec;
  Font font({os2_table(gfnttest::build_os2(spec)),
      hhea_table(gfnttest::build_hhea(1000, -300, 50, 2))});
  GFNT_LineMetrics metrics{};

  ASSERT_EQ(gfnt_face_line_metrics(font.face, GFNT_LINE_METRICS_TYPO, nullptr,
                &metrics, nullptr),
      GFNT_OK);
  EXPECT_EQ(metrics.source, GFNT_LINE_METRICS_TYPO);
  EXPECT_EQ(metrics.ascent, 800);

  ASSERT_EQ(gfnt_face_line_metrics(font.face, GFNT_LINE_METRICS_HHEA, nullptr,
                &metrics, nullptr),
      GFNT_OK);
  EXPECT_EQ(metrics.source, GFNT_LINE_METRICS_HHEA);
  EXPECT_EQ(metrics.ascent, 1000);

  ASSERT_EQ(gfnt_face_line_metrics(font.face, GFNT_LINE_METRICS_WIN, nullptr,
                &metrics, nullptr),
      GFNT_OK);
  EXPECT_EQ(metrics.source, GFNT_LINE_METRICS_WIN);
  EXPECT_EQ(metrics.ascent, 900);
}

TEST(LineMetrics, DescentIsNegativeWhicheverTableAnswered) {
  // M18. usWinDescent is unsigned and positive in the file; sTypoDescender and
  // hhea's descender are negative there. One convention comes out.
  gfnttest::Os2Spec spec;
  Font font({os2_table(gfnttest::build_os2(spec)),
      hhea_table(gfnttest::build_hhea(1000, -300, 50, 2))});

  for (GFNT_LineMetricsPolicy policy : {GFNT_LINE_METRICS_TYPO,
           GFNT_LINE_METRICS_HHEA, GFNT_LINE_METRICS_WIN}) {
    GFNT_LineMetrics metrics{};
    ASSERT_EQ(gfnt_face_line_metrics(font.face, policy, nullptr, &metrics,
                  nullptr),
        GFNT_OK);
    EXPECT_LT(metrics.descent, 0) << "policy " << policy;
    EXPECT_GT(metrics.ascent, 0) << "policy " << policy;
  }
}

TEST(LineMetrics, TheWindowMetricsCarryNoGapOfTheirOwn) {
  // Windows took its external leading from hhea, so borrowing hhea's lineGap
  // here would report a number the caller's policy did not ask for.
  gfnttest::Os2Spec spec;
  Font font({os2_table(gfnttest::build_os2(spec)),
      hhea_table(gfnttest::build_hhea(1000, -300, 50, 2))});
  GFNT_LineMetrics metrics{};

  ASSERT_EQ(gfnt_face_line_metrics(font.face, GFNT_LINE_METRICS_WIN, nullptr,
                &metrics, nullptr),
      GFNT_OK);
  EXPECT_EQ(metrics.line_gap, 0);
  EXPECT_EQ(metrics.descent, -250);
}

TEST(LineMetrics, TheFontPolicyFallsBackToTheWindowMetrics) {
  gfnttest::Os2Spec spec;
  Font font({os2_table(gfnttest::build_os2(spec))});
  GFNT_LineMetrics metrics{};

  ASSERT_EQ(gfnt_face_line_metrics(font.face, GFNT_LINE_METRICS_FONT, nullptr,
                &metrics, nullptr),
      GFNT_OK);
  EXPECT_EQ(metrics.source, GFNT_LINE_METRICS_WIN)
      << "with no hhea and no typo request, the window metrics are what is "
         "left";
}

TEST(LineMetrics, AFontThatStatesNoLineHeightIsUnsupported) {
  Font font({maxp_table(gfnttest::build_maxp(3))});
  GFNT_LineMetrics metrics{};
  EXPECT_EQ(gfnt_face_line_metrics(font.face, GFNT_LINE_METRICS_FONT, nullptr,
                &metrics, &font.error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(gfnt_face_line_metrics(font.face, GFNT_LINE_METRICS_TYPO, nullptr,
                &metrics, nullptr),
      GFNT_ERR_UNSUPPORTED);
}

TEST(LineMetrics, APolicyThisLibraryDoesNotDefineIsACallerError) {
  Font font({hhea_table(gfnttest::build_hhea(1000, -300, 50, 2))});
  GFNT_LineMetrics metrics{};
  EXPECT_EQ(gfnt_face_line_metrics(font.face,
                static_cast<GFNT_LineMetricsPolicy>(99), nullptr, &metrics,
                nullptr),
      GFNT_ERR_INVALID);
}

TEST(Metrics, EveryAccessorRefusesNullArguments) {
  Font font({head_table(gfnttest::build_head())});
  const GFNT_Head * head = nullptr;
  const GFNT_Hhea * hhea = nullptr;
  const GFNT_Os2 * os2 = nullptr;
  const GFNT_Post * post = nullptr;
  int32_t value = 0;
  size_t count = 0;
  uint16_t upem = 0;
  GFNT_LineMetrics metrics{};

  EXPECT_EQ(gfnt_face_head(nullptr, &head, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_head(font.face, nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_hhea(nullptr, &hhea, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_hhea(font.face, nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_os2(nullptr, &os2, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_os2(font.face, nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_post(nullptr, &post, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_post(font.face, nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_units_per_em(font.face, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_num_glyphs(nullptr, &count, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_num_glyphs(font.face, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_FALSE(gfnt_face_num_glyphs_disagreement(nullptr, nullptr));
  EXPECT_EQ(gfnt_face_glyph_advance(nullptr, 0, nullptr, &value, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_advance(font.face, 0, nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_side_bearing(nullptr, 0, nullptr, &value, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_side_bearing(font.face, 0, nullptr, nullptr,
                nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_line_metrics(nullptr, GFNT_LINE_METRICS_FONT, nullptr,
                &metrics, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_line_metrics(font.face, GFNT_LINE_METRICS_FONT, nullptr,
                nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(upem, 0u);
}

TEST(MetricsDump, EachTableNamesItsFieldsAndReportsEveryWriteFailure) {
  gfnttest::Os2Spec spec;
  spec.version = 5;
  Font font({head_table(gfnttest::build_head(2048)),
      hhea_table(gfnttest::build_hhea(1900, -500, 0, 3)),
      os2_table(gfnttest::build_os2(spec)),
      post_table(gfnttest::build_post(0x00020000))});

  const GFNT_Head * head = nullptr;
  const GFNT_Hhea * hhea = nullptr;
  const GFNT_Os2 * os2 = nullptr;
  const GFNT_Post * post = nullptr;
  ASSERT_EQ(gfnt_face_head(font.face, &head, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_hhea(font.face, &hhea, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_os2(font.face, &os2, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_post(font.face, &post, nullptr), GFNT_OK);

  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  ASSERT_EQ(gfnt_head_dump(head, out.get()), GFNT_OK);
  ASSERT_EQ(gfnt_hhea_dump(hhea, out.get()), GFNT_OK);
  ASSERT_EQ(gfnt_os2_dump(os2, out.get()), GFNT_OK);
  ASSERT_EQ(gfnt_post_dump(post, out.get()), GFNT_OK);
  std::string text = out.finish();

  EXPECT_NE(text.find("unitsPerEm 2048"), std::string::npos) << text;
  EXPECT_NE(text.find("numberOfHMetrics 3"), std::string::npos) << text;
  EXPECT_NE(text.find("vendor 'GHTI'"), std::string::npos) << text;
  EXPECT_NE(text.find("opticalSize 80..240"), std::string::npos) << text;
  EXPECT_NE(text.find("underlineThickness 50"), std::string::npos) << text;

  // Every fprintf in every dumper is checked, and the sweep is what runs those
  // arms. The floors are the line counts, so a dumper that grows a line
  // without a check fails here rather than silently.
  struct Dumper {
    const char * name;
    GFNT_Result (*dump)(const void *, FILE *);
    const void * table;
    size_t lines;
  };
  size_t head_failures = 0;
  size_t hhea_failures = 0;
  size_t os2_failures = 0;
  size_t post_failures = 0;

  for (size_t allow = 0; allow < 8; allow++) {
    gfnttest::FailingSink sink(allow);
    ASSERT_NE(sink.get(), nullptr);
    if (gfnt_head_dump(head, sink.get()) == GFNT_ERR_IO) {
      head_failures++;
    }
  }
  for (size_t allow = 0; allow < 8; allow++) {
    gfnttest::FailingSink sink(allow);
    ASSERT_NE(sink.get(), nullptr);
    if (gfnt_hhea_dump(hhea, sink.get()) == GFNT_ERR_IO) {
      hhea_failures++;
    }
  }
  for (size_t allow = 0; allow < 8; allow++) {
    gfnttest::FailingSink sink(allow);
    ASSERT_NE(sink.get(), nullptr);
    if (gfnt_os2_dump(os2, sink.get()) == GFNT_ERR_IO) {
      os2_failures++;
    }
  }
  for (size_t allow = 0; allow < 8; allow++) {
    gfnttest::FailingSink sink(allow);
    ASSERT_NE(sink.get(), nullptr);
    if (gfnt_post_dump(post, sink.get()) == GFNT_ERR_IO) {
      post_failures++;
    }
  }
  EXPECT_EQ(head_failures, 4u) << "head dumps four lines";
  EXPECT_EQ(hhea_failures, 3u) << "hhea dumps three lines";
  EXPECT_EQ(os2_failures, 7u) << "OS/2 dumps seven lines";
  EXPECT_EQ(post_failures, 3u) << "post dumps three lines";
}

TEST(MetricsDump, EveryDumperRefusesNulls) {
  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  GFNT_Head head{};
  GFNT_Hhea hhea{};
  GFNT_Os2 os2{};
  GFNT_Post post{};

  EXPECT_EQ(gfnt_head_dump(nullptr, out.get()), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_head_dump(&head, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_hhea_dump(nullptr, out.get()), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_hhea_dump(&hhea, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_os2_dump(nullptr, out.get()), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_os2_dump(&os2, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_post_dump(nullptr, out.get()), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_post_dump(&post, nullptr), GFNT_ERR_INVALID);
}

// design.md section 15.3: a face is immutable after load except for the table
// memos, which are guarded, so one face may be shared read-only across
// threads. This is a smoke test for that claim rather than a proof of it - the
// proof is a TSan run, which this library does not have a target for yet.
TEST(Metrics, OneFaceServesManyThreads) {
  gfnttest::Os2Spec spec;
  Font font({head_table(gfnttest::build_head(2048)),
      maxp_table(gfnttest::build_maxp(3)),
      hhea_table(gfnttest::build_hhea(1900, -500, 0, 3)),
      hmtx_table(gfnttest::build_hmtx({{500, 10}, {600, 20}, {700, 30}})),
      os2_table(gfnttest::build_os2(spec))});

  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int i = 0; i < 8; i++) {
    threads.emplace_back([&font, &failures]() {
      for (int pass = 0; pass < 50; pass++) {
        const GFNT_Head * head = nullptr;
        const GFNT_Os2 * os2 = nullptr;
        size_t count = 0;
        int32_t advance = 0;
        if (gfnt_face_head(font.face, &head, nullptr) != GFNT_OK
            || head->units_per_em != 2048
            || gfnt_face_os2(font.face, &os2, nullptr) != GFNT_OK
            || gfnt_face_num_glyphs(font.face, &count, nullptr) != GFNT_OK
            || count != 3
            || gfnt_face_glyph_advance(font.face, 2, nullptr, &advance,
                   nullptr)
                != GFNT_OK
            || advance != 700) {
          failures++;
        }
      }
    });
  }
  for (auto & thread : threads) {
    thread.join();
  }
  EXPECT_EQ(failures.load(), 0);
}

TEST(Os2, ATableCutToEveryLengthIsRefusedRatherThanPartlyParsed) {
  // Version 5 so that every version's own fields are in the table: the arms that
  // read the code page ranges, the version 2 block and the optical sizes are
  // each reached only by a table that claims to have them. The whole-font
  // truncation sweep cuts `OS/2` too, but its fixture is one version, so the
  // later blocks were never entered at all.
  gfnttest::Os2Spec spec;
  spec.version = 5;
  const std::vector<uint8_t> whole = gfnttest::build_os2(spec);
  size_t refusals = 0;

  for (size_t cut = 0; cut < whole.size(); cut++) {
    Font font({os2_table(std::vector<uint8_t>(whole.begin(),
        whole.begin() + cut))});
    const GFNT_Os2 * os2 = nullptr;
    GFNT_Error error{};

    // The face itself must load, or the refusal below would be "there is no
    // face" rather than "this table is short" - which is how this test passed
    // while reaching none of the arms it names.
    ASSERT_NE(font.face, nullptr) << "OS/2 cut to " << cut;
    if (gfnt_face_os2(font.face, &os2, &error) == GFNT_ERR_CORRUPT) {
      refusals++;
      continue;
    }
    ADD_FAILURE() << "OS/2 cut to " << cut << " of " << whole.size()
                  << " parsed anyway";
  }
  EXPECT_EQ(refusals, whole.size()) << "every short table is refused";

  // The control: uncut, it parses and the last field is the one only version 5
  // has.
  Font font({os2_table(whole)});
  const GFNT_Os2 * os2 = nullptr;
  ASSERT_EQ(gfnt_face_os2(font.face, &os2, nullptr), GFNT_OK);
  EXPECT_EQ(os2->version, 5);
  // The two fields only version 5 has, which is what makes this table the one
  // whose every block is entered.
  EXPECT_NE(os2->upper_optical_size, 0);
}

TEST(Metrics, AnHmtxShorterThanHheaPromisesRefusesThoseGlyphs) {
  // `hhea` says three long entries and `hmtx` holds two. The advance and the
  // side bearing are separate reads at separate offsets, so each has its own
  // arm, and the numGlyphs minimum cannot hide this: it is computed from what
  // `hmtx` can hold, and this table's own header is what disagrees with it.
  std::vector<uint8_t> hmtx = gfnttest::build_hmtx({{500, 10}, {600, 20}}, {});
  Font font({
      {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, 3)},
      {GFNT_TAG('h', 'm', 't', 'x'), hmtx},
      {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(3)},
  });
  int32_t advance = 0;
  int32_t bearing = 0;
  GFNT_Error error{};

  EXPECT_EQ(gfnt_face_glyph_advance(font.face, 0, nullptr, &advance, &error),
      GFNT_OK) << "the entries that are there still answer";
  EXPECT_EQ(advance, 500);

  const GFNT_Result missing_advance = gfnt_face_glyph_advance(font.face, 2,
      nullptr, &advance, &error);
  const GFNT_Result missing_bearing = gfnt_face_glyph_side_bearing(font.face, 2,
      nullptr, &bearing, &error);
  EXPECT_NE(missing_advance, GFNT_OK);
  EXPECT_NE(missing_bearing, GFNT_OK);

  // And a glyph the face does not have at all, through the side bearing, which
  // has its own propagation to do.
  EXPECT_NE(gfnt_face_glyph_side_bearing(font.face, 99, nullptr, &bearing,
      &error), GFNT_OK);
}

TEST(Metrics, EachLineMetricsPolicyReportsItsOwnTablesFailure) {
  // Every policy reads a different table, so "the font cannot answer" has three
  // sources - and a policy that fell back silently to a table the caller did not
  // ask for is what M8 is about.
  Font broken_os2({
      os2_table(std::vector<uint8_t>(20, 0)),
      {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, 1)},
  });
  Font broken_hhea({
      os2_table(gfnttest::build_os2(gfnttest::Os2Spec{})),
      {GFNT_TAG('h', 'h', 'e', 'a'), std::vector<uint8_t>(8, 0)},
  });
  GFNT_LineMetrics metrics{};
  GFNT_Error error{};

  EXPECT_NE(gfnt_face_line_metrics(broken_os2.face, GFNT_LINE_METRICS_TYPO,
      nullptr, &metrics, &error), GFNT_OK);
  EXPECT_NE(gfnt_face_line_metrics(broken_os2.face, GFNT_LINE_METRICS_WIN,
      nullptr, &metrics, &error), GFNT_OK);
  EXPECT_NE(gfnt_face_line_metrics(broken_hhea.face, GFNT_LINE_METRICS_HHEA,
      nullptr, &metrics, &error), GFNT_OK);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
