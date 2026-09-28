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
 * The Type 1 font program as a container: PFB and PFA framing, `eexec`, and the
 * PostScript the font is written in.
 *
 * documentation/design.md section 7.1. The *language* is in
 * `src/charstring/charstring.c` and needs none of this - a CFF can carry Type 1
 * charstrings, and does in one fixture. What is here is the other half: a file
 * that is a PostScript program, whose private half is encrypted, and which has no
 * table directory, no `maxp`, no `hmtx`, no `cmap` and no `name`.
 *
 * **The bytes have to be derived before anything can read them.** The private
 * portion is `eexec`-encrypted and a PFA's is also ASCII-hex, so unlike every
 * other container the parser cannot point a reader at the file. The loader builds
 * the decrypted program into a blob the face owns, and the face's synthetic
 * directory describes *that* - after which the one-door rule holds again and
 * every reader above it is unaware.
 *
 * **It is not a PostScript interpreter.** The scanner understands exactly what a
 * Type 1 font program contains: names, numbers, arrays, procedures it skips,
 * strings, and the `RD`/`ND`/`NP` idiom that introduces binary. A construction
 * outside that is refused by name rather than approximated.
 *
 * Reference: Adobe Type 1 Font Format (the "black book"), 1990 - chapter 2 for
 * the program's structure, chapter 7 for `eexec` and the charstring encryption,
 * and Adobe Technical Note #5040 for PFB segment framing.
 */

#ifndef GHOTI_IO_GFNT_TYPE1_H
#define GHOTI_IO_GFNT_TYPE1_H

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../charstring/charstring.h"
#include "../outline/outline.h"
#include "../reader/reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The tag the synthetic directory gives a Type 1 font program. */
#define GFNT_TAG_TYPE1 GFNT_TAG('T', 'Y', 'P', '1')

/**
 * How many glyphs and subroutines one font program may hold.
 *
 * Not a limit on the format - a Type 1 font can name as many glyphs as it likes
 * - but the size of the arrays this parser allocates in one go, and therefore
 * what it refuses rather than growing without bound. A face's own
 * ::GFNT_Limits::max_glyphs is applied on top.
 */
#define GFNT_TYPE1_MAX_GLYPHS 65535u
#define GFNT_TYPE1_MAX_SUBRS 65535u

/** One named thing in the font program: where its bytes are. */
typedef struct GFNT_Type1Element {
  size_t offset; ///< From the start of whichever buffer holds it.
  size_t length; ///< In bytes.
} GFNT_Type1Element;

/** One entry of `/CharStrings`: a name and a program. */
typedef struct GFNT_Type1Glyph {
  GFNT_Type1Element name;    ///< Into the program text: the glyph's own name.
  GFNT_Type1Element program; ///< Into `plain`: its decrypted charstring.
} GFNT_Type1Glyph;

/**
 * A parsed Type 1 font program.
 *
 * Every element is an offset into the face's derived bytes rather than a copy:
 * the program is one buffer the face owns for as long as it lives, and copying
 * the charstrings would double a 200 KB font for nothing.
 */
