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
 *                    [--align start|end|left|right|center|justify] [--face N]
 *                    [--mode horizontal|vertical-rl|vertical-lr|sideways-rl|sideways-lr]
 *                    [--orient mixed|mixed-pango|upright|sideways]
 *                    [--justify auto|inter-word|inter-character|kashida|none]
 *                    [--justify-last] [--space-share 0..1] [--line-height-normal|--line-height-multiple N|--line-height-length PX]
 *                    [--vertical-align KIND[:VALUE]] [--split-at BYTE --second-size PX]
 *                    [--second-vertical-align KIND[:VALUE]]
 *                    [--object-at BYTE --object-advance PX --object-ascent PX --object-descent PX]
 *                    [--strut] [--fallback <font>]
 *                    [--flow-left PX --flow-top PX --flow-depth PX]
 *                    [--hyphens manual|manual-pango|none] <font>
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
 *    "rects":[[x,y,w,h]...],
 *    "chars":[[x,y,w,h]...]}   (the rectangle of each code point, in text order)
 *
 * Each line also carries "box":[x,y,w,h], its line box.
 *
 * The carets, hits and rects are there so the differential can check that the
 * hit-testing functions agree with one another and with the glyphs.
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/layout.h>
#include <ghoti.io/font/outline.h>
#include <math.h>
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

typedef struct {
  GFNT_F26Dot6 width, inset, start, depth;
  int shape, side;
} FlowSpec;

static GFNT_Result float_band(void * context, GFNT_F26Dot6 top, GFNT_F26Dot6 height,
    GFNT_Band * out) {
  const FlowSpec * flow = context;
  const int64_t bottom = (int64_t)flow->start + flow->depth;
  const int64_t line_bottom = (int64_t)top + height;
  out->start = 0;
  out->width = flow->width;
  out->next_top = 0;
  if ((int64_t)top < bottom && line_bottom > flow->start) {
    long double intrusion = flow->inset;
    if (flow->shape == 1) {
      const long double radius = flow->inset / 2.0L;
      const long double center = (long double)flow->start + flow->depth / 2.0L;
      const long double lo = top > flow->start ? top : flow->start;
      const long double hi = line_bottom < bottom ? line_bottom : bottom;
      long double dy = 0;
      if (center < lo) dy = lo - center;
      else if (center > hi) dy = center - hi;
      intrusion = radius + sqrtl(radius * radius - dy * dy);
    }
    else if (flow->shape == 2) {
      long double y = top > flow->start ? top : flow->start;
      if (y > bottom) y = bottom;
      const long double fraction = flow->depth ? (y - flow->start) / flow->depth : 0;
      intrusion = flow->inset * (1.0L - 0.4L * fraction);
    }
    if (flow->side == 0) {
      out->start = (GFNT_F26Dot6)ceill(intrusion);
      out->width = flow->width - out->start;
    }
    else {
      out->start = 0;
      out->width = flow->width - (GFNT_F26Dot6)ceill(intrusion);
    }
    out->next_top = bottom > INT32_MAX ? INT32_MAX : (GFNT_F26Dot6)bottom;
  }
  return GFNT_OK;
}

