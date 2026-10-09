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
 * The subsetter. A subset is right when the glyphs it kept are the glyphs the
 * original has: the same outline, the same advance, reached by the same
 * character, and, with the ids retained, shaped to the same run. Each test
 * therefore compares a subset with its source rather than with a stored answer.
 */

#include "test_helpers.h"
#include "failing_allocator.h"
#include "sfnt_builder.h"

#include <algorithm>
#include <array>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/shape.h>
#include <ghoti.io/font/write.h>

#include <gtest/gtest.h>

namespace {

struct Loaded {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Result result = GFNT_OK;

  Loaded() = default;
  explicit Loaded(GFNT_Blob * owned) : blob(owned) {
    result = gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr);
  }
  ~Loaded() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Loaded(const Loaded &) = delete;
  Loaded & operator=(const Loaded &) = delete;
};

std::unique_ptr<Loaded> open_fixture(const std::string & name) {
  auto out = std::make_unique<Loaded>();
  EXPECT_EQ(gfnt_blob_create_file(gfnttest::data("fonts/" + name).c_str(), nullptr,
      nullptr, &out->blob, nullptr), GFNT_OK);
  out->result = gfnt_face_load(out->blob, 0, nullptr, nullptr, &out->face, nullptr);
  EXPECT_EQ(out->result, GFNT_OK);
  return out;
}

std::unique_ptr<Loaded> open_bytes(const std::vector<uint8_t> & bytes) {
  auto out = std::make_unique<Loaded>();
  EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_COPY,
      nullptr, nullptr, &out->blob, nullptr), GFNT_OK);
  out->result = gfnt_face_load(out->blob, 0, nullptr, nullptr, &out->face, nullptr);
  EXPECT_EQ(out->result, GFNT_OK);
  return out;
}

std::unique_ptr<Loaded> subset_of(const GFNT_Face * face,
    const GFNT_SubsetOptions & options, GFNT_Result * result = nullptr) {
  GFNT_Blob * blob = nullptr;
  GFNT_Error error{};
  const GFNT_Result r = gfnt_subset(face, &options, nullptr, nullptr, &blob, &error);
  if (result) {
    *result = r;
  }
  if (r != GFNT_OK) {
    if (!result) {
      ADD_FAILURE() << "subset refused: " << gfnt_result_string(r) << ": "
                    << (error.message ? error.message : "(no message)");
    }
    return nullptr;
  }
  auto out = std::make_unique<Loaded>(blob);
  EXPECT_EQ(out->result, GFNT_OK) << "the subset must load";
  return out;
}

/** Options that own the characters they name, so a braced list may be passed. */
struct Request : GFNT_SubsetOptions {
  std::vector<uint32_t> store;
  Request() { gfnt_subset_options_init(this); }
  Request(const Request &) = delete;
  Request & operator=(const Request &) = delete;
  Request(Request && other) noexcept : GFNT_SubsetOptions(other),
      store(std::move(other.store)) {
    codepoints = store.data();
  }
};

Request options_for(const std::vector<uint32_t> & cps) {
  Request r;
  r.store = cps;
  r.codepoints = r.store.data();
  r.codepoint_count = r.store.size();
  return r;
}

size_t glyph_count(const GFNT_Face * face) {
  size_t n = 0;
  EXPECT_EQ(gfnt_face_num_glyphs(face, &n, nullptr), GFNT_OK);
  return n;
}

uint32_t glyph_of(const GFNT_Face * face, uint32_t cp) {
  uint32_t g = 0;
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(face, cp, &g, nullptr), GFNT_OK);
  return g;
}

/** Every point of a glyph's outline, flattened with its tag, or empty. */
std::vector<int64_t> outline_of(const GFNT_Face * face, uint32_t glyph) {
  std::vector<int64_t> out;
  GFNT_Outline * outline = nullptr;
  if (gfnt_face_glyph_outline(face, glyph, nullptr, nullptr, &outline, nullptr)
      != GFNT_OK) {
    out.push_back(-1);
    return out;
  }
  const size_t n = gfnt_outline_point_count(outline);
  for (size_t i = 0; i < n; ++i) {
    GFNT_Point p;
    GFNT_PointTag tag;
    EXPECT_EQ(gfnt_outline_point_at(outline, i, &p, &tag), GFNT_OK);
    out.push_back(p.x);
    out.push_back(p.y);
    out.push_back(static_cast<int64_t>(tag));
  }
  out.push_back(static_cast<int64_t>(gfnt_outline_contour_count(outline)));
  gfnt_outline_destroy(outline);
  return out;
}

int32_t advance_of(const GFNT_Face * face, uint32_t glyph) {
  int32_t a = -1;
  EXPECT_EQ(gfnt_face_glyph_advance(face, glyph, nullptr, &a, nullptr), GFNT_OK);
  return a;
}

using Shaped = std::vector<std::array<int64_t, 6>>;

Shaped shape(const GFNT_Face * face, const std::vector<uint32_t> & text) {
  GFNT_ShapedRun run{};
  Shaped out;
  EXPECT_EQ(gfnt_face_shape(face, text.data(), text.size(), nullptr, nullptr, &run,
      nullptr), GFNT_OK);
  for (size_t i = 0; i < run.count; ++i) {
    const GFNT_ShapedGlyph & g = run.glyphs[i];
    out.push_back({g.glyph, g.cluster, g.x_advance, g.y_advance, g.x_offset,
        g.y_offset});
  }
  gfnt_shaped_run_free(&run);
  return out;
}

struct ShapeCase {
  std::vector<uint32_t> text;
  std::string note;
};

