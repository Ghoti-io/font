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
 * Fuzz harness for the four tables that vary a font without moving a point: `STAT`,
 * the `FeatureVariations` of `GSUB` and `GPOS`, `cvt `/`cvar`, and a version 2
 * `avar`. They are one harness because they are one kind of input - a table of
 * offsets that each point at a record, read in place on every call - and one
 * property family.
 *
 * The first byte is the options byte (documentation/design.md section 14.2). Bits 0
 * and 1 are the axis count less one; bits 4 and 5 choose the table the rest of the
 * input is: 0 `STAT`, 1 `GSUB` (bit 6 makes it `GPOS`), 2 a `cvt ` and a `cvar`
 * with a split point after the options byte, 3 an `avar`. The face is fixed apart
 * from that, so what varies is what the fuzzer controls.
 *
 * Properties, beyond that nothing crashes: **asking twice gives one answer**, since
 * every one of these reads in place and keeps nothing; **a location that moves
 * nothing answers with the table's own values** for control values; **a count is
 * the number of things read** for `STAT`, so the two ways of asking agree; and **a
 * normalised coordinate is within -1..1**.
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

void check(bool ok) {
  if (!ok) {
    abort();
  }
}

const GFNT_F2Dot14 kPlaces[][4] = {
    {16384, 0, 0, 0}, {-16384, 0, 0, 0}, {8192, 8192, 8192, 8192},
    {1, -1, 12345, -12345}, {16384, 16384, 16384, 16384}, {0, 0, 0, 0},
};

void exercise_stat(const GFNT_Face * face, size_t axes) {
  GFNT_Error error;
  size_t axis_count = 0;
  size_t value_count = 0;
  uint16_t fallback = 0;

  gfnt_error_clear(&error);
  (void)gfnt_face_stat_elided_fallback(face, &fallback, &error);
  if (gfnt_face_stat_axis_count(face, &axis_count, &error) != GFNT_OK
      || gfnt_face_stat_value_count(face, &value_count, &error) != GFNT_OK) {
    return;
  }
  for (size_t i = 0; i < axis_count && i < 64; ++i) {
    GFNT_StatAxis axis;

    (void)gfnt_face_stat_axis_at(face, i, &axis, &error);
  }
  for (size_t i = 0; i < value_count && i < 512; ++i) {
    GFNT_StatValue value;

    if (gfnt_face_stat_value_at(face, i, &value, &error) == GFNT_OK
        && value.format == GFNT_STAT_MULTI) {
      for (size_t p = 0; p < value.pair_count && p < 64; ++p) {
        uint16_t axis = 0;
        GFNT_F16Dot16 coordinate = 0;

        check(gfnt_face_stat_value_pair(face, i, p, &axis, &coordinate, &error)
            == GFNT_OK);
        check(axis < axis_count);
      }
    }
  }
  for (const auto & place : kPlaces) {
    GFNT_F16Dot16 user[4];
    size_t counted = 0;
    size_t listed = 0;
    size_t again = 0;
    size_t matches[8];

    for (size_t i = 0; i < axes; ++i) {
      user[i] = static_cast<GFNT_F16Dot16>(place[i]) * 65536 / 16;
    }
    const GFNT_Result a = gfnt_face_stat_match(face, user, axes, nullptr, 0,
        &counted, &error);
    const GFNT_Result b = gfnt_face_stat_match(face, user, axes, matches, 8,
        &listed, &error);
    const GFNT_Result c = gfnt_face_stat_match(face, user, axes, matches, 8,
        &again, &error);

    check(a == b && b == c);
    if (a == GFNT_OK) {
      check(counted == listed && listed == again);
      for (size_t i = 0; i < listed && i < 8; ++i) {
        check(matches[i] < value_count);
      }
    }
  }
  FILE * null = fopen("/dev/null", "w");

  if (null) {
    (void)gfnt_face_stat_dump(face, null);
    fclose(null);
  }
}

void exercise_layout(const GFNT_Face * face, GFNT_Tag tag, size_t axes) {
  GFNT_Error error;
  size_t count = 0;

  gfnt_error_clear(&error);
  if (gfnt_face_feature_variations_count(face, tag, &count, &error) != GFNT_OK) {
    return;
  }
  for (const auto & place : kPlaces) {
    size_t first = 12345;
    size_t second = 54321;
    const GFNT_Result a = gfnt_face_feature_variations_match(face, tag, place,
        axes, &first, &error);
    const GFNT_Result b = gfnt_face_feature_variations_match(face, tag, place,
        axes, &second, &error);

    check(a == b);
    if (a != GFNT_OK) {
      continue;
    }
    check(first == second);
    check(first == GFNT_FEATURE_VARIATIONS_NONE || first < count);
    if (first != GFNT_FEATURE_VARIATIONS_NONE) {
      size_t substitutions = 0;

      if (gfnt_face_feature_substitution_count(face, tag, first, &substitutions,
              &error) == GFNT_OK) {
        for (size_t s = 0; s < substitutions && s < 64; ++s) {
          uint16_t feature = 0;
          size_t lookups = 0;

          if (gfnt_face_feature_substitution_at(face, tag, first, s, &feature,
                  &lookups, &error) == GFNT_OK) {
            for (size_t k = 0; k < lookups && k < 64; ++k) {
              uint16_t lookup = 0;

              check(gfnt_face_feature_substitution_lookup(face, tag, first, s, k,
                  &lookup, &error) == GFNT_OK);
            }
          }
        }
      }
    }
  }
  FILE * null = fopen("/dev/null", "w");

  if (null) {
    (void)gfnt_face_feature_variations_dump(face, tag, null);
    fclose(null);
  }
}

