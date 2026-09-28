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
 * `name`: the record list, the decode, and the preference order.
 *
 * documentation/design.md section 7.2 and M15. The encoding decides the
 * decode: UTF-16BE for the Unicode and Windows platforms, a generated table for
 * the single-byte Macintosh encodings, and ::GFNT_ERR_UNSUPPORTED for the
 * multi-byte ones - which is a different answer from ::GFNT_ERR_CORRUPT,
 * because the font is fine and this library is the one that is short a table.
 *
 * **The Macintosh encoding is not chosen by the encoding ID alone.** `platEncID`
 * 0 is keyed by `langID`: it is Mac Roman for most languages and Icelandic,
 * Turkish, Croatian, Central European or Romanian for thirteen of them. Reading
 * it as Mac Roman throughout would decode those records to the wrong letters
 * without failing, which is the worst shape of wrong available here. The
 * decision is a generated table, `src/name/mac_encodings.h`, and the rule it
 * encodes came from the reference rather than from memory (section 14).
 *
 * Reference: OpenType Specification 1.9, "name - Naming Table", and its
 * platform and encoding ID appendices.
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/name.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../bitmap/bitmap.h"
#include "../sfnt/sfnt.h"
#include "../type1/type1.h"
#include "mac_encodings.h"

/** The `name` tag, spelled once. */
#define GFNT_NAME_TAG GFNT_TAG('n', 'a', 'm', 'e')

/** Bytes in a `name` header: format, count, stringOffset. */
#define GFNT_NAME_HEADER_BYTES 6

/** Bytes in one name record. */
#define GFNT_NAME_RECORD_BYTES 12

/** US English under the Windows platform: the first choice. */
#define GFNT_NAME_WINDOWS_ENGLISH 0x0409u

/**
 * The header of a `name` table: a reader over it, the record count, and where
 * the string storage begins.
 */
static GFNT_Result gfnt_name_open(const GFNT_Face * face, GFNT_Reader * out,
    size_t * out_count, size_t * out_storage, GFNT_Error * error) {
  uint16_t format = 0;
  uint16_t count = 0;
  uint16_t storage = 0;
  GFNT_Result result;

  result = gfnt_face_table_reader(face, GFNT_NAME_TAG, out, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_u16(out, &format) != GFNT_OK
      || gfnt_read_u16(out, &count) != GFNT_OK
      || gfnt_read_u16(out, &storage) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  // Format 1 adds language-tag records after the name records, for the
  // language IDs at 0x8000 and above. They sit between the records and the
  // string storage, so a reader that finds the storage by its own offset - as
  // this one does - needs nothing from them, and a record's language ID is
  // reported raw.
  if (format != 0 && format != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "a name table format this library does not read");
  }
  if (count > face->limits.max_name_records) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_NAME_TAG, 2,
        GFNT_GLYPH_NONE, "more name records than GFNT_Limits::max_name_records");
  }
  if (storage > out->length) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_NAME_TAG, 4,
        GFNT_GLYPH_NONE, "the name table's string storage starts past its end");
  }

  if (out_count) {
    *out_count = count;
  }
  if (out_storage) {
    *out_storage = storage;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_name_count(const GFNT_Face * face, size_t * out_count,
    GFNT_Error * error) {
  GFNT_Reader reader;
  size_t count = 0;
  GFNT_Result result;

  if (!face || !out_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "no face, or nowhere to put the count");
  }
  result = gfnt_name_open(face, &reader, &count, NULL, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_count = count;
  return GFNT_OK;
}

GFNT_Result gfnt_face_name_at(const GFNT_Face * face, size_t index,
    GFNT_NameRecord * out_record, GFNT_Error * error) {
  GFNT_Reader reader;
  size_t count = 0;
  size_t at;
  uint16_t length = 0;
  uint16_t offset = 0;
  GFNT_Result result;

  if (!face || !out_record) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "no face, or nowhere to put the record");
  }
  result = gfnt_name_open(face, &reader, &count, NULL, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (index >= count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_NAME_TAG, 2,
        GFNT_GLYPH_NONE, "the name table has no record at that index");
  }

  at = GFNT_NAME_HEADER_BYTES + index * GFNT_NAME_RECORD_BYTES;
  *out_record = (GFNT_NameRecord) {0};
  if (gfnt_reader_u16_at(&reader, at, &out_record->platform_id) != GFNT_OK
      || gfnt_reader_u16_at(&reader, at + 2, &out_record->encoding_id)
          != GFNT_OK
      || gfnt_reader_u16_at(&reader, at + 4, &out_record->language_id)
          != GFNT_OK
      || gfnt_reader_u16_at(&reader, at + 6, &out_record->name_id) != GFNT_OK
      || gfnt_reader_u16_at(&reader, at + 8, &length) != GFNT_OK
      || gfnt_reader_u16_at(&reader, at + 10, &offset) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  out_record->length = length;
  out_record->offset = offset;
  return GFNT_OK;
}

