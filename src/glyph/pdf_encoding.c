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
 * The base encodings of a PDF font (include/ghoti.io/font/pdf.h): which glyph name each
 * code of StandardEncoding, WinAnsiEncoding, MacRomanEncoding, MacExpertEncoding, Symbol
 * and ZapfDingbats stands for. The tables are generated (tools/gen/pdf-encodings.sh).
 */

#include <ghoti.io/font/pdf.h>
#include <string.h>
#include "pdf_encodings.h"

const char * gfnt_pdf_encoding_glyph_name(GFNT_PdfEncoding encoding, uint8_t code) {
  if ((unsigned)encoding >= (unsigned)GFNT_PDF_ENCODING_COUNT) {
    return NULL;
  }
  if (gfnt_pdf_encoding_table[encoding][code] == 0) {
    return NULL;
  }
  return gfnt_pdf_encoding_names[gfnt_pdf_encoding_table[encoding][code]];
}

bool gfnt_pdf_encoding_code_for_name(GFNT_PdfEncoding encoding, const char * name,
    uint8_t * out_code) {
  unsigned code;

  if ((unsigned)encoding >= (unsigned)GFNT_PDF_ENCODING_COUNT || !name || !out_code) {
    return false;
  }
  for (code = 0; code < 256; code++) {
    const char * candidate = gfnt_pdf_encoding_glyph_name(encoding, (uint8_t)code);

    if (candidate && strcmp(candidate, name) == 0) {
      *out_code = (uint8_t)code;
      return true;
    }
  }
  return false;
}
