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
 * The Universal Shaping Engine: Microsoft's shaper for the scripts that have no
 * dedicated one, as HarfBuzz implements it.
 *
 * Every character is given a category from its Unicode properties; the run is cut
 * into syllables by a grammar over the categories; the font's basic features run
 * one syllable at a time; then the syllable is *reordered* - a repha behind its
 * base, a left-hand vowel in front of it - and the rest of the features run over
 * the whole run. The specification is "Creating and supporting OpenType fonts for
 * the Universal Shaping Engine"; where HarfBuzz departs from it the departure is
 * named.
 */

#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/norm.h>
#include <string.h>
#include "nfa.h"
#include "plan.h"
#include "uprops.h"
#include "vowel_constraints.h"
#include "use_data.h"

// The categories of USE, with the subclasses that depend on position.
enum {
  UC_O, UC_B, UC_N, UC_GB, UC_CGJ, UC_SUB, UC_H, UC_HN, UC_ZWNJ, UC_ZWJ, UC_WJ,
  UC_R, UC_CS, UC_HVM, UC_SK, UC_G, UC_J, UC_SB, UC_SE, UC_HM, UC_HR, UC_RK,
  UC_VS,
  UC_FABV, UC_FBLW, UC_FPST, UC_MABV, UC_MBLW, UC_MPST, UC_MPRE, UC_CMABV,
  UC_CMBLW, UC_VABV, UC_VBLW, UC_VPST, UC_VPRE, UC_VMABV, UC_VMBLW, UC_VMPST,
  UC_VMPRE, UC_SMABV, UC_SMBLW, UC_FMABV, UC_FMBLW, UC_FMPST, UC_IND, UC_S, UC_VMH,
  UC_COUNT
};

// The syllable kinds, in the order the grammar lists them: that order breaks a tie.
enum {
  US_INDEPENDENT, US_VIRAMA_TERMINATED, US_SAKOT_TERMINATED, US_STANDARD,
  US_NUMBER_JOINER_TERMINATED, US_NUMERAL, US_SYMBOL, US_HIEROGLYPH, US_BROKEN,
  US_NON_CLUSTER, US_COUNT
};

/** The position an Indic positional category puts a mark in. */
enum { UP_ABOVE, UP_BELOW, UP_POST, UP_PRE };

static int use_position(GUNI_IndicPositionalCategory ipc, int fallback) {
  switch (ipc) {
    case GUNI_INPC_LEFT:
    case GUNI_INPC_LEFT_AND_RIGHT:
    case GUNI_INPC_BOTTOM_AND_LEFT:
    case GUNI_INPC_TOP_AND_LEFT:
    case GUNI_INPC_TOP_AND_LEFT_AND_RIGHT:
    case GUNI_INPC_TOP_AND_BOTTOM_AND_LEFT:
    case GUNI_INPC_VISUAL_ORDER_LEFT:
      return UP_PRE;
    case GUNI_INPC_TOP:
    case GUNI_INPC_TOP_AND_BOTTOM:
    case GUNI_INPC_TOP_AND_BOTTOM_AND_RIGHT:
    case GUNI_INPC_TOP_AND_RIGHT:
      return UP_ABOVE;
    case GUNI_INPC_BOTTOM:
    case GUNI_INPC_OVERSTRUCK:
    case GUNI_INPC_BOTTOM_AND_RIGHT:
      return UP_BELOW;
    case GUNI_INPC_RIGHT:
      return UP_POST;
    default:
      return fallback;
  }
}

/** The override Microsoft's data gives a character, or @p fallback if it gives none. */
static int use_override(const GFNT_UseRange * table, size_t count, uint32_t u,
    int fallback) {
  size_t lo = 0;
  size_t hi = count;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;

    if (u < table[mid].first) {
      hi = mid;
    }
    else if (u > table[mid].last) {
      lo = mid + 1;
    }
    else {
      return table[mid].value;
    }
  }
  return fallback;
}

