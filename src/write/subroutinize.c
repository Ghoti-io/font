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
 * Re-subroutinising flattened Type 2 charstrings.
 *
 * documentation/design.md section 12.2. The subsetter flattens every charstring, so no
 * subroutine is shared; this finds the sequences that several charstrings repeat and
 * puts each in one global subroutine.
 *
 * **Operations, not bytes.** A charstring is cut into operations: the operands and the
 * operator that takes them. A sequence of whole operations leaves the stack as it found
 * it, so it can be lifted into a subroutine wherever it occurs, whatever precedes or
 * follows. An operator that leaves its results on the stack (arithmetic, `blend`) does
 * not end an operation. Hint and counter masks (their length is the stem count where
 * they stand), `endchar` and `vsindex` end an operation and are never put in a
 * subroutine.
 *
 * **Finding the repeats** is Re-Pair, weighted by bytes: the pair of neighbouring
 * symbols (an operation, or a subroutine already made) that would save the most is made a
 * subroutine, everywhere at once, and the next pair is looked for among the results.
 * Each round takes every pair that saves at least a quarter as much as the best and
 * shares no symbol with one already taken, so the counts stay exact. The subroutines
 * come out as sequences of two; those that do not pay for themselves once the real size
 * of a call is known (a subroutine used once never does) are put back, which turns the
 * pairs into the long subroutines a repeat deserves.
 *
 * The result is the same program: the drawing interpreter draws the same outline from
 * it, which the tests and `make check-oracle-subset` hold it to.
 */

#include <stdlib.h>
#include <string.h>
#include "../sfnt/sfnt.h"
#include "write.h"

#define NONE 0xFFFFFFFFu
#define CALL_ESTIMATE 3u     // a subroutine number and `callgsubr`, while the numbers are not yet known
#define OFFSET_ESTIMATE 2u   // the INDEX offset a subroutine costs
#define MAX_TOKENS 4000000u  // more than this and the pair tables would run to hundreds of megabytes
#define MAX_RULES 30000u     // keeps the bias at 1131: a call's number is then 1 to 3 bytes

typedef struct Sym {
  uint32_t off;       // a terminal: its bytes in the pool
  uint32_t len;       // a terminal: how many
  uint32_t body_off;  // a rule: its symbols in the body pool
  uint32_t body_len;
  uint32_t uses;      // a rule: references from the sequences and the live bodies
  uint8_t depth;      // 0 for a terminal, else one more than the deepest rule it calls
  uint8_t barrier;    // never part of a rule
  uint8_t rule;
  uint8_t dead;       // a rule that was put back
} Sym;

typedef struct Ctx {
  const GFNT_Allocator * a;
  uint8_t * pool;
  size_t pool_n;
  size_t pool_cap;
  Sym * syms;
  size_t nsyms;
  size_t syms_cap;
  uint32_t * bodies;
  size_t bodies_n;
  size_t bodies_cap;
  uint32_t * seq;     // every glyph's symbols, a barrier after each
  size_t seq_n;
  size_t seq_cap;
  uint32_t * slots;   // interning table: symbol + 1, or 0
  size_t slots_cap;
  uint32_t * rules;   // rule symbols in creation order
  size_t nrules;
  size_t rules_cap;
  uint32_t barrier;   // the symbol that separates glyphs
  bool oom;
} Ctx;

static void * grow(Ctx * c, void * p, size_t * cap, size_t need, size_t elem) {
  size_t nc;
  void * q;

  if (need <= *cap) {
    return p;
  }
  nc = *cap ? *cap : 64;
  while (nc < need) {
    nc *= 2;
  }
  q = c->a->calloc_fn(c->a->ctx, nc, elem);
  if (!q) {
    c->oom = true;
    return p;
  }
  if (p) {
    memcpy(q, p, *cap * elem);
    c->a->free_fn(c->a->ctx, p);
  }
  *cap = nc;
  return q;
}

