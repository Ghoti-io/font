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
 * The cutil and compress functions this library links, for the big-endian golden
 * build.
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
 * So the gate cross-builds `font` alone and links these definitions. `nm` says
 * they are the whole of what it needs:
 *
 *   gcu_allocator_default, gcu_file_read, gcu_file_free,
 *   gcu_file_result_string, gcu_mmap_open, gcu_mmap_close,
 *   gcu_dir_open, gcu_dir_read, gcu_dir_close, gcu_file_stat,
 *   gcu_path_join, gcu_path_canonicalize, gcu_env_get
 *   gcomp_decode_alloc, gcomp_buffer_free, gcomp_options_create,
 *   gcomp_options_destroy, gcomp_options_set_bool, gcomp_options_set_uint64,
 *   gcomp_status_to_string
 *
 * and the list is checked by the gate rather than trusted, so a seventh
 * dependency appearing fails the build instead of silently linking cutil's.
 *
 * **What this is not.** It is not a cutil and it is certainly not a compress. It
 * does not implement either library's behaviour beyond what this one asks of it,
 * and two of the shims **refuse** rather than working: `gcu_mmap_open`, because
 * the golden driver reads files and a mapping would test the target's `mmap`
 * rather than this library's arithmetic; and `gcomp_decode_alloc`, because a
 * second inflater is not what this gate measures.
 *
 * Refusing the inflate costs nothing, and it is worth saying why rather than
 * leaving it to be discovered. No committed rendering comes from a compressed
 * file: gzip is byte-identical decompression, so what a gzipped PCF renders to is
 * what the PCF inside it renders to, and *that* is in the committed set nine
 * times over. A rendering of `bitmap-gz.pcf.gz` would repeat `bitmap.pcf`'s under
 * another name, which is the same reason `bare.cff` is not in the golden set. If
 * one is ever added, this shim has to grow a real inflater or the gate will
 * report it as refused - and `check_golden.py` compares refusals too, so it would
 * say so rather than pass.
 *
 * Nothing outside this gate links this file.
 *
 * The names are written plain and cutil's headers rename them, so the symbols
 * this defines are the namespaced ones the library calls.
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/dir.h>
#include <ghoti.io/cutil/env.h>
#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/mmap.h>
#include <ghoti.io/cutil/path.h>
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

GCU_File_Result gcu_dir_open(GCU_Dir * dir, const char * path,
    const GCU_Allocator * allocator) {
  // Refused. Discovery walks directories; this gate does not, and a real
  // walk here would be the host's filesystem rather than the arithmetic under
  // test.
  (void)path;
  (void)allocator;
  if (!dir) {
    return GCU_FILE_ERR_INVALID;
  }
  memset(dir, 0, sizeof *dir);
  return GCU_FILE_ERR_IO;
}

GCU_File_Result gcu_dir_read(GCU_Dir * dir, const char ** out_name,
    GCU_File_Type * out_type, bool * out_done) {
  (void)dir;
  (void)out_name;
  (void)out_type;
  (void)out_done;
  return GCU_FILE_ERR_IO;
}

void gcu_dir_close(GCU_Dir * dir) {
  if (dir) {
    memset(dir, 0, sizeof *dir);
  }
}

GCU_File_Result gcu_file_stat(const char * path, GCU_File_Info * out) {
  (void)path;
  (void)out;
  return GCU_FILE_ERR_IO;
}

GCU_Path_Result gcu_path_join(GCU_Path_Flavor flavor, const char * base,
    const char * relative, char * out, size_t out_size, size_t * out_len) {
  (void)flavor;
  (void)base;
  (void)relative;
  (void)out;
  (void)out_size;
  (void)out_len;
  return GCU_PATH_ERR_IO;
}

GCU_Path_Result gcu_path_canonicalize(const char * path,
    const GCU_Allocator * allocator, char ** out) {
  (void)path;
  (void)allocator;
  (void)out;
  return GCU_PATH_ERR_IO;
}

size_t gcu_env_get(const char * name, char * buffer, size_t size) {
  (void)name;
  (void)buffer;
  (void)size;
  return 0;
}

//
// compress, for the gzip wrapper a bitmap font usually arrives in.
//

gcomp_status_t gcomp_options_create(gcomp_options_t ** options_out) {
  // A non-NULL handle nothing dereferences: the library sets options on it and
  // then hands it to the decode below, which refuses before reading any.
  static int placeholder;

  if (!options_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *options_out = (gcomp_options_t *)&placeholder;
  return GCOMP_OK;
}

void gcomp_options_destroy(GCOMP_MAYBE_UNUSED(gcomp_options_t * options)) {
}

gcomp_status_t gcomp_options_set_uint64(GCOMP_MAYBE_UNUSED(gcomp_options_t * o),
    GCOMP_MAYBE_UNUSED(const char * key), GCOMP_MAYBE_UNUSED(uint64_t value)) {
  return GCOMP_OK;
}

gcomp_status_t gcomp_options_set_bool(GCOMP_MAYBE_UNUSED(gcomp_options_t * o),
    GCOMP_MAYBE_UNUSED(const char * key), GCOMP_MAYBE_UNUSED(int value)) {
  return GCOMP_OK;
}

gcomp_status_t gcomp_decode_alloc(GCOMP_MAYBE_UNUSED(gcomp_registry_t * r),
    GCOMP_MAYBE_UNUSED(const char * method_name),
    GCOMP_MAYBE_UNUSED(gcomp_options_t * options),
    GCOMP_MAYBE_UNUSED(const void * input_data),
    GCOMP_MAYBE_UNUSED(size_t input_size),
    GCOMP_MAYBE_UNUSED(void ** data_out),
    GCOMP_MAYBE_UNUSED(size_t * size_out)) {
  // Refused, for the reason in the header: no committed rendering comes from a
  // compressed file, and a second inflater is not what this gate measures.
  return GCOMP_ERR_UNSUPPORTED;
}

void gcomp_buffer_free(GCOMP_MAYBE_UNUSED(gcomp_registry_t * registry),
    GCOMP_MAYBE_UNUSED(void * data)) {
}

const char * gcomp_status_to_string(GCOMP_MAYBE_UNUSED(gcomp_status_t status)) {
  return "the golden gate's shim does not inflate";
}
