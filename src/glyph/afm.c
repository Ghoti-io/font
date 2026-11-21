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
 * Reading an Adobe Font Metrics file (include/ghoti.io/font/afm.h).
 *
 * The file is copied once into an arena and tokenised in place, so every string a result
 * offers points into it (a ligature list, which is assembled from several commands, is the
 * one built string). The arrays of characters and of kerning pairs are sized from the counts
 * the sections state, after those are checked against the limits and against what the file's
 * size could hold, and a section whose lines disagree with its count is corrupt rather than
 * quietly short.
 */

#include <ghoti.io/font/afm.h>
#include <stdlib.h>
#include <string.h>

typedef struct AfmKern {
  const char * first;
  const char * second;
  int32_t x;
  uint32_t order; ///< File order, for a stable sort.
} AfmKern;

typedef struct AfmName {
  const char * name;
  uint32_t index;
} AfmName;

struct GFNT_Afm {
  const GFNT_Allocator * allocator;
  char * text;          ///< The copy of the file, tokenised in place.
  GFNT_AfmInfo info;
  GFNT_AfmGlyph * glyphs;
  size_t glyph_count;
  AfmName * by_name;    ///< The named glyphs, sorted by name then index.
  size_t by_name_count;
  int32_t by_code[256]; ///< The first glyph at each code, or -1.
  AfmKern * kerns;      ///< Sorted by (first, second), ties by file order.
  size_t kern_count;
};

/* ---- scanning ---------------------------------------------------------- */

static bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

static char * skip_space(char * p) {
  while (is_space(*p)) {
    p++;
  }
  return p;
}

/** The first token of @p p, NUL-terminated in place; *rest is what follows it, spaces skipped. */
static char * token(char * p, char ** rest) {
  char * start = skip_space(p);
  char * end = start;

  while (*end && !is_space(*end)) {
    end++;
  }
  if (*end) {
    *end++ = '\0';
  }
  *rest = skip_space(end);
  return start;
}

/** The first token of @p p copied out (truncated to @p capacity - 1); *rest follows it. Changes nothing. */
static void peek(const char * p, char * out, size_t capacity, char ** rest) {
  size_t n = 0;

  while (is_space(*p)) {
    p++;
  }
  while (*p && !is_space(*p)) {
    if (n + 1 < capacity) {
      out[n++] = *p;
    }
    p++;
  }
  out[n] = '\0';
  while (is_space(*p)) {
    p++;
  }
  *rest = (char *)p;
}

static void trim_right(char * s) {
  size_t n = strlen(s);

  while (n && is_space(s[n - 1])) {
    s[--n] = '\0';
  }
}

/**
 * A decimal number, read without the locale, as thousandths: a sign, digits, and optionally a
 * point and digits. A fourth decimal rounds the third half away from zero; one that does not
 * fit an int32 in thousandths is refused.
 */
static bool number(char ** p, int32_t * out) {
  char * s = skip_space(*p);
  bool negative = false;
  int64_t whole = 0;
  int64_t fraction = 0;   /* the first three decimals, as thousandths */
  unsigned places = 0;
  unsigned digits = 0;
  bool round_up = false;
  bool seen = false;
  int64_t value;

  if (*s == '-' || *s == '+') {
    negative = *s == '-';
    s++;
  }
  for (; *s >= '0' && *s <= '9'; s++) {
    seen = true;
    if (++digits > 12) {
      return false;
    }
    whole = whole * 10 + (*s - '0');
  }
  if (*s == '.') {
    s++;
    for (; *s >= '0' && *s <= '9'; s++) {
      seen = true;
      if (++digits > 18) {
        return false;
      }
      if (places < 3) {
        fraction = fraction * 10 + (*s - '0');
        places++;
      }
      else if (places == 3) {
        round_up = *s >= '5';
        places++;
      }
    }
  }
  if (!seen || (*s && !is_space(*s))) {
    return false;
  }
  while (places < 3) {
    fraction *= 10;
    places++;
  }
  value = whole * 1000 + fraction + (round_up ? 1 : 0);
  if (value > 2147483647) {
    return false;
  }
  *out = (int32_t)(negative ? -value : value);
  *p = s;
  return true;
}

