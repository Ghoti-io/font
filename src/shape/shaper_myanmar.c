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
 * The Myanmar shaper.
 *
 * A Myanmar syllable is a consonant, with an optional kinzi (Ra, asat and virama)
 * before it, then stacked consonants each after a virama, and then the medial
 * consonants, the vowel signs and the tone and other signs in an order the grammar
 * sets. The medial Ra and the left-hand vowel go before the base; the rest of the
 * signs are put in order by where they are drawn, so that the font's substitutions
 * see them the way it expects.
 *
 * The categories and the grammar are what HarfBuzz's table and machine give,
 * which the differential checks character by character.
 */

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/unicode/char.h>
#include <string.h>
#include "nfa.h"
#include "plan.h"
#include "uprops.h"

// The categories of a Myanmar character.
enum {
  MC_X, MC_C, MC_RA, MC_IV, MC_GB, MC_DOTTEDCIRCLE, MC_H, MC_ZWNJ, MC_ZWJ,
  MC_AS, MC_MY, MC_MR, MC_MW, MC_MH, MC_ML, MC_VPRE, MC_VABV, MC_VBLW,
  MC_VPST, MC_A, MC_DB, MC_SM, MC_PT, MC_VS, MC_P, MC_COUNT
};

enum { MS_CONSONANT, MS_PUNCTUATION, MS_BROKEN, MS_NON_MYANMAR, MS_COUNT };

// Where in the syllable a character is drawn.
enum {
  MP_START, MP_RA_TO_BECOME_REPH, MP_PRE_M, MP_PRE_C, MP_BASE_C, MP_AFTER_MAIN,
  MP_ABOVE_C, MP_BEFORE_SUB, MP_BELOW_C, MP_AFTER_SUB, MP_END
};

typedef struct MyanmarData {
  GFNT_Nfa * grammar;
} MyanmarData;

#define BIT(c) ((uint64_t)1 << (c))

static uint8_t myanmar_category(uint32_t u) {
  if (u == 0x200C) {
    return MC_ZWNJ;
  }
  if (u == 0x200D) {
    return MC_ZWJ;
  }
  if (u == 0x25CC) {
    return MC_DOTTEDCIRCLE;
  }
  if (u == 0x00A0 || u == 0x2010 || u == 0x2011) {
    return MC_GB;
  }
  if (u >= 0xFE00 && u <= 0xFE0F) {
    return MC_VS;
  }
  switch (u) {
    case 0x1004: case 0x101B: case 0x105A:
      return MC_RA;
    case 0x1039:
      return MC_H;
    case 0x103A:
      return MC_AS;
    case 0x103B: case 0x105E: case 0x105F:
      return MC_MY;
    case 0x103C:
      return MC_MR;
    case 0x103D: case 0x1082:
      return MC_MW;
    case 0x103E:
      return MC_MH;
    case 0x1060:
      return MC_ML;
    case 0x1031: case 0x1084:
      return MC_VPRE;
    case 0x102D: case 0x102E: case 0x1033: case 0x1034: case 0x1035:
    case 0x1071: case 0x1072: case 0x1073: case 0x1074: case 0x1085:
    case 0x1086: case 0x109D: case 0xA9E5:
      return MC_VABV;
    case 0x102F: case 0x1030: case 0x1058: case 0x1059:
      return MC_VBLW;
    case 0x102B: case 0x102C: case 0x1056: case 0x1057: case 0x1062:
    case 0x1067: case 0x1068: case 0x1083:
      return MC_VPST;
    case 0x1032: case 0x1036:
      return MC_A;
    case 0x1037: case 0xAA7C: case 0xAA7D:
      return MC_DB;
    case 0x1038: case 0x1087: case 0x1088: case 0x1089: case 0x108A:
    case 0x108B: case 0x108C: case 0x108D: case 0x108F: case 0x109A:
    case 0x109B: case 0x109C:
      return MC_SM;
    case 0x1063: case 0x1064: case 0x1069: case 0x106A: case 0x106B:
    case 0x106C: case 0x106D: case 0xAA7B:
      return MC_PT;
    case 0x104C: case 0x104D: case 0x104F: case 0x109E: case 0x109F:
    case 0xA9E6: case 0xAA70: case 0xAA77: case 0xAA78: case 0xAA79:
      return MC_P;
    default:
      break;
  }
  if ((u >= 0x1021 && u <= 0x102A) || (u >= 0x1052 && u <= 0x1055)) {
    return MC_IV;
  }
  if ((u >= 0x1000 && u <= 0x1020) || u == 0x103F || (u >= 0x1040 && u <= 0x104B)
      || u == 0x104E || u == 0x1050 || u == 0x1051 || (u >= 0x105B && u <= 0x105D)
      || u == 0x1061 || u == 0x1065 || u == 0x1066 || (u >= 0x106E && u <= 0x1070)
      || (u >= 0x1075 && u <= 0x1081) || u == 0x108E || (u >= 0x1090 && u <= 0x1099)
      || (u >= 0xA9E0 && u <= 0xA9E4) || (u >= 0xA9E7 && u <= 0xA9FE)
      || (u >= 0xAA60 && u <= 0xAA6F) || (u >= 0xAA71 && u <= 0xAA76)
      || u == 0xAA7A || u == 0xAA7E || u == 0xAA7F) {
    return MC_C;
  }
  return MC_X;
}

