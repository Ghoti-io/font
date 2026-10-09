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

#include <string.h>
#include "write.h"

void gfnt_wbuf_init(GFNT_WBuf * b, const GFNT_Allocator * allocator) {
  memset(b, 0, sizeof *b);
  b->allocator = allocator;
}

void gfnt_wbuf_free(GFNT_WBuf * b) {
  if (b->data) {
    b->allocator->free_fn(b->allocator->ctx, b->data);
  }
  b->data = NULL;
  b->length = b->capacity = 0;
}

static bool gfnt_wbuf_reserve(GFNT_WBuf * b, size_t more) {
  uint8_t * grown;
  size_t want;

  if (b->oom) {
    return false;
  }
  if (more > (size_t)-1 - b->length) {
    b->oom = true;
    return false;
  }
  want = b->length + more;
  if (want <= b->capacity) {
    return true;
  }
  if (b->capacity < 256) {
    b->capacity = 256;
  }
  while (b->capacity < want) {
    b->capacity *= 2;
  }
  grown = b->allocator->calloc_fn(b->allocator->ctx, 1, b->capacity);
  if (!grown) {
    b->oom = true;
    return false;
  }
  if (b->length) {
    memcpy(grown, b->data, b->length);
  }
  if (b->data) {
    b->allocator->free_fn(b->allocator->ctx, b->data);
  }
  b->data = grown;
  return true;
}

void gfnt_wbuf_u8(GFNT_WBuf * b, uint32_t v) {
  if (gfnt_wbuf_reserve(b, 1)) {
    b->data[b->length++] = (uint8_t)v;
  }
}

void gfnt_wbuf_u16(GFNT_WBuf * b, uint32_t v) {
  if (gfnt_wbuf_reserve(b, 2)) {
    gfnt_write_put16(b->data + b->length, v);
    b->length += 2;
  }
}

void gfnt_wbuf_u32(GFNT_WBuf * b, uint32_t v) {
  if (gfnt_wbuf_reserve(b, 4)) {
    gfnt_write_put32(b->data + b->length, v);
    b->length += 4;
  }
}

void gfnt_wbuf_bytes(GFNT_WBuf * b, const uint8_t * data, size_t length) {
  if (length && gfnt_wbuf_reserve(b, length)) {
    memcpy(b->data + b->length, data, length);
    b->length += length;
  }
}

void gfnt_wbuf_align(GFNT_WBuf * b, size_t unit) {
  while (!b->oom && b->length % unit) {
    gfnt_wbuf_u8(b, 0);
  }
}
