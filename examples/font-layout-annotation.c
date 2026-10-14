/* SPDX-License-Identifier: LGPL-3.0-only */
#include <ghoti.io/font/font.h>
#include <ghoti.io/font/layout.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char ** argv) {
  static const char text[] = "日本語";
  static const char ruby_text[] = "日本";
  static const char nested_text[] = "にほんご";
  static const char nested_reading[] = "ニホン";
  static const char fragment_base[] = "日本語日本語";
  static const char fragment_reading[] = "にほんごにほんご";
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Paragraph * paragraph = NULL;
  GFNT_Layout layout = {0};
  const GFNT_Face * faces[] = {NULL};
  GFNT_TextSpan span = {0};
  GFNT_Annotation ruby = {0};
  GFNT_Annotation annotation_list[3] = {{0}};
  GFNT_ParagraphOptions options = {0};
  GFNT_WritingMode mode = GFNT_WRITING_HORIZONTAL_TB;
  const char * scenario = argc == 4 ? argv[3] : "simple";
  bool fragments = strcmp(scenario, "fragments") == 0;
  bool nested = strcmp(scenario, "nested") == 0;
  bool overhang = strcmp(scenario, "overhang") == 0
      || strcmp(scenario, "overhang-none") == 0
      || strcmp(scenario, "overhang-wrap") == 0
      || strcmp(scenario, "overhang-wrap-none") == 0;
  bool wrap_overhang = strcmp(scenario, "overhang-wrap") == 0
      || strcmp(scenario, "overhang-wrap-none") == 0;
  bool emphasis = strcmp(scenario, "emphasis") == 0
      || strcmp(scenario, "emphasis-default") == 0
      || strcmp(scenario, "emphasis-dot") == 0
      || strcmp(scenario, "emphasis-open-circle") == 0
      || strcmp(scenario, "emphasis-double-circle") == 0
      || strcmp(scenario, "emphasis-triangle") == 0
      || strcmp(scenario, "emphasis-open-sesame") == 0
      || strcmp(scenario, "emphasis-string") == 0;
  bool double_side = strcmp(scenario, "double-side") == 0;
  bool triple_side = strcmp(scenario, "triple-side") == 0;
  if (!fragments && !nested && !overhang && !emphasis && !double_side && !triple_side
      && strcmp(scenario, "simple") != 0) return 2;
  GFNT_Error error;

  if (argc != 3 && argc != 4) return 2;
  if (strcmp(argv[2], "vertical-rl") == 0) mode = GFNT_WRITING_VERTICAL_RL;
  else if (strcmp(argv[2], "vertical-lr") == 0) mode = GFNT_WRITING_VERTICAL_LR;
  else if (strcmp(argv[2], "sideways-rl") == 0) mode = GFNT_WRITING_SIDEWAYS_RL;
  else if (strcmp(argv[2], "sideways-lr") == 0) mode = GFNT_WRITING_SIDEWAYS_LR;
  else if (strcmp(argv[2], "horizontal") != 0) {
    fprintf(stderr, "unsupported writing mode: %s\n", argv[2]);
    return 2;
  }
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  faces[0] = face;
  const char * source = fragments ? fragment_base : text;
  size_t source_length = fragments ? sizeof fragment_base - 1 : sizeof text - 1;
  span.length = source_length;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = 100 * 64;
  ruby.base_start = 0;
  ruby.base_end = overhang ? 3 : source_length;
  ruby.kind = emphasis ? GFNT_ANNOTATION_EMPHASIS : GFNT_ANNOTATION_RUBY;
  ruby.position = GFNT_ANNOTATION_OVER;
  ruby.style.faces = faces;
  ruby.style.face_count = 1;
  ruby.style.size = 40 * 64;
  static const char overhang_reading[] = "日本語";
  ruby.text = nested ? nested_text : fragments ? fragment_reading
      : overhang ? overhang_reading : ruby_text;
  ruby.text_length = nested ? sizeof nested_text - 1
      : fragments ? sizeof fragment_reading - 1
      : overhang ? sizeof overhang_reading - 1 : sizeof ruby_text - 1;
  ruby.align = GFNT_ANNOTATION_ALIGN_CENTER;
  if (emphasis) {
    ruby.text = NULL;
    ruby.text_length = 0;
    ruby.mark_kind = GFNT_EMPHASIS_MARK_CIRCLE;
    ruby.filled = true;
    if (strcmp(scenario, "emphasis-default") == 0) {
      ruby.mark_kind = GFNT_EMPHASIS_MARK_DEFAULT;
    }
    else if (strcmp(scenario, "emphasis-dot") == 0) {
      ruby.mark_kind = GFNT_EMPHASIS_MARK_DOT;
    }
    else if (strcmp(scenario, "emphasis-open-circle") == 0) {
      ruby.filled = false;
    }
    else if (strcmp(scenario, "emphasis-double-circle") == 0) {
      ruby.mark_kind = GFNT_EMPHASIS_MARK_DOUBLE_CIRCLE;
    }
    else if (strcmp(scenario, "emphasis-triangle") == 0) {
      ruby.mark_kind = GFNT_EMPHASIS_MARK_TRIANGLE;
    }
    else if (strcmp(scenario, "emphasis-open-sesame") == 0) {
      ruby.mark_kind = GFNT_EMPHASIS_MARK_SESAME;
      ruby.filled = false;
    }
    else if (strcmp(scenario, "emphasis-string") == 0) {
      ruby.mark_kind = GFNT_EMPHASIS_MARK_STRING;
      ruby.text = "x";
      ruby.text_length = 1;
    }
  }
  ruby.group_break = true;
  if (strcmp(scenario, "overhang") == 0 || strcmp(scenario, "overhang-wrap") == 0)
    ruby.overhang = GFNT_RUBY_OVERHANG_AUTO;
  GFNT_Annotation child = {0};
  if (nested) {
    child.base_start = 0;
    child.base_end = 6;
    child.kind = GFNT_ANNOTATION_RUBY;
    child.position = GFNT_ANNOTATION_OVER;
    child.style = ruby.style;
    child.text = nested_reading;
    child.text_length = sizeof nested_reading - 1;
    child.align = GFNT_ANNOTATION_ALIGN_CENTER;
    child.group_break = true;
    ruby.children = &child;
    ruby.child_count = 1;
  }
  annotation_list[0] = ruby;
  if (double_side || triple_side) {
    annotation_list[0].base_end = 3;
    annotation_list[0].text = "に";
    annotation_list[0].text_length = 3;
    annotation_list[1] = annotation_list[0];
    annotation_list[1].position = GFNT_ANNOTATION_UNDER;
    annotation_list[1].text = "Japan";
    annotation_list[1].text_length = 5;
    if (triple_side) {
      annotation_list[2] = annotation_list[0];
      annotation_list[2].text = "ひ";
      annotation_list[2].text_length = 3;
    }
  }
  if (fragments) ruby.group_break = false;
  options.writing_mode = mode;
  options.text_orientation = GFNT_ORIENT_MIXED;
  options.annotations = double_side || triple_side ? annotation_list : &ruby;
  options.annotation_count = triple_side ? 3 : double_side ? 2 : 1;
  if (gfnt_paragraph_create(source, source_length, &span, 1, &options, NULL,
          &paragraph, &error) != GFNT_OK
      || gfnt_paragraph_layout(paragraph, fragments ? 300 * 64
              : wrap_overhang ? 210 * 64 : GFNT_LAYOUT_UNBOUNDED,
          &layout, &error) != GFNT_OK || !layout.line_count) {
    gfnt_error_dump(&error, stderr);
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(paragraph);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }
  if (strcmp(scenario, "simple") != 0) {
    GFNT_Rect base = {0};
    size_t base_count = 0;
    if ((double_side || triple_side) && (gfnt_layout_range_rects(paragraph, &layout, 0, 3,
            &base, 1, &base_count) != GFNT_OK || base_count != 1)) {
      gfnt_layout_free(&layout);
      gfnt_paragraph_free(paragraph);
      gfnt_face_free(face);
      gfnt_blob_destroy(blob);
      return 1;
    }
    if (double_side || triple_side) {
      printf("{\"base\":[%d,%d,%d,%d],\"lines\":[", base.x, base.y,
          base.width, base.height);
    }
    else {
      printf("{\"lines\":[");
    }
    for (size_t i = 0; i < layout.line_count; ++i) {
      const GFNT_Line * line = &layout.lines[i];
      if (i) putchar(',');
      printf("{\"start\":%zu,\"end\":%zu,\"extent\":%d,\"runs\":[",
          line->start, line->end, line->extent);
      for (size_t j = 0; j < line->run_count; ++j) {
        if (j) putchar(',');
        printf("[%zu,%zu,%d,%d,[", line->runs[j].start, line->runs[j].end,
            line->runs[j].pos, line->runs[j].advance);
        for (size_t k = 0; k < line->runs[j].glyph_count; ++k) {
          if (k) putchar(',');
          printf("[%u,%d,%d]", line->runs[j].glyphs[k].cluster,
              line->runs[j].glyphs[k].pen_x, line->runs[j].glyphs[k].advance);
        }
        printf("]]");
      }
      printf("],\"annotations\":[");
      for (size_t j = 0; j < line->annotation_count; ++j) {
        const GFNT_LineAnnotation * a = &line->annotations[j];
        if (j) putchar(',');
        printf("{\"base_start\":%zu,\"base_end\":%zu,\"box\":[%d,%d,%d,%d],\"runs\":[",
            a->base_start, a->base_end, a->box.x, a->box.y, a->box.width, a->box.height);
        for (size_t k = 0; k < a->run_count; ++k) {
          if (k) putchar(',');
          printf("[%zu,%zu]", a->runs[k].start, a->runs[k].end);
        }
        printf("],\"children\":[");
        for (size_t k = 0; k < a->child_count; ++k) {
          const GFNT_LineAnnotation * c = &a->children[k];
          if (k) putchar(',');
          printf("{\"base_start\":%zu,\"base_end\":%zu,\"box\":[%d,%d,%d,%d],\"runs\":[",
              c->base_start, c->base_end, c->box.x, c->box.y, c->box.width, c->box.height);
          for (size_t m = 0; m < c->run_count; ++m) {
            if (m) putchar(',');
            printf("[%zu,%zu]", c->runs[m].start, c->runs[m].end);
          }
          printf("]}");
        }
        printf("]}");
      }
      printf("]}");
    }
    printf("]}\n");
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(paragraph);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 0;
  }
  if (layout.lines[0].annotation_count != 1) {
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(paragraph);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }
  const GFNT_Rect box = layout.lines[0].annotations[0].box;
  GFNT_Rect base = {0};
  size_t base_count = 0;
  if (gfnt_layout_range_rects(paragraph, &layout, 0, sizeof text - 1,
          &base, 1, &base_count) != GFNT_OK || base_count != 1) {
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(paragraph);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }
  printf("[%d,%d,%d,%d,%d,%d,%d,%d]\n", box.x, box.y, box.width, box.height,
      base.x, base.y, base.width, base.height);
  gfnt_layout_free(&layout);
  gfnt_paragraph_free(paragraph);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
