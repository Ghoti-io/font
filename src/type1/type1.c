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
 * The Type 1 container: PFB and PFA framing, `eexec`, and the PostScript. See
 * type1.h.
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/macros.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "../cff/cff.h"
#include "../core/fixed.h"
#include "../core/matrix.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"
#include "../core/buffer.h"
#include "type1.h"

/** `eexec`'s key, and the four plaintext bytes it throws away. */
#define GFNT_TYPE1_EEXEC_R 55665u
#define GFNT_TYPE1_EEXEC_SKIP 4u
/** The charstring cipher's key. Its skip is `/lenIV`, which the font states. */
#define GFNT_TYPE1_CHARSTRING_R 4330u

/** PFB segment markers (Adobe Technical Note #5040). */
#define GFNT_TYPE1_PFB_MARKER 0x80u
#define GFNT_TYPE1_PFB_ASCII 0x01u
#define GFNT_TYPE1_PFB_BINARY 0x02u
#define GFNT_TYPE1_PFB_EOF 0x03u

/**
 * How far into a file `eexec` may be before this gives up looking.
 *
 * The cleartext header of a real font is a few hundred bytes to a few kilobytes.
 * A bound is needed because the search is what decides a file is Type 1 at all,
 * and without one a large file of anything would be scanned to its end before
 * being refused.
 */
#define GFNT_TYPE1_EEXEC_SEARCH 65536u

/**
 * One pass of Adobe's cipher, which both `eexec` and the charstrings use.
 *
 * The same four lines with a different key, so it is one function: two copies
 * would be two chances to get the constants wrong, and a wrong key produces
 * plausible-looking rubbish rather than an error.
 *
 * Returns the key's state, so a caller can run the leading random bytes through
 * with @p out NULL and then decipher the rest straight into its own buffer. That
 * is what keeps the charstring path from allocating: the padding has to go
 * through the cipher, because every byte after it depends on it, but it does not
 * have to be kept. @p out may be @p in, since each byte's plaintext depends only
 * on that byte and the running key.
 */
static uint16_t gfnt_type1_decipher(const uint8_t * in, size_t length,
    uint16_t r, uint8_t * out) {
  const unsigned c1 = 52845u;
  const unsigned c2 = 22719u;
  unsigned key = r;
  size_t at;

  for (at = 0; at < length; ++at) {
    const uint8_t cipher = in[at];

    if (out) {
      out[at] = (uint8_t)(cipher ^ (key >> 8));
    }
    // Unsigned, and every operand unsigned with it. `(cipher + r) * 52845` on
    // uint16_t operands promotes to `int` and overflows it, which is undefined
    // behaviour - UBSan reported it on the first fixture this read, and the
    // wrapping the cipher wants is only *defined* for unsigned types. The
    // release build happened to produce the right bytes anyway, which is the
    // worst way for this to be wrong.
    key = ((unsigned)cipher + key) * c1 + c2;
    key &= 0xFFFFu;
  }
  return (uint16_t)key;
}

/** Whether a byte is PostScript whitespace. */
static bool gfnt_type1_space(uint8_t byte) {
  return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n'
      || byte == '\f' || byte == 0;
}

/** A hex digit's value, or -1. */
static int gfnt_type1_hex(uint8_t byte) {
  if (byte >= '0' && byte <= '9') { return byte - '0'; }
  if (byte >= 'a' && byte <= 'f') { return byte - 'a' + 10; }
  if (byte >= 'A' && byte <= 'F') { return byte - 'A' + 10; }
  return -1;
}

bool gfnt_type1_looks_like(const GFNT_Reader * blob) {
  GFNT_Reader reader;
  const uint8_t * bytes = NULL;
  size_t length;

  if (!blob) {
    return false;
  }
  reader = *blob;
  length = reader.length < 4 ? reader.length : 4;
  if (length < 2 || gfnt_reader_seek(&reader, 0) != GFNT_OK
      || gfnt_read_bytes(&reader, length, &bytes) != GFNT_OK) {
    return false;
  }
  // PFB's segment marker, which is two bytes and cannot be mistaken for
  // anything: 0x80 is not a PostScript character and not an sfnt version.
  if (bytes[0] == GFNT_TYPE1_PFB_MARKER
      && (bytes[1] == GFNT_TYPE1_PFB_ASCII
          || bytes[1] == GFNT_TYPE1_PFB_BINARY)) {
    return true;
  }
  // Or a PostScript program. `%!` is the only opening a Type 1 font has - the
  // comment that follows it is `PS-AdobeFont`, `FontType1` or `PS-Adobe`
  // depending on who wrote it, so the two bytes are what is checked and the
  // `eexec` search is what confirms it.
  return bytes[0] == '%' && bytes[1] == '!';
}

/**
 * Join a PFB's segments: the ASCII ones into @p clear, the binary into @p cipher.
 *
 * A font over 64 KB is split into several binary segments, so both are
 * accumulated rather than assumed to be one each. The trailing ASCII segment -
 * 512 zeros and `cleartomark` - is joined onto the cleartext like the rest, where
 * it is simply text no key looks at.
 */
static GFNT_Result gfnt_type1_split_pfb(const uint8_t * in, size_t length,
    GFNT_Buffer * clear, GFNT_Buffer * cipher, GFNT_Error * error) {
  size_t at = 0;

  while (at + 2 <= length) {
    uint8_t kind;
    size_t size = 0;
    GFNT_Buffer * into;

    if (in[at] != GFNT_TYPE1_PFB_MARKER) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, at,
          GFNT_GLYPH_NONE, "a PFB segment that does not start with 0x80");
    }
    kind = in[at + 1];
    if (kind == GFNT_TYPE1_PFB_EOF) {
      break;
    }
    if (kind != GFNT_TYPE1_PFB_ASCII && kind != GFNT_TYPE1_PFB_BINARY) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, at + 1,
          GFNT_GLYPH_NONE, "a PFB segment of a kind the format does not define");
    }
    if (at + 6 > length) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, at,
          GFNT_GLYPH_NONE, "a PFB segment header cut short");
    }
    // Little-endian, which is the one length in this library that is: the
    // segment header is a PC file format's and not a font format's.
    size = (size_t)in[at + 2] | ((size_t)in[at + 3] << 8)
        | ((size_t)in[at + 4] << 16) | ((size_t)in[at + 5] << 24);
    at += 6;
    if (size > length - at) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, at,
          GFNT_GLYPH_NONE, "a PFB segment longer than the file");
    }
    into = kind == GFNT_TYPE1_PFB_BINARY ? cipher : clear;
    if (!gfnt_buffer_add(into, in + at, size)) {
      return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_TYPE1, 0,
          GFNT_GLYPH_NONE, "joining a PFB segment");
    }
    at += size;
  }
  if (cipher->length == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE, "a PFB with no binary segment, so no private portion");
  }
  return GFNT_OK;
}

/**
 * Split a PostScript program at `eexec`: text before, ciphertext after.
 *
 * The ciphertext is ASCII-hex when the four bytes after `eexec` are all hex
 * digits, which is Adobe's own rule and the only one available - a binary
 * private portion can begin with any four bytes, including four that spell hex.
 * Fonts that would be ambiguous do not exist because every writer that emits
 * binary emits PFB.
 */
