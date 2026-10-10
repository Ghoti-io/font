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
 * Walking directories into a ::GFNT_FontSet. The rules live on discover.h;
 * this file is the walk, the record, and `fonts.dir` / `fonts.alias`.
 */

#include "discover_int.h"

#include <ghoti.io/cutil/dir.h>
#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/path.h>
#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/name.h>

#include <stdbool.h>
#include <string.h>

/** A directory nested deeper than this is a skip and is not entered. */
#define GFNT_DISCOVER_MAX_DEPTH 64

/**
 * How many alias steps resolve. The next one is ::GFNT_ERR_LIMIT, so a chain
 * of seven still settles and the eighth step — or a cycle — does not.
 */
#define GFNT_DISCOVER_ALIAS_STEPS 8

/** `fonts.dir` / `fonts.alias` larger than this are a skip, not a read. */
#define GFNT_DISCOVER_INDEX_BYTES (1u << 20)

typedef struct DiscoverFace {
  GFNT_FontRecord rec;
  bool weight_stated;
  bool width_stated;
  bool slant_stated;
} DiscoverFace;

struct GFNT_FontSet {
  const GFNT_Allocator * allocator;
  DiscoverFace * faces;
  size_t face_count;
  size_t face_cap;
  GFNT_FontAlias * aliases;
  size_t alias_count;
  size_t alias_cap;
  GFNT_FontSkip * skips;
  size_t skip_count;
  size_t skip_cap;
  char ** visited;
  size_t visited_count;
  size_t visited_cap;
};

static bool is_sep(char c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

static char * discover_dup(const GFNT_Allocator * allocator, const char * text,
    size_t length) {
  char * copy;

  copy = allocator->malloc_fn(allocator->ctx, length + 1);
  if (!copy) {
    return NULL;
  }
  if (length) {
    memcpy(copy, text, length);
  }
  copy[length] = '\0';
  return copy;
}

static char * discover_dup_str(const GFNT_Allocator * allocator,
    const char * text) {
  if (!text) {
    text = "";
  }
  return discover_dup(allocator, text, strlen(text));
}

static void discover_free(const GFNT_Allocator * allocator, void * ptr) {
  allocator->free_fn(allocator->ctx, ptr);
}

static void * discover_grow(const GFNT_Allocator * allocator, void * ptr,
    size_t * capacity, size_t count, size_t element) {
  size_t next;
  size_t bytes;
  void * grown;

  if (count < *capacity) {
    return ptr;
  }
  if (*capacity == 0) {
    next = 8;
  } else if (!gcu_safe_mul_size(*capacity, 2, &next)) {
    return NULL;
  }
  if (!gcu_safe_mul_size(next, element, &bytes)) {
    return NULL;
  }
  grown = allocator->realloc_fn(allocator->ctx, ptr, bytes);
  if (!grown) {
    return NULL;
  }
  *capacity = next;
  return grown;
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
    discover_free(allocator, joined);
    return GFNT_ERR_INTERNAL;
  }
  *out = joined;
  return GFNT_OK;
}

static bool discover_visited(const GFNT_FontSet * set, const char * canon) {
  size_t i;

  for (i = 0; i < set->visited_count; i++) {
    if (strcmp(set->visited[i], canon) == 0) {
      return true;
    }
  }
  return false;
}

/** Takes ownership of @p canon. */
static GFNT_Result discover_mark(GFNT_FontSet * set, char * canon) {
  char ** grown;

  grown = discover_grow(set->allocator, set->visited, &set->visited_cap,
      set->visited_count, sizeof *grown);
  if (!grown) {
    discover_free(set->allocator, canon);
    return GFNT_ERR_OOM;
  }
  set->visited = grown;
  set->visited[set->visited_count++] = canon;
  return GFNT_OK;
}

static GFNT_Result discover_add_skip(GFNT_FontSet * set, const char * path,
    GFNT_Result why) {
  GFNT_FontSkip * grown;
  char * copy;

  copy = discover_dup_str(set->allocator, path);
  if (!copy) {
    return GFNT_ERR_OOM;
  }
  grown = discover_grow(set->allocator, set->skips, &set->skip_cap,
      set->skip_count, sizeof *grown);
  if (!grown) {
    discover_free(set->allocator, copy);
    return GFNT_ERR_OOM;
  }
  set->skips = grown;
  set->skips[set->skip_count].path = copy;
  set->skips[set->skip_count].result = why;
  set->skip_count++;
  return GFNT_OK;
}

static bool discover_alias_has(const GFNT_FontSet * set, const char * name,
    size_t length) {
  size_t i;

  for (i = 0; i < set->alias_count; i++) {
    if (strlen(set->aliases[i].name) == length
        && memcmp(set->aliases[i].name, name, length) == 0) {
      return true;
    }
  }
  return false;
}