static uint8_t use_category(uint32_t u) {
  GUNI_IndicSyllabicCategory isc;
  GUNI_IndicPositionalCategory ipc;
  uint32_t gc;
  GUNI_JoiningType jt;

  if (u == 0x034F) {
    return UC_CGJ;
  }
  if (u == 0x200C) {
    return UC_ZWNJ;
  }
  if (u == 0x200D) {
    return UC_ZWJ;
  }
  if (u == 0x2060) {
    return UC_WJ;
  }
  if (u >= 0xFE00 && u <= 0xFE0F) {
    return UC_VS;
  }
  isc = (GUNI_IndicSyllabicCategory)use_override(gfnt_use_isc_overrides,
      GFNT_USE_ISC_OVERRIDE_COUNT, u, (int)guni_indic_syllabic_category(u));
  ipc = (GUNI_IndicPositionalCategory)use_override(gfnt_use_ipc_overrides,
      GFNT_USE_IPC_OVERRIDE_COUNT, u, (int)guni_indic_positional_category(u));
  // Characters HarfBuzz sorts differently from the Unicode data and Microsoft's
  // overrides, found by shaping every pair of a script's characters against it.
  if ((u >= 0x1B5A && u <= 0x1B6A) || (u >= 0x1B74 && u <= 0x1B7E)) {
    return UC_GB;    // Balinese punctuation and musical symbols.
  }
  if ((u >= 0x1BFC && u <= 0x1BFF) || u == 0x1A1E || u == 0x1A1F
      || (u >= 0x11047 && u <= 0x1104D) || (u >= 0xA9C1 && u <= 0xA9CD)
      || u == 0xA9DE || u == 0xA9DF || u == 0xAADE || u == 0xAADF
      || u == 0x114C6) {
    return UC_O;     // The punctuation of Batak, Buginese, Brahmi, Javanese, Tai Viet, Tirhuta.
  }
  if (u == 0x11302 || u == 0x11303 || u == 0xAAB4 || u == 0x114C1) {
    return UC_VMABV; // Grantha anusvara and visarga, Tai Viet mai thung, Tirhuta sign visarga.
  }
  if ((u >= 0x180B && u <= 0x180D) || u == 0x180F) {
    return UC_N;     // The Mongolian free variation selectors.
  }
  // The Sinhala kunddaliya is punctuation in the Unicode data and takes marks like
  // a letter in HarfBuzz's.
  if (u == 0x0DF4) {
    return UC_B;
  }
  // The Sinhala al-lakuna stands where a vowel modifier does, and before them.
  if (u == 0x0DCA) {
    return UC_VMH;
  }
  switch ((int)isc) {
    case GFNT_USE_ISC_HIEROGLYPH:
      return UC_G;
    case GFNT_USE_ISC_HIEROGLYPH_JOINER:
      return UC_J;
    case GFNT_USE_ISC_HIEROGLYPH_SEGMENT_BEGIN:
      return UC_SB;
    case GFNT_USE_ISC_HIEROGLYPH_SEGMENT_END:
      return UC_SE;
    case GFNT_USE_ISC_HIEROGLYPH_MIRROR:
      return UC_HR;
    case GFNT_USE_ISC_HIEROGLYPH_MODIFIER:
      return UC_HM;
    case GFNT_USE_ISC_FINAL_MODIFIER: {
      int p = use_position(ipc, UP_ABOVE);

      return p == UP_BELOW ? UC_FMBLW : p == UP_POST ? UC_FMPST : UC_FMABV;
    }
    case GFNT_USE_ISC_SYMBOL_MODIFIER: {
      int p = use_position(ipc, UP_ABOVE);

      return p == UP_BELOW ? UC_SMBLW : UC_SMABV;
    }
    default:
      break;
  }
  gc = (uint32_t)guni_general_category(u);
  jt = guni_joining_type(u);
  if (jt == GUNI_JT_JOIN_CAUSING || jt == GUNI_JT_DUAL_JOINING
      || jt == GUNI_JT_LEFT_JOINING || jt == GUNI_JT_RIGHT_JOINING) {
    return UC_B;
  }
  if (u == 0x2015 || u == 0x2022 || (u >= 0x25FB && u <= 0x25FE)
      || u == 0x25CC) {
    return UC_GB;
  }
  switch (isc) {
    case GUNI_INSC_JOINER:
      return UC_ZWJ;
    case GUNI_INSC_NON_JOINER:
      return UC_ZWNJ;
    case GUNI_INSC_NUMBER:
    case GUNI_INSC_CONSONANT:
    case GUNI_INSC_CONSONANT_HEAD_LETTER:
    case GUNI_INSC_TONE_LETTER:
    case GUNI_INSC_VOWEL_INDEPENDENT:
      return UC_B;
    case GUNI_INSC_AVAGRAHA:
    case GUNI_INSC_BINDU:
    case GUNI_INSC_CONSONANT_FINAL:
    case GUNI_INSC_CONSONANT_MEDIAL:
    case GUNI_INSC_CONSONANT_SUBJOINED:
    case GUNI_INSC_VOWEL:
    case GUNI_INSC_VOWEL_DEPENDENT:
      if (gc == GUNI_GC_OTHER_LETTER) {
        return UC_B;
      }
      break;
    default:
      break;
  }
  switch (isc) {
    case GUNI_INSC_CONSONANT_FINAL:
    case GUNI_INSC_CONSONANT_SUCCEEDING_REPHA: {
      int p = use_position(ipc, UP_POST);

      return p == UP_ABOVE ? UC_FABV : p == UP_BELOW ? UC_FBLW : UC_FPST;
    }
    case GUNI_INSC_SYLLABLE_MODIFIER: {
      int p = use_position(ipc, UP_ABOVE);

      return p == UP_BELOW ? UC_FMBLW : p == UP_POST ? UC_FMPST : UC_FMABV;
    }
    case GUNI_INSC_CONSONANT_PLACEHOLDER:
      return UC_GB;
    case GUNI_INSC_VIRAMA:
      (void)gc;
      return UC_HVM;
    case GUNI_INSC_INVISIBLE_STACKER:
      return UC_H;
    case GUNI_INSC_NUMBER_JOINER:
      return UC_HN;
    case GUNI_INSC_CONSONANT_DEAD:
    case GUNI_INSC_MODIFYING_LETTER:
      return UC_IND;
    case GUNI_INSC_CONSONANT_MEDIAL: {
      int p = use_position(ipc, UP_POST);

      return p == UP_PRE ? UC_MPRE : p == UP_ABOVE ? UC_MABV
          : p == UP_BELOW ? UC_MBLW : UC_MPST;
    }
    case GUNI_INSC_BRAHMI_JOINING_NUMBER:
      return UC_N;
    case GUNI_INSC_CONSONANT_PRECEDING_REPHA:
    case GUNI_INSC_CONSONANT_PREFIXED:
      return UC_R;
    case GUNI_INSC_REORDERING_KILLER:
      return UC_RK;
    case GUNI_INSC_CONSONANT_SUBJOINED:
      return UC_SUB;
    case GUNI_INSC_VOWEL:
    case GUNI_INSC_VOWEL_DEPENDENT:
    case GUNI_INSC_PURE_KILLER: {
      int p = use_position(ipc, UP_ABOVE);

      return p == UP_PRE ? UC_VPRE : p == UP_BELOW ? UC_VBLW
          : p == UP_POST ? UC_VPST : UC_VABV;
    }
    case GUNI_INSC_BINDU:
    case GUNI_INSC_TONE_MARK:
    case GUNI_INSC_CANTILLATION_MARK:
    case GUNI_INSC_REGISTER_SHIFTER:
    case GUNI_INSC_VISARGA: {
      int p = use_position(ipc, UP_ABOVE);

      return p == UP_PRE ? UC_VMPRE : p == UP_BELOW ? UC_VMBLW
          : p == UP_POST ? UC_VMPST : UC_VMABV;
    }
    case GUNI_INSC_NUKTA:
    case GUNI_INSC_GEMINATION_MARK:
    case GUNI_INSC_CONSONANT_KILLER: {
      int p = use_position(ipc, UP_BELOW);

      return p == UP_BELOW ? UC_CMBLW : UC_CMABV;
    }
    case GUNI_INSC_CONSONANT_WITH_STACKER:
      return UC_CS;
    default:
      break;
  }
  if (u == 0x1A60) {
    return UC_SK;   // TAI THAM SIGN SAKOT
  }
  if ((gc == GUNI_GC_OTHER_SYMBOL) || gc == GUNI_GC_CURRENCY_SYMBOL) {
    int p = use_position(ipc, UP_ABOVE);

    return p == UP_BELOW ? UC_SMBLW : UC_SMABV;
  }
  if (gc == GUNI_GC_OTHER_PUNCTUATION && u != 0x104E && u != 0x2022) {
    return UC_IND;
  }
  if (gc == GUNI_GC_SPACE_SEPARATOR) {
    return UC_O;
  }
  if (gc == GUNI_GC_NONSPACING_MARK || gc == GUNI_GC_SPACING_MARK
      || gc == GUNI_GC_ENCLOSING_MARK) {
    // A mark nothing names modifies the letter before it, above unless it says
    // otherwise.
    int p = use_position(ipc, UP_ABOVE);

    return p == UP_PRE ? UC_VMPRE : p == UP_BELOW ? UC_VMBLW
        : p == UP_POST ? UC_VMPST : UC_VMABV;
  }
  if (gc == GUNI_GC_OTHER_LETTER || gc == GUNI_GC_LOWERCASE_LETTER
      || gc == GUNI_GC_UPPERCASE_LETTER || gc == GUNI_GC_MODIFIER_LETTER) {
    return UC_B;
  }
  return UC_O;
}

