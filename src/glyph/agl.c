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
 * What text a glyph name stands for, by the Adobe Glyph List specification.
 *
 * A glyph name is not text, but a PDF reader extracting text, and a Type 1 font
 * with no `cmap`, have nothing else to go on. The specification
 * (https://github.com/adobe-type-tools/agl-specification) gives the procedure,
 * and this follows it exactly: drop what follows the first period, split at
 * underscores, and map each component by the list, by `uniXXXX`, or by `uXXXX`
 * to up to four codepoints. A component that none of them maps maps to nothing,
 * and a name whose components all do is a name with no text. Zapf Dingbats has
 * its own list (`a1`, `a100`, ...), which the specification says applies only to
 * a font of that name, so the caller chooses.
 */

#include <ghoti.io/font/glyph.h>
#include <string.h>
#include "agl_data.h"

/** The entry called @p name (of @p length bytes) in a sorted table, or NULL. */
static const GFNT_AglEntry * agl_find(const GFNT_AglEntry * entries, size_t count,
    const char * name, size_t length) {
  size_t low = 0;
  size_t high = count;

  while (low < high) {
    size_t mid = low + (high - low) / 2;
    const char * candidate = entries[mid].name;
    size_t candidate_length = strlen(candidate);
    size_t common = candidate_length < length ? candidate_length : length;
    int order = memcmp(candidate, name, common);

    if (order == 0) {
      order = candidate_length < length ? -1 : candidate_length > length ? 1 : 0;
    }
    if (order == 0) {
      return &entries[mid];
    }
    if (order < 0) {
      low = mid + 1;
    }
    else {
      high = mid;
    }
  }
  return NULL;
}

/** The value of an uppercase hexadecimal digit, or -1: the specification says uppercase. */
static int agl_hex(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/** @p length hexadecimal digits at @p s as a number, or -1 if one is not uppercase hex. */
static int64_t agl_hex_run(const char * s, size_t length) {
  int64_t value = 0;
  size_t i;

  for (i = 0; i < length; i++) {
    int digit = agl_hex(s[i]);

    if (digit < 0) {
      return -1;
    }
    value = value * 16 + digit;
  }
  return value;
}

/** Whether @p value is a scalar value: not a surrogate and not past U+10FFFF. */
static bool agl_scalar(int64_t value) {
  return value >= 0 && value <= 0x10FFFF && !(value >= 0xD800 && value <= 0xDFFF);
}

/**
 * Append the codepoints of one component, counting them whether or not they fit.
 *
 * @return how many it stands for; 0 when no rule maps it.
 */
static size_t agl_component(const char * part, size_t length, bool zapf_dingbats,
    uint32_t * out, size_t capacity, size_t at) {
  const GFNT_AglEntry * entry;
  size_t i;

  if (length == 0) {
    return 0;
  }
  entry = zapf_dingbats
      ? agl_find(gfnt_zapf_entries, sizeof gfnt_zapf_entries / sizeof gfnt_zapf_entries[0], part, length)
      : agl_find(gfnt_agl_entries, sizeof gfnt_agl_entries / sizeof gfnt_agl_entries[0], part, length);
  if (entry) {
    const uint32_t * source = zapf_dingbats ? gfnt_zapf_codepoints : gfnt_agl_codepoints;

    for (i = 0; i < entry->count; i++) {
      if (out && at + i < capacity) {
        out[at + i] = source[entry->first + i];
      }
    }
    return entry->count;
  }
  if (length >= 7 && (length - 3) % 4 == 0 && memcmp(part, "uni", 3) == 0) {
    size_t groups = (length - 3) / 4;
    size_t good = 0;

    for (i = 0; i < groups; i++) {
      if (!agl_scalar(agl_hex_run(part + 3 + 4 * i, 4))) {
        return 0;
      }
      good++;
    }
    for (i = 0; i < good; i++) {
      if (out && at + i < capacity) {
        out[at + i] = (uint32_t)agl_hex_run(part + 3 + 4 * i, 4);
      }
    }
    return good;
  }
  if (length >= 5 && length <= 7 && part[0] == 'u') {
    int64_t value = agl_hex_run(part + 1, length - 1);

    if (agl_scalar(value)) {
      if (out && at < capacity) {
        out[at] = (uint32_t)value;
      }
      return 1;
    }
  }
  return 0;
}

GFNT_Result gfnt_glyph_name_to_unicode(const char * name, bool zapf_dingbats,
    uint32_t * out, size_t capacity, size_t * out_count, GFNT_Error * error) {
  size_t length;
  size_t start = 0;
  size_t count = 0;
  size_t i;

  if (!name || !out_count || (!out && capacity)) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "a glyph name and a place for the count are required");
  }
  length = strcspn(name, ".");
  for (i = 0; i <= length; i++) {
    if (i == length || name[i] == '_') {
      count += agl_component(name + start, i - start, zapf_dingbats, out, capacity, count);
      start = i + 1;
    }
  }
  *out_count = count;
  if (count > capacity) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "the glyph name stands for more codepoints than the buffer holds");
  }
  return GFNT_OK;
}