static bool discover_normalize(const char * text, size_t length, char * out,
    size_t out_cap) {
  size_t i;
  size_t j = 0;

  for (i = 0; i < length; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c == ' ' || c == '-' || c == '\t') {
      continue;
    }
    if (c >= 'A' && c <= 'Z') {
      c = (unsigned char)(c - 'A' + 'a');
    }
    if (j + 1 >= out_cap) {
      return false;
    }
    out[j++] = (char)c;
  }
  out[j] = '\0';
  return j > 0;
}

static bool discover_weight_word(const char * text, size_t length,
    uint16_t * out) {
  static const struct {
    const char * word;
    uint16_t value;
  } words[] = {
    { "thin", 100 },
    { "extralight", 200 },
    { "ultralight", 200 },
    { "light", 300 },
    { "regular", 400 },
    { "roman", 400 },
    { "book", 400 },
    { "normal", 400 },
    { "medium", 500 },
    { "semibold", 600 },
    { "demibold", 600 },
    { "bold", 700 },
    { "extrabold", 800 },
    { "ultrabold", 800 },
    { "black", 900 },
    { "heavy", 900 },
  };
  char word[32];
  size_t i;

  if (!discover_normalize(text, length, word, sizeof word)) {
    return false;
  }
  for (i = 0; i < sizeof words / sizeof words[0]; i++) {
    if (strcmp(word, words[i].word) == 0) {
      *out = words[i].value;
      return true;
    }
  }
  return false;
}

static bool discover_width_word(const char * text, size_t length,
    uint16_t * out) {
  static const struct {
    const char * word;
    uint16_t value;
  } words[] = {
    { "ultracondensed", 1 },
    { "extracondensed", 2 },
    { "condensed", 3 },
    { "semicondensed", 4 },
    { "medium", 5 },
    { "normal", 5 },
    { "semiexpanded", 6 },
    { "expanded", 7 },
    { "extraexpanded", 8 },
    { "ultraexpanded", 9 },
  };
  char word[32];
  size_t i;

  if (!discover_normalize(text, length, word, sizeof word)) {
    return false;
  }
  for (i = 0; i < sizeof words / sizeof words[0]; i++) {
    if (strcmp(word, words[i].word) == 0) {
      *out = words[i].value;
      return true;
    }
  }
  return false;
}

static bool discover_slant_letter(const char * text, size_t length,
    GFNT_Slant * out) {
  char word[3];
  size_t i;

  /* The whole field, not its first letter: ri is italic, ro is oblique,
     and ot is not a slant this library names. */
  if (length == 0 || length > 2) {
    return false;
  }
  for (i = 0; i < length; i++) {
    char c = text[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    word[i] = c;
  }
  word[length] = '\0';
  if (strcmp(word, "r") == 0) {
    *out = GFNT_SLANT_ROMAN;
    return true;
  }
  if (strcmp(word, "i") == 0 || strcmp(word, "ri") == 0) {
    *out = GFNT_SLANT_ITALIC;
    return true;
  }
  if (strcmp(word, "o") == 0 || strcmp(word, "ro") == 0) {
    *out = GFNT_SLANT_OBLIQUE;
    return true;
  }
  return false;
}

/**
 * Field @p want of an XLFD, counting the foundry as 1. The family is 2, the
 * weight 3, the slant 4 and the setwidth 5. The pointer is into @p xlfd.
 */
static bool discover_xlfd_field(const char * xlfd, int want, const char ** out,
    size_t * out_len) {
  const char * cursor;
  int field = 0;

  if (!xlfd || xlfd[0] != '-') {
    return false;
  }
  cursor = xlfd;
  while (*cursor) {
    const char * start;
    if (*cursor != '-') {
      return false;
    }
    cursor++;
    start = cursor;
    while (*cursor && *cursor != '-') {
      cursor++;
    }
    field++;
    if (field == want) {
      *out = start;
      *out_len = (size_t)(cursor - start);
      return true;
    }
  }
  return false;
}

static void discover_apply_xlfd(DiscoverFace * face, const char * xlfd) {
  const char * field;
  size_t length;
  uint16_t value;
  GFNT_Slant slant;

  if (!face->weight_stated && discover_xlfd_field(xlfd, 3, &field, &length)
      && discover_weight_word(field, length, &value)) {
    face->rec.weight = value;
    face->weight_stated = true;
  }
  if (!face->slant_stated && discover_xlfd_field(xlfd, 4, &field, &length)
      && discover_slant_letter(field, length, &slant)) {
    face->rec.slant = slant;
    face->slant_stated = true;
  }
  if (!face->width_stated && discover_xlfd_field(xlfd, 5, &field, &length)
      && discover_width_word(field, length, &value)) {
    face->rec.width = value;
    face->width_stated = true;
  }
}

/**
 * The string an alias stores. An XLFD contributes its family field; anything
 * else is stored as given. NULL is the caller's OOM.
 */
static char * discover_alias_target(const GFNT_Allocator * allocator,
    const char * target) {
  const char * field;
  size_t length;

  if (target[0] == '-'
      && discover_xlfd_field(target, 2, &field, &length)) {
    return discover_dup(allocator, field, length);
  }
  return discover_dup_str(allocator, target);
}

static GFNT_Result discover_add_alias(GFNT_FontSet * set, const char * name,
    size_t name_len, const char * target) {
  GFNT_FontAlias * grown;
  char * name_copy;
  char * target_copy;

  if (discover_alias_has(set, name, name_len)) {
    return GFNT_OK;
  }
  name_copy = discover_dup(set->allocator, name, name_len);
  target_copy = discover_alias_target(set->allocator, target);
  if (!name_copy || !target_copy) {
    discover_free(set->allocator, name_copy);
    discover_free(set->allocator, target_copy);
    return GFNT_ERR_OOM;
  }
  grown = discover_grow(set->allocator, set->aliases, &set->alias_cap,
      set->alias_count, sizeof *grown);
  if (!grown) {
    discover_free(set->allocator, name_copy);
    discover_free(set->allocator, target_copy);
    return GFNT_ERR_OOM;
  }
  set->aliases = grown;
  set->aliases[set->alias_count].name = name_copy;
  set->aliases[set->alias_count].target = target_copy;
  set->alias_count++;
  return GFNT_OK;
}

static const char * discover_skip_ws(const char * cursor, const char * end) {
  while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
    cursor++;
  }
  return cursor;
}

