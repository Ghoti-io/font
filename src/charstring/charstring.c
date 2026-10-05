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
 * The Type 2 and Type 1 charstring interpreters.
 *
 * documentation/design.md section 7.4. One file because the two languages share
 * their curve operators and their idea of a current point, and differ in
 * everything around that: where the advance comes from, whether subroutine
 * numbers are biased, how hints are declared, and how flex is reached.
 *
 * **The operand stack is 16.16 throughout, and the current point with it.** A
 * charstring's coordinates are integers in every real font, but `div` exists and
 * `255` introduces a fixed-point literal, so a reader that kept integers would
 * round twice - once at the operand and once at the point - and disagree with
 * every reference in the last bit. Points are converted to 26.6 as they are
 * emitted, from an exact 16.16 accumulation, which is the same rule
 * `outline.c`'s transform uses.
 *
 * Reference: Adobe *The Type 2 Charstring Format* (technical note #5177) and
 * *Adobe Type 1 Font Format* (the black book), chapter 6 and appendix B for
 * `callothersubr`.
 */

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/charstring.h>
#include <ghoti.io/font/core.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../core/fixed.h"
#include "../outline/outline.h"
#include "charstring.h"

/**
 * Operands the stack holds.
 *
 * Type 2 states 48 and Type 1 states 24. One stack of 48 serves both: no
 * operator's *meaning* depends on where the stack ends, so the only thing a
 * stricter Type 1 limit would do is refuse a font that draws correctly. The
 * limit that matters against a hostile font is the operator count, which is in
 * ::GFNT_Limits where a caller can see it.
 */
#define GFNT_CS_STACK 48

/** The stack a CFF2 charstring may fill, which is what `blend` needs room for. */
#define GFNT_CS_STACK_CFF2 513

/** The most regions one `blend` may scale its deltas by. */
#define GFNT_CS_MAX_REGIONS 64

/** The fractional bits of a region scalar: 2^24 is one. */
#define GFNT_CS_SCALAR_BITS 24

/**
 * 2^16, as a multiplier rather than a shift.
 *
 * **A left shift of a negative value is undefined behaviour in C**, and every
 * one of these values can be negative: a charstring operand, a DICT operand, a
 * quotient. `fuzz_charstring` reported nine of them on its first run - the
 * number decoders here and their twin in the dump - and multiplying is both
 * defined and exactly the same arithmetic on every machine this library runs on.
 */
#define GFNT_FIXED_ONE 65536

/**
 * The largest value that can still be scaled to 16.16 inside an int64.
 *
 * `div` and `sqrt` both form their result by scaling an operand up by 65,536
 * first, and 2^61 times that is not an int64. Saturating to *int32* instead was
 * the first fix and was wrong in a way a test caught: `200 200 mul` is 40,000 in
 * 16.16, which is 2.6 billion as a raw value, so clamping it to int32 turned
 * `sqrt` of it into 181 rather than 200. The bound that matters is the one the
 * scaling needs, and this is it.
 */
#define GFNT_FIXED_SCALE_MAX (INT64_MAX / GFNT_FIXED_ONE)

/** Clamp to a magnitude ::GFNT_FIXED_SCALE_MAX allows, keeping the sign. */
static int64_t gfnt_cs_scalable(int64_t value) {
  if (value > GFNT_FIXED_SCALE_MAX) {
    return GFNT_FIXED_SCALE_MAX;
  }
  if (value < -GFNT_FIXED_SCALE_MAX) {
    return -GFNT_FIXED_SCALE_MAX;
  }
  return value;
}

/** The transient array `put` and `get` share. The specification's size. */
#define GFNT_CS_TRANSIENT 32

/** Type 1's PostScript-interpreter stack, which `pop` reads back. */
#define GFNT_CS_PS_STACK 8

/** Points one Type 1 flex sequence collects before it draws anything. */
#define GFNT_CS_FLEX_POINTS 7

// Type 2 one-byte operators. The numbers are the format's.
#define GFNT_CS_HSTEM 1
#define GFNT_CS_VSTEM 3
#define GFNT_CS_VMOVETO 4
#define GFNT_CS_RLINETO 5
#define GFNT_CS_HLINETO 6
#define GFNT_CS_VLINETO 7
#define GFNT_CS_RRCURVETO 8
#define GFNT_CS_CLOSEPATH 9
#define GFNT_CS_CALLSUBR 10
#define GFNT_CS_VSINDEX 15
#define GFNT_CS_BLEND 16
#define GFNT_CS_RETURN 11
#define GFNT_CS_ESCAPE 12
#define GFNT_CS_HSBW 13
#define GFNT_CS_ENDCHAR 14
#define GFNT_CS_HSTEMHM 18
#define GFNT_CS_HINTMASK 19
#define GFNT_CS_CNTRMASK 20
#define GFNT_CS_RMOVETO 21
#define GFNT_CS_HMOVETO 22
#define GFNT_CS_VSTEMHM 23
#define GFNT_CS_RCURVELINE 24
#define GFNT_CS_RLINECURVE 25
#define GFNT_CS_VVCURVETO 26
#define GFNT_CS_HHCURVETO 27
#define GFNT_CS_SHORTINT 28
#define GFNT_CS_CALLGSUBR 29
#define GFNT_CS_VHCURVETO 30
#define GFNT_CS_HVCURVETO 31
#define GFNT_CS_FIXED 255

// Two-byte operators, after the 12 escape.
#define GFNT_CS2_DOTSECTION 0
#define GFNT_CS2_VSTEM3 1
#define GFNT_CS2_HSTEM3 2
#define GFNT_CS2_AND 3
#define GFNT_CS2_OR 4
#define GFNT_CS2_NOT 5
#define GFNT_CS2_SEAC 6
#define GFNT_CS2_SBW 7
#define GFNT_CS2_ABS 9
#define GFNT_CS2_ADD 10
#define GFNT_CS2_SUB 11
#define GFNT_CS2_DIV 12
#define GFNT_CS2_NEG 14
#define GFNT_CS2_EQ 15
#define GFNT_CS2_CALLOTHERSUBR 16
#define GFNT_CS2_POP 17
#define GFNT_CS2_DROP 18
#define GFNT_CS2_PUT 20
#define GFNT_CS2_GET 21
#define GFNT_CS2_IFELSE 22
#define GFNT_CS2_RANDOM 23
#define GFNT_CS2_MUL 24
#define GFNT_CS2_SQRT 26
#define GFNT_CS2_DUP 27
#define GFNT_CS2_EXCH 28
#define GFNT_CS2_INDEX 29
#define GFNT_CS2_ROLL 30
#define GFNT_CS2_SETCURRENTPOINT 33
#define GFNT_CS2_HFLEX 34
#define GFNT_CS2_FLEX 35
#define GFNT_CS2_HFLEX1 36
#define GFNT_CS2_FLEX1 37

/** One frame of the interpreter: a charstring's bytes and where it is in them. */
typedef struct GFNT_CsFrame {
  const uint8_t * bytes; ///< The program.
  size_t length;         ///< How long it is.
  size_t cursor;         ///< Where execution is.
} GFNT_CsFrame;

/** Everything one run of one charstring carries. */
typedef struct GFNT_CsState {
  GFNT_CharstringType type;          ///< Which language.
  const GFNT_CharstringContext * ctx;///< The container's answers.
  GFNT_Limits limits;                ///< The caps, defaulted if the caller gave none.
  GFNT_Outline * outline;            ///< Where contours go.
  GFNT_Error * error;                ///< Where a refusal is recorded.

  int64_t stack[GFNT_CS_STACK_CFF2]; ///< Operands, 16.16.
  size_t count;                      ///< How many are on it.
  size_t stack_limit;                ///< How many it may hold: 48, or 513 for CFF2.
  bool cff2;                         ///< Whether this is CFF2's Type 2.
  uint32_t vsindex;                  ///< CFF2: the ItemVariationData blends use.
  bool scalars_valid;                ///< Whether `scalars` is for `vsindex`.
  size_t scalar_count;               ///< How many regions it names.
  int64_t scalars[GFNT_CS_MAX_REGIONS]; ///< Their scalars, 24 fractional bits.
  int64_t transient[GFNT_CS_TRANSIENT]; ///< `put` and `get`.

  int64_t x;                         ///< Current point, 16.16.
  int64_t y;                         ///< Current point, 16.16.
  bool contour_open;                 ///< Whether a contour has been begun.

  size_t stems;                      ///< Stem hints declared so far.
  bool width_done;                   ///< Whether the leading width has been decided.
  GFNT_F16Dot16 width;               ///< The advance.
  bool width_stated;                 ///< Whether the charstring stated it.
  GFNT_F16Dot16 side_bearing;        ///< Type 1's `hsbw`/`sbw` left side bearing.
  bool hsbw_seen;                    ///< Type 1: whether `hsbw` or `sbw` came first.

  size_t ops;                        ///< Operators executed.
  size_t depth;                      ///< Subroutine nesting.
  bool ended;                        ///< Whether `endchar` stopped the run.
  bool in_seac;                      ///< Whether this run is already an accent's.
  bool seac;                         ///< Whether it assembled one.

  // Type 1's PostScript side, which only `callothersubr` and `pop` touch.
  int64_t ps_stack[GFNT_CS_PS_STACK]; ///< What `pop` will read back.
  size_t ps_count;                    ///< How much is on it.
  bool flex_active;                   ///< Whether othersubr 1 opened a flex.
  int64_t flex_x[GFNT_CS_FLEX_POINTS];///< The collected points, absolute, 16.16.
  int64_t flex_y[GFNT_CS_FLEX_POINTS];
  size_t flex_count;                  ///< How many have arrived.
  /**
   * Where the collecting `rmoveto`s have reached.
   *
   * Separate from the current point on purpose: a flex's points are collected
   * without the path moving, so that the first curve's first delta is a
   * difference from where the path actually is. Advancing the current point
   * instead would draw the first control point from the reference point.
   */
  int64_t flex_at_x;
  int64_t flex_at_y;
} GFNT_CsState;

/** The subroutine bias Type 2 applies to a subroutine number. */
static int32_t gfnt_cs_bias(size_t count) {
  if (count < 1240) {
    return 107;
  }
  if (count < 33900) {
    return 1131;
  }
  return 32768;
}

/** Record a refusal, with the operator that caused it named by the caller. */
static GFNT_Result gfnt_cs_fail(GFNT_CsState * state, GFNT_Result result,
    size_t offset, const char * message) {
  return gfnt_error_set(state->error, result, 0, offset, GFNT_GLYPH_NONE,
      message);
}

/** Push one operand, or refuse a stack that a valid charstring cannot fill. */
static GFNT_Result gfnt_cs_push(GFNT_CsState * state, int64_t value) {
  if (state->count >= state->stack_limit) {
    return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, 0,
        "more operands than a charstring's stack holds");
  }
  state->stack[state->count++] = gfnt_clamp64(value);
  return GFNT_OK;
}

