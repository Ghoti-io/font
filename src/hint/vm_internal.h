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
 * The context and run structures behind vm.h, shared by the interpreter
 * (tt_exec.c) and the code that builds contexts (tt_prog.c).
 */

#ifndef GHOTI_IO_GFNT_HINT_VM_INTERNAL_H
#define GHOTI_IO_GFNT_HINT_VM_INTERNAL_H

#include <ghoti.io/font/macros.h>
#include "hint_internal.h"
#include "vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A defined function, or an instruction definition. */
typedef struct GFNT_HintFn {
  size_t start;      ///< The offset of the first instruction of the body.
  size_t end;        ///< The offset of the `ENDF` that closes it.
  uint8_t program;   ///< A ::GFNT_HintProgram: where the body is.
  uint8_t opcode;    ///< For an instruction definition, the opcode.
  bool defined;
} GFNT_HintFn;

/** One level of the call stack. */
typedef struct GFNT_HintFrame {
  const GFNT_HintFn * fn;
  size_t return_ip;
  size_t loops_left;
  uint8_t return_program;
} GFNT_HintFrame;

/** A point of a zone, in 26.6 pixels (or font units, for the unscaled copy). */
typedef struct GFNT_HintPoint {
  int32_t x;
  int32_t y;
} GFNT_HintPoint;

#define GFNT_HINT_TOUCH_X 1u
#define GFNT_HINT_TOUCH_Y 2u
#define GFNT_HINT_ON_CURVE 4u

/** A zone: the twilight zone (0) or the glyph's own points (1). */
typedef struct GFNT_HintZone {
  size_t count;            ///< Points in use.
  size_t capacity;         ///< Points allocated.
  GFNT_HintPoint * cur;    ///< Where they are now.
  GFNT_HintPoint * org;    ///< Where they started, scaled and unhinted.
  GFNT_HintPoint * orus;   ///< Where they started, in font units.
  uint8_t * flags;         ///< ON_CURVE and the two touch bits.
  size_t * ends;           ///< The last point of each contour.
  size_t contour_count;
} GFNT_HintZone;

struct GFNT_HintContext {
  const GFNT_Allocator * allocator;
  GFNT_HintLimits limits;
  GFNT_HintParams params;
  GFNT_HintShape shape;
  int32_t scale;                  ///< 16.16: pixels (26.6) per font unit.
  const uint8_t * code[2];        ///< `fpgm`, `prep` (borrowed).
  size_t code_length[2];
  uint8_t * boundary[2];          ///< Which offsets begin an instruction.
  GFNT_HintFn * functions;
  size_t function_count;
  GFNT_HintFn * idefs;            ///< A list; an entry's opcode names it.
  size_t idef_capacity;
  size_t idef_count;
  int32_t * cvt;
  size_t cvt_count;
  int32_t * storage;
  size_t storage_count;
  size_t stack_capacity;
  GFNT_HintGState gstate;
  uint32_t instruct_control;
  GFNT_HintZone twilight;         ///< As `prep` left it.
  bool usable;
  GFNT_HintFault fault;
};

struct GFNT_HintRun {
  const GFNT_Allocator * allocator;
  GFNT_HintContext * ctx;         ///< Writable only while the context is built.
  bool building;                  ///< Whether this run is `fpgm`/`prep`.
  int32_t * stack;
  size_t depth;
  size_t stack_capacity;
  int32_t * storage;
  size_t storage_count;
  int32_t * cvt;
  size_t cvt_count;
  bool owns_state;                ///< Whether storage and cvt are this run's own.
  GFNT_HintGState gs;
  uint32_t instruct_control;
  GFNT_HintZone zone[2];          ///< 0 twilight, 1 glyph.
  GFNT_HintFrame * frames;
  uint8_t * glyph_boundary;
  const uint8_t * glyph_code;
  size_t glyph_length;
  GFNT_HintFault fault;
  bool normalised;                ///< Whether a vector was made from a direction.
  bool iup_called[2];             ///< Whether IUP has run along y, along x.
};

/**
 * Execute one program over a run.
 *
 * @param run The run.
 * @param program Which program it is.
 * @return ::GFNT_HINT_OK or the failure; the run's fault holds the detail.
 */
GFNT_HintFailure gfnt_hint_exec(GFNT_HintRun * run, GFNT_HintProgram program);

/** Allocate a zone's arrays for @p capacity points and @p contours. */
GFNT_Result gfnt_hint_zone_alloc(const GFNT_Allocator * allocator,
    GFNT_HintZone * zone, size_t capacity, size_t contours);

/** Free a zone's arrays. */
void gfnt_hint_zone_free(const GFNT_Allocator * allocator, GFNT_HintZone * zone);

/** Copy @p from into @p to, which must be at least as large. */
void gfnt_hint_zone_copy(GFNT_HintZone * to, const GFNT_HintZone * from);

/** Whether @p op is one of the instructions that move or measure points. */
bool gfnt_hint_is_point_op(uint8_t op);

/**
 * Execute one of the point instructions: pop its operands, do its work in the
 * run's zones.
 *
 * @param run The run.
 * @param op The opcode.
 * @param program The program being executed (the interpreter treats glyph
 *   programs and font programs differently in a few places).
 * @return ::GFNT_HINT_OK or the failure.
 */
GFNT_HintFailure gfnt_hint_point_op(GFNT_HintRun * run, uint8_t op,
    GFNT_HintProgram program);

/** Mark the offsets of @p code that begin an instruction; NULL on no memory. */
uint8_t * gfnt_hint_boundaries(const GFNT_Allocator * allocator,
    const uint8_t * code, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GFNT_HINT_VM_INTERNAL_H */
