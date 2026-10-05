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
 * A small regular-expression matcher. See nfa.h.
 */

#include <stdlib.h>
#include <string.h>
#include "nfa.h"

enum { RE_EMPTY, RE_SET, RE_SEQ, RE_ALT, RE_STAR, RE_REP_COPY };

typedef struct GFNT_ReNode {
  int kind;
  uint64_t set;
  GFNT_Re a;
  GFNT_Re b;
} GFNT_ReNode;

struct GFNT_Nfa {
  // States are numbered; a state has up to two outgoing transitions: an epsilon
  // pair, or one labelled by a set.
  size_t count;
  uint64_t * label;   // 0 for an epsilon state
  int * next1;        // the labelled target, or the first epsilon target; -1
  int * next2;        // the second epsilon target; -1
  int * accept;       // the token a final state accepts, or -1
  int start;
  size_t words;       // uint64_t words in a state set
};

void gfnt_re_init(GFNT_ReBuilder * b) {
  memset(b, 0, sizeof *b);
}

void gfnt_re_free(GFNT_ReBuilder * b) {
  free(b->nodes);
  memset(b, 0, sizeof *b);
}

static GFNT_Re gfnt_re_add(GFNT_ReBuilder * b, int kind, uint64_t set,
    GFNT_Re a, GFNT_Re c) {
  GFNT_ReNode * node;

  if (b->oom) {
    return 0;
  }
  if (b->count == b->capacity) {
    size_t wanted = b->capacity ? b->capacity * 2 : 64;
    GFNT_ReNode * grown = realloc(b->nodes, wanted * sizeof *b->nodes);

    if (!grown) {
      b->oom = true;
      return 0;
    }
    b->nodes = grown;
    b->capacity = wanted;
  }
  node = &b->nodes[b->count];
  node->kind = kind;
  node->set = set;
  node->a = a;
  node->b = c;
  return (GFNT_Re)b->count++;
}

GFNT_Re gfnt_re_empty(GFNT_ReBuilder * b) {
  return gfnt_re_add(b, RE_EMPTY, 0, 0, 0);
}

GFNT_Re gfnt_re_set(GFNT_ReBuilder * b, uint64_t set) {
  return gfnt_re_add(b, RE_SET, set, 0, 0);
}

GFNT_Re gfnt_re_sym(GFNT_ReBuilder * b, unsigned category) {
  return gfnt_re_set(b, (uint64_t)1 << category);
}

GFNT_Re gfnt_re_seq(GFNT_ReBuilder * b, GFNT_Re a, GFNT_Re c) {
  return gfnt_re_add(b, RE_SEQ, 0, a, c);
}

GFNT_Re gfnt_re_alt(GFNT_ReBuilder * b, GFNT_Re a, GFNT_Re c) {
  return gfnt_re_add(b, RE_ALT, 0, a, c);
}

GFNT_Re gfnt_re_star(GFNT_ReBuilder * b, GFNT_Re a) {
  return gfnt_re_add(b, RE_STAR, 0, a, 0);
}

GFNT_Re gfnt_re_plus(GFNT_ReBuilder * b, GFNT_Re a) {
  return gfnt_re_seq(b, a, gfnt_re_star(b, a));
}

GFNT_Re gfnt_re_opt(GFNT_ReBuilder * b, GFNT_Re a) {
  return gfnt_re_alt(b, a, gfnt_re_empty(b));
}

GFNT_Re gfnt_re_rep(GFNT_ReBuilder * b, GFNT_Re a, unsigned min,
    unsigned max) {
  GFNT_Re result = gfnt_re_empty(b);
  unsigned i;

  for (i = 0; i < min; i++) {
    result = gfnt_re_seq(b, result, a);
  }
  if (max > min) {
    // The optional ones nest, so a repetition costs its length and not its square.
    GFNT_Re tail = gfnt_re_empty(b);

    for (i = min; i < max; i++) {
      tail = gfnt_re_opt(b, gfnt_re_seq(b, a, tail));
    }
    result = gfnt_re_seq(b, result, tail);
  }
  return result;
}