/** A 16.16 operand as a 26.6 coordinate, rounded the library's one way. */
static GFNT_F26Dot6 gfnt_cs_to_26dot6(int64_t value) {
  return gfnt_saturate32(gfnt_round_shift(gfnt_clamp64(value), 10));
}

/** Add the current point to the open contour. */
static GFNT_Result gfnt_cs_emit(GFNT_CsState * state, GFNT_PointTag tag) {
  GFNT_Point point;

  point.x = gfnt_cs_to_26dot6(state->x);
  point.y = gfnt_cs_to_26dot6(state->y);
  return gfnt_outline_add_point(state->outline, point, tag, state->error);
}

/**
 * Begin a contour at the current point.
 *
 * A charstring's `moveto` both moves and opens, and a program may move twice in
 * a row; the second one opens an empty contour rather than replacing the first,
 * because that is what the reference pen does with it and a differential
 * compares paths.
 */
static GFNT_Result gfnt_cs_move(GFNT_CsState * state) {
  GFNT_Result result = gfnt_outline_begin_contour(state->outline, state->error);

  if (result != GFNT_OK) {
    return result;
  }
  state->contour_open = true;
  return gfnt_cs_emit(state, GFNT_POINT_ON);
}

/**
 * A straight segment by @p dx and @p dy from the current point.
 *
 * The delta is applied **here** rather than by the caller, so that a charstring
 * which draws before it moves opens its contour at the point the segment leaves
 * from. Applying it first and opening afterwards put the contour's first point
 * where the segment *ends* - a one-segment contour of two identical points and a
 * glyph a side bearing to the right - which is a thing Type 1 fonts in the wild
 * do and Type 2's own rules make unreachable.
 */
static GFNT_Result gfnt_cs_line_by(GFNT_CsState * state, int64_t dx,
    int64_t dy) {
  if (!state->contour_open) {
    GFNT_Result result = gfnt_cs_move(state);
    if (result != GFNT_OK) {
      return result;
    }
  }
  state->x = gfnt_add_clamp64(state->x, dx);
  state->y = gfnt_add_clamp64(state->y, dy);
  return gfnt_cs_emit(state, GFNT_POINT_ON);
}

/**
 * A cubic from the current point, given the three deltas in 16.16.
 *
 * The current point is advanced through the two control points so that each is
 * emitted from the same accumulation the reference adds up, rather than from a
 * sum of already-rounded coordinates.
 */
static GFNT_Result gfnt_cs_curve(GFNT_CsState * state, int64_t dx1, int64_t dy1,
    int64_t dx2, int64_t dy2, int64_t dx3, int64_t dy3) {
  GFNT_Result result;

  if (!state->contour_open) {
    result = gfnt_cs_move(state);
    if (result != GFNT_OK) {
      return result;
    }
  }
  state->x = gfnt_add_clamp64(state->x, dx1);
  state->y = gfnt_add_clamp64(state->y, dy1);
  result = gfnt_cs_emit(state, GFNT_POINT_CUBIC);
  if (result != GFNT_OK) {
    return result;
  }
  state->x = gfnt_add_clamp64(state->x, dx2);
  state->y = gfnt_add_clamp64(state->y, dy2);
  result = gfnt_cs_emit(state, GFNT_POINT_CUBIC);
  if (result != GFNT_OK) {
    return result;
  }
  state->x = gfnt_add_clamp64(state->x, dx3);
  state->y = gfnt_add_clamp64(state->y, dy3);
  return gfnt_cs_emit(state, GFNT_POINT_ON);
}

/**
 * Take the optional leading width, if this stack-clearing operator carries one.
 *
 * @param state The run.
 * @param nominal How many operands the operator itself takes. A stem operator
 *   passes 0 and is recognised by an odd count instead.
 * @param even Whether the operator's own operand count is any even number.
 */
static void gfnt_cs_width(GFNT_CsState * state, size_t nominal, bool even) {
  bool present;

  if (state->width_done) {
    return;
  }
  state->width_done = true;
  present = even ? (state->count % 2) == 1 : state->count > nominal;
  if (!present || state->count == 0) {
    state->width = state->ctx->default_width;
    return;
  }
  state->width = gfnt_saturate32(
      gfnt_add_clamp64(state->ctx->nominal_width, state->stack[0]));
  state->width_stated = true;
  // The width is the bottom-most operand, so everything above it shifts down.
  memmove(state->stack, state->stack + 1,
      (state->count - 1) * sizeof state->stack[0]);
  state->count -= 1;
}

/** Whether @p op is the `endchar` form that names two glyphs to combine. */
static bool gfnt_cs_is_seac_endchar(const GFNT_CsState * state) {
  return state->count == 4;
}

/**
 * Assemble an accented character out of two other glyphs.
 *
 * Shared by `seac` (Type 1, and Type 2's deprecated spelling) and by `endchar`
 * with four operands, because they are one construction: the base glyph is
 * drawn where it stands and the accent is translated. The two glyphs are named
 * by **Standard Encoding code**, which is why the container has to resolve them
 * (design.md section 7.4).
 */
static GFNT_Result gfnt_cs_seac(GFNT_CsState * state, int64_t adx, int64_t ady,
    int64_t bchar, int64_t achar) {
  const uint8_t * bytes = NULL;
  size_t length = 0;
  GFNT_Outline * accent = NULL;
  GFNT_CharstringMetrics ignored;
  GFNT_Result result;

  if (state->in_seac) {
    return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, 0,
        "an accented character whose own base or accent is another accented "
        "character, which would not terminate");
  }
  if (!state->ctx->standard_code) {
    return gfnt_cs_fail(state, GFNT_ERR_UNSUPPORTED, 0,
        "this charstring builds an accented character, and the container "
        "supplied no way to find a glyph by Standard Encoding code");
  }
  if (bchar < 0 || bchar > 255 * GFNT_FIXED_ONE || achar < 0
      || achar > 255 * GFNT_FIXED_ONE) {
    return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, 0,
        "an accented character naming a code outside 0-255");
  }

  // The base glyph, drawn into the outline that is already open.
  result = state->ctx->standard_code(state->ctx->standard_user,
      (uint8_t)(bchar >> 16), &bytes, &length);
  if (result != GFNT_OK) {
    return gfnt_cs_fail(state, result, 0,
        "an accented character whose base glyph this font does not have");
  }
  result = gfnt_charstring_run_nested(state->type, bytes, length, state->ctx,
      state->outline, &ignored, true, state->error);
  if (result != GFNT_OK) {
    return result;
  }

  // The accent, into its own outline so that it can be translated on the way in.
  result = state->ctx->standard_code(state->ctx->standard_user,
      (uint8_t)(achar >> 16), &bytes, &length);
  if (result != GFNT_OK) {
    return gfnt_cs_fail(state, result, 0,
        "an accented character whose accent this font does not have");
  }
  result = gfnt_outline_create(state->outline->allocator, &accent,
      state->error);
  if (result != GFNT_OK) {
    return result;
  }
  gfnt_outline_set_limits(accent, &state->limits);
  result = gfnt_charstring_run_nested(state->type, bytes, length, state->ctx,
      accent, &ignored, true, state->error);
  if (result == GFNT_OK) {
    // Type 1 places the accent by the difference of the two side bearings, and
    // Type 2 by adx alone; the caller has already folded that in.
    result = gfnt_outline_append_transformed(state->outline, accent,
        GFNT_F16DOT16_ONE, 0, 0, GFNT_F16DOT16_ONE,
        gfnt_cs_to_26dot6(adx), gfnt_cs_to_26dot6(ady), state->error);
  }
  gfnt_outline_destroy(accent);
  if (result == GFNT_OK) {
    state->seac = true;
    state->ended = true;
  }
  return result;
}

/** `hlineto` and `vlineto`: one operand per segment, alternating direction. */
static GFNT_Result gfnt_cs_alternating_lines(GFNT_CsState * state,
    bool horizontal) {
  size_t index;
  GFNT_Result result;

  for (index = 0; index < state->count; index++) {
    result = horizontal
        ? gfnt_cs_line_by(state, state->stack[index], 0)
        : gfnt_cs_line_by(state, 0, state->stack[index]);
    if (result != GFNT_OK) {
      return result;
    }
    horizontal = !horizontal;
  }
  return GFNT_OK;
}