static bool discover_take_token(const char * cursor, const char * end,
    const char ** token, size_t * length, const char ** next) {
  cursor = discover_skip_ws(cursor, end);
  if (cursor >= end || *cursor == '!' || *cursor == '\0') {
    return false;
  }
  if (*cursor == '"') {
    cursor++;
    *token = cursor;
    while (cursor < end && *cursor != '"') {
      cursor++;
    }
    *length = (size_t)(cursor - *token);
    if (cursor < end && *cursor == '"') {
      cursor++;
    }
    *next = cursor;
    return true;
  }
  *token = cursor;
  while (cursor < end && *cursor != ' ' && *cursor != '\t') {
    cursor++;
  }
  *length = (size_t)(cursor - *token);
  *next = cursor;
  return *length > 0;
}

static bool discover_line_is_count(const char * line, const char * end) {
  const char * cursor = discover_skip_ws(line, end);
  if (cursor >= end || *cursor < '0' || *cursor > '9') {
    return false;
  }
  while (cursor < end && *cursor >= '0' && *cursor <= '9') {
    cursor++;
  }
  cursor = discover_skip_ws(cursor, end);
  return cursor == end;
}

static bool discover_path_is(const char * path, const char * dir,
    const char * name, size_t name_len) {
  size_t dir_len = strlen(dir);
  size_t path_len = strlen(path);
  size_t extra = 1;

  /* "/" is already the separator. A file there is "/name", not "//name". */
  if (dir_len == 1 && is_sep(dir[0])) {
    extra = 0;
  }
  if (path_len != dir_len + extra + name_len) {
    return false;
  }
  if (memcmp(path, dir, dir_len) != 0) {
    return false;
  }
  if (extra && !is_sep(path[dir_len])) {
    return false;
  }
  return memcmp(path + dir_len + extra, name, name_len) == 0;
}

static void discover_apply_dir_line(GFNT_FontSet * set, const char * dir,
    const char * name, size_t name_len, const char * xlfd) {
  size_t i;

  for (i = 0; i < set->face_count; i++) {
    if (discover_path_is(set->faces[i].rec.path, dir, name, name_len)) {
      discover_apply_xlfd(&set->faces[i], xlfd);
    }
  }
}

static GFNT_Result discover_read_index(GFNT_FontSet * set, const char * path,
    void ** out_data, size_t * out_len, bool * present) {
  GCU_File_Result read;

  *present = false;
  *out_data = NULL;
  read = gcu_file_read(path, GFNT_DISCOVER_INDEX_BYTES, set->allocator,
      out_data, out_len);
  if (read == GCU_FILE_ERR_NOT_FOUND) {
    return GFNT_OK;
  }
  if (read == GCU_FILE_ERR_OOM) {
    return GFNT_ERR_OOM;
  }
  if (read != GCU_FILE_OK) {
    GFNT_Result why = read == GCU_FILE_ERR_LIMIT ? GFNT_ERR_LIMIT : GFNT_ERR_IO;
    return discover_add_skip(set, path, why);
  }
  *present = true;
  return GFNT_OK;
}

