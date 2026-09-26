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
 * The six cutil functions this library links, for the big-endian golden build.
 *
 * **Why this exists, stated rather than buried.** `make check-golden` builds
 * this library for a big-endian target and compares its renderings against the
 * committed ones, which is the only way to see a host-dependent read or a
 * `float` in the path from a font's bytes to a pixel (design.md sections 1 and
 * 14.4). Cross-building **cutil** as well would need cutil's own generated
 * headers produced for the target, and a generated header produced for the wrong
 * target is exactly the class of defect this gate is looking for - it would make
 * the gate's answer depend on the thing it is testing.
 *
 * So the gate cross-builds `font` alone and links these six definitions.
 * `nm` says they are the whole of what it needs:
 *
 *   gcu_allocator_default, gcu_file_read, gcu_file_free,
 *   gcu_file_result_string, gcu_mmap_open, gcu_mmap_close
 *
 * and the list is checked by the gate rather than trusted, so a seventh
 * dependency appearing fails the build instead of silently linking cutil's.
 *
 * **What this is not.** It is not a cutil. It does not implement cutil's
 * behaviour beyond what this library asks of it, and `gcu_mmap_open` refuses
 * rather than mapping - the golden driver reads files, and a mapping would test
 * the target's `mmap` rather than this library's arithmetic. Nothing outside
 * this gate links it.
 *
 * The names are written plain and cutil's headers rename them, so the symbols
 * this defines are the namespaced ones the library calls.
 */

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/mmap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void * shim_malloc(void * ctx, size_t size) {
  (void)ctx;
  return malloc(size ? size : 1u);
}

static void * shim_calloc(void * ctx, size_t nitems, size_t size) {
  (void)ctx;
  if (size != 0 && nitems > (size_t)-1 / size) {
    return NULL;
  }
  return calloc(nitems ? nitems : 1u, size ? size : 1u);
}

static void * shim_realloc(void * ctx, void * ptr, size_t size) {
  (void)ctx;
  return realloc(ptr, size ? size : 1u);
}

static void shim_free(void * ctx, void * ptr) {
  (void)ctx;
  free(ptr);
}

const GCU_Allocator * gcu_allocator_default(void) {
  static const GCU_Allocator allocator = {
    NULL, shim_malloc, shim_calloc, shim_realloc, shim_free,
  };
  return &allocator;
}

const char * gcu_file_result_string(GCU_File_Result result) {
  switch (result) {
    case GCU_FILE_OK:
      return "ok";
    case GCU_FILE_ERR_NOT_FOUND:
      return "not found";
    default:
      break;
  }
  return "file error";
}

GCU_File_Result gcu_file_read(const char * path, size_t max_bytes,
    const GCU_Allocator * allocator, void ** out_data, size_t * out_len) {
  FILE * handle;
  unsigned char * buffer = NULL;
  size_t capacity = 0;
  size_t length = 0;

  if (!path || !out_data || !out_len) {
    return GCU_FILE_ERR_INVALID;
  }
  if (!allocator) {
    allocator = gcu_allocator_default();
  }
  handle = fopen(path, "rb");
  if (!handle) {
    return GCU_FILE_ERR_NOT_FOUND;
  }
  for (;;) {
    size_t got;

    if (length + 1u >= capacity) {
      size_t wanted = capacity ? capacity * 2u : 65536u;
      unsigned char * grown = allocator->realloc_fn(allocator->ctx, buffer,
          wanted);

      if (!grown) {
        allocator->free_fn(allocator->ctx, buffer);
        fclose(handle);
        return GCU_FILE_ERR_OOM;
      }
      buffer = grown;
      capacity = wanted;
    }
    got = fread(buffer + length, 1, capacity - length - 1u, handle);
    length += got;
    if (got == 0) {
      break;
    }
    if (max_bytes != GCU_FILE_UNLIMITED && length > max_bytes) {
      allocator->free_fn(allocator->ctx, buffer);
      fclose(handle);
      return GCU_FILE_ERR_LIMIT;
    }
  }
  if (ferror(handle)) {
    allocator->free_fn(allocator->ctx, buffer);
    fclose(handle);
    return GCU_FILE_ERR_IO;
  }
  fclose(handle);
  // cutil's contract: a NUL one past the length, not counted in it.
  buffer[length] = 0;
  *out_data = buffer;
  *out_len = length;
  return GCU_FILE_OK;
}

void gcu_file_free(const GCU_Allocator * allocator, void * data) {
  if (!allocator) {
    allocator = gcu_allocator_default();
  }
  allocator->free_fn(allocator->ctx, data);
}

int gcu_mmap_open(GCU_Mapped_File * map, const char * path, bool writable) {
  // Refused rather than implemented: the golden driver reads, and a mapping
  // here would put the target's mmap in a gate about this library's arithmetic.
  (void)map;
  (void)path;
  (void)writable;
  return -1;
}

int gcu_mmap_close(GCU_Mapped_File * map) {
  (void)map;
  return 0;
}
