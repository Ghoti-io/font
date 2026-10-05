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
 * Fuzz harness for `gvar`: a whole table, read against a fixed set of glyphs at a
 * location the input chooses.
 *
 * The glyphs are fixed and the table is the input, because what a `gvar` has to be
 * robust against is its own offsets - a header's glyph offsets, a tuple header's
 * sizes, a packed list's run lengths, a point number past the glyph - and a
 * fuzzer given a whole font would spend its budget rediscovering a directory. The
 * glyphs cover the three shapes a tuple is applied to: a simple glyph with two
 * contours (so inference has a contour boundary to respect), a composite whose
 * components are its "points", and a glyph with no contours at all.
 *
 * **Two properties are checked, not just that nothing crashes.** An outline at a
 * location has exactly the points and contours the default instance has - `gvar`
 * moves points, it never adds or removes one - and a variation that moves nothing
 * draws the default. A reader that produced a different count, or applied a delta
 * at the default, would be wrong in a way no sanitizer sees. Neither can fail by
 * construction in the code as it is today; they are here for the edit that makes
 * one true, which is the only kind of edit a property like this can catch.
 *
 * The first byte is the options byte (documentation/design.md section 14.2): bits
 * 0 and 1 are the axis count less one, bit 2 makes the harness write the font's
 * own axis count over the table's, which is what lets the input reach tuple
 * parsing rather than bouncing off the first check, and the high bits drive
 * GFNT_Limits. The next eight bytes are four coordinates.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/outline.h>
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

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 10) {
    return 0;
  }
  const uint8_t options = data[0];
  const size_t axes = 1u + (options & 0x03u);
  GFNT_F2Dot14 coordinates[4];

  for (size_t i = 0; i < 4; ++i) {
    // Clamped to -1..1, which is what normalisation produces: a coordinate outside
    // it is the caller's mistake and not this harness's subject.
    int value = static_cast<int16_t>((data[1 + 2 * i] << 8) | data[2 + 2 * i]);
    if (value > 16384) {
      value = 16384;
    }
    if (value < -16384) {
      value = -16384;
    }
    coordinates[i] = static_cast<GFNT_F2Dot14>(value);
  }
  std::vector<uint8_t> gvar(data + 9, data + size);
  if ((options & 0x04u) != 0 && gvar.size() >= 6) {
    gvar[4] = 0;
    gvar[5] = static_cast<uint8_t>(axes);
  }

  // glyph 0: two contours, one with an off-curve point; 1: a composite of it twice;
  // 2: nothing at all; 3: a composite whose second component is point-matched.
  std::vector<std::vector<uint8_t>> glyphs;
  glyphs.push_back(gfnttest::build_glyf_glyph({
      {{0, 0, true}, {0, 100, false}, {100, 100, true}, {100, 0, true}},
      {{20, 20, true}, {20, 60, true}, {60, 60, true}},
  }));
  glyphs.push_back(gfnttest::build_glyf_composite({
      {0, 0x0002, 10, 20},
      {0, 0x0002, 300, -40},
  }));
  glyphs.push_back(gfnttest::build_glyf_glyph({}));
  glyphs.push_back(gfnttest::build_glyf_composite({
      {0, 0x0002, 5, 5},
      {0, 0x0000, 0, 1},
  }));
  std::vector<uint8_t> glyf;
  std::vector<uint8_t> loca;
  bool long_form = false;
  gfnttest::build_glyf_and_loca(glyphs, &glyf, &loca, &long_form);
  std::vector<std::pair<uint16_t, int16_t>> metrics;
  for (size_t i = 0; i < glyphs.size(); ++i) {
    metrics.push_back({500, 0});
  }
  const std::vector<uint8_t> font = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, long_form ? 1 : 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0,
          static_cast<uint16_t>(glyphs.size()))},
      {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics, {})},
      {GFNT_TAG('m', 'a', 'x', 'p'),
          gfnttest::build_maxp(static_cast<uint16_t>(glyphs.size()))},
      {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
      {GFNT_TAG('l', 'o', 'c', 'a'), loca},
      {GFNT_TAG('f', 'v', 'a', 'r'), fvar_table(axes)},
      {GFNT_TAG('g', 'v', 'a', 'r'), gvar},
  });

  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  if ((options & 0x08u) != 0) {
    limits.max_outline_points = 1u + (options >> 4);
  }
  if ((options & 0x10u) != 0) {
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

  const GFNT_Variation at_location{coordinates, axes};
  const GFNT_F2Dot14 zero[4] = {0, 0, 0, 0};
  const GFNT_Variation at_default{zero, axes};

  for (uint32_t glyph = 0; glyph < glyphs.size(); ++glyph) {
    GFNT_Outline * plain = nullptr;
    GFNT_Outline * moved = nullptr;
    GFNT_Outline * still = nullptr;

    const GFNT_Result base = gfnt_face_glyph_outline(face, glyph, nullptr,
        nullptr, &plain, &error);
    const GFNT_Result varied = gfnt_face_glyph_outline(face, glyph, &at_location,
        nullptr, &moved, &error);
    const GFNT_Result defaulted = gfnt_face_glyph_outline(face, glyph,
        &at_default, nullptr, &still, &error);

    if (base == GFNT_OK && defaulted == GFNT_OK) {
      // A variation that moves nothing is the default instance, point for point.
      if (gfnt_outline_point_count(plain) != gfnt_outline_point_count(still)) {
        abort();
      }
      for (size_t i = 0; i < gfnt_outline_point_count(plain); ++i) {
        GFNT_Point a;
        GFNT_Point b;

        gfnt_outline_point_at(plain, i, &a, nullptr);
        gfnt_outline_point_at(still, i, &b, nullptr);
        if (a.x != b.x || a.y != b.y) {
          abort();
        }
      }
    }
    {
      // The same glyph's advance at the location, which in a font with no `HVAR`
      // is read from the phantom points of this very table. The property: a
      // variation that moves nothing is the default's advance, and asking twice
      // gives one answer.
      int32_t at_plain = 0;
      int32_t at_still = 0;
      int32_t first = 0;
      int32_t second = 0;
      const GFNT_Result a = gfnt_face_glyph_advance(face, glyph, nullptr,
          &at_plain, &error);
      const GFNT_Result b = gfnt_face_glyph_advance(face, glyph, &at_default,
          &at_still, &error);
      const GFNT_Result c = gfnt_face_glyph_advance(face, glyph, &at_location,
          &first, &error);
      const GFNT_Result d = gfnt_face_glyph_advance(face, glyph, &at_location,
          &second, &error);

      if (a == GFNT_OK && b == GFNT_OK && at_plain != at_still) {
        abort();
      }
      if (c != d || (c == GFNT_OK && first != second)) {
        abort();
      }
    }
    if (base == GFNT_OK && varied == GFNT_OK) {
      // gvar moves points; it never adds or removes one.
      if (gfnt_outline_point_count(plain) != gfnt_outline_point_count(moved)
          || gfnt_outline_contour_count(plain)
              != gfnt_outline_contour_count(moved)) {
        abort();
      }
      GFNT_Box drawn;
      GFNT_Box control;

      memset(&drawn, 0, sizeof drawn);
      memset(&control, 0, sizeof control);
      gfnt_outline_bounds(moved, &drawn);
      gfnt_outline_control_box(moved, &control);
    }
    gfnt_outline_destroy(plain);
    gfnt_outline_destroy(moved);
    gfnt_outline_destroy(still);
  }

  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
