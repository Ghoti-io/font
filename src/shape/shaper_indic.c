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
 * The Indic shaper: Devanagari, Bengali, Gurmukhi, Gujarati, Oriya, Tamil,
 * Telugu, Kannada, Malayalam and Sinhala.
 *
 * The run is cut into syllables by a grammar over the characters' categories; each
 * syllable has a base consonant found in it, the characters round the base are
 * put in the order the font's substitutions expect (the left-hand vowel, the
 * reph), the font's basic features are run one at a time over the syllable, and
 * what they have made is put in its final order before the remaining features run
 * over the whole run. The specification is Microsoft's "Developing OpenType
 * Fonts for Devanagari" and its siblings, in the form HarfBuzz gives them: where
 * the two differ, the differential against HarfBuzz decides.
 */

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/norm.h>
#include <string.h>
#include "nfa.h"
#include "plan.h"
#include "uprops.h"
#include "vowel_constraints.h"

// The categories of an Indic character.
enum {
  IC_X, IC_C, IC_V, IC_N, IC_H, IC_ZWNJ, IC_ZWJ, IC_M, IC_SM, IC_A, IC_VD,
  IC_PLACEHOLDER, IC_DOTTEDCIRCLE, IC_RS, IC_COENG, IC_REPHA, IC_RA, IC_CM,
  IC_SYMBOL, IC_CS, IC_GRAMMAR_MA, IC_COUNT
};

// Where in the syllable a character wants to be, in the order the syllable is
// sorted into.
enum {
  IP_START, IP_RA_TO_BECOME_REPH, IP_PRE_M, IP_PRE_C, IP_BASE_C, IP_AFTER_MAIN,
  IP_ABOVE_C, IP_BEFORE_SUB, IP_BELOW_C, IP_AFTER_SUB, IP_BEFORE_POST,
  IP_POST_C, IP_AFTER_POST, IP_FINAL_C, IP_SMVD, IP_END
};

// The syllable kinds, in the order the grammar lists them: that order breaks a tie.
enum {
  IS_CONSONANT, IS_VOWEL, IS_STANDALONE, IS_SYMBOL, IS_BROKEN, IS_NON_INDIC,
  IS_COUNT
};

#define FLAG(c) (1u << (c))
#define JOINER_FLAGS (FLAG(IC_ZWJ) | FLAG(IC_ZWNJ))
#define CONSONANT_FLAGS (FLAG(IC_C) | FLAG(IC_CS) | FLAG(IC_RA) | FLAG(IC_CM) \
    | FLAG(IC_V) | FLAG(IC_PLACEHOLDER) | FLAG(IC_DOTTEDCIRCLE))
#define HALANT_OR_COENG_FLAGS (FLAG(IC_H) | FLAG(IC_COENG))
#define MEDIAL_FLAGS FLAG(IC_CM)

// The features, in the order the plan asks for them.
enum {
  IF_NUKT, IF_AKHN, IF_RPHF, IF_RKRF, IF_PREF, IF_BLWF, IF_ABVF, IF_HALF,
  IF_PSTF, IF_VATU, IF_CJCT, IF_INIT, IF_PRES, IF_ABVS, IF_BLWS, IF_PSTS,
  IF_HALN, IF_COUNT, IF_BASIC = IF_CJCT + 1
};

typedef struct IndicFeature {
  const char * tag;
  bool global;
} IndicFeature;

static const IndicFeature indic_features[IF_COUNT] = {
  {"nukt", true}, {"akhn", true}, {"rphf", false}, {"rkrf", true},
  {"pref", false}, {"blwf", false}, {"abvf", false}, {"half", false},
  {"pstf", false}, {"vatu", true}, {"cjct", true}, {"init", false},
  {"pres", true}, {"abvs", true}, {"blws", true}, {"psts", true},
  {"haln", true},
};

enum { BASE_LAST, BASE_LAST_SINHALA };
enum { REPH_BEFORE_POST, REPH_AFTER_MAIN, REPH_AFTER_SUB, REPH_BEFORE_SUB,
       REPH_AFTER_POST };
enum { REPH_IMPLICIT, REPH_EXPLICIT, REPH_LOG_REPHA };
enum { BLWF_PRE_AND_POST, BLWF_POST_ONLY };

typedef struct IndicConfig {
  const char * script;
  bool has_old_spec;
  uint32_t virama;
  int base_pos;
  int reph_pos;
  int reph_mode;
  int blwf_mode;
} IndicConfig;

static const IndicConfig indic_configs[] = {
  {"", false, 0, BASE_LAST, REPH_BEFORE_POST, REPH_IMPLICIT, BLWF_PRE_AND_POST},
  {"deva", true, 0x094D, BASE_LAST, REPH_BEFORE_POST, REPH_IMPLICIT,
      BLWF_PRE_AND_POST},
  {"beng", true, 0x09CD, BASE_LAST, REPH_AFTER_SUB, REPH_IMPLICIT,
      BLWF_PRE_AND_POST},
  {"guru", true, 0x0A4D, BASE_LAST, REPH_BEFORE_SUB, REPH_IMPLICIT,
      BLWF_PRE_AND_POST},
  {"gujr", true, 0x0ACD, BASE_LAST, REPH_BEFORE_POST, REPH_IMPLICIT,
      BLWF_PRE_AND_POST},
  {"orya", true, 0x0B4D, BASE_LAST, REPH_AFTER_MAIN, REPH_IMPLICIT,
      BLWF_PRE_AND_POST},
  {"taml", true, 0x0BCD, BASE_LAST, REPH_AFTER_POST, REPH_IMPLICIT,
      BLWF_PRE_AND_POST},
  {"telu", true, 0x0C4D, BASE_LAST, REPH_AFTER_POST, REPH_EXPLICIT,
      BLWF_POST_ONLY},
  {"knda", true, 0x0CCD, BASE_LAST, REPH_AFTER_POST, REPH_IMPLICIT,
      BLWF_POST_ONLY},
  {"mlym", true, 0x0D4D, BASE_LAST, REPH_AFTER_MAIN, REPH_LOG_REPHA,
      BLWF_PRE_AND_POST},
  {"sinh", false, 0x0DCA, BASE_LAST_SINHALA, REPH_AFTER_POST, REPH_EXPLICIT,
      BLWF_PRE_AND_POST},
};

typedef struct IndicData {
  const IndicConfig * config;
  bool is_old_spec;
  bool zero_context;
  bool malayalam;
  bool tamil;
  uint32_t mask[IF_COUNT];
  GFNT_Nfa * grammar;
} IndicData;

/* --- categories --------------------------------------------------------- */

static bool indic_is_ra(uint32_t u) {
  switch (u) {
    case 0x0930: case 0x09B0: case 0x09F0: case 0x0A30: case 0x0AB0:
    case 0x0B30: case 0x0BB0: case 0x0C30: case 0x0CB0: case 0x0D30:
    case 0x0DBB:
      return true;
    default:
      return false;
  }
}