/* --- the grammar -------------------------------------------------------- */

#define SYM(c) gfnt_re_sym(&b, (c))
#define SEQ(x, y) gfnt_re_seq(&b, (x), (y))
#define ALT(x, y) gfnt_re_alt(&b, (x), (y))
#define STAR(x) gfnt_re_star(&b, (x))
#define OPT(x) gfnt_re_opt(&b, (x))
#define SET(m) gfnt_re_set(&b, (m))

static GFNT_Nfa * myanmar_grammar(void) {
  GFNT_ReBuilder b;
  GFNT_Nfa * nfa;
  GFNT_Re j, k, c, medial_group, main_vowel_group, post_vowel_group;
  GFNT_Re pwo_tone_group, complex_tail, syllable_tail, any;
  GFNT_Re roots[MS_COUNT];

  gfnt_re_init(&b);
  j = SET(BIT(MC_ZWJ) | BIT(MC_ZWNJ));
  k = SEQ(SEQ(SYM(MC_RA), SYM(MC_AS)), SYM(MC_H));
  c = SET(BIT(MC_C) | BIT(MC_RA));
  medial_group = SEQ(SEQ(OPT(SYM(MC_MY)), OPT(SYM(MC_AS))),
      SEQ(OPT(SYM(MC_MR)), OPT(SEQ(ALT(ALT(SEQ(SYM(MC_MW),
          SEQ(OPT(SYM(MC_MH)), OPT(SYM(MC_ML)))),
          SEQ(SYM(MC_MH), OPT(SYM(MC_ML)))), SYM(MC_ML)), OPT(SYM(MC_AS))))));
  main_vowel_group = SEQ(SEQ(STAR(SEQ(SYM(MC_VPRE), OPT(SYM(MC_VS)))),
          STAR(SYM(MC_VABV))),
      SEQ(SEQ(STAR(SYM(MC_VBLW)), STAR(SYM(MC_A))),
          OPT(SEQ(SYM(MC_DB), OPT(SYM(MC_AS))))));
  post_vowel_group = SEQ(SEQ(SYM(MC_VPST), OPT(SYM(MC_MH))),
      SEQ(SEQ(OPT(SYM(MC_ML)), STAR(SYM(MC_AS))),
          SEQ(SEQ(STAR(SYM(MC_VABV)), STAR(SYM(MC_A))),
              OPT(SEQ(SYM(MC_DB), OPT(SYM(MC_AS)))))));
  pwo_tone_group = SEQ(SEQ(SYM(MC_PT), STAR(SYM(MC_A))),
      SEQ(OPT(SYM(MC_DB)), OPT(SYM(MC_AS))));
  complex_tail = SEQ(SEQ(STAR(SYM(MC_AS)), medial_group),
      SEQ(SEQ(main_vowel_group, STAR(post_vowel_group)),
          SEQ(STAR(ALT(pwo_tone_group, SYM(MC_SM))), OPT(j))));
  syllable_tail = SEQ(STAR(SEQ(SYM(MC_H),
          SEQ(ALT(c, SYM(MC_IV)), OPT(SYM(MC_VS))))),
      ALT(SYM(MC_H), complex_tail));
  any = SET(~(uint64_t)0);
  roots[MS_CONSONANT] = SEQ(SEQ(OPT(k),
      SEQ(SET(BIT(MC_C) | BIT(MC_RA) | BIT(MC_IV) | BIT(MC_GB)
          | BIT(MC_DOTTEDCIRCLE)), OPT(SYM(MC_VS)))), syllable_tail);
  // HarfBuzz's punctuation cluster is a punctuation mark and a plain vowel, a
  // category that no character of its table has, so it never matches: the
  // vowel after a mark starts a syllable of its own.
  roots[MS_PUNCTUATION] = SEQ(SYM(MC_P), SYM(MC_COUNT));
  roots[MS_BROKEN] = SEQ(SEQ(OPT(k), OPT(SYM(MC_VS))), syllable_tail);
  roots[MS_NON_MYANMAR] = any;
  nfa = gfnt_nfa_compile(&b, roots, MS_COUNT);
  gfnt_re_free(&b);
  return nfa;
}

