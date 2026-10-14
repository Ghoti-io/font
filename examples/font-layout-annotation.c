/* SPDX-License-Identifier: LGPL-3.0-only */
#include <ghoti.io/font/font.h>
#include <ghoti.io/font/layout.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char ** argv) {
  static const char text[] = "日本語";
  static const char ruby_text[] = "日本";
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Paragraph * paragraph = NULL;
  GFNT_Layout layout = {0};
  const GFNT_Face * faces[] = {NULL};
  GFNT_TextSpan span = {0};
  GFNT_Annotation ruby = {0};
  GFNT_ParagraphOptions options = {0};
  GFNT_WritingMode mode = GFNT_WRITING_HORIZONTAL_TB;
  GFNT_Error error;

  if (argc != 3) return 2;
  if (strcmp(argv[2], "vertical-rl") == 0) mode = GFNT_WRITING_VERTICAL_RL;
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
  span.length = sizeof text - 1;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = 100 * 64;
  ruby.base_start = 0;
  ruby.base_end = sizeof text - 1;
  ruby.kind = GFNT_ANNOTATION_RUBY;
  ruby.position = GFNT_ANNOTATION_OVER;
  ruby.style.faces = faces;
  ruby.style.face_count = 1;
  ruby.style.size = 40 * 64;
  ruby.text = ruby_text;
  ruby.text_length = sizeof ruby_text - 1;
  ruby.align = GFNT_ANNOTATION_ALIGN_CENTER;
  ruby.group_break = true;
  options.writing_mode = mode;
  options.text_orientation = GFNT_ORIENT_MIXED;
  options.annotations = &ruby;
  options.annotation_count = 1;
  if (gfnt_paragraph_create(text, sizeof text - 1, &span, 1, &options, NULL,
          &paragraph, &error) != GFNT_OK
      || gfnt_paragraph_layout(paragraph, GFNT_LAYOUT_UNBOUNDED, &layout, &error) != GFNT_OK
      || layout.line_count != 1 || layout.lines[0].annotation_count != 1) {
    gfnt_error_dump(&error, stderr);
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(paragraph);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }
  const GFNT_Rect box = layout.lines[0].annotations[0].box;
  printf("[%d,%d,%d,%d]\n", box.x, box.y, box.width, box.height);
  gfnt_layout_free(&layout);
  gfnt_paragraph_free(paragraph);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
