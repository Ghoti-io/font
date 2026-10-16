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
 * A hinting context for a face at a size: the `maxp` sizes, the `cvt ` values
 * and the two font programs, gathered from the face.
 */

#include "vm_internal.h"

#include <ghoti.io/font/cvt.h>
#include <ghoti.io/font/metrics.h>
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"

/* The offsets of the TrueType fields in a version 1.0 `maxp`. */
#define GFNT_MAXP_TWILIGHT 16u
#define GFNT_MAXP_STORAGE 18u
#define GFNT_MAXP_FUNCTIONS 20u
#define GFNT_MAXP_IDEFS 22u
#define GFNT_MAXP_STACK 24u
#define GFNT_MAXP_V1_BYTES 32u

static GFNT_Result gfnt_hint_read_shape(const GFNT_Face * face,
    GFNT_HintShape * shape, GFNT_Error * error) {
  GFNT_Reader reader;
  uint32_t version = 0;
  uint16_t value = 0;
  GFNT_Result result = gfnt_face_table_reader(face, GFNT_TAG('m', 'a', 'x', 'p'), &reader, error);

  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_read_u32(&reader, &version);
  if (result != GFNT_OK) {
    return result;
  }
  if (version != 0x00010000u) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG('m', 'a', 'x', 'p'), 0, 0,
        "only a maxp version 1.0 carries the sizes a TrueType program needs");
  }
  if (!gfnt_reader_has(&reader, GFNT_MAXP_V1_BYTES - 4u)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG('m', 'a', 'x', 'p'), 0, 0,
        "a version 1.0 maxp is 32 bytes");
  }
  result = gfnt_reader_u16_at(&reader, GFNT_MAXP_TWILIGHT, &value);
  if (result != GFNT_OK) {
    return result;
  }
  shape->twilight = value;
  result = gfnt_reader_u16_at(&reader, GFNT_MAXP_STORAGE, &value);
  if (result != GFNT_OK) {
    return result;
  }
  shape->storage = value;
  result = gfnt_reader_u16_at(&reader, GFNT_MAXP_FUNCTIONS, &value);
  if (result != GFNT_OK) {
    return result;
  }
  shape->functions = value;
  result = gfnt_reader_u16_at(&reader, GFNT_MAXP_IDEFS, &value);
  if (result != GFNT_OK) {
    return result;
  }
  shape->idefs = value;
  result = gfnt_reader_u16_at(&reader, GFNT_MAXP_STACK, &value);
  if (result != GFNT_OK) {
    return result;
  }
  shape->stack = value;
  return GFNT_OK;
}

GFNT_Result gfnt_hint_ctx_from_face(const GFNT_Face * face, uint32_t ppem,
    int version, bool grayscale, const GFNT_HintLimits * limits,
    GFNT_HintContext ** out, GFNT_Error * error) {
  GFNT_HintParams params;
  GFNT_HintShape shape;
  const uint8_t * fpgm = NULL;
  const uint8_t * prep = NULL;
  size_t fpgm_length = 0;
  size_t prep_length = 0;
  size_t cvt_count = 0;
  int32_t * cvt = NULL;
  uint16_t units = 0;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out) {
    return GFNT_ERR_INVALID;
  }
  if (gfnt_sfnt_producer(face) != GFNT_PRODUCER_GLYF) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, 0,
        "only a glyf face has TrueType programs");
  }
  result = gfnt_face_units_per_em(face, &units, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_hint_read_shape(face, &shape, error);
  if (result != GFNT_OK) {
    return result;
  }
  /* A missing table is an empty program, which is what a font without hinting is. */
  if (gfnt_face_table_data(face, GFNT_TAG('f', 'p', 'g', 'm'), &fpgm, &fpgm_length)
      != GFNT_OK) {
    fpgm = NULL;
    fpgm_length = 0;
  }
  if (gfnt_face_table_data(face, GFNT_TAG('p', 'r', 'e', 'p'), &prep, &prep_length)
      != GFNT_OK) {
    prep = NULL;
    prep_length = 0;
  }
  result = gfnt_face_cvt_count(face, &cvt_count, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (cvt_count) {
    int32_t * values = face->allocator->calloc_fn(face->allocator->ctx, cvt_count,
        sizeof *values);

    if (!values) {
      return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG('c', 'v', 't', ' '), 0, 0,
          "no memory for the control values");
    }
    result = gfnt_face_cvt_values(face, NULL, values, cvt_count, error);
    if (result != GFNT_OK) {
      face->allocator->free_fn(face->allocator->ctx, values);
      return result;
    }
    cvt = values;
  }
  params.ppem = ppem;
  params.units_per_em = units;
  params.version = version;
  params.grayscale = grayscale;
  result = gfnt_hint_ctx_create(face->allocator, limits, &params, &shape, fpgm,
      fpgm_length, prep, prep_length, cvt, cvt_count, out);
  if (cvt) {
    face->allocator->free_fn(face->allocator->ctx, cvt);
  }
  if (result != GFNT_OK) {
    return gfnt_error_set(error, result, 0, 0, 0,
        "could not make a hinting context for this face and size");
  }
  return GFNT_OK;
}
