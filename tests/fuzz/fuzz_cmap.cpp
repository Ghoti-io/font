/**
 * @file
 *
 * libFuzzer harness for `cmap` alone.
 *
 * documentation/design.md section 14.2. fuzz_sfnt reaches the `cmap` through a
 * whole font, which means the fuzzer spends most of its budget producing a
 * directory that parses at all. Here the harness wraps the input in a valid
 * one-table font, so every byte the fuzzer controls is a `cmap` byte and the
 * subtable formats - format 4's four parallel arrays above all - get the whole
 * budget.
 *
 * The first byte is the options byte: it picks which codepoints are looked up,
 * so that a mutation which only matters at one boundary is still asked about.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/font.h>

#include "../sfnt_builder.h"

namespace {

/** A stream the dumps can be written to without producing output. */
FILE * sink() {
  static FILE * stream = fopen("/dev/null", "wb");
  return stream;
}

/** Every boundary of every subtable format, plus two the options byte picks. */
std::vector<uint32_t> codepoints(uint8_t options) {
  std::vector<uint32_t> out = {
    0x0, 0x41, 0xFF, 0x100, 0xF041, 0xFFFF, 0x10000, 0x10FFFF, 0xFFFFFFFF,
  };
  out.push_back(options);
  out.push_back(0xF000u | options);
  return out;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 1) {
    return 0;
  }

  uint8_t options = data[0];
  std::vector<uint8_t> table(data + 1, data + size);

  // The input is the cmap table; the harness supplies the font around it, so
  // the fuzzer never has to rediscover the sfnt directory.
  std::vector<uint8_t> font = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      {{GFNT_TAG('c', 'm', 'a', 'p'), table}});

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

  if ((options & 0x80u) != 0) {
    gfnt_face_cmap_dump(face, sink());
  }

  const std::vector<uint32_t> points = codepoints(options);
  GFNT_CmapSubtable subtable;
  memset(&subtable, 0, sizeof subtable);

  if (gfnt_face_cmap_best(face, &subtable, &error) == GFNT_OK) {
    for (uint32_t codepoint : points) {
      uint32_t glyph = 0;
      gfnt_face_glyph_for_codepoint(face, codepoint, &glyph, &error);
    }
  }

  size_t count = 0;
  if (gfnt_face_cmap_count(face, &count, &error) == GFNT_OK) {
    for (size_t i = 0; i < count && i < 16; ++i) {
      if (gfnt_face_cmap_at(face, i, &subtable, &error) != GFNT_OK) {
        continue;
      }
      for (uint32_t codepoint : points) {
        uint32_t glyph = 0;
        gfnt_cmap_lookup(face, &subtable, codepoint, &glyph, &error);
      }
    }
  }

  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
