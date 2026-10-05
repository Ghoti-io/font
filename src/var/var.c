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
 * `fvar` and `avar`: the axes of a design space, and how a user's coordinates
 * become the normalised ones `gvar` takes.
 *
 * documentation/design.md section 7.7. `variation.h` says what the two
 * coordinate systems are and why the conversion is a function of its own.
 *
 * Reference: OpenType Specification 1.9, "fvar - Font Variations Table" and
 * "avar - Axis Variations Table".
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/macros.h>
#include <stdio.h>
#include <string.h>
#include "../core/fixed.h"
#include "../tables/tables.h"
#include "var.h"

GFNT_Result gfnt_fvar_parse(const GFNT_Face * face, void * out, void * context,
    GFNT_Error * error) {
  (void)context;
  GFNT_Fvar * fvar = out;
  GFNT_Reader reader;
  GFNT_Result result;
  uint16_t major = 0;
  uint16_t minor = 0;
  uint16_t axes_offset = 0;
  uint16_t reserved = 0;
  uint16_t axis_count = 0;
  uint16_t axis_size = 0;
  uint16_t instance_count = 0;
  uint16_t instance_size = 0;
  size_t wanted_instance_size;
  size_t axes_bytes = 0;
  size_t instances_bytes = 0;
  size_t instance_offset = 0;

  // Zeroed first: gfnt_table_cached() publishes its scratch whatever comes back,
  // so a refusal has to leave something the face can free.
  memset(fvar, 0, sizeof *fvar);
  result = gfnt_face_table_reader(face, GFNT_TAG_FVAR, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_u16(&reader, &major) != GFNT_OK
      || gfnt_read_u16(&reader, &minor) != GFNT_OK
      || gfnt_read_u16(&reader, &axes_offset) != GFNT_OK
      || gfnt_read_u16(&reader, &reserved) != GFNT_OK
      || gfnt_read_u16(&reader, &axis_count) != GFNT_OK
      || gfnt_read_u16(&reader, &axis_size) != GFNT_OK
      || gfnt_read_u16(&reader, &instance_count) != GFNT_OK
      || gfnt_read_u16(&reader, &instance_size) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_FVAR, 0,
        GFNT_GLYPH_NONE, "an fvar shorter than its own header");
  }
  (void)reserved;
  if (major != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_FVAR, 0,
        GFNT_GLYPH_NONE, "an fvar version other than 1.x, which is the only one "
        "the format has");
  }
  (void)minor;
  if (axis_count > face->limits.max_axes) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_FVAR, 8,
        GFNT_GLYPH_NONE, "more axes than GFNT_Limits::max_axes allows");
  }
  if (axis_size != GFNT_FVAR_AXIS_BYTES) {
    // The format fixes it at twenty, and a record of another size would be read
    // at a stride that is not the file's: every axis after the first would be
    // made of two neighbours.
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_FVAR, 10,
        GFNT_GLYPH_NONE, "an fvar axis record is not twenty bytes");
  }
  // Four bytes of header per instance, a Fixed per axis, and then an optional
  // PostScript name ID: the two sizes the format allows and no others.
  wanted_instance_size = 4u + 4u * (size_t)axis_count;
  if (instance_size != wanted_instance_size
      && instance_size != wanted_instance_size + 2u) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_FVAR, 14,
        GFNT_GLYPH_NONE, "an fvar instance record's size does not match the "
        "axis count");
  }
  axes_bytes = (size_t)axis_count * GFNT_FVAR_AXIS_BYTES;
  instances_bytes = (size_t)instance_count * instance_size;
  // The instance array follows the axes, which is how the format lays it out
  // and why it has no offset of its own. Both extents are checked against the
  // table *before* anything is allocated: the counts are 16-bit but the sizes
  // multiply, and a table that is shorter than its own counts is the font
  // contradicting itself.
  if (axes_bytes > reader.length || (size_t)axes_offset > reader.length - axes_bytes
      || instances_bytes > reader.length
      || (size_t)axes_offset + axes_bytes > reader.length - instances_bytes) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_FVAR, axes_offset,
        GFNT_GLYPH_NONE, "an fvar's axes and instances run past the table");
  }

  if (axis_count > 0) {
    fvar->axes = face->allocator->calloc_fn(face->allocator->ctx, axis_count,
        sizeof *fvar->axes);
    if (!fvar->axes) {
      return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_FVAR, 0,
          GFNT_GLYPH_NONE, "no memory for the fvar axis list");
    }
    fvar->axis_count = axis_count;
  }
  result = gfnt_reader_seek(&reader, axes_offset);
  if (result != GFNT_OK) {
    return result;
  }
  for (size_t i = 0; i < axis_count; ++i) {
    GFNT_Axis * axis = &fvar->axes[i];
    size_t at = gfnt_reader_tell(&reader);

    if (gfnt_read_tag(&reader, &axis->tag) != GFNT_OK
        || gfnt_read_fixed(&reader, &axis->min) != GFNT_OK
        || gfnt_read_fixed(&reader, &axis->def) != GFNT_OK
        || gfnt_read_fixed(&reader, &axis->max) != GFNT_OK
        || gfnt_read_u16(&reader, &axis->flags) != GFNT_OK
        || gfnt_read_u16(&reader, &axis->name_id) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_FVAR, at,
          GFNT_GLYPH_NONE, "an fvar axis ends past the table");
    }
    if (axis->min > axis->def || axis->def > axis->max) {
      // Normalising around a default outside its own range divides by a
      // negative span. The font said something that is not a design space, and
      // no rule this library could pick would be the font's.
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_FVAR, at + 4,
          GFNT_GLYPH_NONE, "an fvar axis's default lies outside its own range");
    }
  }

  instance_offset = (size_t)axes_offset + axes_bytes;
  if (instance_count > 0) {
    size_t values = 0;

    if (!gcu_safe_mul_size(instance_count, axis_count, &values)) {
      return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_FVAR, 12,
          GFNT_GLYPH_NONE, "an fvar's instance coordinates overflow");
    }
    fvar->instances = face->allocator->calloc_fn(face->allocator->ctx,
        instance_count, sizeof *fvar->instances);
    fvar->coordinates = face->allocator->calloc_fn(face->allocator->ctx,
        values ? values : 1u, sizeof *fvar->coordinates);
    if (!fvar->instances || !fvar->coordinates) {
      return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_FVAR, 0,
          GFNT_GLYPH_NONE, "no memory for the fvar instance list");
    }
    fvar->instance_count = instance_count;
    result = gfnt_reader_seek(&reader, instance_offset);
    if (result != GFNT_OK) {
      return result;
    }
    for (size_t i = 0; i < instance_count; ++i) {
      GFNT_FvarInstance * instance = &fvar->instances[i];

      if (gfnt_read_u16(&reader, &instance->name_id) != GFNT_OK
          || gfnt_read_u16(&reader, &instance->flags) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_FVAR,
            instance_offset + i * instance_size, GFNT_GLYPH_NONE,
            "an fvar instance ends past the table");
      }
      for (size_t axis = 0; axis < axis_count; ++axis) {
        if (gfnt_read_fixed(&reader,
                &fvar->coordinates[i * axis_count + axis]) != GFNT_OK) {
          return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_FVAR,
              instance_offset + i * instance_size, GFNT_GLYPH_NONE,
              "an fvar instance ends past the table");
        }
      }
      instance->postscript_name_id = GFNT_INSTANCE_NO_POSTSCRIPT_NAME;
      if (instance_size == wanted_instance_size + 2u
          && gfnt_read_u16(&reader, &instance->postscript_name_id) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_FVAR,
            instance_offset + i * instance_size, GFNT_GLYPH_NONE,
            "an fvar instance ends past the table");
      }
    }
  }
  return GFNT_OK;
}