static GFNT_Result gfnt_type1_split_ps(const uint8_t * in, size_t length,
    GFNT_Buffer * clear, GFNT_Buffer * cipher, GFNT_Error * error) {
  static const char marker[] = "eexec";
  const size_t marker_length = sizeof marker - 1;
  size_t limit = length < GFNT_TYPE1_EEXEC_SEARCH
      ? length : GFNT_TYPE1_EEXEC_SEARCH;
  size_t at;
  size_t start = 0;
  bool found = false;
  int hex_digits = 0;

  for (at = 0; at + marker_length <= limit; ++at) {
    if (memcmp(in + at, marker, marker_length) != 0) {
      continue;
    }
    found = true;
    start = at + marker_length;
    break;
  }
  if (!found) {
    return gfnt_error_set(error, GFNT_ERR_FORMAT, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE,
        "a PostScript program with no eexec in its first 64 KB, so not a Type 1 "
        "font program this library can read");
  }
  if (!gfnt_buffer_add(clear, in, start)) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE, "copying a Type 1 program's cleartext");
  }
  // The single end-of-line after `eexec` is part of the token, not of the data.
  while (start < length && gfnt_type1_space(in[start])) {
    start += 1;
  }

  for (at = start; at < length && at < start + 4; ++at) {
    if (gfnt_type1_hex(in[at]) >= 0) {
      hex_digits += 1;
    }
  }
  if (hex_digits == 4) {
    int high = -1;
    size_t end = length;
    size_t zeros = 0;
    size_t zero_start = length;

    // Where the hex *ends* has to be found before any of it is decoded, because
    // what follows it is 512 ASCII zeros - and a zero is a hex digit. "Decode
    // until something is not hex" reads the whole trailer as data and then
    // reports an odd digit count, which is what this did before the fixture
    // caught it.
    //
    // A run of sixteen zeros is the terminator. The number is a judgement:
    // sixteen hex digits are eight zero bytes, and the data here is the output
    // of a stream cipher, so eight zero bytes in a row has a probability of
    // 2^-64 of occurring inside real data. Whitespace does not break the run,
    // because the trailer is written as eight lines of sixty-four.
    for (at = start; at < length; ++at) {
      const uint8_t byte = in[at];

      if (gfnt_type1_space(byte)) {
        continue;
      }
      if (byte == '0') {
        if (zeros == 0) {
          // Where the run began, remembered rather than counted back to: walking
          // backwards over sixteen non-space characters lands one before the
          // first of them, which drops the last digit of the data and makes the
          // digit count odd. The fixture caught exactly that.
          zero_start = at;
        }
        zeros += 1;
        if (zeros >= 16) {
          end = zero_start;
          break;
        }
        continue;
      }
      zeros = 0;
      if (gfnt_type1_hex(byte) < 0) {
        end = at;
        break;
      }
    }

    for (at = start; at < end; ++at) {
      const int value = gfnt_type1_hex(in[at]);

      if (value < 0) {
        if (gfnt_type1_space(in[at])) {
          continue;
        }
        break;
      }
      if (high < 0) {
        high = value;
        continue;
      }
      if (!gfnt_buffer_byte(cipher, (uint8_t)((high << 4) | value))) {
        return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_TYPE1, 0,
            GFNT_GLYPH_NONE, "decoding a Type 1 program's hex");
      }
      high = -1;
    }
    if (high >= 0) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, end,
          GFNT_GLYPH_NONE, "an odd number of hex digits in the private portion");
    }
  }
  else if (!gfnt_buffer_add(cipher, in + start, length - start)) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE, "copying a Type 1 program's private portion");
  }
  if (cipher->length <= GFNT_TYPE1_EEXEC_SKIP) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, start,
        GFNT_GLYPH_NONE, "nothing after eexec but its four random bytes");
  }
  return GFNT_OK;
}

GFNT_Result gfnt_type1_derive(GFNT_Face * face, GFNT_Error * error) {
  GFNT_Reader reader;
  const uint8_t * in = NULL;
  size_t length;
  GFNT_Buffer clear = {NULL, 0, 0, NULL};
  GFNT_Buffer cipher = {NULL, 0, 0, NULL};
  GFNT_Blob * derived = NULL;
  GFNT_Result result;

  clear.allocator = face->allocator;
  cipher.allocator = face->allocator;

  // `bytes` rather than `blob`, so that a `.pfb.gz` works: the gzip layer has
  // already replaced the bytes by the time this runs, and reading the caller's
  // blob here would decipher the compressed file.
  result = gfnt_reader_init_blob(&reader, face->bytes, GFNT_TAG_TYPE1, error);
  if (result != GFNT_OK) {
    return result;
  }
  length = reader.length;
  if (length == 0 || gfnt_read_bytes(&reader, length, &in) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_FORMAT, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE, "too short to be a Type 1 font program");
  }

  result = in[0] == GFNT_TYPE1_PFB_MARKER
      ? gfnt_type1_split_pfb(in, length, &clear, &cipher, error)
      : gfnt_type1_split_ps(in, length, &clear, &cipher, error);
  if (result != GFNT_OK) {
    gfnt_buffer_free(&clear);
    gfnt_buffer_free(&cipher);
    return result;
  }

  gfnt_type1_decipher(cipher.data, cipher.length, GFNT_TYPE1_EEXEC_R,
      cipher.data);
  // The first four plaintext bytes are the random ones `eexec` opens with, and
  // the program starts after them.
  if (cipher.length > GFNT_TYPE1_EEXEC_SKIP) {
    cipher.length -= GFNT_TYPE1_EEXEC_SKIP;
    memmove(cipher.data, cipher.data + GFNT_TYPE1_EEXEC_SKIP, cipher.length);
  }
  else {
    cipher.length = 0;
  }
  if (cipher.length == 0) {
    gfnt_buffer_free(&clear);
    gfnt_buffer_free(&cipher);
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE, "a private portion with no plaintext in it");
  }
  // One program, cleartext then plaintext, which is what the font would have
  // been if nobody had encrypted half of it. Every offset this module records is
  // into these bytes.
  if (!gfnt_buffer_add(&clear, cipher.data, cipher.length)) {
    gfnt_buffer_free(&clear);
    gfnt_buffer_free(&cipher);
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE, "joining a Type 1 program's two halves");
  }
  gfnt_buffer_free(&cipher);

  // GFNT_BLOB_COPY, so the blob owns bytes of its own and this buffer can go.
  // The copy is one pass over a font-sized buffer, once per face, and the
  // alternative is a fourth ownership rule in the blob API for one caller.
  result = gfnt_blob_create_memory(clear.data, clear.length, GFNT_BLOB_COPY,
      &face->limits, face->allocator, &derived, error);
  gfnt_buffer_free(&clear);
  if (result != GFNT_OK) {
    return result;
  }

  // Through the helper, which frees whatever layer these bytes were derived from -
  // the caller's blob is not owned and is left alone, but a gzip layer is.
  gfnt_face_adopt_bytes(face, derived);
  return gfnt_sfnt_single_table_directory(face, GFNT_FLAVOUR_TYPE1,
      GFNT_TAG_TYPE1, error);
}

// ------------------------------------------------ the PostScript scanner

