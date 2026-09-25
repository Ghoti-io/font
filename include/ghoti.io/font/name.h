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
 * `name`: the strings a font carries, decoded to UTF-8.
 *
 * documentation/design.md section 7.2 and M15. A `name` record's bytes mean
 * nothing without its `(platformID, encodingID)` pair: the same bytes are
 * UTF-16BE under the Windows and Unicode platforms and a single-byte Macintosh
 * encoding under platform 1, and a parser that reads them as Latin-1 produces
 * family names with a NUL between every letter. So the encoding decides the
 * decode, and a record this library cannot decode says so rather than
 * guessing.
 *
 * **Macintosh records with bytes above ASCII are ::GFNT_ERR_UNSUPPORTED
 * today.** Decoding them needs the Mac Roman table - 128 codepoints - and
 * section 14's rule is that a vector comes from an oracle rather than from
 * memory. Nothing is lost for the common case: a font that carries a
 * Macintosh name almost always carries the same name under the Windows
 * platform, and the preference order prefers that one anyway.
 */

#ifndef GHOTI_IO_GFNT_NAME_H
#define GHOTI_IO_GFNT_NAME_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Name ID 0: the copyright notice. */
#define GFNT_NAME_COPYRIGHT 0u
/** @brief Name ID 1: the family name, as a menu would list it. */
#define GFNT_NAME_FAMILY 1u
/** @brief Name ID 2: the subfamily - "Regular", "Bold Italic". */
#define GFNT_NAME_SUBFAMILY 2u
/** @brief Name ID 3: a unique identifier for this font. */
#define GFNT_NAME_UNIQUE_ID 3u
/** @brief Name ID 4: the full name. */
#define GFNT_NAME_FULL 4u
/** @brief Name ID 5: the version string. */
#define GFNT_NAME_VERSION 5u
/** @brief Name ID 6: the PostScript name. */
#define GFNT_NAME_POSTSCRIPT 6u
/** @brief Name ID 7: the trademark notice. */
#define GFNT_NAME_TRADEMARK 7u
/** @brief Name ID 8: the manufacturer. */
#define GFNT_NAME_MANUFACTURER 8u
/** @brief Name ID 9: the designer. */
#define GFNT_NAME_DESIGNER 9u
/** @brief Name ID 10: a description. */
#define GFNT_NAME_DESCRIPTION 10u
/** @brief Name ID 11: the vendor's URL. */
#define GFNT_NAME_VENDOR_URL 11u
/** @brief Name ID 12: the designer's URL. */
#define GFNT_NAME_DESIGNER_URL 12u
/** @brief Name ID 13: the licence description. */
#define GFNT_NAME_LICENSE 13u
/** @brief Name ID 14: the licence's URL. */
#define GFNT_NAME_LICENSE_URL 14u
/** @brief Name ID 16: the typographic family, when it differs from ID 1. */
#define GFNT_NAME_TYPOGRAPHIC_FAMILY 16u
/** @brief Name ID 17: the typographic subfamily. */
#define GFNT_NAME_TYPOGRAPHIC_SUBFAMILY 17u
/** @brief Name ID 18: the Macintosh menu name. */
#define GFNT_NAME_COMPATIBLE_FULL 18u
/** @brief Name ID 19: sample text. */
#define GFNT_NAME_SAMPLE_TEXT 19u
/** @brief Name ID 20: the PostScript CID findfont name. */
#define GFNT_NAME_POSTSCRIPT_CID 20u
/** @brief Name ID 21: the WWS family. */
#define GFNT_NAME_WWS_FAMILY 21u
/** @brief Name ID 22: the WWS subfamily. */
#define GFNT_NAME_WWS_SUBFAMILY 22u
/** @brief Name ID 23: the light background palette name. */
#define GFNT_NAME_LIGHT_BACKGROUND 23u
/** @brief Name ID 24: the dark background palette name. */
#define GFNT_NAME_DARK_BACKGROUND 24u
/** @brief Name ID 25: the variations PostScript name prefix. */
#define GFNT_NAME_VARIATIONS_PREFIX 25u

