/**
 * @file
 *
 * Fuzz harness for shaping: `GSUB`, `GPOS` and `GDEF` together, applied to text.
 *
 * Three tables rather than one because what a lookup has to survive is the
 * relationship between them: a coverage that names a glyph the font does not
 * have, a mark class `GDEF` gives to a glyph a `GPOS` lookup attaches to, a
 * nested lookup index past the end of the lookup list. A fuzzer given `GSUB`
 * alone cannot write the `GDEF` that changes which of its glyphs are skipped.
 *
 * The input is an options byte, a byte that splits the rest between the tables
 * (the first and second thirds, in the order `GSUB`, `GPOS`, `GDEF`: the byte
 * places the first cut, the second falls halfway after it), and the tables. The
 * face around them is fixed - sixty-four glyphs, each mapped from the code point
 * at 0x30 plus its index and each 500 wide - so what varies is what the fuzzer
 * controls. The options byte picks the direction, the script, and a handful of
 * features from a fixed list, and seeds the text.
 *
 * Properties, beyond that nothing crashes or reads outside a table:
 *
 *   * **asking twice gives one answer** - the plan is rebuilt every call and
 *     nothing is kept;
 *   * **a result is one of a few**: success, or a refusal naming a corrupt table,
 *     an unsupported one, a limit, or no memory - never anything else;
 *   * **every cluster is a position in the text**, and they run in one direction:
 *     non-decreasing through the run for left to right, non-increasing for right
 *     to left, because no lookup here reorders glyphs;
 *   * **a run is never longer than the limit says** (sixty-four times the text,
 *     and never under 16,384), which is what stops a multiple substitution from
 *     asking for the machine.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/shape.h>

#include "../sfnt_builder.h"

namespace {

void check(bool ok) {
  if (!ok) {
    abort();
  }
}

constexpr size_t kGlyphs = 64;

GFNT_Tag tag(const char * s) {
  return GFNT_TAG(s[0], s[1], s[2], s[3]);
}

const char * const kFeatures[] = {
    "kern", "liga", "ccmp", "mark", "mkmk", "curs", "calt", "rclt", "ss01",
    "ss02", "ss03", "ss04", "ss05", "ss06", "ss07", "ss08", "salt", "dlig",
    "hlig", "frac", "locl", "rlig", "dist", "abvm"};

/** A cmap mapping 0x30 + i to glyph i for i in 1..63. */
std::vector<uint8_t> cmap_table() {
  std::vector<gfnttest::Segment4> segments;
  // One segment: 0x31..0x6F onto glyphs 1..63, then the closing one.
  segments.push_back({0x31, 0x6F, static_cast<int16_t>(1 - 0x31), {}});
  segments.push_back({0xFFFF, 0xFFFF, 1, {}});
  return gfnttest::build_cmap({{3, 1, gfnttest::build_cmap_format4(segments)}});
}

