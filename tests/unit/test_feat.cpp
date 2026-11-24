/**
 * @file
 *
 * `feat`: the features and settings of an Apple font, and the tables that cannot be read.
 *
 * documentation/design.md section 9.6.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <vector>

#include <ghoti.io/font/feat.h>

using gfnttest::Table;

namespace {

struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  explicit Font(std::vector<Table> tables)
      : bytes(gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables)) {
    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED, nullptr, nullptr,
                  &blob, nullptr),
        GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
  }

  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }

  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;
};

struct File {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  explicit File(const std::string & name) {
    EXPECT_EQ(gfnt_blob_create_file(gfnttest::data("fonts/" + name).c_str(), nullptr, nullptr, &blob, nullptr),
        GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr), GFNT_OK);
  }

  ~File() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
};

Table feat_table(std::vector<uint8_t> data) {
  return Table{GFNT_TAG('f', 'e', 'a', 't'), std::move(data)};
}

/** A table with @p features records of 12 bytes each and nothing after them. */
std::vector<uint8_t> header(uint32_t version, uint16_t features) {
  std::vector<uint8_t> t;
  gfnttest::put_u32(t, version);
  gfnttest::put_u16(t, features);
  gfnttest::put_u16(t, 0);
  gfnttest::put_u32(t, 0);
  return t;
}

}  // namespace

TEST(Feat, ListsTheFeaturesAndSettingsOfTheFixture) {
  File file("feat.ttf");
  ASSERT_TRUE(gfnt_face_has_feat(file.face));
  size_t count = 0;
  ASSERT_EQ(gfnt_face_feat_count(file.face, &count, nullptr), GFNT_OK);
  ASSERT_EQ(count, 4u);

  GFNT_FeatFeature f{};
  ASSERT_EQ(gfnt_face_feat_at(file.face, 0, &f, nullptr), GFNT_OK);
  EXPECT_EQ(f.type, 1u);
  EXPECT_EQ(f.name_id, 256u);
  EXPECT_EQ(f.selector_count, 3u);
  EXPECT_TRUE(f.exclusive);
  EXPECT_TRUE(f.has_default);
  EXPECT_EQ(f.default_index, 1u);

  ASSERT_EQ(gfnt_face_feat_at(file.face, 1, &f, nullptr), GFNT_OK);
  EXPECT_EQ(f.type, 3u);
  EXPECT_TRUE(f.exclusive);
  EXPECT_FALSE(f.has_default);

  ASSERT_EQ(gfnt_face_feat_at(file.face, 2, &f, nullptr), GFNT_OK);
  EXPECT_FALSE(f.exclusive);
  EXPECT_TRUE(f.has_default);
  EXPECT_EQ(f.default_index, 0u);

  ASSERT_EQ(gfnt_face_feat_at(file.face, 3, &f, nullptr), GFNT_OK);
  EXPECT_EQ(f.selector_count, 0u);

  GFNT_FeatSelector s{};
  ASSERT_EQ(gfnt_face_feat_selector_at(file.face, 1, 3, &s, nullptr), GFNT_OK);
  EXPECT_EQ(s.selector, 3u);
  EXPECT_EQ(s.name_id, 264u);
  EXPECT_EQ(gfnt_face_feat_selector_at(file.face, 1, 4, &s, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_feat_at(file.face, 4, &f, nullptr), GFNT_ERR_INVALID);
}

TEST(Feat, AFaceWithoutTheTableHasNoFeaturesAndSaysSo) {
  File file("basic.ttf");
  EXPECT_FALSE(gfnt_face_has_feat(file.face));
  size_t count = 7;
  EXPECT_EQ(gfnt_face_feat_count(file.face, &count, nullptr), GFNT_ERR_UNSUPPORTED);
}

TEST(Feat, RefusesNullsAndABadFace) {
  File file("feat.ttf");
  GFNT_FeatFeature f{};
  GFNT_FeatSelector s{};
  size_t count = 0;
  EXPECT_EQ(gfnt_face_feat_count(file.face, nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_feat_count(nullptr, &count, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_feat_at(file.face, 0, nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_feat_selector_at(file.face, 0, 0, nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_feat_dump(file.face, nullptr), GFNT_ERR_INVALID);
  EXPECT_FALSE(gfnt_face_has_feat(nullptr));
  (void)f;
  (void)s;
}

TEST(Feat, TablesThatContradictThemselvesAreCorruptAndNeverReadPastTheirEnd) {
  size_t count = 0;
  GFNT_FeatFeature f{};
  GFNT_FeatSelector s{};

  {
    // Shorter than its header.
    Font font({feat_table({0, 1, 0, 0})});
    EXPECT_EQ(gfnt_face_feat_count(font.face, &count, nullptr), GFNT_ERR_CORRUPT);
  }
  {
    // A version this library does not read.
    Font font({feat_table(header(0x00020000, 0))});
    EXPECT_EQ(gfnt_face_feat_count(font.face, &count, nullptr), GFNT_ERR_UNSUPPORTED);
  }
  {
    // A count that the records cannot fit in.
    Font font({feat_table(header(0x00010000, 3))});
    EXPECT_EQ(gfnt_face_feat_count(font.face, &count, nullptr), GFNT_ERR_CORRUPT);
  }
  {
    // One record whose settings are past the end.
    std::vector<uint8_t> t = header(0x00010000, 1);
    gfnttest::put_u16(t, 1);        // type
    gfnttest::put_u16(t, 2);        // two settings
    gfnttest::put_u32(t, 24);       // at the end of the table: no room
    gfnttest::put_u16(t, 0);
    gfnttest::put_u16(t, 256);
    Font font({feat_table(t)});
    ASSERT_EQ(gfnt_face_feat_count(font.face, &count, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_feat_at(font.face, 0, &f, nullptr), GFNT_ERR_CORRUPT);
    EXPECT_EQ(gfnt_face_feat_selector_at(font.face, 0, 0, &s, nullptr), GFNT_ERR_CORRUPT);
  }
}

TEST(Feat, TheDumpNamesEveryRecordAndFailsWithTheSink) {
  File file("feat.ttf");
  gfnttest::CapturedOutput out;
  ASSERT_NE(out.get(), nullptr);
  ASSERT_EQ(gfnt_face_feat_dump(file.face, out.get()), GFNT_OK);
  const std::string text = out.finish();
  EXPECT_NE(text.find("feat: 4 features"), std::string::npos) << text;
  EXPECT_NE(text.find("feat feature 0: type 1 flags 0xC001 name 256 settings 3"), std::string::npos) << text;
  EXPECT_NE(text.find("feat setting 1.3: selector 3 name 264"), std::string::npos) << text;
  EXPECT_EQ(text.find("feat setting 3."), std::string::npos) << "a feature with no settings lists none";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
