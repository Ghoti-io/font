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
 * Fuzz harness for `fvar` and `avar`: the two tables together, because the
 * interesting inputs are in their relationship - an `avar` whose axis count is not
 * the `fvar`'s, a segment map that does not increase - and a fuzzer given one has
 * to rediscover the other.
 *
 * The first byte is the options byte (documentation/design.md section 14.2), the
 * second a split point: bytes before it are the `fvar` and bytes after it the
 * `avar`, as in fuzz_glyf. Every accessor that reads them is exercised, and one
 * property is checked: **a normalised coordinate is always within -1..1**, which
 * is the contract `gfnt_face_normalize()` states and the one thing a caller
 * indexing a table with it depends on.
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

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 4) {
    return 0;
  }
  const uint8_t options = data[0];
  const size_t split = 1u + (static_cast<size_t>(data[1]) * (size - 2u)) / 256u;
  const std::vector<uint8_t> fvar(data + 2, data + 2 + split);
  const std::vector<uint8_t> avar(data + 2 + split, data + size);
  std::vector<gfnttest::Table> tables = {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
      {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(1)},
      {GFNT_TAG('f', 'v', 'a', 'r'), fvar},
  };
  if (!avar.empty()) {
    tables.push_back({GFNT_TAG('a', 'v', 'a', 'r'), avar});
  }
  const std::vector<uint8_t> font = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE,
      tables);

  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  if ((options & 0x01u) != 0) {
    limits.max_axes = options >> 4;
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

  size_t axes = 0;
  size_t instances = 0;

  gfnt_face_is_variable(face);
  if (gfnt_face_axis_count(face, &axes, &error) == GFNT_OK) {
    for (size_t i = 0; i < axes && i < 64; ++i) {
      GFNT_Axis axis;

      if (gfnt_face_axis_at(face, i, &axis, &error) != GFNT_OK) {
        break;
      }
    }
    // Every extreme and a few between: the clamp, both halves of the default, and
    // a value that is not a whole number of anything.
    const GFNT_F16Dot16 values[] = {INT32_MIN, -(1 << 24), 0, 50 << 16, 100 << 16,
                                    250 << 16, 400 << 16, 650 << 16, 900 << 16,
                                    1 << 24, INT32_MAX, 0x1234567};
    GFNT_F16Dot16 user[64];
    GFNT_F2Dot14 normalised[64];

    for (size_t round = 0; round < sizeof values / sizeof values[0]; ++round) {
      for (size_t i = 0; i < axes && i < 64; ++i) {
        user[i] = values[(round + i) % (sizeof values / sizeof values[0])];
      }
      if (gfnt_face_normalize(face, user, axes < 64 ? axes : 64, normalised, 64,
              &error) == GFNT_OK) {
        for (size_t i = 0; i < axes && i < 64; ++i) {
          if (normalised[i] < -16384 || normalised[i] > 16384) {
            abort();
          }
        }
      }
    }
  }
  if (gfnt_face_instance_count(face, &instances, &error) == GFNT_OK) {
    for (size_t i = 0; i < instances && i < 64; ++i) {
      GFNT_NamedInstance instance;

      if (gfnt_face_instance_at(face, i, &instance, &error) != GFNT_OK) {
        break;
      }
      // Every coordinate of an instance is readable: the pointer is borrowed from
      // the face and a stride that is wrong is a read past it.
      volatile GFNT_F16Dot16 sink = 0;
      for (size_t axis = 0; axis < instance.coordinate_count; ++axis) {
        sink = instance.coordinates[axis];
      }
      (void)sink;
    }
  }
  FILE * null = fopen("/dev/null", "wb");
  if (null) {
    gfnt_face_variation_dump(face, null);
    fclose(null);
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
