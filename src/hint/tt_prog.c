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
 * Building a per-size hinting context (running `fpgm` and `prep`) and running a
 * glyph's program over it.
 */

#include "vm_internal.h"

#include <string.h>

static void * gfnt_hint_calloc(const GFNT_Allocator * a, size_t n, size_t size) {
  return a->calloc_fn(a->ctx, n ? n : 1u, size);
}

static void gfnt_hint_free(const GFNT_Allocator * a, void * p) {
  if (p) {
    a->free_fn(a->ctx, p);
  }
}

/* To the nearest whole pixel with a tie going up. */
static int32_t gfnt_hint_pixel(int32_t v) {
  return (int32_t)((uint32_t)gfnt_hint_add(v, 32) & ~63u);
}

static size_t gfnt_hint_min(size_t a, size_t b) {
  return a < b ? a : b;
}

void gfnt_hint_ctx_destroy(GFNT_HintContext * ctx) {
  const GFNT_Allocator * a;

  if (!ctx) {
    return;
  }
  a = ctx->allocator;
  gfnt_hint_free(a, ctx->boundary[0]);
  gfnt_hint_free(a, ctx->boundary[1]);
  gfnt_hint_free(a, ctx->functions);
  gfnt_hint_free(a, ctx->idefs);
  gfnt_hint_free(a, ctx->cvt);
  gfnt_hint_free(a, ctx->storage);
  gfnt_hint_free(a, ctx->coords);
  gfnt_hint_zone_free(a, &ctx->twilight);
  gfnt_hint_free(a, ctx);
}

void gfnt_hint_run_destroy(GFNT_HintRun * run) {
  const GFNT_Allocator * a;

  if (!run) {
    return;
  }
  a = run->allocator;
  gfnt_hint_free(a, run->stack);
  gfnt_hint_free(a, run->frames);
  gfnt_hint_free(a, run->glyph_boundary);
  if (run->owns_state) {
    gfnt_hint_free(a, run->storage);
    gfnt_hint_free(a, run->cvt);
    gfnt_hint_zone_free(a, &run->zone[0]);
  }
  gfnt_hint_zone_free(a, &run->zone[1]);
  gfnt_hint_free(a, run);
}

static GFNT_Result gfnt_hint_run_create(GFNT_HintContext * ctx, bool building,
    GFNT_HintRun ** out) {
  const GFNT_Allocator * a = ctx->allocator;
  GFNT_HintRun * run = gfnt_hint_calloc(a, 1, sizeof *run);

  if (!run) {
    return GFNT_ERR_OOM;
  }
  run->allocator = a;
  run->ctx = ctx;
  run->scale = ctx->scale;
  run->building = building;
  run->stack_capacity = ctx->stack_capacity;
  run->stack = gfnt_hint_calloc(a, run->stack_capacity, sizeof run->stack[0]);
  run->frames = gfnt_hint_calloc(a, ctx->limits.max_call_depth, sizeof run->frames[0]);
  run->storage_count = ctx->storage_count;
  run->cvt_count = ctx->cvt_count;
  if (building) {
    /* The context's own arrays: what the font programs write is what stays. */
    run->storage = ctx->storage;
    run->cvt = ctx->cvt;
    run->gs = ctx->gstate;
    run->instruct_control = 0;
    /* The twilight zone is shared with the context, which keeps what prep makes. */
    run->zone[0] = ctx->twilight;
  }
  else {
    run->owns_state = true;
    run->storage = gfnt_hint_calloc(a, ctx->storage_count, sizeof run->storage[0]);
    run->cvt = gfnt_hint_calloc(a, ctx->cvt_count, sizeof run->cvt[0]);
    if (run->storage && run->cvt) {
      memcpy(run->storage, ctx->storage, ctx->storage_count * sizeof run->storage[0]);
      memcpy(run->cvt, ctx->cvt, ctx->cvt_count * sizeof run->cvt[0]);
    }
    run->gs = ctx->gstate;
    run->instruct_control = ctx->instruct_control;
    if (gfnt_hint_zone_alloc(a, &run->zone[0], ctx->twilight.capacity, 0) == GFNT_OK) {
      gfnt_hint_zone_copy(&run->zone[0], &ctx->twilight);
    }
    else {
      run->storage = NULL;   /* fails the check below */
    }
  }
  if (!run->stack || !run->frames || !run->storage || !run->cvt) {
    gfnt_hint_run_destroy(run);
    return GFNT_ERR_OOM;
  }
  *out = run;
  return GFNT_OK;
}