/**
 * @brief Passed as a language to mean "whichever the preference order picks".
 *
 * Language IDs are per-platform - 0x409 is US English under Windows and 0
 * is English under Macintosh - so there is no value that means "any" in the
 * format itself.
 */
#define GFNT_LANGUAGE_ANY 0xFFFFu

/**
 * @brief One `name` record: who wrote it, for whom, and where its bytes are.
 */
typedef struct GFNT_NameRecord {
  uint16_t platform_id; ///< ::GFNT_PLATFORM_WINDOWS and its neighbours.
  uint16_t encoding_id; ///< Meaning depends on the platform.
  uint16_t language_id; ///< Per-platform; 0x8000 and up index the lang tags.
  uint16_t name_id;     ///< ::GFNT_NAME_FAMILY and its neighbours.
  size_t length;        ///< Bytes of encoded text in the file.
  size_t offset;        ///< From the start of the string storage.
} GFNT_NameRecord;

/**
 * @brief How many records the `name` table lists.
 *
 * @param face The face.
 * @param out_count Receives the count. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED if the font
 *   has no `name`, ::GFNT_ERR_LIMIT or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_name_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/**
 * @brief The nth record, in the order the font lists them.
 *
 * @param face The face.
 * @param index Which one, from 0.
 * @param out_record Receives it. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED,
 *   ::GFNT_ERR_LIMIT or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_name_at(const GFNT_Face * face, size_t index,
    GFNT_NameRecord * out_record, GFNT_Error * error);

/**
 * @brief Decode one record to a NUL-terminated UTF-8 string.
 *
 * @param face The face.
 * @param record A record from ::gfnt_face_name_at().
 * @param allocator Allocator for the string, or NULL for the default.
 * @param out_text Receives the string, owned by the caller and released with
 *   ::gfnt_name_free(). Written only on success.
 * @param out_length Receives its length in bytes, excluding the NUL, or NULL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED for an
 *   encoding this library cannot decode yet, ::GFNT_ERR_CORRUPT for bytes that
 *   are not valid in the encoding the record claims, or ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_face_name_decode(const GFNT_Face * face,
    const GFNT_NameRecord * record, const GFNT_Allocator * allocator,
    char ** out_text, size_t * out_length, GFNT_Error * error);

/**
 * @brief The best record for a name ID, decoded to UTF-8.
 *
 * The preference order is Windows US English, then any Windows record, then
 * the Unicode platform, then Macintosh English, then any record this library
 * can decode. It is documented rather than clever because every order is
 * arbitrary and a caller that disagrees needs to know what it is overriding.
 *
 * @param face The face.
 * @param name_id ::GFNT_NAME_FAMILY and its neighbours.
 * @param language_id A language to require, or ::GFNT_LANGUAGE_ANY for the
 *   preference order. Language IDs are per-platform, so requiring one narrows
 *   to the platforms that spell it that way.
 * @param allocator Allocator for the string, or NULL for the default.
 * @param out_text Receives the string, released with ::gfnt_name_free().
 *   Written only on success.
 * @param out_length Receives its length in bytes, excluding the NUL, or NULL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED if the font
 *   has no `name` table or no decodable record for that name, ::GFNT_ERR_LIMIT,
 *   ::GFNT_ERR_CORRUPT or ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_face_name(const GFNT_Face * face, uint16_t name_id,
    uint16_t language_id, const GFNT_Allocator * allocator, char ** out_text,
    size_t * out_length, GFNT_Error * error);

/**
 * @brief Release a string from ::gfnt_face_name() or
 * ::gfnt_face_name_decode().
 *
 * @param allocator The allocator it came from, or NULL for the default. It
 *   must be the same one.
 * @param text The string, or NULL.
 */
GFNT_API void gfnt_name_free(const GFNT_Allocator * allocator, char * text);

/**
 * @brief Write the `name` table's records to a stream, one per line.
 *
 * @param face The face.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED,
 *   ::GFNT_ERR_LIMIT, ::GFNT_ERR_CORRUPT or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_face_name_dump(const GFNT_Face * face, FILE * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_NAME_H