/** The cases of a .shape file that need no script, feature or location. */
std::vector<ShapeCase> plain_cases(const std::string & name) {
  std::ifstream in(gfnttest::data("fonts/" + name));
  std::vector<ShapeCase> cases;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::vector<std::string> f;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, '\t')) {
      f.push_back(cell);
    }
    if (f.size() < 6 || f[0] != "latn" || f[1] != "-" || f[2] != "-" || f[3] != "-") {
      continue;
    }
    ShapeCase c;
    std::stringstream cps(f[4]);
    std::string hex;
    while (cps >> hex) {
      c.text.push_back(static_cast<uint32_t>(std::stoul(hex, nullptr, 16)));
    }
    c.note = f[5];
    cases.push_back(c);
  }
  return cases;
}

}  // namespace

// --- glyph sets and the numbering -------------------------------------------

TEST(Subset, EveryRequestedGlyphIsTheOriginalGlyph) {
  // The composite fixture: each character on its own, so that every composite in
  // it is subset with exactly its components and nothing else, and the outline
  // that comes back resolved is the original's only if each component id was
  // renumbered to the glyph it meant.
  auto source = open_fixture("outline-composite.ttf");
  const size_t total = glyph_count(source->face);
  size_t composites = 0;
  for (uint32_t cp = 0x41; cp < 0x41 + total; ++cp) {
    const uint32_t original = glyph_of(source->face, cp);
    if (original == 0) {
      continue;
    }
    auto sub = subset_of(source->face, options_for({cp}));
    ASSERT_NE(sub, nullptr) << std::hex << cp;
    const uint32_t kept = glyph_of(sub->face, cp);
    ASSERT_NE(kept, 0u) << std::hex << cp;
    EXPECT_EQ(outline_of(sub->face, kept), outline_of(source->face, original))
        << "U+" << std::hex << cp;
    EXPECT_EQ(advance_of(sub->face, kept), advance_of(source->face, original))
        << "U+" << std::hex << cp;
    if (glyph_count(sub->face) > 2) {
      ++composites;  // .notdef, the glyph, and at least one component
    }
  }
  EXPECT_GT(composites, 3u) << "the fixture has composites; the set must grow for them";
}

TEST(Subset, OnlyTheRequestedCharactersAreMapped) {
  auto source = open_fixture("outline-composite.ttf");
  auto sub = subset_of(source->face, options_for({0x41, 0x43, 0x41}));
  ASSERT_NE(sub, nullptr);
  EXPECT_NE(glyph_of(sub->face, 0x41), 0u);
  EXPECT_NE(glyph_of(sub->face, 0x43), 0u);
  EXPECT_EQ(glyph_of(sub->face, 0x42), 0u) << "not requested";
  EXPECT_EQ(glyph_of(sub->face, 0x20), 0u) << "not requested";
  EXPECT_EQ(glyph_of(sub->face, 0x5A), 0u) << "not requested";
}

TEST(Subset, RenumberingIsCompactAndKeepsGlyphZero) {
  auto source = open_fixture("outline-composite.ttf");
  auto sub = subset_of(source->face, options_for({0x41}));
  ASSERT_NE(sub, nullptr);
  const size_t n = glyph_count(sub->face);
  EXPECT_LT(n, glyph_count(source->face));
  EXPECT_LE(glyph_of(sub->face, 0x41), n - 1);
  EXPECT_EQ(outline_of(sub->face, 0), outline_of(source->face, 0)) << ".notdef is kept";
}

TEST(Subset, RetainingIdsKeepsEveryIdAndEmptiesTheRest) {
  auto source = open_fixture("outline-composite.ttf");
  Request o = options_for({0x41, 0x44});
  o.retain_gids = true;
  auto sub = subset_of(source->face, o);
  ASSERT_NE(sub, nullptr);
  for (uint32_t cp : {0x41u, 0x44u}) {
    EXPECT_EQ(glyph_of(sub->face, cp), glyph_of(source->face, cp));
  }
  const uint32_t high = std::max(glyph_of(source->face, 0x41), glyph_of(source->face, 0x44));
  EXPECT_GE(glyph_count(sub->face), static_cast<size_t>(high) + 1);
  // A glyph below the highest kept one that nothing asked for is an empty outline.
  size_t emptied = 0;
  for (uint32_t g = 1; g < high; ++g) {
    const std::vector<int64_t> o_in = outline_of(sub->face, g);
    if (o_in.size() <= 1 || o_in[0] == -1) {
      ++emptied;
    }
  }
  EXPECT_GT(emptied, 0u);
}

TEST(Subset, RequestedGlyphIdsAreKeptWithoutACharacter) {
  auto source = open_fixture("outline-composite.ttf");
  const uint32_t g = glyph_of(source->face, 0x43);
  ASSERT_NE(g, 0u);
  Request o = options_for({});
  const uint32_t ids[] = {g};
  o.glyphs = ids;
  o.glyph_count = 1;
  auto sub = subset_of(source->face, o);
  ASSERT_NE(sub, nullptr);
  EXPECT_GE(glyph_count(sub->face), 2u);
  EXPECT_EQ(glyph_of(sub->face, 0x43), 0u) << "no character was asked for";
  // The glyph is there, as the second one: .notdef first, then ids in order.
  EXPECT_EQ(outline_of(sub->face, 1), outline_of(source->face, g));
}

TEST(Subset, NothingRequestedGivesGlyphZeroAlone) {
  auto source = open_fixture("basic.ttf");
  auto sub = subset_of(source->face, options_for({}));
  ASSERT_NE(sub, nullptr);
  EXPECT_EQ(glyph_count(sub->face), 1u);
  EXPECT_EQ(outline_of(sub->face, 0), outline_of(source->face, 0));
}

TEST(Subset, ACharacterTheFaceLacksIsSkipped) {
  auto source = open_fixture("basic.ttf");
  auto sub = subset_of(source->face, options_for({0x4E00, 0x10FFFF}));
  ASSERT_NE(sub, nullptr);
  EXPECT_EQ(glyph_count(sub->face), 1u);
}