static GFNT_Result gfnt_hint_result_of(GFNT_HintFailure failure) {
  switch (failure) {
    case GFNT_HINT_OK: return GFNT_OK;
    case GFNT_HINT_FAIL_BUDGET:
    case GFNT_HINT_FAIL_CALL_DEPTH: return GFNT_ERR_LIMIT;
    case GFNT_HINT_FAIL_UNSUPPORTED: return GFNT_ERR_UNSUPPORTED;
    case GFNT_HINT_FAIL_NO_MEMORY: return GFNT_ERR_OOM;
    default: return GFNT_ERR_CORRUPT;
  }
}

GFNT_Result gfnt_hint_ctx_create(const GFNT_Allocator * allocator,
    const GFNT_HintLimits * limits, const GFNT_HintParams * params,
    const GFNT_HintShape * shape, const uint8_t * fpgm, size_t fpgm_length,
    const uint8_t * prep, size_t prep_length, const int32_t * cvt,
    size_t cvt_count, GFNT_HintContext ** out) {
  GFNT_HintContext * ctx;
  GFNT_HintRun * run = NULL;
  GFNT_Result result;
  GFNT_HintLimits caps;
  size_t stack;

  if (!allocator || !params || !shape || !out || params->ppem == 0
      || params->units_per_em == 0 || (params->version != 35 && params->version != 40)
      || (!fpgm && fpgm_length) || (!prep && prep_length) || (!cvt && cvt_count)) {
    return GFNT_ERR_INVALID;
  }
  if (limits) {
    caps = *limits;
  }
  else {
    gfnt_hint_limits_default(&caps);
  }
  if (shape->storage > caps.max_storage || shape->functions > caps.max_functions
      || shape->idefs > caps.max_idefs || cvt_count > caps.max_cvt
      || fpgm_length > caps.max_program || prep_length > caps.max_program) {
    return GFNT_ERR_LIMIT;
  }
  ctx = gfnt_hint_calloc(allocator, 1, sizeof *ctx);
  if (!ctx) {
    return GFNT_ERR_OOM;
  }
  ctx->allocator = allocator;
  ctx->limits = caps;
  ctx->params = *params;
  ctx->shape = *shape;
  if (params->coord_count) {
    if (!params->coords) {
      gfnt_hint_ctx_destroy(ctx);
      return GFNT_ERR_INVALID;
    }
    ctx->coords = gfnt_hint_calloc(allocator, params->coord_count, sizeof *ctx->coords);
    if (!ctx->coords) {
      gfnt_hint_ctx_destroy(ctx);
      return GFNT_ERR_OOM;
    }
    memcpy(ctx->coords, params->coords, params->coord_count * sizeof *ctx->coords);
    ctx->params.coords = ctx->coords;
    ctx->variation.coords = ctx->coords;
    ctx->variation.count = params->coord_count;
  }
  else {
    ctx->params.coords = NULL;
  }
  /* Pixels (26.6) per font unit, 16.16: the one scale every conversion uses. */
  ctx->scale = gfnt_hint_div_fix((int32_t)(params->ppem * 64u),
      (int32_t)params->units_per_em);
  ctx->code[0] = fpgm;
  ctx->code_length[0] = fpgm_length;
  ctx->code[1] = prep;
  ctx->code_length[1] = prep_length;
  /* A font declares its stack depth in maxp, and some declare too little. */
  stack = shape->stack + 32u;
  ctx->stack_capacity = gfnt_hint_min(stack < 64u ? 64u : stack, caps.max_stack);
  ctx->function_count = shape->functions;
  ctx->idef_capacity = shape->idefs;
  ctx->storage_count = shape->storage;
  ctx->cvt_count = cvt_count;
  ctx->functions = gfnt_hint_calloc(allocator, ctx->function_count, sizeof ctx->functions[0]);
  ctx->idefs = gfnt_hint_calloc(allocator, ctx->idef_capacity, sizeof ctx->idefs[0]);
  ctx->storage = gfnt_hint_calloc(allocator, ctx->storage_count, sizeof ctx->storage[0]);
  ctx->cvt = gfnt_hint_calloc(allocator, ctx->cvt_count, sizeof ctx->cvt[0]);
  ctx->boundary[0] = gfnt_hint_boundaries(allocator, fpgm, fpgm_length);
  ctx->boundary[1] = gfnt_hint_boundaries(allocator, prep, prep_length);
  if (!ctx->functions || !ctx->idefs || !ctx->storage || !ctx->cvt
      || !ctx->boundary[0] || !ctx->boundary[1]) {
    gfnt_hint_ctx_destroy(ctx);
    return GFNT_ERR_OOM;
  }
  for (size_t i = 0; i < cvt_count; ++i) {
    ctx->cvt[i] = gfnt_hint_mul_fix(cvt[i], ctx->scale);
  }
  gfnt_hint_gstate_default(&ctx->gstate);
  if (shape->twilight > caps.max_points) {
    gfnt_hint_ctx_destroy(ctx);
    return GFNT_ERR_LIMIT;
  }
  if (gfnt_hint_zone_alloc(allocator, &ctx->twilight, shape->twilight, 0) != GFNT_OK) {
    gfnt_hint_ctx_destroy(ctx);
    return GFNT_ERR_OOM;
  }
  ctx->twilight.count = shape->twilight;

  result = gfnt_hint_run_create(ctx, true, &run);
  if (result != GFNT_OK) {
    gfnt_hint_ctx_destroy(ctx);
    return result;
  }
  ctx->usable = true;
  if (fpgm_length) {
    if (gfnt_hint_exec(run, GFNT_HINT_PROGRAM_FONT) != GFNT_HINT_OK) {
      ctx->usable = false;
    }
  }
  if (ctx->usable && prep_length) {
    /* The font program leaves a clean stack and state; prep starts from the
       defaults, as every glyph will. */
    run->depth = 0;
    run->gs = ctx->gstate;
    if (gfnt_hint_exec(run, GFNT_HINT_PROGRAM_CVT) != GFNT_HINT_OK) {
      ctx->usable = false;
    }
  }
  ctx->fault = run->fault;
  if (ctx->usable) {
    /* What prep leaves in the graphics state is each glyph's starting point;
       these cannot carry the point-related parts (references, zones, vectors),
       which return to the defaults for every glyph. */
    GFNT_HintGState defaults;

    gfnt_hint_gstate_default(&defaults);
    ctx->gstate = run->gs;
    memcpy(ctx->gstate.pv, defaults.pv, sizeof defaults.pv);
    memcpy(ctx->gstate.fv, defaults.fv, sizeof defaults.fv);
    memcpy(ctx->gstate.dpv, defaults.dpv, sizeof defaults.dpv);
    memcpy(ctx->gstate.rp, defaults.rp, sizeof defaults.rp);
    memcpy(ctx->gstate.zp, defaults.zp, sizeof defaults.zp);
    ctx->gstate.loop = 1;
    /* The rounding state is not carried over either: every glyph starts at the
       grid, whatever the control value program left. */
    gfnt_hint_set_round(&ctx->gstate, GFNT_HINT_ROUND_GRID);
    ctx->instruct_control = run->instruct_control;
  }
  gfnt_hint_run_destroy(run);
  *out = ctx;
  return GFNT_OK;
}

