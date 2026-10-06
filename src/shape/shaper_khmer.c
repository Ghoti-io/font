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
 * The Khmer shaper.
 *
 * A Khmer syllable is a consonant (or an independent vowel, or a placeholder)
 * and, after it, any number of subscript consonants each introduced by a coeng,
 * then the vowel signs and the other signs that go with it. The coeng and Ra
 * pair goes before the base, and so does the left-hand vowel; everything else
 * stays where it was typed, for the font's `pref`, `blwf`, `abvf`, `pstf` and
 * `cfar` to deal with, all at once.
 *
 * The categories of the signs, and the order the grammar lets them in, are what
 * HarfBuzz's table and machine give, which the differential checks character by
 * character: the specification names them only roughly.
 */

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/unicode/char.h>
#include <string.h>
#include "nfa.h"
#include "plan.h"
#include "uprops.h"

// The categories of a Khmer character.
enum {
  KC_X, KC_C, KC_V, KC_RA, KC_COENG, KC_ZWJ, KC_ZWNJ, KC_PLACEHOLDER,
  KC_DOTTEDCIRCLE, KC_ROBATIC, KC_XGROUP, KC_YGROUP, KC_VPRE, KC_VABV, KC_VBLW,
  KC_VPST, KC_COUNT
};

enum { KS_CONSONANT, KS_BROKEN, KS_NON_KHMER, KS_COUNT };

enum { KF_PREF, KF_BLWF, KF_ABVF, KF_PSTF, KF_CFAR, KF_BASIC, KF_COUNT = KF_BASIC };

typedef struct KhmerData {
  uint32_t mask[KF_BASIC];
  GFNT_Nfa * grammar;
} KhmerData;

#define BIT(c) ((uint64_t)1 << (c))

static uint8_t khmer_category(uint32_t u) {
  if (u == 0x200C) {
    return KC_ZWNJ;
  }
  if (u == 0x200D) {
    return KC_ZWJ;
  }
  if (u == 0x25CC) {
    return KC_DOTTEDCIRCLE;
  }
  if (u == 0x00A0 || u == 0x2010 || u == 0x2011) {
    return KC_PLACEHOLDER;
  }
  // The digits and the phnaek muan take a robatic like a consonant, in HarfBuzz.
  if ((u >= 0x17E0 && u <= 0x17E9) || u == 0x17D9) {
    return KC_C;
  }
  if (u == 0x179A) {
    return KC_RA;
  }
  if (u >= 0x1780 && u <= 0x17A2) {
    return KC_C;
  }
  if (u >= 0x17A3 && u <= 0x17B3) {
    return KC_V;
  }
  switch (u) {
    case 0x17B6: case 0x17BF: case 0x17C0: case 0x17C4: case 0x17C5:
      return KC_VPST;
    case 0x17B7: case 0x17B8: case 0x17B9: case 0x17BA: case 0x17BE:
      return KC_VABV;
    case 0x17BB: case 0x17BC: case 0x17BD:
      return KC_VBLW;
    case 0x17C1: case 0x17C2: case 0x17C3:
      return KC_VPRE;
    case 0x17C6: case 0x17CB: case 0x17CD: case 0x17CE: case 0x17CF:
    case 0x17D0: case 0x17D1:
      return KC_XGROUP;
    case 0x17C7: case 0x17C8: case 0x17D3: case 0x17DD:
      return KC_YGROUP;
    case 0x17C9: case 0x17CA: case 0x17CC:
      return KC_ROBATIC;
    case 0x17D2:
      return KC_COENG;
    default:
      return KC_X;
  }
}

/* --- the grammar -------------------------------------------------------- */

#define SYM(c) gfnt_re_sym(&b, (c))
#define SEQ(x, y) gfnt_re_seq(&b, (x), (y))
#define ALT(x, y) gfnt_re_alt(&b, (x), (y))
#define STAR(x) gfnt_re_star(&b, (x))
#define OPT(x) gfnt_re_opt(&b, (x))
#define PLUS(x) gfnt_re_plus(&b, (x))
#define SET(m) gfnt_re_set(&b, (m))