static GFNT_Result discover_parse_alias(GFNT_FontSet * set, const char * bytes,
    size_t length) {
  const char * cursor = bytes;
  const char * end = bytes + length;

  while (cursor < end) {
    const char * line = cursor;
    const char * line_end;
    const char * nl;
    const char * name;
    const char * target;
    size_t name_len;
    size_t target_len;
    const char * next;
    char * target_copy;
    GFNT_Result result;

    nl = memchr(cursor, '\n', (size_t)(end - cursor));
    line_end = nl ? nl : end;
    cursor = nl ? nl + 1 : end;
    if (line_end > line && line_end[-1] == '\r') {
      line_end--;
    }
    if (!discover_take_token(line, line_end, &name, &name_len, &next)) {
      continue;
    }
    if (!discover_take_token(next, line_end, &target, &target_len, &next)) {
      continue;
    }
    target_copy = discover_dup(set->allocator, target, target_len);
    if (!target_copy) {
      return GFNT_ERR_OOM;
    }
    result = discover_add_alias(set, name, name_len, target_copy);
    discover_free(set->allocator, target_copy);
    if (result != GFNT_OK) {
      return result;
    }
  }
  return GFNT_OK;
}

static GFNT_Result discover_parse_dir(GFNT_FontSet * set, const char * dir,
    const char * bytes, size_t length) {
  const char * cursor = bytes;
  const char * end = bytes + length;
  bool first = true;

  while (cursor < end) {
    const char * line = cursor;
    const char * line_end;
    const char * nl;
    const char * name;
    size_t name_len;
    const char * next;
    char * xlfd;

    nl = memchr(cursor, '\n', (size_t)(end - cursor));
    line_end = nl ? nl : end;
    cursor = nl ? nl + 1 : end;
    if (line_end > line && line_end[-1] == '\r') {
      line_end--;
    }
    if (first) {
      first = false;
      if (discover_line_is_count(line, line_end)) {
        continue;
      }
    }
    if (!discover_take_token(line, line_end, &name, &name_len, &next)) {
      continue;
    }
    next = discover_skip_ws(next, line_end);
    if (next >= line_end || *next != '-') {
      continue;
    }
    xlfd = discover_dup(set->allocator, next, (size_t)(line_end - next));
    if (!xlfd) {
      return GFNT_ERR_OOM;
    }
    discover_apply_dir_line(set, dir, name, name_len, xlfd);
    discover_free(set->allocator, xlfd);
  }
  return GFNT_OK;
}

static bool discover_regular_file(const char * path) {
  GCU_File_Info info;

  return gcu_file_stat(path, &info) == GCU_FILE_OK
      && info.type == GCU_FILE_TYPE_REGULAR;
}

static GFNT_Result discover_read_indexes(GFNT_FontSet * set, const char * dir) {
  char * dir_path = NULL;
  char * alias_path = NULL;
  void * data = NULL;
  size_t length = 0;
  bool present = false;
  GFNT_Result result = GFNT_OK;

  result = discover_join(set->allocator, dir, "fonts.dir", &dir_path);
  if (result != GFNT_OK) {
    return result;
  }
  /* A FIFO of this name would block the scan. Only a regular file is read. */
  if (discover_regular_file(dir_path)) {
    result = discover_read_index(set, dir_path, &data, &length, &present);
  }
  discover_free(set->allocator, dir_path);
  if (result != GFNT_OK) {
    return result;
  }
  if (present) {
    result = discover_parse_dir(set, dir, data, length);
    gcu_file_free(set->allocator, data);
    data = NULL;
    if (result != GFNT_OK) {
      return result;
    }
  }

  result = discover_join(set->allocator, dir, "fonts.alias", &alias_path);
  if (result != GFNT_OK) {
    return result;
  }
  if (discover_regular_file(alias_path)) {
    result = discover_read_index(set, alias_path, &data, &length, &present);
  } else {
    present = false;
  }
  discover_free(set->allocator, alias_path);
  if (result != GFNT_OK) {
    return result;
  }
  if (present) {
    result = discover_parse_alias(set, data, length);
    gcu_file_free(set->allocator, data);
  }
  return result;
}

static GFNT_Result discover_copy_name(const GFNT_Face * face, uint16_t name_id,
    const GFNT_Allocator * allocator, char ** out, bool * found,
    GFNT_Error * error) {
  GFNT_Result result;

  *out = NULL;
  *found = false;
  result = gfnt_face_name(face, name_id, GFNT_LANGUAGE_ANY, allocator, out,
      NULL, error);
  if (result == GFNT_OK) {
    *found = true;
    return GFNT_OK;
  }
  if (result == GFNT_ERR_UNSUPPORTED) {
    return GFNT_OK;
  }
  return result;
}

static GFNT_Result discover_choose_name(const GFNT_Face * face,
    uint16_t first, uint16_t second, const GFNT_Allocator * allocator,
    char ** out, GFNT_Error * error) {
  bool found = false;
  GFNT_Result result;

  result = discover_copy_name(face, first, allocator, out, &found, error);
  if (result != GFNT_OK || found) {
    return result;
  }
  return discover_copy_name(face, second, allocator, out, &found, error);
}

static bool discover_is_bitmap_container(GFNT_Tag flavour) {
  return flavour == GFNT_FLAVOUR_PCF || flavour == GFNT_FLAVOUR_BDF;
}