/** What a token is. Everything a Type 1 font program can contain. */
typedef enum {
  GFNT_T1_END = 0,     ///< No more tokens.
  GFNT_T1_NAME,        ///< `/FontName`, without the slash.
  GFNT_T1_NUMBER,      ///< An integer or a real.
  GFNT_T1_KEYWORD,     ///< A bare word: `def`, `dup`, `RD`, `array`.
  GFNT_T1_OPEN_ARRAY,  ///< `[`
  GFNT_T1_CLOSE_ARRAY, ///< `]`
  GFNT_T1_OPEN_PROC,   ///< `{`
  GFNT_T1_CLOSE_PROC,  ///< `}`
  GFNT_T1_STRING       ///< `(...)`, which a font uses for nothing this reads.
} GFNT_Type1TokenKind;

/** One token, as an offset into the program rather than a copy. */
typedef struct GFNT_Type1Token {
  GFNT_Type1TokenKind kind;
  size_t offset;       ///< Where the token's text starts.
  size_t length;       ///< How long it is.
  GFNT_F16Dot16 real;  ///< A number's value in 16.16.
  int64_t integer;     ///< The same as a whole number, when it is one.
  bool is_integer;     ///< Whether it had no fraction and no exponent.
} GFNT_Type1Token;

/**
 * Where the scanner is.
 *
 * A ::GFNT_Reader and an offset rather than a pointer and a length. The reader is
 * what bounds every byte this walks (design.md section 6): a tokeniser over
 * unknown input, with its own hand-written `at < length` tests, is precisely the
 * shape of code that reads one byte past the end - and a fuzzer finding that
 * would implicate this file rather than the reader.
 */
typedef struct GFNT_Type1Scan {
  GFNT_Reader reader;
  size_t length;
  size_t at;
} GFNT_Type1Scan;

/** The byte at @p at, through the reader, or false past the end. */
static bool gfnt_type1_peek(const GFNT_Type1Scan * scan, size_t at,
    uint8_t * out) {
  GFNT_Reader reader = scan->reader;

  if (gfnt_reader_seek(&reader, at) != GFNT_OK) {
    return false;
  }
  return gfnt_read_u8(&reader, out) == GFNT_OK;
}

/**
 * One token's text, as exactly its own extent.
 *
 * ::gfnt_read_bytes() is what checks that the extent is there, which is the same
 * way every other module in this library gets at a run of bytes it has already
 * bounded.
 */
static const uint8_t * gfnt_type1_text(const GFNT_Type1Scan * scan,
    size_t offset, size_t length) {
  GFNT_Reader reader = scan->reader;
  const uint8_t * bytes = NULL;

  if (gfnt_reader_seek(&reader, offset) != GFNT_OK
      || gfnt_read_bytes(&reader, length, &bytes) != GFNT_OK) {
    return NULL;
  }
  return bytes;
}

/** Whether a byte ends a token. */
static bool gfnt_type1_delimiter(uint8_t byte) {
  return gfnt_type1_space(byte) || byte == '/' || byte == '[' || byte == ']'
      || byte == '{' || byte == '}' || byte == '(' || byte == ')'
      || byte == '<' || byte == '>' || byte == '%';
}

/** A number's text as 16.16, and as a whole number when it is one. */
static void gfnt_type1_number(const uint8_t * text, size_t length,
    GFNT_Type1Token * token) {
  int64_t mantissa = 0;
  int digits = 0;
  int fraction_digits = 0;
  int exponent = 0;
  int exponent_sign = 1;
  bool negative = false;
  bool saw_point = false;
  bool in_exponent = false;
  int64_t value;
  int scale;
  size_t at = 0;

  if (at < length && (text[at] == '-' || text[at] == '+')) {
    negative = text[at] == '-';
    at += 1;
  }
  for (; at < length; ++at) {
    const uint8_t byte = text[at];

    if (byte >= '0' && byte <= '9') {
      if (in_exponent) {
        if (exponent < 1000) {
          exponent = exponent * 10 + (byte - '0');
        }
      }
      // Eighteen digits is what an int64 holds before the scaling below, and a
      // number in a font program that needs more is not a coordinate.
      else if (digits < 18) {
        mantissa = mantissa * 10 + (byte - '0');
        digits += 1;
        if (saw_point) {
          fraction_digits += 1;
        }
      }
      continue;
    }
    if (byte == '.') { saw_point = true; continue; }
    if (byte == 'e' || byte == 'E') { in_exponent = true; continue; }
    if (byte == '-' && in_exponent) { exponent_sign = -1; continue; }
  }

  token->is_integer = !saw_point && !in_exponent;
  token->integer = negative ? -mantissa : mantissa;
  scale = exponent_sign * exponent - fraction_digits;
  value = mantissa;
  if (scale >= 0) {
    int step;

    for (step = 0; step < scale && step < 18; ++step) {
      value = gfnt_saturate32((int64_t)gfnt_saturate32(value) * 10);
    }
    value = (int64_t)gfnt_saturate32(value) * GFNT_F16DOT16_ONE;
  }
  else {
    int64_t divisor = 1;
    int step;

    for (step = 0; step < -scale && step < 18; ++step) {
      divisor *= 10;
    }
    // Rounded half away from zero, which is section 5.2's one rule: the same
    // arithmetic the CFF DICT real uses, because these are the same numbers
    // written in a different alphabet.
    value = ((int64_t)gfnt_saturate32(value) * GFNT_F16DOT16_ONE * 2 + divisor)
        / (divisor * 2);
  }
  token->real = (GFNT_F16Dot16)gfnt_saturate32(negative ? -value : value);
}

/**
 * The next token, or false at the end.
 *
 * Comments and whitespace are skipped here so that no caller has to remember
 * to. A `%` inside a string is not a comment, which is why strings are scanned
 * rather than skipped byte by byte.
 */
static bool gfnt_type1_token(GFNT_Type1Scan * scan, GFNT_Type1Token * token) {
  size_t start;
  uint8_t byte = 0;
  uint8_t first;

  memset(token, 0, sizeof *token);
  for (;;) {
    while (gfnt_type1_peek(scan, scan->at, &byte) && gfnt_type1_space(byte)) {
      scan->at += 1;
    }
    if (gfnt_type1_peek(scan, scan->at, &byte) && byte == '%') {
      while (gfnt_type1_peek(scan, scan->at, &byte) && byte != '\n'
          && byte != '\r') {
        scan->at += 1;
      }
      continue;
    }
    break;
  }
  if (!gfnt_type1_peek(scan, scan->at, &byte)) {
    token->kind = GFNT_T1_END;
    token->offset = scan->at;
    return false;
  }

  start = scan->at;
  switch (byte) {
    case '[': scan->at += 1; token->kind = GFNT_T1_OPEN_ARRAY; break;
    case ']': scan->at += 1; token->kind = GFNT_T1_CLOSE_ARRAY; break;
    case '{': scan->at += 1; token->kind = GFNT_T1_OPEN_PROC; break;
    case '}': scan->at += 1; token->kind = GFNT_T1_CLOSE_PROC; break;
    case '(': {
      int depth = 1;

      scan->at += 1;
      while (depth > 0 && gfnt_type1_peek(scan, scan->at, &byte)) {
        if (byte == '\\') { scan->at += 1; }
        else if (byte == '(') { depth += 1; }
        else if (byte == ')') { depth -= 1; }
        scan->at += 1;
      }
      token->kind = GFNT_T1_STRING;
      break;
    }
    case '/': {
      scan->at += 1;
      start = scan->at;
      while (gfnt_type1_peek(scan, scan->at, &byte)
          && !gfnt_type1_delimiter(byte)) {
        scan->at += 1;
      }
      token->kind = GFNT_T1_NAME;
      token->offset = start;
      token->length = scan->at - start;
      return true;
    }
    default: {
      uint8_t second = 0;

      first = byte;
      while (gfnt_type1_peek(scan, scan->at, &byte)
          && !gfnt_type1_delimiter(byte)) {
        scan->at += 1;
      }
      // A sign or a point only begins a number when a digit follows it. `-|` is
      // an operator - the other spelling of `RD` - and reading it as a number
      // makes every charstring in a font that uses that spelling unreadable,
      // which is half of them.
      (void)gfnt_type1_peek(scan, start + 1, &second);
      if ((first >= '0' && first <= '9')
          || ((first == '-' || first == '+' || first == '.')
              && scan->at - start > 1 && second >= '0' && second <= '9')) {
        const uint8_t * text = gfnt_type1_text(scan, start, scan->at - start);

        token->kind = GFNT_T1_NUMBER;
        if (text) {
          gfnt_type1_number(text, scan->at - start, token);
        }
      }
      else {
        token->kind = GFNT_T1_KEYWORD;
      }
      break;
    }
  }
  token->offset = start;
  token->length = scan->at - start;
  // A delimiter that is its own token must still advance, or a `}` with nothing
  // after it would be returned for ever.
  if (token->length == 0 && token->kind != GFNT_T1_END) {
    scan->at += 1;
    token->length = 1;
  }
  return true;
}

