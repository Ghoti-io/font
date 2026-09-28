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
 * Print every CFF glyph's charstring, the path it draws, and what it says about
 * its own advance.
 *
 * Usage: font-charstring <font> [face-index] [stride] [first]
 *
 * **This is the driver `tools/oracle/cff_diff.py` compares against fontTools.**
 * It prints three different things per glyph, and the reason there are three is
 * that each can be right while another is wrong:
 *
 *   * the **program**, operator by operator, which compares against fontTools'
 *     decompilation of the same bytes. A misread operand is a wrong number here
 *     and a plausible shape everywhere else;
 *   * the **path**, which compares against a reference pen and is the only thing
 *     that tests the curve operators' geometry - `hflex`'s recomputed y, the
 *     trailing operand of `hvcurveto`, `flex1`'s choice of axis;
 *   * the **advance the charstring states**, which is `nominalWidthX` plus a
 *     delta, or `defaultWidthX` where the program states nothing. It is a
 *     different fact from `hmtx`'s advance and the two may disagree.
 *
 * `font-outline` is the same idea for `glyf`, and there are two drivers rather
 * than one because the two formats do not answer the same questions: a CFF glyph
 * states no bounding box of its own, and a `glyf` glyph has no program.
 */

#include <stdio.h>
#include <stdlib.h>

#include <ghoti.io/font/charstring.h>
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
  {
    const uint8_t * bytes = NULL;
    size_t length = 0;
    GFNT_CharstringType type = GFNT_CHARSTRING_TYPE2;

    // Asked of glyph 0, which every font has, so that a face with no
    // charstrings says so on one line rather than printing a refusal per glyph.
    // A differential reads nothing as agreement; it must read this as "there is
    // nothing here to compare".
    if (gfnt_face_glyph_charstring(face, 0, &type, &bytes, &length, &error)
        == GFNT_ERR_UNSUPPORTED) {
      printf("charstrings: none\n");
      gfnt_face_free(face);
      gfnt_blob_destroy(blob);
      return 0;
    }
  }
  printf("charstrings: %zu glyph(s), stride %zu from %zu\n", glyphs, stride,
      first);

  for (size_t glyph = first; glyph < glyphs; glyph += stride) {
    GFNT_Outline * outline = NULL;
    GFNT_CharstringMetrics metrics;
    GFNT_CharstringType type = GFNT_CHARSTRING_TYPE2;
    const uint8_t * bytes = NULL;
    size_t length = 0;
    Context context;
    GFNT_Box control;
    char * name = NULL;

    context.glyph = (unsigned long)glyph;
    gfnt_error_clear(&error);
    result = gfnt_face_glyph_charstring(face, (uint32_t)glyph, &type, &bytes,
        &length, &error);
    if (result != GFNT_OK) {
      printf("glyph %zu: refused %s\n", glyph, gfnt_result_string(result));
      continue;
    }
    result = gfnt_face_glyph_charstring_metrics(face, (uint32_t)glyph, &metrics,
        &error);
    if (result != GFNT_OK) {
      printf("glyph %zu: refused %s\n", glyph, gfnt_result_string(result));
      continue;
    }
    result = gfnt_face_glyph_outline(face, (uint32_t)glyph, NULL, NULL,
        &outline, &error);
    if (result != GFNT_OK) {
      printf("glyph %zu: refused %s\n", glyph, gfnt_result_string(result));
      continue;
    }
    // Two lines rather than one: the point and contour counts have no
    // counterpart in a CFF at all - fontTools can draw the glyph and cannot say
    // how many points this library made of it - so they are printed apart from
    // the facts a differential compares.
    printf("glyph %zu: seac %d, bytes %zu\n", glyph, metrics.seac ? 1 : 0,
        length);
    printf("glyph %zu counts: points %zu, contours %zu\n", glyph,
        gfnt_outline_point_count(outline),
        gfnt_outline_contour_count(outline));
    // The advance in 16.16, as the interpreter holds it: a charstring's width
    // can be fractional, because `div` exists.
    printf("glyph %zu width: %d stated %d\n", glyph, metrics.width,
        metrics.width_stated ? 1 : 0);
    if (gfnt_face_glyph_name(face, (uint32_t)glyph, NULL, &name, NULL, &error)
        == GFNT_OK) {
      printf("glyph %zu name: %s\n", glyph, name);
      gfnt_glyph_name_free(NULL, name);
    }
    else {
      printf("glyph %zu name: -\n", glyph);
    }
    if (gfnt_outline_control_box(outline, &control) == GFNT_OK) {
      if (gfnt_box_is_empty(&control)) {
        printf("glyph %zu control: empty\n", glyph);
      }
      else {
        printf("glyph %zu control: %d %d %d %d\n", glyph, control.x_min,
            control.y_min, control.x_max, control.y_max);
      }
    }
    if (gfnt_outline_decompose(outline, &sink, &context, &error) != GFNT_OK) {
      printf("glyph %zu path: refused %s\n", glyph,
          gfnt_result_string(error.result));
    }
    gfnt_outline_destroy(outline);

    // The program last, and prefixed, so that a reader can find the operator a
    // path disagreement came from without running anything.
    printf("glyph %zu program:\n", glyph);
    {
      // gfnt_charstring_dump writes plain lines, and they are bracketed by the
      // two marker lines rather than prefixed one by one: the dump is the thing
      // under test, and wrapping its output here would test a copy of it.
      // With the metrics, so that a hintmask whose stems a subroutine declared
      // still has its mask bytes printed: the stem count comes from the run
      // above rather than from a second way of counting them.
      GFNT_Result dumped = gfnt_charstring_dump(type, bytes, length, &metrics,
          stdout);

      if (dumped != GFNT_OK) {
        printf("program: refused %s\n", gfnt_result_string(dumped));
      }
    }
    printf("glyph %zu program-end\n", glyph);
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
