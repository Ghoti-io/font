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
 * Print a face's TrueType instruction programs as mnemonics.
 *
 * Usage: font-program <font> [face-index] [glyph-stride]
 *        font-program --hex <hex-bytes>
 *
 * Prints `== fpgm`, `== prep`, then `== glyph N` for every glyph that has a
 * program (every glyph-stride'th glyph, default 1), each followed by its
 * disassembly. A program that fails the structural check is followed by a
 * `!! <problem> at <offset>` line. With `--hex`, the bytes given are the program.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>

static const char * problem_name(GFNT_ProgramProblem problem) {
  switch (problem) {
    case GFNT_PROGRAM_OK: return "ok";
    case GFNT_PROGRAM_TRUNCATED_PUSH: return "truncated push";
    case GFNT_PROGRAM_UNBALANCED_IF: return "unbalanced if";
    case GFNT_PROGRAM_UNBALANCED_DEF: return "unbalanced definition";
  }
  return "unknown";
}

static int show(const char * title, const uint8_t * bytes, size_t length) {
  GFNT_ProgramReport report;

  printf("== %s\n", title);
  if (gfnt_program_disassemble(bytes, length, stdout) != GFNT_OK
      || gfnt_program_validate(bytes, length, &report) != GFNT_OK) {
    return 1;
  }
  if (report.problem != GFNT_PROGRAM_OK) {
    printf("!! %s at %zu\n", problem_name(report.problem), report.offset);
  }
  return 0;
}

int main(int argc, char ** argv) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  GFNT_Result result;
  size_t index = 0;
  size_t stride = 1;
  size_t glyphs = 0;
  int status = 0;
  const char * tables[2] = {"fpgm", "prep"};
  const uint8_t * probe = NULL;

  if (argc == 3 && strcmp(argv[1], "--hex") == 0) {
    size_t digits = strlen(argv[2]);
    uint8_t * bytes = malloc(digits / 2u + 1u);

    if (!bytes || digits % 2u) {
      free(bytes);
      fprintf(stderr, "%s: --hex wants an even number of hex digits\n", argv[0]);
      return 2;
    }
    for (size_t i = 0; i < digits / 2u; ++i) {
      char pair[3] = {argv[2][2u * i], argv[2][2u * i + 1u], 0};
      char * end = NULL;

      bytes[i] = (uint8_t)strtoul(pair, &end, 16);
      if (*end) {
        free(bytes);
        fprintf(stderr, "%s: not hex: %s\n", argv[0], pair);
        return 2;
      }
    }
    status = show("hex", bytes, digits / 2u);
    free(bytes);
    return status;
  }
  if (argc < 2) {
    fprintf(stderr, "usage: %s <font> [face-index] [glyph-stride]\n"
        "       %s --hex <hex-bytes>\n", argv[0], argv[0]);
    return 2;
  }
  if (argc > 2) {
    index = (size_t)strtoul(argv[2], NULL, 10);
  }
  if (argc > 3) {
    stride = (size_t)strtoul(argv[3], NULL, 10);
    if (stride == 0) {
      stride = 1;
    }
  }
  gfnt_error_clear(&error);
  result = gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error);
  if (result == GFNT_OK) {
    result = gfnt_face_load(blob, index, NULL, NULL, &face, &error);
  }
  if (result != GFNT_OK) {
    fprintf(stderr, "%s: %s\n", argv[1], gfnt_result_string(result));
    gfnt_error_dump(&error, stderr);
    gfnt_blob_destroy(blob);
    return 1;
  }
  for (size_t t = 0; t < 2; ++t) {
    const uint8_t * data = NULL;
    size_t length = 0;
    GFNT_Tag tag = GFNT_TAG(tables[t][0], tables[t][1], tables[t][2], tables[t][3]);

    if (gfnt_face_table_data(face, tag, &data, &length) == GFNT_OK) {
      status |= show(tables[t], data, length);
    }
  }
  if (gfnt_face_num_glyphs(face, &glyphs, &error) != GFNT_OK
      || gfnt_face_table_data(face, GFNT_TAG('g', 'l', 'y', 'f'), &probe, NULL)
          != GFNT_OK) {
    glyphs = 0;
  }
  for (size_t glyph = 0; glyph < glyphs; glyph += stride) {
    const uint8_t * data = NULL;
    size_t length = 0;
    char title[48];

    result = gfnt_face_glyph_program(face, (uint32_t)glyph, &data, &length, &error);
    if (result != GFNT_OK) {
      printf("== glyph %zu\n?? %s\n", glyph, gfnt_result_string(result));
      continue;
    }
    if (length == 0) {
      continue;
    }
    snprintf(title, sizeof title, "glyph %zu", glyph);
    status |= show(title, data, length);
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return status;
}