/** Whether a token's text is exactly this word. */
static bool gfnt_type1_is(const GFNT_Type1Scan * scan,
    const GFNT_Type1Token * token, const char * word) {
  const size_t length = strlen(word);
  const uint8_t * text;

  if (token->length != length) {
    return false;
  }
  text = gfnt_type1_text(scan, token->offset, length);
  return text && memcmp(text, word, length) == 0;
}

/** Whether a keyword is one of the two spellings of `RD`. */
static bool gfnt_type1_is_rd(const GFNT_Type1Scan * scan,
    const GFNT_Type1Token * token) {
  // `RD` and `-|` are the same operator under two names, and which one a font
  // uses is the writer's habit: Adobe's tools emit `-|`, most others `RD`. A
  // reader that knew one would refuse half the fonts in the world.
  return token->kind == GFNT_T1_KEYWORD
      && (gfnt_type1_is(scan, token, "RD") || gfnt_type1_is(scan, token, "-|"));
}

// ------------------------------------------------------------ the parse

/** Decipher every charstring and subroutine into one arena. Defined below. */
static GFNT_Result gfnt_type1_decipher_all(GFNT_Type1 * type1,
    const uint8_t * base, const GFNT_Allocator * allocator,
    GFNT_Error * error);

/** An encoding entry before the glyph it names is known. */
typedef struct GFNT_Type1Pending {
  size_t offset;
  size_t length;
} GFNT_Type1Pending;

/** State the parse carries between its parts. */
typedef struct GFNT_Type1Parse {
  GFNT_Type1 * type1;
  GFNT_Type1Scan scan;
  const GFNT_Allocator * allocator;
  size_t max_glyphs;
  GFNT_Type1Pending pending[256]; ///< `/Encoding`, before names can be resolved.
  size_t glyph_capacity;
  size_t subr_capacity;
} GFNT_Type1Parse;

/** Room for one more glyph, doubling when the declared count was a lie. */
static bool gfnt_type1_room_glyph(GFNT_Type1Parse * state) {
  GFNT_Type1Glyph * grown;
  size_t capacity;

  if (state->type1->glyph_count < state->glyph_capacity) {
    return true;
  }
  // A `dict` count is a minimum in PostScript, not a bound - a font may declare
  // 13 and define 14 - so the count is where the array starts and not where it
  // has to stop. The cap is the caller's, applied by the caller.
  capacity = state->glyph_capacity ? state->glyph_capacity * 2 : 16;
  grown = state->allocator->realloc_fn(state->allocator->ctx,
      state->type1->glyphs, capacity * sizeof *grown);
  if (!grown) {
    return false;
  }
  state->type1->glyphs = grown;
  state->glyph_capacity = capacity;
  return true;
}

/** The same for subroutines, whose numbers index the array directly. */
static bool gfnt_type1_room_subr(GFNT_Type1Parse * state, size_t index) {
  GFNT_Type1Element * grown;
  size_t capacity = state->subr_capacity ? state->subr_capacity : 16;
  size_t at;

  if (index < state->subr_capacity) {
    return true;
  }
  while (capacity <= index) {
    capacity *= 2;
  }
  grown = state->allocator->realloc_fn(state->allocator->ctx,
      state->type1->subrs, capacity * sizeof *grown);
  if (!grown) {
    return false;
  }
  // Zeroed rather than left as whatever realloc had: a font may number its
  // subroutines sparsely, and a gap has to read as "not there" instead of as
  // whatever bytes were in that memory.
  for (at = state->subr_capacity; at < capacity; ++at) {
    grown[at].offset = 0;
    grown[at].length = 0;
  }
  state->type1->subrs = grown;
  state->subr_capacity = capacity;
  return true;
}

/**
 * The binary that follows `RD`: exactly one space, then the stated length.
 *
 * The one space is the format's, not whitespace to be skipped: the byte after it
 * may itself be a space, and a reader that skipped whitespace here would start
 * the charstring one byte late and decrypt rubbish.
 */
static GFNT_Result gfnt_type1_binary(GFNT_Type1Parse * state, int64_t length,
    GFNT_Type1Element * out, GFNT_Error * error) {
  GFNT_Type1Scan * scan = &state->scan;

  if (length < 0 || (uint64_t)length > (uint64_t)scan->length) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, scan->at,
        GFNT_GLYPH_NONE, "a binary length that is not a length");
  }
  if (scan->at >= scan->length) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, scan->at,
        GFNT_GLYPH_NONE, "an RD with nothing after it");
  }
  scan->at += 1;
  if ((size_t)length > scan->length - scan->at) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, scan->at,
        GFNT_GLYPH_NONE, "a binary string that runs past the end of the program");
  }
  out->offset = scan->at;
  out->length = (size_t)length;
  scan->at += (size_t)length;
  return GFNT_OK;
}