/* --- the syllables -------------------------------------------------------- */

#define SYM(c) gfnt_re_sym(&b, (c))
#define SEQ(x, y) gfnt_re_seq(&b, (x), (y))
#define ALT(x, y) gfnt_re_alt(&b, (x), (y))
#define STAR(x) gfnt_re_star(&b, (x))
#define OPT(x) gfnt_re_opt(&b, (x))
#define PLUS(x) gfnt_re_plus(&b, (x))
#define SET(m) gfnt_re_set(&b, (m))
#define BIT(c) ((uint64_t)1 << (c))

static GFNT_Nfa * use_grammar(void) {
  GFNT_ReBuilder b;
  GFNT_Nfa * nfa;
  GFNT_Re h;
  GFNT_Re cons_mod;
  GFNT_Re medial;
  GFNT_Re dep_vow;
  GFNT_Re vow_mod;
  GFNT_Re fin_cons;
  GFNT_Re fin_mod;
  GFNT_Re start;
  GFNT_Re middle;
  GFNT_Re tail;
  GFNT_Re zwnj;
  GFNT_Re roots[US_COUNT];
  GFNT_Re any;
  GFNT_Re sym_tail;

  gfnt_re_init(&b);
  zwnj = OPT(SYM(UC_ZWNJ));
  h = SET(BIT(UC_H) | BIT(UC_HVM) | BIT(UC_SK));
  cons_mod = SEQ(SEQ(STAR(SYM(UC_CMABV)), STAR(SYM(UC_CMBLW))),
      STAR(SEQ(SEQ(ALT(SEQ(SEQ(SEQ(OPT(SYM(UC_ZWJ)), h), OPT(SYM(UC_ZWJ))),
                           SYM(UC_B)), SYM(UC_SUB)), OPT(SYM(UC_VS))),
          SEQ(STAR(SYM(UC_CMABV)), STAR(SYM(UC_CMBLW))))));
  medial = SEQ(SEQ(OPT(SYM(UC_MPRE)), OPT(SYM(UC_MABV))),
      SEQ(OPT(SYM(UC_MBLW)), OPT(SYM(UC_MPST))));
  dep_vow = SEQ(SEQ(STAR(SYM(UC_VPRE)), STAR(SYM(UC_VABV))),
      SEQ(STAR(SYM(UC_VBLW)), STAR(SYM(UC_VPST))));
  vow_mod = SEQ(SEQ(OPT(SYM(UC_VMH)), STAR(SYM(UC_VMPRE))),
      SEQ(SEQ(STAR(SYM(UC_VMABV)), STAR(SYM(UC_VMBLW))),
          STAR(SYM(UC_VMPST))));
  fin_cons = SEQ(SEQ(STAR(SYM(UC_FABV)), STAR(SYM(UC_FBLW))),
      STAR(SYM(UC_FPST)));
  fin_mod = ALT(SEQ(STAR(SYM(UC_FMABV)), STAR(SYM(UC_FMBLW))),
      OPT(SYM(UC_FMPST)));
  start = SEQ(SEQ(OPT(SET(BIT(UC_R) | BIT(UC_CS))),
                  SET(BIT(UC_B) | BIT(UC_GB) | BIT(UC_O))), OPT(SYM(UC_VS)));
  middle = SEQ(SEQ(SEQ(cons_mod, medial), SEQ(dep_vow, vow_mod)),
      STAR(SEQ(SYM(UC_SK), SYM(UC_B))));
  // A halant straight after the consonant modifiers may be followed by the rest
  // of the tail, with no dependent vowels between.
  tail = ALT(SEQ(SEQ(middle, fin_cons), fin_mod),
      SEQ(SEQ(SEQ(SEQ(cons_mod, medial), SYM(UC_HVM)), vow_mod),
          SEQ(fin_cons, fin_mod)));
  any = SET(~(uint64_t)0);

  roots[US_INDEPENDENT] = SEQ(SEQ(SET(BIT(UC_IND) | BIT(UC_O) | BIT(UC_WJ)),
      OPT(SYM(UC_VS))), zwnj);
  roots[US_VIRAMA_TERMINATED] = SEQ(SEQ(SEQ(SEQ(start, cons_mod), medial),
      SET(BIT(UC_H) | BIT(UC_HVM) | BIT(UC_SK) | BIT(UC_RK))), zwnj);
  roots[US_SAKOT_TERMINATED] = SEQ(SEQ(SEQ(start, middle), SYM(UC_SK)), zwnj);
  roots[US_STANDARD] = SEQ(SEQ(start, tail), zwnj);
  roots[US_NUMBER_JOINER_TERMINATED] = SEQ(SEQ(SEQ(SYM(UC_N), OPT(SYM(UC_VS))),
      SEQ(STAR(SEQ(SEQ(SYM(UC_HN), SYM(UC_N)), OPT(SYM(UC_VS)))),
          SYM(UC_HN))), zwnj);
  roots[US_NUMERAL] = SEQ(SEQ(SEQ(SYM(UC_N), OPT(SYM(UC_VS))),
      STAR(SEQ(SEQ(SYM(UC_HN), SYM(UC_N)), OPT(SYM(UC_VS))))), zwnj);
  sym_tail = ALT(SEQ(PLUS(SYM(UC_SMABV)), STAR(SYM(UC_SMBLW))),
      PLUS(SYM(UC_SMBLW)));
  roots[US_SYMBOL] = SEQ(SEQ(SEQ(SET(BIT(UC_S) | BIT(UC_GB)), OPT(SYM(UC_VS))),
      SEQ(SEQ(STAR(SYM(UC_CMABV)), STAR(SYM(UC_CMBLW))), OPT(sym_tail))), zwnj);
  roots[US_HIEROGLYPH] = SEQ(SEQ(SEQ(STAR(SYM(UC_SB)), SYM(UC_G)),
      SEQ(SEQ(OPT(SYM(UC_VS)), OPT(SYM(UC_HR))), SEQ(OPT(SYM(UC_HM)),
          STAR(SYM(UC_SE))))), SEQ(STAR(SEQ(SEQ(SYM(UC_J), STAR(SYM(UC_SB))),
          SEQ(SEQ(SYM(UC_G), OPT(SYM(UC_VS))), SEQ(SEQ(OPT(SYM(UC_HR)),
              OPT(SYM(UC_HM))), STAR(SYM(UC_SE)))))), OPT(SYM(UC_J))));
  roots[US_BROKEN] = SEQ(SEQ(OPT(SYM(UC_R)),
      ALT(ALT(ALT(tail, sym_tail), SYM(UC_HN)),
          SEQ(cons_mod, SET(BIT(UC_H) | BIT(UC_SK) | BIT(UC_RK))))), zwnj);
  roots[US_NON_CLUSTER] = any;
  nfa = gfnt_nfa_compile(&b, roots, US_COUNT);
  gfnt_re_free(&b);
  return nfa;
}

