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
 * The directories ::gfnt_fontset_scan_system() walks. One list per platform.
 * Linux is the list exercised by the tests. The Windows and macOS lists
 * compile on those platforms and have not been run there.
 */

#include "discover_int.h"

#include <ghoti.io/cutil/env.h>
#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/path.h>
#include <ghoti.io/cutil/safemath.h>

#include <string.h>

/** A growing list of canonical directories. */
typedef struct DiscoverDirs {
  const GFNT_Allocator * allocator;
  char ** paths;
  size_t count;
  size_t capacity;
} DiscoverDirs;

static void discover_dirs_free(DiscoverDirs * dirs) {
  size_t i;

  if (!dirs->paths) {
    return;
  }
  for (i = 0; i < dirs->count; i++) {
    dirs->allocator->free_fn(dirs->allocator->ctx, dirs->paths[i]);
  }
  dirs->allocator->free_fn(dirs->allocator->ctx, dirs->paths);
  dirs->paths = NULL;
  dirs->count = 0;
  dirs->capacity = 0;
}

/**
 * Copy an environment variable. Empty and unset are the same answer: the
 * XDG spec treats an empty value as unset, and a path built from one would
 * be the current directory.
 */
static GFNT_Result discover_env(const GFNT_Allocator * allocator,
    const char * name, char ** out) {
  size_t need;
  char * copy;

  *out = NULL;
  need = gcu_env_get(name, NULL, 0);
  if (need <= 1) {
    return GFNT_OK;
  }
  copy = allocator->malloc_fn(allocator->ctx, need);
  if (!copy) {
    return GFNT_ERR_OOM;
  }
  if (gcu_env_get(name, copy, need) != need || copy[0] == '\0') {
    allocator->free_fn(allocator->ctx, copy);
    return GFNT_OK;
  }
  *out = copy;
  return GFNT_OK;
}

static GFNT_Result discover_join(const GFNT_Allocator * allocator,
    const char * base, const char * relative, char ** out) {
  size_t need = 0;
  char * joined;

  *out = NULL;
  if (gcu_path_join(GCU_PATH_NATIVE, base, relative, NULL, 0, &need)
      != GCU_PATH_OK) {
    return GFNT_ERR_INTERNAL;
  }
  joined = allocator->malloc_fn(allocator->ctx, need + 1);
  if (!joined) {
    return GFNT_ERR_OOM;
  }
  if (gcu_path_join(GCU_PATH_NATIVE, base, relative, joined, need + 1, NULL)
      != GCU_PATH_OK) {
    allocator->free_fn(allocator->ctx, joined);
    return GFNT_ERR_INTERNAL;
  }
  *out = joined;
  return GFNT_OK;
}

static GFNT_Result discover_push(DiscoverDirs * dirs, char * path) {
  size_t next;
  size_t bytes;
  char ** grown;

  if (dirs->count == dirs->capacity) {
    if (dirs->capacity == 0) {
      next = 8;
    } else if (!gcu_safe_mul_size(dirs->capacity, 2, &next)) {
      return GFNT_ERR_OOM;
    }
    if (!gcu_safe_mul_size(next, sizeof *grown, &bytes)) {
      return GFNT_ERR_OOM;
    }
    grown = dirs->allocator->realloc_fn(dirs->allocator->ctx, dirs->paths,
        bytes);
    if (!grown) {
      return GFNT_ERR_OOM;
    }
    dirs->paths = grown;
    dirs->capacity = next;
  }
  dirs->paths[dirs->count++] = path;
  return GFNT_OK;
}

/**
 * Keep @p path when it is a directory this list does not already have.
 * Takes ownership of @p path either way.
 */