static uint32_t new_sym(Ctx * c) {
  c->syms = grow(c, c->syms, &c->syms_cap, c->nsyms + 1, sizeof *c->syms);
  if (c->oom) {
    return NONE;
  }
  memset(&c->syms[c->nsyms], 0, sizeof c->syms[0]);
  return (uint32_t)c->nsyms++;
}

static void push_seq(Ctx * c, uint32_t v) {
  c->seq = grow(c, c->seq, &c->seq_cap, c->seq_n + 1, sizeof *c->seq);
  if (!c->oom) {
    c->seq[c->seq_n++] = v;
  }
}

static uint32_t hash_bytes(const uint8_t * p, size_t n) {
  uint32_t h = 2166136261u;
  size_t i;

  for (i = 0; i < n; ++i) {
    h = (h ^ p[i]) * 16777619u;
  }
  return h;
}

/** The terminal for these bytes, made if it is new. */
static uint32_t intern(Ctx * c, const uint8_t * p, size_t n, bool barrier) {
  size_t i;
  uint32_t id;

  if (c->nsyms * 2 + 2 > c->slots_cap) {
    size_t nc = c->slots_cap ? c->slots_cap * 2 : 1024;
    uint32_t * ns = c->a->calloc_fn(c->a->ctx, nc, sizeof *ns);
    size_t k;

    if (!ns) {
      c->oom = true;
      return NONE;
    }
    for (k = 0; k < c->nsyms; ++k) {
      if (!c->syms[k].rule) {
        size_t at = hash_bytes(c->pool + c->syms[k].off, c->syms[k].len) & (nc - 1);

        while (ns[at]) {
          at = (at + 1) & (nc - 1);
        }
        ns[at] = (uint32_t)k + 1;
      }
    }
    c->a->free_fn(c->a->ctx, c->slots);
    c->slots = ns;
    c->slots_cap = nc;
  }
  i = hash_bytes(p, n) & (c->slots_cap - 1);
  while (c->slots[i]) {
    const Sym * s = &c->syms[c->slots[i] - 1];

    if (s->len == n && memcmp(c->pool + s->off, p, n) == 0) {
      return c->slots[i] - 1;
    }
    i = (i + 1) & (c->slots_cap - 1);
  }
  id = new_sym(c);
  if (id == NONE) {
    return NONE;
  }
  c->pool = grow(c, c->pool, &c->pool_cap, c->pool_n + n + 1, 1);
  if (c->oom) {
    return NONE;
  }
  memcpy(c->pool + c->pool_n, p, n);
  c->syms[id].off = (uint32_t)c->pool_n;
  c->syms[id].len = (uint32_t)n;
  c->syms[id].barrier = barrier ? 1 : 0;
  c->pool_n += n;
  c->slots[i] = id + 1;
  return id;
}

/** The size of the operation a symbol stands for, as a reference to it. */
static uint32_t cur_len(const Ctx * c, uint32_t id) {
  const Sym * s = &c->syms[id];

  return s->rule && !s->dead ? CALL_ESTIMATE : s->len;
}

/* ---- Cutting a charstring into operations --------------------------------- */

static bool arithmetic_op(uint8_t c2) {
  switch (c2) {
    case 3: case 4: case 5: case 9: case 10: case 11: case 12: case 14: case 15: case 18:
    case 20: case 21: case 22: case 23: case 24: case 26: case 27: case 28: case 29: case 30:
      return true;
    default:
      return false;
  }
}

/**
 * Appends the operations of one charstring to the sequence. Returns false for a charstring
 * this does not understand (a call, a reserved operator, operands with no operator), which
 * leaves the whole font unsubroutinised.
 */
