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
 * The growable byte buffer the derived-bytes containers build their arenas in.
 */

#include <ghoti.io/cutil/safemath.h>
#include <string.h>
#include "buffer.h"

/** The first allocation, in bytes. A page, and a small font fits in one. */
#define GFNT_BUFFER_INITIAL 4096u

void gfnt_buffer_init(GFNT_Buffer * buffer, const GFNT_Allocator * allocator) {
  buffer->data = NULL;
  buffer->length = 0;
  buffer->capacity = 0;
  buffer->allocator = allocator;
}

void gfnt_buffer_free(GFNT_Buffer * buffer) {
  if (buffer->data) {
    buffer->allocator->free_fn(buffer->allocator->ctx, buffer->data);
  }
  buffer->data = NULL;
  buffer->length = 0;
  buffer->capacity = 0;
}

void gfnt_buffer_release(GFNT_Buffer * buffer, uint8_t ** out_data,
    size_t * out_length) {
  *out_data = buffer->data;
  if (out_length) {
    *out_length = buffer->length;
  }
  buffer->data = NULL;
  buffer->length = 0;
  buffer->capacity = 0;
}

bool gfnt_buffer_grow(GFNT_Buffer * buffer, size_t extra) {
  size_t needed;
  size_t capacity;
  uint8_t * grown;

  if (!gcu_safe_add_size(buffer->length, extra, &needed)) {
    return false;
  }
  if (needed <= buffer->capacity) {
    return true;
  }
  capacity = buffer->capacity ? buffer->capacity : GFNT_BUFFER_INITIAL;
  while (capacity < needed) {
    size_t doubled;

    if (!gcu_safe_mul_size(capacity, 2, &doubled)) {
      return false;
    }
    capacity = doubled;
  }
  grown = buffer->allocator->realloc_fn(buffer->allocator->ctx, buffer->data,
      capacity);
  if (!grown) {
    return false;
  }
  buffer->data = grown;
  buffer->capacity = capacity;
  return true;
}

bool gfnt_buffer_add(GFNT_Buffer * buffer, const uint8_t * bytes,
    size_t length) {
  if (length == 0) {
    return true;
  }
  if (!gfnt_buffer_grow(buffer, length)) {
    return false;
  }
  memcpy(buffer->data + buffer->length, bytes, length);
  buffer->length += length;
  return true;
}

bool gfnt_buffer_byte(GFNT_Buffer * buffer, uint8_t byte) {
  if (!gfnt_buffer_grow(buffer, 1)) {
    return false;
  }
  buffer->data[buffer->length++] = byte;
  return true;
}

bool gfnt_buffer_zeros(GFNT_Buffer * buffer, size_t count) {
  if (count == 0) {
    return true;
  }
  if (!gfnt_buffer_grow(buffer, count)) {
    return false;
  }
  memset(buffer->data + buffer->length, 0, count);
  buffer->length += count;
  return true;
}
