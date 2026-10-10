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
 * Lay a paragraph out and print the result as JSON, for the differential.
 *
 * Usage: font-layout [--size PIXELS] [--bidi ltr|rtl|auto]
 *                    [--align start|end|left|right|center] [--face N] <font>
 *
 * Reads one request per line from standard input: `<width>\t<hex>`, where the
 * width is in 26.6 pixels or `u` for unbounded and the text is the hex of its
 * UTF-8 bytes (so a newline can be asked for). Prints one line of JSON per
 * request:
 *
 *   {"min":N,"max":N,"w":N,"h":N,"ink":[x,y,w,h],
 *    "lines":[{"s":S,"e":E,"x":X,"w":W,"t":TOP,"hh":HEIGHT,"ink":[x,y,w,h],
 *              "runs":[{"l":LEVEL,"g":[[glyph,cluster,pen_x,advance,x,y],...]}]}],
 *    "carets":[[offset,x_down,line_down,x_up,line_up]...],
 *    "hits":[[offset,hit_offset,hit_affinity,x_of_that_caret]...],
 *    "rects":[[x,y,w,h]...]}
 *
 * The carets, hits and rects are there so the differential can check that the
 * hit-testing functions agree with one another and with the glyphs.
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/layout.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void print_rect(GFNT_Rect r) {
  printf("[%d,%d,%d,%d]", r.x, r.y, r.width, r.height);
}

static int answer(const GFNT_Face * face, const char * text, size_t length,
    GFNT_F26Dot6 width, GFNT_F26Dot6 size, const GFNT_ParagraphOptions * options) {
  const GFNT_Face * faces[1] = {face};
  GFNT_TextSpan span;
  GFNT_Paragraph * p = NULL;
  GFNT_Layout l;
  GFNT_Error error;
  GFNT_F26Dot6 min = 0, max = 0;

  memset(&span, 0, sizeof span);
  memset(&l, 0, sizeof l);
  span.length = length;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = size;
  gfnt_error_clear(&error);
  if (gfnt_paragraph_create(text, length, &span, 1, options, NULL, &p, &error)
          != GFNT_OK
      || gfnt_paragraph_layout(p, width, &l, &error) != GFNT_OK
      || gfnt_paragraph_min_content(p, &min) != GFNT_OK
      || gfnt_paragraph_max_content(p, &max) != GFNT_OK) {
    printf("{\"error\":\"%s\"}\n", error.message ? error.message : "refused");
    gfnt_layout_free(&l);
    gfnt_paragraph_free(p);
    return 0;
  }
  printf("{\"min\":%d,\"max\":%d,\"w\":%d,\"h\":%d,\"ink\":", min, max, l.width,
      l.height);
  print_rect(l.ink);
  printf(",\"lines\":[");
  for (size_t i = 0; i < l.line_count; i++) {
    const GFNT_Line * line = &l.lines[i];

    printf("%s{\"s\":%zu,\"e\":%zu,\"x\":%d,\"w\":%d,\"t\":%d,\"hh\":%d,\"ink\":",
        i ? "," : "", line->start, line->end, line->x, line->width, line->top,
        line->height);
    print_rect(line->ink);
    printf(",\"runs\":[");
    for (size_t k = 0; k < line->run_count; k++) {
      const GFNT_LineRun * run = &line->runs[k];

      printf("%s{\"l\":%u,\"g\":[", k ? "," : "", run->level);
      for (size_t g = 0; g < run->glyph_count; g++) {
        const GFNT_PlacedGlyph * pg = &run->glyphs[g];

        printf("%s[%u,%u,%d,%d,%d,%d]", g ? "," : "", pg->glyph, pg->cluster,
            pg->pen_x, pg->advance, pg->x, pg->y);
      }
      printf("]}");
    }
    printf("]}");
  }
  printf("],\"carets\":[");
  {
    size_t emitted = 0;

    for (size_t off = 0; off <= length; off++) {
      GFNT_Caret down, up;

      if (gfnt_layout_caret(p, &l, off, GFNT_AFFINITY_DOWNSTREAM, &down) != GFNT_OK
          || gfnt_layout_caret(p, &l, off, GFNT_AFFINITY_UPSTREAM, &up) != GFNT_OK) {
        continue;
      }
      printf("%s[%zu,%d,%zu,%d,%zu]", emitted++ ? "," : "", off, down.x, down.line,
          up.x, up.line);
    }
  }
  printf("],\"hits\":[");
  {
    size_t emitted = 0;

    for (size_t off = 0; off <= length; off++) {
      GFNT_Caret down;
      GFNT_Hit hit;
      GFNT_Caret back;

      if (gfnt_layout_caret(p, &l, off, GFNT_AFFINITY_DOWNSTREAM, &down) != GFNT_OK
          || gfnt_layout_hit_test(p, &l, down.x, down.top + 1, &hit) != GFNT_OK
          || gfnt_layout_caret(p, &l, hit.offset, hit.affinity, &back) != GFNT_OK) {
        continue;
      }
      printf("%s[%zu,%zu,%d,%d]", emitted++ ? "," : "", off, hit.offset,
          (int)hit.affinity, back.x);
    }
  }
  printf("],\"rects\":[");
  {
    size_t count = 0;
    GFNT_Rect * rects;

    gfnt_layout_range_rects(p, &l, 0, length, NULL, 0, &count);
    rects = calloc(count ? count : 1, sizeof *rects);
    if (rects) {
      gfnt_layout_range_rects(p, &l, 0, length, rects, count, &count);
      for (size_t i = 0; i < count; i++) {
        printf("%s", i ? "," : "");
        print_rect(rects[i]);
      }
    }
    free(rects);
  }
  printf("]}\n");
  gfnt_layout_free(&l);
  gfnt_paragraph_free(p);
  return 0;
}