/* --- the plan -------------------------------------------------------------- */

typedef struct GFNT_UseData {
  uint32_t rphf_mask;
  void * arabic;                 // the joining masks, for a script that joins
  uint32_t topographical[4];     // isol, init, medi, fina
  GFNT_Nfa * grammar;
} GFNT_UseData;

static const GFNT_Tag use_arabic_features[] = {
  GFNT_TAG('i', 's', 'o', 'l'), GFNT_TAG('f', 'i', 'n', 'a'),
  GFNT_TAG('f', 'i', 'n', '2'), GFNT_TAG('f', 'i', 'n', '3'),
  GFNT_TAG('m', 'e', 'd', 'i'), GFNT_TAG('m', 'e', 'd', '2'),
  GFNT_TAG('i', 'n', 'i', 't'),
};

/** Whether this script joins its letters the way Arabic does. */
static bool use_arabic_joining(GFNT_Tag script) {
  static const GFNT_Tag joining[] = {
    GFNT_TAG('a', 'd', 'l', 'm'), GFNT_TAG('a', 'r', 'a', 'b'),
    GFNT_TAG('c', 'h', 'r', 's'), GFNT_TAG('r', 'o', 'h', 'g'),
    GFNT_TAG('m', 'a', 'n', 'd'), GFNT_TAG('m', 'a', 'n', 'i'),
    GFNT_TAG('m', 'o', 'n', 'g'), GFNT_TAG('n', 'k', 'o', ' '),
    GFNT_TAG('o', 'u', 'g', 'r'), GFNT_TAG('p', 'h', 'a', 'g'),
    GFNT_TAG('p', 'h', 'l', 'p'), GFNT_TAG('s', 'o', 'g', 'd'),
    GFNT_TAG('s', 'y', 'r', 'c'),
  };
  size_t i;

  for (i = 0; i < sizeof joining / sizeof joining[0]; i++) {
    if (joining[i] == script) {
      return true;
    }
  }
  return false;
}

