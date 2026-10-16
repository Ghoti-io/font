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
 * Decoding, validating and printing TrueType programs. Nothing here runs one.
 */

#include "hint_internal.h"

#include <ghoti.io/font/hint.h>

GFNT_Result gfnt_hint_decode(const uint8_t * program, size_t length,
    size_t offset, GFNT_HintInsn * out) {
  const GFNT_HintOp * op;
  size_t count = 0;
  size_t unit = 1;
  size_t header = 1;

  if (!program || !out || offset >= length) {
    return GFNT_ERR_INVALID;
  }
  op = gfnt_hint_op(program[offset]);
  switch ((GFNT_HintData)op->data) {
    case GFNT_HINT_DATA_NPUSHW:
      unit = 2;
      // fall through
    case GFNT_HINT_DATA_NPUSHB:
      if (offset + 1u >= length) {
        return GFNT_ERR_CORRUPT;
      }
      count = program[offset + 1u];
      header = 2;
      break;
    case GFNT_HINT_DATA_PUSHW:
      unit = 2;
      // fall through
    case GFNT_HINT_DATA_PUSHB:
      count = (size_t)(program[offset] - op->first) + 1u;
      break;
    case GFNT_HINT_DATA_NONE:
      break;
  }
  // count and unit are at most 255 and 2, so this cannot overflow size_t unless
  // offset already does, and offset < length.
  if (count * unit > length - offset - header) {
    return GFNT_ERR_CORRUPT;
  }
  out->offset = offset;
  out->opcode = program[offset];
  out->count = count;
  out->word = unit == 2;
  out->data_offset = offset + header;
  out->length = header + count * unit;
  return GFNT_OK;
}

int32_t gfnt_hint_push_value(const uint8_t * program, const GFNT_HintInsn * insn,
    size_t index) {
  if (!insn->word) {
    return program[insn->data_offset + index];
  }
  size_t at = insn->data_offset + index * 2u;
  uint32_t value = ((uint32_t)program[at] << 8) | program[at + 1u];

  return (int32_t)(value >= 0x8000u ? (int64_t)value - 0x10000 : (int64_t)value);
}

GFNT_Result gfnt_program_validate(const uint8_t * bytes, size_t length,
    GFNT_ProgramReport * out_report) {
  GFNT_ProgramReport report = { GFNT_PROGRAM_OK, 0 };
  size_t if_depth = 0;
  // The innermost open IF (offset of the oldest unclosed one is what a report
  // names), the definition currently open, and the IF depth when it opened.
  size_t first_open_if = 0;
  bool in_def = false;
  size_t def_offset = 0;
  size_t def_if_depth = 0;
  size_t at = 0;

  if (!out_report || (!bytes && length)) {
    return GFNT_ERR_INVALID;
  }
  while (at < length && report.problem == GFNT_PROGRAM_OK) {
    GFNT_HintInsn insn;

    if (gfnt_hint_decode(bytes, length, at, &insn) != GFNT_OK) {
      report.problem = GFNT_PROGRAM_TRUNCATED_PUSH;
      report.offset = at;
      break;
    }
    switch (insn.opcode) {
      case 0x58: // IF
        if (if_depth == 0) {
          first_open_if = at;
        }
        ++if_depth;
        break;
      case 0x1B: // ELSE
      case 0x59: // EIF
        if (if_depth == 0 || (in_def && if_depth <= def_if_depth)) {
          report.problem = GFNT_PROGRAM_UNBALANCED_IF;
          report.offset = at;
        }
        else if (insn.opcode == 0x59) {
          --if_depth;
        }
        break;
      case 0x2C: // FDEF
      case 0x89: // IDEF
        if (in_def) {
          report.problem = GFNT_PROGRAM_UNBALANCED_DEF;
          report.offset = at;
        }
        else {
          in_def = true;
          def_offset = at;
          def_if_depth = if_depth;
        }
        break;
      case 0x2D: // ENDF
        if (!in_def) {
          report.problem = GFNT_PROGRAM_UNBALANCED_DEF;
          report.offset = at;
        }
        else if (if_depth != def_if_depth) {
          report.problem = GFNT_PROGRAM_UNBALANCED_IF;
          report.offset = at;
        }
        else {
          in_def = false;
        }
        break;
      default:
        break;
    }
    at += insn.length;
  }
  if (report.problem == GFNT_PROGRAM_OK && in_def) {
    report.problem = GFNT_PROGRAM_UNBALANCED_DEF;
    report.offset = def_offset;
  }
  if (report.problem == GFNT_PROGRAM_OK && if_depth) {
    report.problem = GFNT_PROGRAM_UNBALANCED_IF;
    report.offset = first_open_if;
  }
  *out_report = report;
  return GFNT_OK;
}

GFNT_Result gfnt_program_disassemble(const uint8_t * bytes, size_t length,
    FILE * stream) {
  size_t at = 0;

  if (!stream || (!bytes && length)) {
    return GFNT_ERR_INVALID;
  }
  while (at < length) {
    GFNT_HintInsn insn;
    const GFNT_HintOp * op;
    char flags[8];
    int written;

    if (gfnt_hint_decode(bytes, length, at, &insn) != GFNT_OK) {
      return fprintf(stream, "TRUNCATED[%zu]\n", at) < 0 ? GFNT_ERR_IO : GFNT_OK;
    }
    op = gfnt_hint_op(insn.opcode);
    if (!op->defined) {
      written = fprintf(stream, "INSTR%u[ ]\n", (unsigned)insn.opcode);
    }
    else if (op->flag_bits == 0) {
      written = fprintf(stream, "%s[ ]\n", op->name);
    }
    else {
      for (unsigned b = 0; b < op->flag_bits; ++b) {
        flags[b] = ((insn.opcode - op->first) >> (op->flag_bits - 1u - b)) & 1u ? '1' : '0';
      }
      flags[op->flag_bits] = '\0';
      written = fprintf(stream, "%s[%s]\n", op->name, flags);
    }
    if (written < 0) {
      return GFNT_ERR_IO;
    }
    for (size_t i = 0; i < insn.count; ++i) {
      if (fprintf(stream, "%d\n", (int)gfnt_hint_push_value(bytes, &insn, i)) < 0) {
        return GFNT_ERR_IO;
      }
    }
    at += insn.length;
  }
  return GFNT_OK;
}