static int indic_category_of(GUNI_IndicSyllabicCategory isc) {
  switch (isc) {
    case GUNI_INSC_AVAGRAHA: return IC_SYMBOL;
    case GUNI_INSC_BINDU: return IC_SM;
    case GUNI_INSC_BRAHMI_JOINING_NUMBER: return IC_PLACEHOLDER;
    case GUNI_INSC_CANTILLATION_MARK: return IC_A;
    case GUNI_INSC_CONSONANT: return IC_C;
    case GUNI_INSC_CONSONANT_DEAD: return IC_C;
    case GUNI_INSC_CONSONANT_FINAL: return IC_CM;
    case GUNI_INSC_CONSONANT_HEAD_LETTER: return IC_C;
    case GUNI_INSC_CONSONANT_INITIAL_POSTFIXED: return IC_C;
    case GUNI_INSC_CONSONANT_KILLER: return IC_M;
    case GUNI_INSC_CONSONANT_MEDIAL: return IC_CM;
    case GUNI_INSC_CONSONANT_PLACEHOLDER: return IC_PLACEHOLDER;
    case GUNI_INSC_CONSONANT_PRECEDING_REPHA: return IC_REPHA;
    case GUNI_INSC_CONSONANT_PREFIXED: return IC_X;
    case GUNI_INSC_CONSONANT_SUBJOINED: return IC_CM;
    case GUNI_INSC_CONSONANT_SUCCEEDING_REPHA: return IC_CM;
    case GUNI_INSC_CONSONANT_WITH_STACKER: return IC_CS;
    case GUNI_INSC_GEMINATION_MARK: return IC_SM;
    case GUNI_INSC_INVISIBLE_STACKER: return IC_COENG;
    case GUNI_INSC_JOINER: return IC_ZWJ;
    case GUNI_INSC_MODIFYING_LETTER: return IC_X;
    case GUNI_INSC_NON_JOINER: return IC_ZWNJ;
    case GUNI_INSC_NUKTA: return IC_N;
    case GUNI_INSC_NUMBER: return IC_PLACEHOLDER;
    case GUNI_INSC_NUMBER_JOINER: return IC_PLACEHOLDER;
    case GUNI_INSC_PURE_KILLER: return IC_M;
    case GUNI_INSC_REGISTER_SHIFTER: return IC_RS;
    case GUNI_INSC_REORDERING_KILLER: return IC_M;
    case GUNI_INSC_SYLLABLE_MODIFIER: return IC_SM;
    case GUNI_INSC_TONE_LETTER: return IC_X;
    case GUNI_INSC_TONE_MARK: return IC_N;
    case GUNI_INSC_VIRAMA: return IC_H;
    case GUNI_INSC_VISARGA: return IC_SM;
    case GUNI_INSC_VOWEL: return IC_V;
    case GUNI_INSC_VOWEL_DEPENDENT: return IC_M;
    case GUNI_INSC_VOWEL_INDEPENDENT: return IC_V;
    default: return IC_X;
  }
}

/** The side of its base a character is written on, from its positional category. */
static int indic_side_of(GUNI_IndicPositionalCategory ipc) {
  switch (ipc) {
    case GUNI_INPC_LEFT: return IP_PRE_C;
    case GUNI_INPC_TOP: return IP_ABOVE_C;
    case GUNI_INPC_BOTTOM: return IP_BELOW_C;
    case GUNI_INPC_RIGHT: return IP_POST_C;
    // These resolve to the position of the last part of the split sequence.
    case GUNI_INPC_BOTTOM_AND_RIGHT: return IP_POST_C;
    case GUNI_INPC_BOTTOM_AND_LEFT: return IP_BELOW_C;
    case GUNI_INPC_LEFT_AND_RIGHT: return IP_POST_C;
    case GUNI_INPC_TOP_AND_BOTTOM: return IP_BELOW_C;
    case GUNI_INPC_TOP_AND_BOTTOM_AND_LEFT: return IP_BELOW_C;
    case GUNI_INPC_TOP_AND_BOTTOM_AND_RIGHT: return IP_POST_C;
    case GUNI_INPC_TOP_AND_LEFT: return IP_ABOVE_C;
    case GUNI_INPC_TOP_AND_LEFT_AND_RIGHT: return IP_POST_C;
    case GUNI_INPC_TOP_AND_RIGHT: return IP_POST_C;
    case GUNI_INPC_OVERSTRUCK: return IP_AFTER_MAIN;
    case GUNI_INPC_VISUAL_ORDER_LEFT: return IP_PRE_M;
    default: return IP_END;
  }
}

#define IS_BLOCK(u, base) (((u) & ~0x7Fu) == (base))

/** Where a matra on this side goes, which depends on the script it is in. */
static int indic_matra_position(uint32_t u, int side) {
  bool deva = IS_BLOCK(u, 0x0900), beng = IS_BLOCK(u, 0x0980);
  bool guru = IS_BLOCK(u, 0x0A00), gujr = IS_BLOCK(u, 0x0A80);
  bool orya = IS_BLOCK(u, 0x0B00), taml = IS_BLOCK(u, 0x0B80);
  bool telu = IS_BLOCK(u, 0x0C00), knda = IS_BLOCK(u, 0x0C80);
  bool mlym = IS_BLOCK(u, 0x0D00), sinh = IS_BLOCK(u, 0x0D80);

  switch (side) {
    case IP_PRE_C:
      return IP_PRE_M;
    case IP_POST_C:
      return deva ? IP_AFTER_SUB : beng ? IP_AFTER_POST : guru ? IP_AFTER_POST
          : gujr ? IP_AFTER_POST : orya ? IP_AFTER_POST : taml ? IP_AFTER_POST
          : telu ? (u <= 0x0C42 ? IP_BEFORE_SUB : IP_AFTER_SUB)
          : knda ? (u < 0x0CC3 || u > 0x0CD6 ? IP_BEFORE_SUB : IP_AFTER_SUB)
          : mlym ? IP_AFTER_POST : sinh ? IP_AFTER_SUB : IP_AFTER_SUB;
    case IP_ABOVE_C:
      // Bengali and Malayalam have no top matras.
      return deva ? IP_AFTER_SUB : guru ? IP_AFTER_POST : gujr ? IP_AFTER_SUB
          : orya ? IP_AFTER_MAIN : taml ? IP_AFTER_SUB : telu ? IP_BEFORE_SUB
          : knda ? IP_BEFORE_SUB : sinh ? IP_AFTER_SUB : IP_AFTER_SUB;
    case IP_BELOW_C:
      return deva ? IP_AFTER_SUB : beng ? IP_AFTER_SUB : guru ? IP_AFTER_POST
          : gujr ? IP_AFTER_POST : orya ? IP_AFTER_SUB : taml ? IP_AFTER_POST
          : telu ? IP_BEFORE_SUB : knda ? IP_BEFORE_SUB
          : mlym ? IP_AFTER_POST : sinh ? IP_AFTER_SUB : IP_AFTER_SUB;
    default:
      return side;
  }
}

static bool indic_in_table(uint32_t u) {
  return (u >= 0x0900 && u <= 0x0DFF) || (u >= 0x1CD0 && u <= 0x1CFF)
      || (u >= 0xA8E0 && u <= 0xA8FF);
}

