/**
 * @file
 *
 * The committed renderings, re-rendered here.
 *
 * `tests/data/golden/coverage.txt` holds every rendering of every fixture that
 * carries outlines of its own, at six sizes and five origins, **and every glyph
 * of every bitmap strike** - which reach a coverage through
 * ::gfnt_coverage_from_bitmap() rather than through the scan converter. This suite
 * renders them again and compares. It needs no container, so it runs in `make test` on a
 * fresh clone, and what it catches is a change in the rasteriser that nobody
 * meant to make.
 *
 * It is **not** the determinism gate. Re-rendering on the machine that wrote the
 * file would agree with itself whatever the byte order did; `make check-golden`
 * builds the same driver for three big-endian targets and compares against the
 * same file, and that is the only thing in the repository that can see a
 * host-dependent read. This suite and that gate read one artifact and answer two
 * questions, which is why the artifact is committed rather than computed twice.
 *
 * The expectations are **parsed**, not re-formatted: the line's shape lives in
 * `examples/font-render.c` and is written once. A test that spelled the format
 * again would pass while the two spellings drifted.
 *
 * documentation/design.md section 14.4.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <ghoti.io/font/bitmap.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>

namespace {

/** One line of the golden file. */
struct Expected {
  std::string fixture;
  uint32_t glyph = 0;
  uint32_t ppem = 0;
  GFNT_F26Dot6 origin_x = 0;
  GFNT_F26Dot6 origin_y = 0;
  /** The shape as `WxH+left+top`, or empty when the line is a refusal. */
  std::string shape;
  unsigned long long total = 0;
  unsigned long long hash = 0;
  /** The reason, for a line that records a refusal. */
  std::string refused;

  /** Whether this line is a strike's pixels, from the file's own section. */
  bool strike = false;

  std::string where() const {
    return fixture + " glyph " + std::to_string(glyph) + " at "
        + std::to_string(ppem) + "ppem, origin " + std::to_string(origin_x)
        + "/" + std::to_string(origin_y);
  }
};

/** Every line of the committed file, parsed. */
std::vector<Expected> golden() {
  std::vector<Expected> out;
  const std::string path = gfnttest::data("golden/coverage.txt");
  FILE * handle = fopen(path.c_str(), "r");
  if (!handle) {
    return out;
  }
  char line[512];
  bool strikes = false;
  while (fgets(line, sizeof line, handle)) {
    if (line[0] == '#' || line[0] == '\n') {
      // The file's own section marker, which is what says whether a line is a
      // rasterised outline or a strike's pixels. It used to be guessed from the
      // fixture's extension - right while every strike was a `.pcf`, `.bdf`,
      // `.psf` or `.hex`, and wrong the moment an sfnt with `EBLC` strikes had its
      // pixels committed: a `.ttf` went down the outline path and the test
      // compared a rasterised glyph against a strike's hash.
      if (strncmp(line, "# And the bitmap containers", 27) == 0) {
        strikes = true;
      }
      continue;
    }
    std::istringstream reading(line);
    Expected entry;
    entry.strike = strikes;
    std::string sixth;
    if (!(reading >> entry.fixture >> entry.glyph >> entry.ppem
            >> entry.origin_x >> entry.origin_y >> sixth)) {
      continue;
    }
    if (sixth == "refused") {
      std::string word;
      while (reading >> word) {
        entry.refused += entry.refused.empty() ? word : " " + word;
      }
      out.push_back(entry);
      continue;
    }
    entry.shape = sixth;
    std::string hash;
    if (!(reading >> entry.total >> hash)) {
      continue;
    }
    entry.hash = std::stoull(hash, nullptr, 16);
    out.push_back(entry);
  }
  fclose(handle);
  return out;
}

/** A font held open across the many renderings that use it. */
struct Font {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  explicit Font(const std::string & name) {
    GFNT_Error error{};
    const std::string path = gfnttest::data("fonts/" + name);
    if (gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &blob, &error)
        != GFNT_OK) {
      return;
    }
    if (gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error) != GFNT_OK) {
      face = nullptr;
    }
  }
  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;
};