static bool integer(char ** p, int32_t * out) {
  int32_t v;

  if (!number(p, &v) || v % 1000 != 0) {
    return false;
  }
  *out = v / 1000;
  return true;
}

static bool at_end(char * p) {
  return *skip_space(p) == '\0';
}

static bool keyword(const char * got, const char * want) {
  return strcmp(got, want) == 0;
}

#define AFM_CORRUPT(msg) gfnt_error_set(error, GFNT_ERR_CORRUPT, 0, 0, GFNT_GLYPH_NONE, (msg))

/* ---- the object -------------------------------------------------------- */

void gfnt_afm_free(GFNT_Afm * afm) {
  const GFNT_Allocator * a;
  size_t i;

  if (!afm) {
    return;
  }
  a = afm->allocator;
  for (i = 0; i < afm->glyph_count; i++) {
    a->free_fn(a->ctx, (void *)afm->glyphs[i].ligatures);   /* built, not a slice of the text */
  }
  a->free_fn(a->ctx, afm->glyphs);
  a->free_fn(a->ctx, afm->by_name);
  a->free_fn(a->ctx, afm->kerns);
  a->free_fn(a->ctx, afm->text);
  a->free_fn(a->ctx, afm);
}

/** Append @p successor and @p ligature to a ligature list (freeing and replacing @p *list). */
static bool add_ligature(const GFNT_Allocator * a, char ** list, const char * successor,
    const char * ligature) {
  size_t old = *list ? strlen(*list) : 0;
  size_t s = strlen(successor);
  size_t l = strlen(ligature);
  char * grown = a->malloc_fn(a->ctx, old + (old ? 1 : 0) + s + 1 + l + 1);
  char * at;

  if (!grown) {
    return false;
  }
  at = grown;
  if (old) {
    memcpy(at, *list, old);
    at += old;
    *at++ = ' ';
  }
  memcpy(at, successor, s);
  at += s;
  *at++ = ' ';
  memcpy(at, ligature, l + 1);
  a->free_fn(a->ctx, *list);
  *list = grown;
  return true;
}

/** One line of character metrics: commands separated by semicolons. */
static GFNT_Result read_glyph(const GFNT_Allocator * a, char * line, GFNT_AfmGlyph * g,
    GFNT_Error * error) {
  char * cursor = line;
  char * ligatures = NULL;
  GFNT_Result failed = GFNT_OK;

  memset(g, 0, sizeof *g);
  g->code = -1;
  while (*cursor && failed == GFNT_OK) {
    char * end = strchr(cursor, ';');
    char * rest;
    char * key;

    if (end) {
      *end = '\0';
    }
    key = token(cursor, &rest);
    if (*key == '\0') {
      /* an empty command, or the blank after the last semicolon */
    }
    else if (keyword(key, "C")) {
      if (!integer(&rest, &g->code) || !at_end(rest)) {
        failed = AFM_CORRUPT("a character code that is not a number");
      }
    }
    else if (keyword(key, "CH")) {
      unsigned long v = 0;
      char * p = rest;
      int digits = 0;

      if (*p == '<') {
        p++;
      }
      for (; digits <= 8 && ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F')
          || (*p >= 'a' && *p <= 'f')); p++) {
        v = v * 16 + (unsigned long)(*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
        digits++;
      }
      if (digits == 0 || digits > 8 || v > 0x7FFFFFFFul) {
        failed = AFM_CORRUPT("a hexadecimal character code that is not one");
      }
      else {
        g->code = (int32_t)v;
      }
    }
    else if (keyword(key, "WX") || keyword(key, "W0X")) {
      if (!number(&rest, &g->width_x_milli) || !at_end(rest)) {
        failed = AFM_CORRUPT("an advance that is not a number");
      }
    }
    else if (keyword(key, "WY") || keyword(key, "W0Y")) {
      if (!number(&rest, &g->width_y_milli) || !at_end(rest)) {
        failed = AFM_CORRUPT("an advance that is not a number");
      }
    }
    else if (keyword(key, "W") || keyword(key, "W0")) {
      if (!number(&rest, &g->width_x_milli) || !number(&rest, &g->width_y_milli) || !at_end(rest)) {
        failed = AFM_CORRUPT("an advance vector that is not two numbers");
      }
    }
    else if (keyword(key, "N")) {
      char * after;
      char * name = token(rest, &after);

      if (*name == '\0' || *after) {
        failed = AFM_CORRUPT("a glyph name that is missing or has a space in it");
      }
      else {
        g->name = name;
      }
    }
    else if (keyword(key, "B")) {
      int i;

      for (i = 0; i < 4 && failed == GFNT_OK; i++) {
        if (!number(&rest, &g->bbox_milli[i])) {
          failed = AFM_CORRUPT("a character box that is not four numbers");
        }
      }
      if (failed == GFNT_OK && !at_end(rest)) {
        failed = AFM_CORRUPT("a character box with more than four numbers");
      }
      g->has_bbox = failed == GFNT_OK;
    }
    else if (keyword(key, "L")) {
      char * after;
      char * successor = token(rest, &after);
      char * ligature = token(after, &after);

      if (*successor == '\0' || *ligature == '\0' || *after) {
        failed = AFM_CORRUPT("a ligature that does not name a successor and a ligature");
      }
      else if (!add_ligature(a, &ligatures, successor, ligature)) {
        failed = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "reading a ligature");
      }
    }
    /* Every other command (the vertical metrics, VV) is read past and not offered. */
    cursor = end ? end + 1 : cursor + strlen(cursor);
  }
  if (failed != GFNT_OK) {
    a->free_fn(a->ctx, ligatures);
    return failed;
  }
  g->ligatures = ligatures;
  return GFNT_OK;
}

