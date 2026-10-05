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
 * Print every glyph's advance and side bearing, the line metrics, and the
 * control values, at a location in a variable font's design space.
 *
 * Usage: font-metrics <font> [face-index] [stride] [first] [location]
 *
 * `location` is `tag=value,tag=value` in **user** coordinates, as
 * `font-outline` takes it. Without one the default instance is measured.
 *
 * **This is the driver `tools/oracle/metrics_var_diff.py` compares against
 * fontTools and FreeType.** A metric this library will not answer is printed as
 * `refused` and the result's name, never skipped: a differential that read a
 * missing line as agreement would be comparing nothing, and the refusals are
 * statements the references make too (fontTools has no left-bearing mapping to
 * apply and says so).
 *
 * Each line carries its glyph index, so that a dropped glyph shows up as a
 * missing key and not as every later one disagreeing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>

/** One metric's line: its value, or the name of what it was refused with. */
static void print_value(const char * label, unsigned long glyph, int32_t value,
    GFNT_Result result) {
  if (result == GFNT_OK) {
    printf("glyph %lu %s %ld\n", glyph, label, (long)value);
  }
  else {
    printf("glyph %lu %s refused %s\n", glyph, label, gfnt_result_string(result));
  }
}

static void print_line(const GFNT_Face * face, const char * label,
    GFNT_LineMetricsPolicy policy, const GFNT_Variation * variation) {
  GFNT_LineMetrics metrics;
  GFNT_Result result = gfnt_face_line_metrics(face, policy, variation, &metrics,
      NULL);

  if (result != GFNT_OK) {
    printf("line %s refused %s\n", label, gfnt_result_string(result));
  }
  else if (policy == GFNT_LINE_METRICS_WIN) {
    // The window metrics state no gap, so none is printed: the references have
    // nothing to compare it with.
    printf("line %s %ld %ld\n", label, (long)metrics.ascent,
        (long)metrics.descent);
  }
  else {
    printf("line %s %ld %ld %ld\n", label, (long)metrics.ascent,
        (long)metrics.descent, (long)metrics.line_gap);
  }
}

int main(int argc, char ** argv) {
  const char * path;
  size_t index = 0;
  size_t stride = 1;
  size_t first = 0;
  const char * location = NULL;
  GFNT_F16Dot16 user[64];
  GFNT_F2Dot14 normalised[64];
  GFNT_Variation variation = { NULL, 0, GFNT_DELTA_ROUND_HALF_UP };
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  size_t glyphs = 0;
  size_t axes = 0;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <font> [face-index] [stride] [first] [location]\n",
        argv[0]);
    return 2;
  }
  path = argv[1];
  if (argc > 2) {
    index = (size_t)strtoul(argv[2], NULL, 10);
  }
  if (argc > 3) {
    stride = (size_t)strtoul(argv[3], NULL, 10);
    if (stride == 0) {
      stride = 1;
    }
  }
  if (argc > 4) {
    first = (size_t)strtoul(argv[4], NULL, 10);
  }
  if (argc > 5) {
    location = argv[5];
  }

  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(path, NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, index, NULL, NULL, &face, &error) != GFNT_OK
      || gfnt_face_num_glyphs(face, &glyphs, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }
  if (location) {
    GFNT_Axis axis;
    char copy[512];
    char * cursor;

    if (strlen(location) >= sizeof copy) {
      fprintf(stderr, "location too long\n");
      return 2;
    }
    memcpy(copy, location, strlen(location) + 1);
    if (gfnt_face_axis_count(face, &axes, &error) != GFNT_OK || axes > 64) {
      gfnt_error_dump(&error, stderr);
      return 1;
    }
    // Every axis starts at its default, so a location that names one axis of
    // several leaves the rest where the designer put them.
    for (size_t i = 0; i < axes; ++i) {
      if (gfnt_face_axis_at(face, i, &axis, &error) != GFNT_OK) {
        gfnt_error_dump(&error, stderr);
        return 1;
      }
      user[i] = axis.def;
    }
    for (cursor = strtok(copy, ","); cursor; cursor = strtok(NULL, ",")) {
      char * equals = strchr(cursor, '=');
      bool found = false;

      if (!equals || equals - cursor != 4) {
        fprintf(stderr, "bad location item '%s'\n", cursor);
        return 2;
      }
      for (size_t i = 0; i < axes; ++i) {
        (void)gfnt_face_axis_at(face, i, &axis, &error);
        if (axis.tag == GFNT_TAG(cursor[0], cursor[1], cursor[2], cursor[3])) {
          double value = strtod(equals + 1, NULL);

          // 16.16, rounded to nearest, as `font-outline` does.
          user[i] = (GFNT_F16Dot16)(value * 65536.0 + (value < 0 ? -0.5 : 0.5));
          found = true;
        }
      }
      if (!found) {
        fprintf(stderr, "this face has no axis '%.4s'\n", cursor);
        return 2;
      }
    }
    if (gfnt_face_normalize(face, user, axes, normalised, 64, &error)
        != GFNT_OK) {
      gfnt_error_dump(&error, stderr);
      return 1;
    }
    variation.coords = normalised;
    variation.count = axes;
    printf("variation: %zu axes, normalised", axes);
    for (size_t i = 0; i < axes; ++i) {
      printf(" %d", (int)normalised[i]);
    }
    printf("\n");
  }

  for (size_t glyph = first; glyph < glyphs; glyph += stride) {
    int32_t value = 0;
    GFNT_Result result;

    result = gfnt_face_glyph_advance(face, (uint32_t)glyph,
        location ? &variation : NULL, &value, NULL);
    print_value("advance", glyph, value, result);
    value = 0;
    result = gfnt_face_glyph_side_bearing(face, (uint32_t)glyph,
        location ? &variation : NULL, &value, NULL);
    print_value("bearing", glyph, value, result);
  }
  print_line(face, "hhea", GFNT_LINE_METRICS_HHEA, location ? &variation : NULL);
  print_line(face, "win", GFNT_LINE_METRICS_WIN, location ? &variation : NULL);
  print_line(face, "typo", GFNT_LINE_METRICS_TYPO, location ? &variation : NULL);
  {
    // The control values, which `cvar` moves. A face with none prints nothing.
    size_t count = 0;

    if (gfnt_face_cvt_count(face, &count, NULL) == GFNT_OK && count > 0) {
      int32_t * values = malloc(count * sizeof *values);
      GFNT_Result result;

      if (!values) {
        fprintf(stderr, "no memory\n");
        return 1;
      }
      result = gfnt_face_cvt_values(face, location ? &variation : NULL, values,
          count, NULL);
      if (result == GFNT_OK) {
        printf("cvt %zu", count);
        for (size_t i = 0; i < count; ++i) {
          printf(" %ld", (long)values[i]);
        }
        printf("\n");
      }
      else {
        printf("cvt refused %s\n", gfnt_result_string(result));
      }
      free(values);
    }
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
