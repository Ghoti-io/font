/**
 * @file
 *
 * The truncation sweep: cut each table to every length it could have, and
 * check that the face still obeys property 1.
 *
 * documentation/design.md section 14.1 property 1 and section 14.3. Cutting a
 * font at its end shortens only its *last* table, which is why this sweep
 * truncates each table in turn through its directory length, with the directory
 * otherwise intact. The property is that a truncated font either answers as the
 * whole font did for the data that survived, or refuses with a diagnostic
 * naming the table - never a third thing, and never a crash.
 *
 * Two answers are allowed to change, and both are design decisions rather than
 * exceptions carved out to make the sweep pass:
 *
 * - **The glyph count may shrink.** numGlyphs is the minimum across every table
 *   that indexes glyphs (M12), so a cut `hmtx` legitimately means fewer glyphs.
 *   It may never grow.
 * - **A glyph at or past that count is ERR_INVALID.** That is the same refusal
 *   the whole font gives for a glyph it does not have.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <string>
#include <vector>

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/name.h>

using gfnttest::Table;

namespace {

/** The codepoints the sweep asks about. */
const uint32_t kCodepoints[] = {0x41, 0x42, 0x45, 0x2603, 0x1F600};

/** Everything the sweep compares, from the whole font and from each cut one. */
struct Answers {
  bool units_ok = false;
  uint16_t units = 0;
  bool count_ok = false;
  size_t count = 0;
  std::vector<int32_t> advances;   // by glyph, for glyphs below the count
  std::vector<bool> advance_ok;
  std::vector<uint32_t> glyphs;    // by codepoint
  std::vector<bool> glyph_ok;
  bool family_ok = false;
  std::string family;
  bool metrics_ok = false;
  GFNT_LineMetrics metrics{};
  bool os2_ok = false;
  uint16_t weight = 0;
  bool post_ok = false;
  GFNT_F16Dot16 italic = 0;
};

/**
 * Ask a face everything, recording what failed rather than asserting, so that
 * a cut font's refusals are data.
 */
Answers interrogate(GFNT_Face * face, size_t glyph_slots) {
  Answers answers;
  GFNT_Error error;

  gfnt_error_clear(&error);
  answers.units_ok =
      gfnt_face_units_per_em(face, &answers.units, &error) == GFNT_OK;
  answers.count_ok = gfnt_face_num_glyphs(face, &answers.count, &error)
      == GFNT_OK;

  answers.advances.resize(glyph_slots, 0);
  answers.advance_ok.resize(glyph_slots, false);
  for (size_t glyph = 0; glyph < glyph_slots; glyph++) {
    int32_t advance = 0;
    answers.advance_ok[glyph] = gfnt_face_glyph_advance(face,
                                    (uint32_t)glyph, nullptr, &advance, &error)
        == GFNT_OK;
    answers.advances[glyph] = advance;
  }

  for (uint32_t codepoint : kCodepoints) {
    uint32_t glyph = 0;
    bool ok = gfnt_face_glyph_for_codepoint(face, codepoint, &glyph, &error)
        == GFNT_OK;
    answers.glyph_ok.push_back(ok);
    answers.glyphs.push_back(glyph);
  }

  char * text = nullptr;
  if (gfnt_face_name(face, GFNT_NAME_FAMILY, GFNT_LANGUAGE_ANY, nullptr, &text,
          nullptr, &error)
      == GFNT_OK) {
    answers.family_ok = true;
    answers.family = text;
    gfnt_name_free(nullptr, text);
  }

  answers.metrics_ok = gfnt_face_line_metrics(face, GFNT_LINE_METRICS_FONT,
                           nullptr, &answers.metrics, &error)
      == GFNT_OK;

  // OS/2 and post are asked about so that every table in the fixture is one
  // some query depends on: a table nothing reads could be cut to nothing and
  // the sweep would report that as a font behaving perfectly.
  const GFNT_Os2 * os2 = nullptr;
  if (gfnt_face_os2(face, &os2, &error) == GFNT_OK) {
    answers.os2_ok = true;
    answers.weight = os2->weight_class;
  }
  const GFNT_Post * post = nullptr;
  if (gfnt_face_post(face, &post, &error) == GFNT_OK) {
    answers.post_ok = true;
    answers.italic = post->italic_angle;
  }
  return answers;
}