static int name_cmp(const void * x, const void * y) {
  const AfmName * a = x;
  const AfmName * b = y;
  int order = strcmp(a->name, b->name);

  if (order) {
    return order;
  }
  return a->index < b->index ? -1 : a->index > b->index;
}

static int kern_cmp(const void * x, const void * y) {
  const AfmKern * a = x;
  const AfmKern * b = y;
  int order = strcmp(a->first, b->first);

  if (order == 0) {
    order = strcmp(a->second, b->second);
  }
  if (order) {
    return order;
  }
  return a->order < b->order ? -1 : a->order > b->order;
}

/** What @p rest says, as a global key of the header; false when the key is not one this reads. */
static bool read_header_key(GFNT_AfmInfo * info, const char * key, char * rest, bool * ok) {
  *ok = true;
  trim_right(rest);
  if (keyword(key, "FontName")) {
    info->font_name = rest;
  }
  else if (keyword(key, "FullName")) {
    info->full_name = rest;
  }
  else if (keyword(key, "FamilyName")) {
    info->family_name = rest;
  }
  else if (keyword(key, "Weight")) {
    info->weight = rest;
  }
  else if (keyword(key, "Version")) {
    info->version = rest;
  }
  else if (keyword(key, "Notice")) {
    info->notice = rest;
  }
  else if (keyword(key, "EncodingScheme")) {
    info->encoding_scheme = rest;
  }
  else if (keyword(key, "CharacterSet")) {
    info->character_set = rest;
  }
  else if (keyword(key, "ItalicAngle")) {
    *ok = number(&rest, &info->italic_angle_milli) && at_end(rest);
  }
  else if (keyword(key, "IsFixedPitch")) {
    if (keyword(rest, "true")) {
      info->is_fixed_pitch = true;
    }
    else if (keyword(rest, "false")) {
      info->is_fixed_pitch = false;
    }
    else {
      *ok = false;
    }
  }
  else if (keyword(key, "UnderlinePosition")) {
    *ok = number(&rest, &info->underline_position_milli) && at_end(rest);
  }
  else if (keyword(key, "UnderlineThickness")) {
    *ok = number(&rest, &info->underline_thickness_milli) && at_end(rest);
  }
  else if (keyword(key, "FontBBox")) {
    int i;

    for (i = 0; i < 4 && *ok; i++) {
      *ok = number(&rest, &info->bbox_milli[i]);
    }
    *ok = *ok && at_end(rest);
    info->has_bbox = *ok;
  }
  else if (keyword(key, "CapHeight")) {
    *ok = number(&rest, &info->cap_height_milli) && at_end(rest);
  }
  else if (keyword(key, "XHeight")) {
    *ok = number(&rest, &info->x_height_milli) && at_end(rest);
  }
  else if (keyword(key, "Ascender")) {
    *ok = number(&rest, &info->ascender_milli) && at_end(rest);
  }
  else if (keyword(key, "Descender")) {
    *ok = number(&rest, &info->descender_milli) && at_end(rest);
  }
  else if (keyword(key, "StdHW")) {
    *ok = number(&rest, &info->std_hw_milli) && at_end(rest);
  }
  else if (keyword(key, "StdVW")) {
    *ok = number(&rest, &info->std_vw_milli) && at_end(rest);
  }
  else {
    return false;
  }
  return true;
}

