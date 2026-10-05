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
 * Fuzz harness for `HVAR` and `MVAR`: the two tables together, because both read
 * one structure - an item variation store - and what a store has to survive is its
 * own offsets: a region list past the table, a data block's counts, a delta-set
 * index map's entry width, an index past anything. A fuzzer given one table would
 * rediscover the other's header.
 *
 * The first byte is the options byte (documentation/design.md section 14.2), the
 * second a split point: bytes before it are the `HVAR` and bytes after it the
 * `MVAR`, as in fuzz_variation. Bits 0 and 1 of the options are the axis count less
 * one. The face is fixed apart from that - four glyphs, an `hhea`, an `OS/2` - so
 * that every accessor that reads these tables has something to add a delta to.
 *
 * Every metric accessor is asked at several locations, and two properties are
 * checked, because a metric is the one place a wrong answer is a number nobody
 * can see is wrong: **a variation that moves nothing is the default instance** (the
 * tables are not read at all, so damage in them cannot reach it), and **asking
 * twice gives one answer** - no state is kept between calls, so a lookup that
 * leaves something behind would show as the second differing from the first.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/variation.h>

#include "../sfnt_builder.h"

namespace {

/** An `fvar` of @p axes axes, each 100..400..900, and no instances. */
std::vector<uint8_t> fvar_table(size_t axes) {
  std::vector<uint8_t> out;

  gfnttest::put_u16(out, 1);
  gfnttest::put_u16(out, 0);
  gfnttest::put_u16(out, 16);
  gfnttest::put_u16(out, 2);
  gfnttest::put_u16(out, static_cast<uint16_t>(axes));
  gfnttest::put_u16(out, 20);
  gfnttest::put_u16(out, 0);
  gfnttest::put_u16(out, static_cast<uint16_t>(4 + 4 * axes));
  for (size_t i = 0; i < axes; ++i) {
    out.push_back('a');
    out.push_back('x');
    out.push_back('i');
    out.push_back(static_cast<uint8_t>('0' + i));
    gfnttest::put_u32(out, 100u << 16);
    gfnttest::put_u32(out, 400u << 16);
    gfnttest::put_u32(out, 900u << 16);
    gfnttest::put_u16(out, 0);
    gfnttest::put_u16(out, 256);
  }
  return out;
}

/** Whether two results, and the values they wrote, are one answer. */
void same(GFNT_Result a, int32_t x, GFNT_Result b, int32_t y) {
  if (a != b || (a == GFNT_OK && x != y)) {
    abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 4) {
    return 0;
  }
  const uint8_t options = data[0];
  const size_t axes = 1u + (options & 0x03u);
  const size_t split = 1u + (static_cast<size_t>(data[1]) * (size - 2u)) / 256u;
  const std::vector<uint8_t> hvar(data + 2, data + 2 + split);
  const std::vector<uint8_t> mvar(data + 2 + split, data + size);
  std::vector<std::pair<uint16_t, int16_t>> metrics;

  for (size_t i = 0; i < 4; ++i) {
    metrics.push_back({static_cast<uint16_t>(500 + i), static_cast<int16_t>(i)});
  }
  std::vector<gfnttest::Table> tables = {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, 4)},
      {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics, {})},
      {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(4)},
      {GFNT_TAG('O', 'S', '/', '2'), gfnttest::build_os2()},
      {GFNT_TAG('f', 'v', 'a', 'r'), fvar_table(axes)},
  };
  if (!hvar.empty()) {
    tables.push_back({GFNT_TAG('H', 'V', 'A', 'R'), hvar});
  }
  if (!mvar.empty()) {
    tables.push_back({GFNT_TAG('M', 'V', 'A', 'R'), mvar});
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

  // The extremes, an interior point, a value that is no fraction of anything
  // simple, and a mix of signs across axes.
  const GFNT_F2Dot14 places[][4] = {
      {16384, 0, 0, 0}, {-16384, 0, 0, 0}, {8192, 8192, 8192, 8192},
      {1, -1, 12345, -12345}, {16384, 16384, 16384, 16384}, {0, 0, 0, 0},
  };
  const GFNT_LineMetricsPolicy policies[] = {
      GFNT_LINE_METRICS_FONT, GFNT_LINE_METRICS_TYPO, GFNT_LINE_METRICS_WIN,
      GFNT_LINE_METRICS_HHEA};

  for (const auto & place : places) {
    const GFNT_Variation at{place, axes};
    bool still = true;

    for (size_t i = 0; i < axes; ++i) {
      still = still && place[i] == 0;
    }
    for (uint32_t glyph = 0; glyph < 6; ++glyph) {
      int32_t first = 0;
      int32_t second = 0;
      int32_t plain = 0;
      GFNT_Result a = gfnt_face_glyph_advance(face, glyph, &at, &first, &error);
      GFNT_Result b = gfnt_face_glyph_advance(face, glyph, &at, &second, &error);

      same(a, first, b, second);
      a = gfnt_face_glyph_side_bearing(face, glyph, &at, &first, &error);
      b = gfnt_face_glyph_side_bearing(face, glyph, &at, &second, &error);
      same(a, first, b, second);
      if (still) {
        // The default instance: the tables are not read, so whatever they hold
        // cannot have changed the answer.
        a = gfnt_face_glyph_advance(face, glyph, &at, &first, &error);
        b = gfnt_face_glyph_advance(face, glyph, nullptr, &plain, &error);
        same(a, first, b, plain);
      }
    }
    for (GFNT_LineMetricsPolicy policy : policies) {
      GFNT_LineMetrics a{};
      GFNT_LineMetrics b{};
      const GFNT_Result x = gfnt_face_line_metrics(face, policy, &at, &a, &error);
      const GFNT_Result y = gfnt_face_line_metrics(face, policy, &at, &b, &error);

      if (x != y
          || (x == GFNT_OK
              && (a.ascent != b.ascent || a.descent != b.descent
                  || a.line_gap != b.line_gap))) {
        abort();
      }
    }
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