static void indic_set_properties(GFNT_LInfo * info) {
  uint32_t u = info->unicode;
  int cat = IC_X;
  int pos = IP_END;

  if (indic_in_table(u)) {
    cat = indic_category_of(guni_indic_syllabic_category(u));
    pos = indic_side_of(guni_indic_positional_category(u));
  }
  else if (u == 0x00A0 || u == 0x2010 || u == 0x2011) {
    cat = IC_PLACEHOLDER;
  }
  // Characters newer than HarfBuzz's table, or that it sorts differently from the
  // Unicode data, found by trying each as every category against it.
  switch (u) {
    case 0x0B55:   // The Oriya overline: a nukta.
      cat = IC_N;
      break;
    case 0x0953:   // The Devanagari grave and acute accents: syllable modifiers.
    case 0x0954:
      cat = IC_SM;
      break;
    case 0x09FC:   // The Bengali vedic anusvara and the Kannada and Malayalam
    case 0x0C80:   // signs like it stand where a base would.
      cat = IC_PLACEHOLDER;
      break;
    case 0x0D04:
      cat = IC_C;
      break;
    default:
      break;
  }
  if (u == 0x0A51) {
    cat = IC_M;   // The Gurmukhi udaat: a vowel sign in HarfBuzz's table.
  }
  if (u == 0x0AFB) {
    cat = IC_N;   // The Gujarati shadda: HarfBuzz's table has it as a nukta.
  }
  if (u == 0x17D2) {
    cat = IC_COENG;
  }
  else if (u == 0x200C) {
    cat = IC_ZWNJ;
  }
  else if (u == 0x200D) {
    cat = IC_ZWJ;
  }
  else if (u == 0x25CC) {
    cat = IC_DOTTEDCIRCLE;
  }
  if (FLAG(cat) & CONSONANT_FLAGS) {
    pos = IP_BASE_C;
    if (indic_is_ra(u)) {
      cat = IC_RA;
    }
  }
  else if (cat == IC_M) {
    pos = indic_matra_position(u, pos);
  }
  else if (FLAG(cat) & (FLAG(IC_SM) | FLAG(IC_VD) | FLAG(IC_A)
      | FLAG(IC_SYMBOL))) {
    pos = IP_SMVD;
  }
  if (u == 0x0A51) {
    pos = IP_BELOW_C;   // The udaat is written below, though the Unicode data has no position for it.
  }
  if (u == 0x0B01) {
    pos = IP_BEFORE_SUB;   // The Oriya bindu is before the subjoined, in the spec.
  }
  info->category = (uint8_t)cat;
  info->position = (uint8_t)pos;
}

/* --- the syllable grammar ----------------------------------------------- */

#define SYM(c) gfnt_re_sym(&b, (c))
#define SEQ(x, y) gfnt_re_seq(&b, (x), (y))
#define ALT(x, y) gfnt_re_alt(&b, (x), (y))
#define STAR(x) gfnt_re_star(&b, (x))
#define OPT(x) gfnt_re_opt(&b, (x))
#define PLUS(x) gfnt_re_plus(&b, (x))
#define SET(m) gfnt_re_set(&b, (m))
#define REP(x, lo, hi) gfnt_re_rep(&b, (x), (lo), (hi))
#define BIT(c) ((uint64_t)1 << (c))

static GFNT_Nfa * indic_grammar(void) {
  GFNT_ReBuilder b;
  GFNT_Nfa * nfa;
  GFNT_Re c, n, z, reph, cn, forced_rakar, symbol, matra_group, syllable_tail;
  GFNT_Re place_holder, halant_group, final_halant_group, medial_group;
  GFNT_Re halant_or_matra_group, rest, repha_or_cs;
  GFNT_Re roots[IS_COUNT];

  gfnt_re_init(&b);
  c = SET(BIT(IC_C) | BIT(IC_RA));
  n = SEQ(OPT(SEQ(OPT(SYM(IC_ZWNJ)), SYM(IC_RS))),
      OPT(SEQ(SYM(IC_N), OPT(SYM(IC_N)))));
  z = SET(BIT(IC_ZWJ) | BIT(IC_ZWNJ));
  reph = ALT(SEQ(SYM(IC_RA), SYM(IC_H)), SYM(IC_REPHA));
  cn = SEQ(SEQ(c, OPT(SYM(IC_ZWJ))), n);
  forced_rakar = SEQ(SEQ(SYM(IC_ZWJ), SYM(IC_H)), SEQ(SYM(IC_ZWJ), SYM(IC_RA)));
  symbol = SEQ(SYM(IC_SYMBOL), OPT(SYM(IC_N)));
  matra_group = SEQ(SEQ(REP(z, 0, 3), SET(BIT(IC_M) | BIT(IC_GRAMMAR_MA))), SEQ(OPT(SYM(IC_N)),
      OPT(ALT(SYM(IC_H), forced_rakar))));
  syllable_tail = SEQ(OPT(SEQ(SEQ(SEQ(OPT(z), SYM(IC_SM)),
      SEQ(OPT(SYM(IC_SM)), OPT(SYM(IC_ZWNJ)))), OPT(SYM(IC_GRAMMAR_MA)))),
      STAR(SET(BIT(IC_A) | BIT(IC_VD))));
  place_holder = SET(BIT(IC_PLACEHOLDER) | BIT(IC_DOTTEDCIRCLE));
  halant_group = SEQ(OPT(z), SEQ(SYM(IC_H), OPT(SEQ(SYM(IC_ZWJ), OPT(SYM(IC_N))))));
  final_halant_group = ALT(halant_group, SEQ(SYM(IC_H), SYM(IC_ZWNJ)));
  medial_group = OPT(SYM(IC_CM));
  halant_or_matra_group = ALT(final_halant_group, STAR(matra_group));
  repha_or_cs = SET(BIT(IC_REPHA) | BIT(IC_CS));
  rest = SEQ(SEQ(medial_group, halant_or_matra_group), syllable_tail);

  roots[IS_CONSONANT] = SEQ(SEQ(OPT(repha_or_cs),
      STAR(SEQ(cn, halant_group))), SEQ(cn, rest));
  roots[IS_VOWEL] = SEQ(SEQ(OPT(reph), SEQ(SYM(IC_V), n)),
      ALT(SYM(IC_ZWJ), SEQ(STAR(SEQ(halant_group, cn)), rest)));
  roots[IS_STANDALONE] = SEQ(ALT(SEQ(OPT(repha_or_cs), place_holder),
      SEQ(OPT(reph), SYM(IC_DOTTEDCIRCLE))),
      SEQ(n, SEQ(STAR(SEQ(halant_group, cn)), rest)));
  roots[IS_SYMBOL] = SEQ(symbol, syllable_tail);
  roots[IS_BROKEN] = SEQ(SEQ(OPT(reph), n),
      SEQ(STAR(SEQ(halant_group, cn)), rest));
  roots[IS_NON_INDIC] = SET(~(uint64_t)0);
  nfa = gfnt_nfa_compile(&b, roots, IS_COUNT);
  gfnt_re_free(&b);
  return nfa;
}

static void indic_find_syllables(GFNT_ShapeCtx * ctx) {
  const IndicData * data = ctx->plan->shaper_data;
  GFNT_LBuffer * buf = ctx->buf;
  uint8_t * cats;
  size_t pos = 0;
  unsigned serial = 1;
  size_t i;

  if (!buf->len) {
    return;
  }
  cats = ctx->allocator->calloc_fn(ctx->allocator->ctx, buf->len, 1);
  if (!cats) {
    ctx->oom = true;
    return;
  }
  for (i = 0; i < buf->len; i++) {
    cats[i] = buf->info[i].category;
    // HarfBuzz lets the Gurmukhi vowel sign II follow a syllable modifier
    // without a circle of its own, so for the grammar it is a matra that is
    // also allowed in the tail.
    if (cats[i] == IC_M && buf->info[i].unicode == 0x0A40) {
      if (i && cats[i - 1] == IC_SM) {
        // In the tail it stays where it is, as a mark that is not reordered.
        buf->info[i].category = IC_A;
        buf->info[i].position = IP_END;
      }
      cats[i] = IC_GRAMMAR_MA;
    }
  }
  while (pos < buf->len) {
    int token = IS_NON_INDIC;
    size_t length = gfnt_nfa_match(data->grammar, cats, buf->len, pos, &token);

    if (!length) {
      length = 1;
      token = IS_NON_INDIC;
    }
    for (i = pos; i < pos + length; i++) {
      buf->info[i].syllable = (uint8_t)((serial << 4) | (unsigned)token);
    }
    serial++;
    if (serial == 16) {
      serial = 1;
    }
    pos += length;
  }
  ctx->allocator->free_fn(ctx->allocator->ctx, cats);
}