/** `/Encoding`: `StandardEncoding`, or `dup <code> /<name> put` repeated. */
static GFNT_Result gfnt_type1_encoding(GFNT_Type1Parse * state,
    GFNT_Error * error) {
  GFNT_Type1Scan * scan = &state->scan;
  GFNT_Type1Token token;

  if (!gfnt_type1_token(scan, &token)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, scan->at,
        GFNT_GLYPH_NONE, "an /Encoding with nothing after it");
  }
  if (token.kind == GFNT_T1_KEYWORD
      && gfnt_type1_is(scan, &token, "StandardEncoding")) {
    state->type1->encoding_is_standard = true;
    return GFNT_OK;
  }
  // Otherwise an array being filled in, and the entries are what matter. The
  // `0 1 255 {...} for` that clears it first is a procedure this skips.
  for (;;) {
    const size_t mark = scan->at;

    if (!gfnt_type1_token(scan, &token)) {
      return GFNT_OK;
    }
    // A `/name` ends the array as surely as `readonly` does, and has to: an
    // entry's own name is read by the `dup` case below, so a name reaching this
    // loop belongs to whatever comes next - `/CharStrings`, in a font that
    // forgot to write `readonly def`. Without this the encoding swallowed the
    // rest of the program and the font was refused for an entry that was never
    // an entry.
    if (token.kind == GFNT_T1_NAME) {
      scan->at = mark;
      return GFNT_OK;
    }
    if (token.kind == GFNT_T1_OPEN_PROC) {
      int depth = 1;

      while (depth > 0 && gfnt_type1_token(scan, &token)) {
        if (token.kind == GFNT_T1_OPEN_PROC) { depth += 1; }
        else if (token.kind == GFNT_T1_CLOSE_PROC) { depth -= 1; }
      }
      continue;
    }
    if (token.kind == GFNT_T1_KEYWORD) {
      if (gfnt_type1_is(scan, &token, "readonly")
          || gfnt_type1_is(scan, &token, "def")) {
        return GFNT_OK;
      }
      if (gfnt_type1_is(scan, &token, "dup")) {
        GFNT_Type1Token code;
        GFNT_Type1Token name;

        if (!gfnt_type1_token(scan, &code) || code.kind != GFNT_T1_NUMBER
            || !gfnt_type1_token(scan, &name) || name.kind != GFNT_T1_NAME) {
          return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1,
              scan->at, GFNT_GLYPH_NONE,
              "an /Encoding entry that is not `dup <code> /<name> put`");
        }
        if (code.integer >= 0 && code.integer < 256) {
          state->pending[code.integer].offset = name.offset;
          state->pending[code.integer].length = name.length;
        }
        // A code outside 0..255 is dropped rather than refused: the array is 256
        // entries by definition, so such an entry names no code this font has.
      }
    }
  }
}

/** `/Subrs <count> array`, then `dup <index> <length> RD <bytes> NP`. */
static GFNT_Result gfnt_type1_subrs(GFNT_Type1Parse * state,
    GFNT_Error * error) {
  GFNT_Type1Scan * scan = &state->scan;
  GFNT_Type1Token token;
  GFNT_Result result;

  if (!gfnt_type1_token(scan, &token) || token.kind != GFNT_T1_NUMBER) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, scan->at,
        GFNT_GLYPH_NONE, "a /Subrs without a count");
  }
  if (token.integer < 0 || (uint64_t)token.integer > GFNT_TYPE1_MAX_SUBRS) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_TYPE1, scan->at,
        GFNT_GLYPH_NONE, "more subroutines than this reader allocates for");
  }
  // `array`, which has to be stepped over rather than left for the loop below:
  // the loop treats anything that is not `dup` as the end of the entries, so
  // leaving it made every font's subroutine array read as empty - and that fails
  // nothing until a glyph calls one, which is one glyph in eight here.
  {
    const size_t mark = scan->at;

    if (gfnt_type1_token(scan, &token)
        && !(token.kind == GFNT_T1_KEYWORD
            && gfnt_type1_is(scan, &token, "array"))) {
      scan->at = mark;
    }
  }
  for (;;) {
    const size_t mark = scan->at;
    GFNT_Type1Token index;
    GFNT_Type1Token length;

    if (!gfnt_type1_token(scan, &token)) {
      return GFNT_OK;
    }
    if (token.kind != GFNT_T1_KEYWORD || !gfnt_type1_is(scan, &token, "dup")) {
      // Not another entry: hand the token back, because whatever it is belongs
      // to whoever reads next - `ND`, `noaccess`, or the `/CharStrings` that
      // follows.
      scan->at = mark;
      return GFNT_OK;
    }
    if (!gfnt_type1_token(scan, &index) || index.kind != GFNT_T1_NUMBER
        || !gfnt_type1_token(scan, &length) || length.kind != GFNT_T1_NUMBER
        || !gfnt_type1_token(scan, &token) || !gfnt_type1_is_rd(scan, &token)) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, scan->at,
          GFNT_GLYPH_NONE,
          "a /Subrs entry that is not `dup <index> <length> RD <bytes>`");
    }
    if (index.integer < 0 || (uint64_t)index.integer > GFNT_TYPE1_MAX_SUBRS) {
      return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_TYPE1, scan->at,
          GFNT_GLYPH_NONE, "a subroutine number this reader does not allocate for");
    }
    if (!gfnt_type1_room_subr(state, (size_t)index.integer)) {
      return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_TYPE1, 0,
          GFNT_GLYPH_NONE, "allocating the subroutine array");
    }
    result = gfnt_type1_binary(state, length.integer,
        &state->type1->subrs[index.integer], error);
    if (result != GFNT_OK) {
      return result;
    }
    if ((size_t)index.integer + 1 > state->type1->subr_count) {
      state->type1->subr_count = (size_t)index.integer + 1;
    }
    // `NP`, `|` or `put`, which stores the entry. It has to be consumed here for
    // the same reason `array` did above: the loop treats anything that is not
    // `dup` as the end of the entries, so leaving it made every font's second
    // and later subroutines disappear - and that fails nothing at all until a
    // charstring happens to call one of them.
    {
      const size_t after = scan->at;

      if (gfnt_type1_token(scan, &token) && token.kind != GFNT_T1_KEYWORD) {
        scan->at = after;
      }
    }
  }
}

/** `/CharStrings <count> dict dup begin`, then `/<name> <length> RD <bytes> ND`. */
static GFNT_Result gfnt_type1_charstrings(GFNT_Type1Parse * state,
    GFNT_Error * error) {
  GFNT_Type1Scan * scan = &state->scan;
  GFNT_Type1Token token;
  GFNT_Result result;

  // `<count> dict dup begin` - the words are skipped rather than checked one by
  // one, because which of them a writer emits varies and the entries are what
  // this reads.
  while (gfnt_type1_token(scan, &token)) {
    if (token.kind == GFNT_T1_KEYWORD && gfnt_type1_is(scan, &token, "begin")) {
      break;
    }
    if (token.kind == GFNT_T1_NAME) {
      // A `/name` before `begin` means there was no `begin`: put it back.
      scan->at = token.offset - 1;
      break;
    }
  }
  for (;;) {
    GFNT_Type1Token name;
    GFNT_Type1Token length;
    GFNT_Type1Glyph * glyph;

    if (!gfnt_type1_token(scan, &token)) {
      return GFNT_OK;
    }
    if (token.kind == GFNT_T1_KEYWORD) {
      if (gfnt_type1_is(scan, &token, "end")) {
        return GFNT_OK;
      }
      continue;
    }
    if (token.kind != GFNT_T1_NAME) {
      continue;
    }
    // Kept before the next two reads overwrite it, which is the whole reason
    // this is a separate variable: the glyph's name is the entry's key and the
    // `RD` token would otherwise be recorded as the name of every glyph.
    name = token;
    if (!gfnt_type1_token(scan, &length) || length.kind != GFNT_T1_NUMBER) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, scan->at,
          GFNT_GLYPH_NONE, "a /CharStrings entry with no length");
    }
    if (!gfnt_type1_token(scan, &token) || !gfnt_type1_is_rd(scan, &token)) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, scan->at,
          GFNT_GLYPH_NONE,
          "a /CharStrings entry that is not `/<name> <length> RD <bytes>`");
    }
    if (state->type1->glyph_count >= state->max_glyphs) {
      return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_TYPE1, scan->at,
          GFNT_GLYPH_NONE, "more glyphs than GFNT_Limits::max_glyphs");
    }
    if (!gfnt_type1_room_glyph(state)) {
      return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_TYPE1, 0,
          GFNT_GLYPH_NONE, "allocating the glyph array");
    }
    glyph = &state->type1->glyphs[state->type1->glyph_count];
    glyph->name.offset = name.offset;
    glyph->name.length = name.length;
    result = gfnt_type1_binary(state, length.integer, &glyph->program, error);
    if (result != GFNT_OK) {
      return result;
    }
    state->type1->glyph_count += 1;
  }
}

