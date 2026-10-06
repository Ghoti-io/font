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
 * Shape a string and print the glyphs.
 *
 * Usage: font-shape [options] <font> <text>
 *
 *   --script <tag>      the OpenType script tag, as `latn`
 *   --language <tag>    the OpenType language system tag, as `TRK `
 *   --bidi <ltr|rtl|auto>   (with --batch) shape each line as a mixed-direction
 *                       paragraph, glyphs in visual order
 *   --rtl, --ltr        the run reads right to left, or left to right; with
 *                       neither, the way its script is written
 *   --ttb, --btt        the run reads top to bottom, or bottom to top
 *   --features <list>   as HarfBuzz's: `+liga,-kern,aalt=2,liga[3:5]`
 *   --ptem <points>    the size in points, which Apple's `trak` tracks by
 *   --face <n>          which face of a collection
 *   --unicodes <list>   code points, as `U+0041,U+0056`, instead of <text>
 *   --batch             read one text per line from standard input and print each
 *                       run as one line of JSON, in `hb-shape`'s own format
 *   --location <list>   a location in a variable font's design space, as
 *                       `wght=700,wdth=90`, in user coordinates; axes not named
 *                       stay at their default
 *   --rounding <rule>   how a variation delta exactly halfway between two units
 *                       rounds: `half-up` (the default; HarfBuzz, fontTools) or
 *                       `half-away` (FreeType)
 *   --layout            print the face's GSUB and GPOS (scripts, features,
 *                       lookup types) and stop; <text> is not needed
 *
 * Without --batch, one line per glyph: `glyph cluster x_advance y_advance x_offset y_offset`, in
 * font units. That is the shape `tools/oracle/hb_diff.py` compares with
 * `hb-shape`'s output, and the one a person reading a kern can follow.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/shape.h>

#define MAX_FEATURES 64

/** Decode UTF-8, strictly; returns the number of code points, or -1. */
static long utf8_decode(const char * text, uint32_t * out, size_t capacity) {
  const unsigned char * p = (const unsigned char *)text;
  size_t n = 0;

  while (*p) {
    uint32_t u;
    int more;

    if (*p < 0x80) {
      u = *p;
      more = 0;
    }
    else if ((*p & 0xE0) == 0xC0) {
      u = *p & 0x1F;
      more = 1;
    }
    else if ((*p & 0xF0) == 0xE0) {
      u = *p & 0x0F;
      more = 2;
    }
    else if ((*p & 0xF8) == 0xF0) {
      u = *p & 0x07;
      more = 3;
    }
    else {
      return -1;
    }
    p++;
    while (more--) {
      if ((*p & 0xC0) != 0x80) {
        return -1;
      }
      u = (u << 6) | (*p & 0x3F);
      p++;
    }
    if (n >= capacity) {
      return -1;
    }
    out[n++] = u;
  }
  return (long)n;
}

static GFNT_Tag tag_of(const char * text, size_t length) {
  unsigned char b[4] = {' ', ' ', ' ', ' '};
  size_t i;

  for (i = 0; i < length && i < 4; i++) {
    b[i] = (unsigned char)text[i];
  }
  return GFNT_TAG(b[0], b[1], b[2], b[3]);
}

/** Parse one HarfBuzz-style feature: [+-]tag[[start:end]][=value]. */
static int parse_feature(const char * text, size_t length,
    GFNT_ShapeFeature * out) {
  char buffer[64];
  char * p;
  char * close;

  if (length == 0 || length >= sizeof buffer) {
    return -1;
  }
  memcpy(buffer, text, length);
  buffer[length] = 0;
  p = buffer;
  out->value = 1;
  out->start = 0;
  out->end = GFNT_SHAPE_END;
  if (*p == '-') {
    out->value = 0;
    p++;
  }
  else if (*p == '+') {
    p++;
  }
  {
    size_t k = 0;

    while (p[k] && p[k] != '[' && p[k] != '=') {
      k++;
    }
    if (k == 0 || k > 4) {
      return -1;
    }
    out->tag = tag_of(p, k);
    p += k;
  }
  if (*p == '[') {
    char * colon = strchr(p, ':');

    close = strchr(p, ']');
    if (!close || (colon && colon > close)) {
      return -1;
    }
    if (!colon) {
      // `kern[3]` is the one code point 3, and `kern[]` is the whole run.
      if (close != p + 1) {
        out->start = (size_t)strtoul(p + 1, NULL, 10);
        out->end = out->start + 1;
      }
    }
    else {
      out->start = colon == p + 1 ? 0 : (size_t)strtoul(p + 1, NULL, 10);
      out->end = colon + 1 == close ? GFNT_SHAPE_END
                                    : (size_t)strtoul(colon + 1, NULL, 10);
    }
    p = close + 1;
  }
  if (*p == '=') {
    out->value = (uint32_t)strtoul(p + 1, NULL, 10);
  }
  return 0;
}

