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
 * Rewrite a font as an sfnt, a WOFF, a WOFF 2 or a bitmap font.
 *
 * Usage: font-convert <font> <output> sfnt|woff|woff2 [face-index]
 *        font-convert <font> <output> bdf|pcf|psf1|psf2|hex [strike-index]
 *
 * Reads the font (any container this library reads), takes its tables as the face
 * holds them - so a WOFF or WOFF 2 input is already unwrapped - and writes them with
 * the writer the third argument names. The writers add nothing and drop nothing but
 * what their format cannot carry: no metadata, no private block, and one face only.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/write.h>

/** The bitmap container a name asks for, or -1. */
static int bitmap_format(const char * name) {
  static const char * const names[] = {"bdf", "pcf", "psf1", "psf2", "hex"};
  int i;

  for (i = 0; i < 5; ++i) {
    if (strcmp(name, names[i]) == 0) {
      return i;
    }
  }
  return -1;
}

int main(int argc, char ** argv) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Blob * out = NULL;
  GFNT_WriteTable * tables = NULL;
  GFNT_Error error;
  GFNT_Result result;
  size_t index = 0;
  size_t count;
  size_t i;
  int status = 1;

  if (argc < 4) {
    fprintf(stderr, "usage: %s <font> <output> sfnt|woff|woff2 [face-index]\n",
        argv[0]);
    return 2;
  }
  if (argc > 4) {
    index = (size_t)strtoul(argv[4], NULL, 10);
  }
  gfnt_error_clear(&error);
  result = gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error);
  if (result == GFNT_OK) {
    // For a bitmap format the number names a strike, and the face is the first.
    result = gfnt_face_load(blob, bitmap_format(argv[3]) >= 0 ? 0 : index, NULL,
        NULL, &face, &error);
  }
  if (result != GFNT_OK) {
    fprintf(stderr, "%s: %s\n", argv[1], gfnt_result_string(result));
    gfnt_error_dump(&error, stderr);
    goto done;
  }
  count = gfnt_face_table_count(face);
  tables = calloc(count ? count : 1, sizeof *tables);
  if (!tables) {
    goto done;
  }
  for (i = 0; i < count; ++i) {
    GFNT_Tag tag = 0;
    const uint8_t * data = NULL;
    size_t length = 0;

    if (gfnt_face_table_tag_at(face, i, &tag) != GFNT_OK
        || gfnt_face_table_data(face, tag, &data, &length) != GFNT_OK) {
      fprintf(stderr, "table %zu is unreadable\n", i);
      goto done;
    }
    tables[i] = (GFNT_WriteTable){tag, data, length};
  }
  if (strcmp(argv[3], "sfnt") == 0) {
    result = gfnt_write_sfnt(gfnt_face_flavour(face), tables, count, NULL, NULL,
        &out, &error);
  }
  else if (strcmp(argv[3], "woff") == 0) {
    result = gfnt_write_woff(gfnt_face_flavour(face), tables, count, NULL, NULL,
        &out, &error);
  }
  else if (bitmap_format(argv[3]) >= 0) {
    result = gfnt_write_bitmap_font(face, index, (GFNT_BitmapFormat)bitmap_format(argv[3]),
        NULL, NULL, &out, &error);
  }
  else if (strcmp(argv[3], "woff2") == 0) {
    result = gfnt_write_woff2(gfnt_face_flavour(face), tables, count, NULL, NULL,
        &out, &error);
  }
  else {
    fprintf(stderr, "unknown output format %s\n", argv[3]);
    goto done;
  }
  if (result != GFNT_OK) {
    fprintf(stderr, "writing: %s\n", gfnt_result_string(result));
    gfnt_error_dump(&error, stderr);
    goto done;
  }
  {
    FILE * file = fopen(argv[2], "wb");

    if (!file || fwrite(gfnt_blob_data(out), 1, gfnt_blob_size(out), file)
            != gfnt_blob_size(out)) {
      fprintf(stderr, "cannot write %s\n", argv[2]);
      if (file) {
        fclose(file);
      }
      goto done;
    }
    fclose(file);
  }
  status = 0;

done:
  free(tables);
  gfnt_blob_destroy(out);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return status;
}