int main(int argc, char ** argv) {
  const char * path = NULL;
  GFNT_F26Dot6 size = 16 * 64;
  uint32_t index = 0;
  GFNT_ParagraphOptions options;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  char * line = NULL;
  size_t cap = 0;
  long got;

  memset(&options, 0, sizeof options);
  for (int a = 1; a < argc; a++) {
    if (strcmp(argv[a], "--size") == 0 && a + 1 < argc) {
      size = (GFNT_F26Dot6)(atof(argv[++a]) * 64);
    }
    else if (strcmp(argv[a], "--face") == 0 && a + 1 < argc) {
      index = (uint32_t)atoi(argv[++a]);
    }
    else if (strcmp(argv[a], "--bidi") == 0 && a + 1 < argc) {
      const char * v = argv[++a];

      options.direction = strcmp(v, "rtl") == 0 ? GFNT_BIDI_RTL
          : strcmp(v, "auto") == 0 ? GFNT_BIDI_AUTO : GFNT_BIDI_LTR;
    }
    else if (strcmp(argv[a], "--align") == 0 && a + 1 < argc) {
      const char * v = argv[++a];

      options.align = strcmp(v, "end") == 0 ? GFNT_ALIGN_END
          : strcmp(v, "left") == 0 ? GFNT_ALIGN_LEFT
          : strcmp(v, "right") == 0 ? GFNT_ALIGN_RIGHT
          : strcmp(v, "center") == 0 ? GFNT_ALIGN_CENTER : GFNT_ALIGN_START;
    }
    else if (argv[a][0] != '-') {
      path = argv[a];
    }
    else {
      fprintf(stderr, "unknown option %s\n", argv[a]);
      return 2;
    }
  }
  if (!path) {
    fprintf(stderr, "usage: font-layout [options] <font>\n");
    return 2;
  }
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(path, NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, index, NULL, NULL, &face, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  while ((got = getline(&line, &cap, stdin)) >= 0) {
    char * tab = strchr(line, '\t');
    GFNT_F26Dot6 width;
    char * hex;
    size_t n = 0;
    char * text;

    if (!tab) {
      continue;
    }
    *tab = 0;
    width = line[0] == 'u' ? GFNT_LAYOUT_UNBOUNDED : (GFNT_F26Dot6)atoi(line);
    hex = tab + 1;
    text = malloc(strlen(hex) / 2 + 1);
    if (!text) {
      return 1;
    }
    for (; hexval(hex[0]) >= 0 && hexval(hex[1]) >= 0; hex += 2) {
      text[n++] = (char)(hexval(hex[0]) * 16 + hexval(hex[1]));
    }
    answer(face, text, n, width, size, &options);
    free(text);
  }
  free(line);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
