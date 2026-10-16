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
 * The TrueType virtual machine, internally: limits, the per-size context that
 * runs `fpgm` and `prep`, and a run of one glyph's program over that context.
 *
 * Font programs are untrusted input. Every access to the stack, storage, the
 * control value table, the function table and the instruction stream is
 * checked; every loop is bounded by an instruction budget; a fault ends that
 * program with a named failure and never corrupts anything.
 *
 * This file declares what the instructions in `tt_exec.c` need to share. The
 * point-moving instructions arrive with the zones (H2); until they do, a program
 * that executes one fails with ::GFNT_HINT_FAIL_UNSUPPORTED.
 */

#ifndef GHOTI_IO_GFNT_HINT_VM_H
#define GHOTI_IO_GFNT_HINT_VM_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The caps on what one program may consume. */
typedef struct GFNT_HintLimits {
  size_t max_stack;        ///< Stack elements. 65,536.
  size_t max_storage;      ///< Storage locations. 65,536.
  size_t max_cvt;          ///< Control values. 262,144.
  size_t max_functions;    ///< Function numbers. 65,536.
  size_t max_idefs;        ///< Instruction definitions. 256.
  size_t max_call_depth;   ///< Nested calls. 64.
  size_t max_instructions; ///< Instructions one program may execute. 4,000,000.
  size_t max_loop;         ///< The `SLOOP` count. 65,535.
  size_t max_program;      ///< Bytes in one program. 1 MiB.
  size_t max_points;       ///< Points in a zone, twilight or glyph. 65,540.
} GFNT_HintLimits;

/** Fill in the default limits. */
void gfnt_hint_limits_default(GFNT_HintLimits * limits);

/** Why a program stopped. */
typedef enum GFNT_HintFailure {
  GFNT_HINT_OK = 0,
  GFNT_HINT_FAIL_STACK_UNDERFLOW,  ///< An instruction popped from an empty stack.
  GFNT_HINT_FAIL_STACK_OVERFLOW,   ///< A push past the stack's capacity.
  GFNT_HINT_FAIL_BAD_INDEX,        ///< A storage, CVT, function or stack index out of range.
  GFNT_HINT_FAIL_BAD_OPCODE,       ///< An undefined opcode with no definition.
  GFNT_HINT_FAIL_TRUNCATED,        ///< A push, or a skipped region, runs past the end.
  GFNT_HINT_FAIL_BAD_JUMP,         ///< A jump outside the program or into an operand.
  GFNT_HINT_FAIL_BAD_STRUCTURE,    ///< ENDF with no call, a nested definition, a definition in a glyph program.
  GFNT_HINT_FAIL_UNDEFINED_CALL,   ///< CALL of a function never defined.
  GFNT_HINT_FAIL_DIVIDE_BY_ZERO,   ///< DIV by zero.
  GFNT_HINT_FAIL_BAD_ARGUMENT,     ///< An operand the instruction cannot take.
  GFNT_HINT_FAIL_BUDGET,           ///< The instruction budget ran out.
  GFNT_HINT_FAIL_CALL_DEPTH,       ///< Calls nested too deeply.
  GFNT_HINT_FAIL_UNSUPPORTED,      ///< An instruction not implemented yet.
  GFNT_HINT_FAIL_NO_MEMORY         ///< An allocation failed.
} GFNT_HintFailure;

/** A name for a failure, for diagnostics. */
const char * gfnt_hint_failure_name(GFNT_HintFailure failure);

/** Which program a failure, or a piece of code, belongs to. */
typedef enum GFNT_HintProgram {
  GFNT_HINT_PROGRAM_FONT = 0, ///< `fpgm`.
  GFNT_HINT_PROGRAM_CVT,      ///< `prep`.
  GFNT_HINT_PROGRAM_GLYPH     ///< A glyph's own.
} GFNT_HintProgram;

/** Where and why a program stopped. */
typedef struct GFNT_HintFault {
  GFNT_HintFailure failure; ///< ::GFNT_HINT_OK when nothing went wrong.
  GFNT_HintProgram program; ///< The program that was running.
  size_t offset;            ///< The instruction's offset in it.
  uint8_t opcode;           ///< The instruction.
} GFNT_HintFault;

