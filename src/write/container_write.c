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

#include <stdint.h>
#include <string.h>
#include <ghoti.io/font/macros.h>
#include "../cff/cff.h"
#include "../sfnt/sfnt.h"
#include "../type1/type1.h"
#include "write.h"

/*
 * The writers that re-wrap rather than re-encode: a bare CFF out of an OpenType
 * font and a Type 1 program out of a Type 1 face (PFA or PFB, the private portion
 * encrypted afresh). The gzip layer around any of them is in src/blob/gzip.c,
 * where bytes may be taken from a blob.
 *
 * None of them converts outlines. A TrueType face has no Type 2 charstrings to
 * give and a CFF face has no Type 1 charstrings to give; that conversion is a
 * different piece of work and is refused here by name, not approximated.
 */

GFNT_Result gfnt_write_cff(const GFNT_Face * face, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob, GFNT_Error * error) {
  const uint8_t * data = NULL;
  size_t length = 0;
  GFNT_Limits defaults;

  if (!face || !out_blob) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the font");
  }
  if (!limits) {
    gfnt_limits_default(&defaults);
    limits = &defaults;
  }
  if (gfnt_face_table_data(face, GFNT_TAG_CFF, &data, &length) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF, 0,
        GFNT_GLYPH_NONE,
        "this face has no CFF table; other outlines are not converted to Type 2");
  }
  return gfnt_blob_create_memory(data, length, GFNT_BLOB_COPY, limits,
      allocator, out_blob, error);
}

/** Adobe's cipher, encrypting. */
static uint16_t type1_encrypt(const uint8_t * in, size_t length, uint16_t r,
    uint8_t * out) {
  size_t i;

  for (i = 0; i < length; ++i) {
    uint8_t cipher = (uint8_t)(in[i] ^ (r >> 8));

    r = (uint16_t)((cipher + r) * 52845u + 22719u);
    out[i] = cipher;
  }
  return r;
}

static bool is_hex_digit(uint8_t b) {
  return (b >= '0' && b <= '9') || (b >= 'a' && b <= 'f') || (b >= 'A' && b <= 'F');
}