/**
 * `hvcurveto` and `vhcurveto`: groups of four, alternating which axis the
 * curve leaves and enters on, with an optional fifth operand on the last group.
 *
 * The trailing operand is the one thing a reader is likely to get wrong: it
 * belongs to the *last* group and gives the coordinate the curve would
 * otherwise inherit, so it has to be recognised by "exactly five operands
 * remain" rather than by the total count's parity.
 */
static GFNT_Result gfnt_cs_alternating_curves(GFNT_CsState * state,
    bool horizontal) {
  size_t at = 0;
  GFNT_Result result;

  while (state->count - at >= 4) {
    bool last = (state->count - at) == 5;
    int64_t a = state->stack[at];
    int64_t b = state->stack[at + 1];
    int64_t c = state->stack[at + 2];
    int64_t d = state->stack[at + 3];
    int64_t extra = last ? state->stack[at + 4] : 0;

    if (horizontal) {
      result = gfnt_cs_curve(state, a, 0, b, c, extra, d);
    }
    else {
      result = gfnt_cs_curve(state, 0, a, b, c, d, extra);
    }
    if (result != GFNT_OK) {
      return result;
    }
    horizontal = !horizontal;
    at += last ? 5 : 4;
  }
  return GFNT_OK;
}

/** Integer square root of a 16.16 value, in 16.16. */
static int64_t gfnt_cs_sqrt_fixed(int64_t value) {
  int64_t target;
  int64_t guess;
  int step;

  if (value <= 0) {
    return 0;
  }
  // sqrt(v / 65536) * 65536 == sqrt(v * 65536), computed on integers.
  target = gfnt_cs_scalable(value) * GFNT_FIXED_ONE;
  guess = 1;
  for (step = 0; step < 64; step++) {
    int64_t next;

    if (guess <= 0) {
      return 0;
    }
    next = (guess + target / guess) / 2;
    if (next == guess) {
      break;
    }
    guess = next;
  }
  while (guess > 0 && guess * guess > target) {
    guess -= 1;
  }
  return guess;
}

/** The arithmetic and storage operators, which are two-byte and rare. */
static GFNT_Result gfnt_cs_arithmetic(GFNT_CsState * state, uint8_t op,
    size_t offset, bool * out_handled) {
  int64_t a;
  int64_t b;
  size_t index;

  *out_handled = true;
  switch (op) {
    case GFNT_CS2_ABS:
      if (state->count < 1) { break; }
      a = state->stack[state->count - 1];
      state->stack[state->count - 1] = a < 0 ? -a : a;
      return GFNT_OK;
    case GFNT_CS2_ADD:
      if (state->count < 2) { break; }
      a = state->stack[state->count - 2];
      b = state->stack[state->count - 1];
      state->count -= 1;
      state->stack[state->count - 1] = gfnt_add_clamp64(a, b);
      return GFNT_OK;
    case GFNT_CS2_SUB:
      if (state->count < 2) { break; }
      a = state->stack[state->count - 2];
      b = state->stack[state->count - 1];
      state->count -= 1;
      state->stack[state->count - 1] = gfnt_add_clamp64(a, -b);
      return GFNT_OK;
    case GFNT_CS2_DIV:
      if (state->count < 2) { break; }
      a = state->stack[state->count - 2];
      b = state->stack[state->count - 1];
      state->count -= 1;
      if (b == 0) {
        return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
            "a charstring divided by zero");
      }
      state->stack[state->count - 1] = gfnt_clamp64(gfnt_round_div(
          gfnt_cs_scalable(a) * GFNT_FIXED_ONE, b));
      return GFNT_OK;
    case GFNT_CS2_NEG:
      if (state->count < 1) { break; }
      state->stack[state->count - 1] = -state->stack[state->count - 1];
      return GFNT_OK;
    case GFNT_CS2_MUL:
      if (state->count < 2) { break; }
      // Both factors are saturated into int32 first: their product then fits
      // int64 with room for the rounding term, which a pair of unclamped
      // 16.16 values does not.
      a = gfnt_saturate32(state->stack[state->count - 2]);
      b = gfnt_saturate32(state->stack[state->count - 1]);
      state->count -= 1;
      state->stack[state->count - 1] =
          gfnt_clamp64(gfnt_round_shift(a * b, 16));
      return GFNT_OK;
    case GFNT_CS2_SQRT:
      if (state->count < 1) { break; }
      state->stack[state->count - 1] =
          gfnt_cs_sqrt_fixed(state->stack[state->count - 1]);
      return GFNT_OK;
    case GFNT_CS2_DROP:
      if (state->count < 1) { break; }
      state->count -= 1;
      return GFNT_OK;
    case GFNT_CS2_DUP:
      if (state->count < 1) { break; }
      return gfnt_cs_push(state, state->stack[state->count - 1]);
    case GFNT_CS2_EXCH:
      if (state->count < 2) { break; }
      a = state->stack[state->count - 2];
      state->stack[state->count - 2] = state->stack[state->count - 1];
      state->stack[state->count - 1] = a;
      return GFNT_OK;
    case GFNT_CS2_AND:
      if (state->count < 2) { break; }
      a = state->stack[state->count - 2];
      b = state->stack[state->count - 1];
      state->count -= 1;
      state->stack[state->count - 1] =
          (a != 0 && b != 0) ? GFNT_F16DOT16_ONE : 0;
      return GFNT_OK;
    case GFNT_CS2_OR:
      if (state->count < 2) { break; }
      a = state->stack[state->count - 2];
      b = state->stack[state->count - 1];
      state->count -= 1;
      state->stack[state->count - 1] =
          (a != 0 || b != 0) ? GFNT_F16DOT16_ONE : 0;
      return GFNT_OK;
    case GFNT_CS2_NOT:
      if (state->count < 1) { break; }
      state->stack[state->count - 1] =
          state->stack[state->count - 1] == 0 ? GFNT_F16DOT16_ONE : 0;
      return GFNT_OK;
    case GFNT_CS2_EQ:
      if (state->count < 2) { break; }
      a = state->stack[state->count - 2];
      b = state->stack[state->count - 1];
      state->count -= 1;
      state->stack[state->count - 1] = a == b ? GFNT_F16DOT16_ONE : 0;
      return GFNT_OK;
    case GFNT_CS2_IFELSE: {
      // s1 s2 v1 v2 ifelse leaves s1 when v1 <= v2, and s2 otherwise. All four
      // operands go; the comparison is read before the stack shrinks.
      int64_t chosen;

      if (state->count < 4) { break; }
      chosen = state->stack[state->count - 2] <= state->stack[state->count - 1]
          ? state->stack[state->count - 4] : state->stack[state->count - 3];
      state->count -= 3;
      state->stack[state->count - 1] = chosen;
      return GFNT_OK;
    }
    case GFNT_CS2_PUT:
      if (state->count < 2) { break; }
      a = state->stack[state->count - 2];
      b = state->stack[state->count - 1];
      state->count -= 2;
      index = (size_t)(b >> 16);
      if (b < 0 || index >= GFNT_CS_TRANSIENT) {
        return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
            "a charstring stored outside the transient array");
      }
      state->transient[index] = a;
      return GFNT_OK;
    case GFNT_CS2_GET:
      if (state->count < 1) { break; }
      b = state->stack[state->count - 1];
      index = (size_t)(b >> 16);
      if (b < 0 || index >= GFNT_CS_TRANSIENT) {
        return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
            "a charstring read outside the transient array");
      }
      state->stack[state->count - 1] = state->transient[index];
      return GFNT_OK;
    case GFNT_CS2_INDEX:
      if (state->count < 2) { break; }
      b = state->stack[state->count - 1];
      if (b < 0) {
        b = 0;
      }
      index = (size_t)(b >> 16) + 1;
      if (index >= state->count) {
        return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
            "a charstring copied an operand from below the stack");
      }
      state->stack[state->count - 1] = state->stack[state->count - 1 - index];
      return GFNT_OK;
    case GFNT_CS2_ROLL: {
      int64_t shift;
      size_t n;

      if (state->count < 2) { break; }
      shift = state->stack[state->count - 1] >> 16;
      b = state->stack[state->count - 2] >> 16;
      state->count -= 2;
      if (b < 0 || (size_t)b > state->count) {
        return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
            "a charstring rolled more operands than the stack holds");
      }
      n = (size_t)b;
      if (n > 1) {
        int64_t copy[GFNT_CS_STACK_CFF2];
        size_t base = state->count - n;
        size_t at;
        int64_t turn = shift % (int64_t)n;

        if (turn < 0) {
          turn += (int64_t)n;
        }
        memcpy(copy, state->stack + base, n * sizeof copy[0]);
        for (at = 0; at < n; at++) {
          state->stack[base + (at + (size_t)turn) % n] = copy[at];
        }
      }
      return GFNT_OK;
    }
    case GFNT_CS2_RANDOM:
      // Refused rather than answered. design.md section 1 promises that one
      // font and one size give one bitmap on every platform for ever, and an
      // operator whose value is by definition unpredictable cannot be part of
      // that. Nothing in the corpus uses it.
      return gfnt_cs_fail(state, GFNT_ERR_UNSUPPORTED, offset,
          "this charstring calls random, whose value no deterministic "
          "renderer can reproduce");
    default:
      *out_handled = false;
      return GFNT_OK;
  }
  return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
      "an arithmetic operator with fewer operands than it takes");
}