/** `/FontMatrix [ a b c d e f ]`. */
static GFNT_Result gfnt_type1_matrix(GFNT_Type1Parse * state,
    GFNT_Error * error) {
  GFNT_Type1Scan * scan = &state->scan;
  GFNT_Type1Token token;
  size_t index = 0;

  if (!gfnt_type1_token(scan, &token) || token.kind != GFNT_T1_OPEN_ARRAY) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, scan->at,
        GFNT_GLYPH_NONE, "a /FontMatrix that is not an array");
  }
  while (gfnt_type1_token(scan, &token)) {
    if (token.kind == GFNT_T1_CLOSE_ARRAY) {
      break;
    }
    if (token.kind != GFNT_T1_NUMBER) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1,
          token.offset, GFNT_GLYPH_NONE,
          "a /FontMatrix holding something that is not a number");
    }
    if (index < 6) {
      state->type1->font_matrix[index] = token.real;
    }
    index += 1;
  }
  if (index != 6) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, scan->at,
        GFNT_GLYPH_NONE, "a /FontMatrix that is not six numbers");
  }
  state->type1->font_matrix_stated = true;
  return GFNT_OK;
}

/**
 * `.notdef` is glyph 0, and the rest keep the order `/CharStrings` gave them.
 *
 * Type 1 is **name-keyed**: it has no glyph order of its own, and nothing in the
 * font says which glyph is number 3. An index is this library's own invention,
 * needed because every accessor above takes one - so the rule is written down
 * here and is the only place that decides it. `.notdef` leads because glyph 0
 * means "no glyph" everywhere else in this library and in every format it reads;
 * a Type 1 font that puts `.notdef` in the middle of its dictionary, as many do,
 * would otherwise have a real glyph at 0.
 */
static void gfnt_type1_order(GFNT_Type1 * type1, const uint8_t * base) {
  size_t at;

  for (at = 0; at < type1->glyph_count; ++at) {
    const GFNT_Type1Element * name = &type1->glyphs[at].name;

    if (name->length == strlen(".notdef")
        && memcmp(base + name->offset, ".notdef", name->length) == 0) {
      if (at != 0) {
        const GFNT_Type1Glyph first = type1->glyphs[0];

        type1->glyphs[0] = type1->glyphs[at];
        type1->glyphs[at] = first;
      }
      return;
    }
  }
}

uint32_t gfnt_type1_glyph_for_name(const GFNT_Type1 * type1,
    const uint8_t * base, const char * name, size_t length) {
  size_t at;

  if (!type1 || !base || !name) {
    return GFNT_GLYPH_NONE;
  }
  for (at = 0; at < type1->glyph_count; ++at) {
    const GFNT_Type1Element * stored = &type1->glyphs[at].name;

    if (stored->length == length
        && memcmp(base + stored->offset, name, length) == 0) {
      return (uint32_t)at;
    }
  }
  return GFNT_GLYPH_NONE;
}

GFNT_Result gfnt_type1_parse(const GFNT_Face * face, void * out,
    void * context,
    GFNT_Error * error) {
  // There is one of this table per face, so there is nothing to select; the
  // parameter is here because every memo parser shares one signature.
  (void)context;
  GFNT_Type1 * type1 = out;
  GFNT_Type1Parse state;
  GFNT_Reader table;
  const uint8_t * base = NULL;
  GFNT_Type1Token token;
  size_t at;
  GFNT_Result result;

  memset(type1, 0, sizeof *type1);
  // Four unless the font says otherwise, which is the format's default and not a
  // guess: a charstring's first four plaintext bytes are random padding.
  type1->len_iv = 4;
  for (at = 0; at < 256; ++at) {
    type1->encoding[at] = GFNT_GLYPH_NONE;
  }

  result = gfnt_face_table_reader(face, GFNT_TAG_TYPE1, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  type1->length = table.length;
  if (table.length == 0
      || gfnt_read_bytes(&table, table.length, &base) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE, "a Type 1 program with no bytes");
  }

  memset(&state, 0, sizeof state);
  state.type1 = type1;
  state.allocator = face->allocator;
  state.max_glyphs = face->limits.max_glyphs < GFNT_TYPE1_MAX_GLYPHS
      ? face->limits.max_glyphs : GFNT_TYPE1_MAX_GLYPHS;
  state.scan.reader = table;
  state.scan.length = table.length;

  while (gfnt_type1_token(&state.scan, &token)) {
    if (token.kind != GFNT_T1_NAME) {
      continue;
    }
    if (gfnt_type1_is(&state.scan, &token, "FontName")) {
      GFNT_Type1Token name;

      if (gfnt_type1_token(&state.scan, &name) && name.kind == GFNT_T1_NAME) {
        type1->font_name.offset = name.offset;
        type1->font_name.length = name.length;
      }
      continue;
    }
    // `/FontInfo`'s strings, each a `(...)` whose interior is the value. The
    // parentheses are the syntax and not the string, so they are dropped here
    // rather than by every reader of the field.
    {
      static const struct { const char * key; size_t offset; } strings[] = {
        {"FullName", offsetof(GFNT_Type1, full_name)},
        {"FamilyName", offsetof(GFNT_Type1, family_name)},
        {"Notice", offsetof(GFNT_Type1, notice)},
        {"version", offsetof(GFNT_Type1, version)},
        {"Weight", offsetof(GFNT_Type1, weight)},
      };
      size_t which;
      bool matched = false;

      for (which = 0; which < sizeof strings / sizeof *strings; ++which) {
        GFNT_Type1Token value;
        GFNT_Type1Element * field;

        if (!gfnt_type1_is(&state.scan, &token, strings[which].key)) {
          continue;
        }
        matched = true;
        if (!gfnt_type1_token(&state.scan, &value)
            || value.kind != GFNT_T1_STRING || value.length < 2) {
          break;
        }
        field = (GFNT_Type1Element *)((char *)type1 + strings[which].offset);
        field->offset = value.offset + 1;
        field->length = value.length - 2;
        break;
      }
      if (matched) {
        continue;
      }
    }
    if (gfnt_type1_is(&state.scan, &token, "FontMatrix")) {
      result = gfnt_type1_matrix(&state, error);
    }
    else if (gfnt_type1_is(&state.scan, &token, "lenIV")) {
      GFNT_Type1Token value;

      // `is_integer` matters: a token's `integer` field is its mantissa, so a
      // `/lenIV 1e1` read without this would set 1 rather than 10 and every
      // charstring in the font would be deciphered from the wrong offset -
      // silently, into plausible rubbish. A fractional or exponent-bearing lenIV
      // is not a number of bytes at all, so it is ignored and the default of 4
      // stands.
      if (gfnt_type1_token(&state.scan, &value)
          && value.kind == GFNT_T1_NUMBER && value.is_integer
          && value.integer >= 0 && value.integer <= 16) {
        type1->len_iv = (int32_t)value.integer;
      }
      continue;
    }
    else if (gfnt_type1_is(&state.scan, &token, "Encoding")) {
      result = gfnt_type1_encoding(&state, error);
    }
    else if (gfnt_type1_is(&state.scan, &token, "Subrs")) {
      result = gfnt_type1_subrs(&state, error);
    }
    else if (gfnt_type1_is(&state.scan, &token, "CharStrings")) {
      result = gfnt_type1_charstrings(&state, error);
    }
    else {
      continue;
    }
    if (result != GFNT_OK) {
      gfnt_type1_release(face->allocator, type1);
      return result;
    }
  }

  if (type1->glyph_count == 0) {
    gfnt_type1_release(face->allocator, type1);
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE,
        "a Type 1 program with no /CharStrings, so no glyphs at all");
  }
  gfnt_type1_order(type1, base);
  result = gfnt_type1_decipher_all(type1, base, face->allocator, error);
  if (result != GFNT_OK) {
    gfnt_type1_release(face->allocator, type1);
    return result;
  }

  // The encoding named glyphs, and the glyphs were not known until now: an
  // /Encoding always comes before the /CharStrings it refers to, because one is
  // in the cleartext and the other is behind eexec.
  for (at = 0; at < 256; ++at) {
    if (state.pending[at].length == 0) {
      continue;
    }
    type1->encoding[at] = gfnt_type1_glyph_for_name(type1, base,
        (const char *)(base + state.pending[at].offset),
        state.pending[at].length);
  }
  return GFNT_OK;
}