static GFNT_Nfa * khmer_grammar(void) {
  GFNT_ReBuilder b;
  GFNT_Nfa * nfa;
  GFNT_Re c, cn, joiner, xgroup, ygroup, matra_group, tail, coeng_cn, any;
  GFNT_Re roots[KS_COUNT];

  gfnt_re_init(&b);
  c = SET(BIT(KC_C) | BIT(KC_RA) | BIT(KC_V));
  joiner = SET(BIT(KC_ZWJ) | BIT(KC_ZWNJ));
  cn = SEQ(c, OPT(SEQ(OPT(joiner), SYM(KC_ROBATIC))));
  xgroup = STAR(SEQ(STAR(joiner), SYM(KC_XGROUP)));
  ygroup = STAR(SYM(KC_YGROUP));
  // This grammar was experimentally extracted from what Uniscribe allows.
  matra_group = SEQ(SEQ(SEQ(OPT(SYM(KC_VPRE)), xgroup),
          SEQ(OPT(SYM(KC_VBLW)), xgroup)),
      SEQ(SEQ(OPT(SEQ(OPT(joiner), SYM(KC_VABV))), xgroup),
          OPT(SYM(KC_VPST))));
  tail = SEQ(SEQ(xgroup, matra_group), SEQ(xgroup, ygroup));
  coeng_cn = SEQ(SEQ(OPT(joiner), SYM(KC_COENG)), cn);
  any = SET(~(uint64_t)0);
  // A coeng with no consonant after it is accepted straight after a consonant, and ends the syllable;
  // anywhere else it is a broken cluster of its own, as in HarfBuzz.
  roots[KS_CONSONANT] = SEQ(SEQ(ALT(cn, SET(BIT(KC_PLACEHOLDER)
      | BIT(KC_DOTTEDCIRCLE))), STAR(coeng_cn)),
      ALT(tail, SEQ(OPT(joiner), SYM(KC_COENG))));
  {
    GFNT_Re robatic = SEQ(OPT(joiner), SYM(KC_ROBATIC));
    GFNT_Re coeng = SEQ(OPT(joiner), SYM(KC_COENG));

    roots[KS_BROKEN] = ALT(
        SEQ(SEQ(SEQ(OPT(robatic), STAR(coeng_cn)), tail), STAR(coeng_cn)),
        ALT(SEQ(ALT(SEQ(robatic, STAR(coeng_cn)), PLUS(coeng_cn)), OPT(coeng)),
            coeng));
  }
  roots[KS_NON_KHMER] = any;
  nfa = gfnt_nfa_compile(&b, roots, KS_COUNT);
  gfnt_re_free(&b);
  return nfa;
}