/**
 * Whether this library can decode a record in this platform and encoding.
 */
/**
 * Which generated table decodes a Macintosh record, or ::GFNT_MAC_TABLE_NONE.
 *
 * Both ends of the relation, because one end does not decide: the rules are
 * scanned for an exact (encoding, language) match first and for the encoding's
 * default second. A linear scan over twenty-three rules is not worth indexing.
 */
static int32_t gfnt_name_mac_table(uint16_t encoding, uint16_t language) {
  int32_t fallback = GFNT_MAC_TABLE_NONE;
  bool named = false;

  for (size_t i = 0; i < GFNT_MAC_RULE_COUNT; ++i) {
    if (gfnt_mac_rules[i].encoding != encoding) {
      continue;
    }
    if (gfnt_mac_rules[i].language == (int32_t)language) {
      return gfnt_mac_rules[i].table;
    }
    if (gfnt_mac_rules[i].language == GFNT_MAC_LANGUAGE_ANY) {
      fallback = gfnt_mac_rules[i].table;
      named = true;
    }
  }
  // An encoding ID with no rule at all is not the same as one whose rule says
  // "no table": the first is an ID this library has never heard of and the
  // second is a multi-byte encoding it deliberately refuses. Both refuse, and
  // the diagnostic below tells them apart.
  return named ? fallback : GFNT_MAC_TABLE_NONE;
}

static bool gfnt_name_decodable(uint16_t platform, uint16_t encoding,
    uint16_t language) {
  // Every string under the Unicode and Windows platforms is UTF-16BE,
  // whichever encoding ID it names - including the Windows symbol encoding,
  // whose strings are ordinary UCS-2 even though its cmap is not.
  if (platform == GFNT_PLATFORM_UNICODE || platform == GFNT_PLATFORM_WINDOWS) {
    return true;
  }
  if (platform == GFNT_PLATFORM_MACINTOSH) {
    return gfnt_name_mac_table(encoding, language) != GFNT_MAC_TABLE_NONE;
  }
  return false;
}

/**
 * Write one codepoint as UTF-8 into @p out, or count its bytes when @p out is
 * NULL.
 */