void gfnt_type1_release(const GFNT_Allocator * allocator, void * table) {
  GFNT_Type1 * type1 = table;

  if (!allocator || !type1) {
    return;
  }
  allocator->free_fn(allocator->ctx, type1->glyphs);
  allocator->free_fn(allocator->ctx, type1->subrs);
  allocator->free_fn(allocator->ctx, type1->plain);
  type1->glyphs = NULL;
  type1->subrs = NULL;
  type1->plain = NULL;
  type1->plain_length = 0;
  type1->glyph_count = 0;
  type1->subr_count = 0;
}

bool gfnt_type1_units_per_em(const GFNT_Type1 * type1, size_t * out_upem) {
  if (!type1) {
    return false;
  }
  return gfnt_matrix_units_per_em(type1->font_matrix, type1->font_matrix_stated,
      out_upem);
}

bool gfnt_type1_glyph_bound(const GFNT_Face * face, size_t * out_bound) {
  const GFNT_Type1 * type1 = NULL;

  if (!face || !out_bound || gfnt_sfnt_producer(face) != GFNT_PRODUCER_TYPE1) {
    return false;
  }
  if (gfnt_face_type1(face, &type1, NULL) != GFNT_OK) {
    return false;
  }
  *out_bound = type1->glyph_count;
  return true;
}

/**
 * Decipher every charstring and subroutine into one arena.
 *
 * Done once, at the end of the parse, because the interpreter's accessors
 * promise pointers that outlive a run and because the same subroutine is
 * otherwise deciphered again for every glyph that calls it.
 *
 * The elements' offsets are rewritten from the program text to the arena, which
 * is the one place in this module where an offset changes what it is relative
 * to - after this, a program element means the arena and a name means the text.
 */