/**
 * Read one numeric operand, whose encoding differs between the two languages.
 *
 * The difference is not cosmetic: `255` introduces a **16.16 fixed-point**
 * literal in Type 2 and a plain 32-bit **integer** in Type 1, so a reader that
 * shared one arm would be wrong by a factor of 65,536 on exactly the fonts that
 * use it.
 */
static GFNT_Result gfnt_cs_number(GFNT_CsState * state, uint8_t b0,
    const uint8_t * bytes, size_t length, size_t * cursor) {
  size_t at = *cursor;
  int64_t value;

  if (b0 >= 32 && b0 <= 246) {
    value = ((int64_t)b0 - 139) * GFNT_FIXED_ONE;
  }
  else if (b0 >= 247 && b0 <= 250) {
    if (at >= length) {
      return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, at,
          "a two-byte operand that runs off the end of the charstring");
    }
    value = (((int64_t)b0 - 247) * 256 + bytes[at] + 108) * GFNT_FIXED_ONE;
    at += 1;
  }
  else if (b0 >= 251 && b0 <= 254) {
    if (at >= length) {
      return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, at,
          "a two-byte operand that runs off the end of the charstring");
    }
    value = (-((int64_t)b0 - 251) * 256 - bytes[at] - 108) * GFNT_FIXED_ONE;
    at += 1;
  }
  else if (b0 == GFNT_CS_SHORTINT) {
    if (at + 2 > length) {
      return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, at,
          "a 16-bit operand that runs off the end of the charstring");
    }
    value = (int64_t)(int16_t)(((uint16_t)bytes[at] << 8) | bytes[at + 1]);
    value *= GFNT_FIXED_ONE;
    at += 2;
  }
  else {
    uint32_t raw;

    if (at + 4 > length) {
      return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, at,
          "a 32-bit operand that runs off the end of the charstring");
    }
    raw = ((uint32_t)bytes[at] << 24) | ((uint32_t)bytes[at + 1] << 16)
        | ((uint32_t)bytes[at + 2] << 8) | bytes[at + 3];
    value = (int64_t)(int32_t)raw;
    if (state->type == GFNT_CHARSTRING_TYPE1) {
      // Type 1's 255 is an integer; Type 2's is already 16.16.
      value *= GFNT_FIXED_ONE;
    }
    at += 4;
  }
  *cursor = at;
  return gfnt_cs_push(state, value);
}

/**
 * Make the scalars of the current `vsindex` available to a `blend`.
 *
 * Fetched when first needed and again after each `vsindex`, because a charstring
 * that never blends - most of a font's glyphs at its default location - should not
 * pay to read the store.
 */
static GFNT_Result gfnt_cs_scalars(GFNT_CsState * state, size_t offset) {
  const GFNT_CharstringBlend * blend = state->ctx->blend;
  GFNT_Result result;

  if (state->scalars_valid) {
    return GFNT_OK;
  }
  if (!blend || !blend->scalars) {
    return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
        "a blend or vsindex in a font that has no variation store to blend with");
  }
  result = blend->scalars(blend->user, state->vsindex, state->scalars,
      GFNT_CS_MAX_REGIONS, &state->scalar_count);
  if (result != GFNT_OK) {
    return gfnt_cs_fail(state, result == GFNT_ERR_OOM ? result : GFNT_ERR_CORRUPT,
        offset, "a vsindex the font's variation store has no data for, or one "
        "with more regions than a blend may scale by");
  }
  state->scalars_valid = true;
  return GFNT_OK;
}

/**
 * `blend`: @p n values and then, for each, one delta per region of the current
 * `vsindex`, collapse to the @p n values with their deltas added in.
 *
 * Each delta is a 16.16 number and each scalar carries 24 fractional bits, so a
 * product has 40 and the sum of a value's are rounded **once**, half away from
 * zero, to the 16.16 the stack holds - the rule every other delta in this library
 * follows, and the reason a blend at a location is the same number however many
 * regions contribute.
 */
static GFNT_Result gfnt_cs_blend(GFNT_CsState * state, size_t offset) {
  int64_t n;
  size_t k;
  size_t base;
  GFNT_Result result;

  if (state->count < 1) {
    return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
        "a blend with no operand count");
  }
  result = gfnt_cs_scalars(state, offset);
  if (result != GFNT_OK) {
    return result;
  }
  k = state->scalar_count;
  n = state->stack[--state->count] >> 16;
  if (n < 0 || (uint64_t)n > state->count
      || (uint64_t)n * (k + 1u) > state->count) {
    return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
        "a blend whose operands are fewer than its count says: it takes each "
        "value and one delta per region for it");
  }
  base = state->count - (size_t)n * (k + 1u);
  for (size_t i = 0; i < (size_t)n; ++i) {
    int64_t sum = 0;

    for (size_t j = 0; j < k; ++j) {
      // A delta is at most 2^31 in a 16.16 operand and a scalar at most 2^24, so
      // a term is at most 2^55 and sixty-four of them cannot leave an int64.
      sum += (int64_t)gfnt_saturate32(
                 state->stack[base + (size_t)n + i * k + j])
          * state->scalars[j];
    }
    state->stack[base + i] = gfnt_add_clamp64(state->stack[base + i],
        gfnt_round_shift(sum, GFNT_CS_SCALAR_BITS));
  }
  state->count = base + (size_t)n;
  return GFNT_OK;
}

static GFNT_Result gfnt_cs_run_frame(GFNT_CsState * state,
    const uint8_t * bytes, size_t length);

/** Call a subroutine, biased for Type 2 and not for Type 1. */
static GFNT_Result gfnt_cs_call(GFNT_CsState * state,
    const GFNT_CharstringSubrs * subrs, size_t offset, const char * which) {
  const uint8_t * bytes = NULL;
  size_t sub_length = 0;
  int64_t number;
  int64_t index;
  GFNT_Result result;

  if (state->count < 1) {
    return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
        "a subroutine call with no subroutine number");
  }
  number = state->stack[--state->count] >> 16;
  index = state->type == GFNT_CHARSTRING_TYPE2
      ? number + gfnt_cs_bias(subrs->count) : number;
  if (!subrs->at || index < 0 || (size_t)index >= subrs->count) {
    return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
        "a call to a subroutine this font does not have");
  }
  if (state->depth >= state->limits.max_charstring_depth) {
    return gfnt_cs_fail(state, GFNT_ERR_LIMIT, offset,
        "charstring subroutines nested deeper than "
        "GFNT_Limits::max_charstring_depth");
  }
  result = subrs->at(subrs->user, (uint32_t)index, &bytes, &sub_length);
  if (result != GFNT_OK) {
    return gfnt_cs_fail(state, result, offset, which);
  }
  state->depth += 1;
  result = gfnt_cs_run_frame(state, bytes, sub_length);
  state->depth -= 1;
  return result;
}

/**
 * The flex family, whose four spellings differ only in what they leave out.
 *
 * All four draw two cubics, and the three short forms omit the coordinates a
 * horizontal or symmetric flex can recompute. The recomputations below are
 * written as sums of the operands rather than as differences of accumulated
 * positions: `hflex`'s second curve returns to the y the flex *started* at, and
 * expressing that as "minus the dy operands that have been applied since" is
 * exact, where reading back a rounded current point would not be.
 */
static GFNT_Result gfnt_cs_flex(GFNT_CsState * state, uint8_t op,
    size_t offset) {
  const int64_t * s = state->stack;
  GFNT_Result result;

  switch (op) {
    case GFNT_CS2_FLEX:
      if (state->count < 13) { break; }
      result = gfnt_cs_curve(state, s[0], s[1], s[2], s[3], s[4], s[5]);
      if (result != GFNT_OK) {
        return result;
      }
      // The thirteenth operand is fd, the flex depth, which tells a hinting
      // renderer whether to draw the curves or a straight line. There is no
      // hinting here (design.md section 8.5), so it is read and not used.
      return gfnt_cs_curve(state, s[6], s[7], s[8], s[9], s[10], s[11]);

    case GFNT_CS2_HFLEX:
      // dx1 dx2 dy2 dx3 dx4 dx5 dx6. Both endpoints and the last control point
      // sit on the starting y, so only one dy appears and the second curve
      // takes it back.
      if (state->count < 7) { break; }
      result = gfnt_cs_curve(state, s[0], 0, s[1], s[2], s[3], 0);
      if (result != GFNT_OK) {
        return result;
      }
      return gfnt_cs_curve(state, s[4], 0, s[5], -s[2], s[6], 0);

    case GFNT_CS2_HFLEX1:
      // dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6. The final point returns to the
      // starting y, which is the sum of every dy applied so far, negated.
      if (state->count < 9) { break; }
      result = gfnt_cs_curve(state, s[0], s[1], s[2], s[3], s[4], 0);
      if (result != GFNT_OK) {
        return result;
      }
      return gfnt_cs_curve(state, s[5], 0, s[6], s[7], s[8],
          -gfnt_add_clamp64(gfnt_add_clamp64(s[1], s[3]), s[7]));

    case GFNT_CS2_FLEX1: {
      // dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 d6. The last operand is one
      // coordinate, and which axis it is depends on which way the whole flex
      // travelled - the *larger* total wins, and the other axis returns to
      // where the flex began.
      int64_t dx = 0;
      int64_t dy = 0;
      size_t index;

      if (state->count < 11) { break; }
      for (index = 0; index < 10; index += 2) {
        dx = gfnt_add_clamp64(dx, s[index]);
        dy = gfnt_add_clamp64(dy, s[index + 1]);
      }
      result = gfnt_cs_curve(state, s[0], s[1], s[2], s[3], s[4], s[5]);
      if (result != GFNT_OK) {
        return result;
      }
      if ((dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)) {
        return gfnt_cs_curve(state, s[6], s[7], s[8], s[9], s[10], -dy);
      }
      return gfnt_cs_curve(state, s[6], s[7], s[8], s[9], -dx, s[10]);
    }

    default:
      return gfnt_cs_fail(state, GFNT_ERR_INTERNAL, offset,
          "a flex operator this function was not given");
  }
  return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
      "a flex operator with fewer operands than it takes");
}

