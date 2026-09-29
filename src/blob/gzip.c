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
 * Undoing gzip, through `compress`.
 *
 * documentation/design.md section 7.1. The whole of this library's use of
 * `ghoti.io-compress` is in this file, which is deliberate: one call site is one
 * place to read when the dependency's contract changes, and the rest of the
 * library keeps knowing nothing about compression.
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/font/macros.h>
#include "gzip.h"

/** RFC 1952's identification bytes, and the one compression method it defines. */
#define GFNT_GZIP_ID1 0x1Fu
#define GFNT_GZIP_ID2 0x8Bu
#define GFNT_GZIP_DEFLATE 0x08u

/** The tag a diagnostic from this layer carries: `GZIP`. */
#define GFNT_TAG_GZIP GFNT_TAG('G', 'Z', 'I', 'P')

bool gfnt_gzip_looks_like(const GFNT_Reader * blob) {
  static const uint8_t magic[3] = {
    GFNT_GZIP_ID1, GFNT_GZIP_ID2, GFNT_GZIP_DEFLATE
  };

  for (size_t i = 0; i < sizeof magic; ++i) {
    uint8_t byte = 0;

    if (gfnt_reader_u8_at(blob, i, &byte) != GFNT_OK || byte != magic[i]) {
      return false;
    }
  }
  return true;
}

/**
 * What one of `compress`'s statuses means here.
 *
 * Mapped rather than passed through, because the two vocabularies are different
 * sizes and the caller of a font library should not have to learn a second one.
 * `GCOMP_ERR_UNSUPPORTED` becomes ::GFNT_ERR_CORRUPT rather than
 * ::GFNT_ERR_UNSUPPORTED: from here it means a gzip member using something RFC
 * 1952 does not define, which is a broken file, where this library's UNSUPPORTED
 * means a feature it has chosen not to implement.
 */
static GFNT_Result gfnt_gzip_result(gcomp_status_t status) {
  // No `GCOMP_OK` arm: this is only called on a failure, so one would be a line no
  // input reaches. If it ever did arrive it falls to the default and becomes
  // ::GFNT_ERR_INTERNAL, which is the right answer - a library that returns OK
  // through a failure path is a bug in one of these two and not in the font.
  switch (status) {
    case GCOMP_ERR_MEMORY:
      return GFNT_ERR_OOM;
    case GCOMP_ERR_LIMIT:
      return GFNT_ERR_LIMIT;
    case GCOMP_ERR_CORRUPT:
    case GCOMP_ERR_UNSUPPORTED:
      return GFNT_ERR_CORRUPT;
    case GCOMP_ERR_INVALID_ARG:
    case GCOMP_ERR_INTERNAL:
    case GCOMP_ERR_IO:
    default:
      // An argument this file got wrong, or the library's own invariant: either
      // way it is this library's bug and not the font's, and M-numbered honesty
      // says so rather than blaming the file.
      return GFNT_ERR_INTERNAL;
  }
}

GFNT_Result gfnt_gzip_inflate(const GFNT_Blob * in, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob,
    GFNT_Error * error) {
  GFNT_Reader reader;
  const uint8_t * bytes = NULL;
  gcomp_options_t * options = NULL;
  gcomp_status_t status;
  void * inflated = NULL;
  size_t length = 0;
  GFNT_Result result;

  if (!in || !limits || !out_blob) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_GZIP, 0,
        GFNT_GLYPH_NONE, "no input, no limits, or nowhere to put the blob");
  }
  result = gfnt_reader_init_blob(&reader, in, GFNT_TAG_GZIP, error);
  if (result != GFNT_OK) {
    return result;
  }
  // The whole member at once. A streaming inflate would let this library read a
  // font larger than memory, which is not a thing it does anywhere else: a face
  // is random-access over bytes that are all there (design.md section 5.1).
  if (reader.length == 0
      || gfnt_read_bytes(&reader, reader.length, &bytes) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GZIP, 0,
        GFNT_GLYPH_NONE, "an empty file cannot be a gzip member");
  }

  if (gcomp_options_create(&options) != GCOMP_OK) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_GZIP, 0,
        GFNT_GLYPH_NONE, "allocating the options for an inflate");
  }
  // The output ceiling is the same one a font file read from disk is held to,
  // because what comes out of here *is* the font file from here on. Without it
  // the decoder's own 512 MiB default would apply, which is a number this library
  // has not chosen and a caller cannot see.
  if (gcomp_options_set_uint64(options, "limits.max_output_bytes",
          (uint64_t)limits->max_blob_bytes) != GCOMP_OK
      // `gzip` writes concatenated members and `cat a.gz b.gz` is a legal file.
      // Nobody ships a font that way; refusing one would be inventing a rule.
      || gcomp_options_set_bool(options, "gzip.concat", 1) != GCOMP_OK) {
    gcomp_options_destroy(options);
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, GFNT_TAG_GZIP, 0,
        GFNT_GLYPH_NONE, "the compression library refused an option this "
        "library sets");
  }

  status = gcomp_decode_alloc(NULL, "gzip", options, bytes, reader.length,
      &inflated, &length);
  gcomp_options_destroy(options);
  if (status != GCOMP_OK) {
    // The message is the compression library's own, which names the stage the
    // member stopped at - far more use than anything this file could say about
    // bytes it did not parse.
    return gfnt_error_set(error, gfnt_gzip_result(status), GFNT_TAG_GZIP, 0,
        GFNT_GLYPH_NONE, gcomp_status_to_string(status));
  }
  if (length == 0) {
    gcomp_buffer_free(NULL, inflated);
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GZIP, 0,
        GFNT_GLYPH_NONE, "a gzip member holding no bytes at all");
  }

  // GFNT_BLOB_COPY, and the inflated buffer is then released. It is one pass over
  // a font-sized buffer once per face, and the alternative is a blob that frees
  // its bytes through `compress`'s allocator rather than this library's - a fourth
  // ownership rule in the blob API, for one caller. The Type 1 derivation made the
  // same trade for the same reason.
  result = gfnt_blob_create_memory(inflated, length, GFNT_BLOB_COPY, limits,
      allocator, out_blob, error);
  gcomp_buffer_free(NULL, inflated);
  return result;
}
