/**
 * @file
 *
 * The Type 1 container, from the file's first byte.
 *
 * documentation/design.md sections 7.1 and 14. This is the door a Type 1 font
 * comes in through, and it is the widest one in the library: before any parser
 * sees a token, the input has been through PFB segment framing or a hex decode
 * and an `eexec` decryption - so the bytes the PostScript scanner walks are
 * *computed from* the input rather than a subrange of it, and no bound the file
 * states describes them.
 *
 * That is why this fuzzer exists rather than relying on `fuzz_charstring`: the
 * interpreter is reached here through a decryptor and a tokeniser that a
 * malformed file steers, and a charstring is the *last* thing to go wrong.
 *
 * The input is used unaltered, so a real `.pfb` or `.pfa` is a seed and the
 * corpus can be grown from the fixtures.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/font/charstring.h>
#include <ghoti.io/font/font.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/name.h>
#include <ghoti.io/font/outline.h>
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

  gfnt_error_clear(&error);
  gfnt_limits_default(&limits);
  // Small enough that a file claiming a huge dictionary is refused rather than
  // spending the run's whole budget on one input, and large enough that every
  // fixture still loads.
  limits.max_glyphs = 4096;

  if (gfnt_blob_create_memory(data, size, GFNT_BLOB_BORROWED, &limits, nullptr,
          &blob, &error)
      != GFNT_OK) {
    return 0;
  }
  if (gfnt_face_load(blob, 0, &limits, nullptr, &face, &error) != GFNT_OK) {
    gfnt_blob_destroy(blob);
    return 0;
  }

  // What the face answers for itself, all of which a Type 1 program has to
  // produce out of its own PostScript: there is no table to read any of it from.
  uint16_t upem = 0;
  int32_t advance = 0;
  char * text = nullptr;

  gfnt_face_units_per_em(face, &upem, &error);
  gfnt_face_num_glyphs(face, &glyphs, &error);
  gfnt_face_dump(face, sink());
  for (uint16_t id = 0; id <= 20; ++id) {
    if (gfnt_face_name(face, id, GFNT_LANGUAGE_ANY, nullptr, &text, nullptr,
            &error)
        == GFNT_OK) {
      gfnt_name_free(nullptr, text);
      text = nullptr;
    }
  }
  // Every code of the font's own encoding, which is the only mapping this
  // container has.
  for (uint32_t code = 0; code < 256; ++code) {
    uint32_t glyph = 0;

    gfnt_face_glyph_for_codepoint(face, code, &glyph, &error);
  }

  for (size_t index = 0; index < glyphs && index < 256; ++index) {
    const uint32_t glyph = static_cast<uint32_t>(index);
    GFNT_Outline * outline = nullptr;
    GFNT_CharstringMetrics metrics;
    GFNT_CharstringType type = GFNT_CHARSTRING_TYPE1;
    const uint8_t * bytes = nullptr;
    size_t length = 0;
    bool composite = false;

    memset(&metrics, 0, sizeof metrics);
    if (gfnt_face_glyph_name(face, glyph, nullptr, &text, nullptr, &error)
        == GFNT_OK) {
      gfnt_name_free(nullptr, text);
      text = nullptr;
    }
    gfnt_face_glyph_advance(face, glyph, nullptr, &advance, &error);
    gfnt_face_glyph_is_composite(face, glyph, &composite, &error);
    // The decrypted program, and the dump over it: the dump divides bytes into
    // operators, and for Type 1 those bytes came out of a cipher.
    if (gfnt_face_glyph_charstring(face, glyph, &type, &bytes, &length, &error)
        == GFNT_OK) {
      gfnt_charstring_dump(type, bytes, length, nullptr, sink());
    }
    gfnt_face_glyph_charstring_metrics(face, glyph, &metrics, &error);
    if (gfnt_face_glyph_outline(face, glyph, nullptr, nullptr, &outline, &error)
        != GFNT_OK) {
      continue;
    }
    GFNT_Box drawn;
    GFNT_Box control;

    memset(&drawn, 0, sizeof drawn);
    memset(&control, 0, sizeof control);
    gfnt_outline_bounds(outline, &drawn);
    gfnt_outline_control_box(outline, &control);
    gfnt_outline_dump(outline, sink());
    gfnt_outline_destroy(outline);

    // And through the scan converter at one small size, because the em came from
    // a FontMatrix the file chose and a wrong one lands in the rasteriser.
    GFNT_Coverage coverage;

    memset(&coverage, 0, sizeof coverage);
    if (gfnt_face_render_glyph(face, glyph, 12, nullptr, nullptr, &coverage,
            &error)
        == GFNT_OK) {
      gfnt_coverage_total(&coverage);
      gfnt_coverage_hash(&coverage);
      gfnt_coverage_destroy(&coverage);
    }
  }

  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