static GFNT_Result gfnt_type1_decipher_all(GFNT_Type1 * type1,
    const uint8_t * base, const GFNT_Allocator * allocator,
    GFNT_Error * error) {
  const size_t skip = (size_t)(type1->len_iv < 0 ? 0 : type1->len_iv);
  size_t total = 0;
  size_t cursor = 0;
  size_t at;

  for (at = 0; at < type1->glyph_count; ++at) {
    const size_t length = type1->glyphs[at].program.length;

    if (length < skip) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1,
          type1->glyphs[at].program.offset, (uint32_t)at,
          "a charstring shorter than the random bytes /lenIV says open it");
    }
    if (!gcu_safe_add_size(total, length - skip, &total)) {
      return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_TYPE1, 0,
          GFNT_GLYPH_NONE, "more charstring bytes than a size_t holds");
    }
  }
  for (at = 0; at < type1->subr_count; ++at) {
    const size_t length = type1->subrs[at].length;

    if (length == 0) {
      continue;
    }
    if (length < skip) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1,
          type1->subrs[at].offset, GFNT_GLYPH_NONE,
          "a subroutine shorter than the random bytes /lenIV says open it");
    }
    if (!gcu_safe_add_size(total, length - skip, &total)) {
      return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_TYPE1, 0,
          GFNT_GLYPH_NONE, "more charstring bytes than a size_t holds");
    }
  }
  if (total == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE, "a program whose charstrings are all empty");
  }

  type1->plain = allocator->malloc_fn(allocator->ctx, total);
  if (!type1->plain) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE, "deciphering the charstrings");
  }
  type1->plain_length = total;

  for (at = 0; at < type1->glyph_count; ++at) {
    GFNT_Type1Element * element = &type1->glyphs[at].program;
    const size_t plain = element->length - skip;
    // The padding goes through the cipher without being kept, because the key it
    // leaves behind is what every byte after it depends on.
    const uint16_t key = gfnt_type1_decipher(base + element->offset, skip,
        GFNT_TYPE1_CHARSTRING_R, NULL);

    gfnt_type1_decipher(base + element->offset + skip, plain, key,
        type1->plain + cursor);
    element->offset = cursor;
    element->length = plain;
    cursor += plain;
  }
  for (at = 0; at < type1->subr_count; ++at) {
    GFNT_Type1Element * element = &type1->subrs[at];
    size_t plain;
    uint16_t key;

    if (element->length == 0) {
      continue;
    }
    plain = element->length - skip;
    key = gfnt_type1_decipher(base + element->offset, skip,
        GFNT_TYPE1_CHARSTRING_R, NULL);
    gfnt_type1_decipher(base + element->offset + skip, plain, key,
        type1->plain + cursor);
    element->offset = cursor;
    element->length = plain;
    cursor += plain;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_type1_program(const GFNT_Type1 * type1, uint32_t glyph,
    const uint8_t ** out_bytes, size_t * out_length) {
  if (!type1 || !out_bytes || !out_length || glyph >= type1->glyph_count) {
    return GFNT_ERR_INVALID;
  }
  *out_bytes = type1->plain + type1->glyphs[glyph].program.offset;
  *out_length = type1->glyphs[glyph].program.length;
  return GFNT_OK;
}

GFNT_Result gfnt_type1_subr(const GFNT_Type1 * type1, size_t index,
    const uint8_t ** out_bytes, size_t * out_length) {
  if (!type1 || !out_bytes || !out_length || index >= type1->subr_count) {
    return GFNT_ERR_CORRUPT;
  }
  if (type1->subrs[index].length == 0) {
    // A number the font never defined. Type 1 subroutine numbers are not biased
    // and are used directly, so a gap is reachable from any charstring that
    // names one.
    return GFNT_ERR_CORRUPT;
  }
  *out_bytes = type1->plain + type1->subrs[index].offset;
  *out_length = type1->subrs[index].length;
  return GFNT_OK;
}

GFNT_Result gfnt_face_type1(const GFNT_Face * face, const GFNT_Type1 ** out_type1,
    GFNT_Error * error) {
  GFNT_Face * owner = (GFNT_Face *)face;
  GFNT_Type1 scratch;
  GFNT_Result result;

  if (!face || !out_type1) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_table_cached(face, &owner->type1_state, &owner->type1, &scratch,
      sizeof scratch, gfnt_type1_parse, NULL,
      gfnt_type1_release, error);
  if (result == GFNT_OK) {
    *out_type1 = &owner->type1;
  }
  return result;
}

// ------------------------------------------------------------ drawing

/** What the subroutine accessor needs: the program. */
typedef struct GFNT_Type1Run {
  const GFNT_Type1 * type1;
  const uint8_t * text; ///< The program text, for resolving a seac's names.
} GFNT_Type1Run;

/** ::GFNT_CharstringSubrs::at over `/Subrs`. */
static GFNT_Result gfnt_type1_subr_at(void * user, uint32_t index,
    const uint8_t ** out_bytes, size_t * out_length) {
  const GFNT_Type1Run * run = user;

  return gfnt_type1_subr(run->type1, index, out_bytes, out_length);
}

/**
 * ::GFNT_CharstringContext::standard_code over the Standard Encoding.
 *
 * `seac` names its two components by Standard Encoding code whatever the font's
 * own `/Encoding` says, so the code is turned into a *name* through the standard
 * table and the name is looked up in `/CharStrings`. Going through the font's own
 * encoding instead would build the accent out of whichever glyph that font
 * happens to put at code 193, which is how an accented character comes out
 * looking like a different letter entirely.
 */
static GFNT_Result gfnt_type1_standard_code(void * user, uint8_t code,
    const uint8_t ** out_bytes, size_t * out_length) {
  const GFNT_Type1Run * run = user;
  const char * name = gfnt_cff_standard_encoding_name(code);
  uint32_t glyph;

  if (!name) {
    return GFNT_ERR_CORRUPT;
  }
  glyph = gfnt_type1_glyph_for_name(run->type1, run->text, name, strlen(name));
  if (glyph == GFNT_GLYPH_NONE) {
    return GFNT_ERR_CORRUPT;
  }
  return gfnt_type1_program(run->type1, glyph, out_bytes, out_length);
}

GFNT_Result gfnt_type1_load(const GFNT_Face * face, uint32_t glyph,
    GFNT_Outline * outline, GFNT_CharstringMetrics * out_metrics,
    GFNT_Error * error) {
  const GFNT_Type1 * type1 = NULL;
  GFNT_Reader table;
  const uint8_t * text = NULL;
  const uint8_t * program = NULL;
  size_t length = 0;
  size_t upem = 0;
  GFNT_Type1Run run;
  GFNT_CharstringContext context;
  GFNT_CharstringMetrics metrics;
  GFNT_Result result;

  result = gfnt_face_type1(face, &type1, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (glyph >= type1->glyph_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_TYPE1, 0, glyph,
        "this glyph index is past the face's glyph count");
  }
  // The same refusal a bare CFF gets, for the same reason: with no `head` the
  // FontMatrix is the only statement of the em, and one that does not reduce to
  // an em would have to be applied - which would put these coordinates in a
  // space no reference pen reports.
  if (!gfnt_type1_units_per_em(type1, &upem)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_TYPE1, 0, glyph,
        "a Type 1 FontMatrix that does not reduce to an em, so this font's "
        "charstring coordinates are in a space this library would have to "
        "transform them out of rather than report");
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_TYPE1, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_bytes(&table, table.length, &text) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, 0, glyph,
        "a Type 1 program whose bytes went away");
  }
  result = gfnt_type1_program(type1, glyph, &program, &length);
  if (result != GFNT_OK) {
    return result;
  }

  run.type1 = type1;
  run.text = text;
  memset(&context, 0, sizeof context);
  context.local.at = gfnt_type1_subr_at;
  context.local.user = &run;
  context.local.count = type1->subr_count;
  context.standard_code = gfnt_type1_standard_code;
  context.standard_user = &run;
  context.limits = &face->limits;
  // No nominalWidthX and no defaultWidthX: those are Type 2's, and a Type 1
  // charstring states its advance outright in `hsbw`.

  result = gfnt_charstring_run(GFNT_CHARSTRING_TYPE1, program, length, &context,
      outline, &metrics, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (out_metrics) {
    *out_metrics = metrics;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_type1_metrics(const GFNT_Face * face, uint32_t glyph,
    GFNT_CharstringMetrics * out_metrics, GFNT_Error * error) {
  GFNT_Outline * outline = NULL;
  GFNT_Result result;

  if (!out_metrics) {
    return GFNT_ERR_INVALID;
  }
  // The advance is stated by `hsbw`, which is the first operator of the
  // charstring - but the only way to reach it is to run the program, because a
  // charstring that calls a subroutine before its `hsbw` is legal and some
  // fonts do it. An outline is built and thrown away, which is what the CFF path
  // does for the same reason.
  result = gfnt_outline_create(face->allocator, &outline, error);
  if (result != GFNT_OK) {
    return result;
  }
  gfnt_outline_set_limits(outline, &face->limits);
  result = gfnt_type1_load(face, glyph, outline, out_metrics, error);
  gfnt_outline_destroy(outline);
  return result;
}

GFNT_Result gfnt_type1_is_composite(const GFNT_Face * face, uint32_t glyph,
    bool * out_composite, GFNT_Error * error) {
  GFNT_CharstringMetrics metrics;
  GFNT_Result result;

  if (!out_composite) {
    return GFNT_ERR_INVALID;
  }
  // Running the charstring is the only way to answer: `seac` can be reached
  // through a subroutine, and scanning the bytes for the operator would miss
  // that and would also find it inside an operand that happens to spell it.
  result = gfnt_type1_metrics(face, glyph, &metrics, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_composite = metrics.seac;
  return GFNT_OK;
}

GFNT_Result gfnt_type1_name_at(const GFNT_Face * face, uint32_t glyph,
    char * out, size_t capacity, size_t * out_length, GFNT_Error * error) {
  const GFNT_Type1 * type1 = NULL;
  GFNT_Reader table;
  const uint8_t * text = NULL;
  const GFNT_Type1Element * name;
  GFNT_Result result;

  if (!out_length) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_face_type1(face, &type1, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (glyph >= type1->glyph_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_TYPE1, 0, glyph,
        "this glyph index is past the face's glyph count");
  }
  name = &type1->glyphs[glyph].name;
  *out_length = name->length;
  if (!out) {
    return GFNT_OK;
  }
  if (name->length + 1 > capacity) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_TYPE1, 0, glyph,
        "a glyph name longer than the caller's buffer");
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_TYPE1, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_bytes(&table, table.length, &text) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, 0, glyph,
        "a Type 1 program whose bytes went away");
  }
  memcpy(out, text + name->offset, name->length);
  out[name->length] = '\0';
  return GFNT_OK;
}

uint32_t gfnt_type1_glyph_for_code(const GFNT_Type1 * type1, uint8_t code) {
  if (!type1) {
    return GFNT_GLYPH_NONE;
  }
  return type1->encoding[code];
}
