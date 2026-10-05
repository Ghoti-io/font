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
 * Fuzz harness for `CFF2`: the whole table, as `fuzz_cff` takes a whole `CFF `, and
 * for the same reason - a CFF2 is a nest of offsets (the Top DICT at CharStrings,
 * FDArray, FDSelect and the variation store; each Font DICT at its Private DICT,
 * which points back at its own local subroutines) - and a fuzzer given one
 * structure cannot write the offset that reaches another.
 *
 * What is CFF2's own is `blend`, which scales deltas by region scalars read from a
 * store the table itself carries, so every glyph is drawn at a spread of
 * locations and not only at the default. The first byte is the options byte
 * (documentation/design.md section 14.2): bit 0 gives the face a second axis, bits
 * 1 to 3 pick which of the glyph caps are tightened, and the rest of it is their
 * value, as in fuzz_cff.
 *
 * Properties, beyond that nothing crashes: **asking twice gives one answer**, since
 * nothing is kept between glyphs but the parsed table; and **a location that moves
 * nothing is the default instance**, point for point, which is a property of
 * `blend` too - every scalar is zero there - and one no sanitizer sees.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <ghoti.io/font/charstring.h>
#include <ghoti.io/font/font.h>
#include <ghoti.io/font/outline.h>

#include "../sfnt_builder.h"

namespace {

FILE * sink() {
  static FILE * stream = fopen("/dev/null", "wb");
  return stream;
}

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

std::vector<uint8_t> maxp_table(uint16_t glyphs) {
  std::vector<uint8_t> out = {0x00, 0x00, 0x50, 0x00};

  gfnttest::put_u16(out, glyphs);
  return out;
}

void check(bool ok) {
  if (!ok) {
    abort();
  }
}

/** The outline's points, or an empty list and the failure's result. */
std::vector<int32_t> points_of(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, GFNT_Result * result) {
  GFNT_Outline * outline = nullptr;
  GFNT_Error error;
  std::vector<int32_t> out;

  gfnt_error_clear(&error);
  *result = gfnt_face_glyph_outline(face, glyph, variation, nullptr, &outline,
      &error);
  if (*result != GFNT_OK) {
    return out;
  }
  for (size_t i = 0; i < gfnt_outline_point_count(outline); ++i) {
    GFNT_Point at;

    if (gfnt_outline_point_at(outline, i, &at, nullptr) == GFNT_OK) {
      out.push_back(at.x);
      out.push_back(at.y);
    }
  }
  gfnt_outline_destroy(outline);
  return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 2) {
    return 0;
  }
  const uint8_t options = data[0];
  const size_t axes = 1u + (options & 0x01u);
  const std::vector<uint8_t> cff2(data + 1, data + size);
  const std::vector<uint8_t> font = gfnttest::build_sfnt(GFNT_FLAVOUR_CFF, {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
      {GFNT_TAG('m', 'a', 'x', 'p'), maxp_table(256)},
      {GFNT_TAG('f', 'v', 'a', 'r'), fvar_table(axes)},
      {GFNT_TAG('C', 'F', 'F', '2'), cff2},
  });

  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  if ((options & 0x02u) != 0) {
    limits.max_outline_points = 1u + (options >> 4);
  }
  if ((options & 0x04u) != 0) {
    limits.max_charstring_depth = options >> 6;
  }
  if ((options & 0x08u) != 0) {
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
    const size_t reach = count < 32u ? count : 32u;
    const GFNT_F2Dot14 places[][2] = {
        {16384, 0}, {-16384, 0}, {8192, 8192}, {1, -1}, {12345, -12345},
        {16384, 16384}, {0, 0},
    };

    for (size_t glyph = 0; glyph < reach; ++glyph) {
      const uint32_t index = static_cast<uint32_t>(glyph);
      GFNT_CharstringType type = GFNT_CHARSTRING_CFF2;
      GFNT_CharstringMetrics metrics;
      const uint8_t * bytes = nullptr;
      size_t length = 0;
      GFNT_Result first = GFNT_OK;
      GFNT_Result second = GFNT_OK;
      GFNT_Result plain = GFNT_OK;

      memset(&metrics, 0, sizeof metrics);
      const std::vector<int32_t> base = points_of(face, index, nullptr, &plain);

      if (gfnt_face_glyph_charstring(face, index, &type, &bytes, &length, &error)
          == GFNT_OK) {
        (void)gfnt_charstring_dump(type, bytes, length, nullptr, sink());
        (void)gfnt_face_glyph_charstring_metrics(face, index, &metrics, &error);
      }
      for (const auto & place : places) {
        const GFNT_Variation at{place, axes, GFNT_DELTA_ROUND_HALF_UP };
        bool still = true;

        for (size_t i = 0; i < axes; ++i) {
          still = still && place[i] == 0;
        }
        const std::vector<int32_t> a = points_of(face, index, &at, &first);
        const std::vector<int32_t> b = points_of(face, index, &at, &second);

        check(first == second);
        if (first == GFNT_OK) {
          check(a == b);
          if (still) {
            // A location that moves nothing is the default, and so is `blend`
            // there: every scalar is zero.
            check(plain == GFNT_OK && a == base);
          }
        }
      }
    }
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