static size_t gfnt_name_utf8(uint32_t codepoint, char * out) {
  if (codepoint < 0x80u) {
    if (out) {
      out[0] = (char)codepoint;
    }
    return 1;
  }
  if (codepoint < 0x800u) {
    if (out) {
      out[0] = (char)(0xC0u | (codepoint >> 6));
      out[1] = (char)(0x80u | (codepoint & 0x3Fu));
    }
    return 2;
  }
  if (codepoint < 0x10000u) {
    if (out) {
      out[0] = (char)(0xE0u | (codepoint >> 12));
      out[1] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
      out[2] = (char)(0x80u | (codepoint & 0x3Fu));
    }
    return 3;
  }
  if (out) {
    out[0] = (char)(0xF0u | (codepoint >> 18));
    out[1] = (char)(0x80u | ((codepoint >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (codepoint & 0x3Fu));
  }
  return 4;
}

/**
 * Decode a record's bytes, counting when @p out is NULL and writing when it is
 * not. Both passes walk the same predicate, which is why they are one
 * function: a count and a fill that disagree is this suite's most repeated
 * defect.
 */
static GFNT_Result gfnt_name_transcode(GFNT_Reader * storage,
    const GFNT_NameRecord * record, char * out, size_t * out_length,
    GFNT_Error * error) {
  size_t written = 0;

  if (record->platform_id == GFNT_PLATFORM_MACINTOSH) {
    const int32_t table =
        gfnt_name_mac_table(record->encoding_id, record->language_id);

    if (table == GFNT_MAC_TABLE_NONE) {
      // Unreachable through ::gfnt_face_name_decode(), which asks
      // gfnt_name_decodable() the same question first and reports it there.
      // Kept because what follows indexes gfnt_mac_high[table], and a guard
      // whose absence would be an out-of-bounds read is not a duplicate of a
      // guard that only produces a message.
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_NAME_TAG, 0,
          GFNT_GLYPH_NONE,
          "a Macintosh name encoding this library does not decode");
    }
    for (size_t i = 0; i < record->length; ++i) {
      uint8_t byte = 0;

      if (gfnt_reader_u8_at(storage, record->offset + i, &byte) != GFNT_OK) {
        return GFNT_ERR_CORRUPT;
      }
      // Below 0x80 every one of these encodings is ASCII, which the generator
      // asserts rather than tabulating, so the table holds the high half only.
      written += gfnt_name_utf8(
          byte < 0x80u ? byte : gfnt_mac_high[table][byte - 0x80u],
          out ? out + written : NULL);
    }
    *out_length = written;
    return GFNT_OK;
  }

  // UTF-16BE, which is what every Unicode-platform and Windows-platform record
  // is. An odd length cannot be UTF-16 at all.
  if ((record->length & 1u) != 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_NAME_TAG,
        record->offset, GFNT_GLYPH_NONE,
        "a UTF-16 name record with an odd byte count");
  }
  for (size_t i = 0; i < record->length; i += 2) {
    uint16_t unit = 0;
    uint32_t codepoint;

    if (gfnt_reader_u16_at(storage, record->offset + i, &unit) != GFNT_OK) {
      return GFNT_ERR_CORRUPT;
    }
    if (unit >= 0xD800u && unit < 0xDC00u) {
      uint16_t low = 0;

      if (i + 2 >= record->length
          || gfnt_reader_u16_at(storage, record->offset + i + 2, &low)
              != GFNT_OK
          || low < 0xDC00u || low >= 0xE000u) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_NAME_TAG,
            record->offset + i, GFNT_GLYPH_NONE,
            "a high surrogate with no low surrogate after it");
      }
      codepoint = 0x10000u + (((uint32_t)unit - 0xD800u) << 10)
          + ((uint32_t)low - 0xDC00u);
      i += 2;
    }
    else if (unit >= 0xDC00u && unit < 0xE000u) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_NAME_TAG,
          record->offset + i, GFNT_GLYPH_NONE,
          "a low surrogate with no high surrogate before it");
    }
    else {
      codepoint = unit;
    }
    written += gfnt_name_utf8(codepoint, out ? out + written : NULL);
  }

  *out_length = written;
  return GFNT_OK;
}

GFNT_Result gfnt_face_name_decode(const GFNT_Face * face,
    const GFNT_NameRecord * record, const GFNT_Allocator * allocator,
    char ** out_text, size_t * out_length, GFNT_Error * error) {
  GFNT_Reader reader;
  GFNT_Reader storage;
  size_t storage_at = 0;
  size_t needed = 0;
  size_t written = 0;
  char * text;
  GFNT_Result result;

  if (!face || !record || !out_text) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "no face, no record, or nowhere to put the string");
  }
  if (!gfnt_name_decodable(record->platform_id, record->encoding_id,
          record->language_id)) {
    // Which encoding it is, because the two cases are different news: a
    // Macintosh record in a multi-byte encoding is a table this library has not
    // imported, and anything else is a platform nobody defines. The specific
    // sentence used to live in gfnt_name_transcode() below, behind this very
    // check, so nothing ever reached it and every caller got the general one.
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE,
        record->platform_id == GFNT_PLATFORM_MACINTOSH
            ? "a Macintosh name encoding this library does not decode: the "
              "multi-byte CJK encodings are a data set of their own"
            : "a name encoding this library does not decode yet");
  }

  result = gfnt_name_open(face, &reader, NULL, &storage_at, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_reader_sub(&reader, storage_at, GFNT_READER_REST, &storage);
  if (result != GFNT_OK) {
    return result;
  }

  // Counted first, then written, through one function: a size pass and a fill
  // pass that walk different predicates is the defect this suite has met most
  // often.
  result = gfnt_name_transcode(&storage, record, NULL, &needed, error);
  if (result != GFNT_OK) {
    return result;
  }

  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  text = allocator->malloc_fn(allocator->ctx, needed + 1);
  if (!text) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "allocating the decoded name");
  }

  result = gfnt_name_transcode(&storage, record, text, &written, error);
  if (result != GFNT_OK) {
    allocator->free_fn(allocator->ctx, text);
    return result;
  }
  if (written != needed) {
    // The two passes disagreed, which cannot happen while they share this
    // function - and is worth saying out loud if it ever does.
    allocator->free_fn(allocator->ctx, text);
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "the name's size pass and fill pass disagreed");
  }
  text[written] = '\0';

  *out_text = text;
  if (out_length) {
    *out_length = written;
  }
  return GFNT_OK;
}