/** Whether a halant after a character of this category joins a conjunct. */
static bool use_conjunct_part(uint8_t category) {
  return category == UC_B || category == UC_GB || category == UC_SUB
      || category == UC_CMABV || category == UC_CMBLW;
}

/** Whether the first character after @p i that is not a joiner is a base. */
static bool use_next_is_base(const GFNT_LBuffer * buf, size_t i) {
  for (i++; i < buf->len; i++) {
    if (buf->info[i].category != UC_ZWNJ && buf->info[i].category != UC_ZWJ) {
      return buf->info[i].category == UC_B;
    }
  }
  return false;
}

static void use_find_syllables(GFNT_ShapeCtx * ctx) {
  const GFNT_UseData * data = ctx->plan->shaper_data;
  GFNT_LBuffer * buf = ctx->buf;
  uint8_t * cats;
  size_t * where;
  size_t count = 0;
  size_t pos = 0;
  unsigned serial = 1;
  size_t i;

  if (!buf->len) {
    return;
  }
  cats = ctx->allocator->calloc_fn(ctx->allocator->ctx, buf->len, 1);
  where = ctx->allocator->calloc_fn(ctx->allocator->ctx, buf->len,
      sizeof(size_t));
  if (!cats || !where) {
    ctx->allocator->free_fn(ctx->allocator->ctx, cats);
    ctx->allocator->free_fn(ctx->allocator->ctx, where);
    ctx->oom = true;
    return;
  }
  // A joiner is transparent to the grammar: HarfBuzz lets one sit anywhere in
  // a syllable without changing which sequences are broken. So match over the
  // characters that are not joiners, and give each joiner its predecessor's
  // syllable.
  for (i = 0; i < buf->len; i++) {
    uint8_t c = buf->info[i].category;

    // A non-joiner after a virama is not transparent: it keeps the virama
    // from reaching the consonant after it.
    if ((c == UC_ZWNJ || c == UC_ZWJ) && count
        && (cats[count - 1] == UC_VMH
            || ((cats[count - 1] == UC_H || cats[count - 1] == UC_HVM
                    || cats[count - 1] == UC_SK)
                && (c == UC_ZWNJ || count < 2
                    || !use_conjunct_part(cats[count - 2]))))
        && use_next_is_base(buf, i)) {
      cats[count] = UC_CGJ;
      where[count++] = i;
    }
    else if (c != UC_ZWNJ && c != UC_ZWJ) {
      cats[count] = c;
      where[count++] = i;
    }
    buf->info[i].syllable = 0;
  }
  while (pos < count) {
    int token = US_NON_CLUSTER;
    size_t length = gfnt_nfa_match(data->grammar, cats, count, pos, &token);

    if (!length) {
      length = 1;
      token = US_NON_CLUSTER;
    }
    // A lone repha or stacker is not a broken syllable either.
    if (token == US_BROKEN && length == 1
        && (cats[pos] == UC_R || cats[pos] == UC_CS)) {
      token = US_NON_CLUSTER;
    }
    for (i = pos; i < pos + length; i++) {
      buf->info[where[i]].syllable = (uint8_t)((serial << 4) | (unsigned)token);
    }
    serial++;
    if (serial == 16) {
      serial = 1;
    }
    pos += length;
  }
  // A ZWJ after a halant ended that syllable, and goes with the next one if it is
  // a whole one.
  for (i = 0; i + 1 < buf->len; i++) {
    if (buf->info[i].category == UC_ZWJ
        && (buf->info[i].syllable & 15) == US_NON_CLUSTER
        && buf->info[i + 1].syllable
        && (buf->info[i + 1].syllable & 15) != US_NON_CLUSTER
        && (buf->info[i + 1].syllable & 15) != US_BROKEN
        && buf->info[i + 1].category != UC_ZWNJ
        && buf->info[i + 1].category != UC_ZWJ) {
      buf->info[i].syllable = buf->info[i + 1].syllable;
    }
  }
  for (i = 0; i < buf->len; i++) {
    if (buf->info[i].syllable) {
      continue;
    }
    // A joiner: it belongs to the syllable before it unless that is a broken
    // one or not a cluster. A ZWJ between two syllables then goes with the one
    // after, if that is a whole one. Otherwise it stands alone.
    bool prev_ok = i && buf->info[i - 1].category != UC_ZWNJ
        && (buf->info[i - 1].syllable & 15) != US_NON_CLUSTER;

    if (prev_ok) {
      buf->info[i].syllable = buf->info[i - 1].syllable;
    }
    else {
      // HarfBuzz marks a joiner left on its own as a broken syllable, unless a
      // broken syllable follows, which gets the dotted circle instead.
      int alone = US_NON_CLUSTER;

      if (buf->info[i].category == UC_ZWNJ && !(i + 1 < buf->len
              && (buf->info[i + 1].syllable & 15) == US_BROKEN)) {
        alone = US_BROKEN;
      }
      buf->info[i].syllable = (uint8_t)((serial << 4) | (unsigned)alone);
      serial++;
      if (serial == 16) {
        serial = 1;
      }
    }
  }
  ctx->allocator->free_fn(ctx->allocator->ctx, cats);
  ctx->allocator->free_fn(ctx->allocator->ctx, where);
}

