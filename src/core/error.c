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
 * Diagnostics: where a failure happened, beyond which failure it was.
 *
 * documentation/design.md section 5.6. The offset is within the table rather
 * than within the file, because the table's extent is what the reader
 * validated against and a table dump's offsets are what it can be compared
 * with.
 */

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/core.h>

char * gfnt_tag_string(GFNT_Tag tag, char * out) {
  if (!out) {
    return NULL;
  }

  for (int i = 0; i < 4; ++i) {
    unsigned byte = (unsigned)((tag >> (24 - 8 * i)) & 0xFFu);

    // Anything a terminal would not survive becomes '.'; the number is the
    // identity and this is only the label.
    out[i] = (byte >= 0x20u && byte < 0x7Fu) ? (char)byte : '.';
  }
  out[4] = '\0';
  return out;
}

void gfnt_error_clear(GFNT_Error * error) {
  if (!error) {
    return;
  }

  *error = (GFNT_Error) {
    .result = GFNT_OK,
    .table = 0,
    .offset = 0,
    .glyph = GFNT_GLYPH_NONE,
    .message = NULL,
  };
}

GFNT_Result gfnt_error_set(GFNT_Error * error, GFNT_Result result,
    GFNT_Tag table, size_t offset, uint32_t glyph, const char * message) {
  if (error) {
    *error = (GFNT_Error) {
      .result = result,
      .table = table,
      .offset = offset,
      .glyph = glyph,
      .message = message,
    };
  }
  return result;
}

GFNT_Result gfnt_error_dump(const GFNT_Error * error, FILE * out) {
  char tag[5];

  if (!error || !out) {
    return GFNT_ERR_INVALID;
  }

  if (error->result == GFNT_OK) {
    return fprintf(out, "no error recorded\n") < 0 ? GFNT_ERR_IO : GFNT_OK;
  }

  if (fprintf(out, "%s", gfnt_result_string(error->result)) < 0) {
    return GFNT_ERR_IO;
  }
  if (error->table != 0) {
    if (fprintf(out, " in table '%s'", gfnt_tag_string(error->table, tag))
        < 0) {
      return GFNT_ERR_IO;
    }
  }
  if (fprintf(out, " at offset %zu", error->offset) < 0) {
    return GFNT_ERR_IO;
  }
  if (error->glyph != GFNT_GLYPH_NONE) {
    if (fprintf(out, " for glyph %u", error->glyph) < 0) {
      return GFNT_ERR_IO;
    }
  }
  if (error->message) {
    if (fprintf(out, ": %s", error->message) < 0) {
      return GFNT_ERR_IO;
    }
  }
  return fprintf(out, "\n") < 0 ? GFNT_ERR_IO : GFNT_OK;
}
