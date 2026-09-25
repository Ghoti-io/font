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
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/font.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/name.h>

namespace {

/** A stream the dumps can be written to without producing output. */
FILE * sink() {
  static FILE * stream = fopen("/dev/null", "wb");
  return stream;
}

/** Codepoints worth asking about, including every boundary of every format. */
const uint32_t kCodepoints[] = {
  0x0, 0x20, 0x41, 0x7F, 0xFF, 0x100, 0xD7FF, 0xE000, 0xF041, 0xFFFE, 0xFFFF,
  0x10000, 0x1F600, 0x10FFFF, 0x110000, 0xFFFFFFFF,
};

/** Every table read through the face, with its dump. */
void walk_tables(const GFNT_Face * face, bool with_dump) {
  const GFNT_Head * head = nullptr;
  const GFNT_Hhea * hhea = nullptr;
  const GFNT_Os2 * os2 = nullptr;
  const GFNT_Post * post = nullptr;
  GFNT_Error error;

  gfnt_error_clear(&error);
  if (gfnt_face_head(face, &head, &error) == GFNT_OK && with_dump) {
    gfnt_head_dump(head, sink());
  }
  if (gfnt_face_hhea(face, &hhea, &error) == GFNT_OK && with_dump) {
    gfnt_hhea_dump(hhea, sink());
  }
  if (gfnt_face_os2(face, &os2, &error) == GFNT_OK && with_dump) {
    gfnt_os2_dump(os2, sink());
  }
  if (gfnt_face_post(face, &post, &error) == GFNT_OK && with_dump) {
    gfnt_post_dump(post, sink());
  }

  uint16_t units = 0;
  gfnt_face_units_per_em(face, &units, &error);

  // Asking twice, because the second answer comes from the memo and a memo
  // that disagrees with its own first answer is a defect this reaches.
  size_t count = 0;
  size_t again = 1;
  if (gfnt_face_num_glyphs(face, &count, &error) == GFNT_OK) {
    gfnt_face_num_glyphs(face, &again, nullptr);
    if (again != count) {
      __builtin_trap();
    }
    gfnt_face_num_glyphs_disagreement(face, nullptr);

    size_t limit = count < 8 ? count : 8;
    for (size_t glyph = 0; glyph <= limit; ++glyph) {
      int32_t advance = 0;
      int32_t bearing = 0;
      gfnt_face_glyph_advance(face, (uint32_t)glyph, nullptr, &advance, &error);
      gfnt_face_glyph_side_bearing(face, (uint32_t)glyph, nullptr, &bearing,
          &error);
    }
  }

  for (int policy = 0; policy <= GFNT_LINE_METRICS_WIN; ++policy) {
    GFNT_LineMetrics metrics;
    memset(&metrics, 0, sizeof metrics);
    gfnt_face_line_metrics(face, (GFNT_LineMetricsPolicy)policy, nullptr,
        &metrics, &error);
  }
}

/** The cmap, through both the preferred subtable and each one by name. */
void walk_cmap(const GFNT_Face * face, bool with_dump) {
  GFNT_CmapSubtable subtable;
  GFNT_Error error;
  size_t count = 0;

  gfnt_error_clear(&error);
  memset(&subtable, 0, sizeof subtable);
  if (with_dump) {
    gfnt_face_cmap_dump(face, sink());
  }
  if (gfnt_face_cmap_best(face, &subtable, &error) == GFNT_OK) {
    for (uint32_t codepoint : kCodepoints) {
      uint32_t glyph = 0;
      gfnt_face_glyph_for_codepoint(face, codepoint, &glyph, &error);
    }
  }
  if (gfnt_face_cmap_count(face, &count, &error) != GFNT_OK) {
    return;
  }
  for (size_t i = 0; i < count && i < 8; ++i) {
    if (gfnt_face_cmap_at(face, i, &subtable, &error) != GFNT_OK) {
      continue;
    }
    for (uint32_t codepoint : kCodepoints) {
      uint32_t glyph = 0;
      gfnt_cmap_lookup(face, &subtable, codepoint, &glyph, &error);
    }
  }
}

/** Every name record, decoded, plus the preference order. */
void walk_names(const GFNT_Face * face, bool with_dump) {
  GFNT_Error error;
  size_t count = 0;

  gfnt_error_clear(&error);
  if (with_dump) {
    gfnt_face_name_dump(face, sink());
  }
  if (gfnt_face_name_count(face, &count, &error) == GFNT_OK) {
    for (size_t i = 0; i < count && i < 16; ++i) {
      GFNT_NameRecord record;
      char * text = nullptr;

      memset(&record, 0, sizeof record);
      if (gfnt_face_name_at(face, i, &record, &error) != GFNT_OK) {
        continue;
      }
      if (gfnt_face_name_decode(face, &record, nullptr, &text, nullptr, &error)
          == GFNT_OK) {
        gfnt_name_free(nullptr, text);
      }
    }
  }
  for (uint16_t name_id : {GFNT_NAME_FAMILY, GFNT_NAME_POSTSCRIPT,
           GFNT_NAME_VARIATIONS_PREFIX}) {
    char * text = nullptr;
    if (gfnt_face_name(face, name_id, GFNT_LANGUAGE_ANY, nullptr, &text,
            nullptr, &error)
        == GFNT_OK) {
      gfnt_name_free(nullptr, text);
    }
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 1) {
    return 0;
  }

  uint8_t options = data[0];
  const uint8_t * bytes = data + 1;
  size_t length = size - 1;

  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  // Each bit drives a cap down to a value the input can actually reach, so the
  // ERR_LIMIT arms are exercised rather than being unreachable promises.
  if (options & 0x01u) {
    limits.max_tables = 8;
  }
  if (options & 0x02u) {
    limits.max_glyphs = 32;
  }
  if (options & 0x04u) {
    limits.max_name_records = 4;
  }
  if (options & 0x08u) {
    limits.max_blob_bytes = 4096;
  }
  size_t face_index = (options >> 4) & 0x03u;
  bool with_dump = (options & 0x40u) != 0;

  GFNT_Blob * blob = nullptr;
  GFNT_Error error;
  gfnt_error_clear(&error);
  // Borrowed, not copied: a read one byte past the end lands in ASan's
  // redzone around the fuzzer's own buffer.
  if (gfnt_blob_create_memory(bytes, length, GFNT_BLOB_BORROWED, &limits,
          nullptr, &blob, &error)
      != GFNT_OK) {
    return 0;
  }
  if (with_dump) {
    gfnt_blob_dump(blob, sink());
  }

  size_t faces = 0;
  gfnt_face_count(blob, &limits, &faces, &error);

  for (size_t index : {(size_t)0, face_index}) {
    GFNT_Face * face = nullptr;

    if (gfnt_face_load(blob, index, &limits, nullptr, &face, &error)
        != GFNT_OK) {
      continue;
    }
    if (with_dump) {
      gfnt_face_dump(face, sink());
    }

    size_t tables = gfnt_face_table_count(face);
    for (size_t i = 0; i < tables && i < 32; ++i) {
      GFNT_Tag tag = 0;
      uint32_t stored = 0;
      uint32_t computed = 0;

      if (gfnt_face_table_tag_at(face, i, &tag) != GFNT_OK) {
        continue;
      }
      gfnt_face_has_table(face, tag);
      gfnt_face_table_range(face, tag, nullptr, nullptr);
      gfnt_face_table_checksum(face, tag, &stored, &computed);
    }

    walk_tables(face, with_dump);
    walk_cmap(face, with_dump);
    walk_names(face, with_dump);
    gfnt_face_free(face);
  }

  gfnt_blob_destroy(blob);
  return 0;
}