TEST(Subset, ARequestedGlyphThatDoesNotExistIsRefused) {
  auto source = open_fixture("basic.ttf");
  Request o = options_for({});
  const uint32_t ids[] = {5000};
  o.glyphs = ids;
  o.glyph_count = 1;
  GFNT_Result r = GFNT_OK;
  EXPECT_EQ(subset_of(source->face, o, &r), nullptr);
  EXPECT_EQ(r, GFNT_ERR_INVALID);
}

TEST(Subset, CharactersPastTheBmpGoThroughFormat12) {
  auto source = open_fixture("cmap-format12.ttf");
  size_t checked = 0;
  for (uint32_t cp : {0x10000u, 0x10001u, 0x1F600u, 0x1F601u, 0x20000u}) {
    const uint32_t original = glyph_of(source->face, cp);
    if (original == 0) {
      continue;
    }
    auto sub = subset_of(source->face, options_for({cp, 0x41}));
    ASSERT_NE(sub, nullptr);
    const uint32_t kept = glyph_of(sub->face, cp);
    EXPECT_NE(kept, 0u) << std::hex << cp;
    EXPECT_EQ(outline_of(sub->face, kept), outline_of(source->face, original));
    ++checked;
  }
  EXPECT_GT(checked, 0u) << "the fixture has characters past the BMP";
}

// --- the tables -------------------------------------------------------------

TEST(Subset, TheOutputIsAWellFormedSfnt) {
  auto source = open_fixture("outline-composite.ttf");
  auto sub = subset_of(source->face, options_for({0x41, 0x42, 0x43}));
  ASSERT_NE(sub, nullptr);
  for (size_t i = 0; i < gfnt_face_table_count(sub->face); ++i) {
    GFNT_Tag tag = 0;
    ASSERT_EQ(gfnt_face_table_tag_at(sub->face, i, &tag), GFNT_OK);
    uint32_t stored = 0, computed = 1;
    ASSERT_EQ(gfnt_face_table_checksum(sub->face, tag, &stored, &computed), GFNT_OK);
    EXPECT_EQ(stored, computed) << "table " << i;
  }
  uint32_t sum = 0;
  const uint8_t * p = gfnt_blob_data(sub->blob);
  const size_t n = gfnt_blob_size(sub->blob);
  for (size_t i = 0; i < n; i += 4) {
    sum += (uint32_t(p[i]) << 24) | (uint32_t(p[i + 1]) << 16)
        | (uint32_t(p[i + 2]) << 8) | uint32_t(p[i + 3]);
  }
  EXPECT_EQ(sum, 0xB1B0AFBAu);
  for (const char * t : {"head", "hhea", "maxp", "hmtx", "cmap", "loca", "glyf"}) {
    EXPECT_TRUE(gfnt_face_has_table(sub->face,
        GFNT_TAG(t[0], t[1], t[2], t[3]))) << t;
  }
  const GFNT_Head * head = nullptr;
  ASSERT_EQ(gfnt_face_head(sub->face, &head, nullptr), GFNT_OK);
  EXPECT_EQ(head->units_per_em, 1000u);
  const GFNT_Hhea * hhea = nullptr;
  ASSERT_EQ(gfnt_face_hhea(sub->face, &hhea, nullptr), GFNT_OK);
  EXPECT_LE(hhea->number_of_h_metrics, glyph_count(sub->face));
  EXPECT_GE(hhea->number_of_h_metrics, 1u);
}

TEST(Subset, TheCharacterRangeInOs2FollowsTheRequest) {
  auto source = open_fixture("basic.ttf");
  auto sub = subset_of(source->face, options_for({0x41}));
  ASSERT_NE(sub, nullptr);
  const GFNT_Os2 * os2 = nullptr;
  ASSERT_EQ(gfnt_face_os2(sub->face, &os2, nullptr), GFNT_OK);
  EXPECT_EQ(os2->first_char_index, 0x41u);
  EXPECT_EQ(os2->last_char_index, 0x41u);
}

TEST(Subset, PostIsFormat3) {
  auto source = open_fixture("post-v2.ttf");
  auto sub = subset_of(source->face, options_for({0x41}));
  ASSERT_NE(sub, nullptr);
  const GFNT_Post * post = nullptr;
  ASSERT_EQ(gfnt_face_post(sub->face, &post, nullptr), GFNT_OK);
  EXPECT_EQ(post->version, 0x00030000);
}

