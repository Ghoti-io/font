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
 * The documents of a font's `SVG ` table, for `tools/oracle/svg_diff.py`.
 *
 * Usage: font-svg <out-dir> <font>
 *
 * Writes each document, inflated if it is compressed, to `<out-dir>/<index>.svg`, and prints one
 * JSON object:
 *
 *   {"documents":[{"first":1,"last":4,"compressed":true,"length":412,"text":398}],
 *    "glyph_document":[-1,0,0,0,0,-1]}
 *
 * `glyph_document` has one entry per glyph of the font: the index of the document
 * ::gfnt_face_glyph_svg() finds for it, or -1.
 */

#include <ghoti.io/font/font.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char ** argv) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  size_t count = 0;
  size_t glyphs = 0;
  GFNT_SvgDocument * docs;
  int status = 0;

  if (argc != 3) {
    fprintf(stderr, "usage: font-svg <out-dir> <font>\n");
    return 2;
  }
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(argv[2], NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK
      || gfnt_face_num_glyphs(face, &glyphs, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  if (gfnt_face_svg_document_count(face, &count, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }
  docs = calloc(count ? count : 1, sizeof *docs);
  if (!docs) {
    return 1;
  }
  printf("{\"documents\":[");
  for (size_t i = 0; i < count; i++) {
    GFNT_Blob * text = NULL;
    char path[4096];
    FILE * out;

    if (gfnt_face_svg_document_at(face, i, &docs[i], &error) != GFNT_OK
        || gfnt_svg_document_inflate(face, &docs[i], NULL, &text, &error) != GFNT_OK) {
      gfnt_error_dump(&error, stderr);
      status = 1;
      break;
    }
    snprintf(path, sizeof path, "%s/%zu.svg", argv[1], i);
    out = fopen(path, "wb");
    if (!out || (gfnt_blob_size(text) && fwrite(gfnt_blob_data(text), 1, gfnt_blob_size(text), out)
                     != gfnt_blob_size(text))) {
      fprintf(stderr, "cannot write %s\n", path);
      status = 1;
    }
    if (out) {
      fclose(out);
    }
    printf("%s{\"first\":%" PRIu32 ",\"last\":%" PRIu32 ",\"compressed\":%s,\"length\":%zu,"
           "\"text\":%zu}", i ? "," : "", docs[i].first_glyph, docs[i].last_glyph,
        docs[i].compressed ? "true" : "false", docs[i].length, gfnt_blob_size(text));
    gfnt_blob_destroy(text);
  }
  printf("],\"glyph_document\":[");
  for (size_t g = 0; g < glyphs && status == 0; g++) {
    GFNT_SvgDocument found;
    bool has = false;
    long index = -1;

    if (gfnt_face_glyph_svg(face, (uint32_t)g, &found, &has, &error) != GFNT_OK) {
      gfnt_error_dump(&error, stderr);
      status = 1;
      break;
    }
    for (size_t i = 0; has && i < count; i++) {
      if (docs[i].data == found.data && docs[i].first_glyph == found.first_glyph) {
        index = (long)i;
        break;
      }
    }
    printf("%s%ld", g ? "," : "", index);
  }
  printf("]}\n");
  free(docs);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return status;
}
