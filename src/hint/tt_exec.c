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
 * The TrueType interpreter loop and the instructions that need no outline: the
 * stack, storage, the control value table, arithmetic and logic, control flow,
 * functions and instruction definitions, and the graphics state.
 *
 * Written from the instruction-set chapter of the OpenType specification. Where
 * that chapter is silent the choice made here is a defined one - an error, or a
 * stated value - and is marked "unspecified" so a differential can find it.
 */

#include "vm_internal.h"

#include <string.h>

/* Flow of control inside the loop: a failure names itself and leaves. */
#define FAIL(code) do { failure = (code); goto fault; } while (0)
#define NEED(n) do { if (run->depth < (size_t)(n)) FAIL(GFNT_HINT_FAIL_STACK_UNDERFLOW); } while (0)
#define ROOM(n) do { if (run->depth + (size_t)(n) > run->stack_capacity) FAIL(GFNT_HINT_FAIL_STACK_OVERFLOW); } while (0)
#define POP() (run->stack[--run->depth])
#define PUSH(v) do { int32_t pushed_ = (v); run->stack[run->depth++] = pushed_; } while (0)
#define TOP() (run->stack[run->depth - 1u])

const char * gfnt_hint_failure_name(GFNT_HintFailure failure) {
  switch (failure) {
    case GFNT_HINT_OK: return "ok";
    case GFNT_HINT_FAIL_STACK_UNDERFLOW: return "stack underflow";
    case GFNT_HINT_FAIL_STACK_OVERFLOW: return "stack overflow";
    case GFNT_HINT_FAIL_BAD_INDEX: return "index out of range";
    case GFNT_HINT_FAIL_BAD_OPCODE: return "undefined opcode";
    case GFNT_HINT_FAIL_TRUNCATED: return "instruction runs past the end of the program";
    case GFNT_HINT_FAIL_BAD_JUMP: return "jump to an invalid place";
    case GFNT_HINT_FAIL_BAD_STRUCTURE: return "misplaced definition or ENDF";
    case GFNT_HINT_FAIL_UNDEFINED_CALL: return "call of an undefined function";
    case GFNT_HINT_FAIL_DIVIDE_BY_ZERO: return "division by zero";
    case GFNT_HINT_FAIL_BAD_ARGUMENT: return "bad operand";
    case GFNT_HINT_FAIL_BUDGET: return "instruction budget exhausted";
    case GFNT_HINT_FAIL_CALL_DEPTH: return "calls nested too deeply";
    case GFNT_HINT_FAIL_UNSUPPORTED: return "instruction not implemented";
    case GFNT_HINT_FAIL_NO_MEMORY: return "out of memory";
  }
  return "unknown";
}

void gfnt_hint_limits_default(GFNT_HintLimits * limits) {
  if (!limits) {
    return;
  }
  limits->max_stack = 65536;
  limits->max_storage = 65536;
  limits->max_cvt = 262144;
  limits->max_functions = 65536;
  limits->max_idefs = 256;
  limits->max_call_depth = 64;
  limits->max_instructions = 4000000;
  limits->max_loop = 65535;
  limits->max_program = 1048576;
  limits->max_points = 65540;
}

uint8_t * gfnt_hint_boundaries(const GFNT_Allocator * allocator,
    const uint8_t * code, size_t length) {
  uint8_t * bits = allocator->calloc_fn(allocator->ctx, length / 8u + 1u, 1);
  size_t at = 0;

  if (!bits) {
    return NULL;
  }
  while (at < length) {
    GFNT_HintInsn insn;

    bits[at / 8u] = (uint8_t)(bits[at / 8u] | (1u << (at % 8u)));
    if (gfnt_hint_decode(code, length, at, &insn) != GFNT_OK) {
      break;
    }
    at += insn.length;
  }
  return bits;
}

static bool gfnt_hint_is_boundary(const uint8_t * bits, size_t at) {
  return (bits[at / 8u] >> (at % 8u)) & 1u;
}

/* The result of scanning forward over a structured region. */
typedef enum GFNT_HintScan {
  GFNT_HINT_SCAN_ELSE,  /* stopped after an ELSE at the region's level */
  GFNT_HINT_SCAN_END    /* stopped after the closing EIF or ENDF */
} GFNT_HintScan;

