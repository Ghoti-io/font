/**
 * @file
 *
 * The characters a face's glyph names give when it has no `cmap` (src/cmap/namecmap.h):
 * the plain name over a variant, the lower glyph among equals, no character for a name that
 * stands for a sequence or for nothing, the `cmap` winning when there is one, and the
 * refusal for a face that names nothing.
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
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/face.h>

namespace {

/** A `post` version 2 table whose every glyph has a name of its own (indices from 258). */
std::vector<uint8_t> post_with_names(const std::vector<std::string> & names) {
  std::vector<uint8_t> post = gfnttest::build_post(0x00020000);
  post.resize(32);
  gfnttest::put_u16(post, static_cast<uint16_t>(names.size()));
  for (size_t i = 0; i < names.size(); i++) {
    gfnttest::put_u16(post, static_cast<uint16_t>(258 + i));
  }
  for (const std::string & name : names) {
    post.push_back(static_cast<uint8_t>(name.size()));
    post.insert(post.end(), name.begin(), name.end());
  }
  return post;
}

struct Face {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  Face(std::vector<gfnttest::Table> tables, uint16_t glyphs) {
    std::vector<std::pair<uint16_t, int16_t>> metrics(glyphs, {500, 0});
    tables.push_back({GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)});
    tables.push_back({GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, glyphs)});
    tables.push_back({GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics)});
    tables.push_back({GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(glyphs)});
    bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
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

  /** The glyph for a codepoint, or 0xFFFFFFFF when the lookup is not answered as OK. */
  uint32_t glyph(uint32_t codepoint) const {
    uint32_t g = 77;
    return gfnt_face_glyph_for_codepoint(face, codepoint, &g, nullptr) == GFNT_OK ? g : 0xFFFFFFFFu;
  }
};

const std::vector<std::string> kNames = {".notdef", "A", "a.sc", "a", "uni20AC", "f_i",
    "dalethatafpatah", "u1F600", "A.alt", "g9", "Aacute.sc", "Aacute"};

}  // namespace

TEST(NameCmap, AFaceWithNamesAndNoCmapIsReadThroughTheNames) {
  Face f({{GFNT_TAG('p', 'o', 's', 't'), post_with_names(kNames)}},
      static_cast<uint16_t>(kNames.size()));
  EXPECT_EQ(f.glyph('A'), 1u);
  EXPECT_EQ(f.glyph(0x20AC), 4u);
  EXPECT_EQ(f.glyph(0x1F600), 7u);
  EXPECT_EQ(f.glyph(0xC1), 11u);   // the plain Aacute, not the earlier Aacute.sc
}

TEST(NameCmap, ThePlainNameBeatsAVariantEvenWhenTheVariantComesFirst) {
  Face f({{GFNT_TAG('p', 'o', 's', 't'), post_with_names(kNames)}},
      static_cast<uint16_t>(kNames.size()));
  EXPECT_EQ(f.glyph('a'), 3u);   // a.sc is glyph 2
}

TEST(NameCmap, ANameThatIsASequenceOrNothingAnswersForNoCharacter) {
  Face f({{GFNT_TAG('p', 'o', 's', 't'), post_with_names(kNames)}},
      static_cast<uint16_t>(kNames.size()));
  EXPECT_EQ(f.glyph('f'), 0u);      // f_i is a ligature
  EXPECT_EQ(f.glyph('i'), 0u);
  EXPECT_EQ(f.glyph(0x05D3), 0u);   // dalethatafpatah is two characters
  EXPECT_EQ(f.glyph('g'), 0u);
  EXPECT_EQ(f.glyph('B'), 0u);      // no glyph is named B
}

TEST(NameCmap, ACmapWinsWhenThereIsOne) {
  Face f({{GFNT_TAG('p', 'o', 's', 't'), post_with_names(kNames)},
          {GFNT_TAG('c', 'm', 'a', 'p'), gfnttest::build_cmap({{3, 1,
              gfnttest::build_cmap_format4({{'A', 'A', static_cast<int16_t>(2 - 'A'), {}},
                                            {0xFFFF, 0xFFFF, 1, {}}})}})}},
      static_cast<uint16_t>(kNames.size()));
  EXPECT_EQ(f.glyph('A'), 2u);   // the cmap's, not the name's (glyph 1)
  EXPECT_EQ(f.glyph(0x20AC), 0u);   // the cmap does not have it, and the names are not consulted
}

TEST(NameCmap, AFaceThatNamesNothingIsStillRefused) {
  Face f({{GFNT_TAG('p', 'o', 's', 't'), gfnttest::build_post(0x00030000)}}, 4);
  EXPECT_EQ(f.glyph('A'), 0xFFFFFFFFu);
  GFNT_Error error;
  gfnt_error_clear(&error);
  uint32_t g = 0;
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(f.face, 'A', &g, &error), GFNT_ERR_UNSUPPORTED);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
