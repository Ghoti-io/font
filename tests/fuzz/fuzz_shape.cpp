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
 * With bit 0x80 of the options the first two thirds are Apple's `morx` and `feat`
 * instead of `GSUB` and `GPOS`.
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
#include <algorithm>
#include <vector>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/shape.h>

#include "../sfnt_builder.h"

namespace {

// What the current input is, for the message when a check fails.
const char * g_script = "";
std::vector<uint32_t> g_text;
const GFNT_ShapedRun * g_run = nullptr;

void check_at(bool ok, int line) {
  if (!ok) {
    fprintf(stderr, "fuzz_shape: check failed at line %d, script %s, text:",
        line, g_script);
    for (uint32_t u : g_text) {
      fprintf(stderr, " %04X", u);
    }
    fprintf(stderr, "\n");
    if (g_run) {
      fprintf(stderr, "  glyph/cluster:");
      for (size_t i = 0; i < g_run->count; ++i) {
        fprintf(stderr, " %u/%u", g_run->glyphs[i].glyph, g_run->glyphs[i].cluster);
      }
      fprintf(stderr, "\n");
    }
    abort();
  }
}

#define check(ok) check_at((ok), __LINE__)

constexpr size_t kGlyphs = 64;

GFNT_Tag tag(const char * s) {
  return GFNT_TAG(s[0], s[1], s[2], s[3]);
}

const char * const kFeatures[] = {
    "kern", "liga", "ccmp", "mark", "mkmk", "curs", "calt", "rclt", "ss01",
    "ss02", "ss03", "ss04", "ss05", "ss06", "ss07", "ss08", "salt", "dlig",
    "hlig", "frac", "locl", "rlig", "dist", "abvm"};

/**
 * The characters of a script that has a shaper of its own: one of each kind that
 * shaper tells apart, so that a run of them reaches every rule it has. The first
 * entry is the script's tag.
 */
struct Alphabet {
  const char * script;
  std::vector<uint32_t> chars;
};

const std::vector<Alphabet> & alphabets() {
  static const std::vector<Alphabet> table = {
      {"deva", {0x0915, 0x0916, 0x0917, 0x0930, 0x0937, 0x0905, 0x0906, 0x093C,
          0x093E, 0x093F, 0x0940, 0x0941, 0x0947, 0x094D, 0x0902, 0x0903,
          0x0901, 0x0951, 0x200C, 0x200D, 0x25CC, 0x20, 0x0964}},
      {"beng", {0x0995, 0x09B0, 0x09AF, 0x09A4, 0x0985, 0x09BC, 0x09BE, 0x09BF,
          0x09C7, 0x09CB, 0x09CD, 0x0982, 0x09CE, 0x200C, 0x200D, 0x25CC}},
      {"taml", {0x0B95, 0x0BB0, 0x0BB3, 0x0B85, 0x0BBE, 0x0BC6, 0x0BC7, 0x0BCA,
          0x0BCB, 0x0BCD, 0x0B82, 0x200D, 0x25CC}},
      {"knda", {0x0C95, 0x0CB0, 0x0CB3, 0x0C85, 0x0CBC, 0x0CBE, 0x0CC6, 0x0CC8,
          0x0CCA, 0x0CCD, 0x0C82, 0x200D, 0x0CF1, 0x25CC}},
      {"mlym", {0x0D15, 0x0D30, 0x0D33, 0x0D05, 0x0D3E, 0x0D46, 0x0D4A, 0x0D4D,
          0x0D02, 0x0D4E, 0x200D, 0x25CC}},
      {"khmr", {0x1780, 0x1781, 0x179A, 0x17A3, 0x17B6, 0x17B7, 0x17BB, 0x17BE,
          0x17C1, 0x17C6, 0x17C7, 0x17C9, 0x17CB, 0x17D2, 0x200C, 0x200D,
          0x25CC}},
      {"mymr", {0x1000, 0x1004, 0x101B, 0x1021, 0x102B, 0x102D, 0x102F, 0x1031,
          0x1032, 0x1037, 0x1038, 0x1039, 0x103A, 0x103B, 0x103C, 0x103D,
          0x103E, 0x1060, 0x200C, 0x25CC}},
      {"hang", {0x1100, 0x1112, 0x1161, 0x1175, 0x11A8, 0x11C2, 0xAC00, 0xD55C,
          0xD558, 0x302E, 0x302F, 0x25CC}},
      {"java", {0xA98F, 0xA992, 0xA9B4, 0xA9BA, 0xA9B3, 0xA9BF, 0xA9C0, 0xA982,
          0xA983, 0x200C, 0x200D, 0x20, 0x25CC}},
      {"bali", {0x1B05, 0x1B13, 0x1B38, 0x1B3E, 0x1B44, 0x1B00, 0x200D, 0x25CC}},
      {"tibt", {0x0F40, 0x0F41, 0x0F71, 0x0F72, 0x0F74, 0x0F7C, 0x0F84, 0x0F90,
          0x0F83, 0x200D, 0x25CC}},
      {"thai", {0x0E01, 0x0E02, 0x0E33, 0x0E34, 0x0E38, 0x0E48, 0x0E4D, 0x0E40,
          0x0E31}},
      {"arab", {0x0627, 0x0628, 0x062C, 0x0644, 0x064E, 0x0651, 0x0640, 0x200C,
          0x200D, 0x0622, 0xFEFB}},
      {"hebr", {0x05D0, 0x05D1, 0x05B0, 0x05BC, 0x05C1, 0xFB31, 0x200D}},
      {"sinh", {0x0D9A, 0x0DBB, 0x0DCA, 0x0DD9, 0x0DDC, 0x0DCF, 0x0D82, 0x200D,
          0x25CC}},
  };
  return table;
}

/** A cmap mapping the characters of the alphabet, or 0x30 + i, to glyph i. */
std::vector<uint8_t> cmap_table(const Alphabet * alphabet) {
  std::vector<gfnttest::Segment4> segments;

  if (!alphabet) {
    // One segment: 0x31..0x6F onto glyphs 1..63, then the closing one.
    segments.push_back({0x31, 0x6F, static_cast<int16_t>(1 - 0x31), {}});
  }
  else {
    std::vector<uint32_t> sorted = alphabet->chars;

    // The text will use glyphs in the order the characters were listed, but the
    // cmap wants them by code point.
    std::vector<std::pair<uint32_t, uint16_t>> pairs;
    for (size_t i = 0; i < sorted.size(); ++i) {
      pairs.push_back({sorted[i], static_cast<uint16_t>(i + 1)});
    }
    std::sort(pairs.begin(), pairs.end());
    for (const auto & p : pairs) {
      segments.push_back({static_cast<uint16_t>(p.first),
          static_cast<uint16_t>(p.first),
          static_cast<int16_t>(p.second - p.first), {}});
    }
  }
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

  // Which script the text is in: Latin as it always was, or one of the scripts
  // that have a shaper, chosen by the options and the size.
  uint32_t pick_state = 0xC2B2AE35u ^ (static_cast<uint32_t>(options) << 16)
      ^ static_cast<uint32_t>(size);
  const size_t choice = next(pick_state) % (alphabets().size() * 2);
  const Alphabet * alphabet =
      choice < alphabets().size() ? &alphabets()[choice] : nullptr;

  std::vector<std::pair<uint16_t, int16_t>> metrics(kGlyphs, {500, 0});
  std::vector<gfnttest::Table> tables = {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'),
          gfnttest::build_hhea(800, -200, 0, static_cast<uint16_t>(kGlyphs))},
      {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics)},
      {GFNT_TAG('m', 'a', 'x', 'p'),
          gfnttest::build_maxp(static_cast<uint16_t>(kGlyphs))},
      {GFNT_TAG('c', 'm', 'a', 'p'), cmap_table(alphabet)},
  };
  if (options & 0x80u) {
    // Apple's tables instead: the first third is `morx`, the second `feat`.
    if (!gsub.empty()) {
      tables.push_back({GFNT_TAG('m', 'o', 'r', 'x'), gsub});
    }
    if (!gpos.empty()) {
      tables.push_back({GFNT_TAG('f', 'e', 'a', 't'), gpos});
    }
  }
  else {
    if (!gsub.empty()) {
      tables.push_back({GFNT_TAG('G', 'S', 'U', 'B'), gsub});
    }
    if (!gpos.empty()) {
      tables.push_back({GFNT_TAG('G', 'P', 'O', 'S'), gpos});
    }
    if (!gdef.empty()) {
      tables.push_back({GFNT_TAG('G', 'D', 'E', 'F'), gdef});
    }
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
    if (alphabet) {
      text.push_back(alphabet->chars[next(state) % alphabet->chars.size()]);
    }
    else {
      text.push_back(0x30 + 1 + (next(state) % (kGlyphs - 1)));
    }
  }

  g_script = alphabet ? alphabet->script : "latn";
  g_text = text;

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
  shape_options.script = (options & 0x08u) ? 0
      : tag(alphabet ? alphabet->script : "latn");
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

  g_run = &first;
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