static GFNT_Result discover_fill_face(GFNT_FontSet * set, const char * path,
    GFNT_Face * face, GFNT_Error * error) {
  DiscoverFace built;
  DiscoverFace * grown;
  char * family = NULL;
  char * style = NULL;
  char * full = NULL;
  char * path_copy = NULL;
  bool found = false;
  const GFNT_Os2 * os2 = NULL;
  GFNT_Result result;
  uint16_t weight;

  memset(&built, 0, sizeof built);
  built.rec.flavour = gfnt_face_flavour(face);
  built.rec.index = gfnt_face_index(face);
  built.rec.slant = GFNT_SLANT_ROMAN;

  result = discover_choose_name(face, GFNT_NAME_TYPOGRAPHIC_FAMILY,
      GFNT_NAME_FAMILY, set->allocator, &family, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = discover_choose_name(face, GFNT_NAME_TYPOGRAPHIC_SUBFAMILY,
      GFNT_NAME_SUBFAMILY, set->allocator, &style, error);
  if (result != GFNT_OK) {
    discover_free(set->allocator, family);
    return result;
  }
  if (!family) {
    family = discover_dup(set->allocator, "", 0);
  }
  if (!style) {
    style = discover_dup(set->allocator, "", 0);
  }
  if (!family || !style) {
    discover_free(set->allocator, family);
    discover_free(set->allocator, style);
    return GFNT_ERR_OOM;
  }

  result = gfnt_face_os2(face, &os2, error);
  if (result == GFNT_OK) {
    built.rec.weight = os2->weight_class;
    built.rec.width = os2->width_class;
    built.weight_stated = true;
    built.width_stated = true;
    built.slant_stated = true;
    if (os2->fs_selection & GFNT_OS2_FS_ITALIC) {
      built.rec.slant = GFNT_SLANT_ITALIC;
    } else if (os2->fs_selection & GFNT_OS2_FS_OBLIQUE) {
      built.rec.slant = GFNT_SLANT_OBLIQUE;
    }
    memcpy(built.rec.unicode_range, os2->unicode_range,
        sizeof built.rec.unicode_range);
  } else if (result != GFNT_ERR_UNSUPPORTED) {
    discover_free(set->allocator, family);
    discover_free(set->allocator, style);
    return result;
  }

  if (!built.weight_stated
      && discover_is_bitmap_container(built.rec.flavour)
      && discover_weight_word(style, strlen(style), &weight)) {
    built.rec.weight = weight;
    built.weight_stated = true;
  }

  result = discover_copy_name(face, GFNT_NAME_FULL, set->allocator, &full,
      &found, error);
  if (result != GFNT_OK) {
    discover_free(set->allocator, family);
    discover_free(set->allocator, style);
    return result;
  }
  if (found && full && full[0] == '-') {
    discover_apply_xlfd(&built, full);
  }
  discover_free(set->allocator, full);

  path_copy = discover_dup_str(set->allocator, path);
  if (!path_copy) {
    discover_free(set->allocator, family);
    discover_free(set->allocator, style);
    return GFNT_ERR_OOM;
  }
  built.rec.path = path_copy;
  built.rec.family = family;
  built.rec.style = style;

  grown = discover_grow(set->allocator, set->faces, &set->face_cap,
      set->face_count, sizeof *grown);
  if (!grown) {
    discover_free(set->allocator, path_copy);
    discover_free(set->allocator, family);
    discover_free(set->allocator, style);
    return GFNT_ERR_OOM;
  }
  set->faces = grown;
  set->faces[set->face_count++] = built;
  return GFNT_OK;
}

static bool discover_is_index_name(const char * name) {
  return strcmp(name, "fonts.dir") == 0 || strcmp(name, "fonts.alias") == 0;
}

static GFNT_Result discover_consider_file(GFNT_FontSet * set, const char * path,
    GFNT_Error * error) {
  char * canon = NULL;
  GFNT_Blob * blob = NULL;
  GFNT_Result result;
  size_t count = 0;
  size_t index;
  GCU_Path_Result canon_result;

  canon_result = gcu_path_canonicalize(path, set->allocator, &canon);
  if (canon_result == GCU_PATH_ERR_OOM) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "canonicalising a font path");
  }
  if (canon_result != GCU_PATH_OK) {
    return discover_add_skip(set, path, GFNT_ERR_IO);
  }
  if (discover_visited(set, canon)) {
    discover_free(set->allocator, canon);
    return GFNT_OK;
  }
  result = discover_mark(set, canon);
  if (result != GFNT_OK) {
    return result;
  }

  result = gfnt_blob_create_file(path, NULL, set->allocator, &blob, error);
  if (result != GFNT_OK) {
    if (result == GFNT_ERR_OOM) {
      return result;
    }
    return discover_add_skip(set, path, result);
  }

  result = gfnt_face_count(blob, NULL, &count, error);
  if (result == GFNT_ERR_FORMAT) {
    gfnt_blob_destroy(blob);
    return GFNT_OK;
  }
  if (result != GFNT_OK) {
    gfnt_blob_destroy(blob);
    if (result == GFNT_ERR_OOM) {
      return result;
    }
    return discover_add_skip(set, path, result);
  }

  for (index = 0; index < count; index++) {
    GFNT_Face * face = NULL;

    result = gfnt_face_load(blob, index, NULL, set->allocator, &face, error);
    if (result == GFNT_ERR_FORMAT) {
      continue;
    }
    if (result != GFNT_OK) {
      gfnt_face_free(face);
      if (result == GFNT_ERR_OOM) {
        gfnt_blob_destroy(blob);
        return result;
      }
      result = discover_add_skip(set, path, result);
      if (result != GFNT_OK) {
        gfnt_blob_destroy(blob);
        return result;
      }
      continue;
    }
    result = discover_fill_face(set, path, face, error);
    gfnt_face_free(face);
    if (result == GFNT_ERR_OOM) {
      gfnt_blob_destroy(blob);
      return result;
    }
    if (result != GFNT_OK) {
      result = discover_add_skip(set, path, result);
      if (result != GFNT_OK) {
        gfnt_blob_destroy(blob);
        return result;
      }
    }
  }
  gfnt_blob_destroy(blob);
  return GFNT_OK;
}