static GFNT_Result discover_consider(DiscoverDirs * dirs, char * path) {
  GCU_File_Info info;
  char * canon = NULL;
  size_t i;
  GFNT_Result result;

  if (!path) {
    return GFNT_ERR_OOM;
  }
  if (gcu_file_stat(path, &info) != GCU_FILE_OK
      || info.type != GCU_FILE_TYPE_DIRECTORY) {
    dirs->allocator->free_fn(dirs->allocator->ctx, path);
    return GFNT_OK;
  }
  {
    GCU_Path_Result canon_result = gcu_path_canonicalize(path,
        dirs->allocator, &canon);
    dirs->allocator->free_fn(dirs->allocator->ctx, path);
    if (canon_result == GCU_PATH_ERR_OOM) {
      return GFNT_ERR_OOM;
    }
    if (canon_result != GCU_PATH_OK) {
      return GFNT_OK;
    }
  }
  for (i = 0; i < dirs->count; i++) {
    if (strcmp(dirs->paths[i], canon) == 0) {
      dirs->allocator->free_fn(dirs->allocator->ctx, canon);
      return GFNT_OK;
    }
  }
  result = discover_push(dirs, canon);
  if (result != GFNT_OK) {
    dirs->allocator->free_fn(dirs->allocator->ctx, canon);
  }
  return result;
}

static GFNT_Result discover_consider_join(DiscoverDirs * dirs,
    const char * base, const char * relative) {
  char * path = NULL;
  GFNT_Result result;

  result = discover_join(dirs->allocator, base, relative, &path);
  if (result != GFNT_OK) {
    return result;
  }
  return discover_consider(dirs, path);
}

#if defined(_WIN32)

static GFNT_Result discover_platform_dirs(DiscoverDirs * dirs,
    GFNT_Error * error) {
  char * windir = NULL;
  char * local = NULL;
  GFNT_Result result = GFNT_OK;

  /* TODO(windows): never compiled or run on Windows. %WINDIR%\Fonts and
   * %LOCALAPPDATA%\Microsoft\Windows\Fonts. The registry and DirectWrite are
   * not read. notes/suite/WINDOWS-TODO.md says how to verify this list.
   */
  result = discover_env(dirs->allocator, "WINDIR", &windir);
  if (result == GFNT_OK && windir) {
    result = discover_consider_join(dirs, windir, "Fonts");
  }
  if (result == GFNT_OK) {
    result = discover_env(dirs->allocator, "LOCALAPPDATA", &local);
  }
  if (result == GFNT_OK && local) {
    result = discover_consider_join(dirs, local, "Microsoft\\Windows\\Fonts");
  }
  dirs->allocator->free_fn(dirs->allocator->ctx, windir);
  dirs->allocator->free_fn(dirs->allocator->ctx, local);
  if (result != GFNT_OK) {
    return gfnt_error_set(error, result, 0, 0, GFNT_GLYPH_NONE,
        "building the Windows font directory list");
  }
  return GFNT_OK;
}

#elif defined(__APPLE__)

static GFNT_Result discover_platform_dirs(DiscoverDirs * dirs,
    GFNT_Error * error) {
  char * home = NULL;
  GFNT_Result result;
  char * system_fonts;
  char * library_fonts;

  /* TODO(macos): never compiled or run on macOS. /System/Library/Fonts,
   * /Library/Fonts and ~/Library/Fonts. CoreText is not consulted.
   * notes/suite/WINDOWS-TODO.md says how to verify this list.
   */
  system_fonts = dirs->allocator->malloc_fn(dirs->allocator->ctx,
      sizeof "/System/Library/Fonts");
  library_fonts = dirs->allocator->malloc_fn(dirs->allocator->ctx,
      sizeof "/Library/Fonts");
  if (!system_fonts || !library_fonts) {
    dirs->allocator->free_fn(dirs->allocator->ctx, system_fonts);
    dirs->allocator->free_fn(dirs->allocator->ctx, library_fonts);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "building the macOS font directory list");
  }
  memcpy(system_fonts, "/System/Library/Fonts", sizeof "/System/Library/Fonts");
  memcpy(library_fonts, "/Library/Fonts", sizeof "/Library/Fonts");
  result = discover_consider(dirs, system_fonts);
  if (result == GFNT_OK) {
    result = discover_consider(dirs, library_fonts);
  } else {
    dirs->allocator->free_fn(dirs->allocator->ctx, library_fonts);
  }
  if (result == GFNT_OK) {
    result = discover_env(dirs->allocator, "HOME", &home);
  }
  if (result == GFNT_OK && home) {
    result = discover_consider_join(dirs, home, "Library/Fonts");
  }
  dirs->allocator->free_fn(dirs->allocator->ctx, home);
  if (result != GFNT_OK) {
    return gfnt_error_set(error, result, 0, 0, GFNT_GLYPH_NONE,
        "building the macOS font directory list");
  }
  return GFNT_OK;
}