void gfnt_fvar_release(const GFNT_Allocator * allocator, void * table) {
  GFNT_Fvar * fvar = table;

  if (!allocator || !fvar) {
    return;
  }
  allocator->free_fn(allocator->ctx, fvar->axes);
  allocator->free_fn(allocator->ctx, fvar->instances);
  allocator->free_fn(allocator->ctx, fvar->coordinates);
  memset(fvar, 0, sizeof *fvar);
}

GFNT_Result gfnt_avar_parse(const GFNT_Face * face, void * out, void * context,
    GFNT_Error * error) {
  (void)context;
  GFNT_Avar * avar = out;
  const GFNT_Fvar * fvar = NULL;
  GFNT_Reader reader;
  GFNT_Result result;
  uint16_t major = 0;
  uint16_t minor = 0;
  uint16_t reserved = 0;
  uint16_t axis_count = 0;
  size_t total = 0;
  size_t filled = 0;

  memset(avar, 0, sizeof *avar);
  // The axis count it has to match is fvar's, so a face whose fvar is refused
  // has an avar nobody can interpret either.
  result = gfnt_face_fvar(face, &fvar, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_AVAR, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_u16(&reader, &major) != GFNT_OK
      || gfnt_read_u16(&reader, &minor) != GFNT_OK
      || gfnt_read_u16(&reader, &reserved) != GFNT_OK
      || gfnt_read_u16(&reader, &axis_count) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_AVAR, 0,
        GFNT_GLYPH_NONE, "an avar shorter than its own header");
  }
  (void)minor;
  (void)reserved;
  if (major != 1) {
    // Version 2 adds a variation store that re-maps an axis as a function of the
    // others. Reading it as version 1 would apply the segment maps and silently
    // drop that, which is a different design space and no error.
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_AVAR, 0,
        GFNT_GLYPH_NONE, "an avar version other than 1, which this library does "
        "not read");
  }
  if (axis_count != fvar->axis_count) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_AVAR, 6,
        GFNT_GLYPH_NONE, "an avar's axis count is not the fvar's");
  }
  if (axis_count == 0) {
    return GFNT_OK;
  }
  avar->first = face->allocator->calloc_fn(face->allocator->ctx, axis_count,
      sizeof *avar->first);
  avar->count = face->allocator->calloc_fn(face->allocator->ctx, axis_count,
      sizeof *avar->count);
  if (!avar->first || !avar->count) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_AVAR, 0,
        GFNT_GLYPH_NONE, "no memory for the avar segment maps");
  }
  avar->axis_count = axis_count;

  // Two passes, the second only after the first has proved the pairs fit: the
  // count of each map is read before its pairs, so the total is not known until
  // every map has been walked, and sizing an array from a count that has not been
  // checked against the table is how a 16-bit field becomes a large allocation.
  {
    GFNT_Reader scan = reader;

    for (size_t i = 0; i < axis_count; ++i) {
      uint16_t pairs = 0;

      if (gfnt_read_u16(&scan, &pairs) != GFNT_OK
          || gfnt_reader_skip(&scan, (size_t)pairs * 4u) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_AVAR,
            gfnt_reader_tell(&scan), GFNT_GLYPH_NONE,
            "an avar segment map runs past the table");
      }
      avar->count[i] = pairs;
      avar->first[i] = total;
      total += pairs;
    }
  }
  if (total > 0) {
    avar->pairs = face->allocator->calloc_fn(face->allocator->ctx, total,
        sizeof *avar->pairs);
    if (!avar->pairs) {
      return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_AVAR, 0,
          GFNT_GLYPH_NONE, "no memory for the avar segment maps");
    }
  }
  for (size_t i = 0; i < axis_count; ++i) {
    uint16_t pairs = 0;

    if (gfnt_read_u16(&reader, &pairs) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_AVAR,
          gfnt_reader_tell(&reader), GFNT_GLYPH_NONE,
          "an avar segment map runs past the table");
    }
    for (uint16_t p = 0; p < pairs; ++p) {
      GFNT_AvarPair * pair = &avar->pairs[filled];

      if (gfnt_read_f2dot14(&reader, &pair->from) != GFNT_OK
          || gfnt_read_f2dot14(&reader, &pair->to) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_AVAR,
            gfnt_reader_tell(&reader), GFNT_GLYPH_NONE,
            "an avar segment map runs past the table");
      }
      if (p > 0 && pair->from <= avar->pairs[filled - 1].from) {
        // A map that does not strictly increase in its source coordinate is not
        // a function: two targets for one input, or a division by zero between
        // them. Nothing here would pick one on the font's behalf.
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_AVAR,
            gfnt_reader_tell(&reader) - 4u, GFNT_GLYPH_NONE,
            "an avar segment map's source coordinates do not increase");
      }
      filled += 1;
    }
  }
  return GFNT_OK;
}