/* --- reordering --------------------------------------------------------- */

static bool indic_is_one_of(const GFNT_LInfo * info, uint32_t flags) {
  // If it ligated, all bets are off.
  if (info->props & GFNT_PROP_LIGATED) {
    return false;
  }
  return (FLAG(info->category) & flags) != 0;
}

static bool indic_is_joiner(const GFNT_LInfo * info) {
  return indic_is_one_of(info, JOINER_FLAGS);
}

static bool indic_is_consonant(const GFNT_LInfo * info) {
  return indic_is_one_of(info, CONSONANT_FLAGS);
}

static bool indic_is_halant(const GFNT_LInfo * info) {
  return indic_is_one_of(info, FLAG(IC_H));
}

/** Take the glyph at @p from and put it at @p to, the ones between moving over. */
static void indic_move(GFNT_LBuffer * buf, size_t from, size_t to) {
  GFNT_LInfo t = buf->info[from];
  GFNT_LPos p = buf->pos[from];
  size_t k;

  if (from < to) {
    for (k = from; k < to; k++) {
      buf->info[k] = buf->info[k + 1];
      buf->pos[k] = buf->pos[k + 1];
    }
  }
  else {
    for (k = from; k > to; k--) {
      buf->info[k] = buf->info[k - 1];
      buf->pos[k] = buf->pos[k - 1];
    }
  }
  buf->info[to] = t;
  buf->pos[to] = p;
}

static bool indic_virama_glyph(const GFNT_ShapeCtx * ctx, const IndicData * data,
    uint32_t * glyph) {
  return gfnt_face_glyph_for_codepoint(ctx->face, data->config->virama, glyph,
      NULL) == GFNT_OK && *glyph != 0;
}

static bool indic_would(const GFNT_ShapeCtx * ctx, const IndicData * data,
    int feature, const uint32_t * glyphs, size_t count) {
  const char * t = indic_features[feature].tag;

  return gfnt_plan_would_substitute(ctx, GFNT_TAG(t[0], t[1], t[2], t[3]),
      glyphs, count, data->zero_context);
}

/** What the font does with a consonant under a virama: below, post, or neither. */
static int indic_consonant_position(const GFNT_ShapeCtx * ctx,
    const IndicData * data, uint32_t consonant, uint32_t virama) {
  // In old-spec fonts the order of glyphs is consonant, virama; in new-spec
  // fonts, virama, consonant. Some fonts copied the old lookups to the new
  // without changing them, so both are asked.
  uint32_t glyphs[3] = {virama, consonant, virama};

  if (indic_would(ctx, data, IF_BLWF, glyphs, 2)
      || indic_would(ctx, data, IF_BLWF, glyphs + 1, 2)) {
    return IP_BELOW_C;
  }
  if (indic_would(ctx, data, IF_PSTF, glyphs, 2)
      || indic_would(ctx, data, IF_PSTF, glyphs + 1, 2)) {
    return IP_POST_C;
  }
  if (indic_would(ctx, data, IF_PREF, glyphs, 2)
      || indic_would(ctx, data, IF_PREF, glyphs + 1, 2)) {
    return IP_POST_C;
  }
  // A `vatu` form counts in an old-spec font, and in a new-spec one for Oriya,
  // Telugu and Kannada only (found with a ligature for every pair of glyphs under
  // `vatu`, in every script): it puts the consonant below the base.
  if ((data->is_old_spec || data->config->virama == 0x0B4D
          || data->config->virama == 0x0C4D || data->config->virama == 0x0CCD)
      && (indic_would(ctx, data, IF_VATU, glyphs, 2)
          || indic_would(ctx, data, IF_VATU, glyphs + 1, 2))) {
    return IP_BELOW_C;
  }
  return IP_BASE_C;
}

static void indic_update_consonant_positions(GFNT_ShapeCtx * ctx,
    const IndicData * data) {
  GFNT_LBuffer * buf = ctx->buf;
  uint32_t virama;
  size_t i;

  if (data->config->base_pos != BASE_LAST
      || !indic_virama_glyph(ctx, data, &virama)) {
    return;
  }
  for (i = 0; i < buf->len; i++) {
    if (buf->info[i].position == IP_BASE_C) {
      buf->info[i].position = (uint8_t)indic_consonant_position(ctx, data,
          buf->info[i].glyph, virama);
    }
  }
}