/**
 * The preference order of the header, as a rank: lower is better.
 */
static int gfnt_name_rank(const GFNT_NameRecord * record) {
  if (record->platform_id == GFNT_PLATFORM_WINDOWS) {
    return record->language_id == GFNT_NAME_WINDOWS_ENGLISH ? 0 : 1;
  }
  if (record->platform_id == GFNT_PLATFORM_UNICODE) {
    return 2;
  }
  if (record->platform_id == GFNT_PLATFORM_MACINTOSH) {
    return record->language_id == 0 ? 3 : 4;
  }
  return 5;
}

/**
 * A CFF string's bytes as UTF-8: counted with a NULL @p out, written with one.
 *
 * CFF says its strings hold ASCII, and fonts put Latin-1 in them. Latin-1 is
 * what fontTools decodes them as, and fontTools is the reference every string
 * this library reports is compared against - so a byte above 0x7F becomes the
 * codepoint of the same value rather than being refused or passed through as
 * a byte that would make the result invalid UTF-8.
 */
static size_t gfnt_name_latin1(const char * in, size_t length, char * out) {
  size_t written = 0;

  for (size_t i = 0; i < length; ++i) {
    written += gfnt_name_utf8((unsigned char)in[i], out ? out + written : NULL);
  }
  return written;
}

/**
 * The names a Type 1 font program states about itself.
 *
 * `/FontName` is the PostScript name - what a PDF identifies a `FontFile` by -
 * and `/FontInfo` holds the rest as PostScript strings. There is no `name` table
 * in this container and never will be, so these are not a fallback: they are
 * where a Type 1 font's names live.
 */
static GFNT_Result gfnt_name_from_type1(const GFNT_Face * face,
    uint16_t name_id, const GFNT_Allocator * allocator, char ** out_text,
    size_t * out_length, GFNT_Error * error) {
  const GFNT_Type1 * type1 = NULL;
  GFNT_Reader table;
  const uint8_t * base = NULL;
  const GFNT_Type1Element * element = NULL;
  size_t needed;
  char * text;
  GFNT_Result result = gfnt_face_type1(face, &type1, error);

  if (result != GFNT_OK) {
    return result;
  }
  switch (name_id) {
    case GFNT_NAME_COPYRIGHT: element = &type1->notice; break;
    case GFNT_NAME_FAMILY: element = &type1->family_name; break;
    case GFNT_NAME_SUBFAMILY: element = &type1->weight; break;
    case GFNT_NAME_FULL: element = &type1->full_name; break;
    case GFNT_NAME_VERSION: element = &type1->version; break;
    case GFNT_NAME_POSTSCRIPT: element = &type1->font_name; break;
    default: break;
  }
  if (!element || element->length == 0) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "a name this font program does not state: a Type 1 "
        "program states its /FontName and /FontInfo's version, family, full "
        "name, weight and notice, and nothing else a `name` table would hold");
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_TYPE1, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_bytes(&table, table.length, &base) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_TYPE1, 0,
        GFNT_GLYPH_NONE, "a Type 1 program whose bytes went away");
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  // Latin-1 for the same reason a CFF string is: PostScript says its strings are
  // bytes, and Latin-1 is what every reference makes of the ones above 0x7F.
  needed = gfnt_name_latin1((const char *)(base + element->offset),
      element->length, NULL);
  text = allocator->malloc_fn(allocator->ctx, needed + 1);
  if (!text) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "allocating the decoded name");
  }
  gfnt_name_latin1((const char *)(base + element->offset), element->length,
      text);
  text[needed] = '\0';
  gfnt_error_clear(error);
  *out_text = text;
  if (out_length) {
    *out_length = needed;
  }
  return GFNT_OK;
}

