/**
 * @file
 *
 * The four bitmap containers, from the file's first byte.
 *
 * documentation/design.md sections 7.1, 7.5 and 14. One harness for all four
 * because the door is one: `gfnt_face_load()` probes them in turn, and which one
 * claims an input is itself a decision a malformed file can steer. A harness per
 * container would test each parser and not that.
 *
 * What makes this worth a fuzzer of its own rather than leaving it to
 * `fuzz_sfnt`: every number that sizes an allocation here comes out of the file
 * and none of them is checked against a table directory, because three of these
 * formats have no directory at all. A PCF states four block sizes of which one is
 * true; a BDF states a box per glyph in text; a `.hex` line's width *is* its digit
 * count; a PSF 1 states no glyph count anywhere. And the rows are then
 * **rewritten** - bits reversed, bytes swapped within a scan unit, padding
 * cleared - so the bytes the normaliser walks are computed from the input rather
 * than a subrange of it.
 *
 * The input is used unaltered, so a real `.pcf`, `.bdf`, `.psf` or `.hex` is a
 * seed and the corpus is grown from the fixtures - **including a gzipped one**,
 * which puts the inflater in the path too. `compress` fuzzes its own decoder; what
 * is new here is the composition, where a mutated wrapper decides how many bytes
 * the container probe is then handed.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <ghoti.io/font/bitmap.h>
#include <ghoti.io/font/font.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/name.h>
#include <ghoti.io/font/raster.h>

namespace {

/** A stream the dumps can be written to without producing output. */
FILE * sink() {
  static FILE * stream = fopen("/dev/null", "wb");
  return stream;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error;
  GFNT_Limits limits;
  size_t glyphs = 0;
  size_t strikes = 0;
  size_t mappings = 0;

  gfnt_error_clear(&error);
  gfnt_limits_default(&limits);
  // Small enough that a file claiming an enormous font is refused rather than
  // spending the run's budget on one input, and large enough that every fixture
  // still loads - bitmap-v1.psf has 512 glyphs.
  limits.max_glyphs = 4096;
  limits.max_ppem = 256;

  if (gfnt_blob_create_memory(data, size, GFNT_BLOB_BORROWED, &limits, nullptr,
          &blob, &error)
      != GFNT_OK) {
    return 0;
  }
  if (gfnt_face_load(blob, 0, &limits, nullptr, &face, &error) != GFNT_OK) {
    gfnt_blob_destroy(blob);
    return 0;
  }

  // What the face answers for itself. Every one of these comes out of the
  // container's own header or properties: there is no table to read any of it
  // from, and two of the four state none of it.
  uint16_t upem = 0;
  char * text = nullptr;

  gfnt_face_units_per_em(face, &upem, &error);
  gfnt_face_num_glyphs(face, &glyphs, &error);
  gfnt_face_dump(face, sink());
  gfnt_face_strike_count(face, &strikes, &error);
  for (size_t i = 0; i < strikes; ++i) {
    GFNT_Strike strike;

    memset(&strike, 0, sizeof strike);
    gfnt_face_strike_at(face, i, &strike, &error);
    for (uint32_t policy = 0; policy <= GFNT_STRIKE_PREFER_STRIKE; ++policy) {
      bool from_outlines = false;

      gfnt_face_select_strike(face, 16,
          static_cast<GFNT_StrikePolicy>(policy), &strike, &from_outlines,
          &error);
    }
  }
  for (uint16_t id = 0; id <= 20; ++id) {
    if (gfnt_face_name(face, id, GFNT_LANGUAGE_ANY, nullptr, &text, nullptr,
            &error)
        == GFNT_OK) {
      gfnt_name_free(nullptr, text);
      text = nullptr;
    }
  }

  // The encoding, both ways round: enumerated, and looked up. The enumeration is
  // what a file's own ranges drive, and the lookup is a bisection over the sorted
  // result - so a mapping the parse built wrong is reachable from either side.
  if (gfnt_face_bitmap_encoding_count(face, &mappings, &error) == GFNT_OK) {
    for (size_t i = 0; i < mappings && i < 4096; ++i) {
      uint32_t codepoint = 0;
      uint32_t glyph = 0;

      if (gfnt_face_bitmap_encoding_at(face, i, &codepoint, &glyph, &error)
          == GFNT_OK) {
        uint32_t found = 0;

        gfnt_face_glyph_for_codepoint(face, codepoint, &found, &error);
      }
    }
  }
  for (uint32_t code = 0; code < 512; ++code) {
    uint32_t glyph = 0;

    gfnt_face_glyph_for_codepoint(face, code, &glyph, &error);
  }

  for (size_t index = 0; index < glyphs && index < 512; ++index) {
    const uint32_t glyph = static_cast<uint32_t>(index);
    GFNT_BitmapGlyph bitmap;
    GFNT_Coverage coverage;

    memset(&bitmap, 0, sizeof bitmap);
    memset(&coverage, 0, sizeof coverage);
    if (gfnt_face_glyph_name(face, glyph, nullptr, &text, nullptr, &error)
        == GFNT_OK) {
      gfnt_name_free(nullptr, text);
      text = nullptr;
    }
    if (gfnt_face_glyph_bitmap(face, glyph, 0, &bitmap, &error) != GFNT_OK) {
      continue;
    }
    // The dump reads every pixel through the accessor, which is what walks the
    // stride a normalised row was written with.
    gfnt_bitmap_dump(&bitmap, sink());
    if (gfnt_coverage_from_bitmap(&bitmap, nullptr, &coverage, &error)
        == GFNT_OK) {
      gfnt_coverage_total(&coverage);
      gfnt_coverage_hash(&coverage);
      gfnt_coverage_dump_art(&coverage, sink());
      gfnt_coverage_destroy(&coverage);
    }
  }

  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