/** How the graphics state rounds. */
typedef enum GFNT_HintRound {
  GFNT_HINT_ROUND_GRID = 0, ///< RTG: to the nearest whole pixel.
  GFNT_HINT_ROUND_HALF,     ///< RTHG: to the nearest half pixel (pixel + 1/2).
  GFNT_HINT_ROUND_DOUBLE,   ///< RTDG: to the nearest half pixel (multiples of 1/2).
  GFNT_HINT_ROUND_DOWN,     ///< RDTG: down to a whole pixel.
  GFNT_HINT_ROUND_UP,       ///< RUTG: up to a whole pixel.
  GFNT_HINT_ROUND_OFF,      ///< ROFF: not at all.
  GFNT_HINT_ROUND_SUPER,    ///< SROUND: period, phase and threshold.
  GFNT_HINT_ROUND_SUPER45   ///< S45ROUND: the same, on a 45-degree grid.
} GFNT_HintRound;

/** The graphics state: the specification's table, with its defaults. */
typedef struct GFNT_HintGState {
  bool auto_flip;            ///< TRUE.
  int32_t cvt_cut_in;        ///< 17/16 pixel (26.6).
  int32_t delta_base;        ///< 9.
  int32_t delta_shift;       ///< 3.
  int32_t loop;              ///< 1.
  int32_t min_distance;      ///< 1 pixel.
  int32_t sw_cut_in;         ///< 0.
  int32_t sw_value;          ///< 0 (pixels, 26.6).
  GFNT_HintRound round;      ///< RTG.
  int32_t period;            ///< Rounding period, in 1/1024 of a 26.6 unit.
  int32_t phase;             ///< Rounding phase, in the same unit.
  int32_t threshold;         ///< Rounding threshold, in the same unit.
  uint16_t scan_control;     ///< As `SCANCTRL` set it; 0.
  uint16_t scan_type;        ///< As `SCANTYPE` set it; 0.
  int16_t pv[2];             ///< Projection vector, 2.14; the x axis.
  int16_t fv[2];             ///< Freedom vector, 2.14; the x axis.
  int16_t dpv[2];            ///< Dual projection vector, 2.14.
  uint32_t rp[3];            ///< Reference points 0 to 2; 0.
  uint8_t zp[3];             ///< Zone pointers 0 to 2; 1.
} GFNT_HintGState;

/** What the program is told about the size it runs at. */
typedef struct GFNT_HintParams {
  uint32_t ppem;           ///< Pixels per em, whole.
  uint16_t units_per_em;   ///< The face's em.
  int version;             ///< The interpreter version: 35 or 40.
  bool grayscale;          ///< Whether to say the target is grayscale.
} GFNT_HintParams;

/** The sizes of the arrays a font declares in `maxp`. */
typedef struct GFNT_HintShape {
  size_t stack;     ///< maxStackElements.
  size_t storage;   ///< maxStorage.
  size_t functions; ///< maxFunctionDefs.
  size_t idefs;     ///< maxInstructionDefs.
  size_t twilight;  ///< maxTwilightPoints.
} GFNT_HintShape;

/**
 * A glyph's points as the run takes them: in font units, the four phantom points
 * last, with the contours that end at each point.
 */
typedef struct GFNT_HintGlyphInput {
  size_t point_count;        ///< Points including the four phantom points.
  const int32_t * x;         ///< Font units.
  const int32_t * y;
  const uint8_t * on_curve;  ///< Non-zero for an on-curve point.
  size_t contour_count;
  const size_t * ends;       ///< The last point of each contour.
} GFNT_HintGlyphInput;

typedef struct GFNT_HintContext GFNT_HintContext;
typedef struct GFNT_HintRun GFNT_HintRun;