/*
 * Skip from @p at to the end of an IF's branch (@p want_else) or to its EIF.
 * Every instruction scanned counts against the budget, so a program cannot make
 * the skipping free.
 */
static GFNT_HintFailure gfnt_hint_skip_if(const uint8_t * code, size_t length,
    size_t * at, bool stop_at_else, size_t * budget, GFNT_HintScan * how) {
  size_t depth = 1;

  while (*at < length) {
    GFNT_HintInsn insn;

    if (gfnt_hint_decode(code, length, *at, &insn) != GFNT_OK) {
      return GFNT_HINT_FAIL_TRUNCATED;
    }
    if (*budget == 0) {
      return GFNT_HINT_FAIL_BUDGET;
    }
    --*budget;
    *at += insn.length;
    if (insn.opcode == 0x58) {
      ++depth;
    }
    else if (insn.opcode == 0x1B && depth == 1 && stop_at_else) {
      *how = GFNT_HINT_SCAN_ELSE;
      return GFNT_HINT_OK;
    }
    else if (insn.opcode == 0x59 && --depth == 0) {
      *how = GFNT_HINT_SCAN_END;
      return GFNT_HINT_OK;
    }
  }
  return GFNT_HINT_FAIL_TRUNCATED;
}

/* Skip a definition's body: from @p at to the offset of its ENDF. */
static GFNT_HintFailure gfnt_hint_skip_def(const uint8_t * code, size_t length,
    size_t at, size_t * budget, size_t * endf) {
  while (at < length) {
    GFNT_HintInsn insn;

    if (gfnt_hint_decode(code, length, at, &insn) != GFNT_OK) {
      return GFNT_HINT_FAIL_TRUNCATED;
    }
    if (*budget == 0) {
      return GFNT_HINT_FAIL_BUDGET;
    }
    --*budget;
    if (insn.opcode == 0x2D) {
      *endf = at;
      return GFNT_HINT_OK;
    }
    if (insn.opcode == 0x2C || insn.opcode == 0x89) {
      return GFNT_HINT_FAIL_BAD_STRUCTURE; /* definitions do not nest */
    }
    at += insn.length;
  }
  return GFNT_HINT_FAIL_TRUNCATED;
}

/* The magnitude the DELTA instructions' low nibble stands for, in 26.6. */
static int32_t gfnt_hint_delta_amount(unsigned nibble, int32_t shift) {
  int32_t steps = nibble < 8u ? (int32_t)nibble - 8 : (int32_t)nibble - 7;

  return (steps * 64) / (1 << shift);
}

/* Switch the code being executed to program @p which. */
#define SELECT(which) do { \
    if ((which) == GFNT_HINT_PROGRAM_GLYPH) { \
      code = run->glyph_code; length = run->glyph_length; boundary = run->glyph_boundary; \
    } else { \
      code = ctx->code[(which)]; length = ctx->code_length[(which)]; boundary = ctx->boundary[(which)]; \
    } } while (0)

/* Enter @p function_ to run it @p loops times, returning to the next instruction. */
#define BEGIN_CALL(function_, loops) do { \
    if (call_depth >= limits->max_call_depth) FAIL(GFNT_HINT_FAIL_CALL_DEPTH); \
    run->frames[call_depth].fn = (function_); \
    run->frames[call_depth].return_ip = ip; \
    run->frames[call_depth].loops_left = (loops); \
    run->frames[call_depth].return_program = (uint8_t)current; \
    ++call_depth; \
    current = (GFNT_HintProgram)(function_)->program; \
    SELECT(current); \
    ip = (function_)->start; \
  } while (0)