static void use_setup_rphf_mask(GFNT_ShapeCtx * ctx) {
  const GFNT_UseData * data = ctx->plan->shaper_data;
  GFNT_LBuffer * buf = ctx->buf;
  size_t start = 0;

  if (!data->rphf_mask) {
    return;
  }
  while (start < buf->len) {
    size_t end = start + 1;
    size_t limit;
    size_t i;

    while (end < buf->len && buf->info[end].syllable == buf->info[start].syllable) {
      end++;
    }
    limit = buf->info[start].category == UC_R ? 1
        : (end - start < 3 ? end - start : 3);
    for (i = start; i < start + limit; i++) {
      buf->info[i].mask |= data->rphf_mask;
    }
    start = end;
  }
}

/** Isolated, initial, medial and final forms for scripts that do not join by letter. */
static void use_setup_topographical_masks(GFNT_ShapeCtx * ctx) {
  const GFNT_UseData * data = ctx->plan->shaper_data;
  GFNT_LBuffer * buf = ctx->buf;
  uint32_t masks[4];
  uint32_t all = 0;
  uint32_t other;
  size_t start = 0;
  size_t last_start = 0;
  int last_form = -1;   // -1 none, 0 isol, 1 init, 2 medi, 3 fina
  int i;

  if (data->arabic) {
    return;
  }
  for (i = 0; i < 4; i++) {
    masks[i] = data->topographical[i];
    if (masks[i] == ctx->plan->global_mask) {
      masks[i] = 0;
    }
    all |= masks[i];
  }
  if (!all) {
    return;
  }
  other = ~all;
  while (start < buf->len) {
    size_t end = start + 1;
    unsigned type;
    size_t k;

    while (end < buf->len && buf->info[end].syllable == buf->info[start].syllable) {
      end++;
    }
    type = buf->info[start].syllable & 0x0F;
    switch (type) {
      case US_INDEPENDENT:
      case US_SYMBOL:
      case US_NON_CLUSTER:
        last_form = -1;
        break;
      default: {
        bool join = last_form == 3 || last_form == 0;

        if (join) {
          last_form = last_form == 3 ? 2 : 1;
          for (k = last_start; k < start; k++) {
            buf->info[k].mask = (buf->info[k].mask & other) | masks[last_form];
          }
        }
        last_form = join ? 3 : 0;
        for (k = start; k < end; k++) {
          buf->info[k].mask = (buf->info[k].mask & other) | masks[last_form];
        }
        break;
      }
    }
    last_start = start;
    start = end;
  }
}

static void use_setup_syllables(GFNT_ShapeCtx * ctx) {
  use_find_syllables(ctx);
  use_setup_rphf_mask(ctx);
  use_setup_topographical_masks(ctx);
}

static bool use_is_halant(const GFNT_LInfo * info) {
  return (info->category == UC_H || info->category == UC_HVM)
      && !(info->props & GFNT_PROP_LIGATED);
}

static void use_record_rphf(GFNT_ShapeCtx * ctx) {
  const GFNT_UseData * data = ctx->plan->shaper_data;
  GFNT_LBuffer * buf = ctx->buf;
  size_t start = 0;

  if (!data->rphf_mask) {
    return;
  }
  while (start < buf->len) {
    size_t end = start + 1;
    size_t i;

    while (end < buf->len && buf->info[end].syllable == buf->info[start].syllable) {
      end++;
    }
    // A repha the feature has made is recorded as one.
    for (i = start; i < end && (buf->info[i].mask & data->rphf_mask); i++) {
      if (buf->info[i].props & GFNT_PROP_SUBSTITUTED) {
        buf->info[i].category = UC_R;
        break;
      }
    }
    start = end;
  }
}