GFNT_Re gfnt_re_seqn(GFNT_ReBuilder * b, size_t n, const GFNT_Re * parts) {
  GFNT_Re result = gfnt_re_empty(b);
  size_t i;

  for (i = 0; i < n; i++) {
    result = gfnt_re_seq(b, result, parts[i]);
  }
  return result;
}

/** A fragment of the automaton under construction: where it starts and ends. */
typedef struct Frag {
  int start;
  int end;
} Frag;

typedef struct Builder {
  GFNT_Nfa * nfa;
  size_t capacity;
  bool oom;
} Builder;

static int nfa_state(Builder * bld, uint64_t label) {
  GFNT_Nfa * n = bld->nfa;

  if (bld->oom) {
    return 0;
  }
  if (n->count == bld->capacity) {
    size_t wanted = bld->capacity ? bld->capacity * 2 : 256;
    uint64_t * l = realloc(n->label, wanted * sizeof *l);
    int * a;
    int * b;
    int * c;

    if (!l) {
      bld->oom = true;
      return 0;
    }
    n->label = l;
    a = realloc(n->next1, wanted * sizeof *a);
    if (!a) {
      bld->oom = true;
      return 0;
    }
    n->next1 = a;
    b = realloc(n->next2, wanted * sizeof *b);
    if (!b) {
      bld->oom = true;
      return 0;
    }
    n->next2 = b;
    c = realloc(n->accept, wanted * sizeof *c);
    if (!c) {
      bld->oom = true;
      return 0;
    }
    n->accept = c;
    bld->capacity = wanted;
  }
  n->label[n->count] = label;
  n->next1[n->count] = -1;
  n->next2[n->count] = -1;
  n->accept[n->count] = -1;
  return (int)n->count++;
}

/** Add an epsilon edge from @p from to @p to, in a free slot of @p from. */
static void nfa_eps(Builder * bld, int from, int to) {
  GFNT_Nfa * n = bld->nfa;

  if (bld->oom) {
    return;
  }
  if (n->next1[from] < 0) {
    n->next1[from] = to;
  }
  else {
    n->next2[from] = to;
  }
}

static Frag nfa_build(Builder * bld, const GFNT_ReNode * nodes, GFNT_Re re) {
  const GFNT_ReNode * node = &nodes[re];
  Frag f;
  Frag g;
  int s;
  int e;

  switch (node->kind) {
    case RE_EMPTY:
      s = nfa_state(bld, 0);
      f.start = s;
      f.end = s;
      return f;
    case RE_SET:
      s = nfa_state(bld, node->set);
      e = nfa_state(bld, 0);
      if (!bld->oom) {
        bld->nfa->next1[s] = e;
      }
      f.start = s;
      f.end = e;
      return f;
    case RE_SEQ:
      f = nfa_build(bld, nodes, node->a);
      g = nfa_build(bld, nodes, node->b);
      nfa_eps(bld, f.end, g.start);
      f.end = g.end;
      return f;
    case RE_ALT:
      f = nfa_build(bld, nodes, node->a);
      g = nfa_build(bld, nodes, node->b);
      s = nfa_state(bld, 0);
      e = nfa_state(bld, 0);
      nfa_eps(bld, s, f.start);
      nfa_eps(bld, s, g.start);
      nfa_eps(bld, f.end, e);
      nfa_eps(bld, g.end, e);
      f.start = s;
      f.end = e;
      return f;
    case RE_STAR:
      f = nfa_build(bld, nodes, node->a);
      s = nfa_state(bld, 0);
      e = nfa_state(bld, 0);
      nfa_eps(bld, s, f.start);
      nfa_eps(bld, s, e);
      nfa_eps(bld, f.end, f.start);
      nfa_eps(bld, f.end, e);
      f.start = s;
      f.end = e;
      return f;
    default:
      break;
  }
  f.start = 0;
  f.end = 0;
  return f;
}

