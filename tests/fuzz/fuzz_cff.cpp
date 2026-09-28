/**
 * @file
 *
 * libFuzzer harness for the `CFF ` table alone.
 *
 * documentation/design.md section 14.2. The fuzzer owns **the whole table** and
 * the harness supplies only the sfnt around it, for the reason `fuzz_cmap` does
 * the same: a font's directory is not what is being tested, and a harness that
 * made the fuzzer produce one first would spend most of its budget there.
 *
 * A CFF is a nest of offsets - the Top DICT points at the charset, the encoding,
 * the `CharStrings` INDEX, the Private DICT and the `FDArray`; the Private DICT
 * points at its local subroutines *relative to itself*; every INDEX's offsets are
 * one-based from the byte before its data - and giving the fuzzer the whole table
 * is what lets it write offsets that point into each other. That relationship is
 * where the interesting inputs are, and it is the same argument `fuzz_glyf` makes
 * for taking `loca` and `glyf` together.
 *
 * `fuzz_charstring` is the other door: it runs a program with no container at
 * all, so a byte sequence does not have to be a valid CFF before it can reach the
 * interpreter.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <ghoti.io/font/charstring.h>
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

/** A `head` with a real em, so scaling is never a divide by zero. */
std::vector<uint8_t> head_table() {
  std::vector<uint8_t> out(54, 0);
  out[1] = 1;      // version 1.0
  out[12] = 0x5F;  // magicNumber
  out[13] = 0x0F;
  out[14] = 0x3C;
  out[15] = 0xF5;
  out[18] = 0x03;  // unitsPerEm 1000
  out[19] = 0xE8;
  return out;
}

/** A `maxp` version 0.5, which is what an OTTO font carries. */
std::vector<uint8_t> maxp_table(uint16_t glyphs) {
  std::vector<uint8_t> out(6, 0);
  out[1] = 0x50;  // 0.5 in 16.16's high half is 0x0000_8000; see below.
  out[0] = 0x00;
  out[1] = 0x00;
  out[2] = 0x50;
  out[3] = 0x00;
  out[4] = static_cast<uint8_t>(glyphs >> 8);
  out[5] = static_cast<uint8_t>(glyphs & 0xFF);
  return out;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 2) {
    return 0;
  }

  const uint8_t options = data[0];
  std::vector<uint8_t> cff(data + 1, data + size);

  // A generous glyph count, so that `maxp` is never what stops the loop: the
  // numGlyphs minimum takes the CharStrings count when it is smaller, which is
  // the arm worth reaching.
  std::vector<uint8_t> font = gfnttest::build_sfnt(GFNT_FLAVOUR_CFF, {
      {GFNT_TAG('h', 'e', 'a', 'd'), head_table()},
      {GFNT_TAG('m', 'a', 'x', 'p'), maxp_table(256)},
      {GFNT_TAG('C', 'F', 'F', ' '), cff},
  });

  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  // Every cap the charstring path has, driven from the options byte: a cap that
  // is never small is a cap nothing tests.
  if ((options & 0x02u) != 0) {
    limits.max_outline_points = 1u + (options >> 4);
  }
  if ((options & 0x04u) != 0) {
    limits.max_contours = 1u + (options >> 5);
  }
  if ((options & 0x08u) != 0) {
    limits.max_charstring_depth = options >> 6;
  }
  if ((options & 0x10u) != 0) {
    limits.max_charstring_ops = 1u + (options >> 3);
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
    // Capped, so that a font claiming hundreds of glyphs does not spend the
    // unit's whole time budget here and get filed as a slow unit.
    const size_t reach = count < 64u ? count : 64u;

    for (size_t glyph = 0; glyph < reach; ++glyph) {
      const uint32_t index = static_cast<uint32_t>(glyph);
      GFNT_Outline * outline = nullptr;
      GFNT_CharstringMetrics metrics;
      GFNT_CharstringType type = GFNT_CHARSTRING_TYPE2;
      const uint8_t * bytes = nullptr;
      size_t length = 0;
      char * name = nullptr;

      memset(&metrics, 0, sizeof metrics);
      // The charset, which is a walk of its own and can refuse on its own.
      if (gfnt_face_glyph_name(face, index, nullptr, &name, nullptr, &error)
          == GFNT_OK) {
        gfnt_glyph_name_free(nullptr, name);
      }
      if (gfnt_face_glyph_charstring(face, index, &type, &bytes, &length,
              &error)
          == GFNT_OK && (options & 0x20u) != 0) {
        gfnt_charstring_dump(type, bytes, length, nullptr, sink());
      }
      gfnt_face_glyph_charstring_metrics(face, index, &metrics, &error);
      if (gfnt_face_glyph_outline(face, index, nullptr, nullptr, &outline,
              &error)
          != GFNT_OK) {
        continue;
      }
      // Everything that walks the points, because an interpreter can produce an
      // outline whose contours are wrong and only the walk finds out.
      GFNT_Box drawn;
      GFNT_Box control;

      memset(&drawn, 0, sizeof drawn);
      memset(&control, 0, sizeof control);
      gfnt_outline_bounds(outline, &drawn);
      gfnt_outline_control_box(outline, &control);
      if ((options & 0x40u) != 0) {
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