std::string shape_of(const GFNT_Coverage & coverage) {
  return std::to_string(coverage.width) + "x" + std::to_string(coverage.height)
      + "+" + std::to_string(coverage.left) + "+"
      + std::to_string(coverage.top);
}

TEST(Golden, TheCommittedFileIsThereAndHasRenderingsInIt) {
  const std::vector<Expected> expected = golden();

  // The denominator, so that a suite which has stopped reading the file cannot
  // report a clean run: every test below iterates it, and an empty list passes
  // all of them.
  ASSERT_FALSE(expected.empty())
      << "tests/data/golden/coverage.txt is missing or unreadable; run "
         "`make golden`";
  EXPECT_GT(expected.size(), 1000u) << "only " << expected.size()
      << " rendering(s) committed, which is fewer than the fixtures have";
  size_t refusals = 0;
  for (const Expected & entry : expected) {
    if (!entry.refused.empty()) {
      refusals += 1;
    }
  }
  // Some of the committed lines are refusals - the cubic fixtures, and the
  // glyph whose `loca` entry runs backwards - and a run where they had all
  // become renderings would mean a refusal had stopped firing.
  EXPECT_GT(refusals, 0u);
  EXPECT_LT(refusals, expected.size() / 2u);
}

TEST(Golden, EveryCommittedRenderingIsReproduced) {
  const std::vector<Expected> expected = golden();
  ASSERT_FALSE(expected.empty());
  std::map<std::string, Font *> fonts;
  size_t checked = 0;
  size_t failures = 0;

  for (const Expected & entry : expected) {
    Font *& font = fonts[entry.fixture];
    if (!font) {
      font = new Font(entry.fixture);
    }
    ASSERT_NE(font->face, nullptr) << entry.fixture << " did not load";

    GFNT_RasterOptions options{};
    GFNT_Coverage coverage{};
    GFNT_Error error{};
    options.origin_x = entry.origin_x;
    options.origin_y = entry.origin_y;
    // A strike's glyph is pixels, not a path, and reaches a coverage by the other
    // route. Which route a line came from is read out of the committed file's own
    // section marker, as it is in tools/golden/check_golden.py and for the same
    // reason: the file is the contract.
    GFNT_Result result;
    if (entry.strike) {
      GFNT_BitmapGlyph bitmap{};
      // **Which strike** comes from the ppem column, which for a strike line is
      // the strike's own size. A standalone container has one strike and this was
      // `0`; an EBLC face has several, and taking strike 0 for all of them would
      // compare 10-pixel pixels against a 16-pixel hash.
      size_t strikes = 0;
      size_t chosen = 0;
      bool found = false;

      if (gfnt_face_strike_count(font->face, &strikes, nullptr) == GFNT_OK) {
        for (size_t i = 0; i < strikes; ++i) {
          GFNT_Strike candidate{};

          if (gfnt_face_strike_at(font->face, i, &candidate, nullptr) == GFNT_OK
              && candidate.ppem_y == entry.ppem) {
            chosen = i;
            found = true;
            break;
          }
        }
      }
      // A refusal line has a ppem of 0 and matches no strike, which is right: the
      // glyph is refused at strike 0 as readily as anywhere.
      if (!found && !entry.refused.empty()) {
        found = true;
      }
      ASSERT_TRUE(found) << entry.where()
          << ": no strike of this fixture is that size, so the committed line "
             "and the font disagree about what sizes it has";

      result = gfnt_face_glyph_bitmap(font->face, entry.glyph, chosen, &bitmap,
          &error);
      if (result == GFNT_OK) {
        result = gfnt_coverage_from_bitmap(&bitmap, nullptr, &coverage, &error);
      }
    }
    else {
      result = gfnt_face_render_glyph(font->face, entry.glyph, entry.ppem,
          &options, nullptr, &coverage, &error);
    }

    if (!entry.refused.empty()) {
      EXPECT_NE(result, GFNT_OK) << entry.where()
          << " renders now, and the committed file records a refusal";
      gfnt_coverage_destroy(&coverage);
      checked += 1;
      continue;
    }
    if (result != GFNT_OK) {
      // Reported once per failure rather than aborting, so a change that moved
      // many renderings says how many.
      if (failures < 8) {
        ADD_FAILURE() << entry.where() << " was refused as "
                      << gfnt_result_string(result) << ", and the committed "
                      << "file has a rendering for it";
      }
      failures += 1;
      gfnt_coverage_destroy(&coverage);
      continue;
    }
    const std::string shape = shape_of(coverage);
    const unsigned long long total = gfnt_coverage_total(&coverage);
    const unsigned long long hash = gfnt_coverage_hash(&coverage);
    if (shape != entry.shape || total != entry.total || hash != entry.hash) {
      if (failures < 8) {
        ADD_FAILURE() << entry.where() << ": committed " << entry.shape << " "
                      << entry.total << " " << std::hex << entry.hash
                      << ", rendered " << shape << " " << std::dec << total
                      << " " << std::hex << hash;
      }
      failures += 1;
    }
    gfnt_coverage_destroy(&coverage);
    checked += 1;
  }
  for (auto & pair : fonts) {
    delete pair.second;
  }
  EXPECT_EQ(failures, 0u) << failures << " of " << expected.size()
      << " committed renderings differ; if the change was meant, "
         "`make golden` rewrites the file";
  EXPECT_EQ(checked, expected.size());
}