static bool tokenize(Ctx * c, const uint8_t * b, size_t n, const uint8_t * masks,
    size_t nmasks) {
  size_t pos = 0;
  size_t start = 0;
  size_t mask = 0;

  while (pos < n) {
    uint8_t ch = b[pos];
    size_t len;
    bool ends = false;
    bool barrier = false;

    if (ch >= 32 || ch == 28) {
      len = ch == 28 ? 3 : ch <= 246 ? 1 : ch == 255 ? 5 : 2;
      if (pos + len > n) {
        return false;
      }
      pos += len;
      continue;
    }
    switch (ch) {
      case 1: case 3: case 4: case 5: case 6: case 7: case 8: case 18: case 21: case 22:
      case 23: case 24: case 25: case 26: case 27: case 30: case 31:
        len = 1;
        ends = true;
        break;
      case 14: case 15:
        len = 1;
        ends = barrier = true;
        break;
      case 16:
        len = 1;
        break;
      case 19: case 20:
        if (mask >= nmasks || pos + 1 + masks[mask] > n) {
          return false;
        }
        len = 1 + (size_t)masks[mask++];
        ends = barrier = true;
        break;
      case 12:
        if (pos + 2 > n) {
          return false;
        }
        len = 2;
        if (b[pos + 1] >= 34 && b[pos + 1] <= 37) {
          ends = true;
        }
        else if (!arithmetic_op(b[pos + 1])) {
          return false;
        }
        break;
      default:
        return false;
    }
    pos += len;
    if (ends) {
      uint32_t id = intern(c, b + start, pos - start, barrier);

      if (id == NONE) {
        return false;
      }
      push_seq(c, id);
      start = pos;
    }
  }
  return start == n;
}

/* ---- Counting pairs ------------------------------------------------------- */

typedef struct PairEnt {
  uint64_t key;     // (a + 1) << 32 | (b + 1); zero is empty
  uint32_t count;
} PairEnt;

typedef struct Cand {
  uint64_t key;
  int64_t benefit;
} Cand;

static int cand_cmp(const void * x, const void * y) {
  const Cand * p = x;
  const Cand * q = y;

  if (p->benefit != q->benefit) {
    return p->benefit < q->benefit ? 1 : -1;
  }
  return p->key < q->key ? -1 : p->key > q->key ? 1 : 0;
}

static size_t pair_slot(const PairEnt * t, size_t cap, uint64_t key) {
  size_t i = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 20) & (cap - 1);

  while (t[i].key && t[i].key != key) {
    i = (i + 1) & (cap - 1);
  }
  return i;
}

static void count_pairs(Ctx * c, PairEnt * t, size_t cap) {
  size_t i;
  bool run = false;   // the previous position was the left of a counted pair of equals

  for (i = 0; i + 1 < c->seq_n; ++i) {
    uint32_t x = c->seq[i];
    uint32_t y = c->seq[i + 1];
    uint64_t key;
    size_t at;

    if (c->syms[x].barrier || c->syms[y].barrier) {
      run = false;
      continue;
    }
    if (x == y && run) {
      run = false;   // overlaps the pair just counted
      continue;
    }
    key = ((uint64_t)x + 1) << 32 | ((uint64_t)y + 1);
    at = pair_slot(t, cap, key);
    t[at].key = key;
    ++t[at].count;
    run = x == y;
  }
}

/* ---- The rounds ----------------------------------------------------------- */

static bool add_rule(Ctx * c, uint32_t x, uint32_t y, uint32_t * out) {
  uint32_t id = new_sym(c);

  if (id == NONE) {
    return false;
  }
  c->bodies = grow(c, c->bodies, &c->bodies_cap, c->bodies_n + 2, sizeof *c->bodies);
  c->rules = grow(c, c->rules, &c->rules_cap, c->nrules + 1, sizeof *c->rules);
  if (c->oom) {
    return false;
  }
  c->syms[id].rule = 1;
  c->syms[id].body_off = (uint32_t)c->bodies_n;
  c->syms[id].body_len = 2;
  c->syms[id].depth = (uint8_t)(1 + (c->syms[x].depth > c->syms[y].depth ? c->syms[x].depth
      : c->syms[y].depth));
  c->bodies[c->bodies_n++] = x;
  c->bodies[c->bodies_n++] = y;
  c->rules[c->nrules++] = id;
  *out = id;
  return true;
}

