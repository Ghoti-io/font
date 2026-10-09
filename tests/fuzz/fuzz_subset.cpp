/**
 * @file
 *
 * libFuzzer harness for the sfnt container and every table parser behind it.
 *
 * documentation/design.md section 14.2. The whole input after the first byte is
 * a font file, so the fuzzer works on the directory, the collection header and
 * every table this library parses at once - which is the right shape for layer
 * 0, because those are all offset arithmetic over the same bytes.
 *
 * **The first byte is the options byte**, and it drives GFNT_Limits and the
 * face index. Without it the fuzzer would only ever exercise the default caps,
 * and a limit nothing reaches is a promise nobody has checked (section 15.2).
 *
 * The blob borrows the fuzzer's buffer rather than copying it, so a read one
 * byte past the end is an ASan report rather than a read of this harness's own
 * heap.
 *
 * Copyright 2026 by Corey Pennycuff

/**
 * @file
 *
 * Fuzz the subsetter: a whole font, and a byte of options for what to ask of it.
 *
 * The subsetter reads whatever the loader accepted, and the loader accepts
 * fonts whose tables disagree with each other, so this is the writer's way of
 * meeting what the parser tolerates. One property is checked and not only that
 * nothing crashes: **a subset that was made loads again**, with a glyph count
 * that is the one the call produced. A subsetter that wrote a font its own
 * reader refuses has a bug whichever side it is on.
 *
 * The first byte: bit 0 retains glyph ids, 1 skips the `GSUB` closure, 2 drops
 * hinting, 3 drops the layout tables, 4 caps the glyph limit low, 5 caps the
 * blob limit low, 6 asks for a restricted feature list.
 */

#include <cstdint>
#include <cstring>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/write.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 1) {
    return 0;
  }
  const uint8_t bits = data[0];
  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  if (bits & 0x10u) {
    limits.max_glyphs = 24;
  }
  if (bits & 0x20u) {
    limits.max_blob_bytes = 8192;
  }

  GFNT_Blob * blob = nullptr;
  if (gfnt_blob_create_memory(data + 1, size - 1, GFNT_BLOB_BORROWED, &limits,
          nullptr, &blob, nullptr) != GFNT_OK) {
    return 0;
  }
  GFNT_Face * face = nullptr;
  if (gfnt_face_load(blob, 0, &limits, nullptr, &face, nullptr) == GFNT_OK) {
    static const uint32_t kCodepoints[] = {0x20, 0x41, 0x42, 0x66, 0x69, 0xE9,
        0x300, 0x627, 0x915, 0x4E00, 0x10000, 0x1F600};
    static const uint32_t kGlyphs[] = {1, 2, 5, 40};
    static const GFNT_Tag kFeatures[] = {GFNT_TAG('l', 'i', 'g', 'a'),
        GFNT_TAG('c', 'c', 'm', 'p')};
    GFNT_SubsetOptions options;

    gfnt_subset_options_init(&options);
    options.codepoints = kCodepoints;
    options.codepoint_count = sizeof kCodepoints / sizeof *kCodepoints;
    options.glyphs = kGlyphs;
    options.glyph_count = sizeof kGlyphs / sizeof *kGlyphs;
    options.retain_gids = (bits & 0x01u) != 0;
    options.no_gsub_closure = (bits & 0x02u) != 0;
    options.drop_hinting = (bits & 0x04u) != 0;
    options.drop_layout = (bits & 0x08u) != 0;
    if (bits & 0x40u) {
      options.features = kFeatures;
      options.feature_count = 2;
    }

    GFNT_Blob * out = nullptr;
    if (gfnt_subset(face, &options, &limits, nullptr, &out, nullptr) == GFNT_OK) {
      GFNT_Face * again = nullptr;

      if (gfnt_face_load(out, 0, nullptr, nullptr, &again, nullptr) != GFNT_OK) {
        __builtin_trap();
      }
      size_t count = 0;
      if (gfnt_face_num_glyphs(again, &count, nullptr) != GFNT_OK || count == 0) {
        __builtin_trap();
      }
      gfnt_face_free(again);
      gfnt_blob_destroy(out);
    }
    gfnt_face_free(face);
  }
  gfnt_blob_destroy(blob);
  return 0;
}
