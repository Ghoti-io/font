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
 * Write every table of a face, as this library holds it, to a directory.
 *
 * Usage: font-tables <font> <directory> [face-index]
 *
 * Each table lands in `<directory>/<tag as eight hex digits>`. For a WOFF, a
 * WOFF 2 or a gzipped font the tables are those of the sfnt the face rebuilt, so
 * this is how the rebuild is compared against another implementation's. The face
 * index defaults to 0.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>

int main(int argc, char ** argv) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  GFNT_Result result;
  size_t index = 0;
  size_t i;

  if (argc < 3) {
    fprintf(stderr, "usage: %s <font> <directory> [face-index]\n", argv[0]);
    return 2;
  }
  if (argc > 3) {
    index = (size_t)strtoul(argv[3], NULL, 10);
  }
  gfnt_error_clear(&error);
  result = gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error);
  if (result == GFNT_OK) {
    result = gfnt_face_load(blob, index, NULL, NULL, &face, &error);
  }
  if (result != GFNT_OK) {
    fprintf(stderr, "%s: %s\n", argv[1], gfnt_result_string(result));
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  for (i = 0; i < gfnt_face_table_count(face); ++i) {
    GFNT_Tag tag = 0;
    const uint8_t * data = NULL;
    size_t length = 0;
    char path[4096];
    FILE * out;

    if (gfnt_face_table_tag_at(face, i, &tag) != GFNT_OK
        || gfnt_face_table_data(face, tag, &data, &length) != GFNT_OK) {
      fprintf(stderr, "table %zu is unreadable\n", i);
      continue;
    }
    snprintf(path, sizeof path, "%s/%08" PRIx32, argv[2], (uint32_t)tag);
    out = fopen(path, "wb");
    if (!out || (length && fwrite(data, 1, length, out) != length)) {
      fprintf(stderr, "cannot write %s\n", path);
      if (out) {
        fclose(out);
      }
      gfnt_face_free(face);
      gfnt_blob_destroy(blob);
      return 1;
    }
    fclose(out);
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
