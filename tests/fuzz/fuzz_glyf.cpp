/**
 * @file
 *
 * libFuzzer harness for `glyf` and `loca` alone.
 *
 * documentation/design.md section 14.2. `fuzz_sfnt` reaches a glyph through a
 * whole font, so most of its budget goes on producing a directory that parses;
 * here the harness supplies the font around the input, splitting the fuzzer's
 * bytes into a `loca` and a `glyf` so that every byte it controls is one of
 * those two tables.
 *
 * The split is deliberate and is where the interesting inputs live: `loca`
 * decides where each glyph's description starts and ends, and a `loca` the
 * fuzzer wrote against a `glyf` it also wrote is how an entry running backwards,
 * an entry past the end, and a description truncated mid-flag-stream all get
 * produced without the fuzzer having to discover the relationship.
 *
 * The options byte drives `GFNT_Limits` - the point, per section 15.2, is that
 * a limit is a stated promise - and picks the `indexToLocFormat`, because the
 * short format stores each offset halved and is therefore a different code path
 * rather than the same one with a width parameter.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/outline.h>

#include "../sfnt_builder.h"

namespace {

/** A stream the dumps can be written to without producing output. */
FILE * sink() {
  static FILE * stream = fopen("/dev/null", "wb");
  return stream;
}

/** A `head` whose only interesting field is indexToLocFormat. */
std::vector<uint8_t> head_table(bool long_loca) {
  std::vector<uint8_t> out(54, 0);
  // version 1.0, so the parser accepts it.
  out[1] = 1;
  // unitsPerEm at offset 18: a real one, so scaling is not a divide by zero.
  out[18] = 0x03;
  out[19] = 0xE8;
  // magicNumber at offset 12.
  out[12] = 0x5F;
  out[13] = 0x0F;
  out[14] = 0x3C;
  out[15] = 0xF5;
  // indexToLocFormat at offset 50, signed 16-bit.
  out[51] = long_loca ? 1 : 0;
  return out;
}

/** A `maxp` claiming @p glyphs, which is what makes loca's length mean something. */
std::vector<uint8_t> maxp_table(uint16_t glyphs) {
  std::vector<uint8_t> out(6, 0);
  out[1] = 0;
  out[0] = 0;
  out[1] = 0x01;  // version 0.5's high half.
  out[4] = static_cast<uint8_t>(glyphs >> 8);
  out[5] = static_cast<uint8_t>(glyphs & 0xFF);
  return out;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 3) {
    return 0;
  }

  const uint8_t options = data[0];
  const bool long_loca = (options & 0x01u) != 0;
  // Where loca ends and glyf begins. The fuzzer picks it, so both tables get
  // the budget and neither is always tiny.
  const size_t split = 1u + (static_cast<size_t>(data[1]) * (size - 2u)) / 256u;

  std::vector<uint8_t> loca(data + 2, data + 2 + split);
  std::vector<uint8_t> glyf(data + 2 + split, data + size);

  // numGlyphs from what loca can index, so the two agree often enough for the
  // glyph loop to reach the parser rather than bouncing off the count.
  const size_t entry = long_loca ? 4u : 2u;
  const size_t indexable = loca.size() / entry;
  const uint16_t glyphs = static_cast<uint16_t>(
      indexable > 1u ? (indexable - 1u > 0xFFFFu ? 0xFFFFu : indexable - 1u)
                     : 1u);

  std::vector<uint8_t> font = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {
      {GFNT_TAG('h', 'e', 'a', 'd'), head_table(long_loca)},
      {GFNT_TAG('m', 'a', 'x', 'p'), maxp_table(glyphs)},
      {GFNT_TAG('l', 'o', 'c', 'a'), loca},
      {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
  });

  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  // Every cap the outline path has, driven from the options byte: a cap that is
  // never small is a cap nothing tests.
  if ((options & 0x02u) != 0) {
    limits.max_outline_points = 1u + (options >> 4);
  }
  if ((options & 0x04u) != 0) {
    limits.max_contours = 1u + (options >> 5);
  }
  if ((options & 0x08u) != 0) {
    limits.max_composite_depth = options >> 6;
  }

  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error;

  gfnt_error_clear(&error);
  if (gfnt_blob_create_memory(font.data(), font.size(), GFNT_BLOB_BORROWED,
          nullptr, nullptr, &blob, &error)
      != GFNT_OK) {
    return 0;
  }
  if (gfnt_face_load(blob, 0, &limits, nullptr, &face, &error) != GFNT_OK) {
    gfnt_blob_destroy(blob);
    return 0;
  }

  size_t count = 0;
  if (gfnt_face_num_glyphs(face, &count, &error) == GFNT_OK) {
    // Capped, so that a font claiming 65,535 glyphs does not spend the whole
    // unit's time budget in this loop and get filed as a slow unit.
    const size_t reach = count < 64u ? count : 64u;

    for (size_t glyph = 0; glyph < reach; ++glyph) {
      GFNT_Outline * outline = nullptr;
      bool composite = false;
      GFNT_Box box;

      memset(&box, 0, sizeof box);
      gfnt_face_glyph_is_composite(face, static_cast<uint32_t>(glyph),
          &composite, &error);
      gfnt_face_glyph_stated_box(face, static_cast<uint32_t>(glyph), &box,
          &error);
      if (gfnt_face_glyph_outline(face, static_cast<uint32_t>(glyph), nullptr,
              nullptr, &outline, &error)
          != GFNT_OK) {
        continue;
      }
      // Everything that walks the points, because a parser can produce an
      // outline whose contour ends are wrong and only the walk finds out.
      GFNT_Box drawn;
      GFNT_Box control;
      memset(&drawn, 0, sizeof drawn);
      memset(&control, 0, sizeof control);
      gfnt_outline_bounds(outline, &drawn);
      gfnt_outline_control_box(outline, &control);
      if ((options & 0x10u) != 0) {
        gfnt_outline_dump(outline, sink());
        gfnt_outline_path_dump(outline, sink());
      }
      gfnt_outline_destroy(outline);
    }
  }

  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