/** Type 1's `callothersubr`, which is how it reaches flex and hint replacement. */
static GFNT_Result gfnt_cs_othersubr(GFNT_CsState * state, size_t offset) {
  int64_t which;
  int64_t argc;
  size_t index;

  if (state->count < 2) {
    return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
        "callothersubr without its subroutine number and argument count");
  }
  which = state->stack[state->count - 1] >> 16;
  argc = state->stack[state->count - 2] >> 16;
  state->count -= 2;
  if (argc < 0 || (size_t)argc > state->count) {
    return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
        "callothersubr claiming more arguments than the stack holds");
  }
  switch (which) {
    case 1:
      // Flex begins. The seven rmovetos that follow are collected rather than
      // drawn, which is what makes this one construction rather than seven
      // one-point contours.
      state->flex_active = true;
      state->flex_count = 0;
      state->count -= (size_t)argc;
      return GFNT_OK;
    case 2:
      state->count -= (size_t)argc;
      return GFNT_OK;
    case 0: {
      // Flex ends. The seven collected points are absolute; the first is the
      // reference point, which exists for a hinting renderer to measure the
      // flex's depth against and is not on the curve at all. The other six are
      // two cubics, and the current point has not moved since the flex opened -
      // which is what makes the first delta a difference from it.
      GFNT_Result result;

      if (!state->flex_active || state->flex_count < GFNT_CS_FLEX_POINTS) {
        return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, offset,
            "a flex that ended before its seven points had arrived");
      }
      state->flex_active = false;
      state->count -= (size_t)argc;
      result = gfnt_cs_curve(state,
          gfnt_add_clamp64(state->flex_x[1], -state->x),
          gfnt_add_clamp64(state->flex_y[1], -state->y),
          gfnt_add_clamp64(state->flex_x[2], -state->flex_x[1]),
          gfnt_add_clamp64(state->flex_y[2], -state->flex_y[1]),
          gfnt_add_clamp64(state->flex_x[3], -state->flex_x[2]),
          gfnt_add_clamp64(state->flex_y[3], -state->flex_y[2]));
      if (result != GFNT_OK) {
        return result;
      }
      result = gfnt_cs_curve(state,
          gfnt_add_clamp64(state->flex_x[4], -state->flex_x[3]),
          gfnt_add_clamp64(state->flex_y[4], -state->flex_y[3]),
          gfnt_add_clamp64(state->flex_x[5], -state->flex_x[4]),
          gfnt_add_clamp64(state->flex_y[5], -state->flex_y[4]),
          gfnt_add_clamp64(state->flex_x[6], -state->flex_x[5]),
          gfnt_add_clamp64(state->flex_y[6], -state->flex_y[5]));
      if (result != GFNT_OK) {
        return result;
      }
      // `pop pop setcurrentpoint` follows, and the two pops read these back:
      // the x first, so it is on top.
      state->ps_count = 0;
      state->ps_stack[state->ps_count++] = state->y;
      state->ps_stack[state->ps_count++] = state->x;
      return GFNT_OK;
    }
    case 3:
      // Hint replacement: the argument is the subroutine number holding the
      // new hints, which `pop callsubr` then calls. Hints are not read here, so
      // the call is left to happen and does nothing but run the subroutine.
      state->ps_count = 0;
      if (argc >= 1) {
        state->ps_stack[state->ps_count++] =
            state->stack[state->count - (size_t)argc];
      }
      else {
        state->ps_stack[state->ps_count++] = 3 * GFNT_FIXED_ONE;
      }
      state->count -= (size_t)argc;
      return GFNT_OK;
    default:
      // An OtherSubr this library does not implement - the multiple-master
      // family is the usual one. Its arguments become its results, which is
      // what the specification says an unknown OtherSubr's `pop`s should see,
      // and is the behaviour that lets a font using one still draw.
      state->ps_count = 0;
      for (index = 0; index < (size_t)argc && index < GFNT_CS_PS_STACK;
          index++) {
        state->ps_stack[state->ps_count++] =
            state->stack[state->count - 1 - index];
      }
      state->count -= (size_t)argc;
      return GFNT_OK;
  }
}

/**
 * Execute one charstring or subroutine.
 *
 * Recursive, once per subroutine call, bounded by
 * ::GFNT_Limits::max_charstring_depth. Operands are read from the **bottom** of
 * the stack, which is the order the specification lists them in; a valid
 * charstring leaves exactly the operands its operator takes, and where a broken
 * one leaves more, reading them in the stated order is the only reading that is
 * defensible.
 */