uint32_t next(uint32_t & state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 8) {
    return 0;
  }
  const uint8_t options = data[0];
  const size_t payload = size - 2;
  const size_t first_cut = (static_cast<size_t>(data[1]) * payload) / 256u;
  const size_t second_cut = first_cut + (payload - first_cut) / 2;
  const uint8_t * body = data + 2;
  std::vector<uint8_t> gsub(body, body + first_cut);
  std::vector<uint8_t> gpos(body + first_cut, body + second_cut);
  std::vector<uint8_t> gdef(body + second_cut, body + payload);

  std::vector<std::pair<uint16_t, int16_t>> metrics(kGlyphs, {500, 0});
  std::vector<gfnttest::Table> tables = {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'),
          gfnttest::build_hhea(800, -200, 0, static_cast<uint16_t>(kGlyphs))},
      {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics)},
      {GFNT_TAG('m', 'a', 'x', 'p'),
          gfnttest::build_maxp(static_cast<uint16_t>(kGlyphs))},
      {GFNT_TAG('c', 'm', 'a', 'p'), cmap_table()},
  };
  if (!gsub.empty()) {
    tables.push_back({GFNT_TAG('G', 'S', 'U', 'B'), gsub});
  }
  if (!gpos.empty()) {
    tables.push_back({GFNT_TAG('G', 'P', 'O', 'S'), gpos});
  }
  if (!gdef.empty()) {
    tables.push_back({GFNT_TAG('G', 'D', 'E', 'F'), gdef});
  }
  const std::vector<uint8_t> font = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      tables);

  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error;

  gfnt_error_clear(&error);
  if (gfnt_blob_create_memory(font.data(), font.size(), GFNT_BLOB_BORROWED,
          nullptr, nullptr, &blob, &error)
      != GFNT_OK) {
    return 0;
  }
  if (gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error) != GFNT_OK) {
    gfnt_blob_destroy(blob);
    return 0;
  }

  // The text: a run of glyphs picked by a generator seeded from the input.
  uint32_t state = 0x9E3779B9u ^ (static_cast<uint32_t>(options) << 8)
      ^ static_cast<uint32_t>(size);
  std::vector<uint32_t> text;
  const size_t length = 1 + (next(state) % 24);
  for (size_t i = 0; i < length; ++i) {
    text.push_back(0x30 + 1 + (next(state) % (kGlyphs - 1)));
  }

  GFNT_ShapeFeature features[3];
  size_t feature_count = 0;
  for (size_t i = 0; i < 3; ++i) {
    if (options & (1u << i)) {
      features[feature_count].tag = tag(kFeatures[next(state) % 24]);
      features[feature_count].value = 1 + (next(state) % 3);
      features[feature_count].start = 0;
      features[feature_count].end = GFNT_SHAPE_END;
      if (options & 0x40u) {
        features[feature_count].start = next(state) % length;
        features[feature_count].end = next(state) % (length + 1);
        if (features[feature_count].end < features[feature_count].start) {
          features[feature_count].end = features[feature_count].start;
        }
      }
      ++feature_count;
    }
  }
  GFNT_ShapeOptions shape_options{};
  shape_options.script = (options & 0x08u) ? 0 : tag("latn");
  shape_options.direction = (options & 0x10u) ? GFNT_DIRECTION_RTL
                                              : GFNT_DIRECTION_LTR;
  shape_options.features = features;
  shape_options.feature_count = feature_count;

  GFNT_ShapedRun first{};
  GFNT_ShapedRun second{};
  const GFNT_Result a = gfnt_face_shape(face, text.data(), text.size(),
      &shape_options, nullptr, &first, &error);
  const GFNT_Result b = gfnt_face_shape(face, text.data(), text.size(),
      &shape_options, nullptr, &second, &error);

  check(a == b);
  check(a == GFNT_OK || a == GFNT_ERR_CORRUPT || a == GFNT_ERR_UNSUPPORTED
      || a == GFNT_ERR_LIMIT || a == GFNT_ERR_OOM);
  if (a == GFNT_OK) {
    const size_t limit = text.size() * 64 > 16384 ? text.size() * 64 : 16384;

    check(first.count == second.count);
    check(first.count <= limit);
    for (size_t i = 0; i < first.count; ++i) {
      check(first.glyphs[i].glyph == second.glyphs[i].glyph
          && first.glyphs[i].cluster == second.glyphs[i].cluster
          && first.glyphs[i].x_advance == second.glyphs[i].x_advance
          && first.glyphs[i].x_offset == second.glyphs[i].x_offset
          && first.glyphs[i].y_offset == second.glyphs[i].y_offset);
      check(first.glyphs[i].cluster < text.size());
      if (i > 0) {
        if (shape_options.direction == GFNT_DIRECTION_LTR) {
          check(first.glyphs[i].cluster >= first.glyphs[i - 1].cluster);
        }
        else {
          check(first.glyphs[i].cluster <= first.glyphs[i - 1].cluster);
        }
      }
    }
  }
  gfnt_shaped_run_free(&first);
  gfnt_shaped_run_free(&second);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