bool gfnt_hint_ctx_usable(const GFNT_HintContext * ctx) {
  return ctx && ctx->usable;
}

GFNT_HintFault gfnt_hint_ctx_fault(const GFNT_HintContext * ctx) {
  GFNT_HintFault none = {GFNT_HINT_OK, GFNT_HINT_PROGRAM_FONT, 0, 0};

  return ctx ? ctx->fault : none;
}

int32_t gfnt_hint_ctx_cvt(const GFNT_HintContext * ctx, size_t index) {
  return ctx && index < ctx->cvt_count ? ctx->cvt[index] : 0;
}

int32_t gfnt_hint_ctx_storage(const GFNT_HintContext * ctx, size_t index) {
  return ctx && index < ctx->storage_count ? ctx->storage[index] : 0;
}

const GFNT_HintGState * gfnt_hint_ctx_gstate(const GFNT_HintContext * ctx) {
  return &ctx->gstate;
}

uint32_t gfnt_hint_ctx_instruct_control(const GFNT_HintContext * ctx) {
  return ctx->instruct_control;
}

GFNT_Result gfnt_hint_run_glyph(const GFNT_HintContext * ctx,
    const GFNT_HintGlyphInput * input, const uint8_t * program, size_t length,
    GFNT_HintRun ** out) {
  GFNT_HintRun * run = NULL;
  GFNT_Result result;

  if (!ctx || !out || (!program && length)) {
    return GFNT_ERR_INVALID;
  }
  *out = NULL;
  if (!ctx->usable) {
    return GFNT_ERR_INVALID;
  }
  /* The run only reads the context: its own copies take the writes. */
  result = gfnt_hint_run_create((GFNT_HintContext *)(uintptr_t)ctx, false, &run);
  if (result != GFNT_OK) {
    return result;
  }
  run->glyph_code = program;
  run->glyph_length = length;
  *out = run;
  if (input) {
    GFNT_HintZone * z = &run->zone[1];

    if (input->point_count > ctx->limits.max_points
        || gfnt_hint_zone_alloc(run->allocator, z, input->point_count,
            input->contour_count) != GFNT_OK) {
      run->fault.failure = GFNT_HINT_FAIL_NO_MEMORY;
      return GFNT_ERR_OOM;
    }
    z->count = input->point_count;
    z->contour_count = input->contour_count;
    if (input->pixels) {
      run->composite = true;
      run->scale = 0x10000;
    }
    for (size_t i = 0; i < input->point_count; ++i) {
      z->orus[i].x = input->x[i];
      z->orus[i].y = input->y[i];
      z->org[i].x = input->pixels ? input->x[i] : input->org_x ? input->org_x[i]
          : gfnt_hint_mul_fix(input->x[i], ctx->scale);
      z->org[i].y = input->pixels ? input->y[i] : input->org_y ? input->org_y[i]
          : gfnt_hint_mul_fix(input->y[i], ctx->scale);
      z->cur[i] = z->org[i];
      z->flags[i] = input->on_curve[i] ? GFNT_HINT_ON_CURVE : 0;
    }
    for (size_t c = 0; c < input->contour_count; ++c) {
      z->ends[c] = input->ends[c];
    }
    if (input->point_count >= 4) {
      /* The phantom points are placed on the pixel grid before the program runs:
         the origin and the advance across, the top and the bottom up and down. */
      size_t n = input->point_count;

      /* Where they are now is on the grid; where they began is not, so a program
         that moves with them (SHP from the advance, say) sees the rounding as a
         shift. */
      for (size_t i = n - 4u; i < n; ++i) {
        z->cur[i] = z->org[i];
      }
      /* The version 40 compatibility behaviour does not move anything along x,
         and placing the origin and the advance on the grid is such a move. */
      if (!(ctx->params.version >= 40 && !(ctx->instruct_control & 4u))) {
        z->cur[n - 4u].x = gfnt_hint_pixel(z->org[n - 4u].x);
        z->cur[n - 3u].x = gfnt_hint_pixel(z->org[n - 3u].x);
      }
      z->cur[n - 2u].y = gfnt_hint_pixel(z->org[n - 2u].y);
      z->cur[n - 1u].y = gfnt_hint_pixel(z->org[n - 1u].y);
    }
  }
  if (length > ctx->limits.max_program) {
    run->fault.failure = GFNT_HINT_FAIL_BUDGET;
    return GFNT_ERR_LIMIT;
  }
  return gfnt_hint_result_of(gfnt_hint_exec(run, GFNT_HINT_PROGRAM_GLYPH));
}