/** One round: returns the number of rules made, or zero when nothing pays. */
static size_t one_round(Ctx * c, uint8_t max_depth, uint32_t * lhs, uint8_t * taken,
    size_t lhs_cap) {
  size_t cap = 1024;
  PairEnt * t;
  Cand * cands = NULL;
  size_t ncands = 0;
  size_t i;
  size_t made = 0;
  int64_t best = 0;
  int64_t threshold;
  size_t w = 0;
  uint32_t * touched = NULL;
  size_t ntouched = 0;

  while (cap < c->seq_n * 2 + 2) {
    cap *= 2;
  }
  t = c->a->calloc_fn(c->a->ctx, cap, sizeof *t);
  if (!t) {
    c->oom = true;
    return 0;
  }
  count_pairs(c, t, cap);
  for (i = 0; i < cap; ++i) {
    if (t[i].key) {
      ++ncands;
    }
  }
  cands = c->a->calloc_fn(c->a->ctx, ncands + 1, sizeof *cands);
  touched = c->a->calloc_fn(c->a->ctx, 2 * (ncands + 1), sizeof *touched);
  if (!cands || !touched) {
    c->oom = true;
    goto done;
  }
  ncands = 0;
  for (i = 0; i < cap; ++i) {
    uint32_t x;
    uint32_t y;
    int64_t len;
    int64_t benefit;

    if (!t[i].key) {
      continue;
    }
    x = (uint32_t)(t[i].key >> 32) - 1;
    y = (uint32_t)(t[i].key & 0xFFFFFFFFu) - 1;
    if (c->syms[x].depth >= max_depth || c->syms[y].depth >= max_depth) {
      continue;
    }
    len = (int64_t)cur_len(c, x) + cur_len(c, y);
    benefit = (int64_t)t[i].count * (len - CALL_ESTIMATE)
        - (len + 1 + OFFSET_ESTIMATE);
    if (benefit > 0) {
      cands[ncands].key = t[i].key;
      cands[ncands].benefit = benefit;
      ++ncands;
    }
  }
  if (!ncands) {
    goto done;
  }
  qsort(cands, ncands, sizeof *cands, cand_cmp);
  best = cands[0].benefit;
  threshold = best / 4 > 0 ? best / 4 : 1;
  for (i = 0; i < ncands && cands[i].benefit >= threshold; ++i) {
    uint32_t x = (uint32_t)(cands[i].key >> 32) - 1;
    uint32_t y = (uint32_t)(cands[i].key & 0xFFFFFFFFu) - 1;
    uint32_t rule;

    if (c->nrules >= MAX_RULES) {
      break;
    }
    if (x >= lhs_cap || y >= lhs_cap || taken[x] || taken[y]) {
      continue;   // a symbol already in a rule of this round is left for the next
    }
    if (!add_rule(c, x, y, &rule)) {
      goto done;
    }
    lhs[x] = rule;
    taken[x] = taken[y] = 1;
    touched[ntouched++] = x;
    touched[ntouched++] = y;
    ++made;
  }
  if (made) {
    for (i = 0; i < c->seq_n;) {
      uint32_t x = c->seq[i];

      if (i + 1 < c->seq_n && x < lhs_cap && lhs[x] != NONE
          && c->bodies[c->syms[lhs[x]].body_off + 1] == c->seq[i + 1]) {
        c->seq[w++] = lhs[x];
        i += 2;
      }
      else {
        c->seq[w++] = x;
        ++i;
      }
    }
    c->seq_n = w;
  }
  for (i = 0; i < ntouched; ++i) {
    taken[touched[i]] = 0;
    lhs[touched[i]] = NONE;
  }
done:
  c->a->free_fn(c->a->ctx, t);
  c->a->free_fn(c->a->ctx, cands);
  c->a->free_fn(c->a->ctx, touched);
  return made;
}

/* ---- Putting back what does not pay --------------------------------------- */