/**
 * Make the context for one size: scale the control values, run `fpgm`, then
 * `prep`.
 *
 * A program that fails does not fail the call: the context records which one
 * and why, and ::gfnt_hint_ctx_usable() says it cannot be used to hint. The call
 * fails only for a bad argument, a limit exceeded by the font's declared sizes
 * or memory.
 *
 * @param allocator The allocator.
 * @param limits The caps, or NULL for the defaults.
 * @param params The size.
 * @param shape The array sizes `maxp` declares.
 * @param fpgm The font program (borrowed; must outlive the context), or NULL.
 * @param fpgm_length Its length.
 * @param prep The control value program (borrowed), or NULL.
 * @param prep_length Its length.
 * @param cvt The control values in font units, or NULL.
 * @param cvt_count How many.
 * @param out Receives the context.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_LIMIT or ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_hint_ctx_create(const GFNT_Allocator * allocator,
    const GFNT_HintLimits * limits, const GFNT_HintParams * params,
    const GFNT_HintShape * shape, const uint8_t * fpgm, size_t fpgm_length,
    const uint8_t * prep, size_t prep_length, const int32_t * cvt,
    size_t cvt_count, GFNT_HintContext ** out);

/**
 * Make the context for a face at a size, gathering what ::gfnt_hint_ctx_create()
 * needs from the face: the `maxp` sizes, the control values at the default
 * location, and the two font programs (borrowed from the face, which must
 * outlive the context).
 *
 * @param face The face.
 * @param ppem Pixels per em.
 * @param version The interpreter version, 35 or 40.
 * @param grayscale Whether to tell the program the target is grayscale.
 * @param limits The caps, or NULL for the defaults.
 * @param out Receives the context.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED for a face with no `glyf` or a
 *   `maxp` that is not version 1.0; ::GFNT_ERR_CORRUPT; ::GFNT_ERR_LIMIT;
 *   ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_hint_ctx_from_face(const GFNT_Face * face, uint32_t ppem,
    int version, bool grayscale, const GFNT_HintLimits * limits,
    GFNT_HintContext ** out, GFNT_Error * error);

/** What became of a glyph asked to be hinted. */
typedef enum GFNT_HintStatus {
  GFNT_HINT_STATUS_HINTED = 0,                 ///< Its program ran.
  GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM,        ///< It has none.
  GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED,    ///< The context or its program failed.
  GFNT_HINT_STATUS_UNHINTED_COMPOSITE          ///< Composites arrive with the next step.
} GFNT_HintStatus;

/**
 * Hint one glyph: its outline in pixels after its program has run, or unhinted
 * and scaled the same way when it cannot be, and the status says which.
 *
 * @param face The face the context was made from.
 * @param ctx The context.
 * @param glyph The glyph.
 * @param out Receives the outline, in ::GFNT_OUTLINE_PIXELS.
 * @param status Receives what happened.
 * @param fault Receives the program's fault when it failed, or NULL.
 * @param advance Receives the advance in 26.6 as the glyph's phantom points left
 *   it (the scaled advance when unhinted), or NULL.
 * @param touches Receives one touch byte per outline point (bit 0 x, bit 1 y) when
 *   the glyph was hinted, or NULL. A diagnostic: it says which points the program
 *   moved and which the interpolation placed.
 * @param touch_capacity How many bytes @p touches holds.
 * @param normalised Receives whether the program made a vector from a direction
 *   (see ::gfnt_hint_run_normalised()), or NULL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK (hinted or not), or a failure to read or allocate.
 */
GFNT_Result gfnt_hint_glyph_outline(const GFNT_Face * face,
    const GFNT_HintContext * ctx, uint32_t glyph, GFNT_Outline ** out,
    GFNT_HintStatus * status, GFNT_HintFault * fault, int32_t * advance,
    uint8_t * touches, size_t touch_capacity, bool * normalised, GFNT_Error * error);

/** Free a context. NULL is ignored. */
void gfnt_hint_ctx_destroy(GFNT_HintContext * ctx);

/** Whether `fpgm` and `prep` both ran to the end. */
bool gfnt_hint_ctx_usable(const GFNT_HintContext * ctx);

/** The fault that stopped `fpgm` or `prep`, or one with ::GFNT_HINT_OK. */
GFNT_HintFault gfnt_hint_ctx_fault(const GFNT_HintContext * ctx);

/** The scaled control value @p index, in 26.6; 0 for an index out of range. */
int32_t gfnt_hint_ctx_cvt(const GFNT_HintContext * ctx, size_t index);

/** The storage value @p index after `prep`; 0 for an index out of range. */
int32_t gfnt_hint_ctx_storage(const GFNT_HintContext * ctx, size_t index);

/** The graphics state `prep` left, which is each glyph's starting one. */
const GFNT_HintGState * gfnt_hint_ctx_gstate(const GFNT_HintContext * ctx);

/** The `INSTCTRL` flags `prep` set: bit 0 no grid-fitting, bit 1 default state, bit 2 native ClearType. */
uint32_t gfnt_hint_ctx_instruct_control(const GFNT_HintContext * ctx);