/**
 * The name a font program states about itself, for a container with no `name`.
 *
 * A bare CFF has no table directory and so no `name` table, and it still has to
 * answer for what it is and who owns it - a PDF `FontFile3` is identified by the
 * PostScript name in its Name INDEX, and a font file in a directory has to
 * carry its own licence. The Top DICT holds the rest.
 *
 * Only the names a CFF can actually state are answered. `Weight` answers
 * SUBFAMILY because that is the field it is: "Regular", "Bold". Everything else
 * is unsupported, which is the truth rather than an empty string.
 *
 * The language is not consulted. A CFF string carries no language tag, so there
 * is one string per name and it is the answer to every request - a font that
 * states one unlabelled name has not said it is English.
 */
/**
 * A name from a standalone bitmap container's own strings.
 *
 * BDF states these as properties (`FAMILY_NAME`, `COPYRIGHT`, `WEIGHT_NAME`, and
 * the XLFD `FONT` line) and PCF as the same properties compiled into a table, so
 * the two share this. **The bytes are Latin-1**, which is what XLFD says a
 * property string is, so they go out through the same decoder a Macintosh name
 * record does rather than being copied as though they were already UTF-8.
 *
 * The XLFD name answers ::GFNT_NAME_POSTSCRIPT as well as ::GFNT_NAME_FULL: it is
 * the one string that names the whole font, and a caller asking for "the name of
 * this font" should not be refused because the font is not PostScript.
 */
static GFNT_Result gfnt_name_from_bitmap(const GFNT_Face * face,
    uint16_t name_id, const GFNT_Allocator * allocator, char ** out_text,
    size_t * out_length, GFNT_Error * error) {
  GFNT_BitmapString which;
  const char * raw = NULL;
  char * text;
  size_t needed;
  GFNT_Result result;

  switch (name_id) {
    case GFNT_NAME_COPYRIGHT: which = GFNT_BITMAP_STRING_COPYRIGHT; break;
    case GFNT_NAME_FAMILY: which = GFNT_BITMAP_STRING_FAMILY; break;
    case GFNT_NAME_SUBFAMILY: which = GFNT_BITMAP_STRING_WEIGHT; break;
    case GFNT_NAME_FULL:
    case GFNT_NAME_POSTSCRIPT: which = GFNT_BITMAP_STRING_FULL; break;
    default:
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_NAME_TAG, 0,
          GFNT_GLYPH_NONE, "a name a bitmap font does not state: a BDF or PCF "
          "states its family, weight, copyright and XLFD name, and nothing else "
          "a `name` table would have held");
  }
  result = gfnt_bitmap_string_for(face, which, &raw, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  needed = gfnt_name_latin1(raw, strlen(raw), NULL);
  text = allocator->malloc_fn(allocator->ctx, needed + 1);
  if (!text) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "allocating the bitmap font's name");
  }
  if (gfnt_name_latin1(raw, strlen(raw), text) != needed) {
    allocator->free_fn(allocator->ctx, text);
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "the name's size pass and fill pass disagreed");
  }
  text[needed] = '\0';
  gfnt_error_clear(error);
  *out_text = text;
  if (out_length) {
    *out_length = needed;
  }
  return GFNT_OK;
}