void gfnt_avar_release(const GFNT_Allocator * allocator, void * table) {
  GFNT_Avar * avar = table;

  if (!allocator || !avar) {
    return;
  }
  allocator->free_fn(allocator->ctx, avar->pairs);
  allocator->free_fn(allocator->ctx, avar->first);
  allocator->free_fn(allocator->ctx, avar->count);
  memset(avar, 0, sizeof *avar);
}

GFNT_Result gfnt_face_fvar(const GFNT_Face * face, const GFNT_Fvar ** out_fvar,
    GFNT_Error * error) {
  GFNT_Fvar scratch;
  GFNT_Result result;

  if (!face || !out_fvar) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the table");
  }
  if (!gfnt_face_has_table(face, GFNT_TAG_FVAR)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_FVAR, 0,
        GFNT_GLYPH_NONE, "the face has no fvar, so it has no design space");
  }
  result = gfnt_table_cached(face, &((GFNT_Face *)face)->fvar_state,
      &((GFNT_Face *)face)->fvar, &scratch, sizeof scratch, gfnt_fvar_parse,
      NULL, gfnt_fvar_release, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_fvar = &face->fvar;
  return GFNT_OK;
}

GFNT_Result gfnt_face_avar(const GFNT_Face * face, const GFNT_Avar ** out_avar,
    GFNT_Error * error) {
  GFNT_Avar scratch;
  GFNT_Result result;

  if (!face || !out_avar) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the table");
  }
  if (!gfnt_face_has_table(face, GFNT_TAG_AVAR)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_AVAR, 0,
        GFNT_GLYPH_NONE, "the face has no avar");
  }
  result = gfnt_table_cached(face, &((GFNT_Face *)face)->avar_state,
      &((GFNT_Face *)face)->avar, &scratch, sizeof scratch, gfnt_avar_parse,
      NULL, gfnt_avar_release, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_avar = &face->avar;
  return GFNT_OK;
}