TEST(Subset, ALongLocaIsChosenWhenGlyfIsLarge) {
  // Seventy glyphs of a few hundred bytes each pass 0x1FFFE only if the source
  // is enormous, so this builds one: a glyph with a very large instruction
  // program, repeated, written with the test builder.
  std::vector<uint8_t> big(12000, 0x20);
  std::vector<uint8_t> glyph = gfnttest::build_glyf_glyph(
      {{{0, 0, true}, {100, 0, true}, {50, 100, true}}}, big);
  std::vector<uint8_t> glyf;
  std::vector<uint8_t> loca;
  const int glyphs = 12;
  for (int i = 0; i < glyphs; ++i) {
    gfnttest::put_u32(loca, static_cast<uint32_t>(glyf.size()));
    glyf.insert(glyf.end(), glyph.begin(), glyph.end());
    while (glyf.size() % 4) {
      glyf.push_back(0);
    }
  }
  gfnttest::put_u32(loca, static_cast<uint32_t>(glyf.size()));
  ASSERT_GT(glyf.size(), 0x1FFFEu);

  auto font = [&](const std::vector<uint8_t> & g, const std::vector<uint8_t> & l,
                  int loca_format, int n) {
    std::vector<uint8_t> hhea, maxp, hmtx, cmap;
    gfnttest::put_u32(hhea, 0x00010000);
    gfnttest::put_s16(hhea, 800);
    gfnttest::put_s16(hhea, -200);
    for (int i = 0; i < 13; ++i) gfnttest::put_u16(hhea, 0);
    gfnttest::put_u16(hhea, 1);
    gfnttest::put_u32(maxp, 0x00010000);
    gfnttest::put_u16(maxp, n);
    for (int i = 0; i < 13; ++i) gfnttest::put_u16(maxp, 0);
    gfnttest::put_u16(hmtx, 500);
    gfnttest::put_s16(hmtx, 0);
    for (int i = 1; i < n; ++i) gfnttest::put_s16(hmtx, 0);
    // cmap (3,1) format 4, one segment per glyph: U+0041 + i -> glyph i + 1.
    const int segs = (n - 1) + 1;
    std::vector<uint8_t> sub;
    gfnttest::put_u16(sub, 4);
    gfnttest::put_u16(sub, 16 + 8 * segs);
    gfnttest::put_u16(sub, 0);
    gfnttest::put_u16(sub, 2 * segs);
    gfnttest::put_u16(sub, 0);
    gfnttest::put_u16(sub, 0);
    gfnttest::put_u16(sub, 0);
    for (int i = 0; i < n - 1; ++i) gfnttest::put_u16(sub, 0x41 + i);
    gfnttest::put_u16(sub, 0xFFFF);
    gfnttest::put_u16(sub, 0);
    for (int i = 0; i < n - 1; ++i) gfnttest::put_u16(sub, 0x41 + i);
    gfnttest::put_u16(sub, 0xFFFF);
    for (int i = 0; i < n - 1; ++i) gfnttest::put_u16(sub, (i + 1) - (0x41 + i));
    gfnttest::put_u16(sub, 1);
    for (int i = 0; i < segs; ++i) gfnttest::put_u16(sub, 0);
    gfnttest::put_u16(cmap, 0);
    gfnttest::put_u16(cmap, 1);
    gfnttest::put_u16(cmap, 3);
    gfnttest::put_u16(cmap, 1);
    gfnttest::put_u32(cmap, 12);
    cmap.insert(cmap.end(), sub.begin(), sub.end());
    return gfnttest::build_sfnt(0x00010000, {
        {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, loca_format)},
        {GFNT_TAG('h', 'h', 'e', 'a'), hhea},
        {GFNT_TAG('m', 'a', 'x', 'p'), maxp},
        {GFNT_TAG('h', 'm', 't', 'x'), hmtx},
        {GFNT_TAG('c', 'm', 'a', 'p'), cmap},
        {GFNT_TAG('l', 'o', 'c', 'a'), l},
        {GFNT_TAG('g', 'l', 'y', 'f'), g}});
  };
  auto source = open_bytes(font(glyf, loca, 1, glyphs));
  ASSERT_EQ(source->result, GFNT_OK);
  // Keeping all of them leaves a long loca; keeping two small ones does not.
  std::vector<uint32_t> all;
  for (int i = 0; i < glyphs - 1; ++i) all.push_back(0x41 + i);
  auto kept_all = subset_of(source->face, options_for(all));
  ASSERT_NE(kept_all, nullptr);
  const GFNT_Head * head = nullptr;
  ASSERT_EQ(gfnt_face_head(kept_all->face, &head, nullptr), GFNT_OK);
  EXPECT_EQ(head->index_to_loc_format, 1);
  auto kept_two = subset_of(source->face, options_for({0x41}));
  ASSERT_NE(kept_two, nullptr);
  ASSERT_EQ(gfnt_face_head(kept_two->face, &head, nullptr), GFNT_OK);
  EXPECT_EQ(head->index_to_loc_format, 0);
  EXPECT_EQ(outline_of(kept_two->face, glyph_of(kept_two->face, 0x41)),
      outline_of(source->face, glyph_of(source->face, 0x41)));
}

// --- GSUB -------------------------------------------------------------------

TEST(Subset, ShapingWithRetainedIdsMatchesTheOriginalOnEveryGsubCase) {
  // The retained subset keeps the layout tables as they are and the ids where
  // they were, so a run shapes to the same glyph ids in both; what can differ is
  // whether the glyphs it names still draw. Each shaped glyph is therefore also
  // compared by outline, which is where an incomplete closure shows.
  auto source = open_fixture("layout-gsub.ttf");
  const auto cases = plain_cases("layout-gsub.shape");
  ASSERT_GT(cases.size(), 10u);
  size_t smaller = 0;
  for (const ShapeCase & c : cases) {
    // U+0020 as well: the shaper puts a space where it hides a joiner.
    std::vector<uint32_t> wanted = c.text;
    wanted.push_back(0x20);
    Request o = options_for(wanted);
    o.retain_gids = true;
    auto sub = subset_of(source->face, o);
    ASSERT_NE(sub, nullptr) << c.note;
    const Shaped want = shape(source->face, c.text);
    const Shaped got = shape(sub->face, c.text);
    ASSERT_EQ(got, want) << c.note;
    for (const auto & g : want) {
      EXPECT_EQ(outline_of(sub->face, static_cast<uint32_t>(g[0])),
          outline_of(source->face, static_cast<uint32_t>(g[0])))
          << c.note << ": glyph " << g[0];
    }
    if (glyph_count(sub->face) < glyph_count(source->face)) {
      ++smaller;
    }
  }
  EXPECT_GT(smaller, 0u) << "a subset should actually be smaller for some case";
}

TEST(Subset, ShapingWithRetainedIdsMatchesTheOriginalOnEveryGposCase) {
  auto source = open_fixture("layout-gpos.ttf");
  const auto cases = plain_cases("layout-gpos.shape");
  ASSERT_GT(cases.size(), 10u);
  for (const ShapeCase & c : cases) {
    // U+0020 as well: the shaper puts a space where it hides a joiner.
    std::vector<uint32_t> wanted = c.text;
    wanted.push_back(0x20);
    Request o = options_for(wanted);
    o.retain_gids = true;
    auto sub = subset_of(source->face, o);
    ASSERT_NE(sub, nullptr) << c.note;
    EXPECT_EQ(shape(sub->face, c.text), shape(source->face, c.text)) << c.note;
  }
}

