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

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GFNT_HINT_H */
