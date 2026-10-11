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
 * Every picture a color font's strikes hold, for `tools/oracle/strike_image_diff.py`.
 *
 * Usage: font-strike-images [--decode] [--glyphs A-B] <out-dir> <font>
 *
 * For each color strike (`GFNT_GLYPH_BITMAP_PNG`) and each glyph it carries, writes
 * the font's own bytes to `<out-dir>/<strike>-<glyph>.img` and, with `--decode`,
 * the decoded pixels to `<out-dir>/<strike>-<glyph>.rgba` (8-bit, straight alpha,
 * rows top first with no padding). Prints one JSON line per glyph:
 *
 *   {"strike":0,"glyph":5,"format":"png","length":799,"w":136,"h":128,
 *    "bx":0,"by":101,"oy":-27,"adv":136}
 *
 * or `{"strike":0,"glyph":5,"absent":true}` for a glyph the strike does not carry,
 * or `{"strike":0,"glyph":5,"error":"..."}` for any other refusal. The first line
 * is `{"strikes":[{"index":0,"ppem":109,...}]}`.
 */

#include <ghoti.io/font/font.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char * format_name(GFNT_ImageFormat format) {
  switch (format) {
    case GFNT_IMAGE_PNG:
      return "png";
    case GFNT_IMAGE_JPEG:
      return "jpeg";
    default:
      return "tiff";
  }
}

static int write_file(const char * dir, size_t strike, uint32_t glyph,
    const char * extension, const void * data, size_t length) {
  char path[4096];
  FILE * out;

  snprintf(path, sizeof path, "%s/%zu-%" PRIu32 ".%s", dir, strike, glyph,
      extension);
  out = fopen(path, "wb");
  if (!out) {
    fprintf(stderr, "cannot write %s\n", path);
    return 1;
  }
  if (length && fwrite(data, 1, length, out) != length) {
    fclose(out);
    return 1;
  }
  return fclose(out) == 0 ? 0 : 1;
}

int main(int argc, char ** argv) {
  const char * dir = NULL;
  const char * path = NULL;
  bool decode = false;
  uint32_t first = 0;
  uint32_t last = UINT32_MAX;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  size_t strikes = 0;
  size_t glyphs = 0;
  int status = 0;

  for (int a = 1; a < argc; a++) {
    if (strcmp(argv[a], "--decode") == 0) {
      decode = true;
    }
    else if (strcmp(argv[a], "--glyphs") == 0 && a + 1 < argc) {
      first = (uint32_t)strtoul(argv[++a], (char **)&path, 10);
      last = (path && *path == '-') ? (uint32_t)strtoul(path + 1, NULL, 10)
                                    : first;
      path = NULL;
    }
    else if (argv[a][0] == '-') {
      fprintf(stderr, "unknown option %s\n", argv[a]);
      return 2;
    }
    else if (!dir) {
      dir = argv[a];
    }
    else {
      path = argv[a];
    }
  }
  if (!dir || !path) {
    fprintf(stderr,
        "usage: font-strike-images [--decode] [--glyphs A-B] <out-dir> <font>\n");
    return 2;
  }
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(path, NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  if (gfnt_face_strike_count(face, &strikes, &error) != GFNT_OK
      || gfnt_face_num_glyphs(face, &glyphs, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }

  printf("{\"strikes\":[");
  for (size_t s = 0; s < strikes; s++) {
    GFNT_Strike strike;

    if (gfnt_face_strike_at(face, s, &strike, &error) != GFNT_OK) {
      gfnt_error_dump(&error, stderr);
      status = 1;
      break;
    }
    printf("%s{\"index\":%zu,\"ppem_x\":%" PRIu32 ",\"ppem_y\":%" PRIu32
           ",\"depth\":%u,\"color\":%s,\"ascent\":%" PRId32 ",\"descent\":%"
           PRId32 "}",
        s ? "," : "", s, strike.ppem_x, strike.ppem_y,
        (unsigned)strike.bit_depth,
        strike.kind == GFNT_GLYPH_BITMAP_PNG ? "true" : "false", strike.ascent,
        strike.descent);
  }
  printf("]}\n");

  for (size_t s = 0; s < strikes && status == 0; s++) {
    GFNT_Strike strike;

    if (gfnt_face_strike_at(face, s, &strike, NULL) != GFNT_OK
        || strike.kind != GFNT_GLYPH_BITMAP_PNG) {
      continue;
    }
    for (size_t g = first; g < glyphs && g <= last && status == 0; g++) {
      GFNT_GlyphImage image;
      GFNT_Result result;

      gfnt_error_clear(&error);
      result = gfnt_face_glyph_image(face, (uint32_t)g, s, &image, &error);
      if (result == GFNT_ERR_UNSUPPORTED && error.message
          && strstr(error.message, "does not carry")) {
        printf("{\"strike\":%zu,\"glyph\":%zu,\"absent\":true}\n", s, g);
        continue;
      }
      if (result != GFNT_OK) {
        printf("{\"strike\":%zu,\"glyph\":%zu,\"error\":\"%s\"}\n", s, g,
            error.message ? error.message : "?");
        continue;
      }
      printf("{\"strike\":%zu,\"glyph\":%zu,\"format\":\"%s\",\"length\":%zu,"
             "\"w\":%" PRIu32 ",\"h\":%" PRIu32 ",\"bx\":%" PRId32
             ",\"by\":%" PRId32 ",\"oy\":%" PRId32 ",\"adv\":%" PRId32,
          s, g, format_name(image.format), image.length, image.width,
          image.height, image.bearing_x, image.bearing_y, image.origin_y,
          image.advance);
      if (write_file(dir, s, (uint32_t)g, "img", image.data, image.length)) {
        status = 1;
      }
      if (decode) {
        GFNT_ColorBitmap bitmap;

        gfnt_error_clear(&error);
        result = gfnt_face_glyph_color_bitmap(face, (uint32_t)g, s, &bitmap,
            &error);
        if (result == GFNT_OK) {
          printf(",\"dw\":%" PRIu32 ",\"dh\":%" PRIu32, bitmap.width,
              bitmap.height);
          if (write_file(dir, s, (uint32_t)g, "rgba", bitmap.rgba,
                  bitmap.height * bitmap.stride)) {
            status = 1;
          }
          gfnt_color_bitmap_free(&bitmap);
        }
        else {
          printf(",\"decode_error\":\"%s\"",
              error.message ? error.message : "?");
        }
      }
      printf("}\n");
    }
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return status;
}