typedef enum {
  SECTION_NONE = 0,
  SECTION_CHARS,
  SECTION_KERNPAIRS
} Section;

static GFNT_Result parse_text(GFNT_Afm * afm, const GFNT_Limits * limits, GFNT_Error * error) {
  const GFNT_Allocator * a = afm->allocator;
  char * p = afm->text;
  bool started = false;
  bool finished = false;
  Section section = SECTION_NONE;
  const char * skip_until = NULL;
  size_t declared = 0;          /* the count the open section stated */
  size_t seen = 0;              /* the lines of it read so far */
  bool had_chars = false;
  size_t text_size = strlen(afm->text);
  size_t i;
  GFNT_Result result;

  while (*p && !finished) {
    char * line = p;
    char * end = p;
    char key[40];
    char * rest;
    size_t length;

    while (*end && *end != '\n' && *end != '\r') {
      end++;
    }
    length = (size_t)(end - line);
    if (*end == '\r' && end[1] == '\n') {
      *end++ = '\0';
    }
    if (*end) {
      *end++ = '\0';
    }
    p = end;
    if (length > limits->max_line_length) {
      return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
          "a line of the AFM file is longer than the line limit");
    }
    peek(line, key, sizeof key, &rest);
    if (key[0] == '\0') {
      continue;
    }
    if (!started) {
      if (!keyword(key, "StartFontMetrics")) {
        return AFM_CORRUPT("the file does not begin with StartFontMetrics");
      }
      started = true;
      continue;
    }
    if (skip_until) {
      if (keyword(key, skip_until)) {
        skip_until = NULL;
      }
      continue;
    }
    if (keyword(key, "EndFontMetrics")) {
      if (section != SECTION_NONE) {
        return AFM_CORRUPT("EndFontMetrics inside a section that was not closed");
      }
      finished = true;
      continue;
    }
    if (section != SECTION_NONE && keyword(key, "Comment")) {
      continue;
    }
    if (section == SECTION_CHARS) {
      if (keyword(key, "EndCharMetrics")) {
        if (seen != declared) {
          return AFM_CORRUPT("the character metrics are not as many as StartCharMetrics says");
        }
        section = SECTION_NONE;
        continue;
      }
      if (seen >= declared) {
        return AFM_CORRUPT("more character metrics than StartCharMetrics says");
      }
      result = read_glyph(a, line, &afm->glyphs[seen], error);
      if (result != GFNT_OK) {
        return result;
      }
      seen++;
      afm->glyph_count = seen;
      continue;
    }
    if (section == SECTION_KERNPAIRS) {
      if (keyword(key, "EndKernPairs")) {
        if (seen != declared) {
          return AFM_CORRUPT("the kerning pairs are not as many as StartKernPairs says");
        }
        section = SECTION_NONE;
        continue;
      }
      if (seen >= declared) {
        return AFM_CORRUPT("more kerning pairs than StartKernPairs says");
      }
      seen++;
      if (keyword(key, "KPX") || keyword(key, "KP")) {
        char * after;
        char * first;
        char * second;
        int32_t x;
        int32_t y;

        (void)token(line, &after);
        first = token(after, &after);
        second = token(after, &after);
        if (*first == '\0' || *second == '\0' || !number(&after, &x)
            || (keyword(key, "KP") ? !number(&after, &y) : false) || !at_end(after)) {
          return AFM_CORRUPT("a kerning pair that does not name two glyphs and an amount");
        }
        afm->kerns[afm->kern_count].first = first;
        afm->kerns[afm->kern_count].second = second;
        afm->kerns[afm->kern_count].x = x;
        afm->kerns[afm->kern_count].order = (uint32_t)afm->kern_count;
        afm->kern_count++;
      }
      else if (!keyword(key, "KPH") && !keyword(key, "KPY")) {
        return AFM_CORRUPT("a line of a kerning section that is not a kerning pair");
      }
      continue;
    }

    if (keyword(key, "StartCharMetrics")) {
      char * count_text = rest;
      int32_t n;

      if (had_chars) {
        return AFM_CORRUPT("two sections of character metrics");
      }
      if (!integer(&count_text, &n) || n < 0 || !at_end(count_text)) {
        return AFM_CORRUPT("StartCharMetrics without a count");
      }
      if ((size_t)n > limits->max_glyphs || (size_t)n > text_size) {
        return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
            "more characters than the glyph limit allows, or than the file could hold");
      }
      afm->glyphs = a->calloc_fn(a->ctx, (size_t)n ? (size_t)n : 1, sizeof *afm->glyphs);
      if (!afm->glyphs) {
        return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "listing the characters");
      }
      had_chars = true;
      declared = (size_t)n;
      seen = 0;
      section = SECTION_CHARS;
    }
    else if (keyword(key, "StartKernPairs") || keyword(key, "StartKernPairs0")) {
      char * count_text = rest;
      int32_t n;

      if (afm->kerns) {
        return AFM_CORRUPT("two sections of kerning pairs");
      }
      if (!integer(&count_text, &n) || n < 0 || !at_end(count_text)) {
        return AFM_CORRUPT("StartKernPairs without a count");
      }
      if ((size_t)n > text_size) {
        return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
            "more kerning pairs than the file could hold");
      }
      afm->kerns = a->calloc_fn(a->ctx, (size_t)n ? (size_t)n : 1, sizeof *afm->kerns);
      if (!afm->kerns) {
        return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "listing the kerning pairs");
      }
      declared = (size_t)n;
      seen = 0;
      section = SECTION_KERNPAIRS;
    }
    else if (keyword(key, "StartKernPairs1")) {
      skip_until = "EndKernPairs";
    }
    else if (keyword(key, "StartTrackKern")) {
      skip_until = "EndTrackKern";
    }
    else if (keyword(key, "StartComposites")) {
      skip_until = "EndComposites";
    }
    else if (keyword(key, "StartDirection")) {
      char * d = rest;
      int32_t direction;

      if (!integer(&d, &direction) || !at_end(d)) {
        return AFM_CORRUPT("StartDirection without a direction");
      }
      if (direction == 1) {
        skip_until = "EndDirection";
      }
    }
    else if (keyword(key, "StartFontMetrics") || keyword(key, "EndCharMetrics")
        || keyword(key, "EndKernPairs")) {
      return AFM_CORRUPT("a section marker where none is open");
    }
    else if (!keyword(key, "Comment") && !keyword(key, "StartKernData")
        && !keyword(key, "EndKernData") && !keyword(key, "EndDirection")
        && !keyword(key, "EndTrackKern") && !keyword(key, "EndComposites")) {
      char * value;
      bool ok = true;

      (void)token(line, &value);
      if (read_header_key(&afm->info, key, value, &ok) && !ok) {
        return AFM_CORRUPT("a global key whose value is not what the key takes");
      }
    }
  }
  if (!started || !finished) {
    return AFM_CORRUPT("the file has no EndFontMetrics");
  }
  if (skip_until) {
    return AFM_CORRUPT("a skipped section is not closed");
  }

  /* The lookups. */
  afm->by_name = a->calloc_fn(a->ctx, afm->glyph_count ? afm->glyph_count : 1, sizeof *afm->by_name);
  if (!afm->by_name) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "indexing the characters");
  }
  for (i = 0; i < afm->glyph_count; i++) {
    const GFNT_AfmGlyph * g = &afm->glyphs[i];

    if (g->name) {
      afm->by_name[afm->by_name_count].name = g->name;
      afm->by_name[afm->by_name_count].index = (uint32_t)i;
      afm->by_name_count++;
    }
    if (g->code >= 0 && g->code < 256 && afm->by_code[g->code] < 0) {
      afm->by_code[g->code] = (int32_t)i;
    }
  }
  qsort(afm->by_name, afm->by_name_count, sizeof *afm->by_name, name_cmp);
  if (afm->kerns) {
    qsort(afm->kerns, afm->kern_count, sizeof *afm->kerns, kern_cmp);
  }
  return GFNT_OK;
}