int main(int argc, char ** argv) {
  const char * path = NULL;
  const char * text = NULL;
  const char * unicodes = NULL;
  const char * features = NULL;
  GFNT_ShapeOptions options;
  GFNT_ShapeFeature list[MAX_FEATURES];
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  GFNT_ShapedRun run;
  uint32_t codepoints[4096];
  long count = 0;
  size_t index = 0;
  size_t i;
  bool layout = false;
  bool batch = false;
  bool bidi = false;
  GFNT_BidiDirection bidi_direction = GFNT_BIDI_LTR;
  const char * location = NULL;
  GFNT_F2Dot14 normalised[64];
  GFNT_Variation variation;
  GFNT_DeltaRounding rounding = GFNT_DELTA_ROUND_HALF_UP;
  bool direction_given = false;
  GFNT_ShapeOptions line_options;
  int arg;

  memset(&options, 0, sizeof options);
  for (arg = 1; arg < argc; arg++) {
    if (strcmp(argv[arg], "--script") == 0 && arg + 1 < argc) {
      options.script = tag_of(argv[arg + 1], strlen(argv[arg + 1]));
      arg++;
    }
    else if (strcmp(argv[arg], "--language") == 0 && arg + 1 < argc) {
      options.language = tag_of(argv[arg + 1], strlen(argv[arg + 1]));
      arg++;
    }
    else if (strcmp(argv[arg], "--rtl") == 0) {
      options.direction = GFNT_DIRECTION_RTL;
      direction_given = true;
    }
    else if (strcmp(argv[arg], "--ltr") == 0) {
      options.direction = GFNT_DIRECTION_LTR;
      direction_given = true;
    }
    else if (strcmp(argv[arg], "--ttb") == 0) {
      options.direction = GFNT_DIRECTION_TTB;
      direction_given = true;
    }
    else if (strcmp(argv[arg], "--btt") == 0) {
      options.direction = GFNT_DIRECTION_BTT;
      direction_given = true;
    }
    else if (strcmp(argv[arg], "--bidi") == 0 && arg + 1 < argc) {
      const char * d = argv[++arg];

      bidi = true;
      bidi_direction = !strcmp(d, "rtl") ? GFNT_BIDI_RTL
          : !strcmp(d, "auto") ? GFNT_BIDI_AUTO : GFNT_BIDI_LTR;
    }
    else if (strcmp(argv[arg], "--ptem") == 0 && arg + 1 < argc) {
      options.point_size = strtof(argv[++arg], NULL);
    }
    else if (strcmp(argv[arg], "--features") == 0 && arg + 1 < argc) {
      features = argv[++arg];
    }
    else if (strcmp(argv[arg], "--unicodes") == 0 && arg + 1 < argc) {
      unicodes = argv[++arg];
    }
    else if (strcmp(argv[arg], "--location") == 0 && arg + 1 < argc) {
      location = argv[++arg];
    }
    else if (strcmp(argv[arg], "--rounding") == 0 && arg + 1 < argc) {
      const char * rule = argv[++arg];

      if (strcmp(rule, "half-up") == 0) {
        rounding = GFNT_DELTA_ROUND_HALF_UP;
      }
      else if (strcmp(rule, "half-away") == 0) {
        rounding = GFNT_DELTA_ROUND_HALF_AWAY;
      }
      else {
        fprintf(stderr, "--rounding is half-up or half-away\n");
        return 2;
      }
    }
    else if (strcmp(argv[arg], "--batch") == 0) {
      batch = true;
    }
    else if (strcmp(argv[arg], "--layout") == 0) {
      layout = true;
    }
    else if (strcmp(argv[arg], "--face") == 0 && arg + 1 < argc) {
      index = (size_t)strtoul(argv[++arg], NULL, 10);
    }
    else if (!path) {
      path = argv[arg];
    }
    else if (!text) {
      text = argv[arg];
    }
    else {
      path = NULL;
      break;
    }
  }
  if (!path || (!text && !unicodes && !layout && !batch)) {
    fprintf(stderr,
        "usage: %s [--script tag] [--language tag] [--rtl|--ttb|--btt] [--features list]"
        " [--face n] [--unicodes U+0041,...] <font> <text>\n", argv[0]);
    return 2;
  }

  if (layout) {
    GFNT_Blob * lb = NULL;
    GFNT_Face * lf = NULL;
    GFNT_Result r;

    gfnt_error_clear(&error);
    if (gfnt_blob_create_file(path, NULL, NULL, &lb, &error) != GFNT_OK
        || gfnt_face_load(lb, index, NULL, NULL, &lf, &error) != GFNT_OK) {
      gfnt_error_dump(&error, stderr);
      gfnt_blob_destroy(lb);
      return 1;
    }
    r = gfnt_face_layout_dump(lf, GFNT_TAG('G', 'S', 'U', 'B'), stdout);
    if (r != GFNT_OK) {
      printf("GSUB: %s\n", gfnt_result_string(r));
    }
    r = gfnt_face_layout_dump(lf, GFNT_TAG('G', 'P', 'O', 'S'), stdout);
    if (r != GFNT_OK) {
      printf("GPOS: %s\n", gfnt_result_string(r));
    }
    gfnt_face_free(lf);
    gfnt_blob_destroy(lb);
    return 0;
  }
  if (unicodes) {
    const char * p = unicodes;

    while (*p) {
      char * end;
      unsigned long u;

      if ((p[0] == 'U' || p[0] == 'u') && p[1] == '+') {
        p += 2;
      }
      u = strtoul(p, &end, 16);
      if (end == p || (size_t)count >= sizeof codepoints / sizeof codepoints[0]) {
        fprintf(stderr, "bad --unicodes\n");
        return 2;
      }
      codepoints[count++] = (uint32_t)u;
      p = end;
      if (*p == ',') {
        p++;
      }
    }
  }
  else if (text) {
    count = utf8_decode(text, codepoints,
        sizeof codepoints / sizeof codepoints[0]);
    if (count < 0) {
      fprintf(stderr, "the text is not valid UTF-8 (or is too long)\n");
      return 2;
    }
  }
  if (features) {
    const char * p = features;
    size_t n = 0;

    while (*p) {
      const char * comma = strchr(p, ',');
      size_t length = comma ? (size_t)(comma - p) : strlen(p);

      if (n >= MAX_FEATURES || parse_feature(p, length, &list[n]) != 0) {
        fprintf(stderr, "bad --features\n");
        return 2;
      }
      n++;
      p += length;
      if (*p == ',') {
        p++;
      }
    }
    options.features = list;
    options.feature_count = n;
  }

  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(path, NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, index, NULL, NULL, &face, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  if (location) {
    GFNT_F16Dot16 user[64];
    GFNT_Axis axis;
    char copy[512];
    char * cursor;
    size_t axes = 0;
    size_t k;

    if (strlen(location) >= sizeof copy
        || gfnt_face_axis_count(face, &axes, &error) != GFNT_OK || axes > 64) {
      fprintf(stderr, "bad --location, or not a variable font\n");
      return 2;
    }
    memcpy(copy, location, strlen(location) + 1);
    // Every axis starts at its default, so a location that names one axis of
    // several leaves the rest where the designer put them.
    for (k = 0; k < axes; ++k) {
      (void)gfnt_face_axis_at(face, k, &axis, &error);
      user[k] = axis.def;
    }
    for (cursor = strtok(copy, ","); cursor; cursor = strtok(NULL, ",")) {
      char * equals = strchr(cursor, '=');
      bool found = false;

      if (!equals || equals - cursor != 4) {
        fprintf(stderr, "bad location item '%s'\n", cursor);
        return 2;
      }
      for (k = 0; k < axes; ++k) {
        (void)gfnt_face_axis_at(face, k, &axis, &error);
        if (axis.tag == GFNT_TAG(cursor[0], cursor[1], cursor[2], cursor[3])) {
          double value = strtod(equals + 1, NULL);

          user[k] = (GFNT_F16Dot16)(value * 65536.0 + (value < 0 ? -0.5 : 0.5));
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
    variation.delta_rounding = rounding;
    options.variation = &variation;
  }
  if (batch) {
    char line[8192];

    while (fgets(line, sizeof line, stdin)) {
      size_t length = strlen(line);
      long n;

      if (length && line[length - 1] == '\n') {
        line[--length] = 0;
      }
      n = utf8_decode(line, codepoints, sizeof codepoints / sizeof codepoints[0]);
      if (n < 0) {
        printf("{\"error\":\"utf8\"}\n");
        continue;
      }
      memset(&run, 0, sizeof run);
      gfnt_error_clear(&error);
      line_options = options;
      if (!direction_given) {
        line_options.direction = gfnt_shape_script_direction(options.script
            ? options.script : gfnt_shape_script_of(codepoints, (size_t)n));
      }
      if (bidi) {
        GFNT_FaceRuns runs;
        const GFNT_Face * one = face;

        memset(&runs, 0, sizeof runs);
        if (gfnt_faces_shape_bidi(&one, 1, codepoints, (size_t)n,
                bidi_direction, &line_options, NULL, &runs, &error)
            != GFNT_OK) {
          printf("{\"error\":\"%s\"}\n", gfnt_result_string(error.result));
          continue;
        }
        printf("[");
        for (size_t r = 0, first = 1; r < runs.count; r++) {
          for (i = 0; i < runs.runs[r].run.count; i++, first = 0) {
            const GFNT_ShapedGlyph * g = &runs.runs[r].run.glyphs[i];

            printf("%s{\"g\":%u,\"cl\":%u,\"dx\":%d,\"dy\":%d,\"ax\":%d,"
                "\"ay\":%d}", first ? "" : ",", g->glyph, g->cluster,
                g->x_offset, g->y_offset, g->x_advance, g->y_advance);
          }
        }
        printf("]\n");
        gfnt_face_runs_free(&runs);
        continue;
      }
      if (gfnt_face_shape(face, codepoints, (size_t)n, &line_options, NULL,
              &run, &error) != GFNT_OK) {
        printf("{\"error\":\"%s\"}\n", gfnt_result_string(error.result));
        continue;
      }
      printf("[");
      for (i = 0; i < run.count; i++) {
        const GFNT_ShapedGlyph * g = &run.glyphs[i];

        printf("%s{\"g\":%u,\"cl\":%u,\"dx\":%d,\"dy\":%d,\"ax\":%d,\"ay\":%d}",
            i ? "," : "", g->glyph, g->cluster, g->x_offset, g->y_offset,
            g->x_advance, g->y_advance);
      }
      printf("]\n");
      gfnt_shaped_run_free(&run);
    }
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 0;
  }
  memset(&run, 0, sizeof run);
  if (!direction_given) {
    options.direction = gfnt_shape_script_direction(options.script
        ? options.script : gfnt_shape_script_of(codepoints, (size_t)count));
  }
  if (gfnt_face_shape(face, codepoints, (size_t)count, &options, NULL, &run,
          &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }
  for (i = 0; i < run.count; i++) {
    const GFNT_ShapedGlyph * g = &run.glyphs[i];

    printf("%u %u %d %d %d %d\n", g->glyph, g->cluster, g->x_advance,
        g->y_advance, g->x_offset, g->y_offset);
  }
  gfnt_shaped_run_free(&run);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