static GFNT_Result gfnt_name_from_font_program(const GFNT_Face * face,
    uint16_t name_id, const GFNT_Allocator * allocator, char ** out_text,
    size_t * out_length, GFNT_Error * error) {
  const GFNT_Cff * cff = NULL;
  uint32_t sid = GFNT_CFF_SID_NONE;
  bool font_name = false;
  char * raw = NULL;
  char * text = NULL;
  size_t length = 0;
  size_t needed = 0;
  GFNT_Result result;

  if (gfnt_face_is_bitmap(face)) {
    return gfnt_name_from_bitmap(face, name_id, allocator, out_text, out_length,
        error);
  }
  // A Type 1 program states its names in `/FontInfo` and `/FontName`, which are
  // PostScript strings rather than SIDs into an INDEX - so it answers here, and
  // the CFF path below is for a CFF.
  if (gfnt_sfnt_producer(face) == GFNT_PRODUCER_TYPE1) {
    return gfnt_name_from_type1(face, name_id, allocator, out_text, out_length,
        error);
  }
  result = gfnt_face_cff(face, &cff, NULL);
  if (result != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "a face with no `name` table and no font program to "
        "state its names instead");
  }
  switch (name_id) {
    case GFNT_NAME_COPYRIGHT:
      // Copyright first, then Notice: the two operators say the same kind of
      // thing, most fonts state one of them, and Adobe's own tools write the
      // copyright line into Notice.
      sid = cff->sid_copyright != GFNT_CFF_SID_NONE
          ? cff->sid_copyright : cff->sid_notice;
      break;
    case GFNT_NAME_FAMILY: sid = cff->sid_family_name; break;
    case GFNT_NAME_SUBFAMILY: sid = cff->sid_weight; break;
    case GFNT_NAME_FULL: sid = cff->sid_full_name; break;
    case GFNT_NAME_VERSION: sid = cff->sid_version; break;
    case GFNT_NAME_POSTSCRIPT: font_name = true; break;
    default: break;
  }
  if (!font_name && sid == GFNT_CFF_SID_NONE) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "a name this font program does not state: a CFF states "
        "its version, family, full name, weight, copyright and PostScript name, "
        "and nothing else a `name` table would have held");
  }

  result = font_name
      ? gfnt_cff_font_name(face, cff, NULL, 0, &length, error)
      : gfnt_cff_string(face, cff, sid, NULL, 0, &length, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  raw = allocator->malloc_fn(allocator->ctx, length + 1);
  if (!raw) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "allocating the font program's name");
  }
  result = font_name
      ? gfnt_cff_font_name(face, cff, raw, length + 1, &length, error)
      : gfnt_cff_string(face, cff, sid, raw, length + 1, &length, error);
  if (result != GFNT_OK) {
    allocator->free_fn(allocator->ctx, raw);
    return result;
  }

  // Counted and written through one function, for the same reason the `name`
  // path is: a size pass and a fill pass that walk different predicates is the
  // defect this suite has met most often.
  needed = gfnt_name_latin1(raw, length, NULL);
  text = allocator->malloc_fn(allocator->ctx, needed + 1);
  if (!text) {
    allocator->free_fn(allocator->ctx, raw);
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "allocating the decoded name");
  }
  if (gfnt_name_latin1(raw, length, text) != needed) {
    allocator->free_fn(allocator->ctx, raw);
    allocator->free_fn(allocator->ctx, text);
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "the name's size pass and fill pass disagreed");
  }
  allocator->free_fn(allocator->ctx, raw);
  text[needed] = '\0';
  gfnt_error_clear(error);

  *out_text = text;
  if (out_length) {
    *out_length = needed;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_name(const GFNT_Face * face, uint16_t name_id,
    uint16_t language_id, const GFNT_Allocator * allocator, char ** out_text,
    size_t * out_length, GFNT_Error * error) {
  GFNT_NameRecord best = {0};
  size_t count = 0;
  int best_rank = -1;
  GFNT_Result result;

  if (!face || !out_text) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE, "no face, or nowhere to put the string");
  }
  result = gfnt_face_name_count(face, &count, error);
  if (result == GFNT_ERR_UNSUPPORTED) {
    // No `name` table at all, which for a container with no table directory is
    // the normal case rather than a defect.
    return gfnt_name_from_font_program(face, name_id, allocator, out_text,
        out_length, error);
  }
  if (result != GFNT_OK) {
    return result;
  }

  for (size_t i = 0; i < count; ++i) {
    GFNT_NameRecord record;
    int rank;

    if (gfnt_face_name_at(face, i, &record, NULL) != GFNT_OK) {
      continue;
    }
    if (record.name_id != name_id) {
      continue;
    }
    if (language_id != GFNT_LANGUAGE_ANY && record.language_id != language_id) {
      continue;
    }
    if (!gfnt_name_decodable(record.platform_id, record.encoding_id,
            record.language_id)) {
      continue;
    }
    rank = gfnt_name_rank(&record);
    if (best_rank < 0 || rank < best_rank) {
      best_rank = rank;
      best = record;
    }
  }

  if (best_rank < 0) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_NAME_TAG, 0,
        GFNT_GLYPH_NONE,
        "the font has no record for that name in an encoding this library "
        "decodes");
  }
  return gfnt_face_name_decode(face, &best, allocator, out_text, out_length,
      error);
}

