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
 * TrueType instructions, internally: the opcode table and the decoder.
 *
 * The one table both the decoder and the interpreter read, so that "how long is
 * this instruction" and "what does it do" cannot drift apart. Nothing in here
 * runs a program.
 */

#ifndef GHOTI_IO_GFNT_HINT_INTERNAL_H
#define GHOTI_IO_GFNT_HINT_INTERNAL_H

#include <ghoti.io/font/hint.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A stack effect that depends on the state: a loop count, an argument. */
#define GFNT_HINT_VARIABLE (-1)

/** What follows an opcode in the byte stream. */
typedef enum GFNT_HintData {
  GFNT_HINT_DATA_NONE = 0, ///< Nothing.
  GFNT_HINT_DATA_NPUSHB,   ///< A count byte, then that many bytes.
  GFNT_HINT_DATA_NPUSHW,   ///< A count byte, then that many big-endian words.
  GFNT_HINT_DATA_PUSHB,    ///< 1 + the low three bits of the opcode, bytes.
  GFNT_HINT_DATA_PUSHW     ///< 1 + the low three bits of the opcode, words.
} GFNT_HintData;

/** What the table says of one opcode value. */
typedef struct GFNT_HintOp {
  const char * name;   ///< The mnemonic without flags; "INSTR" for an undefined opcode.
  uint8_t flag_bits;   ///< How many low bits of the opcode are flags (0 to 5).
  int8_t pops;         ///< Values popped, or ::GFNT_HINT_VARIABLE.
  int8_t pushes;       ///< Values pushed, or ::GFNT_HINT_VARIABLE.
  uint8_t data;        ///< A ::GFNT_HintData.
  bool defined;        ///< False for an opcode the specification leaves undefined.
  uint8_t first;       ///< The first opcode of the row; an opcode's flags are its
                       ///< distance from this (they are not its low bits: `MD`
                       ///< is 0x49 and 0x4A).
} GFNT_HintOp;

/**
 * The table's row for @p opcode. Never NULL; an undefined opcode has a row with
 * ::GFNT_HintOp::defined false, which a program may still give a meaning with
 * `IDEF`.
 */
const GFNT_HintOp * gfnt_hint_op(uint8_t opcode);

/** One decoded instruction. */
typedef struct GFNT_HintInsn {
  size_t offset;      ///< Where it starts in the program.
  size_t length;      ///< Its whole length in bytes, opcode and data.
  size_t count;       ///< Values pushed by a push; otherwise 0.
  size_t data_offset; ///< Where a push's first value starts.
  uint8_t opcode;     ///< The opcode byte.
  bool word;          ///< A push of words.
} GFNT_HintInsn;

/**
 * Decode the instruction at @p offset.
 *
 * @param program The program.
 * @param length Its length.
 * @param offset Where the instruction starts; less than @p length.
 * @param out Receives the instruction. Written only on success.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for an offset outside the program;
 *   ::GFNT_ERR_CORRUPT when a push's data runs past the end.
 */
GFNT_Result gfnt_hint_decode(const uint8_t * program, size_t length,
    size_t offset, GFNT_HintInsn * out);

/**
 * The @p index'th value a push instruction pushes: a byte zero-extended, a word
 * sign-extended. @p index must be below ::GFNT_HintInsn::count.
 */
int32_t gfnt_hint_push_value(const uint8_t * program, const GFNT_HintInsn * insn,
    size_t index);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GFNT_HINT_INTERNAL_H */