static void khmer_find_syllables(GFNT_ShapeCtx * ctx) {
  const KhmerData * data = ctx->plan->shaper_data;
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
    int token = KS_NON_KHMER;
    size_t length = gfnt_nfa_match(data->grammar, cats, buf->len, pos, &token);

    if (!length) {
      length = 1;
      token = KS_NON_KHMER;
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

static void khmer_move(GFNT_LBuffer * buf, size_t from, size_t to) {
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

static void khmer_reorder_syllable(GFNT_ShapeCtx * ctx, const KhmerData * data,
    size_t start, size_t end) {
  GFNT_LBuffer * buf = ctx->buf;
  GFNT_LInfo * info = buf->info;
  uint32_t mask = data->mask[KF_BLWF] | data->mask[KF_ABVF]
      | data->mask[KF_PSTF];
  unsigned num_coengs = 0;
  size_t i;

  // The post-base features apply after the first character.
  for (i = start + 1; i < end; i++) {
    info[i].mask |= mask;
  }
  for (i = start + 1; i < end; i++) {
    // When a coeng and a consonant are found, and fewer than two subscripts
    // have been, the pair is handled by the subscript's type.
    if (info[i].category == KC_COENG && num_coengs <= 2 && i + 1 < end) {
      num_coengs++;
      if (info[i + 1].category == KC_RA) {
        size_t j;

        // A coeng and Ra goes to the front, with the pre-base form.
        for (j = 0; j < 2; j++) {
          info[i + j].mask |= data->mask[KF_PREF];
        }
        gfnt_merge_clusters(info, buf->len, start, i + 2);
        khmer_move(buf, i + 1, start);
        khmer_move(buf, i + 1, start);
        // What follows is marked for 'cfar', which tells U+1784 U+17D2 U+179A
        // U+17D2 U+1782 from U+1784 U+17D2 U+1782 U+17D2 U+179A.
        if (data->mask[KF_CFAR]) {
          for (j = i + 2; j < end; j++) {
            info[j].mask |= data->mask[KF_CFAR];
          }
        }
        num_coengs = 2;   // Done.
      }
    }
    else if (info[i].category == KC_VPRE) {
      // The left-hand vowel goes to the front.
      gfnt_merge_clusters(info, buf->len, start, i + 1);
      khmer_move(buf, i, start);
    }
  }
}

static void khmer_setup_syllables(GFNT_ShapeCtx * ctx) {
  khmer_find_syllables(ctx);
}

static void khmer_reorder(GFNT_ShapeCtx * ctx) {
  const KhmerData * data = ctx->plan->shaper_data;
  GFNT_LBuffer * buf = ctx->buf;
  size_t start = 0;

  if (!gfnt_syllabic_insert_dotted_circles(ctx, KS_BROKEN, KC_DOTTEDCIRCLE, -1,
          -1)) {
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

      if (type == KS_CONSONANT || type == KS_BROKEN) {
        khmer_reorder_syllable(ctx, data, start, end);
      }
    }
    start = end;
  }
}

/* --- the plan ----------------------------------------------------------- */

static void khmer_collect_features(GFNT_Plan * plan) {
  static const GFNT_Tag basic[KF_BASIC] = {
    GFNT_TAG('p', 'r', 'e', 'f'), GFNT_TAG('b', 'l', 'w', 'f'),
    GFNT_TAG('a', 'b', 'v', 'f'), GFNT_TAG('p', 's', 't', 'f'),
    GFNT_TAG('c', 'f', 'a', 'r'),
  };
  static const GFNT_Tag other[] = {
    GFNT_TAG('p', 'r', 'e', 's'), GFNT_TAG('a', 'b', 'v', 's'),
    GFNT_TAG('b', 'l', 'w', 's'), GFNT_TAG('p', 's', 't', 's'),
  };
  size_t i;

  // Before any lookup has run.
  gfnt_plan_pause(plan, khmer_setup_syllables);
  gfnt_plan_pause(plan, khmer_reorder);
  // Uniscribe does not pause between the basic features.
  gfnt_plan_enable(plan, GFNT_TAG('l', 'o', 'c', 'l'), GFNT_PF_PER_SYLLABLE, 1);
  gfnt_plan_enable(plan, GFNT_TAG('c', 'c', 'm', 'p'), GFNT_PF_PER_SYLLABLE, 1);
  for (i = 0; i < KF_BASIC; i++) {
    gfnt_plan_add(plan, basic[i], GFNT_PF_MANUAL_ZWNJ | GFNT_PF_MANUAL_ZWJ);
  }
  gfnt_plan_pause(plan, gfnt_syllabic_clear_syllables);
  for (i = 0; i < sizeof other / sizeof other[0]; i++) {
    gfnt_plan_enable(plan, other[i], GFNT_PF_MANUAL_ZWNJ | GFNT_PF_MANUAL_ZWJ, 1);
  }
}

static void khmer_override_features(GFNT_Plan * plan) {
  // The Khmer specification has `clig` among the required features.
  gfnt_plan_enable(plan, GFNT_TAG('c', 'l', 'i', 'g'), 0, 1);
  gfnt_plan_enable(plan, GFNT_TAG('l', 'i', 'g', 'a'), 0, 0);
}

static void * khmer_data_create(const GFNT_Plan * plan,
    const GFNT_Allocator * allocator) {
  KhmerData * data = allocator->calloc_fn(allocator->ctx, 1, sizeof *data);
  static const GFNT_Tag basic[KF_BASIC] = {
    GFNT_TAG('p', 'r', 'e', 'f'), GFNT_TAG('b', 'l', 'w', 'f'),
    GFNT_TAG('a', 'b', 'v', 'f'), GFNT_TAG('p', 's', 't', 'f'),
    GFNT_TAG('c', 'f', 'a', 'r'),
  };
  size_t i;

  if (!data) {
    return NULL;
  }
  for (i = 0; i < KF_BASIC; i++) {
    data->mask[i] = gfnt_plan_found_mask(plan, basic[i]);
  }
  data->grammar = khmer_grammar();
  if (!data->grammar) {
    allocator->free_fn(allocator->ctx, data);
    return NULL;
  }
  return data;
}

static void khmer_data_destroy(void * pointer, const GFNT_Allocator * allocator) {
  KhmerData * data = pointer;

  gfnt_nfa_free(data->grammar);
  allocator->free_fn(allocator->ctx, data);
}

static void khmer_setup_masks(GFNT_ShapeCtx * ctx) {
  size_t i;

  for (i = 0; i < ctx->buf->len; i++) {
    ctx->buf->info[i].category = khmer_category(ctx->buf->info[i].unicode);
  }
}

/* --- normalisation ------------------------------------------------------ */

static bool khmer_decompose(uint32_t ab, uint32_t * a, uint32_t * b, void * ctx) {
  (void)ctx;
  switch (ab) {
    case 0x17BE: case 0x17BF: case 0x17C0: case 0x17C4: case 0x17C5:
      // The left-hand part these vowels lack is added in front.
      *a = 0x17C1;
      *b = ab;
      return true;
    default:
      return gfnt_unicode_decompose(ab, a, b, NULL);
  }
}

static const GFNT_NormHooks khmer_hooks = {
  .decompose = khmer_decompose,
};

const GFNT_Shaper gfnt_shaper_khmer = {
  .name = "khmer",
  .collect_features = khmer_collect_features,
  .override_features = khmer_override_features,
  .data_create = khmer_data_create,
  .data_destroy = khmer_data_destroy,
  .normalization = GFNT_NORM_DECOMPOSED,
  .normalization_hooks = &khmer_hooks,
  .setup_masks = khmer_setup_masks,
  .zero_width_marks = 0,
  .fallback_position = false,
};
