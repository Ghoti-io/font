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
 * The TrueType opcode table, from the OpenType specification's "TrueType
 * Instruction Set" chapter and from nothing else: mnemonic, how many low bits
 * are flags, the stack effect and what data follows. An opcode the chapter does
 * not define is undefined here.
 */

#include "hint_internal.h"

typedef struct GFNT_HintRange {
  uint8_t first;
  uint8_t last;
  GFNT_HintOp op;
} GFNT_HintRange;

#define R(first, last, name, bits, pops, pushes, data) \
  { first, last, { name, bits, pops, pushes, data, true, first } }

// Ordered by opcode. A stack effect of GFNT_HINT_VARIABLE depends on the state: a loop
// count, or a value read off the stack. Otherwise the counts are exact.
static const GFNT_HintRange gfnt_hint_ranges[] = {
  R(0x00, 0x01, "SVTCA", 1, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x02, 0x03, "SPVTCA", 1, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x04, 0x05, "SFVTCA", 1, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x06, 0x07, "SPVTL", 1, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x08, 0x09, "SFVTL", 1, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x0A, 0x0A, "SPVFS", 0, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x0B, 0x0B, "SFVFS", 0, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x0C, 0x0C, "GPV", 0, 0, 2, GFNT_HINT_DATA_NONE),
  R(0x0D, 0x0D, "GFV", 0, 0, 2, GFNT_HINT_DATA_NONE),
  R(0x0E, 0x0E, "SFVTPV", 0, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x0F, 0x0F, "ISECT", 0, 5, 0, GFNT_HINT_DATA_NONE),
  R(0x10, 0x10, "SRP0", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x11, 0x11, "SRP1", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x12, 0x12, "SRP2", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x13, 0x13, "SZP0", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x14, 0x14, "SZP1", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x15, 0x15, "SZP2", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x16, 0x16, "SZPS", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x17, 0x17, "SLOOP", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x18, 0x18, "RTG", 0, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x19, 0x19, "RTHG", 0, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x1A, 0x1A, "SMD", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x1B, 0x1B, "ELSE", 0, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x1C, 0x1C, "JMPR", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x1D, 0x1D, "SCVTCI", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x1E, 0x1E, "SSWCI", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x1F, 0x1F, "SSW", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x20, 0x20, "DUP", 0, 1, 2, GFNT_HINT_DATA_NONE),
  R(0x21, 0x21, "POP", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x22, 0x22, "CLEAR", 0, GFNT_HINT_VARIABLE, 0, GFNT_HINT_DATA_NONE),
  R(0x23, 0x23, "SWAP", 0, 2, 2, GFNT_HINT_DATA_NONE),
  R(0x24, 0x24, "DEPTH", 0, 0, 1, GFNT_HINT_DATA_NONE),
  R(0x25, 0x25, "CINDEX", 0, GFNT_HINT_VARIABLE, 1, GFNT_HINT_DATA_NONE),
  R(0x26, 0x26, "MINDEX", 0, GFNT_HINT_VARIABLE, GFNT_HINT_VARIABLE, GFNT_HINT_DATA_NONE),
  R(0x27, 0x27, "ALIGNPTS", 0, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x29, 0x29, "UTP", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x2A, 0x2A, "LOOPCALL", 0, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x2B, 0x2B, "CALL", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x2C, 0x2C, "FDEF", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x2D, 0x2D, "ENDF", 0, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x2E, 0x2F, "MDAP", 1, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x30, 0x31, "IUP", 1, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x32, 0x33, "SHP", 1, GFNT_HINT_VARIABLE, 0, GFNT_HINT_DATA_NONE),
  R(0x34, 0x35, "SHC", 1, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x36, 0x37, "SHZ", 1, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x38, 0x38, "SHPIX", 0, GFNT_HINT_VARIABLE, 0, GFNT_HINT_DATA_NONE),
  R(0x39, 0x39, "IP", 0, GFNT_HINT_VARIABLE, 0, GFNT_HINT_DATA_NONE),
  R(0x3A, 0x3B, "MSIRP", 1, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x3C, 0x3C, "ALIGNRP", 0, GFNT_HINT_VARIABLE, 0, GFNT_HINT_DATA_NONE),
  R(0x3D, 0x3D, "RTDG", 0, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x3E, 0x3F, "MIAP", 1, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x40, 0x40, "NPUSHB", 0, 0, GFNT_HINT_VARIABLE, GFNT_HINT_DATA_NPUSHB),
  R(0x41, 0x41, "NPUSHW", 0, 0, GFNT_HINT_VARIABLE, GFNT_HINT_DATA_NPUSHW),
  R(0x42, 0x42, "WS", 0, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x43, 0x43, "RS", 0, 1, 1, GFNT_HINT_DATA_NONE),
  R(0x44, 0x44, "WCVTP", 0, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x45, 0x45, "RCVT", 0, 1, 1, GFNT_HINT_DATA_NONE),
  R(0x46, 0x47, "GC", 1, 1, 1, GFNT_HINT_DATA_NONE),
  R(0x48, 0x48, "SCFS", 0, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x49, 0x4A, "MD", 1, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x4B, 0x4B, "MPPEM", 0, 0, 1, GFNT_HINT_DATA_NONE),
  R(0x4C, 0x4C, "MPS", 0, 0, 1, GFNT_HINT_DATA_NONE),
  R(0x4D, 0x4D, "FLIPON", 0, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x4E, 0x4E, "FLIPOFF", 0, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x4F, 0x4F, "DEBUG", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x50, 0x50, "LT", 0, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x51, 0x51, "LTEQ", 0, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x52, 0x52, "GT", 0, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x53, 0x53, "GTEQ", 0, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x54, 0x54, "EQ", 0, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x55, 0x55, "NEQ", 0, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x56, 0x56, "ODD", 0, 1, 1, GFNT_HINT_DATA_NONE),
  R(0x57, 0x57, "EVEN", 0, 1, 1, GFNT_HINT_DATA_NONE),
  R(0x58, 0x58, "IF", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x59, 0x59, "EIF", 0, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x5A, 0x5A, "AND", 0, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x5B, 0x5B, "OR", 0, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x5C, 0x5C, "NOT", 0, 1, 1, GFNT_HINT_DATA_NONE),
  R(0x5D, 0x5D, "DELTAP1", 0, GFNT_HINT_VARIABLE, 0, GFNT_HINT_DATA_NONE),
  R(0x5E, 0x5E, "SDB", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x5F, 0x5F, "SDS", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x60, 0x60, "ADD", 0, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x61, 0x61, "SUB", 0, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x62, 0x62, "DIV", 0, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x63, 0x63, "MUL", 0, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x64, 0x64, "ABS", 0, 1, 1, GFNT_HINT_DATA_NONE),
  R(0x65, 0x65, "NEG", 0, 1, 1, GFNT_HINT_DATA_NONE),
  R(0x66, 0x66, "FLOOR", 0, 1, 1, GFNT_HINT_DATA_NONE),
  R(0x67, 0x67, "CEILING", 0, 1, 1, GFNT_HINT_DATA_NONE),
  R(0x68, 0x6B, "ROUND", 2, 1, 1, GFNT_HINT_DATA_NONE),
  R(0x6C, 0x6F, "NROUND", 2, 1, 1, GFNT_HINT_DATA_NONE),
  R(0x70, 0x70, "WCVTF", 0, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x71, 0x71, "DELTAP2", 0, GFNT_HINT_VARIABLE, 0, GFNT_HINT_DATA_NONE),
  R(0x72, 0x72, "DELTAP3", 0, GFNT_HINT_VARIABLE, 0, GFNT_HINT_DATA_NONE),
  R(0x73, 0x73, "DELTAC1", 0, GFNT_HINT_VARIABLE, 0, GFNT_HINT_DATA_NONE),
  R(0x74, 0x74, "DELTAC2", 0, GFNT_HINT_VARIABLE, 0, GFNT_HINT_DATA_NONE),
  R(0x75, 0x75, "DELTAC3", 0, GFNT_HINT_VARIABLE, 0, GFNT_HINT_DATA_NONE),
  R(0x76, 0x76, "SROUND", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x77, 0x77, "S45ROUND", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x78, 0x78, "JROT", 0, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x79, 0x79, "JROF", 0, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x7A, 0x7A, "ROFF", 0, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x7C, 0x7C, "RUTG", 0, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x7D, 0x7D, "RDTG", 0, 0, 0, GFNT_HINT_DATA_NONE),
  R(0x7E, 0x7E, "SANGW", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x7F, 0x7F, "AA", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x80, 0x80, "FLIPPT", 0, GFNT_HINT_VARIABLE, 0, GFNT_HINT_DATA_NONE),
  R(0x81, 0x81, "FLIPRGON", 0, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x82, 0x82, "FLIPRGOFF", 0, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x85, 0x85, "SCANCTRL", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x86, 0x87, "SDPVTL", 1, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x88, 0x88, "GETINFO", 0, 1, 1, GFNT_HINT_DATA_NONE),
  R(0x89, 0x89, "IDEF", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x8A, 0x8A, "ROLL", 0, 3, 3, GFNT_HINT_DATA_NONE),
  R(0x8B, 0x8B, "MAX", 0, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x8C, 0x8C, "MIN", 0, 2, 1, GFNT_HINT_DATA_NONE),
  R(0x8D, 0x8D, "SCANTYPE", 0, 1, 0, GFNT_HINT_DATA_NONE),
  R(0x8E, 0x8E, "INSTCTRL", 0, 2, 0, GFNT_HINT_DATA_NONE),
  R(0x91, 0x91, "GETVARIATION", 0, 0, GFNT_HINT_VARIABLE, GFNT_HINT_DATA_NONE),
  R(0xB0, 0xB7, "PUSHB", 0, 0, GFNT_HINT_VARIABLE, GFNT_HINT_DATA_PUSHB),
  R(0xB8, 0xBF, "PUSHW", 0, 0, GFNT_HINT_VARIABLE, GFNT_HINT_DATA_PUSHW),
  R(0xC0, 0xDF, "MDRP", 5, 1, 0, GFNT_HINT_DATA_NONE),
  R(0xE0, 0xFF, "MIRP", 5, 2, 0, GFNT_HINT_DATA_NONE),
};

static const GFNT_HintOp gfnt_hint_undefined = {
  "INSTR", 0, 0, 0, GFNT_HINT_DATA_NONE, false, 0
};

const GFNT_HintOp * gfnt_hint_op(uint8_t opcode) {
  for (size_t i = 0; i < sizeof gfnt_hint_ranges / sizeof gfnt_hint_ranges[0]; ++i) {
    if (opcode < gfnt_hint_ranges[i].first) {
      break;
    }
    if (opcode <= gfnt_hint_ranges[i].last) {
      return &gfnt_hint_ranges[i].op;
    }
  }
  return &gfnt_hint_undefined;
}
