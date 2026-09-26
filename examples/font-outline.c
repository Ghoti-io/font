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
 * Print every glyph's outline, its points and its path.
 *
 * Usage: font-outline <font> [face-index] [stride] [first]
 *
 * **This is the driver `tools/oracle/glyf_diff.py` compares against fontTools.**
 * It prints two different things per glyph and both are deliberate: the points
 * as the font stores them, which compare against `glyf`'s own coordinates and
 * flags, and the path `gfnt_outline_decompose()` makes of them, which compares
 * against a reference pen. A `glyf` reader can have the points exactly right
 * and the implicit-midpoint rule wrong, and only the second comparison sees it.
 *
 * `stride` and `first` exist because a full run over a large font is tens of
 * thousands of glyphs, and the differential wants both a sample of every font
 * and an exhaustive pass over a few. A stride of 1 is every glyph.
 *
 * Each line carries its glyph index, so that the reader on the other side does
 * not have to track position - a dropped glyph then shows up as a missing key
 * rather than as every later glyph disagreeing.
 */

#include <stdio.h>
#include <stdlib.h>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/outline.h>

/** What the path walk needs to prefix each line with its glyph. */
typedef struct Context {
  unsigned long glyph;
} Context;

static GFNT_Result print_segment(const Context * context, const char * verb,
    const GFNT_Point * points, size_t count) {
  if (printf("glyph %lu path: %s", context->glyph, verb) < 0) {
    return GFNT_ERR_IO;
  }
  for (size_t i = 0; i < count; ++i) {
    if (printf(" %d %d", points[i].x, points[i].y) < 0) {
      return GFNT_ERR_IO;
    }
  }
  return printf("\n") < 0 ? GFNT_ERR_IO : GFNT_OK;
}

static GFNT_Result on_move(void * user, GFNT_Point to) {
  return print_segment(user, "move", &to, 1);
}

static GFNT_Result on_line(void * user, GFNT_Point to) {
  return print_segment(user, "line", &to, 1);
}

static GFNT_Result on_quad(void * user, GFNT_Point control, GFNT_Point to) {
  GFNT_Point pair[2];

  pair[0] = control;
  pair[1] = to;
  return print_segment(user, "quad", pair, 2);
}

static GFNT_Result on_cubic(void * user, GFNT_Point c1, GFNT_Point c2,
    GFNT_Point to) {
  GFNT_Point triple[3];

  triple[0] = c1;
  triple[1] = c2;
  triple[2] = to;
  return print_segment(user, "cubic", triple, 3);
}

static GFNT_Result on_close(void * user) {
  return print_segment(user, "close", NULL, 0);
}

int main(int argc, char ** argv) {
  const GFNT_OutlineSink sink = {
    on_move, on_line, on_quad, on_cubic, on_close,
  };
  const char * path;
  size_t index = 0;
  size_t stride = 1;
  size_t first = 0;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  GFNT_Result result;
  size_t glyphs = 0;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <font> [face-index] [stride] [first]\n",
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

  gfnt_error_clear(&error);
  result = gfnt_blob_create_file(path, NULL, NULL, &blob, &error);
  if (result != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  result = gfnt_face_load(blob, index, NULL, NULL, &face, &error);
  if (result != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  if (gfnt_face_num_glyphs(face, &glyphs, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }
  printf("outlines: %zu glyph(s), stride %zu from %zu\n", glyphs, stride,
      first);

  for (size_t glyph = first; glyph < glyphs; glyph += stride) {
    GFNT_Outline * outline = NULL;
    Context context;
    bool composite = false;
    GFNT_Box stated;
    GFNT_Box drawn;
    GFNT_Box control;

    context.glyph = (unsigned long)glyph;
    gfnt_error_clear(&error);
    if (gfnt_face_glyph_is_composite(face, (uint32_t)glyph, &composite, &error)
        != GFNT_OK) {
      // Said rather than skipped: a differential reads a missing glyph as
      // agreement, and "this library refused it" is the fact to compare.
      printf("glyph %zu: refused %s\n", glyph,
          gfnt_result_string(error.result));
      continue;
    }
    if (gfnt_face_glyph_stated_box(face, (uint32_t)glyph, &stated, &error)
        != GFNT_OK) {
      printf("glyph %zu: refused %s\n", glyph,
          gfnt_result_string(error.result));
      continue;
    }
    result = gfnt_face_glyph_outline(face, (uint32_t)glyph, NULL, NULL,
        &outline, &error);
    if (result != GFNT_OK) {
      printf("glyph %zu: refused %s\n", glyph, gfnt_result_string(result));
      continue;
    }
    printf("glyph %zu: composite %d, points %zu, contours %zu\n", glyph,
        composite ? 1 : 0, gfnt_outline_point_count(outline),
        gfnt_outline_contour_count(outline));
    if (gfnt_box_is_empty(&stated)) {
      printf("glyph %zu stated: empty\n", glyph);
    }
    else {
      printf("glyph %zu stated: %d %d %d %d\n", glyph, stated.x_min,
          stated.y_min, stated.x_max, stated.y_max);
    }
    if (gfnt_outline_bounds(outline, &drawn) == GFNT_OK) {
      if (gfnt_box_is_empty(&drawn)) {
        printf("glyph %zu drawn: empty\n", glyph);
      }
      else {
        printf("glyph %zu drawn: %d %d %d %d\n", glyph, drawn.x_min,
            drawn.y_min, drawn.x_max, drawn.y_max);
      }
    }
    // The control box as well as the curve's own, and the pair is the point:
    // `glyf`'s stated box is the *coordinate* box by definition, so that is the
    // one a reference can be compared against, and the curve's own box has no
    // counterpart in the file at all.
    if (gfnt_outline_control_box(outline, &control) == GFNT_OK) {
      if (gfnt_box_is_empty(&control)) {
        printf("glyph %zu control: empty\n", glyph);
      }
      else {
        printf("glyph %zu control: %d %d %d %d\n", glyph, control.x_min,
            control.y_min, control.x_max, control.y_max);
      }
    }
    for (size_t contour = 0; contour < gfnt_outline_contour_count(outline);
        ++contour) {
      size_t contour_first = 0;
      size_t count = 0;

      (void)gfnt_outline_contour_at(outline, contour, &contour_first, &count);
      printf("glyph %zu contour %zu: first %zu, points %zu\n", glyph, contour,
          contour_first, count);
    }
    for (size_t point = 0; point < gfnt_outline_point_count(outline);
        ++point) {
      GFNT_Point where;
      GFNT_PointTag tag = GFNT_POINT_ON;

      (void)gfnt_outline_point_at(outline, point, &where, &tag);
      printf("glyph %zu point %zu: %d %d %s\n", glyph, point, where.x, where.y,
          gfnt_point_tag_string(tag));
    }
    if (gfnt_outline_decompose(outline, &sink, &context, &error) != GFNT_OK) {
      printf("glyph %zu path: refused %s\n", glyph,
          gfnt_result_string(error.result));
    }
    gfnt_outline_destroy(outline);
  }

  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