GFNT_Nfa * gfnt_nfa_compile(const GFNT_ReBuilder * b, const GFNT_Re * roots,
    size_t count) {
  Builder bld;
  GFNT_Nfa * nfa;
  int root_start;
  size_t i;
  int cursor = -1;

  if (b->oom) {
    return NULL;
  }
  nfa = calloc(1, sizeof *nfa);
  if (!nfa) {
    return NULL;
  }
  bld.nfa = nfa;
  bld.capacity = 0;
  bld.oom = false;
  // A chain of epsilon states, one per root, so that the start state reaches each.
  root_start = nfa_state(&bld, 0);
  cursor = root_start;
  for (i = 0; i < count && !bld.oom; i++) {
    Frag f = nfa_build(&bld, b->nodes, roots[i]);
    int split;

    // `cursor` branches to this root and to the next splitter.
    split = nfa_state(&bld, 0);
    nfa_eps(&bld, cursor, f.start);
    nfa_eps(&bld, cursor, split);
    if (!bld.oom) {
      nfa->accept[f.end] = (int)i;
    }
    cursor = split;
  }
  if (bld.oom) {
    gfnt_nfa_free(nfa);
    return NULL;
  }
  nfa->start = root_start;
  nfa->words = (nfa->count + 63) / 64;
  return nfa;
}

void gfnt_nfa_free(GFNT_Nfa * nfa) {
  if (!nfa) {
    return;
  }
  free(nfa->label);
  free(nfa->next1);
  free(nfa->next2);
  free(nfa->accept);
  free(nfa);
}

/** Add a state and everything reachable from it by epsilon edges to a set. */
static void nfa_closure(const GFNT_Nfa * n, uint64_t * set, int * stack,
    int state) {
  size_t top = 0;

  stack[top++] = state;
  while (top) {
    int s = stack[--top];

    if (set[s >> 6] & ((uint64_t)1 << (s & 63))) {
      continue;
    }
    set[s >> 6] |= (uint64_t)1 << (s & 63);
    if (n->label[s] == 0) {
      if (n->next1[s] >= 0) {
        stack[top++] = n->next1[s];
      }
      if (n->next2[s] >= 0) {
        stack[top++] = n->next2[s];
      }
    }
  }
}

size_t gfnt_nfa_match(const GFNT_Nfa * nfa, const uint8_t * categories,
    size_t length, size_t start, int * token) {
  uint64_t * cur;
  uint64_t * next;
  int * stack;
  size_t best = 0;
  int best_token = -1;
  size_t pos;
  size_t w = nfa->words;

  cur = calloc(2 * w, sizeof *cur);
  stack = malloc((nfa->count + 1) * sizeof *stack);
  if (!cur || !stack) {
    free(cur);
    free(stack);
    return 0;
  }
  next = cur + w;
  nfa_closure(nfa, cur, stack, nfa->start);
  for (pos = start;; pos++) {
    size_t s;
    int found = -1;
    bool any = false;

    // Does any state in the set accept? The first token wins a tie.
    for (s = 0; s < nfa->count; s++) {
      if ((cur[s >> 6] >> (s & 63)) & 1) {
        any = true;
        if (nfa->accept[s] >= 0 && (found < 0 || nfa->accept[s] < found)) {
          found = nfa->accept[s];
        }
      }
    }
    if (found >= 0 && pos > start) {
      best = pos - start;
      best_token = found;
    }
    if (!any || pos >= length) {
      break;
    }
    memset(next, 0, w * sizeof *next);
    for (s = 0; s < nfa->count; s++) {
      if (((cur[s >> 6] >> (s & 63)) & 1) && nfa->label[s]
          && ((nfa->label[s] >> categories[pos]) & 1)
          && nfa->next1[s] >= 0) {
        nfa_closure(nfa, next, stack, nfa->next1[s]);
      }
    }
    {
      uint64_t * t = cur;

      cur = next;
      next = t;
    }
  }
  free(stack);
  free(cur < next ? cur : next);
  if (token) {
    *token = best_token;
  }
  return best;
}