static void use_record_pref(GFNT_ShapeCtx * ctx) {
  GFNT_LBuffer * buf = ctx->buf;
  size_t start = 0;

  while (start < buf->len) {
    size_t end = start + 1;
    size_t i;

    while (end < buf->len && buf->info[end].syllable == buf->info[start].syllable) {
      end++;
    }
    // A pre-base form the feature has made behaves as a pre-base vowel does.
    for (i = start; i < end; i++) {
      if (buf->info[i].props & GFNT_PROP_SUBSTITUTED) {
        buf->info[i].category = UC_VPRE;
        break;
      }
    }
    start = end;
  }
}

static bool use_post_base(uint8_t category) {
  switch (category) {
    case UC_FABV: case UC_FBLW: case UC_FPST: case UC_MABV: case UC_MBLW:
    case UC_MPST: case UC_MPRE: case UC_VABV: case UC_VBLW: case UC_VPST:
    case UC_VPRE: case UC_VMABV: case UC_VMBLW: case UC_VMPST: case UC_VMPRE:
      return true;
    default:
      return false;
  }
}

static void use_reorder_syllable(GFNT_LBuffer * buf, size_t start, size_t end) {
  GFNT_LInfo * info = buf->info;
  unsigned type = info[start].syllable & 0x0F;
  size_t j;
  size_t i;

  // Only a few kinds of syllable are reordered.
  if (type != US_VIRAMA_TERMINATED && type != US_SAKOT_TERMINATED
      && type != US_STANDARD && type != US_BROKEN) {
    return;
  }
  // A repha goes towards the end of the syllable, but before the first post-base glyph.
  if (info[start].category == UC_R && end - start > 1) {
    for (i = start + 1; i < end; i++) {
      bool post = use_post_base(info[i].category) || use_is_halant(&info[i]);

      if (post || i == end - 1) {
        GFNT_LInfo t;
        size_t k;

        if (post) {
          i--;
        }
        gfnt_merge_clusters(info, buf->len, start, i + 1);
        t = info[start];
        for (k = start; k < i; k++) {
          info[k] = info[k + 1];
          buf->pos[k] = buf->pos[k + 1];
        }
        info[i] = t;
        break;
      }
    }
  }
  // Pre-base vowels and modifiers come back to the front, or to just after a halant.
  j = start;
  for (i = start; i < end; i++) {
    uint8_t category = info[i].category;

    if (use_is_halant(&info[i])) {
      j = i + 1;
    }
    else if ((category == UC_VPRE || category == UC_VMPRE)
        && gfnt_l_lig_comp(&info[i]) == 0 && j < i) {
      GFNT_LInfo t;
      size_t k;

      gfnt_merge_clusters(info, buf->len, j, i + 1);
      t = info[i];
      for (k = i; k > j; k--) {
        info[k] = info[k - 1];
      }
      info[j] = t;
    }
  }
}

static void use_reorder(GFNT_ShapeCtx * ctx) {
  GFNT_LBuffer * buf = ctx->buf;
  size_t start = 0;

  if (!gfnt_syllabic_insert_dotted_circles(ctx, US_BROKEN, UC_B, UC_R, -1)) {
    return;
  }
  while (start < buf->len) {
    size_t end = start + 1;

    while (end < buf->len && buf->info[end].syllable == buf->info[start].syllable) {
      end++;
    }
    use_reorder_syllable(buf, start, end);
    start = end;
  }
}