/**
 * A font with one of everything this library reads, built so that the sweep has
 * no ambiguity to tolerate: the two `cmap` subtables map the swept codepoints
 * identically, so a fallback from one to the other cannot change an answer, and
 * there is exactly one `name` record per name ID, so passing over an unreadable
 * record cannot substitute a different string.
 */
std::vector<Table> whole_font() {
  return {
      {GFNT_TAG('O', 'S', '/', '2'), gfnttest::build_os2(gfnttest::Os2Spec{})},
      {GFNT_TAG('c', 'm', 'a', 'p'), gfnttest::build_cmap({
           {3, 1, gfnttest::build_cmap_format4({
                {0x41, 0x45, static_cast<int16_t>(-0x41 + 1), {}},
                {0xFFFF, 0xFFFF, 1, {}},
            })},
           {3, 10, gfnttest::build_cmap_format12({{0x41, 0x45, 1}})},
       })},
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000)},
      {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 100, 3)},
      {GFNT_TAG('h', 'm', 't', 'x'),
          gfnttest::build_hmtx({{500, 10}, {600, 20}, {700, 30}}, {40, 50})},
      {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(5)},
      {GFNT_TAG('n', 'a', 'm', 'e'), gfnttest::build_name({
           {3, 1, 0x409, GFNT_NAME_FAMILY, gfnttest::utf16be("Ghoti Sweep")},
       })},
      {GFNT_TAG('p', 'o', 's', 't'), gfnttest::build_post()},
  };
}

} // namespace