static void myanmar_find_syllables(GFNT_ShapeCtx * ctx) {
  const MyanmarData * data = ctx->plan->shaper_data;
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
  }
  while (pos < buf->len) {
    int token = MS_NON_MYANMAR;
    size_t length = gfnt_nfa_match(data->grammar, cats, buf->len, pos, &token);

    // A joiner by itself is not a broken syllable.
    if (!length || (token == MS_BROKEN && length == 1
            && (cats[pos] == MC_ZWJ || cats[pos] == MC_ZWNJ))) {
      length = 1;
      token = MS_NON_MYANMAR;
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

static bool myanmar_is_consonant(const GFNT_LInfo * info) {
  uint8_t c = info->category;

  return c == MC_C || c == MC_RA || c == MC_IV || c == MC_GB
      || c == MC_DOTTEDCIRCLE;
}

static void myanmar_move(GFNT_LBuffer * buf, size_t from, size_t to) {
  GFNT_LInfo t = buf->info[from];
  GFNT_LPos p = buf->pos[from];
  size_t k;

  for (k = from; k > to; k--) {
    buf->info[k] = buf->info[k - 1];
    buf->pos[k] = buf->pos[k - 1];
  }
  buf->info[to] = t;
  buf->pos[to] = p;
}

static void myanmar_reorder_syllable(GFNT_ShapeCtx * ctx, size_t start,
    size_t end) {
  GFNT_LBuffer * buf = ctx->buf;
  GFNT_LInfo * info = buf->info;
  size_t base = start;
  bool has_reph = false;
  size_t limit = start;
  size_t i;
  int pos;

  if (start + 3 <= end && info[start].category == MC_RA
      && info[start + 1].category == MC_AS && info[start + 2].category == MC_H) {
    limit += 3;
    base = start;
    has_reph = true;
  }
  if (!has_reph) {
    base = limit;
  }
  for (i = limit; i < end; i++) {
    if (myanmar_is_consonant(&info[i])) {
      base = i;
      break;
    }
  }

  // Reorder.
  i = start;
  for (; i < start + (has_reph ? 3u : 0u); i++) {
    info[i].position = MP_AFTER_MAIN;
  }
  for (; i < base; i++) {
    info[i].position = MP_PRE_C;
  }
  if (i < end) {
    info[i].position = MP_BASE_C;
    i++;
  }
  pos = MP_AFTER_MAIN;
  // The loop that follows is ugly, but it is all of Myanmar's reordering.
  for (; i < end; i++) {
    uint8_t cat = info[i].category;

    if (cat == MC_MR) {
      // Pre-base reordering.
      info[i].position = MP_PRE_C;
      continue;
    }
    if (cat == MC_VPRE) {
      // The left-hand vowel.
      info[i].position = MP_PRE_M;
      continue;
    }
    if (cat == MC_VS) {
      info[i].position = info[i - 1].position;
      continue;
    }
    if (pos == MP_AFTER_MAIN && cat == MC_VBLW) {
      pos = MP_BELOW_C;
      info[i].position = (uint8_t)pos;
      continue;
    }
    if (pos == MP_BELOW_C && cat == MC_A) {
      info[i].position = MP_BEFORE_SUB;
      continue;
    }
    if (pos == MP_BELOW_C && cat == MC_VBLW) {
      info[i].position = (uint8_t)pos;
      continue;
    }
    if (pos == MP_BELOW_C && cat != MC_A) {
      pos = MP_AFTER_SUB;
      info[i].position = (uint8_t)pos;
      continue;
    }
    info[i].position = (uint8_t)pos;
  }

  // Sit tight: a stable sort by position, merging the clusters of what moves.
  for (i = start + 1; i < end; i++) {
    size_t j = i;

    while (j > start && info[j - 1].position > info[i].position) {
      j--;
    }
    if (j < i) {
      gfnt_merge_clusters(info, buf->len, j, i + 1);
      myanmar_move(buf, i, j);
    }
  }
  // HarfBuzz leaves a run of left-hand vowels in the opposite order to the one
  // it was written in, each keeping the variation selector after it.
  i = start;
  while (i < end) {
    size_t run_end = i;

    if (info[i].position != MP_PRE_M) {
      i++;
      continue;
    }
    while (run_end < end && info[run_end].position == MP_PRE_M) {
      run_end++;
    }
    // Move each later block, the vowel and what follows it, to the front in turn.
    {
      size_t k = i;

      while (k < run_end) {
        size_t block_end = k + 1;
        size_t m;

        while (block_end < run_end && info[block_end].category == MC_VS) {
          block_end++;
        }
        for (m = 0; k > i && m < block_end - k; m++) {
          myanmar_move(buf, k + m, i + m);
        }
        k = block_end;
      }
    }
    i = run_end;
  }
}

static void myanmar_setup_syllables(GFNT_ShapeCtx * ctx) {
  myanmar_find_syllables(ctx);
}

static void myanmar_reorder(GFNT_ShapeCtx * ctx) {
  GFNT_LBuffer * buf = ctx->buf;
  size_t start = 0;

  if (!gfnt_syllabic_insert_dotted_circles(ctx, MS_BROKEN, MC_GB, MC_RA, MP_BASE_C)) {
    return;
  }
  while (start < buf->len) {
    size_t end = start + 1;

    while (end < buf->len
        && buf->info[end].syllable == buf->info[start].syllable) {
      end++;
    }
    {
      unsigned type = buf->info[start].syllable & 0x0F;

      if (type == MS_CONSONANT || type == MS_BROKEN) {
        myanmar_reorder_syllable(ctx, start, end);
      }
    }
    start = end;
  }
}

/* --- the plan ----------------------------------------------------------- */

static void myanmar_collect_features(GFNT_Plan * plan) {
  static const GFNT_Tag basic[] = {
    GFNT_TAG('r', 'p', 'h', 'f'), GFNT_TAG('p', 'r', 'e', 'f'),
    GFNT_TAG('b', 'l', 'w', 'f'), GFNT_TAG('p', 's', 't', 'f'),
  };
  static const GFNT_Tag other[] = {
    GFNT_TAG('p', 'r', 'e', 's'), GFNT_TAG('a', 'b', 'v', 's'),
    GFNT_TAG('b', 'l', 'w', 's'), GFNT_TAG('p', 's', 't', 's'),
  };
  size_t i;

  // Before any lookup has run.
  gfnt_plan_pause(plan, myanmar_setup_syllables);
  gfnt_plan_enable(plan, GFNT_TAG('l', 'o', 'c', 'l'), GFNT_PF_PER_SYLLABLE, 1);
  // The Indic specifications do not require ccmp, but if there is a use of it, it
  // is typically at the beginning.
  gfnt_plan_enable(plan, GFNT_TAG('c', 'c', 'm', 'p'), GFNT_PF_PER_SYLLABLE, 1);
  gfnt_plan_pause(plan, myanmar_reorder);
  for (i = 0; i < sizeof basic / sizeof basic[0]; i++) {
    gfnt_plan_enable(plan, basic[i], GFNT_PF_MANUAL_ZWJ, 1);
    gfnt_plan_pause(plan, NULL);
  }
  gfnt_plan_pause(plan, gfnt_syllabic_clear_syllables);
  for (i = 0; i < sizeof other / sizeof other[0]; i++) {
    gfnt_plan_enable(plan, other[i], GFNT_PF_MANUAL_ZWJ, 1);
  }
}

static void * myanmar_data_create(const GFNT_Plan * plan,
    const GFNT_Allocator * allocator) {
  MyanmarData * data = allocator->calloc_fn(allocator->ctx, 1, sizeof *data);

  (void)plan;
  if (!data) {
    return NULL;
  }
  data->grammar = myanmar_grammar();
  if (!data->grammar) {
    allocator->free_fn(allocator->ctx, data);
    return NULL;
  }
  return data;
}

static void myanmar_data_destroy(void * pointer, const GFNT_Allocator * allocator) {
  MyanmarData * data = pointer;

  gfnt_nfa_free(data->grammar);
  allocator->free_fn(allocator->ctx, data);
}

static void myanmar_setup_masks(GFNT_ShapeCtx * ctx) {
  size_t i;

  for (i = 0; i < ctx->buf->len; i++) {
    ctx->buf->info[i].category = myanmar_category(ctx->buf->info[i].unicode);
  }
}

const GFNT_Shaper gfnt_shaper_myanmar = {
  .name = "myanmar",
  .collect_features = myanmar_collect_features,
  .data_create = myanmar_data_create,
  .data_destroy = myanmar_data_destroy,
  .normalization = GFNT_NORM_COMPOSED_DIACRITICS_NO_SHORT_CIRCUIT,
  .setup_masks = myanmar_setup_masks,
  .zero_width_marks = 0,
  .fallback_position = false,
};
