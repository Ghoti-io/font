/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Font.
 *
 * Ghoti.io Font is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Font is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Fuzz harness for the `SVG ` table: the whole table is the input after an options byte.
 * A face of eight glyphs is built around it and every reader is run: the count, every record,
 * a lookup of every glyph (and a few past the end), and the inflation of every document under the
 * default caps and under a tight one.
 *
 * Properties, beyond that nothing crashes or reads outside the table: **a record that reads
 * lies inside the table**, **a glyph a lookup finds is in the range of the record it returns**,
 * and **asking twice gives one answer**.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <ghoti.io/font/font.h>

#include "../sfnt_builder.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 2) {
    return 0;
  }
  const uint8_t options = data[0];
  const std::vector<uint8_t> svg(data + 1, data + size);
  std::vector<std::pair<uint16_t, int16_t>> metrics(8, {500, 0});
  std::vector<gfnttest::Table> tables = {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, 8)},
      {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics)},
      {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(8)},
      {GFNT_TAG('S', 'V', 'G', ' '), svg},
  };
  const std::vector<uint8_t> font = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Limits limits;

  gfnt_limits_default(&limits);
  if (options & 0x01u) {
    limits.max_blob_bytes = 64;
  }
  if (gfnt_blob_create_memory(font.data(), font.size(), GFNT_BLOB_BORROWED, nullptr, nullptr,
          &blob, nullptr) != GFNT_OK) {
    return 0;
  }
  if (gfnt_face_load(blob, 0, &limits, nullptr, &face, nullptr) != GFNT_OK) {
    gfnt_blob_destroy(blob);
    return 0;
  }
  size_t count = 0;
  const GFNT_Result counted = gfnt_face_svg_document_count(face, &count, nullptr);

  if (counted == GFNT_OK) {
    for (size_t i = 0; i < count && i < 512; ++i) {
      GFNT_SvgDocument doc{};
      GFNT_SvgDocument again{};
      const GFNT_Result a = gfnt_face_svg_document_at(face, i, &doc, nullptr);
      const GFNT_Result b = gfnt_face_svg_document_at(face, i, &again, nullptr);

      if (a != b || (a == GFNT_OK && std::memcmp(&doc, &again, sizeof doc) != 0)) {
        abort();
      }
      if (a == GFNT_OK) {
        const uint8_t * base = nullptr;
        size_t length = 0;

        if (gfnt_face_table_data(face, GFNT_TAG('S', 'V', 'G', ' '), &base, &length) == GFNT_OK
            && (doc.data < base || doc.data + doc.length > base + length)) {
          abort();
        }
        GFNT_Blob * text = nullptr;
        if (gfnt_svg_document_inflate(face, &doc, nullptr, &text, nullptr) == GFNT_OK) {
          gfnt_blob_destroy(text);
        }
      }
    }
  }
  for (uint32_t g = 0; g < 12; ++g) {
    GFNT_SvgDocument doc{};
    bool found = false;
    const GFNT_Result r = gfnt_face_glyph_svg(face, g, &doc, &found, nullptr);

    if (r == GFNT_OK && found && (g < doc.first_glyph || g > doc.last_glyph)) {
      abort();
    }
    if (r != GFNT_OK && found) {
      abort();
    }
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
