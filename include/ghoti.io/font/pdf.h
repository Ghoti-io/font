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
 * What a PDF writer needs from a font that the font files do not state as text:
 * the `W` array of widths and the `ToUnicode` CMap of an embedded subset.
 *
 * Both return a blob of ASCII, owned by the caller and released with
 * ::gfnt_blob_destroy(). They write the *contents* a PDF object holds (the
 * array's elements, the CMap's program), not the object around it, because the
 * object's dictionary, its stream length and its filters are the PDF library's.
 * Nothing here parses or writes a PDF.
 *
 * Like `write.h` this is not part of the `font.h` umbrella.
 */

#ifndef GHOTI_IO_GFNT_PDF_H
#define GHOTI_IO_GFNT_PDF_H

#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The elements of a `W` array for the given glyphs, in 1000-unit glyph space.
 *
 * What a `CIDFontType2` whose `CIDToGIDMap` is `Identity` (so the CID is the glyph
 * id, which is what ::GFNT_SubsetOptions::retain_gids produces) states as its
 * widths. The glyphs may come in any order and repeat. A run of four or more
 * consecutive glyphs of one width is written as `first last width`; every other
 * run of consecutive glyphs as `first [w w w]`.
 *
 * A width is the glyph's advance scaled from the face's units to 1000 to the
 * thousandth, so a face of 1000 units per em states integers, a face of 2048 states
 * what 2048 gives (`500.977`, rounded half away from zero), and no floating point
 * is involved: the same text comes out on every platform.
 *
 * @param face The face.
 * @param glyphs The glyph ids.
 * @param count How many.
 * @param variation The instance, or NULL for the default.
 * @param limits Caps, or NULL for the defaults.
 * @param allocator Where the blob comes from, or NULL for the default.
 * @param out_blob Receives the text. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a glyph the face lacks or no glyphs;
 *   whatever ::gfnt_face_glyph_advance() answers (a face with no `hmtx` is
 *   ::GFNT_ERR_UNSUPPORTED); ::GFNT_ERR_LIMIT; ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_pdf_widths(const GFNT_Face * face, const uint32_t * glyphs,
    size_t count, const GFNT_Variation * variation, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob, GFNT_Error * error);

/** One code of a font and the text it stands for. */
typedef struct GFNT_PdfToUnicodeEntry {
  uint32_t code;              ///< The character code (the CID, for a composite font).
  const uint32_t * codepoints; ///< The Unicode scalar values it stands for.
  size_t count;               ///< How many. Zero is an entry that says nothing and is left out.
} GFNT_PdfToUnicodeEntry;

/** The most codepoints one entry of a `ToUnicode` CMap may stand for. */
#define GFNT_PDF_TOUNICODE_MAX_CODEPOINTS 32

/**
 * @brief A `ToUnicode` CMap program for the given codes.
 *
 * What lets a reader copy text out of a subset. The entries may come in any order;
 * a code may not repeat. Codes that run consecutively and stand for consecutive
 * single codepoints are written as `bfrange`, as many as the PDF specification lets
 * one range hold, and the rest as `bfchar`; each block holds at most 100 entries, as
 * the specification says. A codepoint past U+FFFF is written as a surrogate pair.
 *
 * @param entries The codes.
 * @param count How many.
 * @param code_bytes How wide a code is in the font's strings: 1 (a simple font) or 2 (a
 *   composite font with an identity encoding).
 * @param limits Caps, or NULL for the defaults.
 * @param allocator Where the blob comes from, or NULL for the default.
 * @param out_blob Receives the program. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a width other than 1 or 2, a code that does not
 *   fit it, a code that repeats, more than ::GFNT_PDF_TOUNICODE_MAX_CODEPOINTS, a
 *   codepoint that is a surrogate or past U+10FFFF, or a NULL where a value is needed;
 *   ::GFNT_ERR_LIMIT; ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_pdf_tounicode(const GFNT_PdfToUnicodeEntry * entries, size_t count,
    unsigned code_bytes, const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Blob ** out_blob, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif
