/* SPDX-License-Identifier: LGPL-3.0-only */
#include <ghoti.io/font/font.h>
#include <ghoti.io/font/layout.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char ** argv) {
  static const char balanced_text[] = "一\n二\n三\n四";
  static const char auto_text[] = "一\n二\n三\n四\n五";
  static const char varied_text[] = "一\n二\n三\n四";
  const bool automatic = argc == 3 && strcmp(argv[2], "auto") == 0;
  const bool conflict = argc == 3 && strcmp(argv[2], "conflict") == 0;
  const bool rtl = argc == 3 && strcmp(argv[2], "rtl") == 0;
  const bool vertical_rl = argc == 3 && strcmp(argv[2], "vertical-rl") == 0;
  const bool vertical_lr = argc == 3 && strcmp(argv[2], "vertical-lr") == 0;
  const bool varied = argc == 3 && strcmp(argv[2], "balance-varied") == 0;
  const bool discriminating = argc == 3 && strcmp(argv[2], "balance-minimax") == 0;
  const bool vertical = vertical_rl || vertical_lr;
  const char * text = automatic ? auto_text : (varied || discriminating) ? varied_text : balanced_text;
  const size_t text_length = automatic ? sizeof auto_text - 1 : (varied || discriminating)
      ? sizeof varied_text - 1 : sizeof balanced_text - 1;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Paragraph * paragraph = NULL;
  GFNT_ColumnLayout columns = {0};
  const GFNT_Face * faces[] = {NULL};
  GFNT_TextSpan span = {0};
  GFNT_TextSpan varied_spans[4] = {{0}};
  GFNT_ColumnOptions options = {0};
  GFNT_Error error;
  GFNT_ParagraphOptions paragraph_options = {0};

  if (argc != 2 && !automatic && !conflict && !rtl && !vertical && !varied && !discriminating) return 2;
  if (rtl) paragraph_options.direction = GFNT_BIDI_RTL;
  if (vertical_rl) paragraph_options.writing_mode = GFNT_WRITING_VERTICAL_RL;
  if (vertical_lr) paragraph_options.writing_mode = GFNT_WRITING_VERTICAL_LR;
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  faces[0] = face;
  span.length = text_length;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = 20 * 64;
  if (varied || discriminating) {
    const size_t lengths[] = {4, 4, 4, 3};
    const GFNT_F26Dot6 sizes[] = {discriminating ? 40 * 64 : 10 * 64,
        discriminating ? 10 * 64 : 30 * 64, 10 * 64,
        discriminating ? 10 * 64 : 30 * 64};
    for (size_t i = 0; i < 4; i++) {
      varied_spans[i] = span;
      varied_spans[i].length = lengths[i];
      varied_spans[i].style.size = sizes[i];
      if (discriminating) {
        varied_spans[i].style.line_height.kind = GFNT_LINE_HEIGHT_LENGTH;
        varied_spans[i].style.line_height.value = sizes[i];
      }
    }
  }
  options.inline_size = (vertical ? 41 : 80) * 64;
  options.block_extent = (vertical ? 41 : automatic || conflict ? 61 : (varied || discriminating) ? 100 : 50) * 64;
  options.count = 2;
  if (automatic || conflict || vertical) options.fill = GFNT_COLUMN_FILL_AUTO;
  if (conflict) {
    options.orphans = 3;
    options.widows = 2;
  }
  if (gfnt_paragraph_create(text, text_length, (varied || discriminating) ? varied_spans : &span,
          (varied || discriminating) ? 4 : 1, &paragraph_options, NULL,
          &paragraph, &error) != GFNT_OK
      || gfnt_paragraph_layout_columns(paragraph, &options, &columns, &error) != GFNT_OK
      || (!vertical && (!columns.complete
          || columns.layout.line_count != (automatic ? 5u : 4u)))) {
    gfnt_error_dump(&error, stderr);
    gfnt_column_layout_free(&columns);
    gfnt_paragraph_free(paragraph);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }
  printf("{\"complete\":%s,\"next\":%zu,\"columns\":[",
      columns.complete ? "true" : "false", columns.next_byte);
  for (size_t i = 0; i <= columns.column_count; i++) {
    if (i) putchar(',');
    printf("%zu", columns.column_line_starts[i]);
  }
  printf("],\"lines\":[");
  for (size_t i = 0; i < columns.layout.line_count; i++) {
    const GFNT_Rect box = columns.layout.lines[i].box;
    if (i) putchar(',');
    printf("[%d,%d,%d,%d,%d]", box.x, box.y, box.width, box.height,
        columns.layout.lines[i].extent);
  }
  puts("]}");
  gfnt_column_layout_free(&columns);
  gfnt_paragraph_free(paragraph);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