void exercise_cvt(const GFNT_Face * face, const std::vector<uint8_t> & cvt,
    size_t axes) {
  GFNT_Error error;
  size_t count = 0;

  gfnt_error_clear(&error);
  check(gfnt_face_cvt_count(face, &count, &error) == GFNT_OK);
  check(count == cvt.size() / 2);
  std::vector<int32_t> plain(count + 1);
  std::vector<int32_t> first(count + 1);
  std::vector<int32_t> second(count + 1);

  check(gfnt_face_cvt_values(face, nullptr, plain.data(), plain.size(), &error)
      == GFNT_OK);
  for (size_t i = 0; i < count; ++i) {
    check(plain[i] == static_cast<int16_t>((cvt[2 * i] << 8) | cvt[2 * i + 1]));
  }
  for (const auto & place : kPlaces) {
    const GFNT_Variation at{place, axes, GFNT_DELTA_ROUND_HALF_UP };
    bool still = true;

    for (size_t i = 0; i < axes; ++i) {
      still = still && place[i] == 0;
    }
    const GFNT_Result a = gfnt_face_cvt_values(face, &at, first.data(),
        first.size(), &error);
    const GFNT_Result b = gfnt_face_cvt_values(face, &at, second.data(),
        second.size(), &error);

    check(a == b);
    if (a == GFNT_OK) {
      check(first == second);
      if (still) {
        check(first == plain);
      }
    }
  }
}

void exercise_avar(const GFNT_Face * face, size_t axes) {
  GFNT_Error error;

  gfnt_error_clear(&error);
  for (const auto & place : kPlaces) {
    GFNT_F16Dot16 user[4];
    GFNT_F2Dot14 out[4];
    GFNT_F2Dot14 again[4];

    for (size_t i = 0; i < axes; ++i) {
      // 100 to 900 across the whole range of the coordinate: the axes' own.
      user[i] = static_cast<GFNT_F16Dot16>(
          (((static_cast<int64_t>(place[i]) + 16384) * 800) / 32768 + 100) * 65536);
    }
    const GFNT_Result a = gfnt_face_normalize(face, user, axes, out, 4, &error);
    const GFNT_Result b = gfnt_face_normalize(face, user, axes, again, 4, &error);

    check(a == b);
    if (a == GFNT_OK) {
      for (size_t i = 0; i < axes; ++i) {
        check(out[i] >= -16384 && out[i] <= 16384 && out[i] == again[i]);
      }
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 4) {
    return 0;
  }
  const uint8_t options = data[0];
  const size_t axes = 1u + (options & 0x03u);
  const unsigned which = (options >> 4) & 0x03u;
  const size_t split = 1u + (static_cast<size_t>(data[1]) * (size - 2u)) / 256u;
  const std::vector<uint8_t> first(data + 2, data + 2 + split);
  const std::vector<uint8_t> rest(data + 2 + split, data + size);
  std::vector<gfnttest::Table> tables = {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
      {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(1)},
      {GFNT_TAG('f', 'v', 'a', 'r'), fvar_table(axes)},
  };
  GFNT_Tag layout = GFNT_TAG('G', 'S', 'U', 'B');
  std::vector<uint8_t> cvt;

  switch (which) {
    case 0:
      // One table, so everything after the two header bytes.
      tables.push_back({GFNT_TAG('S', 'T', 'A', 'T'),
          std::vector<uint8_t>(data + 2, data + size)});
      break;
    case 1:
      if (options & 0x40u) {
        layout = GFNT_TAG('G', 'P', 'O', 'S');
      }
      tables.push_back({layout, std::vector<uint8_t>(data + 2, data + size)});
      break;
    case 2:
      cvt = first;
      tables.push_back({GFNT_TAG('c', 'v', 't', ' '), first});
      if (!rest.empty()) {
        tables.push_back({GFNT_TAG('c', 'v', 'a', 'r'), rest});
      }
      break;
    default:
      tables.push_back({GFNT_TAG('a', 'v', 'a', 'r'),
          std::vector<uint8_t>(data + 2, data + size)});
      break;
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
  switch (which) {
    case 0: exercise_stat(face, axes); break;
    case 1: exercise_layout(face, layout, axes); break;
    case 2: exercise_cvt(face, cvt, axes); break;
    default: exercise_avar(face, axes); break;
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