#else

static GFNT_Result discover_consider_data_dirs(DiscoverDirs * dirs,
    const char * spec) {
  char * copy;
  char * cursor;
  GFNT_Result result = GFNT_OK;

  copy = dirs->allocator->malloc_fn(dirs->allocator->ctx, strlen(spec) + 1);
  if (!copy) {
    return GFNT_ERR_OOM;
  }
  memcpy(copy, spec, strlen(spec) + 1);
  cursor = copy;
  while (result == GFNT_OK && *cursor) {
    char * colon = strchr(cursor, ':');
    if (colon) {
      *colon = '\0';
    }
    if (cursor[0] != '\0'
        && gcu_path_is_absolute(GCU_PATH_NATIVE, cursor)) {
      result = discover_consider_join(dirs, cursor, "fonts");
    }
    if (!colon) {
      break;
    }
    cursor = colon + 1;
  }
  dirs->allocator->free_fn(dirs->allocator->ctx, copy);
  return result;
}

static GFNT_Result discover_platform_dirs(DiscoverDirs * dirs,
    GFNT_Error * error) {
  char * home = NULL;
  char * xdg_home = NULL;
  char * xdg_dirs = NULL;
  char * share = NULL;
  GFNT_Result result;

  result = discover_env(dirs->allocator, "HOME", &home);
  if (result == GFNT_OK) {
    result = discover_env(dirs->allocator, "XDG_DATA_HOME", &xdg_home);
  }
  if (result == GFNT_OK) {
    if (xdg_home && gcu_path_is_absolute(GCU_PATH_NATIVE, xdg_home)) {
      result = discover_consider_join(dirs, xdg_home, "fonts");
    } else if (home) {
      result = discover_consider_join(dirs, home, ".local/share/fonts");
    }
  }
  if (result == GFNT_OK && home) {
    result = discover_consider_join(dirs, home, ".fonts");
  }
  if (result == GFNT_OK) {
    result = discover_env(dirs->allocator, "XDG_DATA_DIRS", &xdg_dirs);
  }
  if (result == GFNT_OK) {
    result = discover_consider_data_dirs(dirs,
        xdg_dirs ? xdg_dirs : "/usr/local/share:/usr/share");
  }
  if (result == GFNT_OK) {
    share = dirs->allocator->malloc_fn(dirs->allocator->ctx,
        sizeof "/usr/share/fonts");
    if (!share) {
      result = GFNT_ERR_OOM;
    } else {
      memcpy(share, "/usr/share/fonts", sizeof "/usr/share/fonts");
      result = discover_consider(dirs, share);
    }
  }
  dirs->allocator->free_fn(dirs->allocator->ctx, home);
  dirs->allocator->free_fn(dirs->allocator->ctx, xdg_home);
  dirs->allocator->free_fn(dirs->allocator->ctx, xdg_dirs);
  if (result != GFNT_OK) {
    return gfnt_error_set(error, result, 0, 0, GFNT_GLYPH_NONE,
        "building the font directory list");
  }
  return GFNT_OK;
}

#endif

GFNT_Result gfnt_discover_system_dirs(const GFNT_Allocator * allocator,
    char *** out_dirs, size_t * out_count, GFNT_Error * error) {
  DiscoverDirs dirs;
  GFNT_Result result;

  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  if (!out_dirs || !out_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "nowhere to put the system font directories");
  }
  memset(&dirs, 0, sizeof dirs);
  dirs.allocator = allocator;
  result = discover_platform_dirs(&dirs, error);
  if (result != GFNT_OK) {
    discover_dirs_free(&dirs);
    return result;
  }
  *out_dirs = dirs.paths;
  *out_count = dirs.count;
  return GFNT_OK;
}
