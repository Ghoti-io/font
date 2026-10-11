/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Font.
 *
 * Ghoti.io Font is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Font is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Renaming a subset (`GFNT_SubsetOptions::family_name`): the records that state the family are
 * replaced in every language and platform, everything else is carried, a `CFF ` is named to
 * match, and nothing of the old name is left behind where a reserved name could be found.
 */

#include "test_helpers.h"
#include "failing_allocator.h"
#include "sfnt_builder.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/name.h>
#include <ghoti.io/font/write.h>

#include <gtest/gtest.h>

namespace {

using namespace gfnttest;

std::vector<uint8_t> glyph_font(std::vector<NameRecord> names, uint32_t name_format = 0) {
  const uint16_t n = 4;
  std::vector<uint8_t> hmtx;
  std::vector<uint8_t> loca;
  for (uint16_t g = 0; g < n; ++g) {
    put_u16(hmtx, 500);
    put_s16(hmtx, 0);
  }
  for (uint16_t g = 0; g <= n; ++g) {
    put_u16(loca, 0);
  }
  std::vector<Table> tables = {
      {GFNT_TAG('h', 'e', 'a', 'd'), build_head(1000, 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'), build_hhea(800, -200, 0, n)},
      {GFNT_TAG('m', 'a', 'x', 'p'), build_maxp(n)},
      {GFNT_TAG('h', 'm', 't', 'x'), hmtx},
      {GFNT_TAG('l', 'o', 'c', 'a'), loca},
      {GFNT_TAG('g', 'l', 'y', 'f'), {}},
      {GFNT_TAG('c', 'm', 'a', 'p'),
          build_cmap({{3, 10, build_cmap_format12({{0x41, 0x43, 1}})}})},
  };
  if (!names.empty()) {
    tables.push_back({GFNT_TAG('n', 'a', 'm', 'e'), build_name(names, name_format)});
  }
  return build_sfnt(0x00010000, tables);
}

NameRecord win(uint16_t id, const std::string & text, uint16_t language = 0x409) {
  return {3, 1, language, id, utf16be(text)};
}

NameRecord mac(uint16_t id, const std::string & text) {
  return {1, 0, 0, id, raw_bytes(text)};
}

struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  explicit Font(std::vector<uint8_t> b) : bytes(std::move(b)) {
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED, nullptr,
                  nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr), GFNT_OK);
  }
  static std::vector<uint8_t> file(const std::string & name) {
    std::ifstream in(gfnttest::data("fonts/" + name), std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;
  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  std::string name(uint16_t id, uint16_t language = GFNT_LANGUAGE_ANY) const {
    char * text = nullptr;
    if (gfnt_face_name(face, id, language, nullptr, &text, nullptr, nullptr) != GFNT_OK) {
      return "<none>";
    }
    std::string out = text;
    gfnt_name_free(nullptr, text);
    return out;
  }
  size_t name_count() const {
    size_t n = 0;
    gfnt_face_name_count(face, &n, nullptr);
    return n;
  }
};

struct Subset {
  GFNT_Blob * blob = nullptr;
  GFNT_Result result = GFNT_OK;
  std::vector<uint8_t> bytes;
  Subset(const GFNT_Face * source, const char * family, bool retain = false) {
    const uint32_t chars[] = {0x41, 0x42};
    GFNT_SubsetOptions o;
    gfnt_subset_options_init(&o);
    o.codepoints = chars;
    o.codepoint_count = 2;
    o.family_name = family;
    o.retain_gids = retain;
    result = gfnt_subset(source, &o, nullptr, nullptr, &blob, nullptr);
    if (result == GFNT_OK) {
      bytes.assign(gfnt_blob_data(blob), gfnt_blob_data(blob) + gfnt_blob_size(blob));
    }
  }
  ~Subset() { gfnt_blob_destroy(blob); }
};

/** Whether @p needle occurs in @p haystack as bytes, as ASCII or as UTF-16BE. */
bool mentions(const std::vector<uint8_t> & haystack, const std::string & needle) {
  const std::vector<uint8_t> ascii(needle.begin(), needle.end());
  const std::vector<uint8_t> wide = utf16be(needle);
  return std::search(haystack.begin(), haystack.end(), ascii.begin(), ascii.end()) != haystack.end()
      || std::search(haystack.begin(), haystack.end(), wide.begin(), wide.end()) != haystack.end();
}

std::vector<NameRecord> foo_black() {
  return {win(0, "Copyright (c) The Authors"), win(1, "Foo Black"), win(2, "Regular"),
      win(3, "FooBlack-unique"), win(4, "Foo Black"), win(5, "Version 1.0"),
      win(6, "Foo-Black"), win(13, "OFL text"), win(16, "Foo"), win(17, "Black"),
      win(256, "Weight"), win(1, "Foo Noir", 0x40C), win(16, "Foo Fr", 0x40C),
      mac(1, "Foo Black"), mac(2, "Regular"), mac(4, "Foo Black"), mac(6, "Foo-Black"),
      mac(18, "Foo Black"), win(20, "Foo-Black-CID"), win(25, "Foo"), win(21, "Foo WWS")};
}

}  // namespace

TEST(SubsetRename, EveryRecordThatStatesTheFamilyIsReplacedInEveryLanguage) {
  Font source(glyph_font(foo_black()));
  Subset sub(source.face, "Bar Type");
  ASSERT_EQ(sub.result, GFNT_OK);
  Font out(sub.bytes);
  EXPECT_EQ(out.name(1), "Bar Type");
  EXPECT_EQ(out.name(16), "Bar Type");
  EXPECT_EQ(out.name(4), "Bar Type Black");
  EXPECT_EQ(out.name(6), "BarType-Black");
  EXPECT_EQ(out.name(3), "Bar Type;Black;subset");
  EXPECT_FALSE(mentions(sub.bytes, "Foo")) << "no trace of the old family, in any language";
  EXPECT_FALSE(mentions(sub.bytes, "Noir"));
  EXPECT_EQ(out.name(21), "Bar Type") << "the WWS family is renamed too";
  EXPECT_EQ(out.name(18), "<none>") << "the Macintosh compatible full name is dropped";
  EXPECT_EQ(out.name(20), "<none>");
  EXPECT_EQ(out.name(25), "<none>");
}

TEST(SubsetRename, EverythingElseIsCarriedAsItWas) {
  Font source(glyph_font(foo_black()));
  Subset sub(source.face, "Bar Type");
  ASSERT_EQ(sub.result, GFNT_OK);
  Font out(sub.bytes);
  EXPECT_EQ(out.name(0), "Copyright (c) The Authors") << "a copyright names its holder, not the family";
  EXPECT_EQ(out.name(2), "Regular");
  EXPECT_EQ(out.name(5), "Version 1.0");
  EXPECT_EQ(out.name(13), "OFL text");
  EXPECT_EQ(out.name(17), "Black");
  EXPECT_EQ(out.name(256), "Weight") << "an axis name is not a family name";
}

TEST(SubsetRename, TheRecordsAreSortedAsTheFormatRequires) {
  Font source(glyph_font(foo_black()));
  Subset sub(source.face, "Bar Type");
  ASSERT_EQ(sub.result, GFNT_OK);
  Font out(sub.bytes);
  std::vector<std::array<uint32_t, 4>> keys;
  for (size_t i = 0; i < out.name_count(); ++i) {
    GFNT_NameRecord r{};
    ASSERT_EQ(gfnt_face_name_at(out.face, i, &r, nullptr), GFNT_OK);
    keys.push_back({r.platform_id, r.encoding_id, r.language_id, r.name_id});
  }
  EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
  EXPECT_GE(keys.size(), 10u);
}

TEST(SubsetRename, AMacintoshFontKeepsMacintoshNamesForAnAsciiFamily) {
  Font source(glyph_font(foo_black()));
  Subset sub(source.face, "Bar Type");
  ASSERT_EQ(sub.result, GFNT_OK);
  Font out(sub.bytes);
  bool mac_family = false;
  for (size_t i = 0; i < out.name_count(); ++i) {
    GFNT_NameRecord r{};
    ASSERT_EQ(gfnt_face_name_at(out.face, i, &r, nullptr), GFNT_OK);
    if (r.platform_id == 1 && r.name_id == 1) {
      mac_family = true;
    }
  }
  EXPECT_TRUE(mac_family);
  Subset unicode(source.face, "Bär Type");
  ASSERT_EQ(unicode.result, GFNT_OK);
  Font out2(unicode.bytes);
  for (size_t i = 0; i < out2.name_count(); ++i) {
    GFNT_NameRecord r{};
    ASSERT_EQ(gfnt_face_name_at(out2.face, i, &r, nullptr), GFNT_OK);
    EXPECT_FALSE(r.platform_id == 1 && r.name_id == 1) << "Macintosh Roman is not written for a non-ASCII name";
  }
}

TEST(SubsetRename, ATypographicFamilyIsWrittenOnlyWhereTheSourceHadOne) {
  Font source(glyph_font({win(0, "c"), win(1, "Foo"), win(2, "Italic"), win(4, "Foo Italic"),
      win(6, "Foo-Italic")}));
  Subset sub(source.face, "Bar");
  ASSERT_EQ(sub.result, GFNT_OK);
  Font out(sub.bytes);
  EXPECT_EQ(out.name(16), "<none>");
  EXPECT_EQ(out.name(1), "Bar");
  EXPECT_EQ(out.name(4), "Bar Italic") << "the style comes from the source's subfamily";
  EXPECT_EQ(out.name(6), "Bar-Italic");
}

TEST(SubsetRename, AFontWithNoNameTableGetsOne) {
  Font source(glyph_font({}));
  Subset sub(source.face, "Bar");
  ASSERT_EQ(sub.result, GFNT_OK);
  Font out(sub.bytes);
  EXPECT_EQ(out.name(1), "Bar");
  EXPECT_EQ(out.name(4), "Bar");
  EXPECT_EQ(out.name(6), "Bar-Regular");
}

TEST(SubsetRename, ANonAsciiFamilyKeepsItsLettersAndMakesAnAsciiPostScriptName) {
  Font source(glyph_font(foo_black()));
  Subset sub(source.face, "\xC3\x89l \xC3\x91" "and\xC3\xBA");     // "Él Ñandú"
  ASSERT_EQ(sub.result, GFNT_OK);
  Font out(sub.bytes);
  EXPECT_EQ(out.name(1), "\xC3\x89l \xC3\x91" "and\xC3\xBA");
  EXPECT_EQ(out.name(6), "land-Black") << "the letters outside ASCII are left out of a PostScript name";
  Subset astral(source.face, "Bar \xF0\x9F\x98\x80");
  ASSERT_EQ(astral.result, GFNT_OK);
  Font out2(astral.bytes);
  EXPECT_EQ(out2.name(1), "Bar \xF0\x9F\x98\x80") << "a surrogate pair survives";
}

TEST(SubsetRename, AFamilyThatIsNotAFamilyIsRefusedBeforeAnythingIsWritten) {
  Font source(glyph_font(foo_black()));
  for (const char * bad : {"", "\xC3", "ab\xFF", "\xED\xA0\x80"}) {
    Subset sub(source.face, bad);
    EXPECT_EQ(sub.result, GFNT_ERR_INVALID) << "family " << bad;
  }
  Subset longer(source.face, std::string(201, 'x').c_str());
  EXPECT_EQ(longer.result, GFNT_ERR_INVALID);
}

TEST(SubsetRename, ALanguageTagTableInNameFormatOneIsCarried) {
  // Format 1 with one language tag record: "x-test", and a record in language 0x8000 using it.
  std::vector<uint8_t> t;
  const std::vector<NameRecord> records = {win(0, "c"), win(1, "Foo"), win(2, "Regular"),
      win(9, "Designer", 0x8000)};
  put_u16(t, 1);
  put_u16(t, static_cast<uint16_t>(records.size()));
  const size_t header = 6 + records.size() * 12 + 2 + 4;
  put_u16(t, static_cast<uint16_t>(header));
  std::vector<uint8_t> storage;
  for (const auto & r : records) {
    put_u16(t, r.platform_id);
    put_u16(t, r.encoding_id);
    put_u16(t, r.language_id);
    put_u16(t, r.name_id);
    put_u16(t, static_cast<uint16_t>(r.text.size()));
    put_u16(t, static_cast<uint16_t>(storage.size()));
    storage.insert(storage.end(), r.text.begin(), r.text.end());
  }
  const std::vector<uint8_t> tag = utf16be("x-test");
  put_u16(t, 1);
  put_u16(t, static_cast<uint16_t>(tag.size()));
  put_u16(t, static_cast<uint16_t>(storage.size()));
  storage.insert(storage.end(), tag.begin(), tag.end());
  t.insert(t.end(), storage.begin(), storage.end());

  std::vector<uint8_t> hmtx;
  std::vector<uint8_t> loca;
  for (int g = 0; g < 4; ++g) {
    put_u16(hmtx, 500);
    put_s16(hmtx, 0);
  }
  for (int g = 0; g <= 4; ++g) {
    put_u16(loca, 0);
  }
  Font source(build_sfnt(0x00010000, {
      {GFNT_TAG('h', 'e', 'a', 'd'), build_head(1000, 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'), build_hhea(800, -200, 0, 4)},
      {GFNT_TAG('m', 'a', 'x', 'p'), build_maxp(4)},
      {GFNT_TAG('h', 'm', 't', 'x'), hmtx},
      {GFNT_TAG('l', 'o', 'c', 'a'), loca},
      {GFNT_TAG('g', 'l', 'y', 'f'), {}},
      {GFNT_TAG('c', 'm', 'a', 'p'), build_cmap({{3, 10, build_cmap_format12({{0x41, 0x43, 1}})}})},
      {GFNT_TAG('n', 'a', 'm', 'e'), t}}));
  Subset sub(source.face, "Bar");
  ASSERT_EQ(sub.result, GFNT_OK);
  EXPECT_TRUE(mentions(sub.bytes, "x-test")) << "the tag is still there";
  Font out(sub.bytes);
  // The table says it has one language tag, and the tag's offset leads to its text.
  const uint8_t * nm = nullptr;
  size_t nm_len = 0;
  ASSERT_EQ(gfnt_face_table_data(out.face, GFNT_TAG('n', 'a', 'm', 'e'), &nm, &nm_len), GFNT_OK);
  const size_t nrecords = (nm[2] << 8) | nm[3];
  const size_t nstorage = (nm[4] << 8) | nm[5];
  const size_t tags_at = 6 + nrecords * 12;
  ASSERT_LT(tags_at + 6, nm_len);
  EXPECT_EQ((nm[tags_at] << 8) | nm[tags_at + 1], 1);
  const size_t tag_len = (nm[tags_at + 2] << 8) | nm[tags_at + 3];
  const size_t tag_off = nstorage + ((nm[tags_at + 4] << 8) | nm[tags_at + 5]);
  EXPECT_EQ(std::vector<uint8_t>(nm + tag_off, nm + tag_off + tag_len), utf16be("x-test"));
  EXPECT_EQ(out.name(1), "Bar");
  EXPECT_EQ(out.name(9, 0x8000), "Designer");
}

TEST(SubsetRename, ACffIsNamedToMatchAndLosesTheNamesOnlyItsTopDictUsed) {
  Font source(Font::file("cff-names.otf"));
  ASSERT_TRUE(mentions(source.bytes, "Reserved Full Name"));
  Subset sub(source.face, "Bar Type");
  ASSERT_EQ(sub.result, GFNT_OK);
  EXPECT_TRUE(mentions(sub.bytes, "BarType-Regular")) << "the CFF Name INDEX";
  EXPECT_FALSE(mentions(sub.bytes, "Reserved Full Name"));
  EXPECT_FALSE(mentions(sub.bytes, "Reserved Family"));
  EXPECT_FALSE(mentions(sub.bytes, "GhotiFixtureCFFnames"));
  EXPECT_FALSE(mentions(sub.bytes, "CFF names"));
  Font out(sub.bytes);
  EXPECT_EQ(out.name(1), "Bar Type");
}

TEST(SubsetRename, ACffWithoutARenameKeepsItsNames) {
  Font source(Font::file("cff-names.otf"));
  Subset sub(source.face, nullptr);
  ASSERT_EQ(sub.result, GFNT_OK);
  EXPECT_TRUE(mentions(sub.bytes, "Reserved Full Name"));
  EXPECT_TRUE(mentions(sub.bytes, "GhotiFixtureCFFnames-Regular"));
}

TEST(SubsetRename, ACidKeyedCffLosesItsFontDictNames) {
  Font source(Font::file("cff-cid-names.otf"));
  ASSERT_TRUE(mentions(source.bytes, "Reserved Fd One"));
  Subset sub(source.face, "Bar");
  ASSERT_EQ(sub.result, GFNT_OK);
  EXPECT_FALSE(mentions(sub.bytes, "Reserved Fd One"));
  EXPECT_FALSE(mentions(sub.bytes, "Reserved Fd Two"));
  EXPECT_FALSE(mentions(sub.bytes, "GhotiFixtureCFFcidnames"));
  EXPECT_TRUE(mentions(sub.bytes, "Fixture")) << "the registry and ordering strings are not names";
  Subset kept(source.face, nullptr);
  ASSERT_EQ(kept.result, GFNT_OK);
  EXPECT_TRUE(mentions(kept.bytes, "Reserved Fd One")) << "unrenamed, they are carried";
}

TEST(SubsetRename, ARenamedSubsetStillMatchesTheOriginalGlyphForGlyph) {
  Font source(Font::file("variable-gvar.ttf"));
  Subset renamed(source.face, "Bar");
  Subset plain(source.face, nullptr);
  ASSERT_EQ(renamed.result, GFNT_OK);
  ASSERT_EQ(plain.result, GFNT_OK);
  Font a(renamed.bytes);
  Font b(plain.bytes);
  for (const char * tag : {"glyf", "loca", "gvar", "fvar", "hmtx", "cmap"}) {
    const GFNT_Tag t = GFNT_TAG(tag[0], tag[1], tag[2], tag[3]);
    const uint8_t * da = nullptr;
    const uint8_t * db = nullptr;
    size_t la = 0;
    size_t lb = 0;
    ASSERT_EQ(gfnt_face_table_data(a.face, t, &da, &la), GFNT_OK);
    ASSERT_EQ(gfnt_face_table_data(b.face, t, &db, &lb), GFNT_OK);
    EXPECT_EQ(std::vector<uint8_t>(da, da + la), std::vector<uint8_t>(db, db + lb)) << tag;
  }
}

TEST(SubsetRename, EveryAllocationFailureIsSurvived) {
  Font source(glyph_font(foo_black()));
  const uint32_t chars[] = {0x41};
  GFNT_SubsetOptions o;
  gfnt_subset_options_init(&o);
  o.codepoints = chars;
  o.codepoint_count = 1;
  o.family_name = "Bar Type";
  size_t requests = 0;
  {
    gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
    GFNT_Blob * out = nullptr;
    ASSERT_EQ(gfnt_subset(source.face, &o, nullptr, counting.get(), &out, nullptr), GFNT_OK);
    requests = counting.requests();
    gfnt_blob_destroy(out);
    EXPECT_EQ(counting.live(), 0u);
  }
  for (size_t fail = 0; fail < requests; ++fail) {
    gfnttest::FailingAllocator failing(fail);
    GFNT_Blob * out = nullptr;
    const GFNT_Result r = gfnt_subset(source.face, &o, nullptr, failing.get(), &out, nullptr);
    if (r != GFNT_OK) {
      EXPECT_EQ(r, GFNT_ERR_OOM) << "request " << fail;
    }
    gfnt_blob_destroy(out);
    EXPECT_EQ(failing.live(), 0u) << "request " << fail << " leaked";
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