typedef struct GFNT_Type1 {
  size_t length;             ///< The derived program's total size.
  GFNT_Type1Element font_name; ///< `/FontName`, without its slash.
  /**
   * `/FontInfo`'s strings, without their parentheses, or zero length for absent.
   *
   * What a Type 1 font names itself with. There is no `name` table here, so
   * these are the only strings the file states - and a committed fixture has to
   * carry its own licence, which is `/Notice`.
   */
  GFNT_Type1Element full_name;
  GFNT_Type1Element family_name;
  GFNT_Type1Element notice;
  GFNT_Type1Element version;
  GFNT_Type1Element weight;
  GFNT_F16Dot16 font_matrix[6]; ///< `/FontMatrix`, in PostScript order.
  bool font_matrix_stated;   ///< Whether the program carried one.

  /**
   * `/lenIV`: how many random bytes open each decrypted charstring.
   *
   * Four unless the font says otherwise. A font that says zero is not corrupt -
   * some subsetters write it - so it is read rather than assumed.
   */
  int32_t len_iv;

  GFNT_Type1Glyph * glyphs;  ///< `/CharStrings`, in the order they appear.
  size_t glyph_count;
  GFNT_Type1Element * subrs; ///< `/Subrs`, indexed by their own numbers.
  size_t subr_count;

  /**
   * Every charstring and subroutine, decrypted, end to end.
   *
   * Decrypted once at parse time rather than per call, for two reasons. The
   * interpreter's subroutine accessor promises "a pointer that outlives the
   * run", and a charstring that has to be deciphered into scratch has no such
   * pointer to give; and the same bytes would otherwise be deciphered again for
   * every glyph that calls the subroutine, at every size it is drawn.
   *
   * The `/lenIV` random bytes are consumed here and do not appear.
   */
  uint8_t * plain;
  size_t plain_length;

  /**
   * `/Encoding`: which glyph each of the 256 codes names, or
   * ::GFNT_GLYPH_NONE.
   *
   * Resolved at parse time from names to indices, because the array is 256
   * entries and resolving a name per lookup would make a code point lookup a
   * linear scan of the whole font.
   */
  uint32_t encoding[256];
  bool encoding_is_standard; ///< Whether it said `StandardEncoding`.
} GFNT_Type1;

/**
 * Whether a blob could be a Type 1 font program, and needs deriving.
 *
 * PFB's `0x80 0x01` segment marker, or the `%!` of a PostScript program. Both are
 * stronger than a bare CFF's four header bytes, which is why Type 1 is tried
 * first of the two.
 *
 * @param blob A reader over the whole blob, or NULL.
 * @return Whether to try deriving a Type 1 program from it.
 */
bool gfnt_type1_looks_like(const GFNT_Reader * blob);

/**
 * Turn the file into the program: unwrap PFB or PFA, and `eexec`-decrypt.
 *
 * Builds a blob the face owns, points the face's `bytes` at it, and gives the
 * face a synthetic directory of one ::GFNT_TAG_TYPE1 entry spanning it.
 *
 * @param face The face. Its blob, allocator and limits must be set.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_FORMAT for a file that is not one of these,
 *   ::GFNT_ERR_CORRUPT, ::GFNT_ERR_LIMIT or ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_type1_derive(GFNT_Face * face, GFNT_Error * error);

/**
 * Parse the derived program, for ::gfnt_table_cached().
 *
 * @param face The face.
 * @param out A ::GFNT_Type1 to fill in.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_UNSUPPORTED, ::GFNT_ERR_CORRUPT,
 *   ::GFNT_ERR_LIMIT or ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_type1_parse(const GFNT_Face * face, void * out,
    GFNT_Error * error);

/** Release what a parse allocated. For the face's cache teardown. */
void gfnt_type1_release(const GFNT_Allocator * allocator, GFNT_Type1 * type1);

/**
 * The face's parsed program, parsing it on first use.
 *
 * @param face The face.
 * @param out_type1 Receives a pointer into the face's memo.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, or what the parse returned.
 */
GFNT_Result gfnt_face_type1(const GFNT_Face * face, const GFNT_Type1 ** out_type1,
    GFNT_Error * error);

/**
 * How many glyphs the program holds, for the numGlyphs minimum.
 *
 * @param face The face.
 * @param out_bound Receives the count.
 * @return true when the face has a readable Type 1 program whose count this is.
 */
bool gfnt_type1_glyph_bound(const GFNT_Face * face, size_t * out_bound);

/**
 * How many charstring units this program puts in an em.
 *
 * @param type1 The parsed program, or NULL.
 * @param out_upem Receives the em.
 * @return true when the `FontMatrix` states an em this library can report.
 */
bool gfnt_type1_units_per_em(const GFNT_Type1 * type1, size_t * out_upem);

/**
 * One glyph's decrypted charstring, borrowed.
 *
 * Borrowed rather than copied: the bytes were deciphered once at parse time and
 * live as long as the face does, which is also what the interpreter's accessors
 * require.
 *
 * @param type1 The parsed program.
 * @param glyph Which glyph.
 * @param out_bytes Receives the pointer.
 * @param out_length Receives the length.
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID for a glyph this program lacks.
 */