TEST(Golden, ASubPixelOffsetMovesAGlyphSidewaysAndNotUpwards) {
  // What is *exactly* invariant across the four horizontal origins of one glyph
  // at one size: the bitmap's height, and its width to within the one column a
  // sideways shift can add. A rasteriser that mixed the two offsets up, or
  // applied a horizontal one to the vertical extent, changes the height - and
  // the committed hashes would report that as sixteen lines differing without
  // saying what they had in common.
  //
  // **The glyph's area is deliberately not asserted here.** It is nearly
  // invariant and not exactly so: each edge's cell crossings are interpolated
  // with rounding, so a shape with many edges accumulates. Measured over the
  // committed file, the worst spread is 3.4% - the 200-point zigzag at 8 ppem,
  // where the whole glyph is five pixels of ink and two hundred edges round
  // inside them. A bound loose enough to admit that is looser than a single
  // dropped pixel row on a tall glyph, so it would be a bound that passes rather
  // than a bound that checks. What covers the area is `testRaster`, against a
  // sampler that computes it a different way.
  const std::vector<Expected> expected = golden();
  ASSERT_FALSE(expected.empty());
  std::map<std::string, std::vector<const Expected *>> groups;

  for (const Expected & entry : expected) {
    if (!entry.refused.empty() || entry.origin_y != 0 || entry.shape.empty()) {
      continue;
    }
    groups[entry.fixture + "/" + std::to_string(entry.glyph) + "/"
        + std::to_string(entry.ppem)].push_back(&entry);
  }
  ASSERT_GT(groups.size(), 100u);

  size_t compared = 0;
  for (const auto & pair : groups) {
    const std::vector<const Expected *> & group = pair.second;
    if (group.size() < 2) {
      continue;
    }
    uint32_t first_width = 0;
    uint32_t first_height = 0;
    int32_t left = 0;
    int32_t top = 0;
    ASSERT_EQ(sscanf(group[0]->shape.c_str(), "%ux%u+%d+%d", &first_width,
        &first_height, &left, &top), 4) << group[0]->shape;
    for (size_t i = 1; i < group.size(); ++i) {
      uint32_t width = 0;
      uint32_t height = 0;
      ASSERT_EQ(sscanf(group[i]->shape.c_str(), "%ux%u+%d+%d", &width, &height,
          &left, &top), 4) << group[i]->shape;
      EXPECT_EQ(height, first_height)
          << pair.first << ": " << group[0]->shape << " at origin "
          << group[0]->origin_x << " and " << group[i]->shape << " at "
          << group[i]->origin_x
          << " - a horizontal offset changed the vertical extent";
      const long long difference = static_cast<long long>(width)
          - static_cast<long long>(first_width);
      EXPECT_LE(std::abs(difference), 1)
          << pair.first << ": width " << first_width << " at origin "
          << group[0]->origin_x << " and " << width << " at "
          << group[i]->origin_x;
      compared += 1;
    }
  }
  EXPECT_GT(compared, 300u);
}

}  // namespace

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