GFNT_HintFailure gfnt_hint_exec(GFNT_HintRun * run, GFNT_HintProgram program) {
  GFNT_HintContext * ctx = run->ctx;
  const GFNT_HintLimits * limits = &ctx->limits;
  const uint8_t * code;
  size_t length;
  const uint8_t * boundary;
  size_t ip = 0;
  size_t call_depth = 0;
  size_t budget = limits->max_instructions;
  GFNT_HintFailure failure = GFNT_HINT_OK;
  GFNT_HintProgram current = program;
  GFNT_HintInsn insn;
  uint8_t op = 0;
  size_t op_offset = 0;

  SELECT(program);
  if (length > limits->max_program) {
    return GFNT_HINT_FAIL_TRUNCATED;
  }

  for (;;) {
    if (ip >= length) {
      if (call_depth) {
        /* A function that runs off its end without ENDF: the ENDF scan makes
           this impossible, but the check costs nothing. */
        FAIL(GFNT_HINT_FAIL_BAD_STRUCTURE);
      }
      break;
    }
    op_offset = ip;
    if (gfnt_hint_decode(code, length, ip, &insn) != GFNT_OK) {
      FAIL(GFNT_HINT_FAIL_TRUNCATED);
    }
    if (budget == 0) {
      FAIL(GFNT_HINT_FAIL_BUDGET);
    }
    --budget;
    op = insn.opcode;
    ip += insn.length;

    switch (op) {
      /* ---- pushing ---------------------------------------------------- */
      case 0x40: case 0x41:
      case 0xB0: case 0xB1: case 0xB2: case 0xB3:
      case 0xB4: case 0xB5: case 0xB6: case 0xB7:
      case 0xB8: case 0xB9: case 0xBA: case 0xBB:
      case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        ROOM(insn.count);
        for (size_t i = 0; i < insn.count; ++i) {
          PUSH(gfnt_hint_push_value(code, &insn, i));
        }
        break;

      /* ---- the stack -------------------------------------------------- */
      case 0x20: /* DUP */
        NEED(1); ROOM(1);
        PUSH(TOP());
        break;
      case 0x21: /* POP */
        NEED(1);
        --run->depth;
        break;
      case 0x22: /* CLEAR */
        run->depth = 0;
        break;
      case 0x23: { /* SWAP */
        int32_t t;
        NEED(2);
        t = run->stack[run->depth - 1u];
        run->stack[run->depth - 1u] = run->stack[run->depth - 2u];
        run->stack[run->depth - 2u] = t;
        break;
      }
      case 0x24: /* DEPTH */
        ROOM(1);
        PUSH((int32_t)run->depth);
        break;
      case 0x25: { /* CINDEX */
        int32_t k;
        NEED(1);
        k = POP();
        if (k < 1 || (size_t)k > run->depth) FAIL(GFNT_HINT_FAIL_BAD_INDEX);
        PUSH(run->stack[run->depth - (size_t)k]);
        break;
      }
      case 0x26: { /* MINDEX */
        int32_t k;
        int32_t moved;
        NEED(1);
        k = POP();
        if (k < 1 || (size_t)k > run->depth) FAIL(GFNT_HINT_FAIL_BAD_INDEX);
        moved = run->stack[run->depth - (size_t)k];
        memmove(&run->stack[run->depth - (size_t)k],
            &run->stack[run->depth - (size_t)k + 1u],
            ((size_t)k - 1u) * sizeof run->stack[0]);
        run->stack[run->depth - 1u] = moved;
        break;
      }
      case 0x8A: { /* ROLL: c b a -> b a c */
        int32_t a;
        int32_t b;
        int32_t c;
        NEED(3);
        a = POP();
        b = POP();
        c = POP();
        PUSH(b);
        PUSH(a);
        PUSH(c);
        break;
      }

      /* ---- storage and the control value table ------------------------ */
      case 0x42: { /* WS */
        int32_t value;
        int32_t index;
        NEED(2);
        value = POP();
        index = POP();
        if (index < 0 || (size_t)index >= run->storage_count) FAIL(GFNT_HINT_FAIL_BAD_INDEX);
        run->storage[index] = value;
        break;
      }
      case 0x43: { /* RS */
        int32_t index;
        NEED(1);
        index = TOP();
        if (index < 0 || (size_t)index >= run->storage_count) FAIL(GFNT_HINT_FAIL_BAD_INDEX);
        TOP() = run->storage[index];
        break;
      }
      case 0x44: case 0x70: { /* WCVTP, WCVTF */
        int32_t value;
        int32_t index;
        NEED(2);
        value = POP();
        index = POP();
        if (index < 0 || (size_t)index >= run->cvt_count) FAIL(GFNT_HINT_FAIL_BAD_INDEX);
        run->cvt[index] = op == 0x44 ? value : gfnt_hint_mul_fix(value, ctx->scale);
        break;
      }
      case 0x45: { /* RCVT */
        int32_t index;
        NEED(1);
        index = TOP();
        if (index < 0 || (size_t)index >= run->cvt_count) FAIL(GFNT_HINT_FAIL_BAD_INDEX);
        TOP() = run->cvt[index];
        break;
      }

      /* ---- arithmetic and logic --------------------------------------- */
      case 0x60: { int32_t n1; NEED(2); n1 = POP(); TOP() = gfnt_hint_add(TOP(), n1); break; }
      case 0x61: { int32_t n1; NEED(2); n1 = POP(); TOP() = gfnt_hint_sub(TOP(), n1); break; }
      case 0x62: { /* DIV: n2 / n1; truncated (the specification says so) */
        int32_t n1;
        NEED(2);
        n1 = POP();
        if (n1 == 0) FAIL(GFNT_HINT_FAIL_DIVIDE_BY_ZERO);
        TOP() = gfnt_hint_muldiv_trunc(TOP(), 64, n1);
        break;
      }
      case 0x63: { /* MUL: rounded; the specification does not say */
        int32_t n1;
        NEED(2);
        n1 = POP();
        TOP() = gfnt_hint_muldiv(TOP(), n1, 64);
        break;
      }
      case 0x64: /* ABS */
        NEED(1);
        TOP() = TOP() < 0 ? gfnt_hint_sub(0, TOP()) : TOP();
        break;
      case 0x65: /* NEG */
        NEED(1);
        TOP() = gfnt_hint_sub(0, TOP());
        break;
      case 0x66: /* FLOOR */
        NEED(1);
        TOP() = (int32_t)((uint32_t)TOP() & ~63u);
        break;
      case 0x67: /* CEILING */
        NEED(1);
        TOP() = (int32_t)((uint32_t)gfnt_hint_add(TOP(), 63) & ~63u);
        break;
      case 0x8B: { int32_t b; NEED(2); b = POP(); if (b > TOP()) TOP() = b; break; }
      case 0x8C: { int32_t b; NEED(2); b = POP(); if (b < TOP()) TOP() = b; break; }
      case 0x50: { int32_t b; NEED(2); b = POP(); TOP() = TOP() < b; break; }
      case 0x51: { int32_t b; NEED(2); b = POP(); TOP() = TOP() <= b; break; }
      case 0x52: { int32_t b; NEED(2); b = POP(); TOP() = TOP() > b; break; }
      case 0x53: { int32_t b; NEED(2); b = POP(); TOP() = TOP() >= b; break; }
      case 0x54: { int32_t b; NEED(2); b = POP(); TOP() = TOP() == b; break; }
      case 0x55: { int32_t b; NEED(2); b = POP(); TOP() = TOP() != b; break; }
      case 0x56: case 0x57: { /* ODD, EVEN: round, then test the integer part */
        uint32_t whole;
        NEED(1);
        whole = (uint32_t)gfnt_hint_round(&run->gs, TOP()) >> 6;
        TOP() = op == 0x56 ? (int32_t)(whole & 1u) : (int32_t)(~whole & 1u);
        break;
      }
      case 0x5A: { int32_t b; NEED(2); b = POP(); TOP() = TOP() != 0 && b != 0; break; }
      case 0x5B: { int32_t b; NEED(2); b = POP(); TOP() = TOP() != 0 || b != 0; break; }
      case 0x5C: NEED(1); TOP() = TOP() == 0; break;

      /* ---- rounding --------------------------------------------------- */
      case 0x18: gfnt_hint_set_round(&run->gs, GFNT_HINT_ROUND_GRID); break;
      case 0x19: gfnt_hint_set_round(&run->gs, GFNT_HINT_ROUND_HALF); break;
      case 0x3D: gfnt_hint_set_round(&run->gs, GFNT_HINT_ROUND_DOUBLE); break;
      case 0x7D: gfnt_hint_set_round(&run->gs, GFNT_HINT_ROUND_DOWN); break;
      case 0x7C: gfnt_hint_set_round(&run->gs, GFNT_HINT_ROUND_UP); break;
      case 0x7A: gfnt_hint_set_round(&run->gs, GFNT_HINT_ROUND_OFF); break;
      case 0x76: case 0x77:
        NEED(1);
        gfnt_hint_set_super_round(&run->gs, POP(), op == 0x77);
        break;
      case 0x68: case 0x69: case 0x6A: case 0x6B: /* ROUND[ab]: no engine compensation */
        NEED(1);
        TOP() = gfnt_hint_round(&run->gs, TOP());
        break;
      case 0x6C: case 0x6D: case 0x6E: case 0x6F: /* NROUND[ab] */
        NEED(1);
        break;

      /* ---- the rest of the graphics state ----------------------------- */
      case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: {
        /* SVTCA, SPVTCA, SFVTCA: a == 1 is the x axis. */
        int16_t x = (op & 1u) ? 0x4000 : 0;
        int16_t y = (op & 1u) ? 0 : 0x4000;
        if (op <= 0x01 || (op >= 0x02 && op <= 0x03)) {
          run->gs.pv[0] = x; run->gs.pv[1] = y;
          run->gs.dpv[0] = x; run->gs.dpv[1] = y;
        }
        if (op <= 0x01 || op >= 0x04) {
          run->gs.fv[0] = x; run->gs.fv[1] = y;
        }
        break;
      }
      case 0x0C: ROOM(2); PUSH(run->gs.pv[0]); PUSH(run->gs.pv[1]); break;
      case 0x0D: ROOM(2); PUSH(run->gs.fv[0]); PUSH(run->gs.fv[1]); break;
      case 0x0E: /* SFVTPV */
        run->gs.fv[0] = run->gs.pv[0];
        run->gs.fv[1] = run->gs.pv[1];
        break;
      case 0x10: case 0x11: case 0x12: { /* SRP0-2: validated when used */
        int32_t p;
        NEED(1);
        p = POP();
        run->gs.rp[op - 0x10u] = (uint32_t)p;
        break;
      }
      case 0x13: case 0x14: case 0x15: case 0x16: { /* SZP0-2, SZPS */
        int32_t zone;
        NEED(1);
        zone = POP();
        if (zone != 0 && zone != 1) FAIL(GFNT_HINT_FAIL_BAD_ARGUMENT);
        if (op == 0x16) {
          run->gs.zp[0] = run->gs.zp[1] = run->gs.zp[2] = (uint8_t)zone;
        }
        else {
          run->gs.zp[op - 0x13u] = (uint8_t)zone;
        }
        break;
      }
      case 0x17: { /* SLOOP */
        int32_t n;
        NEED(1);
        n = POP();
        if (n < 0 || (size_t)n > limits->max_loop) FAIL(GFNT_HINT_FAIL_BAD_ARGUMENT);
        run->gs.loop = n;
        break;
      }
      case 0x1A: NEED(1); run->gs.min_distance = POP(); break;
      case 0x1D: NEED(1); run->gs.cvt_cut_in = POP(); break;
      case 0x1E: NEED(1); run->gs.sw_cut_in = POP(); break;
      case 0x1F: NEED(1); run->gs.sw_value = gfnt_hint_mul_fix(POP(), ctx->scale); break;
      case 0x4D: run->gs.auto_flip = true; break;
      case 0x4E: run->gs.auto_flip = false; break;
      case 0x5E: NEED(1); run->gs.delta_base = POP(); break;
      case 0x5F: {
        int32_t shift;
        NEED(1);
        shift = POP();
        if (shift < 0 || shift > 6) FAIL(GFNT_HINT_FAIL_BAD_ARGUMENT);
        run->gs.delta_shift = shift;
        break;
      }
      case 0x7E: case 0x7F: NEED(1); --run->depth; break; /* SANGW, AA: no effect */
      case 0x85: NEED(1); run->gs.scan_control = (uint16_t)((uint32_t)POP() & 0xFFFFu); break;
      case 0x8D: NEED(1); run->gs.scan_type = (uint16_t)((uint32_t)POP() & 0xFFFFu); break;
      case 0x8E: { /* INSTCTRL: the selector is on top */
        int32_t selector;
        int32_t value;
        NEED(2);
        selector = POP();
        value = POP();
        if (selector >= 1 && selector <= 3) {
          uint32_t mask = 1u << (selector - 1);
          /* Selectors 1 and 2 take effect only from the control value program. */
          if (selector == 3 || program == GFNT_HINT_PROGRAM_CVT) {
            run->instruct_control = (run->instruct_control & ~mask)
                | ((uint32_t)value & mask);
          }
        }
        break;
      }
      case 0x4B: ROOM(1); PUSH((int32_t)ctx->params.ppem); break; /* MPPEM */
      case 0x4C: /* MPS: the size in pixels for the original interpreter, in 26.6 for 40 */
        ROOM(1);
        PUSH(ctx->params.version >= 40 ? (int32_t)(ctx->params.ppem * 64u) : (int32_t)ctx->params.ppem);
        break;
      case 0x88: { /* GETINFO */
        int32_t selector;
        int32_t result = 0;
        NEED(1);
        selector = TOP();
        if (selector & 1) result |= ctx->params.version;
        /* The size of the answer for each question is FreeType's: the chapter's
           table names the bits, and which of them a renderer sets is its own. */
        if (ctx->params.version >= 40) {
          if (selector & 64) result |= 1 << 13;     /* ClearType */
          if (selector & 1024) result |= 1 << 17;   /* sub-pixel positioning */
          if (selector & 2048) result |= 1 << 18;   /* symmetric rendering */
          if (selector & 4096) result |= 1 << 19;   /* gray ClearType */
        }
        else if ((selector & 32) && ctx->params.grayscale) {
          result |= 1 << 12;                        /* grayscale */
        }
        TOP() = result;
        break;
      }
      case 0x4F: NEED(1); --run->depth; break; /* DEBUG: no debugger */

      /* ---- DELTAC ----------------------------------------------------- */
      case 0x73: case 0x74: case 0x75: {
        int32_t n;
        int32_t first_ppem = run->gs.delta_base + 16 * (int32_t)(op - 0x73u);
        NEED(1);
        n = POP();
        if (n < 0) FAIL(GFNT_HINT_FAIL_BAD_ARGUMENT);
        NEED(2u * (size_t)n);
        while (n-- > 0) {
          int32_t index = POP();
          int32_t arg = POP();
          int32_t wanted = first_ppem + ((arg >> 4) & 15);

          if (index < 0 || (size_t)index >= run->cvt_count) FAIL(GFNT_HINT_FAIL_BAD_INDEX);
          if ((int32_t)ctx->params.ppem == wanted) {
            run->cvt[index] = gfnt_hint_add(run->cvt[index],
                gfnt_hint_delta_amount((unsigned)(arg & 15), run->gs.delta_shift));
          }
        }
        break;
      }

      /* ---- control flow ----------------------------------------------- */
      case 0x58: { /* IF */
        int32_t e;
        NEED(1);
        e = POP();
        if (e == 0) {
          GFNT_HintScan how;
          failure = gfnt_hint_skip_if(code, length, &ip, true, &budget, &how);
          if (failure != GFNT_HINT_OK) goto fault;
        }
        break;
      }
      case 0x1B: { /* ELSE reached from the true branch: skip to EIF */
        GFNT_HintScan how;
        failure = gfnt_hint_skip_if(code, length, &ip, false, &budget, &how);
        if (failure != GFNT_HINT_OK) goto fault;
        break;
      }
      case 0x59: /* EIF */
        break;
      case 0x1C: case 0x78: case 0x79: { /* JMPR, JROT, JROF */
        int32_t condition = 1;
        int32_t offset;
        int64_t target;
        if (op != 0x1C) {
          NEED(2);
          condition = POP();
        }
        else {
          NEED(1);
        }
        offset = POP();
        if (op == 0x78 && condition == 0) break;
        if (op == 0x79 && condition != 0) break;
        target = (int64_t)op_offset + offset;
        if (offset == 0 || target < 0 || target > (int64_t)length) FAIL(GFNT_HINT_FAIL_BAD_JUMP);
        if ((size_t)target < length) {
          if (!boundary && current == GFNT_HINT_PROGRAM_GLYPH) {
            uint8_t * made = gfnt_hint_boundaries(run->allocator, code, length);
            if (!made) FAIL(GFNT_HINT_FAIL_NO_MEMORY);
            /* Only a glyph program reaches here: the font programs' tables are
               built with the context. */
            run->glyph_boundary = made;
            boundary = made;
          }
          if (!boundary || !gfnt_hint_is_boundary(boundary, (size_t)target)) FAIL(GFNT_HINT_FAIL_BAD_JUMP);
        }
        ip = (size_t)target;
        break;
      }

      /* ---- functions and instruction definitions ---------------------- */
      case 0x2C: case 0x89: { /* FDEF, IDEF */
        int32_t id;
        size_t endf = 0;
        GFNT_HintFn * slot;
        NEED(1);
        id = POP();
        if (program == GFNT_HINT_PROGRAM_GLYPH || !run->building) FAIL(GFNT_HINT_FAIL_BAD_STRUCTURE);
        failure = gfnt_hint_skip_def(code, length, ip, &budget, &endf);
        if (failure != GFNT_HINT_OK) goto fault;
        if (op == 0x2C) {
          if (id < 0 || (size_t)id >= ctx->function_count) FAIL(GFNT_HINT_FAIL_BAD_INDEX);
          slot = &ctx->functions[id];
        }
        else {
          size_t i;
          if (id < 0 || id > 255) FAIL(GFNT_HINT_FAIL_BAD_ARGUMENT);
          /* An instruction definition affects only undefined opcodes. GETVARIATION
             (0x91) is the exception the specification itself makes: fonts are
             required to define it, because not every environment has it. */
          if (gfnt_hint_op((uint8_t)id)->defined && id != 0x91) {
            ip = endf + 1u;
            break;
          }
          for (i = 0; i < ctx->idef_count; ++i) {
            if (ctx->idefs[i].opcode == (uint8_t)id) break;
          }
          if (i == ctx->idef_count) {
            if (ctx->idef_count >= ctx->idef_capacity) FAIL(GFNT_HINT_FAIL_BAD_INDEX);
            ++ctx->idef_count;
          }
          slot = &ctx->idefs[i];
          slot->opcode = (uint8_t)id;
        }
        slot->start = ip;
        slot->end = endf;
        slot->program = (uint8_t)current;
        slot->defined = true;
        ip = endf + 1u;
        break;
      }
      case 0x2B: case 0x2A: { /* CALL, LOOPCALL */
        int32_t id;
        int32_t count = 1;
        NEED(op == 0x2A ? 2 : 1);
        id = POP();
        if (op == 0x2A) {
          count = POP();
        }
        if (id < 0 || (size_t)id >= ctx->function_count) FAIL(GFNT_HINT_FAIL_BAD_INDEX);
        if (!ctx->functions[id].defined) FAIL(GFNT_HINT_FAIL_UNDEFINED_CALL);
        if (count <= 0) break;
        BEGIN_CALL(&ctx->functions[id], (size_t)count);
        break;
      }
      case 0x2D: { /* ENDF: end of a call, or of one more iteration */
        GFNT_HintFrame * frame;
        if (call_depth == 0) FAIL(GFNT_HINT_FAIL_BAD_STRUCTURE);
        frame = &run->frames[call_depth - 1u];
        if (--frame->loops_left > 0) {
          if (budget == 0) FAIL(GFNT_HINT_FAIL_BUDGET);
          --budget;
          ip = frame->fn->start;
        }
        else {
          current = (GFNT_HintProgram)frame->return_program;
          SELECT(current);
          ip = frame->return_ip;
          --call_depth;
        }
        break;
      }

      default: {
        size_t i;
        if (gfnt_hint_is_point_op(op)) {
          failure = gfnt_hint_point_op(run, op, current);
          if (failure != GFNT_HINT_OK) goto fault;
          break;
        }
        /* An opcode with no meaning of its own runs its instruction definition,
           if it has one. */
        for (i = 0; i < ctx->idef_count; ++i) {
          if (ctx->idefs[i].opcode == op) break;
        }
        if (i == ctx->idef_count) {
          FAIL(gfnt_hint_op(op)->defined ? GFNT_HINT_FAIL_UNSUPPORTED
              : GFNT_HINT_FAIL_BAD_OPCODE);
        }
        BEGIN_CALL(&ctx->idefs[i], 1);
        break;
      }
    }
  }
  return GFNT_HINT_OK;

fault:
  run->fault.failure = failure;
  run->fault.program = current;
  run->fault.offset = op_offset;
  run->fault.opcode = op;
  return failure;
}