static void indic_reorder_consonant_syllable(GFNT_ShapeCtx * ctx,
    const IndicData * data, size_t start, size_t end) {
  GFNT_LBuffer * buf = ctx->buf;
  GFNT_LInfo * info = buf->info;
  const IndicConfig * config = data->config;
  size_t base = end;
  bool has_reph = false;
  size_t limit = start;
  size_t i;

  // Kannada: a syllable that starts with Ra, halant, ZWJ is shaped as Ra, ZWJ,
  // halant (found with a mask and a pair comparison: the first shows what the
  // second does). The clusters stay where they were.
  if (config->virama == 0x0CCD && start + 3 <= end
      && info[start].category == IC_RA && info[start + 1].category == IC_H
      && info[start + 2].category == IC_ZWJ) {
    GFNT_LInfo swap = info[start + 1];
    uint32_t cluster = info[start + 1].cluster;

    info[start + 1] = info[start + 2];
    info[start + 1].cluster = cluster;
    cluster = info[start + 2].cluster;
    info[start + 2] = swap;
    info[start + 2].cluster = cluster;
  }

  // 1. Find the base consonant. If the syllable starts with Ra and a halant that
  // the font forms a reph from, the Ra is out of the running.
  if (data->mask[IF_RPHF] && start + 3 <= end
      && info[start].category != IC_H
      && ((config->reph_mode == REPH_IMPLICIT
              && !indic_is_joiner(&info[start + 2]))
          || (config->reph_mode == REPH_EXPLICIT
              && info[start + 2].category == IC_ZWJ))) {
    uint32_t glyphs[3] = {info[start].glyph, info[start + 1].glyph,
        config->reph_mode == REPH_EXPLICIT ? info[start + 2].glyph : 0};

    // The font is asked about the three glyphs, and about the first two.
    if (indic_would(ctx, data, IF_RPHF, glyphs,
            2 + (config->reph_mode == REPH_EXPLICIT ? 1u : 0u))
        || indic_would(ctx, data, IF_RPHF, glyphs, 2)) {
      limit += 2;
      while (limit < end && indic_is_joiner(&info[limit])) {
        limit++;
      }
      base = start;
      has_reph = true;
    }
  }
  else if (config->reph_mode == REPH_LOG_REPHA
      && info[start].category == IC_REPHA) {
    limit += 1;
    while (limit < end && indic_is_joiner(&info[limit])) {
      limit++;
    }
    base = start;
    has_reph = true;
  }

  switch (config->base_pos) {
    case BASE_LAST: {
      // From the end of the syllable, back to a consonant that has no below-
      // or post-base form.
      bool seen_below = false;

      i = end;
      do {
        i--;
        if (indic_is_consonant(&info[i])) {
          // Post-base forms have to follow below-base forms.
          if (info[i].position != IP_BELOW_C
              && (info[i].position != IP_POST_C || seen_below)) {
            base = i;
            break;
          }
          if (info[i].position == IP_BELOW_C) {
            seen_below = true;
          }
          // Or it is the first consonant, whatever forms it has.
          base = i;
        }
        else {
          // A ZWJ after a halant stops the search, asking for an explicit half
          // form. One before a halant asks for a subjoined form, so it goes on.
          if (start < i && info[i].category == IC_ZWJ
              && info[i - 1].category == IC_H) {
            break;
          }
        }
      } while (i > limit);
      break;
    }
    case BASE_LAST_SINHALA: {
      // Sinhala's ZWJ behaviour is different, and the font need not be asked
      // about consonant positions.
      if (!has_reph) {
        base = limit;
      }
      // The last base consonant not blocked by a ZWJ: one right before a
      // consonant asks for a subjoined form.
      for (i = limit; i < end; i++) {
        if (indic_is_consonant(&info[i])) {
          if (limit < i && info[i - 1].category == IC_ZWJ) {
            break;
          }
          base = i;
        }
      }
      // All the consonants after it are below.
      for (i = base + 1; i < end; i++) {
        if (indic_is_consonant(&info[i])) {
          info[i].position = IP_BELOW_C;
        }
      }
      break;
    }
  }

  // If the syllable starts with Ra and a halant and has no other consonant,
  // there is no reph.
  if (has_reph && base == start && limit - base <= 2) {
    has_reph = false;
    if (config->reph_mode != REPH_EXPLICIT) {
      limit = start;   // and the font is not asked to form one
    }
  }

  // Everything before the base is before it in the syllable.
  for (i = start; i < base; i++) {
    if (info[i].position > IP_PRE_C) {
      info[i].position = IP_PRE_C;
    }
  }
  if (base < end) {
    info[base].position = IP_BASE_C;
  }

  // A final consonant is one after a matra: Sinhala has them.
  for (i = base + 1; i < end; i++) {
    if (info[i].category == IC_M) {
      size_t j;

      for (j = i + 1; j < end; j++) {
        if (indic_is_consonant(&info[j])) {
          info[j].position = IP_FINAL_C;
          break;
        }
      }
      break;
    }
  }

  if (has_reph) {
    info[start].position = IP_RA_TO_BECOME_REPH;
  }

  // Attach joiners, nukta, register shifters, medials and halants to the previous
  // character, to move with it.
  {
    int last_pos = IP_START;

    for (i = start; i < end; i++) {
      if (FLAG(info[i].category) & (JOINER_FLAGS | FLAG(IC_N) | FLAG(IC_RS)
              | MEDIAL_FLAGS | HALANT_OR_COENG_FLAGS)) {
        info[i].position = (uint8_t)last_pos;
        if (info[i].category == IC_H && info[i].position == IP_PRE_M) {
          size_t j;

          // Uniscribe does not move the halant with a left matra.
          for (j = i; j > start; j--) {
            if (info[j - 1].position != IP_PRE_M) {
              info[i].position = info[j - 1].position;
              break;
            }
          }
        }
      }
      else if (info[i].position != IP_SMVD) {
        last_pos = info[i].position;
      }
    }
  }

  // A halant after the base goes with the consonant it is to be formed with, and
  // so does a nukta or joiner that stands between that and the one before.
  for (i = end - 1; i > base + 1; i--) {
    if (info[i - 1].category == IC_H && indic_is_consonant(&info[i])
        && info[i].position > IP_BASE_C) {
      size_t k = i;

      while (k > base + 1
          && (FLAG(info[k - 1].category) & (JOINER_FLAGS | FLAG(IC_N)
              | FLAG(IC_RS) | HALANT_OR_COENG_FLAGS))) {
        info[k - 1].position = info[i].position;
        k--;
      }
    }
  }

  // For old-style script tags, the first post-base halant moves to after the last
  // consonant, whatever is there, except in Kannada, which leaves it where it is
  // when the syllable ends in a halant.
  if (data->is_old_spec) {
    // Only Kannada leaves it where it is when the syllable ends in a halant
    // (found with a ligature for every pair of glyphs, in every script).
    bool disallow_double_halants = config->virama == 0x0CCD;

    for (i = base + 1; i < end; i++) {
      if (info[i].category == IC_H) {
        size_t j;

        for (j = end - 1; j > i; j--) {
          if (indic_is_consonant(&info[j])
              || (disallow_double_halants && info[j].category == IC_H)) {
            break;
          }
        }
        if (info[j].category != IC_H && j > i) {
          indic_move(buf, i, j);
        }
        break;
      }
    }
  }

  // The features' masks: the glyphs each applies to.
  {
    uint32_t mask;

    for (i = start; i < limit; i++) {
      info[i].mask |= data->mask[IF_RPHF];
    }
    // The halants right after the reph are reached too, with their joiners
    // (found with a ligature for every pair).
    if (has_reph) {
      for (i = limit; i < end && (info[i].category == IC_H
              || (i > limit && indic_is_joiner(&info[i]))); i++) {
        info[i].mask |= data->mask[IF_RPHF];
      }
    }
    // Pre-base. In an old-spec font `blwf` reaches only a Devanagari-style Ra
    // and its halant, and not when a ZWJ asks for a half form (found with a
    // ligature for every pair of glyphs; a consonant the font forms below the base
    // gets nothing here); a new-spec font has it on every glyph there.
    mask = data->mask[IF_HALF] | data->mask[IF_AKHN] | data->mask[IF_CJCT];
    if (config->blwf_mode == BLWF_PRE_AND_POST && !data->is_old_spec) {
      mask |= data->mask[IF_BLWF];
    }
    for (i = start; i < base; i++) {
      info[i].mask |= mask;
      if (config->blwf_mode == BLWF_PRE_AND_POST && data->is_old_spec
          && ((info[i].category == IC_RA && config->virama == 0x094D
                  && i + 1 < end && info[i + 1].category == IC_H
                  && !(i + 2 < end && info[i + 2].category == IC_ZWJ))
              || (info[i].category == IC_H && i > start
                  && info[i - 1].category == IC_RA
                  && config->virama == 0x094D
                  && !(i + 1 < end && info[i + 1].category == IC_ZWJ)))) {
        info[i].mask |= data->mask[IF_BLWF];
      }
    }
    // The base.
    mask = data->mask[IF_AKHN] | data->mask[IF_CJCT];
    if (base < end) {
      info[base].mask |= mask;
    }
    // Post-base. A left-hand matra is after the base in the text but goes in
    // front of it, and takes the pre-base features for it (found with one lookup
    // under each feature: HarfBuzz gives such a matra `half` and not `abvf`).
    mask = data->mask[IF_BLWF] | data->mask[IF_ABVF] | data->mask[IF_PSTF]
        | data->mask[IF_CJCT];
    {
      uint32_t before = data->mask[IF_HALF] | data->mask[IF_AKHN]
          | data->mask[IF_CJCT];

      if (config->blwf_mode == BLWF_PRE_AND_POST && !data->is_old_spec) {
        before |= data->mask[IF_BLWF];
      }
      for (i = base + 1; i < end; i++) {
        info[i].mask |= info[i].position < IP_BASE_C ? before : mask;
      }
    }
  }

  // The effects of ZWJ and ZWNJ.
  for (i = start + 1; i < end; i++) {
    if (indic_is_joiner(&info[i])) {
      bool non_joiner = info[i].category == IC_ZWNJ;
      size_t j = i;

      do {
        j--;
        // A ZWNJ disables half forms. (A joiner disables CJCT just by being
        // there: the lookup is not told to skip it.)
        if (non_joiner) {
          info[j].mask &= ~data->mask[IF_HALF];
        }
      } while (j > start && !indic_is_consonant(&info[j]));
    }
  }

  // Sort the syllable by position: the matras and the rest go where they go. The
  // sort is stable. Each character carries its place in the syllable while it
  // runs, so that the clusters of everything that moved can be merged after.
  {
    uint8_t tag = info[start].syllable;

    for (i = start; i < end; i++) {
      info[i].syllable = (uint8_t)(i - start);
    }
    for (i = start + 1; i < end; i++) {
      size_t j = i;

      while (j > start && info[j - 1].position > info[i].position) {
        j--;
      }
      if (j < i) {
        indic_move(buf, i, j);
      }
    }
    // Find the base again: the sort has moved it.
    base = end;
    for (i = start; i < end; i++) {
      if (info[i].position == IP_BASE_C) {
        base = i;
        break;
      }
    }
    if (data->mask[IF_PREF] && base + 2 < end) {
      // Find a halant, Ra pair and mark it for pre-base-reordering.
      for (i = base + 1; i + 1 < end; i++) {
        uint32_t glyphs[2] = {info[i].glyph, info[i + 1].glyph};

        if (indic_would(ctx, data, IF_PREF, glyphs, 2)) {
          info[i++].mask |= data->mask[IF_PREF];
          info[i++].mask |= data->mask[IF_PREF];
          break;
        }
      }
    }
    // The positions after the base may shuffle about: in old-style mode halants
    // are moved too, so everything after the base is merged. Otherwise each
    // cycle of the permutation is merged. A left-hand matra, which went to the
    // front, takes the whole cycle it is in with it.
    if (data->is_old_spec || end - start > 127) {
      gfnt_merge_clusters(info, buf->len, base, end);
    }
    else {
      for (i = base + 1; i < end; i++) {
        if (info[i].syllable != 255) {
          size_t max = i;
          size_t j = start + info[i].syllable;

          while (j != i) {
            size_t next = start + info[j].syllable;

            if (j > max) {
              max = j;
            }
            info[j].syllable = 255;
            j = next;
          }
          if (i != max) {
            gfnt_merge_clusters(info, buf->len, i, max + 1);
          }
        }
      }
    }
    for (i = start; i < end; i++) {
      info[i].syllable = tag;
    }
  }
}