static GFNT_Result gfnt_cs_run_frame(GFNT_CsState * state,
    const uint8_t * bytes, size_t length) {
  const int64_t * s = state->stack;
  size_t cursor = 0;
  size_t index;
  GFNT_Result result;

  if (!bytes && length > 0) {
    return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, 0,
        "a charstring with a length and no bytes");
  }
  while (cursor < length && !state->ended) {
    uint8_t op = bytes[cursor++];

    if (op >= 32
        || (op == GFNT_CS_SHORTINT
            && state->type == GFNT_CHARSTRING_TYPE2)) {
      result = gfnt_cs_number(state, op, bytes, length, &cursor);
      if (result != GFNT_OK) {
        return result;
      }
      continue;
    }

    state->ops += 1;
    if (state->ops > state->limits.max_charstring_ops) {
      return gfnt_cs_fail(state, GFNT_ERR_LIMIT, cursor - 1,
          "more charstring operators than GFNT_Limits::max_charstring_ops, "
          "which is what stops a font whose subroutines call each other from "
          "expanding without bound");
    }

    switch (op) {
      case GFNT_CS_HSTEM:
      case GFNT_CS_VSTEM:
      case GFNT_CS_HSTEMHM:
      case GFNT_CS_VSTEMHM:
        if (state->type != GFNT_CHARSTRING_TYPE2) {
          // Type 1 has hstem and vstem and reserves the other two bytes: the
          // `hm` names a hint mask, which Type 1 does not have. Every other
          // operator Type 2 added is refused here by name, and these two were
          // reached through the case they share with the pair Type 1 does have
          // - so a Type 1 charstring could declare stems with a byte that
          // language never spells, and be drawn rather than refused.
          if (op == GFNT_CS_HSTEMHM || op == GFNT_CS_VSTEMHM) {
            return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
                "hstemhm or vstemhm, which Type 1 does not have - it has hstem "
                "and vstem, and no hint masks for the hm to refer to");
          }
        }
        else {
          // All four stem operators are stack-clearing, so all four can carry
          // the leading width. The specification's list names hstem and hstemhm
          // and not the vertical pair, which reads as an omission rather than a
          // rule: a charstring whose first operator is vstem has exactly the
          // same odd operand count to explain.
          gfnt_cs_width(state, 0, true);
          state->stems += state->count / 2;
        }
        state->count = 0;
        break;

      case GFNT_CS_HINTMASK:
      case GFNT_CS_CNTRMASK: {
        size_t mask_bytes;

        if (state->type != GFNT_CHARSTRING_TYPE2) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "hintmask, which Type 1 does not have");
        }
        gfnt_cs_width(state, 0, true);
        // Operands still on the stack here are an implicit vstem declaration,
        // which is the one place a stem count grows without a stem operator.
        state->stems += state->count / 2;
        state->count = 0;
        mask_bytes = (state->stems + 7) / 8;
        if (cursor + mask_bytes > length) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor,
              "a hintmask whose mask bytes run off the end of the charstring");
        }
        // The mask says which declared hints are active. Nothing here hints
        // (design.md section 8.5), so it is skipped - by the count its stems
        // imply, which is why the stem count has to be right even so.
        cursor += mask_bytes;
        break;
      }

      case GFNT_CS_RMOVETO:
        if (state->type == GFNT_CHARSTRING_TYPE2) {
          gfnt_cs_width(state, 2, false);
        }
        if (state->count < 2) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "rmoveto with fewer than two operands");
        }
        if (state->flex_active) {
          if (state->flex_count >= GFNT_CS_FLEX_POINTS) {
            return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
                "a flex collecting more than seven points");
          }
          state->flex_at_x = gfnt_add_clamp64(state->flex_at_x, s[0]);
          state->flex_at_y = gfnt_add_clamp64(state->flex_at_y, s[1]);
          state->flex_x[state->flex_count] = state->flex_at_x;
          state->flex_y[state->flex_count] = state->flex_at_y;
          state->flex_count += 1;
          state->count = 0;
          break;
        }
        state->x = gfnt_add_clamp64(state->x, s[0]);
        state->y = gfnt_add_clamp64(state->y, s[1]);
        state->count = 0;
        result = gfnt_cs_move(state);
        if (result != GFNT_OK) {
          return result;
        }
        break;

      case GFNT_CS_HMOVETO:
      case GFNT_CS_VMOVETO:
        if (state->type == GFNT_CHARSTRING_TYPE2) {
          gfnt_cs_width(state, 1, false);
        }
        if (state->count < 1) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "a one-axis moveto with no operand");
        }
        if (op == GFNT_CS_HMOVETO) {
          state->x = gfnt_add_clamp64(state->x, s[0]);
        }
        else {
          state->y = gfnt_add_clamp64(state->y, s[0]);
        }
        state->count = 0;
        result = gfnt_cs_move(state);
        if (result != GFNT_OK) {
          return result;
        }
        break;

      case GFNT_CS_RLINETO:
        for (index = 0; index + 1 < state->count; index += 2) {
          result = gfnt_cs_line_by(state, s[index], s[index + 1]);
          if (result != GFNT_OK) {
            return result;
          }
        }
        state->count = 0;
        break;

      case GFNT_CS_HLINETO:
      case GFNT_CS_VLINETO:
        result = gfnt_cs_alternating_lines(state, op == GFNT_CS_HLINETO);
        if (result != GFNT_OK) {
          return result;
        }
        state->count = 0;
        break;

      case GFNT_CS_RRCURVETO:
        for (index = 0; index + 5 < state->count; index += 6) {
          result = gfnt_cs_curve(state, s[index], s[index + 1], s[index + 2],
              s[index + 3], s[index + 4], s[index + 5]);
          if (result != GFNT_OK) {
            return result;
          }
        }
        state->count = 0;
        break;

      case GFNT_CS_RCURVELINE:
        if (state->type != GFNT_CHARSTRING_TYPE2) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "rcurveline, which Type 1 does not have");
        }
        if (state->count < 8) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "rcurveline with fewer operands than one curve and one line");
        }
        for (index = 0; index + 5 < state->count - 2; index += 6) {
          result = gfnt_cs_curve(state, s[index], s[index + 1], s[index + 2],
              s[index + 3], s[index + 4], s[index + 5]);
          if (result != GFNT_OK) {
            return result;
          }
        }
        result = gfnt_cs_line_by(state, s[state->count - 2],
            s[state->count - 1]);
        if (result != GFNT_OK) {
          return result;
        }
        state->count = 0;
        break;

      case GFNT_CS_RLINECURVE:
        if (state->type != GFNT_CHARSTRING_TYPE2) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "rlinecurve, which Type 1 does not have");
        }
        if (state->count < 8) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "rlinecurve with fewer operands than one line and one curve");
        }
        for (index = 0; index + 1 < state->count - 6; index += 2) {
          result = gfnt_cs_line_by(state, s[index], s[index + 1]);
          if (result != GFNT_OK) {
            return result;
          }
        }
        index = state->count - 6;
        result = gfnt_cs_curve(state, s[index], s[index + 1], s[index + 2],
            s[index + 3], s[index + 4], s[index + 5]);
        if (result != GFNT_OK) {
          return result;
        }
        state->count = 0;
        break;

      case GFNT_CS_VVCURVETO:
      case GFNT_CS_HHCURVETO: {
        int64_t lead = 0;

        if (state->type != GFNT_CHARSTRING_TYPE2) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "vvcurveto or hhcurveto, which Type 1 does not have");
        }
        index = 0;
        // An odd operand count means the first curve's first control point is
        // displaced on the other axis as well - once, for the first curve only.
        if ((state->count % 4) == 1) {
          lead = s[0];
          index = 1;
        }
        for (; index + 3 < state->count; index += 4) {
          if (op == GFNT_CS_VVCURVETO) {
            result = gfnt_cs_curve(state, lead, s[index], s[index + 1],
                s[index + 2], 0, s[index + 3]);
          }
          else {
            result = gfnt_cs_curve(state, s[index], lead, s[index + 1],
                s[index + 2], s[index + 3], 0);
          }
          if (result != GFNT_OK) {
            return result;
          }
          lead = 0;
        }
        state->count = 0;
        break;
      }

      case GFNT_CS_VHCURVETO:
      case GFNT_CS_HVCURVETO:
        result = gfnt_cs_alternating_curves(state, op == GFNT_CS_HVCURVETO);
        if (result != GFNT_OK) {
          return result;
        }
        state->count = 0;
        break;

      case GFNT_CS_CALLSUBR:
        result = gfnt_cs_call(state, &state->ctx->local, cursor - 1,
            "a local subroutine the container refused to produce");
        if (result != GFNT_OK) {
          return result;
        }
        break;

      case GFNT_CS_CALLGSUBR:
        if (state->type != GFNT_CHARSTRING_TYPE2) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "callgsubr, which Type 1 does not have");
        }
        result = gfnt_cs_call(state, &state->ctx->global, cursor - 1,
            "a global subroutine the container refused to produce");
        if (result != GFNT_OK) {
          return result;
        }
        break;

      case GFNT_CS_RETURN:
        return GFNT_OK;

      case GFNT_CS_VSINDEX:
        if (!state->cff2) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "vsindex, which only a CFF2 charstring has");
        }
        if (state->count < 1) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "a vsindex with no operand");
        }
        // It takes one operand and leaves the rest of the stack alone: it is not a
        // stack-clearing operator, and an operand pushed before it is still there.
        {
          int64_t chosen = state->stack[--state->count] >> 16;

          if (chosen < 0 || chosen > 0xFFFF) {
            return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
                "a vsindex that is not a possible ItemVariationData index");
          }
          state->vsindex = (uint32_t)chosen;
          state->scalars_valid = false;
        }
        break;

      case GFNT_CS_BLEND:
        if (!state->cff2) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "blend, which only a CFF2 charstring has");
        }
        result = gfnt_cs_blend(state, cursor - 1);
        if (result != GFNT_OK) {
          return result;
        }
        break;

      case GFNT_CS_ENDCHAR:
        if (state->cff2) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "endchar, which CFF2 removed: a charstring ends where its bytes do");
        }
        if (state->type == GFNT_CHARSTRING_TYPE2) {
          if (!state->width_done) {
            state->width_done = true;
            // endchar takes nothing, or the four operands of an accented
            // character. One more than either means the first is the width, so
            // this is the one stack-clearing operator whose width cannot be
            // recognised by "more than the nominal count".
            if (state->count == 1 || state->count == 5) {
              state->width = gfnt_saturate32(
                  gfnt_add_clamp64(state->ctx->nominal_width, s[0]));
              state->width_stated = true;
              memmove(state->stack, state->stack + 1,
                  (state->count - 1) * sizeof state->stack[0]);
              state->count -= 1;
            }
            else {
              state->width = state->ctx->default_width;
            }
          }
          if (gfnt_cs_is_seac_endchar(state)) {
            result = gfnt_cs_seac(state, s[0], s[1], s[2], s[3]);
            if (result != GFNT_OK) {
              return result;
            }
          }
          else if (state->count != 0) {
            return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
                "endchar with operands that are neither a width nor the four "
                "of an accented character");
          }
        }
        state->count = 0;
        state->ended = true;
        break;

      case GFNT_CS_CLOSEPATH:
        if (state->type != GFNT_CHARSTRING_TYPE1) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "closepath, which Type 2 does not have - its contours close "
              "themselves");
        }
        // Contours here are regions and close themselves, so this ends the
        // current one rather than adding a segment.
        state->contour_open = false;
        state->count = 0;
        break;

      case GFNT_CS_HSBW:
        if (state->type != GFNT_CHARSTRING_TYPE1) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "hsbw, which Type 2 replaced with nominalWidthX and a delta");
        }
        if (state->count < 2) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
              "hsbw without its side bearing and width");
        }
        // Type 1 starts drawing at the left side bearing, not at the origin:
        // every coordinate in the charstring is relative to it.
        state->side_bearing = gfnt_saturate32(s[0]);
        state->width = gfnt_saturate32(s[1]);
        state->width_stated = true;
        state->width_done = true;
        state->hsbw_seen = true;
        state->x = s[0];
        state->y = 0;
        state->count = 0;
        break;

      case GFNT_CS_ESCAPE: {
        uint8_t op2;
        bool handled = false;

        if (cursor >= length) {
          return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor,
              "a two-byte operator whose second byte is not there");
        }
        op2 = bytes[cursor++];
        switch (op2) {
          case GFNT_CS2_FLEX:
          case GFNT_CS2_HFLEX:
          case GFNT_CS2_HFLEX1:
          case GFNT_CS2_FLEX1:
            if (state->type != GFNT_CHARSTRING_TYPE2) {
              return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 2,
                  "a Type 2 flex operator in a Type 1 charstring, which "
                  "reaches flex through callothersubr instead");
            }
            result = gfnt_cs_flex(state, op2, cursor - 2);
            if (result != GFNT_OK) {
              return result;
            }
            state->count = 0;
            break;

          case GFNT_CS2_SEAC:
            if (state->type != GFNT_CHARSTRING_TYPE1) {
              return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 2,
                  "the Type 1 seac operator; Type 2 spells the same "
                  "construction as endchar with four operands");
            }
            if (state->count < 5) {
              return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 2,
                  "seac with fewer than five operands");
            }
            // asb adx ady bchar achar. The accent moves by adx *corrected* for
            // the difference between this charstring's side bearing and the
            // accent's own stated one, which is what asb is for; dropping the
            // correction puts every accent a side bearing to one side.
            result = gfnt_cs_seac(state,
                gfnt_add_clamp64(gfnt_add_clamp64(state->side_bearing, -s[0]),
                    s[1]),
                s[2], s[3], s[4]);
            if (result != GFNT_OK) {
              return result;
            }
            state->count = 0;
            break;

          case GFNT_CS2_SBW:
            if (state->type != GFNT_CHARSTRING_TYPE1) {
              return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 2,
                  "sbw, which is Type 1's two-axis hsbw");
            }
            if (state->count < 4) {
              return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 2,
                  "sbw without its two side bearings and two widths");
            }
            state->side_bearing = gfnt_saturate32(s[0]);
            state->width = gfnt_saturate32(s[2]);
            state->width_stated = true;
            state->width_done = true;
            state->hsbw_seen = true;
            state->x = s[0];
            state->y = s[1];
            state->count = 0;
            break;

          case GFNT_CS2_CALLOTHERSUBR:
            if (state->type != GFNT_CHARSTRING_TYPE1) {
              return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 2,
                  "callothersubr, which Type 2 does not have");
            }
            if (!state->flex_active) {
              state->flex_at_x = state->x;
              state->flex_at_y = state->y;
            }
            result = gfnt_cs_othersubr(state, cursor - 2);
            if (result != GFNT_OK) {
              return result;
            }
            break;

          case GFNT_CS2_POP:
            if (state->type != GFNT_CHARSTRING_TYPE1) {
              return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 2,
                  "pop, which Type 2 does not have");
            }
            // An OtherSubr that produced nothing leaves nothing to read back.
            // Zero is what the reference interpreters push, and refusing here
            // would refuse fonts whose unknown OtherSubrs this library ran
            // without knowing how many results they meant to leave.
            result = gfnt_cs_push(state,
                state->ps_count > 0 ? state->ps_stack[--state->ps_count] : 0);
            if (result != GFNT_OK) {
              return result;
            }
            break;

          case GFNT_CS2_DOTSECTION:
          case GFNT_CS2_VSTEM3:
          case GFNT_CS2_HSTEM3:
            if (state->type != GFNT_CHARSTRING_TYPE1) {
              return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 2,
                  "a Type 1 hint operator in a Type 2 charstring");
            }
            state->count = 0;
            break;

          case GFNT_CS2_SETCURRENTPOINT:
            if (state->type != GFNT_CHARSTRING_TYPE1) {
              return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 2,
                  "setcurrentpoint, which Type 2 does not have");
            }
            if (state->count < 2) {
              return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 2,
                  "setcurrentpoint without both coordinates");
            }
            // Absolute, and the only operator here that is.
            state->x = s[0];
            state->y = s[1];
            state->count = 0;
            break;

          default:
            if (state->type == GFNT_CHARSTRING_TYPE1
                && op2 != GFNT_CS2_DIV) {
              return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 2,
                  "a two-byte operator Type 1 does not have");
            }
            result = gfnt_cs_arithmetic(state, op2, cursor - 2, &handled);
            if (result != GFNT_OK) {
              return result;
            }
            if (!handled) {
              return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 2,
                  "a two-byte charstring operator this format does not define");
            }
            break;
        }
        break;
      }

      default:
        return gfnt_cs_fail(state, GFNT_ERR_CORRUPT, cursor - 1,
            "a charstring operator this format does not define");
    }
  }
  return GFNT_OK;
}