GFNT_Result gfnt_type1_program(const GFNT_Type1 * type1, uint32_t glyph,
    const uint8_t ** out_bytes, size_t * out_length);

/**
 * One subroutine's decrypted bytes, borrowed, by its own number.
 *
 * @param type1 The parsed program.
 * @param index The subroutine number, as the charstring spelled it.
 * @param out_bytes Receives the pointer.
 * @param out_length Receives the length.
 * @return ::GFNT_OK, or ::GFNT_ERR_CORRUPT for a number the font left a gap at.
 */
GFNT_Result gfnt_type1_subr(const GFNT_Type1 * type1, size_t index,
    const uint8_t ** out_bytes, size_t * out_length);

/**
 * Draw one glyph, running its charstring as Type 1.
 *
 * @param face The face.
 * @param glyph Which glyph.
 * @param outline Receives the contours, appended.
 * @param out_metrics Receives what the run reported, or NULL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_UNSUPPORTED,
 *   ::GFNT_ERR_CORRUPT, ::GFNT_ERR_LIMIT or ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_type1_load(const GFNT_Face * face, uint32_t glyph,
    GFNT_Outline * outline, GFNT_CharstringMetrics * out_metrics,
    GFNT_Error * error);

/**
 * What one glyph's charstring says about its own advance and side bearing.
 *
 * @param face The face.
 * @param glyph Which glyph.
 * @param out_metrics Receives the metrics.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, or what the run returned.
 */
GFNT_Result gfnt_type1_metrics(const GFNT_Face * face, uint32_t glyph,
    GFNT_CharstringMetrics * out_metrics, GFNT_Error * error);

/**
 * Whether this glyph is an accented character built from two others.
 *
 * @param face The face.
 * @param glyph Which glyph.
 * @param out_composite Receives the answer.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, or what running the charstring returned.
 */
GFNT_Result gfnt_type1_is_composite(const GFNT_Face * face, uint32_t glyph,
    bool * out_composite, GFNT_Error * error);

/**
 * One glyph's name, counted with a NULL @p out and written with one.
 *
 * The same two-pass contract `post` and the CFF charset use, so that
 * `glyph/names.c` chooses between three sources and nothing else.
 *
 * @param face The face.
 * @param glyph Which glyph.
 * @param out Where to write it, or NULL to ask for the length.
 * @param capacity How much room @p out has, including the terminator.
 * @param out_length Receives the length without the terminator.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_LIMIT or ::GFNT_ERR_CORRUPT.
 */
GFNT_Result gfnt_type1_name_at(const GFNT_Face * face, uint32_t glyph,
    char * out, size_t capacity, size_t * out_length, GFNT_Error * error);

/**
 * The glyph the font's own `/Encoding` puts at a code, or ::GFNT_GLYPH_NONE.
 *
 * A *code*, not a codepoint: a Type 1 `/Encoding` is 256 entries of the font's
 * own choosing and says nothing about Unicode. Turning a codepoint into one of
 * these needs the Adobe Glyph List, which this library does not carry yet - so a
 * Type 1 face answers ::gfnt_face_glyph_for_codepoint() with ERR_UNSUPPORTED
 * rather than guessing that code 193 means U+00C1.
 *
 * @param type1 The parsed program, or NULL.
 * @param code The code.
 * @return The glyph, or ::GFNT_GLYPH_NONE.
 */
uint32_t gfnt_type1_glyph_for_code(const GFNT_Type1 * type1, uint8_t code);

/**
 * The glyph a name belongs to, or ::GFNT_GLYPH_NONE.
 *
 * @param type1 The parsed program.
 * @param base The derived bytes, for reading the stored names.
 * @param name The name to find, not NUL-terminated.
 * @param length Its length.
 * @return The glyph index, or ::GFNT_GLYPH_NONE.
 */
uint32_t gfnt_type1_glyph_for_name(const GFNT_Type1 * type1,
    const uint8_t * base, const char * name, size_t length);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_TYPE1_H