/**
 * Reverse the order of the left-hand matras the sort left side by side: HarfBuzz
 * does, each matra keeping the nukta or halant that followed it.
 */
static void indic_reverse_matra_runs(GFNT_LBuffer * buf, size_t start,
    size_t end) {
  size_t i = start;

  while (i < end) {
    if (buf->info[i].position == IP_PRE_M) {
      size_t j = i;
      size_t k;
      size_t lo;
      size_t hi;

      while (j < end && buf->info[j].position == IP_PRE_M) {
        j++;
      }
      // Reverse the whole run, then each block (a matra and what follows it)
      // back into its own order.
      for (lo = i, hi = j - 1; lo < hi; lo++, hi--) {
        GFNT_LInfo t = buf->info[lo];
        GFNT_LPos p = buf->pos[lo];

        buf->info[lo] = buf->info[hi];
        buf->pos[lo] = buf->pos[hi];
        buf->info[hi] = t;
        buf->pos[hi] = p;
      }
      for (k = i; k < j;) {
        size_t e = k + 1;

        // After the reversal a block's matra is its last element.
        while (e < j && buf->info[e - 1].category != IC_M) {
          e++;
        }
        for (lo = k, hi = e - 1; lo < hi; lo++, hi--) {
          GFNT_LInfo t = buf->info[lo];
          GFNT_LPos p = buf->pos[lo];

          buf->info[lo] = buf->info[hi];
          buf->pos[lo] = buf->pos[hi];
          buf->info[hi] = t;
          buf->pos[hi] = p;
        }
        k = e;
      }
      i = j;
    }
    else {
      i++;
    }
  }
}

static void indic_reorder_syllable(GFNT_ShapeCtx * ctx, const IndicData * data,
    size_t start, size_t end) {
  unsigned type = ctx->buf->info[start].syllable & 0x0F;

  switch (type) {
    case IS_VOWEL:
    case IS_CONSONANT:
    case IS_BROKEN:
    case IS_STANDALONE:
      // The vowels were made to look like consonants, and so were the dotted
      // circle and the placeholder.
      indic_reorder_consonant_syllable(ctx, data, start, end);
      indic_reverse_matra_runs(ctx->buf, start, end);
      break;
    default:
      break;
  }
}

static void indic_setup_syllables(GFNT_ShapeCtx * ctx) {
  indic_find_syllables(ctx);
}

static void indic_initial_reordering(GFNT_ShapeCtx * ctx) {
  const IndicData * data = ctx->plan->shaper_data;
  GFNT_LBuffer * buf = ctx->buf;
  size_t start = 0;

  indic_update_consonant_positions(ctx, data);
  if (!gfnt_syllabic_insert_dotted_circles(ctx, IS_BROKEN, IC_DOTTEDCIRCLE,
          IC_REPHA, IP_BASE_C)) {
    return;
  }
  while (start < buf->len) {
    size_t end = start + 1;

    while (end < buf->len
        && buf->info[end].syllable == buf->info[start].syllable) {
      end++;
    }
    indic_reorder_syllable(ctx, data, start, end);
    start = end;
  }
}

/** Whether a character of this category is part of a word, as HarfBuzz counts it. */
static bool indic_continues_word(uint8_t gc) {
  switch (gc) {
    case GUNI_GC_FORMAT:
    case GUNI_GC_UNASSIGNED:
    case GUNI_GC_PRIVATE_USE:
    case GUNI_GC_SURROGATE:
    case GUNI_GC_LOWERCASE_LETTER:
    case GUNI_GC_MODIFIER_LETTER:
    case GUNI_GC_OTHER_LETTER:
    case GUNI_GC_TITLECASE_LETTER:
    case GUNI_GC_UPPERCASE_LETTER:
    case GUNI_GC_SPACING_MARK:
    case GUNI_GC_ENCLOSING_MARK:
    case GUNI_GC_NONSPACING_MARK:
      return true;
    default:
      return false;
  }
}