TEST(Truncation, EveryTableCutToEveryLengthObeysPropertyOne) {
  const std::vector<Table> tables = whole_font();
  const std::vector<uint8_t> whole =
      gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
  const size_t glyph_slots = 7;  // five glyphs, and two past the end

  Answers reference;
  {
    GFNT_Blob * blob = nullptr;
    GFNT_Face * face = nullptr;
    ASSERT_EQ(gfnt_blob_create_memory(whole.data(), whole.size(),
                  GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
        GFNT_OK);
    ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr),
        GFNT_OK);
    reference = interrogate(face, glyph_slots);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  // The instrument itself: if the whole font cannot answer, the sweep below
  // would be comparing failures with failures and would pass on anything.
  ASSERT_TRUE(reference.units_ok);
  ASSERT_TRUE(reference.count_ok);
  ASSERT_EQ(reference.count, 5u);
  ASSERT_TRUE(reference.family_ok);
  ASSERT_EQ(reference.family, "Ghoti Sweep");
  ASSERT_TRUE(reference.metrics_ok);
  ASSERT_EQ(reference.metrics.source, GFNT_LINE_METRICS_HHEA)
      << "this fixture's OS/2 does not set the typographic bit, so the font's "
         "own request is hhea";
  ASSERT_TRUE(reference.os2_ok);
  ASSERT_TRUE(reference.post_ok);
  for (size_t i = 0; i < 5; i++) {
    ASSERT_TRUE(reference.advance_ok[i]) << "glyph " << i;
  }
  ASSERT_FALSE(reference.advance_ok[5]) << "glyph 5 is past the count";
  for (size_t i = 0; i < GFNT_ARRAY_SIZE(kCodepoints); i++) {
    ASSERT_TRUE(reference.glyph_ok[i]);
  }
  ASSERT_EQ(reference.glyphs[0], 1u) << "'A'";

  size_t cuts = 0;
  size_t expected_cuts = 0;
  size_t refusals = 0;
  size_t survivals = 0;
  size_t shrunken_counts = 0;
  std::vector<size_t> refusals_per_table(tables.size(), 0);

  for (const Table & table : tables) {
    // Every length from 0 to the table's own, inclusive.
    expected_cuts += table.data.size() + 1;
  }

  for (size_t index = 0; index < tables.size(); index++) {
    const size_t full = tables[index].data.size();
    char tag_text[5];
    gfnt_tag_string(tables[index].tag, tag_text);

    for (size_t length = 0; length <= full; length++) {
      std::vector<uint8_t> bytes = whole;
      // The directory length is what bounds the table's extent, so cutting it
      // is what a truncated table means to this reader - and the bytes beyond
      // are still there, which is the case a reader that trusted the file
      // rather than the extent would walk into.
      gfnttest::patch_u32(bytes, gfnttest::entry_offset(0, index) + 12,
          static_cast<uint32_t>(length));

      GFNT_Blob * blob = nullptr;
      GFNT_Face * face = nullptr;
      ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                    GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
          GFNT_OK);
      GFNT_Result loaded = gfnt_face_load(blob, 0, nullptr, nullptr, &face,
          nullptr);
      // The directory still describes tables inside the blob, so the load
      // itself must always succeed: a shorter table is not a broken font until
      // something reads it.
      ASSERT_EQ(loaded, GFNT_OK)
          << "table '" << tag_text << "' cut to " << length;

      Answers cut = interrogate(face, glyph_slots);
      cuts++;

      std::string where = std::string("table '") + tag_text + "' cut to "
          + std::to_string(length) + " of " + std::to_string(full);

      if (cut.units_ok) {
        EXPECT_EQ(cut.units, reference.units) << where;
      }
      if (cut.count_ok) {
        EXPECT_LE(cut.count, reference.count)
            << where << ": the glyph count may shrink, never grow";
        if (cut.count < reference.count) {
          shrunken_counts++;
        }
      }
      for (size_t glyph = 0; glyph < glyph_slots; glyph++) {
        if (!cut.advance_ok[glyph]) {
          continue;
        }
        ASSERT_TRUE(cut.count_ok) << where;
        EXPECT_LT(glyph, cut.count)
            << where << ": an advance answered for a glyph the face says it "
                        "does not have";
        EXPECT_EQ(cut.advances[glyph], reference.advances[glyph])
            << where << ", glyph " << glyph;
      }
      for (size_t i = 0; i < GFNT_ARRAY_SIZE(kCodepoints); i++) {
        if (cut.glyph_ok[i]) {
          EXPECT_EQ(cut.glyphs[i], reference.glyphs[i])
              << where << ", codepoint " << kCodepoints[i];
        }
      }
      if (cut.family_ok) {
        EXPECT_EQ(cut.family, reference.family) << where;
      }
      if (cut.metrics_ok) {
        // The zero policy is the font's own request, and a font whose hhea no
        // longer parses has asked for something else (design.md section 10.3).
        // So the source may change - to the window metrics, which is the
        // documented last resort - and the numbers change with it. What must
        // hold is that the answer says which table it came from, because a
        // fallback nobody can audit is the thing M8 is about.
        if (cut.metrics.source == reference.metrics.source) {
          EXPECT_EQ(cut.metrics.ascent, reference.metrics.ascent) << where;
          EXPECT_EQ(cut.metrics.descent, reference.metrics.descent) << where;
        }
        else {
          EXPECT_EQ(cut.metrics.source, GFNT_LINE_METRICS_WIN)
              << where << ": the only documented fallback from hhea";
          EXPECT_FALSE(cut.metrics_ok && cut.units_ok
              && cut.metrics.source == GFNT_LINE_METRICS_TYPO)
              << where << ": nothing here sets the typographic bit";
        }
      }
      if (cut.os2_ok) {
        EXPECT_EQ(cut.weight, reference.weight) << where;
      }
      if (cut.post_ok) {
        EXPECT_EQ(cut.italic, reference.italic) << where;
      }

      if (length == full) {
        // The control at the top of each table's sweep: an untruncated table
        // must answer exactly as the reference did, or the comparisons above
        // are being made against something that was already failing.
        EXPECT_TRUE(cut.units_ok) << where;
        EXPECT_TRUE(cut.count_ok) << where;
        EXPECT_EQ(cut.count, reference.count) << where;
        EXPECT_TRUE(cut.family_ok) << where;
        survivals++;
      }
      else {
        // A refusal, for the sweep's own bookkeeping, is any answer the whole
        // font gave that this cut font does not: a query that now fails, a
        // glyph count that shrank, or a line-metrics policy that fell back.
        // Counting only outright failures missed hhea entirely - cutting it
        // leaves every table this sweep reads intact and only takes the
        // advances and the metrics source with it.
        bool changed = !cut.units_ok || !cut.count_ok || !cut.family_ok
            || !cut.os2_ok || !cut.post_ok || cut.count < reference.count
            || cut.metrics_ok != reference.metrics_ok
            || (cut.metrics_ok
                && cut.metrics.source != reference.metrics.source);

        for (size_t glyph = 0; glyph < glyph_slots && !changed; glyph++) {
          changed = reference.advance_ok[glyph] && !cut.advance_ok[glyph];
        }
        for (size_t i = 0; i < GFNT_ARRAY_SIZE(kCodepoints) && !changed; i++) {
          changed = reference.glyph_ok[i] && !cut.glyph_ok[i];
        }
        if (changed) {
          refusals++;
          refusals_per_table[index]++;
        }
      }

      gfnt_face_free(face);
      gfnt_blob_destroy(blob);
    }
  }

  // The sweep's own denominators, derived from the fixture rather than guessed:
  // a sweep that stopped cutting anything - a builder change, a patched offset
  // that lands in padding - would otherwise report a clean pass over nothing.
  EXPECT_EQ(cuts, expected_cuts)
      << "the sweep must cover every length of every table";
  EXPECT_EQ(survivals, tables.size())
      << "every table's full length must be one of the cases";
  EXPECT_GT(refusals, tables.size())
      << "a sweep where nothing refuses is measuring nothing";
  for (size_t index = 0; index < tables.size(); index++) {
    char tag_text[5];
    gfnt_tag_string(tables[index].tag, tag_text);
    EXPECT_GT(refusals_per_table[index], 0u)
        << "cutting '" << tag_text << "' changed no answer at any length, so "
           "nothing this sweep asks about depends on it";
  }
  EXPECT_GT(shrunken_counts, 0u)
      << "cutting hmtx must reduce the glyph count, or M12's minimum is not "
         "being taken";
}