TEST(Subset, TheLigatureIsKeptWhenItsPartsAre) {
  auto source = open_fixture("layout-gsub.ttf");
  const std::vector<uint32_t> fi = {0x66, 0x69};
  const Shaped original = shape(source->face, fi);
  ASSERT_EQ(original.size(), 1u) << "f i is a ligature in the fixture";
  const uint32_t ligature = static_cast<uint32_t>(original[0][0]);
  const std::vector<int64_t> drawn = outline_of(source->face, ligature);
  ASSERT_GT(drawn.size(), 1u);

  Request closed = options_for(fi);
  closed.retain_gids = true;
  auto with = subset_of(source->face, closed);
  ASSERT_NE(with, nullptr);
  EXPECT_EQ(outline_of(with->face, ligature), drawn);

  GFNT_SubsetOptions open = closed;
  open.no_gsub_closure = true;
  auto without = subset_of(source->face, open);
  ASSERT_NE(without, nullptr);
  const std::vector<int64_t> o = outline_of(without->face, ligature);
  EXPECT_TRUE(o.size() <= 1 || o[0] == -1)
      << "without the closure the ligature is not drawn";
}

TEST(Subset, ARestrictedFeatureListClosesOnlyOverThoseFeatures) {
  auto source = open_fixture("layout-gsub.ttf");
  const std::vector<uint32_t> fi = {0x66, 0x69};
  const Shaped original = shape(source->face, fi);
  ASSERT_EQ(original.size(), 1u);
  const uint32_t ligature = static_cast<uint32_t>(original[0][0]);
  Request o = options_for(fi);
  o.retain_gids = true;
  const GFNT_Tag only[] = {GFNT_TAG('s', 'm', 'c', 'p')};  // not the ligature's feature
  o.features = only;
  o.feature_count = 1;
  auto sub = subset_of(source->face, o);
  ASSERT_NE(sub, nullptr);
  const std::vector<int64_t> drawn = outline_of(sub->face, ligature);
  EXPECT_TRUE(drawn.size() <= 1 || drawn[0] == -1);
}

TEST(Subset, LayoutIsDroppedOnRequestWhetherOrNotIdsMove) {
  auto source = open_fixture("layout-gsub.ttf");
  Request o = options_for({0x66, 0x69});
  o.drop_layout = true;
  auto dropped = subset_of(source->face, o);
  ASSERT_NE(dropped, nullptr);
  EXPECT_FALSE(gfnt_face_has_table(dropped->face, GFNT_TAG('G', 'S', 'U', 'B')));
  EXPECT_FALSE(gfnt_face_has_table(dropped->face, GFNT_TAG('G', 'D', 'E', 'F')));

  o.drop_layout = false;
  o.retain_gids = true;
  o.drop_layout = true;
  auto retained = subset_of(source->face, o);
  ASSERT_NE(retained, nullptr);
  EXPECT_FALSE(gfnt_face_has_table(retained->face, GFNT_TAG('G', 'S', 'U', 'B')));
}

// --- layout tables under renumbering ----------------------------------------

namespace {

std::vector<uint8_t> table_bytes(const Loaded & f, const char * t) {
  size_t offset = 0, length = 0;
  EXPECT_EQ(gfnt_face_table_range(f.face, GFNT_TAG(t[0], t[1], t[2], t[3]), &offset,
      &length), GFNT_OK) << t;
  const uint8_t * p = gfnt_blob_data(f.blob);
  return std::vector<uint8_t>(p + offset, p + offset + length);
}

/** A renumbered subset with the glyph numbering it used. */
struct Renumbered {
  std::unique_ptr<Loaded> font;
  GFNT_SubsetMap map{};
  Renumbered() = default;
  Renumbered(const Renumbered &) = delete;
  Renumbered & operator=(const Renumbered &) = delete;
  ~Renumbered() { gfnt_subset_map_free(&map, nullptr); }
};

void make_renumbered(Renumbered & out, const GFNT_Face * face, Request & o) {
  o.map = &out.map;
  out.font = subset_of(face, o);
}

/**
 * Shape @p text in the source and in a renumbered subset made for it: each glyph
 * must be the source's glyph under the map, with the same placement, and draw the
 * same.
 */
void expect_same_through_map(const Loaded & source, const std::vector<uint32_t> & text,
    const std::string & note, bool borrow_space = true) {
  std::vector<uint32_t> wanted = text;
  if (borrow_space) {
    wanted.push_back(0x20);
  }
  Request o = options_for(wanted);
  Renumbered sub;
  make_renumbered(sub, source.face, o);
  ASSERT_NE(sub.font, nullptr) << note;
  const Shaped want = shape(source.face, text);
  const Shaped got = shape(sub.font->face, text);
  ASSERT_EQ(got.size(), want.size()) << note;
  for (size_t i = 0; i < want.size(); ++i) {
    ASSERT_LT(got[i][0], static_cast<int64_t>(sub.map.count)) << note;
    EXPECT_EQ(static_cast<int64_t>(sub.map.old_of_new[got[i][0]]), want[i][0])
        << note << ": glyph " << i;
    for (size_t k = 1; k < 6; ++k) {
      EXPECT_EQ(got[i][k], want[i][k]) << note << ": glyph " << i << " field " << k;
    }
    EXPECT_EQ(outline_of(sub.font->face, static_cast<uint32_t>(got[i][0])),
        outline_of(source.face, static_cast<uint32_t>(want[i][0])))
        << note << ": glyph " << i;
  }
}

}  // namespace

TEST(Subset, ShapingAfterRenumberingMatchesTheOriginalOnEveryGsubCase) {
  auto source = open_fixture("layout-gsub.ttf");
  const auto cases = plain_cases("layout-gsub.shape");
  ASSERT_GT(cases.size(), 10u);
  for (const ShapeCase & c : cases) {
    expect_same_through_map(*source, c.text, c.note);
  }
}