static void recount(Ctx * c, uint32_t * uses) {
  size_t i;
  size_t k;

  memset(uses, 0, c->nsyms * sizeof *uses);
  for (i = 0; i < c->seq_n; ++i) {
    ++uses[c->seq[i]];
  }
  for (i = 0; i < c->nrules; ++i) {
    const Sym * r = &c->syms[c->rules[i]];

    if (r->dead) {
      continue;
    }
    for (k = 0; k < r->body_len; ++k) {
      ++uses[c->bodies[r->body_off + k]];
    }
  }
}

/** Marks the rules whose references would cost less as the operations themselves. */
static bool prune(Ctx * c, uint32_t * uses) {
  bool any = false;
  size_t pass;

  recount(c, uses);
  for (pass = 0; pass < 6; ++pass) {
    bool changed = false;
    size_t i;

    for (i = c->nrules; i > 0; --i) {
      Sym * r = &c->syms[c->rules[i - 1]];
      int64_t body = 0;
      int64_t saving;
      size_t k;

      if (r->dead) {
        continue;
      }
      for (k = 0; k < r->body_len; ++k) {
        body += cur_len(c, c->bodies[r->body_off + k]);
      }
      saving = (int64_t)uses[c->rules[i - 1]] * (body - (int64_t)CALL_ESTIMATE)
          - (body + 1 + (int64_t)OFFSET_ESTIMATE);
      if (saving > 0) {
        continue;
      }
      r->dead = 1;
      r->len = (uint32_t)body;
      for (k = 0; k < r->body_len; ++k) {
        uses[c->bodies[r->body_off + k]] += uses[c->rules[i - 1]] - 1;
      }
      uses[c->rules[i - 1]] = 0;
      changed = any = true;
    }
    if (!changed) {
      break;
    }
  }
  return any;
}

/**
 * A symbol written out: a dead rule as the symbols it stands for, a live one as itself.
 * With @p out NULL it only counts.
 */
static void expand(const Ctx * c, uint32_t id, uint32_t * out, size_t * n) {
  const Sym * s = &c->syms[id];

  if (s->rule && s->dead) {
    size_t k;

    for (k = 0; k < s->body_len; ++k) {
      expand(c, c->bodies[s->body_off + k], out, n);
    }
    return;
  }
  if (out) {
    out[*n] = id;
  }
  ++*n;
}

/** Rewrites the sequence and the live bodies without the dead rules. */
static bool settle(Ctx * c) {
  size_t i;
  size_t k;
  size_t seq_len = 0;
  size_t body_len = 0;
  uint32_t * ns;
  uint32_t * nb;
  size_t n = 0;
  size_t nbn = 0;

  for (i = 0; i < c->seq_n; ++i) {
    expand(c, c->seq[i], NULL, &seq_len);
  }
  for (i = 0; i < c->nrules; ++i) {
    const Sym * r = &c->syms[c->rules[i]];

    if (!r->dead) {
      for (k = 0; k < r->body_len; ++k) {
        expand(c, c->bodies[r->body_off + k], NULL, &body_len);
      }
    }
  }
  ns = c->a->calloc_fn(c->a->ctx, seq_len + 1, sizeof *ns);
  nb = c->a->calloc_fn(c->a->ctx, body_len + 1, sizeof *nb);
  if (!ns || !nb) {
    c->a->free_fn(c->a->ctx, ns);
    c->a->free_fn(c->a->ctx, nb);
    c->oom = true;
    return false;
  }
  for (i = 0; i < c->seq_n; ++i) {
    expand(c, c->seq[i], ns, &n);
  }
  for (i = 0; i < c->nrules; ++i) {
    Sym * r = &c->syms[c->rules[i]];
    size_t start = nbn;

    if (r->dead) {
      continue;
    }
    for (k = 0; k < r->body_len; ++k) {
      expand(c, c->bodies[r->body_off + k], nb, &nbn);
    }
    r->body_off = (uint32_t)start;
    r->body_len = (uint32_t)(nbn - start);
  }
  c->a->free_fn(c->a->ctx, c->seq);
  c->a->free_fn(c->a->ctx, c->bodies);
  c->seq = ns;
  c->seq_n = n;
  c->seq_cap = seq_len + 1;
  c->bodies = nb;
  c->bodies_n = nbn;
  c->bodies_cap = body_len + 1;
  return true;
}

