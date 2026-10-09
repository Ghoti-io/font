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
 * Write a font holding only some of another's glyphs.
 *
 * Usage: font-subset [--retain-gids] [--no-closure] [--drop-hinting]
 *                    [--drop-layout] [--features liga,kern] [--woff]
 *                    [--unicodes U+0041,U+0042] [--glyphs 3,4] <in> <out>
 *
 * The driver `tools/oracle/subset_diff.py` runs, and a way to see what the
 * subsetter does to a font on the machine. The characters are given as code
 * points, comma separated, with or without a `U+`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/write.h>

/** A copy of a string, because strdup is not C17. */
static char * copy_of(const char * text) {
  size_t n = strlen(text) + 1;
  char * out = malloc(n);

  if (out) {
    memcpy(out, text, n);
  }
  return out;
}

static int fail(const char * what, GFNT_Result result, const GFNT_Error * error) {
  fprintf(stderr, "%s: %s\n", what, gfnt_result_string(result));
  if (error && error->result != GFNT_OK) {
    fputs("  ", stderr);
    gfnt_error_dump(error, stderr);
  }
  return 1;
}

/** Parse "a,b,c" as numbers in base @p base into a new array. */
static uint32_t * parse_list(const char * text, int base, size_t * out_count) {
  size_t capacity = 16;
  size_t count = 0;
  uint32_t * values = malloc(capacity * sizeof *values);
  char * copy = copy_of(text);
  char * token = copy ? strtok(copy, ",") : NULL;

  while (values && token) {
    if (base == 16 && (token[0] == 'U' || token[0] == 'u') && token[1] == '+') {
      token += 2;
    }
    if (count == capacity) {
      uint32_t * grown = realloc(values, capacity * 2 * sizeof *values);

      if (!grown) {
        break;
      }
      values = grown;
      capacity *= 2;
    }
    values[count++] = (uint32_t)strtoul(token, NULL, base);
    token = strtok(NULL, ",");
  }
  free(copy);
  *out_count = count;
  return values;
}

/** Feature tags from "liga,kern": each padded with spaces to four. */
static GFNT_Tag * parse_tags(const char * text, size_t * out_count) {
  size_t count = 0;
  GFNT_Tag * tags = calloc(strlen(text) / 2 + 1, sizeof *tags);
  char * copy = copy_of(text);
  char * token = copy ? strtok(copy, ",") : NULL;

  while (tags && token) {
    char four[4] = {' ', ' ', ' ', ' '};
    size_t n = strlen(token) < 4 ? strlen(token) : 4;

    memcpy(four, token, n);
    tags[count++] = GFNT_TAG(four[0], four[1], four[2], four[3]);
    token = strtok(NULL, ",");
  }
  free(copy);
  *out_count = count;
  return tags;
}

int main(int argc, char ** argv) {
  GFNT_SubsetOptions options;
  GFNT_Blob * in = NULL;
  GFNT_Blob * out = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  GFNT_Result result;
  const char * paths[2] = {NULL, NULL};
  int npaths = 0;
  bool woff = false;
  FILE * file;

  gfnt_error_clear(&error);
  gfnt_subset_options_init(&options);
  for (int i = 1; i < argc; ++i) {
    const char * a = argv[i];

    if (!strcmp(a, "--retain-gids")) {
      options.retain_gids = true;
    }
    else if (!strcmp(a, "--no-closure")) {
      options.no_gsub_closure = true;
    }
    else if (!strcmp(a, "--drop-hinting")) {
      options.drop_hinting = true;
    }
    else if (!strcmp(a, "--drop-layout")) {
      options.drop_layout = true;
    }
    else if (!strcmp(a, "--woff")) {
      woff = true;
    }
    else if (!strcmp(a, "--unicodes") && i + 1 < argc) {
      options.codepoints = parse_list(argv[++i], 16, &options.codepoint_count);
    }
    else if (!strcmp(a, "--glyphs") && i + 1 < argc) {
      options.glyphs = parse_list(argv[++i], 10, &options.glyph_count);
    }
    else if (!strcmp(a, "--features") && i + 1 < argc) {
      options.features = parse_tags(argv[++i], &options.feature_count);
    }
    else if (a[0] != '-' && npaths < 2) {
      paths[npaths++] = a;
    }
    else {
      fprintf(stderr, "usage: %s [--retain-gids] [--no-closure] [--drop-hinting] "
          "[--drop-layout] [--features tags] [--woff] [--unicodes list] "
          "[--glyphs list] <in> <out>\n", argv[0]);
      return 2;
    }
  }
  if (npaths != 2) {
    fprintf(stderr, "%s: needs an input and an output\n", argv[0]);
    return 2;
  }
  result = gfnt_blob_create_file(paths[0], NULL, NULL, &in, &error);
  if (result != GFNT_OK) {
    return fail(paths[0], result, &error);
  }
  result = gfnt_face_load(in, 0, NULL, NULL, &face, &error);
  if (result != GFNT_OK) {
    return fail(paths[0], result, &error);
  }
  result = gfnt_subset(face, &options, NULL, NULL, &out, &error);
  if (result != GFNT_OK) {
    return fail("subset", result, &error);
  }
  if (woff) {
    // A WOFF is the same tables compressed one by one: re-read the sfnt just made
    // and write its tables again.
    GFNT_Face * made = NULL;
    GFNT_WriteTable tables[64];
    size_t n;
    GFNT_Blob * packed = NULL;

    result = gfnt_face_load(out, 0, NULL, NULL, &made, &error);
    if (result != GFNT_OK) {
      return fail("reload", result, &error);
    }
    n = gfnt_face_table_count(made);
    if (n > 64) {
      return fail("too many tables", GFNT_ERR_LIMIT, NULL);
    }
    for (size_t t = 0; t < n; ++t) {
      GFNT_Tag tag = 0;
      size_t offset = 0;
      size_t length = 0;

      gfnt_face_table_tag_at(made, t, &tag);
      gfnt_face_table_range(made, tag, &offset, &length);
      tables[t].tag = tag;
      tables[t].data = gfnt_blob_data(out) + offset;
      tables[t].length = length;
    }
    result = gfnt_write_woff(gfnt_face_flavour(made), tables, n, NULL, NULL,
        &packed, &error);
    if (result != GFNT_OK) {
      return fail("woff", result, &error);
    }
    gfnt_face_free(made);
    gfnt_blob_destroy(out);
    out = packed;
  }
  file = fopen(paths[1], "wb");
  if (!file || fwrite(gfnt_blob_data(out), 1, gfnt_blob_size(out), file)
          != gfnt_blob_size(out) || fclose(file) != 0) {
    fprintf(stderr, "%s: could not write\n", paths[1]);
    return 1;
  }
  printf("%zu bytes\n", gfnt_blob_size(out));
  gfnt_blob_destroy(out);
  gfnt_face_free(face);
  gfnt_blob_destroy(in);
  return 0;
}