static GFNT_Result discover_walk(GFNT_FontSet * set, const char * path,
    int depth, bool root_must_open, GFNT_Error * error);

static GFNT_Result discover_walk_children(GFNT_FontSet * set, const char * path,
    int depth, char ** names, size_t name_count, GFNT_Error * error) {
  size_t i;
  GFNT_Result result = GFNT_OK;

  for (i = 0; i < name_count && result == GFNT_OK; i++) {
    char * child = NULL;
    GCU_File_Info info;
    GCU_File_Result stat;

    result = discover_join(set->allocator, path, names[i], &child);
    if (result != GFNT_OK) {
      break;
    }
    stat = gcu_file_stat(child, &info);
    if (stat != GCU_FILE_OK) {
      result = discover_add_skip(set, child, GFNT_ERR_IO);
      discover_free(set->allocator, child);
      continue;
    }
    if (info.type == GCU_FILE_TYPE_DIRECTORY) {
      if (depth + 1 > GFNT_DISCOVER_MAX_DEPTH) {
        result = discover_add_skip(set, child, GFNT_ERR_LIMIT);
      } else {
        result = discover_walk(set, child, depth + 1, false, error);
      }
    } else if (info.type == GCU_FILE_TYPE_REGULAR) {
      if (!discover_is_index_name(names[i])) {
        result = discover_consider_file(set, child, error);
      }
    }
    discover_free(set->allocator, child);
  }
  return result;
}

static GFNT_Result discover_walk(GFNT_FontSet * set, const char * path,
    int depth, bool root_must_open, GFNT_Error * error) {
  char * canon = NULL;
  GCU_Dir directory;
  char ** names = NULL;
  size_t name_count = 0;
  size_t name_cap = 0;
  GFNT_Result result = GFNT_OK;
  GCU_File_Result opened;

  if (depth > GFNT_DISCOVER_MAX_DEPTH) {
    return discover_add_skip(set, path, GFNT_ERR_LIMIT);
  }
  {
    GCU_Path_Result canon_result = gcu_path_canonicalize(path, set->allocator,
        &canon);
    if (canon_result == GCU_PATH_ERR_OOM) {
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "canonicalising a font directory");
    }
    if (canon_result != GCU_PATH_OK) {
      if (root_must_open) {
        return gfnt_error_set(error, GFNT_ERR_IO, 0, 0, GFNT_GLYPH_NONE,
            "a scan root is not a directory");
      }
      return discover_add_skip(set, path, GFNT_ERR_IO);
    }
  }
  if (discover_visited(set, canon)) {
    discover_free(set->allocator, canon);
    return GFNT_OK;
  }
  result = discover_mark(set, canon);
  if (result != GFNT_OK) {
    return result;
  }

  memset(&directory, 0, sizeof directory);
  opened = gcu_dir_open(&directory, path, set->allocator);
  if (opened != GCU_FILE_OK) {
    gcu_dir_close(&directory);
    if (root_must_open) {
      return gfnt_error_set(error, GFNT_ERR_IO, 0, 0, GFNT_GLYPH_NONE,
          "a scan root is not a directory");
    }
    return discover_add_skip(set, path, GFNT_ERR_IO);
  }

  for (;;) {
    const char * name = NULL;
    bool done = false;
    char ** grown;
    char * copy;

    opened = gcu_dir_read(&directory, &name, NULL, &done);
    if (opened != GCU_FILE_OK) {
      gcu_dir_close(&directory);
      result = discover_add_skip(set, path, GFNT_ERR_IO);
      goto done_names;
    }
    if (done) {
      break;
    }
    copy = discover_dup_str(set->allocator, name);
    grown = discover_grow(set->allocator, names, &name_cap, name_count,
        sizeof *grown);
    if (grown) {
      names = grown;
    }
    if (!copy || !grown) {
      discover_free(set->allocator, copy);
      gcu_dir_close(&directory);
      result = GFNT_ERR_OOM;
      goto done_names;
    }
    names[name_count++] = copy;
  }
  gcu_dir_close(&directory);

  result = discover_walk_children(set, path, depth, names, name_count, error);
  if (result == GFNT_OK) {
    result = discover_read_indexes(set, path);
  }