static int answer(const GFNT_Face * face, const GFNT_Face * fallback, const char * text, size_t length,
    GFNT_F26Dot6 width, GFNT_F26Dot6 size, const GFNT_ParagraphOptions * options,
    const GFNT_TextStyle * style_options, size_t split_at, GFNT_F26Dot6 second_size,
    const GFNT_TextStyle * second_style_options, size_t object_at,
    GFNT_InlineObject * inline_object, const FlowSpec * flow) {
  const GFNT_Face * faces[2] = {face, fallback};
  GFNT_TextSpan spans[4];
  GFNT_TextSpan base_span;
  GFNT_TextSpan second_span;
  GFNT_Paragraph * p = NULL;
  GFNT_Layout l;
  GFNT_Error error;
  GFNT_F26Dot6 min = 0, max = 0;

  memset(spans, 0, sizeof spans);
  memset(&l, 0, sizeof l);
  if (split_at > length) split_at = length;
  spans[0].length = split_at ? split_at : length;
  spans[0].style.faces = faces;
  spans[0].style.face_count = fallback ? 2 : 1;
  spans[0].style.size = size;
  spans[0].style.line_height = style_options->line_height;
  spans[0].style.vertical_align = style_options->vertical_align;
  spans[0].style.vertical_align_value = style_options->vertical_align_value;
  base_span = spans[0];
  second_span = base_span;
  size_t span_count = 1;
  if (split_at && split_at < length) {
    spans[1] = base_span;
    spans[1].length = length - split_at;
    spans[1].style.size = second_size ? second_size : size;
    spans[1].style.vertical_align = second_style_options->vertical_align;
    spans[1].style.vertical_align_value = second_style_options->vertical_align_value;
    second_span = spans[1];
    span_count = 2;
  }
  if (inline_object) {
    if (object_at > length || length - object_at < 3) {
      printf("{\"error\":\"object range is outside text\"}\n");
      return 0;
    }
    if (split_at > object_at && split_at < object_at + 3) {
      printf("{\"error\":\"split falls inside inline object\"}\n");
      return 0;
    }
    size_t boundaries[] = {0, length, object_at, object_at + 3, split_at};
    size_t boundary_count = split_at && split_at < length ? 5 : 4;
    for (size_t i = 0; i < boundary_count; i++) {
      for (size_t j = i + 1; j < boundary_count; j++) {
        if (boundaries[j] < boundaries[i]) {
          size_t swap = boundaries[i];
          boundaries[i] = boundaries[j];
          boundaries[j] = swap;
        }
      }
    }
    span_count = 0;
    for (size_t i = 0; i + 1 < boundary_count; i++) {
      size_t begin = boundaries[i];
      size_t end = boundaries[i + 1];
      GFNT_TextSpan part;

      if (begin == end) continue;
      part = split_at && begin >= split_at ? second_span : base_span;
      part.length = end - begin;
      if (begin == object_at && end == object_at + 3) {
        part.style.inline_object = inline_object;
      }
      spans[span_count++] = part;
    }
  }
  gfnt_error_clear(&error);
  GFNT_LineConstraint constraint = {(void *)flow, flow ? float_band : NULL};
  if (gfnt_paragraph_create(text, length, spans, span_count, options, NULL, &p, &error)
          != GFNT_OK
      || (flow ? gfnt_paragraph_layout_flow(p, &constraint, &l, &error)
               : gfnt_paragraph_layout(p, width, &l, &error)) != GFNT_OK
      || gfnt_paragraph_min_content(p, &min) != GFNT_OK
      || gfnt_paragraph_max_content(p, &max) != GFNT_OK) {
    printf("{\"error\":\"%s\"}\n", error.message ? error.message : "refused");
    gfnt_layout_free(&l);
    gfnt_paragraph_free(p);
    return 0;
  }
  printf("{\"min\":%d,\"max\":%d,\"w\":%d,\"h\":%d,\"mode\":%d,"
         "\"truncated\":%s,\"ink\":", min, max, l.width, l.height,
      (int)l.writing_mode, l.truncated ? "true" : "false");
  print_rect(l.ink);
  printf(",\"lines\":[");
  for (size_t i = 0; i < l.line_count; i++) {
    const GFNT_Line * line = &l.lines[i];

    printf("%s{\"s\":%zu,\"e\":%zu,\"x\":%d,\"w\":%d,\"t\":%d,\"hh\":%d,"
           "\"baseline\":%d,\"ascent\":%d,\"descent\":%d,\"justified\":%s,"
           "\"truncated\":%s,\"box\":",
        i ? "," : "", line->start, line->end, line->offset, line->extent, line->box.y,
        line->box.height, line->baseline, line->ascent, line->descent,
        line->justified ? "true" : "false",
        line->truncated ? "true" : "false");
    print_rect(line->box);
    printf(",\"ink\":");
    print_rect(line->ink);
    printf(",\"runs\":[");
    for (size_t k = 0; k < line->run_count; k++) {
      const GFNT_LineRun * run = &line->runs[k];

      printf("%s{\"span\":%zu,\"face_index\":%zu,\"fallback\":%s,\"object\":%s,\"ellipsis\":%s,"
             "\"hyphen\":%s,\"l\":%u,\"o\":%d,\"start\":%zu,\"end\":%zu,"
             "\"pos\":%d,\"advance\":%d,\"g\":[",
          k ? "," : "", run->span, run->face_index, run->fallback ? "true" : "false",
          !run->face && run->glyph_count ? "true" : "false",
          run->ellipsis ? "true" : "false", run->hyphen ? "true" : "false", run->level,
          (int)run->orientation, run->start, run->end, run->pos, run->advance);
      for (size_t g = 0; g < run->glyph_count; g++) {
        const GFNT_PlacedGlyph * pg = &run->glyphs[g];

        printf("%s[%u,%u,%d,%d,%d,%d]", g ? "," : "", pg->glyph, pg->cluster,
            pg->pen_x, pg->advance, pg->x, pg->y);
      }
      // The outline box of each glyph in font units, and the units per em, so that a reader
      // can place the ink: the box is "null" for a glyph with none.
      printf("],\"py\":[");
      for (size_t g = 0; g < run->glyph_count; g++) {
        printf("%s%d", g ? "," : "", run->glyphs[g].pen_y);
      }
      printf("],\"b\":[");
      for (size_t g = 0; g < run->glyph_count; g++) {
        GFNT_Outline * outline = NULL;
        GFNT_Box box;

        printf("%s", g ? "," : "");
        if (run->face && gfnt_face_glyph_outline(run->face, run->glyphs[g].glyph, NULL, NULL, &outline, NULL)
                == GFNT_OK) {
          if (gfnt_outline_bounds(outline, &box) == GFNT_OK && !gfnt_box_is_empty(&box)) {
            printf("[%d,%d,%d,%d]", box.x_min, box.y_min, box.x_max, box.y_max);
          }
          else {
            printf("null");
          }
          gfnt_outline_destroy(outline);
        }
        else {
          printf("null");
        }
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
          || gfnt_layout_hit_test(p, &l, down.horizontal ? down.x + 1 : down.x,
              down.horizontal ? down.y : down.y + 1, &hit) != GFNT_OK
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
  printf("],\"chars\":[");
  for (size_t at = 0, first = 1; at < length;) {
    size_t used = 1;
    GFNT_Rect r1 = {0, 0, 0, 0};
    size_t got = 0;

    while (at + used < length && ((unsigned char)text[at + used] & 0xC0) == 0x80) {
      used++;
    }
    gfnt_layout_range_rects(p, &l, at, at + used, &r1, 1, &got);
    printf("%s", first ? "" : ",");
    first = 0;
    print_rect(got ? r1 : (GFNT_Rect){0, 0, 0, 0});
    at += used;
  }
  printf("]}\n");
  gfnt_layout_free(&l);
  gfnt_paragraph_free(p);
  return 0;
}

int main(int argc, char ** argv) {
  const char * path = NULL;
  const char * fallback_path = NULL;
  GFNT_F26Dot6 size = 16 * 64;
  uint32_t index = 0;
  GFNT_ParagraphOptions options;
  GFNT_TextStyle style_options;
  GFNT_TextStyle second_style_options;
  GFNT_F26Dot6 second_size = 0;
  size_t split_at = 0;
  size_t object_at = 0;
  FlowSpec flow = {0};
  int have_flow = 0;
  GFNT_InlineObject inline_object;
  int have_object = 0;
  GFNT_Blob * blob = NULL;
  GFNT_Blob * fallback_blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Face * fallback = NULL;
  GFNT_Error error;
  char * line = NULL;
  size_t cap = 0;
  long got;

  memset(&options, 0, sizeof options);
  memset(&style_options, 0, sizeof style_options);
  memset(&second_style_options, 0, sizeof second_style_options);
  memset(&inline_object, 0, sizeof inline_object);
  for (int a = 1; a < argc; a++) {
    if (strcmp(argv[a], "--size") == 0 && a + 1 < argc) {
      size = (GFNT_F26Dot6)(atof(argv[++a]) * 64);
    }
    else if (strcmp(argv[a], "--split-at") == 0 && a + 1 < argc) {
      split_at = (size_t)strtoul(argv[++a], NULL, 10);
    }
    else if (strcmp(argv[a], "--second-size") == 0 && a + 1 < argc) {
      second_size = (GFNT_F26Dot6)(atof(argv[++a]) * 64);
    }
    else if (strcmp(argv[a], "--object-at") == 0 && a + 1 < argc) {
      object_at = (size_t)strtoul(argv[++a], NULL, 10);
      have_object = 1;
    }
    else if (strcmp(argv[a], "--object-advance") == 0 && a + 1 < argc) {
      inline_object.advance = (GFNT_F26Dot6)(atof(argv[++a]) * 64.0 + 0.5);
    }
    else if (strcmp(argv[a], "--object-ascent") == 0 && a + 1 < argc) {
      inline_object.ascent = (GFNT_F26Dot6)(atof(argv[++a]) * 64.0 + 0.5);
    }
    else if (strcmp(argv[a], "--object-descent") == 0 && a + 1 < argc) {
      inline_object.descent = (GFNT_F26Dot6)(atof(argv[++a]) * 64.0 + 0.5);
    }
    else if (strcmp(argv[a], "--flow-left") == 0 && a + 1 < argc) {
      flow.inset = (GFNT_F26Dot6)(atof(argv[++a]) * 64.0 + 0.5);
      have_flow = 1;
    }
    else if (strcmp(argv[a], "--flow-depth") == 0 && a + 1 < argc) {
      flow.depth = (GFNT_F26Dot6)(atof(argv[++a]) * 64.0 + 0.5);
      have_flow = 1;
    }
    else if (strcmp(argv[a], "--flow-top") == 0 && a + 1 < argc) {
      flow.start = (GFNT_F26Dot6)(atof(argv[++a]) * 64.0 + 0.5);
      have_flow = 1;
    }
    else if (strcmp(argv[a], "--flow-shape") == 0 && a + 1 < argc) {
      const char * shape = argv[++a];
      flow.shape = strcmp(shape, "circle") == 0 ? 1
          : strcmp(shape, "polygon") == 0 ? 2 : 0;
      have_flow = 1;
    }
    else if (strcmp(argv[a], "--flow-side") == 0 && a + 1 < argc) {
      flow.side = strcmp(argv[++a], "right") == 0 ? 1 : 0;
      have_flow = 1;
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
          : strcmp(v, "justify") == 0 ? GFNT_ALIGN_JUSTIFY
          : strcmp(v, "center") == 0 ? GFNT_ALIGN_CENTER : GFNT_ALIGN_START;
    }
    else if (strcmp(argv[a], "--mode") == 0 && a + 1 < argc) {
      const char * v = argv[++a];

      options.writing_mode = strcmp(v, "vertical-rl") == 0 ? GFNT_WRITING_VERTICAL_RL
          : strcmp(v, "vertical-lr") == 0 ? GFNT_WRITING_VERTICAL_LR
          : strcmp(v, "sideways-rl") == 0 ? GFNT_WRITING_SIDEWAYS_RL
          : strcmp(v, "sideways-lr") == 0 ? GFNT_WRITING_SIDEWAYS_LR
          : GFNT_WRITING_HORIZONTAL_TB;
    }
    else if (strcmp(argv[a], "--justify") == 0 && a + 1 < argc) {
      const char * v = argv[++a];

      options.justify = strcmp(v, "inter-word") == 0 ? GFNT_JUSTIFY_INTER_WORD
          : strcmp(v, "inter-character") == 0 ? GFNT_JUSTIFY_INTER_CHARACTER
          : strcmp(v, "kashida") == 0 ? GFNT_JUSTIFY_KASHIDA
          : strcmp(v, "none") == 0 ? GFNT_JUSTIFY_NONE : GFNT_JUSTIFY_AUTO;
    }
    else if (strcmp(argv[a], "--justify-last") == 0) {
      options.justify_last = true;
    }
    else if (strcmp(argv[a], "--space-share") == 0 && a + 1 < argc) {
      options.space_share = (GFNT_F16Dot16)(atof(argv[++a]) * 65536.0 + 0.5);
    }
    else if (strcmp(argv[a], "--line-height-multiple") == 0 && a + 1 < argc) {
      style_options.line_height.kind = GFNT_LINE_HEIGHT_MULTIPLE;
      style_options.line_height.value = (int32_t)(atof(argv[++a]) * 65536.0 + 0.5);
    }
    else if (strcmp(argv[a], "--line-height-length") == 0 && a + 1 < argc) {
      style_options.line_height.kind = GFNT_LINE_HEIGHT_LENGTH;
      style_options.line_height.value = (int32_t)(atof(argv[++a]) * 64.0 + 0.5);
    }
    else if (strcmp(argv[a], "--vertical-align") == 0 && a + 1 < argc) {
      char * v = argv[++a];
      char * colon = strchr(v, ':');
      double amount = 0;
      if (colon) {
        *colon++ = 0;
        amount = atof(colon);
      }
      style_options.vertical_align = strcmp(v, "sub") == 0 ? GFNT_VERTICAL_ALIGN_SUB
          : strcmp(v, "super") == 0 ? GFNT_VERTICAL_ALIGN_SUPER
          : strcmp(v, "length") == 0 ? GFNT_VERTICAL_ALIGN_LENGTH
          : strcmp(v, "percentage") == 0 ? GFNT_VERTICAL_ALIGN_PERCENTAGE
          : strcmp(v, "text-top") == 0 ? GFNT_VERTICAL_ALIGN_TEXT_TOP
          : strcmp(v, "text-bottom") == 0 ? GFNT_VERTICAL_ALIGN_TEXT_BOTTOM
          : strcmp(v, "middle") == 0 ? GFNT_VERTICAL_ALIGN_MIDDLE
          : strcmp(v, "top") == 0 ? GFNT_VERTICAL_ALIGN_TOP
          : strcmp(v, "bottom") == 0 ? GFNT_VERTICAL_ALIGN_BOTTOM
          : GFNT_VERTICAL_ALIGN_BASELINE;
      style_options.vertical_align_value = style_options.vertical_align == GFNT_VERTICAL_ALIGN_PERCENTAGE
          ? (int32_t)(amount * 65536.0 + (amount < 0 ? -0.5 : 0.5))
          : (int32_t)(amount * 64.0 + (amount < 0 ? -0.5 : 0.5));
    }
    else if (strcmp(argv[a], "--second-vertical-align") == 0 && a + 1 < argc) {
      char * v = argv[++a];
      char * colon = strchr(v, ':');
      double amount = 0;
      if (colon) {
        *colon++ = 0;
        amount = atof(colon);
      }
      second_style_options.vertical_align = strcmp(v, "sub") == 0 ? GFNT_VERTICAL_ALIGN_SUB
          : strcmp(v, "super") == 0 ? GFNT_VERTICAL_ALIGN_SUPER
          : strcmp(v, "length") == 0 ? GFNT_VERTICAL_ALIGN_LENGTH
          : strcmp(v, "percentage") == 0 ? GFNT_VERTICAL_ALIGN_PERCENTAGE
          : strcmp(v, "text-top") == 0 ? GFNT_VERTICAL_ALIGN_TEXT_TOP
          : strcmp(v, "text-bottom") == 0 ? GFNT_VERTICAL_ALIGN_TEXT_BOTTOM
          : strcmp(v, "middle") == 0 ? GFNT_VERTICAL_ALIGN_MIDDLE
          : strcmp(v, "top") == 0 ? GFNT_VERTICAL_ALIGN_TOP
          : strcmp(v, "bottom") == 0 ? GFNT_VERTICAL_ALIGN_BOTTOM
          : GFNT_VERTICAL_ALIGN_BASELINE;
      second_style_options.vertical_align_value = second_style_options.vertical_align
              == GFNT_VERTICAL_ALIGN_PERCENTAGE
          ? (int32_t)(amount * 65536.0 + (amount < 0 ? -0.5 : 0.5))
          : (int32_t)(amount * 64.0 + (amount < 0 ? -0.5 : 0.5));
    }
    else if (strcmp(argv[a], "--strut") == 0) {
      options.strut = true;
    }
    else if (strcmp(argv[a], "--hyphens") == 0 && a + 1 < argc) {
      const char * v = argv[++a];

      options.hyphens = strcmp(v, "none") == 0 ? GFNT_HYPHENS_NONE
          : strcmp(v, "manual-pango") == 0 ? GFNT_HYPHENS_MANUAL_PANGO : GFNT_HYPHENS_MANUAL;
    }
    else if (strcmp(argv[a], "--orient") == 0 && a + 1 < argc) {
      const char * v = argv[++a];

      options.text_orientation = strcmp(v, "upright") == 0 ? GFNT_ORIENT_UPRIGHT
          : strcmp(v, "sideways") == 0 ? GFNT_ORIENT_SIDEWAYS
          : strcmp(v, "mixed-pango") == 0 ? GFNT_ORIENT_MIXED_PANGO : GFNT_ORIENT_MIXED;
    }
    else if (strcmp(argv[a], "--fallback") == 0 && a + 1 < argc) {
      fallback_path = argv[++a];
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
  if (fallback_path
      && (gfnt_blob_create_file(fallback_path, NULL, NULL, &fallback_blob, &error) != GFNT_OK
          || gfnt_face_load(fallback_blob, 0, NULL, NULL, &fallback, &error) != GFNT_OK)) {
    gfnt_error_dump(&error, stderr);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    gfnt_blob_destroy(fallback_blob);
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
    flow.width = width;
    answer(face, fallback, text, n, width, size, &options, &style_options,
        split_at, second_size, &second_style_options, object_at,
        have_object ? &inline_object : NULL, have_flow ? &flow : NULL);
    free(text);
  }
  free(line);
  gfnt_face_free(face);
  gfnt_face_free(fallback);
  gfnt_blob_destroy(blob);
  gfnt_blob_destroy(fallback_blob);
  return 0;
}