GFNT_Result gfnt_write_type1(const GFNT_Face * face, GFNT_Type1Format format,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Blob ** out_blob, GFNT_Error * error) {
  const uint8_t * program = NULL;
  size_t length = 0;
  size_t program_length = 0;
  size_t split = 0;
  size_t private_at;
  size_t search;
  uint8_t * plain = NULL;
  uint8_t * cipher = NULL;
  GFNT_WBuf out;
  GFNT_Limits defaults;
  GFNT_Result result;
  size_t plain_length;
  unsigned attempt;
  size_t i;

  if (!face || !out_blob || (int)format < 0 || format >= GFNT_TYPE1_FORMAT_COUNT) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, nowhere to put the font, or a format that is not one");
  }
  if (!limits) {
    gfnt_limits_default(&defaults);
    limits = &defaults;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  if (gfnt_face_flavour(face) != GFNT_FLAVOUR_TYPE1
      || gfnt_face_table_data(face, GFNT_TAG_TYPE1, &program, &length) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
        "only a Type 1 face can be written as Type 1; outlines are not converted");
  }
  // The face holds the program as the cleartext up to `eexec` (and, for a PFB,
  // the ASCII trailer that followed the binary segment) and then the private
  // portion's plaintext, which begins where the face recorded.
  // The real `eexec` is the last one before the private portion: a comment in the
  // header can say the word too, and the trailer a PFB keeps after its binary
  // segment cannot.
  private_at = face->type1_private;
  search = private_at < length ? private_at : length;
  for (i = search; i >= 5; --i) {
    if (memcmp(program + i - 5, "eexec", 5) == 0) {
      split = i;
      break;
    }
  }
  program_length = length;
  if (split == 0 || private_at < split || private_at >= length) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE, "the program has no eexec to split it at");
  }
  // A PFA whose private portion is raw binary has nothing to say where the cipher
  // stops, so the reader takes everything to the end of the file - and what comes
  // after `closefile` there is the ASCII trailer, deciphered into noise. The
  // private portion ends at `closefile` and the line break after it.
  {
    size_t k;

    for (k = length; k >= private_at + 9u; --k) {
      if (memcmp(program + k - 9, "closefile", 9) == 0) {
        length = k;
        if (length < program_length && (program[length] == '\r' || program[length] == '\n')) {
          length += 1;
        }
        break;
      }
    }
  }
  plain_length = length - private_at + 4u;
  plain = allocator->calloc_fn(allocator->ctx, plain_length, 1);
  cipher = allocator->calloc_fn(allocator->ctx, plain_length, 1);
  gfnt_wbuf_init(&out, allocator);
  if (!plain || !cipher) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE, "allocating the private portion");
    goto done;
  }
  memcpy(plain + 4, program + private_at, length - private_at);
  // The four bytes eexec opens with are arbitrary; they are chosen so the first
  // four of the ciphertext are not all hex digits, which is the one rule a reader
  // uses to tell binary from ASCII-hex.
  for (attempt = 0; attempt < 256u; ++attempt) {
    plain[0] = (uint8_t)attempt;
    plain[1] = plain[2] = plain[3] = 0;
    type1_encrypt(plain, plain_length, 55665u, cipher);
    if (!(is_hex_digit(cipher[0]) && is_hex_digit(cipher[1]) && is_hex_digit(cipher[2])
            && is_hex_digit(cipher[3]))) {
      break;
    }
  }
  if (format == GFNT_TYPE1_FORMAT_PFB) {
    gfnt_wbuf_u8(&out, 0x80);
    gfnt_wbuf_u8(&out, 0x01);
    gfnt_wbuf_u8(&out, (split + 1) & 0xFF);
    gfnt_wbuf_u8(&out, ((split + 1) >> 8) & 0xFF);
    gfnt_wbuf_u8(&out, ((split + 1) >> 16) & 0xFF);
    gfnt_wbuf_u8(&out, ((split + 1) >> 24) & 0xFF);
    gfnt_wbuf_bytes(&out, program, split);
    gfnt_wbuf_u8(&out, '\n');
    gfnt_wbuf_u8(&out, 0x80);
    gfnt_wbuf_u8(&out, 0x02);
    gfnt_wbuf_u8(&out, plain_length & 0xFF);
    gfnt_wbuf_u8(&out, (plain_length >> 8) & 0xFF);
    gfnt_wbuf_u8(&out, (plain_length >> 16) & 0xFF);
    gfnt_wbuf_u8(&out, (plain_length >> 24) & 0xFF);
    gfnt_wbuf_bytes(&out, cipher, plain_length);
  }
  else {
    static const char digits[] = "0123456789abcdef";

    gfnt_wbuf_bytes(&out, program, split);
    gfnt_wbuf_u8(&out, '\n');
    for (i = 0; i < plain_length; ++i) {
      gfnt_wbuf_u8(&out, (uint8_t)digits[cipher[i] >> 4]);
      gfnt_wbuf_u8(&out, (uint8_t)digits[cipher[i] & 15]);
      if ((i & 31u) == 31u || i + 1 == plain_length) {
        gfnt_wbuf_u8(&out, '\n');
      }
    }
  }
  {
    static const char trailer[] =
        "0000000000000000000000000000000000000000000000000000000000000000\n";
    size_t t_length = 8 * (sizeof trailer - 1) + sizeof "cleartomark\n" - 1;

    if (format == GFNT_TYPE1_FORMAT_PFB) {
      gfnt_wbuf_u8(&out, 0x80);
      gfnt_wbuf_u8(&out, 0x01);
      gfnt_wbuf_u8(&out, t_length & 0xFF);
      gfnt_wbuf_u8(&out, (t_length >> 8) & 0xFF);
      gfnt_wbuf_u8(&out, 0);
      gfnt_wbuf_u8(&out, 0);
    }
    for (i = 0; i < 8; ++i) {
      gfnt_wbuf_bytes(&out, (const uint8_t *)trailer, sizeof trailer - 1);
    }
    gfnt_wbuf_bytes(&out, (const uint8_t *)"cleartomark\n", sizeof "cleartomark\n" - 1);
    if (format == GFNT_TYPE1_FORMAT_PFB) {
      gfnt_wbuf_u8(&out, 0x80);
      gfnt_wbuf_u8(&out, 0x03);
    }
  }
  if (out.oom) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE, "building the program");
    goto done;
  }
  result = gfnt_blob_create_memory(out.data, out.length, GFNT_BLOB_COPY, limits,
      allocator, out_blob, error);

done:
  allocator->free_fn(allocator->ctx, plain);
  allocator->free_fn(allocator->ctx, cipher);
  gfnt_wbuf_free(&out);
  return result;
}