static void indic_final_reordering_syllable(GFNT_ShapeCtx * ctx,
    const IndicData * data, size_t start, size_t end) {
  GFNT_LBuffer * buf = ctx->buf;
  GFNT_LInfo * info = buf->info;
  const IndicConfig * config = data->config;
  bool try_pref = data->mask[IF_PREF] != 0;
  uint32_t virama_glyph = 0;
  size_t base;
  size_t i;

  // This function leans on halants, and ligation and multiple substitution may
  // have mislaid some. Recover the case where a halant glyph is plainly wanted
  // and has been lost.
  if (indic_virama_glyph(ctx, data, &virama_glyph)) {
    for (i = start; i < end; i++) {
      if (info[i].glyph == virama_glyph
          && (info[i].props & GFNT_PROP_LIGATED)
          && (info[i].props & GFNT_PROP_MULTIPLIED)) {
        info[i].category = IC_H;
        info[i].props &= (uint16_t)~(GFNT_PROP_LIGATED | GFNT_PROP_MULTIPLIED);
      }
    }
  }

  // Find the base again.
  for (base = start; base < end; base++) {
    if (info[base].position >= IP_BASE_C) {
      if (try_pref && base + 1 < end) {
        for (i = base + 1; i < end; i++) {
          if ((info[i].mask & data->mask[IF_PREF])
              && !((info[i].props & GFNT_PROP_SUBSTITUTED)
                  && (info[i].props & GFNT_PROP_LIGATED)
                  && !(info[i].props & GFNT_PROP_MULTIPLIED))) {
            // A pref candidate that formed nothing: the base is about here.
            base = i;
            while (base < end && indic_is_halant(&info[base])) {
              base++;
            }
            info[base].position = IP_BASE_C;
            try_pref = false;
            break;
          }
        }
      }
      // For Malayalam, skip over unformed below forms, but not post forms.
      if (data->malayalam) {
        for (i = base + 1; i < end; i++) {
          while (i < end && indic_is_joiner(&info[i])) {
            i++;
          }
          if (i == end || !indic_is_halant(&info[i])) {
            break;
          }
          i++;   // The halant.
          while (i < end && indic_is_joiner(&info[i])) {
            i++;
          }
          if (i < end && indic_is_consonant(&info[i])
              && info[i].position == IP_BELOW_C) {
            base = i;
            info[base].position = IP_BASE_C;
          }
        }
      }
      if (start < base && info[base].position > IP_BASE_C) {
        base--;
      }
      break;
    }
  }
  if (base == end && start < base && info[base - 1].category == IC_ZWJ
      && !(info[base - 1].props & GFNT_PROP_LIGATED)) {
    base--;
  }
  if (base < end) {
    while (start < base
        && indic_is_one_of(&info[base], FLAG(IC_N) | FLAG(IC_H))) {
      base--;
    }
  }

  // Reorder the matras. A pre-base matra that was moved before the basic
  // features can come closer to the main consonant, according to whether half
  // forms were made: it goes after the last standalone halant, after the
  // initial matra position and before the main consonant. If a ZWJ or ZWNJ
  // follows the halant, it goes after that.
  if (start + 1 < end && start < base) {
    // If we lost track of the base, alas, before the last thing.
    size_t new_pos = base == end ? base - 2 : base - 1;

    // Malayalam and Tamil have no half or explicit virama forms: what 'half'
    // makes there is a chillu or a ligature.
    if (!data->malayalam && !data->tamil) {
      while (new_pos > start
          && (!indic_is_one_of(&info[new_pos],
                  FLAG(IC_M) | FLAG(IC_H) | FLAG(IC_COENG))
              // A halant a ZWJ follows asks for a half form: not a place for it.
              || (info[new_pos].category == IC_H && new_pos + 1 < end
                  && info[new_pos + 1].category == IC_ZWJ))) {
        new_pos--;
      }
      // If there is no halant, nothing moves. If there is, only if it is not
      // the matra's own.
      if (indic_is_halant(&info[new_pos]) && info[new_pos].position != IP_PRE_M) {
        if (new_pos + 1 < end && indic_is_joiner(&info[new_pos + 1])) {
          new_pos++;
        }
      }
      else {
        new_pos = start;
      }
    }
    if (start < new_pos && info[new_pos].position != IP_PRE_M) {
      // Now see whether there is a matra to move.
      for (i = new_pos; i > start; i--) {
        if (info[i - 1].position == IP_PRE_M) {
          size_t old_pos = i - 1;

          if (old_pos < base && base <= new_pos) {
            base--;
          }
          indic_move(buf, old_pos, new_pos);
          // The merge is deliberately after the move: reordering a matra is
          // special.
          gfnt_merge_clusters(info, buf->len, new_pos,
              end < base + 1 ? end : base + 1);
          new_pos--;
        }
      }
    }
    else {
      for (i = start; i < base; i++) {
        if (info[i].position == IP_PRE_M) {
          gfnt_merge_clusters(info, buf->len, i,
              end < base + 1 ? end : base + 1);
          break;
        }
      }
    }
  }

  // Reorder the reph. It starts at the beginning of the syllable and is moved
  // to its place now that 'rphf' has made it.
  if (start + 1 < end && info[start].position == IP_RA_TO_BECOME_REPH
      && ((info[start].category == IC_REPHA)
          != ((info[start].props & GFNT_PROP_LIGATED)
              && !(info[start].props & GFNT_PROP_MULTIPLIED)))) {
    size_t new_reph_pos;
    int reph_pos = config->reph_pos;
    bool moved = false;

    // 1. If the reph goes after the post-base consonant forms, go to step 5.
    if (reph_pos != REPH_AFTER_POST) {
      // 2. Not after post-base: after the first explicit halant between the
      // first post-reph consonant and the main consonant, and after any joiner
      // that follows it.
      new_reph_pos = start + 1;
      while (new_reph_pos < base && !indic_is_halant(&info[new_reph_pos])) {
        new_reph_pos++;
      }
      if (new_reph_pos < base && indic_is_halant(&info[new_reph_pos])) {
        if (new_reph_pos + 1 < base && indic_is_joiner(&info[new_reph_pos + 1])) {
          new_reph_pos++;
        }
        moved = true;
      }
      // 3. After the main consonant: after the first consonant not ligated
      // with it.
      if (!moved && reph_pos == REPH_AFTER_MAIN) {
        new_reph_pos = base;
        while (new_reph_pos + 1 < end
            && info[new_reph_pos + 1].position <= IP_AFTER_MAIN) {
          new_reph_pos++;
        }
        if (new_reph_pos < end) {
          moved = true;
        }
      }
      // 4. Before post-base: before the first post-base consonant not ligated
      // with the main one, or if there is none, before the first matra,
      // syllable modifier or vedic sign.
      if (!moved && reph_pos == REPH_AFTER_SUB) {
        new_reph_pos = base;
        while (new_reph_pos + 1 < end
            && !(FLAG(info[new_reph_pos + 1].position)
                & (FLAG(IP_POST_C) | FLAG(IP_AFTER_POST) | FLAG(IP_SMVD)))) {
          new_reph_pos++;
        }
        if (new_reph_pos < end) {
          moved = true;
        }
      }
    }
    // 5. Before the first post-base matra, syllable modifier or vedic sign that
    // is after the intended place.
    if (!moved) {
      new_reph_pos = start + 1;
      while (new_reph_pos < base && !indic_is_halant(&info[new_reph_pos])) {
        new_reph_pos++;
      }
      if (new_reph_pos < base && indic_is_halant(&info[new_reph_pos])) {
        if (new_reph_pos + 1 < base && indic_is_joiner(&info[new_reph_pos + 1])) {
          new_reph_pos++;
        }
        moved = true;
      }
    }
    // 6. Otherwise, the end of the syllable.
    if (!moved) {
      new_reph_pos = end - 1;
      while (new_reph_pos > start && info[new_reph_pos].position == IP_SMVD) {
        new_reph_pos--;
      }
      // If the reph is to end up after a matra and halant, put it before the
      // halant, so it can interact with the matra: but not for a plain
      // consonant and halant.
      if (indic_is_halant(&info[new_reph_pos])) {
        for (i = base + 1; i < new_reph_pos; i++) {
          if (info[i].category == IC_M) {
            new_reph_pos--;
          }
        }
      }
    }
    gfnt_merge_clusters(info, buf->len, start, new_reph_pos + 1);
    indic_move(buf, start, new_reph_pos);
    if (start < base && base <= new_reph_pos) {
      base--;
    }
  }

  // Reorder the pre-base-reordering consonants.
  if (try_pref && base + 1 < end) {
    for (i = base + 1; i < end; i++) {
      if ((info[i].mask & data->mask[IF_PREF])
          && (info[i].props & GFNT_PROP_SUBSTITUTED)
          && (info[i].props & GFNT_PROP_LIGATED)
          && !(info[i].props & GFNT_PROP_MULTIPLIED)) {
        // Only a glyph that something made while 'pref' was applied. (A font may
        // shape a Ra with the feature in general but block it in some context.)
        // Find the target as for a pre-base matra; failing that, just before the
        // main consonant.
        size_t new_pos = base;

        if (!data->malayalam && !data->tamil) {
          while (new_pos > start
              && !indic_is_one_of(&info[new_pos - 1],
                  FLAG(IC_M) | FLAG(IC_H) | FLAG(IC_COENG))) {
            new_pos--;
          }
        }
        if (new_pos > start && indic_is_halant(&info[new_pos - 1])) {
          // A joiner after the halant: after that.
          if (new_pos < end && indic_is_joiner(&info[new_pos])) {
            new_pos++;
          }
        }
        {
          size_t old_pos = i;

          gfnt_merge_clusters(info, buf->len, new_pos, old_pos + 1);
          indic_move(buf, old_pos, new_pos);
          if (new_pos <= base && base < old_pos) {
            base++;
          }
        }
        break;
      }
    }
  }

  // 'init' goes to a left matra at the start of a word.
  if (info[start].position == IP_PRE_M) {
    if (!start || !indic_continues_word(buf->info[start - 1].gc)) {
      info[start].mask |= data->mask[IF_INIT];
    }
  }
}