void gfnt_name_free(const GFNT_Allocator * allocator, char * text) {
  if (!text) {
    return;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  allocator->free_fn(allocator->ctx, text);
}

/**
 * Write a decoded name with its control characters escaped.
 *
 * A name record routinely contains newlines - every multi-line copyright notice
 * does - and a dump whose records can run over several lines is one no tool can
 * read back and no person can scan. `tools/oracle/ttx_diff.py` parses these
 * lines, so this is the difference between a dump and a dump that is an
 * interface.
 *
 * Written as runs rather than character by character: the first version called
 * fprintf once per byte, which turned one name into fifty writes and made the
 * write-failure sweep in tests/unit/test_name.cpp depend on the *content* of a
 * record rather than on the number of lines. A name with nothing to escape is
 * one write now.
 */
static GFNT_Result gfnt_name_dump_escaped(FILE * out, const char * text) {
  const char * run = text;
  const unsigned char * at = (const unsigned char *)text;

  for (; *at; ++at) {
    const char * escape = NULL;
    char hex[5];

    switch (*at) {
      case '\n':
        escape = "\\n";
        break;
      case '\r':
        escape = "\\r";
        break;
      case '\t':
        escape = "\\t";
        break;
      case '\\':
        escape = "\\\\";
        break;
      default:
        // Only C0 and DEL are escaped. Everything else is UTF-8 by
        // construction - the decoder produced it - and mangling it here would
        // make the dump a worse record than the font.
        if (*at < 0x20u || *at == 0x7Fu) {
          snprintf(hex, sizeof hex, "\\x%02X", (unsigned)*at);
          escape = hex;
        }
        break;
    }
    if (!escape) {
      continue;
    }

    if ((const char *)at > run) {
      size_t length = (size_t)((const char *)at - run);

      if (fwrite(run, 1, length, out) != length) {
        return GFNT_ERR_IO;
      }
    }
    if (fprintf(out, "%s", escape) < 0) {
      return GFNT_ERR_IO;
    }
    run = (const char *)at + 1;
  }

  if (*run) {
    size_t length = strlen(run);

    if (fwrite(run, 1, length, out) != length) {
      return GFNT_ERR_IO;
    }
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_name_dump(const GFNT_Face * face, FILE * out) {
  size_t count = 0;
  GFNT_Result result;

  if (!face || !out) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_face_name_count(face, &count, NULL);
  if (result != GFNT_OK) {
    return result;
  }
  if (fprintf(out, "name: %zu records\n", count) < 0) {
    return GFNT_ERR_IO;
  }

  for (size_t i = 0; i < count; ++i) {
    GFNT_NameRecord record;
    char * text = NULL;

    if (gfnt_face_name_at(face, i, &record, NULL) != GFNT_OK) {
      if (fprintf(out, "name record %zu: unreadable\n", i) < 0) {
        return GFNT_ERR_IO;
      }
      continue;
    }
    if (gfnt_face_name_decode(face, &record, NULL, &text, NULL, NULL)
        != GFNT_OK) {
      if (fprintf(out,
              "name record %zu: platform %u, encoding %u, language %u, "
              "name %u, %zu bytes, not decodable\n",
              i, record.platform_id, record.encoding_id, record.language_id,
              record.name_id, record.length)
          < 0) {
        return GFNT_ERR_IO;
      }
      continue;
    }
    if (fprintf(out,
            "name record %zu: platform %u, encoding %u, language %u, "
            "name %u: '",
            i, record.platform_id, record.encoding_id, record.language_id,
            record.name_id)
        < 0) {
      gfnt_name_free(NULL, text);
      return GFNT_ERR_IO;
    }
    result = gfnt_name_dump_escaped(out, text);
    gfnt_name_free(NULL, text);
    if (result != GFNT_OK) {
      return result;
    }
    if (fprintf(out, "'\n") < 0) {
      return GFNT_ERR_IO;
    }
  }
  return GFNT_OK;
}