bool gfnt_face_is_variable(const GFNT_Face * face) {
  return face && gfnt_face_has_table(face, GFNT_TAG_FVAR);
}

GFNT_Result gfnt_face_axis_count(const GFNT_Face * face, size_t * out_count,
    GFNT_Error * error) {
  const GFNT_Fvar * fvar = NULL;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_count) {
    return GFNT_ERR_INVALID;
  }
  if (!gfnt_face_is_variable(face)) {
    *out_count = 0;
    return GFNT_OK;
  }
  result = gfnt_face_fvar(face, &fvar, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_count = fvar->axis_count;
  return GFNT_OK;
}

GFNT_Result gfnt_face_axis_at(const GFNT_Face * face, size_t index,
    GFNT_Axis * out_axis, GFNT_Error * error) {
  const GFNT_Fvar * fvar = NULL;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_axis) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_face_fvar(face, &fvar, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (index >= fvar->axis_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_FVAR, 0,
        GFNT_GLYPH_NONE, "this axis index is past the face's axis count");
  }
  *out_axis = fvar->axes[index];
  return GFNT_OK;
}

GFNT_Result gfnt_face_instance_count(const GFNT_Face * face, size_t * out_count,
    GFNT_Error * error) {
  const GFNT_Fvar * fvar = NULL;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_count) {
    return GFNT_ERR_INVALID;
  }
  if (!gfnt_face_is_variable(face)) {
    *out_count = 0;
    return GFNT_OK;
  }
  result = gfnt_face_fvar(face, &fvar, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_count = fvar->instance_count;
  return GFNT_OK;
}