GFNT_HintFault gfnt_hint_run_fault(const GFNT_HintRun * run) {
  return run->fault;
}

bool gfnt_hint_run_on_curve(const GFNT_HintRun * run, size_t index) {
  return index < run->zone[1].count && (run->zone[1].flags[index] & GFNT_HINT_ON_CURVE);
}

bool gfnt_hint_run_normalised(const GFNT_HintRun * run) {
  return run->normalised;
}

uint8_t gfnt_hint_run_touch(const GFNT_HintRun * run, size_t index) {
  return index < run->zone[1].count ? (uint8_t)(run->zone[1].flags[index] & 3u) : 0;
}

size_t gfnt_hint_run_point_count(const GFNT_HintRun * run) {
  return run->zone[1].count;
}

bool gfnt_hint_run_point(const GFNT_HintRun * run, size_t index, int32_t * x, int32_t * y) {
  if (index >= run->zone[1].count) {
    return false;
  }
  *x = run->zone[1].cur[index].x;
  *y = run->zone[1].cur[index].y;
  return true;
}

size_t gfnt_hint_run_depth(const GFNT_HintRun * run) {
  return run->depth;
}

int32_t gfnt_hint_run_stack(const GFNT_HintRun * run, size_t index) {
  return index < run->depth ? run->stack[index] : 0;
}

int32_t gfnt_hint_run_storage(const GFNT_HintRun * run, size_t index) {
  return index < run->storage_count ? run->storage[index] : 0;
}

int32_t gfnt_hint_run_cvt(const GFNT_HintRun * run, size_t index) {
  return index < run->cvt_count ? run->cvt[index] : 0;
}

const GFNT_HintGState * gfnt_hint_run_gstate(const GFNT_HintRun * run) {
  return &run->gs;
}

uint32_t gfnt_hint_run_instruct_control(const GFNT_HintRun * run) {
  return run->instruct_control;
}
