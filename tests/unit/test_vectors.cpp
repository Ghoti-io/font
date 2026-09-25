/**
 * @file
 *
 * The generated vectors, against the committed text they were generated beside.
 *
 * `src/tables/post_names.h` and `src/name/mac_encodings.h` are generated from
 * the pinned `fonttools` image by `tools/vectors/make_vectors.py`
 * (documentation/design.md section 14: a vector comes from an oracle and is
 * never written from memory). The same run writes the text
 * files under `tests/data/vectors/`, and this suite checks the library against
 * them.
 *
 * **What this gate can and cannot see, stated because the split is the point.**
 * The text and the headers come from one generator, so agreement here does not
 * mean the generator is right - `ttx_diff` is what checks the tables against
 * reality, over 346 faces. What this catches is a table **edited**: a codepoint
 * "fixed" by hand, a name corrected from memory, a row deleted. It does that on
 * a fresh clone with no container engine, which `check-vectors` cannot, and
 * that is why both exist.
 *
 * Every table entry is reached through the public API rather than by including
 * the generated header: `post-v1.ttf` is the standard glyph order, so naming its
 * 258 glyphs walks the whole vector, and `name-mac-encodings.ttf` carries all
 * 128 high bytes of each Macintosh encoding, so decoding its records walks all
 * eight tables. The fixtures were built for exactly this.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/name.h>

namespace {

/** A fixture read from disk, with a blob and a face over it. */
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
    EXPECT_EQ(result, GFNT_OK) << name << ": " << error.message;
  }

  ~Fixture() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }

  Fixture(const Fixture &) = delete;
  Fixture & operator=(const Fixture &) = delete;
};

/**
 * Every non-comment, non-blank line of a committed vector file, split on tabs.
 *
 * A file that cannot be opened returns nothing, and every caller below checks
 * the row count against what the generator writes - so a missing or truncated
 * vector file fails rather than passing with nothing to compare. That is the
 * denominator this suite would otherwise be assuming.
 */
