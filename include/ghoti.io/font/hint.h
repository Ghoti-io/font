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
 * TrueType instruction programs: find them, check them and read them.
 *
 * documentation/design.md section 8.5. A TrueType font carries programs in
 * `fpgm`, `prep` and in each glyph; they are **untrusted input**. This header
 * is the part that does not run one: it hands over a glyph's program and says
 * whether a program is well formed, and prints it as the specification's
 * mnemonics.
 *
 * Reference: OpenType Specification 1.9, "TrueType Instruction Set".
 */

#ifndef GHOTI_IO_GFNT_HINT_H
#define GHOTI_IO_GFNT_HINT_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A glyph's own instructions.
 *
 * A simple glyph's, or a composite's (the ones after its last component, if it
 * has any). `fpgm`, `prep` and `cvt ` are tables: ::gfnt_face_table_data().
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param out_bytes Receives the program, **NULL when the glyph has none**. It
 *   points into the face's data and lives as long as the face does. Written
 *   only on success.
 * @param out_length Receives its length, 0 when there is none. Written only on
 *   success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a bad glyph or argument;
 *   ::GFNT_ERR_UNSUPPORTED for a face with no `glyf`; ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_glyph_program(const GFNT_Face * face,
    uint32_t glyph, const uint8_t ** out_bytes, size_t * out_length,
    GFNT_Error * error);

/** What is wrong with a program, found without running it. */
typedef enum GFNT_ProgramProblem {
  GFNT_PROGRAM_OK = 0,           ///< Nothing.
  GFNT_PROGRAM_TRUNCATED_PUSH,   ///< A push's values run past the end.
  GFNT_PROGRAM_UNBALANCED_IF,    ///< `ELSE` or `EIF` with no `IF`, or an `IF` never closed, or one closed across a function boundary.
  GFNT_PROGRAM_UNBALANCED_DEF    ///< `ENDF` with no `FDEF`/`IDEF`, a definition inside a definition, or one never ended.
} GFNT_ProgramProblem;

/** The first problem ::gfnt_program_validate() found. */
typedef struct GFNT_ProgramReport {
  GFNT_ProgramProblem problem; ///< What, or ::GFNT_PROGRAM_OK.
  size_t offset;               ///< Where: the offending instruction, or the
                               ///< opening one that was never closed.
} GFNT_ProgramReport;

/**
 * @brief Check that a program's structure holds, without running it.
 *
 * Every push is complete; `IF`/`ELSE`/`EIF` and `FDEF`/`IDEF`/`ENDF` nest and
 * close. What a jump lands on depends on the stack and is the interpreter's to
 * check. A program this refuses is not necessarily one a renderer refuses to
 * run, since a renderer may never reach the bad part; it is one a reader of the
 * font should know about.
 *
 * @param bytes The program; may be NULL if @p length is 0.
 * @param length Its length.
 * @param out_report Receives the finding. Written only on success.
 * @return ::GFNT_OK (whether or not the program is well formed; the report says),
 *   or ::GFNT_ERR_INVALID for a NULL argument.
 */
GFNT_API GFNT_Result gfnt_program_validate(const uint8_t * bytes, size_t length,
    GFNT_ProgramReport * out_report);

/**
 * @brief Write a program as the specification's mnemonics, one instruction a line.
 *
 * Flags in brackets (`MDRP[01101]`), a push's values on the lines after it, an
 * undefined opcode as `INSTR<decimal>`. A truncated push stops the listing at
 * that point with a line saying so.
 *
 * @param bytes The program.
 * @param length Its length.
 * @param stream Where to write.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a NULL argument; ::GFNT_ERR_IO if
 *   the stream refused a write.
 */
GFNT_API GFNT_Result gfnt_program_disassemble(const uint8_t * bytes,
    size_t length, FILE * stream);

/**
 * @brief Whether and how to hint (design.md section 8.5).
 *
 * The zero value is the font's own request, which is the library's rule for a
 * policy (design.md section 10.3).
 */
typedef enum GFNT_HintMode {
  /** Do what the font asks: run its program at a size its `gasp` applies grid-fitting to; a face with no program, or a program that fails, is left as drawn. */
  GFNT_HINT_MODE_FONT = 0,
  GFNT_HINT_MODE_NONE,         ///< The outline as drawn, scaled and not fitted.
  GFNT_HINT_MODE_FONT_STRICT,  ///< As ::GFNT_HINT_MODE_FONT, but a program that fails fails the call.
  GFNT_HINT_MODE_AUTO          ///< Reserved for an automatic hinter; refused as ::GFNT_ERR_UNSUPPORTED.
} GFNT_HintMode;

/** Which interpreter behaviour a TrueType program runs under. */
typedef enum GFNT_HintVersion {
  GFNT_HINT_VERSION_AUTO = 0,  ///< Version 40, which is what FreeType and Windows run.
  GFNT_HINT_VERSION_35,        ///< The original interpreter.
  GFNT_HINT_VERSION_40         ///< The ClearType-era interpreter, with its backward-compatibility rules.
} GFNT_HintVersion;

/** What became of a glyph asked to be hinted. */
typedef enum GFNT_HintStatus {
  GFNT_HINT_STATUS_HINTED = 0,                 ///< Its program ran.
  GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM,        ///< It has none, or the font does not hint at this size.
  GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED     ///< The context or a program failed; the outline is as drawn.
} GFNT_HintStatus;

/**
 * @brief A face prepared for hinting at one size.
 *
 * Made by ::gfnt_hint_context_create(), which runs the font program and the
 * control value program once. Immutable afterwards, so glyphs can be hinted
 * from it on several threads at once. The face must outlive it.
 */
typedef struct GFNT_HintContext GFNT_HintContext;

/**
 * @brief Prepare a face for hinting at @p ppem.
 *
 * A program that fails does not fail this call; every glyph then comes back
 * unhinted with ::GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED.
 *
 * @param face The face.
 * @param ppem Pixels per em; zero is invalid, and above `max_ppem` is ::GFNT_ERR_LIMIT.
 * @param version The interpreter behaviour.
 * @param variation The normalised location of a variable face, or NULL for the default
 *   instance. The control values come with `cvar` applied, `GETVARIATION` answers with it
 *   and every glyph hints from its `gvar`-moved points.
 * @param out_context Receives the context; free it with ::gfnt_hint_context_destroy().
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED for a face that has no TrueType
 *   outlines; ::GFNT_ERR_INVALID, ::GFNT_ERR_LIMIT, ::GFNT_ERR_CORRUPT or ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_hint_context_create(const GFNT_Face * face,
    uint32_t ppem, GFNT_HintVersion version, const GFNT_Variation * variation,
    GFNT_HintContext ** out_context, GFNT_Error * error);

/** Free a context. NULL is ignored. */
GFNT_API void gfnt_hint_context_destroy(GFNT_HintContext * context);

/**
 * @brief Whether the font asks for grid-fitting at a size.
 *
 * Reads `gasp`: true when the range holding @p ppem sets its grid-fit bit, or
 * when the face has no `gasp` (a font that carries a program is asking).
 *
 * @param face The face.
 * @param ppem Pixels per em.
 * @return Whether to hint.
 */
GFNT_API bool gfnt_face_gasp_gridfit(const GFNT_Face * face, uint32_t ppem);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GFNT_HINT_H */