done_names:
  {
    size_t i;
    for (i = 0; i < name_count; i++) {
      discover_free(set->allocator, names[i]);
    }
    discover_free(set->allocator, names);
  }
  if (result == GFNT_ERR_OOM) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "scanning a font directory");
  }
  if (result == GFNT_ERR_INTERNAL) {
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
        "joining a path in a font directory");
  }
  return result;
}

static GFNT_FontSet * discover_new(const GFNT_Allocator * allocator) {
  GFNT_FontSet * set;

  set = allocator->calloc_fn(allocator->ctx, 1, sizeof *set);
  if (!set) {
    return NULL;
  }
  set->allocator = allocator;
  return set;
}

static bool discover_is_directory(const char * path) {
  GCU_File_Info info;

  if (gcu_file_stat(path, &info) != GCU_FILE_OK) {
    return false;
  }
  return info.type == GCU_FILE_TYPE_DIRECTORY;
}

static GFNT_Result discover_copy_aliases(GFNT_FontSet * set,
    const GFNT_FontAlias * aliases, size_t alias_count) {
  size_t i;

  for (i = 0; i < alias_count; i++) {
    GFNT_Result result = discover_add_alias(set, aliases[i].name,
        strlen(aliases[i].name), aliases[i].target);
    if (result != GFNT_OK) {
      return result;
    }
  }
  return GFNT_OK;
}

/**
 * A copy of @p path with a trailing separator removed, except for a root.
 * `C:\` stays `C:\`: stripping it would leave `C:`, that drive's current
 * directory.
 */
static char * discover_strip_root(const GFNT_Allocator * allocator,
    const char * path) {
  char * copy = discover_dup_str(allocator, path);
  size_t length;

  if (!copy) {
    return NULL;
  }
  length = strlen(copy);
  while (length > 1 && is_sep(copy[length - 1])) {
    if (length == 3 && copy[1] == ':'
        && ((copy[0] >= 'A' && copy[0] <= 'Z')
            || (copy[0] >= 'a' && copy[0] <= 'z'))) {
      break;
    }
    copy[--length] = '\0';
  }
  return copy;
}

static GFNT_Result discover_scan(const char * const * dirs, size_t dir_count,
    const GFNT_FontAlias * aliases, size_t alias_count,
    const GFNT_Allocator * allocator, bool root_must_open,
    GFNT_FontSet ** out_set, GFNT_Error * error) {
  GFNT_FontSet * set;
  size_t i;
  GFNT_Result result;

  set = discover_new(allocator);
  if (!set) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the font set");
  }
  result = discover_copy_aliases(set, aliases, alias_count);
  if (result != GFNT_OK) {
    gfnt_fontset_free(set);
    return gfnt_error_set(error, result, 0, 0, GFNT_GLYPH_NONE,
        "copying the caller's aliases");
  }
  for (i = 0; i < dir_count; i++) {
    char * root = discover_strip_root(allocator, dirs[i]);

    if (!root) {
      gfnt_fontset_free(set);
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "copying a scan root");
    }
    result = discover_walk(set, root, 0, root_must_open, error);
    discover_free(allocator, root);
    if (result != GFNT_OK) {
      gfnt_fontset_free(set);
      return result;
    }
  }
  // A skip records its own result. The scan itself succeeded, so a diagnostic
  // left by the last face that was skipped is not the call's answer.
  gfnt_error_clear(error);
  *out_set = set;
  return GFNT_OK;
}

static GFNT_Result discover_check_aliases(const GFNT_FontAlias * aliases,
    size_t alias_count, GFNT_Error * error) {
  size_t i;

  if (!aliases && alias_count != 0) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "an alias count with no aliases");
  }
  for (i = 0; i < alias_count; i++) {
    if (!aliases[i].name || !aliases[i].target) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "an alias with no name or no target");
    }
  }
  return GFNT_OK;
}