static void use_collect_features(GFNT_Plan * plan) {
  static const GFNT_Tag basic[] = {
    GFNT_TAG('r', 'k', 'r', 'f'), GFNT_TAG('a', 'b', 'v', 'f'),
    GFNT_TAG('b', 'l', 'w', 'f'), GFNT_TAG('h', 'a', 'l', 'f'),
    GFNT_TAG('p', 's', 't', 'f'), GFNT_TAG('v', 'a', 't', 'u'),
    GFNT_TAG('c', 'j', 'c', 't'),
  };
  static const GFNT_Tag other[] = {
    GFNT_TAG('a', 'b', 'v', 's'), GFNT_TAG('b', 'l', 'w', 's'),
    GFNT_TAG('h', 'a', 'l', 'n'), GFNT_TAG('p', 'r', 'e', 's'),
    GFNT_TAG('p', 's', 't', 's'),
  };
  size_t i;

  // Before any lookup has run.
  gfnt_plan_pause(plan, use_setup_syllables);
  // The default glyph pre-processing group.
  gfnt_plan_enable(plan, GFNT_TAG('l', 'o', 'c', 'l'), GFNT_PF_PER_SYLLABLE, 1);
  gfnt_plan_enable(plan, GFNT_TAG('c', 'c', 'm', 'p'), GFNT_PF_PER_SYLLABLE, 1);
  gfnt_plan_enable(plan, GFNT_TAG('n', 'u', 'k', 't'), GFNT_PF_PER_SYLLABLE, 1);
  gfnt_plan_enable(plan, GFNT_TAG('a', 'k', 'h', 'n'),
      GFNT_PF_MANUAL_ZWJ | GFNT_PF_PER_SYLLABLE, 1);
  // The reordering group: rphf then pref, each followed by a look at what it did.
  gfnt_plan_pause(plan, gfnt_syllabic_clear_substitution_flags);
  gfnt_plan_add(plan, GFNT_TAG('r', 'p', 'h', 'f'),
      GFNT_PF_MANUAL_ZWJ | GFNT_PF_PER_SYLLABLE);
  gfnt_plan_pause(plan, use_record_rphf);
  gfnt_plan_pause(plan, gfnt_syllabic_clear_substitution_flags);
  gfnt_plan_enable(plan, GFNT_TAG('p', 'r', 'e', 'f'),
      GFNT_PF_MANUAL_ZWJ | GFNT_PF_PER_SYLLABLE, 1);
  gfnt_plan_pause(plan, use_record_pref);
  // The orthographic unit shaping group.
  for (i = 0; i < sizeof basic / sizeof basic[0]; i++) {
    gfnt_plan_enable(plan, basic[i], GFNT_PF_MANUAL_ZWJ | GFNT_PF_PER_SYLLABLE, 1);
  }
  gfnt_plan_pause(plan, use_reorder);
  gfnt_plan_pause(plan, gfnt_syllabic_clear_syllables);
  // The topographical features, then the standard typographic ones.
  for (i = 0; i < sizeof use_arabic_features / sizeof use_arabic_features[0]; i++) {
    gfnt_plan_add(plan, use_arabic_features[i], GFNT_PF_PER_SYLLABLE);
  }
  gfnt_plan_pause(plan, NULL);
  for (i = 0; i < sizeof other / sizeof other[0]; i++) {
    gfnt_plan_enable(plan, other[i], GFNT_PF_MANUAL_ZWJ, 1);
  }
}

static void * use_data_create(const GFNT_Plan * plan,
    const GFNT_Allocator * allocator) {
  GFNT_UseData * data = allocator->calloc_fn(allocator->ctx, 1, sizeof *data);

  if (!data) {
    return NULL;
  }
  data->rphf_mask = gfnt_plan_mask(plan, GFNT_TAG('r', 'p', 'h', 'f'));
  data->topographical[0] = gfnt_plan_mask(plan, GFNT_TAG('i', 's', 'o', 'l'));
  data->topographical[1] = gfnt_plan_mask(plan, GFNT_TAG('i', 'n', 'i', 't'));
  data->topographical[2] = gfnt_plan_mask(plan, GFNT_TAG('m', 'e', 'd', 'i'));
  data->topographical[3] = gfnt_plan_mask(plan, GFNT_TAG('f', 'i', 'n', 'a'));
  if (use_arabic_joining(plan->script)) {
    data->arabic = gfnt_arabic_masks_create(plan, allocator);
    if (!data->arabic) {
      allocator->free_fn(allocator->ctx, data);
      return NULL;
    }
  }
  data->grammar = use_grammar();
  if (!data->grammar) {
    if (data->arabic) {
      gfnt_arabic_masks_destroy(data->arabic, allocator);
    }
    allocator->free_fn(allocator->ctx, data);
    return NULL;
  }
  return data;
}

static void use_data_destroy(void * pointer, const GFNT_Allocator * allocator) {
  GFNT_UseData * data = pointer;

  if (data->arabic) {
    gfnt_arabic_masks_destroy(data->arabic, allocator);
  }
  gfnt_nfa_free(data->grammar);
  allocator->free_fn(allocator->ctx, data);
}

static void use_setup_masks(GFNT_ShapeCtx * ctx) {
  const GFNT_UseData * data = ctx->plan->shaper_data;
  size_t i;

  if (data->arabic) {
    gfnt_arabic_masks_apply(data->arabic, ctx);
  }
  for (i = 0; i < ctx->buf->len; i++) {
    ctx->buf->info[i].category = use_category(ctx->buf->info[i].unicode);
  }
}

/** Split vowels are not put back together; a few Bengali ones are. */
static bool use_compose(uint32_t a, uint32_t b, uint32_t * ab, void * ctx) {
  (void)ctx;
  if (guni_general_category(a) == GUNI_GC_NONSPACING_MARK
      || guni_general_category(a) == GUNI_GC_SPACING_MARK
      || guni_general_category(a) == GUNI_GC_ENCLOSING_MARK) {
    return false;
  }
  if (a == 0x09C7 && b == 0x09BE) {
    *ab = 0x09CB;
    return true;
  }
  *ab = guni_compose(a, b);
  return *ab != 0;
}

static const GFNT_NormHooks use_hooks = {
  .compose = use_compose,
};

const GFNT_Shaper gfnt_shaper_use = {
  .name = "use",
  .collect_features = use_collect_features,
  .data_create = use_data_create,
  .data_destroy = use_data_destroy,
  .preprocess_text = gfnt_vowel_constraints,
  .normalization = GFNT_NORM_COMPOSED_DIACRITICS_NO_SHORT_CIRCUIT,
  .normalization_hooks = &use_hooks,
  .setup_masks = use_setup_masks,
  .zero_width_marks = 1,
  .fallback_position = false,
};