// Section 14.3's other half: cutting the *file* shortens only the last table,
// and the directory then describes tables that are not there. That is a load
// failure rather than a lazy one, because the directory is what the load reads.
TEST(Truncation, CuttingTheFileIsRefusedAtLoad) {
  const std::vector<uint8_t> whole =
      gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, whole_font());
  size_t refusals = 0;

  for (size_t length = 0; length < whole.size(); length++) {
    std::vector<uint8_t> bytes(whole.begin(), whole.begin() + length);
    GFNT_Blob * blob = nullptr;
    GFNT_Face * face = nullptr;
    GFNT_Error error;

    gfnt_error_clear(&error);
    ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                  GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr),
        GFNT_OK);
    GFNT_Result result = gfnt_face_load(blob, 0, nullptr, nullptr, &face,
        &error);
    if (result == GFNT_OK) {
      // Only possible when the cut happens to land at or past the end of every
      // table the directory names, which it cannot here: the last table's
      // bytes are the last bytes of the file.
      ADD_FAILURE() << "a file cut to " << length << " of " << whole.size()
                    << " loaded";
      gfnt_face_free(face);
    }
    else {
      EXPECT_TRUE(result == GFNT_ERR_CORRUPT || result == GFNT_ERR_FORMAT)
          << "at " << length << " bytes";
      EXPECT_NE(error.message, nullptr) << "at " << length << " bytes";
      refusals++;
    }
    gfnt_blob_destroy(blob);
  }
  EXPECT_EQ(refusals, whole.size())
      << "every prefix of this font is either a corrupt font or not a font";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