GFNT_Result gfnt_fontset_scan(const char * const * dirs, size_t dir_count,
    const GFNT_FontAlias * aliases, size_t alias_count,
    const GFNT_Allocator * allocator, GFNT_FontSet ** out_set,
    GFNT_Error * error) {
  size_t i;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!out_set || !dirs || dir_count == 0) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no directory list, or nowhere to put the set");
  }
  result = discover_check_aliases(aliases, alias_count, error);
  if (result != GFNT_OK) {
    return result;
  }
  for (i = 0; i < dir_count; i++) {
    if (!dirs[i]) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "a directory entry is NULL");
    }
    if (!discover_is_directory(dirs[i])) {
      return gfnt_error_set(error, GFNT_ERR_IO, 0, 0, GFNT_GLYPH_NONE,
          "a scan root is not a directory");
    }
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  return discover_scan(dirs, dir_count, aliases, alias_count, allocator, true,
      out_set, error);
}

static void discover_free_dir_list(const GFNT_Allocator * allocator,
    char ** dirs, size_t count) {
  size_t i;

  if (!dirs) {
    return;
  }
  for (i = 0; i < count; i++) {
    discover_free(allocator, dirs[i]);
  }
  discover_free(allocator, dirs);
}

GFNT_Result gfnt_fontset_scan_system(const GFNT_FontAlias * aliases,
    size_t alias_count, const GFNT_Allocator * allocator,
    GFNT_FontSet ** out_set, GFNT_Error * error) {
  char ** dirs = NULL;
  size_t dir_count = 0;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!out_set) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "nowhere to put the set");
  }
  result = discover_check_aliases(aliases, alias_count, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  result = gfnt_discover_system_dirs(allocator, &dirs, &dir_count, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = discover_scan((const char * const *)dirs, dir_count, aliases,
      alias_count, allocator, false, out_set, error);
  discover_free_dir_list(allocator, dirs, dir_count);
  return result;
}

size_t gfnt_fontset_count(const GFNT_FontSet * set) {
  return set ? set->face_count : 0;
}

const GFNT_FontRecord * gfnt_fontset_at(const GFNT_FontSet * set,
    size_t index) {
  if (!set || index >= set->face_count) {
    return NULL;
  }
  return &set->faces[index].rec;
}

size_t gfnt_fontset_alias_count(const GFNT_FontSet * set) {
  return set ? set->alias_count : 0;
}

const GFNT_FontAlias * gfnt_fontset_alias_at(const GFNT_FontSet * set,
    size_t index) {
  if (!set || index >= set->alias_count) {
    return NULL;
  }
  return &set->aliases[index];
}

size_t gfnt_fontset_skip_count(const GFNT_FontSet * set) {
  return set ? set->skip_count : 0;
}

const GFNT_FontSkip * gfnt_fontset_skip_at(const GFNT_FontSet * set,
    size_t index) {
  if (!set || index >= set->skip_count) {
    return NULL;
  }
  return &set->skips[index];
}

GFNT_Result gfnt_fontset_resolve_alias(const GFNT_FontSet * set,
    const char * name, const char ** out_target, GFNT_Error * error) {
  const char * current;
  int step;

  gfnt_error_clear(error);
  if (!set || !name || !out_target) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no set, no name, or nowhere to put the alias");
  }
  current = name;
  for (step = 1; step <= GFNT_DISCOVER_ALIAS_STEPS; step++) {
    size_t i;
    const GFNT_FontAlias * hit = NULL;

    for (i = 0; i < set->alias_count; i++) {
      if (strcmp(set->aliases[i].name, current) == 0) {
        hit = &set->aliases[i];
        break;
      }
    }
    if (!hit) {
      *out_target = current;
      return GFNT_OK;
    }
    /* An XLFD family stored as the alias's own name has settled. Following
       it would only cycle. A longer cycle still hits the step limit. */
    if (strcmp(hit->target, current) == 0) {
      *out_target = hit->target;
      return GFNT_OK;
    }
    if (step == GFNT_DISCOVER_ALIAS_STEPS) {
      return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
          "an alias chain longer than 7 steps");
    }
    current = hit->target;
  }
  return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
      "an alias chain longer than 7 steps");
}

void gfnt_fontset_free(GFNT_FontSet * set) {
  size_t i;
  const GFNT_Allocator * allocator;

  if (!set) {
    return;
  }
  allocator = set->allocator;
  for (i = 0; i < set->face_count; i++) {
    discover_free(allocator, (void *)set->faces[i].rec.path);
    discover_free(allocator, (void *)set->faces[i].rec.family);
    discover_free(allocator, (void *)set->faces[i].rec.style);
  }
  discover_free(allocator, set->faces);
  for (i = 0; i < set->alias_count; i++) {
    discover_free(allocator, (void *)set->aliases[i].name);
    discover_free(allocator, (void *)set->aliases[i].target);
  }
  discover_free(allocator, set->aliases);
  for (i = 0; i < set->skip_count; i++) {
    discover_free(allocator, (void *)set->skips[i].path);
  }
  discover_free(allocator, set->skips);
  for (i = 0; i < set->visited_count; i++) {
    discover_free(allocator, set->visited[i]);
  }
  discover_free(allocator, set->visited);
  discover_free(allocator, set);
}
