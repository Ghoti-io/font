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
 * A small regular-expression matcher over a text's character categories.
 *
 * The scripts that are shaped a syllable at a time (the Universal Shaping Engine's,
 * Indic, Khmer, Myanmar) define a syllable by a regular expression over the
 * categories of the characters, and take the *longest* syllable that begins where
 * the last one ended, the first-listed when two kinds are as long. That is what a
 * scanner generator makes of a list of expressions, and it is what this does, by
 * running a Thompson automaton over the run: the expressions are built once from
 * the grammar's own shapes - a set of categories, a sequence, an alternative, a
 * repetition - and a match is a walk that remembers the last place any of them
 * would have stopped.
 *
 * Categories are numbers below 64, so a set of them is one `uint64_t`.
 */

#ifndef GHOTI_IO_GFNT_NFA_H
#define GHOTI_IO_GFNT_NFA_H

#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** An expression under construction: an index into its builder. */
typedef int GFNT_Re;

typedef struct GFNT_Nfa GFNT_Nfa;

/** The expressions of one grammar, and what they are made of. */
typedef struct GFNT_ReBuilder {
  struct GFNT_ReNode * nodes;
  size_t count;
  size_t capacity;
  bool oom;
} GFNT_ReBuilder;

void gfnt_re_init(GFNT_ReBuilder * b);
void gfnt_re_free(GFNT_ReBuilder * b);

/** Nothing: matches the empty string. */
GFNT_Re gfnt_re_empty(GFNT_ReBuilder * b);
/** One character whose category is in @p set (bit n for category n). */
GFNT_Re gfnt_re_set(GFNT_ReBuilder * b, uint64_t set);
/** One of the categories listed. */
GFNT_Re gfnt_re_sym(GFNT_ReBuilder * b, unsigned category);
GFNT_Re gfnt_re_seq(GFNT_ReBuilder * b, GFNT_Re a, GFNT_Re c);
GFNT_Re gfnt_re_alt(GFNT_ReBuilder * b, GFNT_Re a, GFNT_Re c);
GFNT_Re gfnt_re_star(GFNT_ReBuilder * b, GFNT_Re a);
GFNT_Re gfnt_re_plus(GFNT_ReBuilder * b, GFNT_Re a);
GFNT_Re gfnt_re_opt(GFNT_ReBuilder * b, GFNT_Re a);
/** @p a repeated between @p min and @p max times, inclusive. */
GFNT_Re gfnt_re_rep(GFNT_ReBuilder * b, GFNT_Re a, unsigned min, unsigned max);

/** Sequence of several. */
GFNT_Re gfnt_re_seqn(GFNT_ReBuilder * b, size_t n, const GFNT_Re * parts);

/**
 * Compile a grammar: @p roots are tried in the order given. Token @p i is the index
 * in the list. Returns NULL on no memory.
 */
GFNT_Nfa * gfnt_nfa_compile(const GFNT_ReBuilder * b, const GFNT_Re * roots,
    size_t count);
void gfnt_nfa_free(GFNT_Nfa * nfa);

/**
 * The longest match starting at @p start, and which expression made it.
 *
 * @param categories The category of each character of the run.
 * @param token Receives the first of the expressions that matched that long.
 * @return How many characters, or 0 if none of the expressions matches even one.
 */
size_t gfnt_nfa_match(const GFNT_Nfa * nfa, const uint8_t * categories,
    size_t length, size_t start, int * token);

#endif