GFNT_Result gfnt_charstring_run_nested(GFNT_CharstringType type,
    const uint8_t * bytes, size_t length,
    const GFNT_CharstringContext * context, GFNT_Outline * outline,
    GFNT_CharstringMetrics * out_metrics, bool in_seac, GFNT_Error * error) {
  GFNT_CsState state;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!context || !outline || (!bytes && length > 0)
      || type >= GFNT_CHARSTRING_TYPE_COUNT) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no charstring, no outline, no context, or a language that is neither "
        "Type 1 nor Type 2");
  }
  memset(&state, 0, sizeof state);
  if (type == GFNT_CHARSTRING_CFF2) {
    // CFF2 is Type 2 with differences, so everything that asks "is this Type 2"
    // keeps getting yes; what differs is read from `cff2`.
    state.cff2 = true;
    type = GFNT_CHARSTRING_TYPE2;
  }
  state.stack_limit = state.cff2 ? GFNT_CS_STACK_CFF2 : GFNT_CS_STACK;
  if (context->blend) {
    state.vsindex = context->blend->default_vsindex;
  }
  state.type = type;
  state.ctx = context;
  state.outline = outline;
  state.error = error;
  state.in_seac = in_seac;
  if (context->limits) {
    state.limits = *context->limits;
  }
  else {
    gfnt_limits_default(&state.limits);
  }
  state.width = context->default_width;
  if (state.cff2) {
    // CFF2 states no width: the advance is `hmtx`'s, so there is none to find and
    // no operator that may carry one.
    state.width_done = true;
  }
  if (context->limits) {
    // One source of truth for this run: the caps a caller states in the context
    // are the caps the outline enforces on the points this run adds. Leaving the
    // outline's own were two places for one fact, and a caller that lowered
    // max_outline_points in the context would have watched it be ignored.
    gfnt_outline_set_limits(outline, context->limits);
  }

  result = gfnt_cs_run_frame(&state, bytes, length);
  if (result != GFNT_OK) {
    return result;
  }
  if (type == GFNT_CHARSTRING_TYPE1 && !state.hsbw_seen) {
    // Type 1 keeps the advance in the charstring and nowhere else, so a program
    // that never states one has not drawn a glyph this library can report on.
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, 0, 0, GFNT_GLYPH_NONE,
        "a Type 1 charstring that never ran hsbw or sbw, so it states neither "
        "an advance nor a side bearing and there is nowhere else to find them");
  }
  if (out_metrics) {
    out_metrics->width = state.width;
    out_metrics->width_stated = state.width_stated;
    out_metrics->side_bearing = state.side_bearing;
    out_metrics->stems = state.stems;
    out_metrics->operators = state.ops;
    out_metrics->seac = state.seac;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_charstring_run(GFNT_CharstringType type,
    const uint8_t * bytes, size_t length,
    const GFNT_CharstringContext * context, GFNT_Outline * outline,
    GFNT_CharstringMetrics * out_metrics, GFNT_Error * error) {
  return gfnt_charstring_run_nested(type, bytes, length, context, outline,
      out_metrics, false, error);
}

const char * gfnt_charstring_type_string(GFNT_CharstringType type) {
  switch (type) {
    case GFNT_CHARSTRING_TYPE2: return "Type 2";
    case GFNT_CHARSTRING_TYPE1: return "Type 1";
    case GFNT_CHARSTRING_CFF2: return "CFF2";
    case GFNT_CHARSTRING_TYPE_COUNT: break;
  }
  return "unknown";
}

/** The operator a byte spells in this language, and not in the other one.
 *
 * The two languages divide one byte range differently, and a single table that
 * named every byte in both would lie in both directions: 13 is `hsbw` in Type 1
 * and reserved in Type 2, and 19 is `hintmask` in Type 2 and reserved in Type 1.
 * The interpreter refuses each of those by name; a dump that named them anyway
 * would report that a font contains an operator the language it is written in
 * does not have. For `hintmask` it is worse than a wrong name, because the
 * caller divides the bytes after it as a mask - so one reserved byte in a Type 1
 * charstring would make every operator printed after it fiction.
 */
static const char * gfnt_cs_op_name(GFNT_CharstringType type, bool cff2,
    uint8_t op, uint8_t op2) {
  const bool t1 = type == GFNT_CHARSTRING_TYPE1;

  if (op != GFNT_CS_ESCAPE) {
    switch (op) {
      // Spelled the same in both languages.
      case GFNT_CS_HSTEM: return "hstem";
      case GFNT_CS_VSTEM: return "vstem";
      case GFNT_CS_VMOVETO: return "vmoveto";
      case GFNT_CS_RLINETO: return "rlineto";
      case GFNT_CS_HLINETO: return "hlineto";
      case GFNT_CS_VLINETO: return "vlineto";
      case GFNT_CS_RRCURVETO: return "rrcurveto";
      case GFNT_CS_CALLSUBR: return "callsubr";
      case GFNT_CS_RETURN: return "return";
      case GFNT_CS_ENDCHAR: return "endchar";
      case GFNT_CS_RMOVETO: return "rmoveto";
      case GFNT_CS_HMOVETO: return "hmoveto";
      case GFNT_CS_VHCURVETO: return "vhcurveto";
      case GFNT_CS_HVCURVETO: return "hvcurveto";
      // Type 1's, which Type 2 reserves.
      case GFNT_CS_CLOSEPATH: return t1 ? "closepath" : "reserved";
      case GFNT_CS_HSBW: return t1 ? "hsbw" : "reserved";
      // Type 2's, which Type 1 reserves.
      case GFNT_CS_HSTEMHM: return t1 ? "reserved" : "hstemhm";
      case GFNT_CS_HINTMASK: return t1 ? "reserved" : "hintmask";
      case GFNT_CS_CNTRMASK: return t1 ? "reserved" : "cntrmask";
      case GFNT_CS_VSTEMHM: return t1 ? "reserved" : "vstemhm";
      case GFNT_CS_RCURVELINE: return t1 ? "reserved" : "rcurveline";
      case GFNT_CS_RLINECURVE: return t1 ? "reserved" : "rlinecurve";
      case GFNT_CS_VVCURVETO: return t1 ? "reserved" : "vvcurveto";
      case GFNT_CS_HHCURVETO: return t1 ? "reserved" : "hhcurveto";
      case GFNT_CS_CALLGSUBR: return t1 ? "reserved" : "callgsubr";
      // CFF2's two, which Type 2 and Type 1 both reserve.
      case GFNT_CS_VSINDEX: return cff2 ? "vsindex" : "reserved";
      case GFNT_CS_BLEND: return cff2 ? "blend" : "reserved";
      default: return "reserved";
    }
  }
  switch (op2) {
    // The one two-byte operator both languages have.
    case GFNT_CS2_DIV: return "div";
    // Type 1's, which Type 2 reserves.
    case GFNT_CS2_DOTSECTION: return t1 ? "dotsection" : "reserved";
    case GFNT_CS2_VSTEM3: return t1 ? "vstem3" : "reserved";
    case GFNT_CS2_HSTEM3: return t1 ? "hstem3" : "reserved";
    case GFNT_CS2_SEAC: return t1 ? "seac" : "reserved";
    case GFNT_CS2_SBW: return t1 ? "sbw" : "reserved";
    case GFNT_CS2_CALLOTHERSUBR: return t1 ? "callothersubr" : "reserved";
    case GFNT_CS2_POP: return t1 ? "pop" : "reserved";
    case GFNT_CS2_SETCURRENTPOINT: return t1 ? "setcurrentpoint" : "reserved";
    // Type 2's, which Type 1 reserves. The arithmetic is all Type 2's: Type 1
    // has only div, which is above.
    case GFNT_CS2_AND: return t1 ? "reserved" : "and";
    case GFNT_CS2_OR: return t1 ? "reserved" : "or";
    case GFNT_CS2_NOT: return t1 ? "reserved" : "not";
    case GFNT_CS2_ABS: return t1 ? "reserved" : "abs";
    case GFNT_CS2_ADD: return t1 ? "reserved" : "add";
    case GFNT_CS2_SUB: return t1 ? "reserved" : "sub";
    case GFNT_CS2_NEG: return t1 ? "reserved" : "neg";
    case GFNT_CS2_EQ: return t1 ? "reserved" : "eq";
    case GFNT_CS2_DROP: return t1 ? "reserved" : "drop";
    case GFNT_CS2_PUT: return t1 ? "reserved" : "put";
    case GFNT_CS2_GET: return t1 ? "reserved" : "get";
    case GFNT_CS2_IFELSE: return t1 ? "reserved" : "ifelse";
    case GFNT_CS2_RANDOM: return t1 ? "reserved" : "random";
    case GFNT_CS2_MUL: return t1 ? "reserved" : "mul";
    case GFNT_CS2_SQRT: return t1 ? "reserved" : "sqrt";
    case GFNT_CS2_DUP: return t1 ? "reserved" : "dup";
    case GFNT_CS2_EXCH: return t1 ? "reserved" : "exch";
    case GFNT_CS2_INDEX: return t1 ? "reserved" : "index";
    case GFNT_CS2_ROLL: return t1 ? "reserved" : "roll";
    case GFNT_CS2_HFLEX: return t1 ? "reserved" : "hflex";
    case GFNT_CS2_FLEX: return t1 ? "reserved" : "flex";
    case GFNT_CS2_HFLEX1: return t1 ? "reserved" : "hflex1";
    case GFNT_CS2_FLEX1: return t1 ? "reserved" : "flex1";
    default: return "reserved";
  }
}

/** Write one 16.16 operand the way the dump spells numbers. */
static int gfnt_cs_dump_operand(FILE * out, int64_t value) {
  if ((value & 0xFFFF) == 0) {
    return fprintf(out, " %ld", (long)(value >> 16));
  }
  // A fractional operand is printed as its raw 16.16, tagged, because a decimal
  // rendering would be a second rounding rule in a file whose purpose is to
  // show exactly what the font said.
  return fprintf(out, " %ld+%u/65536", (long)(value >> 16),
      (unsigned)(value & 0xFFFF));
}

GFNT_Result gfnt_charstring_dump(GFNT_CharstringType type,
    const uint8_t * bytes, size_t length,
    const GFNT_CharstringMetrics * metrics, FILE * out) {
  size_t cursor = 0;
  size_t stems = 0;
  size_t pending = 0;
  int64_t operands[GFNT_CS_STACK_CFF2];
  bool called_subr = false;
  const bool cff2 = type == GFNT_CHARSTRING_CFF2;
  // What a run of this program reported, when the caller has one. It is the
  // stem count that decides every mask's width, and a subroutine may have
  // declared some of them.
  const bool stems_known = metrics != NULL;

  if ((!bytes && length > 0) || !out
      || type >= GFNT_CHARSTRING_TYPE_COUNT) {
    return GFNT_ERR_INVALID;
  }
  if (fprintf(out, "charstring %s %zu bytes\n",
      gfnt_charstring_type_string(type), length) < 0) {
    return GFNT_ERR_IO;
  }
  if (cff2) {
    // Everything below asks whether the language is Type 2, and CFF2's is.
    type = GFNT_CHARSTRING_TYPE2;
  }
  while (cursor < length) {
    size_t at = cursor;
    uint8_t op = bytes[cursor++];
    uint8_t op2 = 0;
    size_t index;

    if (op >= 32 || (op == GFNT_CS_SHORTINT
        && type == GFNT_CHARSTRING_TYPE2)) {
      // Operands are accumulated and printed with the operator they belong to,
      // which is the only arrangement in which a line of this file is readable
      // as a drawing instruction.
      int64_t value = 0;

      if (op >= 32 && op <= 246) {
        value = ((int64_t)op - 139) * GFNT_FIXED_ONE;
      }
      else if (op >= 247 && op <= 250) {
        if (cursor >= length) { break; }
        value = (((int64_t)op - 247) * 256 + bytes[cursor++] + 108)
            * GFNT_FIXED_ONE;
      }
      else if (op >= 251 && op <= 254) {
        if (cursor >= length) { break; }
        value = (-((int64_t)op - 251) * 256 - bytes[cursor++] - 108)
            * GFNT_FIXED_ONE;
      }
      else if (op == GFNT_CS_SHORTINT) {
        if (cursor + 2 > length) { break; }
        value = (int64_t)(int16_t)(((uint16_t)bytes[cursor] << 8)
            | bytes[cursor + 1]);
        value *= GFNT_FIXED_ONE;
        cursor += 2;
      }
      else {
        uint32_t raw;

        if (cursor + 4 > length) { break; }
        raw = ((uint32_t)bytes[cursor] << 24) | ((uint32_t)bytes[cursor + 1] << 16)
            | ((uint32_t)bytes[cursor + 2] << 8) | bytes[cursor + 3];
        value = (int64_t)(int32_t)raw;
        if (type == GFNT_CHARSTRING_TYPE1) {
          value *= GFNT_FIXED_ONE;
        }
        cursor += 4;
      }
      if (pending < (cff2 ? GFNT_CS_STACK_CFF2 : GFNT_CS_STACK)) {
        operands[pending++] = value;
      }
      continue;
    }
    if (op == GFNT_CS_ESCAPE) {
      if (cursor >= length) { break; }
      op2 = bytes[cursor++];
    }
    if (fprintf(out, "%zu %s", at, gfnt_cs_op_name(type, cff2, op, op2)) < 0) {
      return GFNT_ERR_IO;
    }
    for (index = 0; index < pending; index++) {
      if (gfnt_cs_dump_operand(out, operands[index]) < 0) {
        return GFNT_ERR_IO;
      }
    }
    // Stems are counted only to divide a mask, and only Type 2 has masks -
    // in Type 1 two of these four bytes are reserved and spell nothing.
    if (type == GFNT_CHARSTRING_TYPE2
        && (op == GFNT_CS_HSTEM || op == GFNT_CS_VSTEM
            || op == GFNT_CS_HSTEMHM || op == GFNT_CS_VSTEMHM)) {
      stems += pending / 2;
    }
    if (op == GFNT_CS_CALLSUBR || op == GFNT_CS_CALLGSUBR) {
      called_subr = true;
    }
    if (type == GFNT_CHARSTRING_TYPE2
        && (op == GFNT_CS_HINTMASK || op == GFNT_CS_CNTRMASK)) {
      size_t mask_bytes;

      stems += pending / 2;
      mask_bytes = ((stems_known ? metrics->stems : stems) + 7) / 8;
      if (called_subr && !stems_known) {
        // The mask's width comes from how many stems have been declared, and a
        // subroutine can declare them. A dump does not run subroutines, so from
        // here the byte stream cannot be divided into operators at all - and
        // guessing a width would print operators that are not there.
        if (fprintf(out, " (mask width unknown: a subroutine was called, and "
            "stems declared inside one cannot be counted without running it; "
            "pass the metrics of a run to see the rest)\n") < 0) {
          return GFNT_ERR_IO;
        }
        return GFNT_OK;
      }
      if (cursor + mask_bytes > length) {
        if (fprintf(out, " (mask runs past the end)\n") < 0) {
          return GFNT_ERR_IO;
        }
        return GFNT_ERR_CORRUPT;
      }
      if (fprintf(out, " mask") < 0) {
        return GFNT_ERR_IO;
      }
      for (index = 0; index < mask_bytes; index++) {
        if (fprintf(out, " %02X", bytes[cursor + index]) < 0) {
          return GFNT_ERR_IO;
        }
      }
      cursor += mask_bytes;
    }
    if (fprintf(out, "\n") < 0) {
      return GFNT_ERR_IO;
    }
    pending = 0;
  }
  if (pending > 0) {
    // Operands with no operator after them: the charstring ended mid-instruction.
    if (fprintf(out, "%zu trailing-operands %zu\n", cursor, pending) < 0) {
      return GFNT_ERR_IO;
    }
  }
  return GFNT_OK;
}