/* ---- Writing the result --------------------------------------------------- */

/** A subroutine number as a Type 2 integer: one to three bytes. */
static size_t put_number(uint8_t * p, int32_t v) {
  if (v >= -107 && v <= 107) {
    p[0] = (uint8_t)(v + 139);
    return 1;
  }
  if (v >= 108 && v <= 1131) {
    p[0] = (uint8_t)(247 + ((v - 108) >> 8));
    p[1] = (uint8_t)((v - 108) & 0xFF);
    return 2;
  }
  if (v >= -1131 && v <= -108) {
    p[0] = (uint8_t)(251 + ((-v - 108) >> 8));
    p[1] = (uint8_t)((-v - 108) & 0xFF);
    return 2;
  }
  p[0] = 28;
  p[1] = (uint8_t)((uint32_t)v >> 8);
  p[2] = (uint8_t)v;
  return 3;
}

static void put_symbol(Ctx * c, uint32_t id, const uint32_t * index, int32_t bias, GFNT_WBuf * out) {
  const Sym * s = &c->syms[id];

  if (s->rule) {
    uint8_t num[3];
    size_t n = put_number(num, (int32_t)index[id] - bias);

    gfnt_wbuf_bytes(out, num, n);
    gfnt_wbuf_u8(out, 29);
  }
  else {
    gfnt_wbuf_bytes(out, c->pool + s->off, s->len);
  }
}

typedef struct Order {
  uint32_t id;
  uint32_t uses;
} Order;

static int order_cmp(const void * x, const void * y) {
  const Order * p = x;
  const Order * q = y;

  if (p->uses != q->uses) {
    return p->uses < q->uses ? 1 : -1;
  }
  return p->id < q->id ? -1 : p->id > q->id ? 1 : 0;
}