GFNT_Result gfnt_face_instance_at(const GFNT_Face * face, size_t index,
    GFNT_NamedInstance * out_instance, GFNT_Error * error) {
  const GFNT_Fvar * fvar = NULL;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_instance) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_face_fvar(face, &fvar, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (index >= fvar->instance_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_FVAR, 0,
        GFNT_GLYPH_NONE, "this instance index is past the face's instance count");
  }
  out_instance->coordinates = fvar->coordinates + index * fvar->axis_count;
  out_instance->coordinate_count = fvar->axis_count;
  out_instance->name_id = fvar->instances[index].name_id;
  out_instance->flags = fvar->instances[index].flags;
  out_instance->postscript_name_id = fvar->instances[index].postscript_name_id;
  return GFNT_OK;
}

/** One axis: clamp, then the default-relative linear map, in 16.16. */
static int32_t gfnt_axis_normalise(const GFNT_Axis * axis, int32_t value) {
  if (value < axis->min) {
    value = axis->min;
  }
  if (value > axis->max) {
    value = axis->max;
  }
  // `def - min` and `max - def` are the two spans, and each is positive here
  // because the value is on that side of the default - which is why a default
  // equal to one end never divides by zero: nothing is on the other side of it.
  if (value < axis->def) {
    return -(int32_t)gfnt_round_div(
        ((int64_t)axis->def - value) * 65536, (int64_t)axis->def - axis->min);
  }
  if (value > axis->def) {
    return (int32_t)gfnt_round_div(
        ((int64_t)value - axis->def) * 65536, (int64_t)axis->max - axis->def);
  }
  return 0;
}

/** One axis's `avar` segment map, applied to a 16.16 normalised value. */
static int32_t gfnt_avar_map(const GFNT_Avar * avar, size_t axis, int32_t value) {
  const GFNT_AvarPair * pairs = avar->pairs + avar->first[axis];
  size_t count = avar->count[axis];
  size_t i;

  // The pairs are 2.14 and the value is 16.16, so each is widened by two bits.
  if (count == 0) {
    return value;
  }
  if (value <= (int32_t)pairs[0].from * 4) {
    // Before the first pair, or on it: the map's own offset carries on, which is
    // what fontTools does and what a map that covers -1..1 never reaches.
    return value + ((int32_t)pairs[0].to - pairs[0].from) * 4;
  }
  for (i = 1; i < count; ++i) {
    if (value <= (int32_t)pairs[i].from * 4) {
      break;
    }
  }
  if (i == count) {
    return value + ((int32_t)pairs[count - 1].to - pairs[count - 1].from) * 4;
  }
  {
    int64_t f0 = (int64_t)pairs[i - 1].from * 4;
    int64_t f1 = (int64_t)pairs[i].from * 4;
    int64_t t0 = (int64_t)pairs[i - 1].to * 4;
    int64_t t1 = (int64_t)pairs[i].to * 4;

    return (int32_t)(t0 + gfnt_round_div(((int64_t)value - f0) * (t1 - t0),
        f1 - f0));
  }
}