std::vector<std::vector<std::string>> rows(const std::string & name) {
  std::vector<std::vector<std::string>> out;
  std::ifstream stream(gfnttest::data("vectors/" + name));
  std::string line;

  while (std::getline(stream, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::vector<std::string> fields;
    std::string field;
    std::istringstream parts(line);
    while (std::getline(parts, field, '\t')) {
      fields.push_back(field);
    }
    out.push_back(fields);
  }
  return out;
}

/** Bytes written as hex, as `mac_encodings.txt` spells them, back to a string. */
std::string from_hex(const std::string & hex) {
  std::string out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

constexpr size_t kStandardNames = 258;
constexpr size_t kMacTables = 8;
constexpr size_t kHighBytes = 128;

TEST(Vectors, TheStandardGlyphOrderIsWhatTheGeneratorWrote) {
  const auto expected = rows("standard_glyph_order.txt");
  ASSERT_EQ(expected.size(), kStandardNames)
      << "tests/data/vectors/standard_glyph_order.txt should hold 258 names; "
         "run `make gen-vectors`";

  // post format 1.0 *is* the standard order, so this walks the whole vector.
  Fixture fixture("post-v1.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(fixture.face, &glyphs, nullptr), GFNT_OK);
  ASSERT_EQ(glyphs, kStandardNames)
      << "post-v1.ttf must have exactly the 258 standard glyphs";

  size_t checked = 0;
  for (size_t index = 0; index < expected.size(); ++index) {
    ASSERT_EQ(expected[index].size(), 2u) << "row " << index;
    ASSERT_EQ(expected[index][0], std::to_string(index))
        << "the file is not in index order at row " << index;

    char * name = nullptr;
    ASSERT_EQ(gfnt_face_glyph_name(fixture.face, (uint32_t)index, nullptr,
                  &name, nullptr, nullptr),
        GFNT_OK) << "glyph " << index;
    EXPECT_EQ(std::string(name), expected[index][1]) << "glyph " << index;
    gfnt_glyph_name_free(nullptr, name);
    ++checked;
  }
  EXPECT_EQ(checked, kStandardNames);
}

TEST(Vectors, TheStandardOrderIsAlsoReachedByIndexFromAFormat2Table) {
  // The same vector through the other path: format 2.0 spells the first 258
  // names as indices, and `post-v2.ttf` uses index 257 - the last standard name
  // - on purpose, because a boundary nothing crosses is a boundary nobody
  // checked. A planted off-by-one there once passed a two-font oracle run.
  Fixture fixture("post-v2.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  const auto expected = rows("standard_glyph_order.txt");
  ASSERT_EQ(expected.size(), kStandardNames);

  char * name = nullptr;
  ASSERT_EQ(gfnt_face_glyph_name(fixture.face, 5, nullptr, &name, nullptr,
                nullptr),
      GFNT_OK);
  EXPECT_EQ(std::string(name), expected[kStandardNames - 1][1])
      << "glyph 5 of post-v2.ttf is standard name 257";
  gfnt_glyph_name_free(nullptr, name);

  // And the stored-string path beside it, so the two are distinguishable.
  ASSERT_EQ(gfnt_face_glyph_name(fixture.face, 4, nullptr, &name, nullptr,
                nullptr),
      GFNT_OK);
  EXPECT_EQ(std::string(name), "ghoti.alt");
  gfnt_glyph_name_free(nullptr, name);

  uint32_t glyph = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_name(fixture.face, "ghoti.alt", &glyph,
                nullptr),
      GFNT_OK);
  EXPECT_EQ(glyph, 4u);
  EXPECT_EQ(gfnt_face_glyph_for_name(fixture.face, "notAGlyphHere", &glyph,
                nullptr),
      GFNT_ERR_INVALID);
}

TEST(Vectors, EverySingleByteMacintoshEncodingDecodesAsTheGeneratorSaid) {
  // Expected text per table: the 128 high bytes, in order, as UTF-8.
  const auto encodings = rows("mac_encodings.txt");
  ASSERT_EQ(encodings.size(), kMacTables * kHighBytes)
      << "tests/data/vectors/mac_encodings.txt should hold 8 x 128 rows; run "
         "`make gen-vectors`";

  std::map<std::string, std::string> whole;
  std::map<std::string, size_t> counted;
  for (const auto & row : encodings) {
    ASSERT_EQ(row.size(), 4u);
    whole[row[0]] += from_hex(row[3]);
    counted[row[0]] += 1;
  }
  ASSERT_EQ(whole.size(), kMacTables);
  for (const auto & pair : counted) {
    EXPECT_EQ(pair.second, kHighBytes) << pair.first;
  }

  // Which table each (encoding, language) names, from the committed selector.
  const auto selector = rows("mac_selector.txt");
  ASSERT_GT(selector.size(), 0u);
  std::map<std::string, std::string> table_for;
  for (const auto & row : selector) {
    ASSERT_EQ(row.size(), 3u);
    table_for[row[0] + "/" + row[1]] = row[2];
  }

  // The fixture carries one record per case, whose text is every high byte.
  Fixture fixture("name-mac-encodings.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  size_t count = 0;
  ASSERT_EQ(gfnt_face_name_count(fixture.face, &count, nullptr), GFNT_OK);
  ASSERT_GT(count, 0u);

  size_t checked = 0;
  std::vector<std::string> seen;
  for (size_t index = 0; index < count; ++index) {
    GFNT_NameRecord record{};
    ASSERT_EQ(gfnt_face_name_at(fixture.face, index, &record, nullptr),
        GFNT_OK);
    // Sample text (19) is the nameID the generator uses for these, so the
    // ordinary family and copyright records are left out of the comparison.
    if (record.platform_id != GFNT_PLATFORM_MACINTOSH || record.name_id != 19) {
      continue;
    }

    const std::string key = std::to_string(record.encoding_id) + "/"
        + std::to_string(record.language_id);
    std::string table;
    if (table_for.count(key)) {
      table = table_for[key];
    }
    else {
      const std::string fallback =
          std::to_string(record.encoding_id) + "/ANY";
      ASSERT_TRUE(table_for.count(fallback))
          << "the selector names no table for " << key;
      table = table_for[fallback];
    }
    ASSERT_NE(table, "-") << key << " should be a single-byte encoding";
    ASSERT_TRUE(whole.count(table)) << table;

    char * text = nullptr;
    ASSERT_EQ(gfnt_face_name_decode(fixture.face, &record, nullptr, &text,
                  nullptr, nullptr),
        GFNT_OK) << key;
    EXPECT_EQ(std::string(text), whole[table])
        << "record (encoding " << record.encoding_id << ", language "
        << record.language_id << ") should decode through " << table;
    gfnt_name_free(nullptr, text);
    seen.push_back(table);
    ++checked;
  }

  // Every table must have been reached, or this suite checked less than it
  // appears to: a fixture that lost a record would otherwise pass.
  EXPECT_EQ(checked, 11u) << "the fixture carries eleven cases";
  for (const auto & pair : whole) {
    EXPECT_NE(std::find(seen.begin(), seen.end(), pair.first), seen.end())
        << pair.first << " was never exercised";
  }
}

TEST(Vectors, TheEncodingIdAloneDoesNotDecideTheMacintoshTable) {
  // The trap this table exists for: platEncID 0 is Mac Roman for most languages
  // and five other encodings for thirteen of them. Two records that differ only
  // in langID must decode differently, or the language is being ignored.
  Fixture fixture("name-mac-encodings.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  size_t count = 0;
  ASSERT_EQ(gfnt_face_name_count(fixture.face, &count, nullptr), GFNT_OK);

  std::map<uint16_t, std::string> by_language;
  for (size_t index = 0; index < count; ++index) {
    GFNT_NameRecord record{};
    ASSERT_EQ(gfnt_face_name_at(fixture.face, index, &record, nullptr),
        GFNT_OK);
    if (record.platform_id != GFNT_PLATFORM_MACINTOSH
        || record.encoding_id != 0 || record.name_id != 19) {
      continue;
    }
    char * text = nullptr;
    ASSERT_EQ(gfnt_face_name_decode(fixture.face, &record, nullptr, &text,
                  nullptr, nullptr),
        GFNT_OK);
    by_language[record.language_id] = text;
    gfnt_name_free(nullptr, text);
  }

  // Roman, Icelandic, Turkish, Croatian, Central European and Romanian, all
  // under encoding 0.
  ASSERT_EQ(by_language.size(), 6u);
  ASSERT_TRUE(by_language.count(0));
  for (const auto & pair : by_language) {
    if (pair.first == 0) {
      continue;
    }
    EXPECT_NE(pair.second, by_language[0])
        << "language " << pair.first << " decoded identically to Mac Roman, so "
        << "the language is not reaching the table choice";
  }
}

TEST(Vectors, AMultiByteMacintoshEncodingIsRefusedAndNotGuessed) {
  // Encoding 1 is Macintosh Japanese, which is multi-byte and not tabulated.
  // The refusal is the answer: a byte-per-codepoint guess would produce
  // plausible mojibake, which is worse than an error.
  const auto selector = rows("mac_selector.txt");
  ASSERT_GT(selector.size(), 0u);

  size_t refused = 0;
  for (const auto & row : selector) {
    ASSERT_EQ(row.size(), 3u);
    if (row[2] == "-") {
      ++refused;
    }
  }
  EXPECT_EQ(refused, 4u)
      << "encodings 1, 2, 3 and 25 are the multi-byte ones; the selector "
         "should name no table for exactly those";
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