GFNT_Result gfnt_cff_subroutinize(const GFNT_Allocator * a, const uint8_t * cs,
    const size_t * cs_at, const uint8_t * masks, const size_t * mask_at, size_t glyphs,
    uint8_t max_depth, GFNT_WBuf * out_cs, size_t * out_at, GFNT_WBuf * out_subrs,
    size_t ** out_sub_at, size_t * out_count, GFNT_Error * error) {
  Ctx c;
  GFNT_Result result = GFNT_OK;
  uint32_t * lhs = NULL;
  uint8_t * taken = NULL;
  uint32_t * uses = NULL;
  Order * order = NULL;
  uint32_t * index = NULL;
  size_t lhs_cap = 0;
  size_t g;
  size_t i;
  size_t live = 0;
  size_t flat_total = 0;
  size_t new_total = 0;

  *out_count = 0;
  *out_sub_at = NULL;
  if (max_depth == 0 || glyphs == 0) {
    return GFNT_OK;
  }
  memset(&c, 0, sizeof c);
  c.a = a;
  c.barrier = intern(&c, (const uint8_t *)"", 0, true);
  for (g = 0; g < glyphs && !c.oom; ++g) {
    flat_total += cs_at[g + 1] - cs_at[g];
    if (!tokenize(&c, cs + cs_at[g], cs_at[g + 1] - cs_at[g],
            masks ? masks + mask_at[g] : NULL, mask_at[g + 1] - mask_at[g])) {
      if (!c.oom) {
        goto done;   // not understood: leave the charstrings as they are
      }
    }
    push_seq(&c, c.barrier);
    if (c.seq_n > MAX_TOKENS) {
      goto done;   // too many to look through: leave the charstrings whole
    }
  }
  if (c.oom) {
    goto oom;
  }

  // Rounds.
  lhs_cap = 0;
  for (;;) {
    size_t made;

    if (c.nsyms + 4096 > lhs_cap) {
      uint32_t * bigger;
      size_t nc = c.nsyms * 2 + 8192;

      uint8_t * flags = a->calloc_fn(a->ctx, nc, 1);

      bigger = a->calloc_fn(a->ctx, nc, sizeof *bigger);
      if (!bigger || !flags) {
        a->free_fn(a->ctx, bigger);
        a->free_fn(a->ctx, flags);
        goto oom;
      }
      memset(bigger, 0xFF, nc * sizeof *bigger);
      a->free_fn(a->ctx, lhs);
      a->free_fn(a->ctx, taken);
      lhs = bigger;
      taken = flags;
      lhs_cap = nc;
    }
    made = one_round(&c, max_depth, lhs, taken, lhs_cap);
    if (c.oom) {
      goto oom;
    }
    if (!made || c.nrules >= MAX_RULES) {
      break;
    }
  }
  if (!c.nrules) {
    goto done;
  }

  uses = a->calloc_fn(a->ctx, c.nsyms + 1, sizeof *uses);
  if (!uses) {
    goto oom;
  }
  if (prune(&c, uses)) {
    if (!settle(&c)) {
      goto oom;
    }
  }
  recount(&c, uses);

  // The live rules, most used first so that they get the short call numbers.
  order = a->calloc_fn(a->ctx, c.nrules + 1, sizeof *order);
  index = a->calloc_fn(a->ctx, c.nsyms + 1, sizeof *index);
  if (!order || !index) {
    goto oom;
  }
  for (i = 0; i < c.nrules; ++i) {
    const Sym * r = &c.syms[c.rules[i]];

    if (!r->dead) {
      order[live].id = c.rules[i];
      order[live].uses = uses[c.rules[i]];
      ++live;
    }
  }
  if (!live) {
    goto done;
  }
  qsort(order, live, sizeof *order, order_cmp);
  for (i = 0; i < live; ++i) {
    index[order[i].id] = (uint32_t)i;
  }
  {
    const int32_t bias = live < 1240 ? 107 : live < 33900 ? 1131 : 32768;
    size_t k;
    size_t * sub_at = a->calloc_fn(a->ctx, live + 1, sizeof *sub_at);

    if (!sub_at) {
      goto oom;
    }
    for (i = 0; i < live; ++i) {
      const Sym * r = &c.syms[order[i].id];

      sub_at[i] = out_subrs->length;
      for (k = 0; k < r->body_len; ++k) {
        put_symbol(&c, c.bodies[r->body_off + k], index, bias, out_subrs);
      }
      gfnt_wbuf_u8(out_subrs, 11);
    }
    sub_at[live] = out_subrs->length;
    // The charstrings, each glyph up to its barrier.
    g = 0;
    out_at[0] = 0;
    for (i = 0; i < c.seq_n; ++i) {
      if (c.seq[i] == c.barrier) {
        out_at[++g] = out_cs->length;
        continue;
      }
      put_symbol(&c, c.seq[i], index, bias, out_cs);
    }
    if (out_cs->oom || out_subrs->oom) {
      a->free_fn(a->ctx, sub_at);
      goto oom;
    }
    // An INDEX costs a count, an offset size and an offset for each item (at least).
    new_total = out_cs->length + out_subrs->length + 2 * live;
    if (g != glyphs || new_total >= flat_total) {
      a->free_fn(a->ctx, sub_at);
      out_cs->length = 0;
      out_subrs->length = 0;
      goto done;   // no smaller: keep the charstrings whole
    }
    *out_sub_at = sub_at;
    *out_count = live;
  }
  goto done;

oom:
  result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
      "finding repeated charstring sequences");
done:
  a->free_fn(a->ctx, c.pool);
  a->free_fn(a->ctx, c.syms);
  a->free_fn(a->ctx, c.bodies);
  a->free_fn(a->ctx, c.seq);
  a->free_fn(a->ctx, c.slots);
  a->free_fn(a->ctx, c.rules);
  a->free_fn(a->ctx, lhs);
  a->free_fn(a->ctx, taken);
  a->free_fn(a->ctx, uses);
  a->free_fn(a->ctx, order);
  a->free_fn(a->ctx, index);
  return result;
}