static void indic_final_reordering(GFNT_ShapeCtx * ctx) {
  const IndicData * data = ctx->plan->shaper_data;
  GFNT_LBuffer * buf = ctx->buf;
  size_t start = 0;

  while (start < buf->len) {
    size_t end = start + 1;

    while (end < buf->len
        && buf->info[end].syllable == buf->info[start].syllable) {
      end++;
    }
    indic_final_reordering_syllable(ctx, data, start, end);
    start = end;
  }
}

/* --- the plan ----------------------------------------------------------- */

static GFNT_Tag indic_tag(const char * t) {
  return GFNT_TAG(t[0], t[1], t[2], t[3]);
}

static void indic_collect_features(GFNT_Plan * plan) {
  size_t i;

  // Before any lookup has run.
  gfnt_plan_pause(plan, indic_setup_syllables);
  gfnt_plan_enable(plan, GFNT_TAG('l', 'o', 'c', 'l'), GFNT_PF_PER_SYLLABLE, 1);
  // The Indic specifications do not require ccmp, but if there is a use of it,
  // it is typically at the beginning.
  gfnt_plan_enable(plan, GFNT_TAG('c', 'c', 'm', 'p'), GFNT_PF_PER_SYLLABLE, 1);
  gfnt_plan_pause(plan, indic_initial_reordering);
  // The basic features, one at a time, each limited to the glyphs the reordering
  // chose.
  for (i = 0; i < IF_BASIC; i++) {
    uint32_t joiners = GFNT_PF_MANUAL_ZWNJ | GFNT_PF_MANUAL_ZWJ
        | GFNT_PF_PER_SYLLABLE;

    if (indic_features[i].global) {
      gfnt_plan_enable(plan, indic_tag(indic_features[i].tag), joiners, 1);
    }
    else {
      gfnt_plan_add(plan, indic_tag(indic_features[i].tag), joiners);
    }
    gfnt_plan_pause(plan, NULL);
  }
  gfnt_plan_pause(plan, indic_final_reordering);
  // The others, all at once.
  for (; i < IF_COUNT; i++) {
    uint32_t joiners = GFNT_PF_MANUAL_ZWNJ | GFNT_PF_MANUAL_ZWJ
        | GFNT_PF_PER_SYLLABLE;

    if (indic_features[i].global) {
      gfnt_plan_enable(plan, indic_tag(indic_features[i].tag), joiners, 1);
    }
    else {
      gfnt_plan_add(plan, indic_tag(indic_features[i].tag), joiners);
    }
  }
  gfnt_plan_enable(plan, GFNT_TAG('c', 'a', 'l', 't'), 0, 1);
  gfnt_plan_enable(plan, GFNT_TAG('c', 'l', 'i', 'g'), 0, 1);
}

static void indic_override_features(GFNT_Plan * plan) {
  // HarfBuzz leaves `liga` off for the Indic scripts, whatever else the plan
  // enables (found by shaping one lookup under every feature tag: it was the
  // only tag the two disagreed on). A caller's `+liga` still turns it on.
  gfnt_plan_enable(plan, GFNT_TAG('l', 'i', 'g', 'a'), 0, 0);
}

static void * indic_data_create(const GFNT_Plan * plan,
    const GFNT_Allocator * allocator) {
  IndicData * data = allocator->calloc_fn(allocator->ctx, 1, sizeof *data);
  size_t i;

  if (!data) {
    return NULL;
  }
  data->config = &indic_configs[0];
  for (i = 1; i < sizeof indic_configs / sizeof indic_configs[0]; i++) {
    if (indic_tag(indic_configs[i].script) == plan->script) {
      data->config = &indic_configs[i];
    }
  }
  data->malayalam = plan->script == GFNT_TAG('m', 'l', 'y', 'm');
  data->tamil = plan->script == GFNT_TAG('t', 'a', 'm', 'l');
  // The new-spec script tags end in a 2.
  data->is_old_spec = data->config->has_old_spec
      && (plan->tables[0].script & 0xFFu) != '2';
  // New-spec fonts of the main scripts, and scripts with one spec only, are asked
  // with no context; old-spec fonts, and Malayalam, are not.
  data->zero_context = !data->is_old_spec && !data->malayalam;
  for (i = 0; i < IF_COUNT; i++) {
    data->mask[i] = indic_features[i].global ? 0
        : gfnt_plan_found_mask(plan, indic_tag(indic_features[i].tag));
  }
  data->grammar = indic_grammar();
  if (!data->grammar) {
    allocator->free_fn(allocator->ctx, data);
    return NULL;
  }
  return data;
}

static void indic_data_destroy(void * pointer, const GFNT_Allocator * allocator) {
  IndicData * data = pointer;

  gfnt_nfa_free(data->grammar);
  allocator->free_fn(allocator->ctx, data);
}

static void indic_setup_masks(GFNT_ShapeCtx * ctx) {
  size_t i;

  // The masks cannot be set here: what is known of the characters is saved, and
  // the masks are set at the pause after the syllables are found.
  for (i = 0; i < ctx->buf->len; i++) {
    indic_set_properties(&ctx->buf->info[i]);
  }
}

/* --- normalisation ------------------------------------------------------ */

static bool indic_decompose(uint32_t ab, uint32_t * a, uint32_t * b, void * pointer) {
  (void)pointer;
  switch (ab) {
    // Do not take these apart.
    case 0x0931:   // Devanagari RRA.
    case 0x0B94:   // Tamil AU.
    case 0x09DC:   // Bengali RRA.
    case 0x09DD:   // Bengali RHA.
      return false;
    default:
      break;
  }
  return gfnt_unicode_decompose(ab, a, b, NULL);
}

/** Split vowels are not put back together; the Bengali YYA with a nukta is. */
static bool indic_compose(uint32_t a, uint32_t b, uint32_t * ab, void * ctx) {
  GUNI_GeneralCategory gc = guni_general_category(a);

  (void)ctx;
  if (gc == GUNI_GC_NONSPACING_MARK || gc == GUNI_GC_SPACING_MARK
      || gc == GUNI_GC_ENCLOSING_MARK) {
    return false;
  }
  if (a == 0x09AF && b == 0x09BC) {
    *ab = 0x09DF;
    return true;
  }
  *ab = guni_compose(a, b);
  return *ab != 0;
}

static const GFNT_NormHooks indic_hooks = {
  .decompose = indic_decompose,
  .compose = indic_compose,
};

const GFNT_Shaper gfnt_shaper_indic = {
  .name = "indic",
  .collect_features = indic_collect_features,
  .trailing_pause = gfnt_syllabic_clear_syllables,
  .override_features = indic_override_features,
  .data_create = indic_data_create,
  .data_destroy = indic_data_destroy,
  .preprocess_text = gfnt_vowel_constraints,
  .normalization = GFNT_NORM_COMPOSED_DIACRITICS_NO_SHORT_CIRCUIT,
  .normalization_hooks = &indic_hooks,
  .setup_masks = indic_setup_masks,
  .zero_width_marks = 0,
  .fallback_position = false,
};