GFNT_Result gfnt_afm_parse(const void * data, size_t size, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Afm ** out_afm, GFNT_Error * error) {
  GFNT_Limits local;
  GFNT_Afm * afm;
  GFNT_Result result;
  size_t i;
  int c;

  if (!out_afm || (!data && size)) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "the bytes of the file and a place for the result are required");
  }
  if (!limits) {
    gfnt_limits_default(&local);
    limits = &local;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  if (size > limits->max_blob_bytes) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "the AFM file is larger than the blob limit");
  }
  for (i = 0; i < size; i++) {
    if (((const unsigned char *)data)[i] == 0) {
      return AFM_CORRUPT("a NUL byte in the file");
    }
  }
  afm = allocator->calloc_fn(allocator->ctx, 1, sizeof *afm);
  if (!afm) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "allocating the AFM");
  }
  afm->allocator = allocator;
  for (c = 0; c < 256; c++) {
    afm->by_code[c] = -1;
  }
  afm->text = allocator->malloc_fn(allocator->ctx, size + 1);
  if (!afm->text) {
    gfnt_afm_free(afm);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "copying the AFM text");
  }
  if (size) {
    memcpy(afm->text, data, size);
  }
  afm->text[size] = '\0';
  result = parse_text(afm, limits, error);
  if (result != GFNT_OK) {
    gfnt_afm_free(afm);
    return result;
  }
  *out_afm = afm;
  return GFNT_OK;
}