GFNT_Result gfnt_face_normalize(const GFNT_Face * face,
    const GFNT_F16Dot16 * user_coordinates, size_t count,
    GFNT_F2Dot14 * out_coordinates, size_t capacity, GFNT_Error * error) {
  const GFNT_Fvar * fvar = NULL;
  const GFNT_Avar * avar = NULL;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_coordinates || (count > 0 && !user_coordinates)) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_face_fvar(face, &fvar, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (count > fvar->axis_count || capacity < fvar->axis_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_FVAR, 0,
        GFNT_GLYPH_NONE, "more coordinates than the face has axes, or an output "
        "too small to hold one per axis");
  }
  if (gfnt_face_has_table(face, GFNT_TAG_AVAR)) {
    result = gfnt_face_avar(face, &avar, error);
    if (result != GFNT_OK) {
      return result;
    }
  }
  for (size_t i = 0; i < fvar->axis_count; ++i) {
    int32_t value = i < count ? user_coordinates[i] : fvar->axes[i].def;
    int32_t normalised = gfnt_axis_normalise(&fvar->axes[i], value);
    int64_t narrow;

    if (avar) {
      normalised = gfnt_avar_map(avar, i, normalised);
    }
    // 16.16 to 2.14: two bits go, rounding half up (floor of x + 2 over 4),
    // which is FreeType's FT_fixedToFdot14 and the reason the two readers can
    // agree to the bit. Clamped, because a map may carry a value past one.
    narrow = gfnt_floor_div((int64_t)normalised + 2, 4);
    if (narrow > 16384) {
      narrow = 16384;
    }
    if (narrow < -16384) {
      narrow = -16384;
    }
    out_coordinates[i] = (GFNT_F2Dot14)narrow;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_variation_dump(const GFNT_Face * face, FILE * out) {
  const GFNT_Fvar * fvar = NULL;
  const GFNT_Avar * avar = NULL;
  GFNT_Result result;

  if (!face || !out) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_face_fvar(face, &fvar, NULL);
  if (result != GFNT_OK) {
    return result;
  }
  if (fprintf(out, "fvar: %zu axes, %zu instances\n", fvar->axis_count,
          fvar->instance_count) < 0) {
    return GFNT_ERR_IO;
  }
  for (size_t i = 0; i < fvar->axis_count; ++i) {
    char tag[5];

    gfnt_tag_string(fvar->axes[i].tag, tag);
    if (fprintf(out,
            "fvar axis %zu: '%s' min %d default %d max %d flags 0x%04X name "
            "%u\n",
            i, tag, (int)fvar->axes[i].min, (int)fvar->axes[i].def,
            (int)fvar->axes[i].max, (unsigned)fvar->axes[i].flags,
            (unsigned)fvar->axes[i].name_id) < 0) {
      return GFNT_ERR_IO;
    }
  }
  for (size_t i = 0; i < fvar->instance_count; ++i) {
    if (fprintf(out, "fvar instance %zu: name %u flags 0x%04X postscript %u "
            "coordinates", i, (unsigned)fvar->instances[i].name_id,
            (unsigned)fvar->instances[i].flags,
            (unsigned)fvar->instances[i].postscript_name_id) < 0) {
      return GFNT_ERR_IO;
    }
    for (size_t axis = 0; axis < fvar->axis_count; ++axis) {
      if (fprintf(out, " %d",
              (int)fvar->coordinates[i * fvar->axis_count + axis]) < 0) {
        return GFNT_ERR_IO;
      }
    }
    if (fprintf(out, "\n") < 0) {
      return GFNT_ERR_IO;
    }
  }
  if (!gfnt_face_has_table(face, GFNT_TAG_AVAR)) {
    return GFNT_OK;
  }
  // An avar the library refuses is reported as the table's own failure, after
  // the fvar lines: the design space is readable and the mapping is not, and the
  // dump says which.
  result = gfnt_face_avar(face, &avar, NULL);
  if (result != GFNT_OK) {
    return result;
  }
  for (size_t i = 0; i < avar->axis_count; ++i) {
    if (fprintf(out, "avar axis %zu: %u pairs", i, (unsigned)avar->count[i]) < 0) {
      return GFNT_ERR_IO;
    }
    for (size_t p = 0; p < avar->count[i]; ++p) {
      const GFNT_AvarPair * pair = &avar->pairs[avar->first[i] + p];

      if (fprintf(out, " %d:%d", (int)pair->from, (int)pair->to) < 0) {
        return GFNT_ERR_IO;
      }
    }
    if (fprintf(out, "\n") < 0) {
      return GFNT_ERR_IO;
    }
  }
  return GFNT_OK;
}
