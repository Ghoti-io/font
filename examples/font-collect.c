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
 * Gather fonts into one collection.
 *
 * Usage: font-collect <output> ttc|woff2 <font>...
 *
 * Every face of every input font - a collection contributes each of its faces - goes
 * into one TrueType Collection or one WOFF 2 collection, with tables the faces share
 * stored once.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/write.h>

int main(int argc, char ** argv) {
  GFNT_Blob * blobs[64];
  GFNT_Face * loaded[256];
  GFNT_WriteTable * tables[256];
  GFNT_WriteFace faces[256];
  size_t blob_count = 0;
  size_t face_count = 0;
  GFNT_Blob * out = NULL;
  GFNT_Error error;
  GFNT_Result result = GFNT_OK;
  int status = 1;
  int a;
  size_t i, k;

  if (argc < 4) {
    fprintf(stderr, "usage: %s <output> ttc|woff2 <font>...\n", argv[0]);
    return 2;
  }
  gfnt_error_clear(&error);
  for (a = 3; a < argc && result == GFNT_OK; ++a) {
    size_t count = 0;

    if (blob_count == 64) {
      fprintf(stderr, "too many inputs\n");
      goto done;
    }
    result = gfnt_blob_create_file(argv[a], NULL, NULL, &blobs[blob_count], &error);
    if (result != GFNT_OK) {
      fprintf(stderr, "%s: %s\n", argv[a], gfnt_result_string(result));
      goto done;
    }
    ++blob_count;
    result = gfnt_face_count(blobs[blob_count - 1], NULL, &count, &error);
    for (i = 0; i < count && result == GFNT_OK; ++i) {
      GFNT_Face * face = NULL;
      size_t tables_in;

      if (face_count == 256) {
        fprintf(stderr, "too many faces\n");
        goto done;
      }
      result = gfnt_face_load(blobs[blob_count - 1], i, NULL, NULL, &face, &error);
      if (result != GFNT_OK) {
        break;
      }
      tables_in = gfnt_face_table_count(face);
      tables[face_count] = calloc(tables_in ? tables_in : 1, sizeof **tables);
      for (k = 0; k < tables_in; ++k) {
        GFNT_Tag tag = 0;
        const uint8_t * data = NULL;
        size_t length = 0;

        gfnt_face_table_tag_at(face, k, &tag);
        gfnt_face_table_data(face, tag, &data, &length);
        tables[face_count][k] = (GFNT_WriteTable){tag, data, length};
      }
      faces[face_count] = (GFNT_WriteFace){gfnt_face_flavour(face),
          tables[face_count], tables_in};
      loaded[face_count] = face;
      ++face_count;
    }
  }
  if (result != GFNT_OK) {
    fprintf(stderr, "reading: %s\n", gfnt_result_string(result));
    gfnt_error_dump(&error, stderr);
    goto done;
  }
  result = strcmp(argv[2], "woff2") == 0
      ? gfnt_write_woff2_collection(faces, face_count, NULL, NULL, &out, &error)
      : gfnt_write_collection(faces, face_count, NULL, NULL, &out, &error);
  if (result != GFNT_OK) {
    fprintf(stderr, "writing: %s\n", gfnt_result_string(result));
    gfnt_error_dump(&error, stderr);
    goto done;
  }
  {
    FILE * file = fopen(argv[1], "wb");

    if (!file || fwrite(gfnt_blob_data(out), 1, gfnt_blob_size(out), file)
            != gfnt_blob_size(out)) {
      fprintf(stderr, "cannot write %s\n", argv[1]);
      if (file) {
        fclose(file);
      }
      goto done;
    }
    fclose(file);
  }
  status = 0;

done:
  gfnt_blob_destroy(out);
  for (i = 0; i < face_count; ++i) {
    free(tables[i]);
    gfnt_face_free(loaded[i]);
  }
  for (i = 0; i < blob_count; ++i) {
    gfnt_blob_destroy(blobs[i]);
  }
  return status;
}