TEST(Subset, ShapingAfterRenumberingMatchesTheOriginalOnEveryGposCase) {
  auto source = open_fixture("layout-gpos.ttf");
  const auto cases = plain_cases("layout-gpos.shape");
  ASSERT_GT(cases.size(), 10u);
  for (const ShapeCase & c : cases) {
    expect_same_through_map(*source, c.text, c.note);
  }
}

TEST(Subset, RenumberedLayoutTablesAreWrittenAndSmallerThanTheOriginals) {
  for (const char * name : {"layout-gsub.ttf", "layout-gpos.ttf"}) {
    auto source = open_fixture(name);
    Request o = options_for({0x66, 0x69});
    Renumbered sub;
    make_renumbered(sub, source->face, o);
    ASSERT_NE(sub.font, nullptr) << name;
    const char * layout = std::string(name) == "layout-gsub.ttf" ? "GSUB" : "GPOS";
    ASSERT_TRUE(gfnt_face_has_table(sub.font->face,
        GFNT_TAG(layout[0], layout[1], layout[2], layout[3]))) << name;
    EXPECT_LT(table_bytes(*sub.font, layout).size(), table_bytes(*source, layout).size())
        << name;
  }
}

TEST(Subset, TheMapNamesTheSourceGlyphOfEveryKeptGlyph) {
  auto source = open_fixture("layout-gsub.ttf");
  Request o = options_for({0x66, 0x69});
  Renumbered sub;
  make_renumbered(sub, source->face, o);
  ASSERT_NE(sub.font, nullptr);
  ASSERT_EQ(sub.map.count, glyph_count(sub.font->face));
  EXPECT_EQ(sub.map.old_of_new[0], 0u);
  for (uint32_t cp : {0x66u, 0x69u}) {
    EXPECT_EQ(sub.map.old_of_new[glyph_of(sub.font->face, cp)], glyph_of(source->face, cp));
  }
  for (size_t i = 1; i < sub.map.count; ++i) {
    EXPECT_LT(sub.map.old_of_new[i - 1], sub.map.old_of_new[i]) << "ids stay in order";
  }
}

TEST(Subset, ALookupNoFeatureReachesIsNotWritten) {
  // Restricting to a feature the ligature is not under leaves the ligature
  // lookup unreferenced, so it goes; the font still shapes the plain text.
  auto source = open_fixture("layout-gsub.ttf");
  Request all = options_for({0x66, 0x69});
  Request some = options_for({0x66, 0x69});
  const GFNT_Tag only[] = {GFNT_TAG('s', 'm', 'c', 'p')};
  some.features = only;
  some.feature_count = 1;
  Renumbered a, b;
  make_renumbered(a, source->face, all);
  make_renumbered(b, source->face, some);
  ASSERT_NE(a.font, nullptr);
  ASSERT_NE(b.font, nullptr);
  EXPECT_LT(table_bytes(*b.font, "GSUB").size(), table_bytes(*a.font, "GSUB").size());
  EXPECT_LT(glyph_count(b.font->face), glyph_count(a.font->face))
      << "the ligature glyph is not kept either";
}

TEST(Subset, WithoutTheClosureARenumberedGsubDropsWhatItCannotName) {
  auto source = open_fixture("layout-gsub.ttf");
  Request o = options_for({0x66, 0x69});
  o.no_gsub_closure = true;
  Renumbered sub;
  make_renumbered(sub, source->face, o);
  ASSERT_NE(sub.font, nullptr);
  const Shaped got = shape(sub.font->face, {0x66, 0x69});
  ASSERT_EQ(got.size(), 2u) << "no ligature: its glyph was not kept, so its rule is gone";
  EXPECT_EQ(sub.map.old_of_new[got[0][0]], glyph_of(source->face, 0x66));
  EXPECT_EQ(sub.map.old_of_new[got[1][0]], glyph_of(source->face, 0x69));
}

TEST(Subset, ARenumberedSubsetCanBeSubsetAgainWithItsLayout) {
  auto source = open_fixture("layout-gsub.ttf");
  Request o = options_for({0x66, 0x69, 0x20});
  auto first = subset_of(source->face, o);
  ASSERT_NE(first, nullptr);
  Request again = options_for({0x66, 0x69, 0x20});
  auto second = subset_of(first->face, again);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(shape(second->face, {0x66, 0x69}).size(), shape(source->face, {0x66, 0x69}).size());
}

// --- hinting ----------------------------------------------------------------