/**
 * Run one glyph's program over the context.
 *
 * The run works on its own copy of the control values, storage and graphics
 * state, so the context is not changed and any number of runs may proceed on
 * different threads.
 *
 * @param ctx The context; one that is not usable is refused.
 * @param input The glyph's points, or NULL for none (a test of the arithmetic).
 * @param program The glyph's instructions.
 * @param length Their length.
 * @param out Receives the run, **also when the program fails** (so its fault and
 *   partial state can be read); NULL only on allocation failure.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a context that is not usable;
 *   ::GFNT_ERR_CORRUPT, ::GFNT_ERR_LIMIT or ::GFNT_ERR_UNSUPPORTED for a program
 *   that failed (see ::gfnt_hint_run_fault()); ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_hint_run_glyph(const GFNT_HintContext * ctx,
    const GFNT_HintGlyphInput * input, const uint8_t * program, size_t length,
    GFNT_HintRun ** out);

/** Free a run. NULL is ignored. */
void gfnt_hint_run_destroy(GFNT_HintRun * run);

/** Why the run stopped. */
GFNT_HintFault gfnt_hint_run_fault(const GFNT_HintRun * run);

/** How many values are on the stack. */
size_t gfnt_hint_run_depth(const GFNT_HintRun * run);

/** The stack value @p index from the bottom; 0 if out of range. */
int32_t gfnt_hint_run_stack(const GFNT_HintRun * run, size_t index);

/** The storage value @p index; 0 if out of range. */
int32_t gfnt_hint_run_storage(const GFNT_HintRun * run, size_t index);

/** The control value @p index, in 26.6, as the run left it; 0 if out of range. */
int32_t gfnt_hint_run_cvt(const GFNT_HintRun * run, size_t index);

/** How many points the glyph zone holds. */
size_t gfnt_hint_run_point_count(const GFNT_HintRun * run);

/** Where point @p index of the glyph zone ended up, in 26.6; false if out of range. */
bool gfnt_hint_run_point(const GFNT_HintRun * run, size_t index, int32_t * x, int32_t * y);

/** Whether the run made a unit vector from a direction (`SPVTL` and its kin), which is where this library and FreeType can differ by a 16384th. */
bool gfnt_hint_run_normalised(const GFNT_HintRun * run);

/** The touch flags (bit 0 x, bit 1 y) of glyph-zone point @p index at the end of the run. */
uint8_t gfnt_hint_run_touch(const GFNT_HintRun * run, size_t index);

/** The `INSTCTRL` flags the run ended with. */
uint32_t gfnt_hint_run_instruct_control(const GFNT_HintRun * run);

/** The graphics state the run ended in. */
const GFNT_HintGState * gfnt_hint_run_gstate(const GFNT_HintRun * run);

/* -- arithmetic (tt_math.c) ------------------------------------------------ */

/** a * b / c, rounded to nearest with ties away from zero; 0 if c is 0. */
int32_t gfnt_hint_muldiv(int32_t a, int32_t b, int32_t c);
/** a * b / c, truncated toward zero; 0 if c is 0. */
int32_t gfnt_hint_muldiv_trunc(int32_t a, int32_t b, int32_t c);
/** a * b where b is 16.16, rounded to nearest, ties away from zero. */
int32_t gfnt_hint_mul_fix(int32_t a, int32_t b);
/** The 16.16 ratio a / b, rounded; 0 if b is 0. */
int32_t gfnt_hint_div_fix(int32_t a, int32_t b);
/** a + b modulo 2^32, as the machine's 32-bit words add. */
int32_t gfnt_hint_add(int32_t a, int32_t b);
/** a - b modulo 2^32. */
int32_t gfnt_hint_sub(int32_t a, int32_t b);
/** The 26.6 value @p value rounded as the graphics state says (no compensation). */
int32_t gfnt_hint_round(const GFNT_HintGState * gs, int32_t value);
/** Set the rounding state from `SROUND`'s operand. */
void gfnt_hint_set_super_round(GFNT_HintGState * gs, int32_t operand,
    bool at_45_degrees);
/** Set a plain rounding state (everything but the two super modes). */
void gfnt_hint_set_round(GFNT_HintGState * gs, GFNT_HintRound mode);
/** Fill @p gs with the specification's defaults. */
void gfnt_hint_gstate_default(GFNT_HintGState * gs);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GFNT_HINT_VM_H */