/* ---- access ------------------------------------------------------------ */

GFNT_AfmInfo gfnt_afm_info(const GFNT_Afm * afm) {
  GFNT_AfmInfo none;

  memset(&none, 0, sizeof none);
  return afm ? afm->info : none;
}

size_t gfnt_afm_glyph_count(const GFNT_Afm * afm) {
  return afm ? afm->glyph_count : 0;
}

GFNT_Result gfnt_afm_glyph_at(const GFNT_Afm * afm, size_t index, GFNT_AfmGlyph * out) {
  if (!afm || !out || index >= afm->glyph_count) {
    return GFNT_ERR_INVALID;
  }
  *out = afm->glyphs[index];
  return GFNT_OK;
}

bool gfnt_afm_glyph_for_name(const GFNT_Afm * afm, const char * name, GFNT_AfmGlyph * out) {
  size_t low = 0;
  size_t high;

  if (!afm || !name || !out) {
    return false;
  }
  high = afm->by_name_count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;

    if (strcmp(afm->by_name[mid].name, name) < 0) {
      low = mid + 1;
    }
    else {
      high = mid;
    }
  }
  if (low < afm->by_name_count && strcmp(afm->by_name[low].name, name) == 0) {
    *out = afm->glyphs[afm->by_name[low].index];
    return true;
  }
  return false;
}

bool gfnt_afm_glyph_for_code(const GFNT_Afm * afm, int32_t code, GFNT_AfmGlyph * out) {
  if (!afm || !out || code < 0 || code > 255 || afm->by_code[code] < 0) {
    return false;
  }
  *out = afm->glyphs[afm->by_code[code]];
  return true;
}

bool gfnt_afm_kern(const GFNT_Afm * afm, const char * first, const char * second,
    int32_t * out_kern_milli) {
  size_t low = 0;
  size_t high;

  if (!afm || !first || !second || !out_kern_milli) {
    return false;
  }
  high = afm->kern_count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    int order = strcmp(afm->kerns[mid].first, first);

    if (order == 0) {
      order = strcmp(afm->kerns[mid].second, second);
    }
    if (order < 0) {
      low = mid + 1;
    }
    else {
      high = mid;
    }
  }
  if (low < afm->kern_count && strcmp(afm->kerns[low].first, first) == 0
      && strcmp(afm->kerns[low].second, second) == 0) {
    *out_kern_milli = afm->kerns[low].x;
    return true;
  }
  return false;
}

size_t gfnt_afm_kern_count(const GFNT_Afm * afm) {
  return afm ? afm->kern_count : 0;
}