namespace {

/** A three-glyph font whose glyphs carry instructions: .notdef, a triangle, and a composite of the triangle. */
std::vector<uint8_t> hinted_font(std::vector<uint8_t> * glyf_out = nullptr) {
  const std::vector<uint8_t> program = {0xB0, 0x01, 0x2F, 0x00, 0x2E};
  std::vector<uint8_t> notdef = gfnttest::build_glyf_glyph(
      {{{0, 0, true}, {100, 0, true}, {100, 100, true}}}, program);
  std::vector<uint8_t> triangle = gfnttest::build_glyf_glyph(
      {{{0, 0, true}, {300, 0, true}, {150, 400, true}}}, program);
  std::vector<uint8_t> composite;
  gfnttest::put_s16(composite, -1);
  gfnttest::put_s16(composite, 0);
  gfnttest::put_s16(composite, 0);
  gfnttest::put_s16(composite, 300);
  gfnttest::put_s16(composite, 400);
  // One component: glyph 1, word offsets (0x0001), with instructions (0x0100).
  gfnttest::put_u16(composite, 0x0001 | 0x0100);
  gfnttest::put_u16(composite, 1);
  gfnttest::put_s16(composite, 20);
  gfnttest::put_s16(composite, 30);
  gfnttest::put_u16(composite, static_cast<uint16_t>(program.size()));
  composite.insert(composite.end(), program.begin(), program.end());
  std::vector<uint8_t> glyf, loca;
  for (const auto * g : {&notdef, &triangle, &composite}) {
    gfnttest::put_u16(loca, static_cast<uint16_t>(glyf.size() / 2));
    glyf.insert(glyf.end(), g->begin(), g->end());
    while (glyf.size() % 4) glyf.push_back(0);
  }
  gfnttest::put_u16(loca, static_cast<uint16_t>(glyf.size() / 2));
  if (glyf_out) {
    *glyf_out = glyf;
  }
  std::vector<uint8_t> hhea, maxp, hmtx, cmap, sub;
  gfnttest::put_u32(hhea, 0x00010000);
  gfnttest::put_s16(hhea, 800);
  gfnttest::put_s16(hhea, -200);
  for (int i = 0; i < 13; ++i) gfnttest::put_u16(hhea, 0);
  gfnttest::put_u16(hhea, 3);
  gfnttest::put_u32(maxp, 0x00010000);
  gfnttest::put_u16(maxp, 3);
  for (int i = 0; i < 13; ++i) gfnttest::put_u16(maxp, 0);
  for (int i = 0; i < 3; ++i) {
    gfnttest::put_u16(hmtx, 500 + 10 * i);
    gfnttest::put_s16(hmtx, 7 + 11 * i);  // distinct, so a wrong one is visible
  }
  // U+0041 -> 1, U+0042 -> 2, in one format 4 segment (delta 1 - 0x41).
  gfnttest::put_u16(sub, 4);
  gfnttest::put_u16(sub, 16 + 8 * 2);
  gfnttest::put_u16(sub, 0);
  gfnttest::put_u16(sub, 4);
  gfnttest::put_u16(sub, 4);
  gfnttest::put_u16(sub, 1);
  gfnttest::put_u16(sub, 0);
  gfnttest::put_u16(sub, 0x42);
  gfnttest::put_u16(sub, 0xFFFF);
  gfnttest::put_u16(sub, 0);
  gfnttest::put_u16(sub, 0x41);
  gfnttest::put_u16(sub, 0xFFFF);
  gfnttest::put_u16(sub, static_cast<uint16_t>(1 - 0x41));
  gfnttest::put_u16(sub, 1);
  gfnttest::put_u16(sub, 0);
  gfnttest::put_u16(sub, 0);
  gfnttest::put_u16(cmap, 0);
  gfnttest::put_u16(cmap, 1);
  gfnttest::put_u16(cmap, 3);
  gfnttest::put_u16(cmap, 1);
  gfnttest::put_u32(cmap, 12);
  cmap.insert(cmap.end(), sub.begin(), sub.end());
  return gfnttest::build_sfnt(0x00010000, {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'), hhea},
      {GFNT_TAG('m', 'a', 'x', 'p'), maxp},
      {GFNT_TAG('h', 'm', 't', 'x'), hmtx},
      {GFNT_TAG('c', 'm', 'a', 'p'), cmap},
      {GFNT_TAG('l', 'o', 'c', 'a'), loca},
      {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
      {GFNT_TAG('f', 'p', 'g', 'm'), {0xB0, 0x00}},
      {GFNT_TAG('p', 'r', 'e', 'p'), {0xB0, 0x00}},
      {GFNT_TAG('c', 'v', 't', ' '), {0x00, 0x10}}});
}

}  // namespace

TEST(Subset, SideBearingsAndAdvancesFollowTheirGlyphs) {
  auto source = open_bytes(hinted_font());
  ASSERT_EQ(source->result, GFNT_OK);
  for (bool retain : {false, true}) {
    Request o = options_for({0x42, 0x41});
    o.retain_gids = retain;
    auto sub = subset_of(source->face, o);
    ASSERT_NE(sub, nullptr);
    for (uint32_t cp : {0x41u, 0x42u}) {
      const uint32_t a = glyph_of(source->face, cp);
      const uint32_t b = glyph_of(sub->face, cp);
      int32_t want = -1, got = -2;
      ASSERT_EQ(gfnt_face_glyph_side_bearing(source->face, a, nullptr, &want, nullptr), GFNT_OK);
      ASSERT_EQ(gfnt_face_glyph_side_bearing(sub->face, b, nullptr, &got, nullptr), GFNT_OK);
      EXPECT_EQ(got, want) << "side bearing of U+" << std::hex << cp << " retain " << retain;
      EXPECT_NE(want, 0);
      EXPECT_EQ(advance_of(sub->face, b), advance_of(source->face, a));
    }
  }
}

TEST(Subset, HintingIsKeptByDefaultAndGlyphsAreCopiedByteForByte) {
  std::vector<uint8_t> glyf;
  auto source = open_bytes(hinted_font(&glyf));
  ASSERT_EQ(source->result, GFNT_OK);
  Request o = options_for({0x41, 0x42});
  o.retain_gids = true;  // so the composite's component id is unchanged too
  GFNT_Result why = GFNT_OK;
  auto sub = subset_of(source->face, o, &why);
  ASSERT_NE(sub, nullptr) << gfnt_result_string(why);
  EXPECT_EQ(table_bytes(*sub, "glyf"), glyf);
  EXPECT_TRUE(gfnt_face_has_table(sub->face, GFNT_TAG('f', 'p', 'g', 'm')));
  EXPECT_TRUE(gfnt_face_has_table(sub->face, GFNT_TAG('p', 'r', 'e', 'p')));
  EXPECT_TRUE(gfnt_face_has_table(sub->face, GFNT_TAG('c', 'v', 't', ' ')));
}

TEST(Subset, DroppingHintingRemovesInstructionsFromBothKindsOfGlyph) {
  auto source = open_bytes(hinted_font());
  ASSERT_EQ(source->result, GFNT_OK);
  Request o = options_for({0x41, 0x42});
  o.drop_hinting = true;
  auto sub = subset_of(source->face, o);
  ASSERT_NE(sub, nullptr);
  EXPECT_FALSE(gfnt_face_has_table(sub->face, GFNT_TAG('f', 'p', 'g', 'm')));
  EXPECT_FALSE(gfnt_face_has_table(sub->face, GFNT_TAG('p', 'r', 'e', 'p')));
  EXPECT_FALSE(gfnt_face_has_table(sub->face, GFNT_TAG('c', 'v', 't', ' ')));
  const std::vector<uint8_t> glyf = table_bytes(*sub, "glyf");
  // The program's bytes appear nowhere in what was written.
  const std::vector<uint8_t> program = {0xB0, 0x01, 0x2F, 0x00, 0x2E};
  EXPECT_EQ(std::search(glyf.begin(), glyf.end(), program.begin(), program.end()),
      glyf.end());
  // The composite is the glyph that opens with a contour count of -1; the flag
  // that announces instructions after its components is clear, and the rest of
  // its flags are as they were (word offsets: 0x0001).
  {
    const std::array<uint8_t, 2> minus_one = {0xFF, 0xFF};
    const auto at = std::search(glyf.begin(), glyf.end(), minus_one.begin(),
        minus_one.end());
    ASSERT_NE(at, glyf.end());
    const size_t i = static_cast<size_t>(at - glyf.begin());
    ASSERT_GE(glyf.size(), i + 12);
    EXPECT_EQ((glyf[i + 10] << 8) | glyf[i + 11], 0x0001);
  }
  // And the glyphs still draw as they did, the composite's offset included.
  for (uint32_t cp : {0x41u, 0x42u}) {
    EXPECT_EQ(outline_of(sub->face, glyph_of(sub->face, cp)),
        outline_of(source->face, glyph_of(source->face, cp))) << std::hex << cp;
  }
  // The flag that announces instructions is clear in the composite.
  auto plain = open_bytes(hinted_font());
  Request keep = options_for({0x42});
  auto with = subset_of(plain->face, keep);
  ASSERT_NE(with, nullptr);
  EXPECT_GT(table_bytes(*with, "glyf").size(), glyf.size() / 2);
}

// --- what is refused --------------------------------------------------------

TEST(Subset, FacesItCannotSubsetAreRefusedByName) {
  for (const char * name : {"cff.otf", "variable-gvar.ttf", "type1.pfb", "bitmap.bdf"}) {
    auto source = open_fixture(name);
    GFNT_Result r = GFNT_OK;
    EXPECT_EQ(subset_of(source->face, options_for({0x41}), &r), nullptr) << name;
    EXPECT_EQ(r, GFNT_ERR_UNSUPPORTED) << name;
  }
}

TEST(Subset, ABadArgumentIsRefused) {
  auto source = open_fixture("basic.ttf");
  GFNT_Blob * out = nullptr;
  EXPECT_EQ(gfnt_subset(nullptr, nullptr, nullptr, nullptr, &out, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_subset(source->face, nullptr, nullptr, nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
  GFNT_SubsetOptions o;
  gfnt_subset_options_init(&o);
  o.codepoint_count = 3;  // a count and no array
  EXPECT_EQ(gfnt_subset(source->face, &o, nullptr, nullptr, &out, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(out, nullptr);
  // NULL options mean the defaults: glyph 0 alone.
  ASSERT_EQ(gfnt_subset(source->face, nullptr, nullptr, nullptr, &out, nullptr), GFNT_OK);
  gfnt_blob_destroy(out);
}

TEST(Subset, TheLimitsHold) {
  auto source = open_fixture("outline-composite.ttf");
  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_glyphs = 5;
  GFNT_Blob * out = nullptr;
  Request o = options_for({0x41});
  EXPECT_EQ(gfnt_subset(source->face, &o, &limits, nullptr, &out, nullptr),
      GFNT_ERR_LIMIT);
  gfnt_limits_default(&limits);
  limits.max_blob_bytes = 100;
  EXPECT_NE(gfnt_subset(source->face, &o, &limits, nullptr, &out, nullptr), GFNT_OK);
  EXPECT_EQ(out, nullptr);
}

TEST(Subset, AnAllocationFailureAnywhereIsReported) {
  auto source = open_fixture("layout-gsub.ttf");
  Request o = options_for({0x66, 0x69, 0x41});
  size_t requests = 0;
  {
    gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
    GFNT_Blob * out = nullptr;
    ASSERT_EQ(gfnt_subset(source->face, &o, nullptr, counting.get(), &out, nullptr),
        GFNT_OK);
    requests = counting.requests();
    gfnt_blob_destroy(out);
    EXPECT_EQ(counting.live(), 0u);
  }
  ASSERT_GT(requests, 10u);
  size_t refused = 0;
  for (size_t fail = 0; fail < requests; ++fail) {
    gfnttest::FailingAllocator failing(fail);
    GFNT_Blob * out = nullptr;
    const GFNT_Result r = gfnt_subset(source->face, &o, nullptr, failing.get(), &out,
        nullptr);
    if (r != GFNT_OK) {
      ++refused;
      EXPECT_EQ(r, GFNT_ERR_OOM) << "request " << fail;
      EXPECT_EQ(out, nullptr);
    }
    gfnt_blob_destroy(out);
    EXPECT_EQ(failing.live(), 0u) << "request " << fail << " leaked";
  }
  EXPECT_GT(refused, 5u);
}

TEST(Subset, ASubsetCanBeSubsetAgainAndWrittenAsWoff) {
  auto source = open_fixture("outline-composite.ttf");
  auto first = subset_of(source->face, options_for({0x41, 0x42, 0x43}));
  ASSERT_NE(first, nullptr);
  auto second = subset_of(first->face, options_for({0x42}));
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(outline_of(second->face, glyph_of(second->face, 0x42)),
      outline_of(source->face, glyph_of(source->face, 0x42)));
  EXPECT_EQ(glyph_of(second->face, 0x41), 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
