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
 * The default shaper: the plan (which features, which lookups, in what order) and
 * the pipeline that runs it. See shape.h for what is and is not done.
 */

#include <ghoti.io/font/featurevar.h>
#include <ghoti.io/font/shape.h>
#include <stdlib.h>
#include <string.h>
#include "../layout_tables/layout.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"
#include "../var/var.h"
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/unicode/char.h>
#include "fallback.h"
#include "normalize.h"
#include <ghoti.io/unicode/bidi.h>
#include "plan.h"
#include "vertical.h"
#include "script_tags.h"
#include "uprops.h"

/* --- the pipeline -------------------------------------------------------- */

void gfnt_shaped_run_free(GFNT_ShapedRun * run) {
  if (run && run->glyphs && run->allocator) {
    run->allocator->free_fn(run->allocator->ctx, run->glyphs);
  }
  if (run) {
    memset(run, 0, sizeof *run);
  }
}

/**
 * Run one table's lookups, stage by stage. After each stage of substitution the
 * shaper's work for that pause is done, when it has any.
 */
static GFNT_Result gfnt_shape_apply(GFNT_LApply * c, const GFNT_PlanTable * pt,
    size_t stages, GFNT_ShapeCtx * ctx, GFNT_Error * error, size_t * out_ran) {
  size_t i;
  size_t stage;

  for (stage = 0; stage < stages; stage++) {
    for (i = 0; i < pt->count; i++) {
      const GFNT_PlanLookup * l = &pt->lookups[i];

      if (l->stage != stage) {
        continue;
      }
      c->lookup_mask = l->mask;
      c->auto_zwnj = l->auto_zwnj;
      c->auto_zwj = l->auto_zwj;
      c->per_syllable = l->per_syllable;
      gfnt_l_apply_lookup_to_buffer(c, l->index);
      (*out_ran)++;
      if (c->fault.bad) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, c->fault.table,
            c->fault.offset, GFNT_GLYPH_NONE,
            "a layout lookup reads past its own table");
      }
      if (c->buf->limit) {
        return gfnt_error_set(error, GFNT_ERR_LIMIT, c->lt->tag, 0,
            GFNT_GLYPH_NONE,
            "a lookup grew the run past sixty-four times its length");
      }
      if (c->buf->oom) {
        return gfnt_error_set(error, GFNT_ERR_OOM, c->lt->tag, 0,
            GFNT_GLYPH_NONE, "no memory while applying a layout lookup");
      }
    }
    if (ctx && ctx->plan->pause[stage]) {
      ctx->plan->pause[stage](ctx);
      if (ctx->oom) {
        return gfnt_error_set(error, GFNT_ERR_OOM, c->lt->tag, 0,
            GFNT_GLYPH_NONE, "no memory in a shaper's step");
      }
    }
  }
  return GFNT_OK;
}

/**
 * Size the spaces the font has no glyph for, which were mapped to its space.
 *
 * HarfBuzz's `_hb_ot_shape_fallback_spaces()`: an em fraction of the font's own
 * em, a figure space as wide as a digit, a punctuation space as wide as a full
 * stop, a narrow space half the width the space glyph has. A space that the font
 * does map, or that a lookup has ligated, is left as the font says.
 */
static void gfnt_fallback_spaces(const GFNT_Face * face, GFNT_LBuffer * buf,
    const GFNT_Variation * variation, bool vertical) {
  uint16_t upem = 0;
  size_t i;

  (void)gfnt_face_units_per_em(face, &upem, NULL);
  for (i = 0; i < buf->len; i++) {
    GFNT_LInfo * info = &buf->info[i];
    GFNT_LPos * pos = &buf->pos[i];

    if (!info->space || (info->props & GFNT_PROP_LIGATED)) {
      continue;
    }
    switch (info->space) {
      case GFNT_SPACE_EM:
      case GFNT_SPACE_EM_2:
      case GFNT_SPACE_EM_3:
      case GFNT_SPACE_EM_4:
      case GFNT_SPACE_EM_5:
      case GFNT_SPACE_EM_6:
      case GFNT_SPACE_EM_16:
        {
          int32_t em = (int32_t)(((int64_t)upem + info->space / 2)
              / info->space);

          if (vertical) {
            pos->y_advance = -em;
          }
          else {
            pos->x_advance = em;
          }
        }
        break;
      case GFNT_SPACE_4_EM_18:
        if (vertical) {
          pos->y_advance = -(int32_t)((int64_t)upem * 4 / 18);
        }
        else {
          pos->x_advance = (int32_t)((int64_t)upem * 4 / 18);
        }
        break;
      case GFNT_SPACE_FIGURE:
      case GFNT_SPACE_PUNCTUATION: {
        static const uint32_t figure[] = {'0', '1', '2', '3', '4', '5', '6',
            '7', '8', '9'};
        static const uint32_t punctuation[] = {'.', ','};
        const uint32_t * probe = info->space == GFNT_SPACE_FIGURE
            ? figure : punctuation;
        size_t n = info->space == GFNT_SPACE_FIGURE ? 10 : 2;
        size_t k;

        for (k = 0; k < n; k++) {
          uint32_t glyph = 0;
          int32_t advance = 0;

          if (gfnt_face_glyph_for_codepoint(face, probe[k], &glyph, NULL)
                  == GFNT_OK && glyph
              && (vertical
                  ? gfnt_vertical_advance(face, glyph, variation, &advance)
                  : gfnt_face_glyph_advance(face, glyph, variation, &advance,
                      NULL)) == GFNT_OK) {
            if (vertical) {
              pos->y_advance = advance;
            }
            else {
              pos->x_advance = advance;
            }
            break;
          }
        }
        break;
      }
      case GFNT_SPACE_NARROW:
        if (vertical) {
          pos->y_advance /= 2;
        }
        else {
          pos->x_advance /= 2;
        }
        break;
      default:
        break;
    }
  }
}

/**
 * Mark the characters that continue the grapheme before them but are not marks:
 * a ZWJ and the emoji it joins, an emoji modifier, a half-width katakana voiced
 * mark and a tag character.
 */
static void gfnt_mark_continuations(GFNT_LInfo * chars, size_t count) {
  size_t i;

  for (i = 1; i < count; i++) {
    uint32_t u = chars[i].unicode;

    if (chars[i].flags & GFNT_GF_ZWJ) {
      chars[i].flags |= GFNT_GF_CONTINUATION;
      if (i + 1 < count && guni_has_property(chars[i + 1].unicode,
              GUNI_PROP_EXTENDED_PICTOGRAPHIC)) {
        i++;
        chars[i].flags |= GFNT_GF_CONTINUATION;
      }
    }
    else if ((u >= 0x1F3FB && u <= 0x1F3FF) || (u >= 0xFF9E && u <= 0xFF9F)
        || (u >= 0xE0020 && u <= 0xE007F)) {
      chars[i].flags |= GFNT_GF_CONTINUATION;
    }
  }
}

/** Reverse the run grapheme by grapheme: each cluster keeps its own order. */
static void gfnt_reverse_clusters(GFNT_LInfo * chars, size_t count) {
  size_t i;
  size_t start = 0;

  for (i = 0; i < count / 2; i++) {
    GFNT_LInfo t = chars[i];

    chars[i] = chars[count - 1 - i];
    chars[count - 1 - i] = t;
  }
  for (i = 1; i <= count; i++) {
    if (i == count || chars[i].cluster != chars[i - 1].cluster) {
      size_t lo = start;
      size_t hi = i - 1;

      while (lo < hi) {
        GFNT_LInfo t = chars[lo];

        chars[lo++] = chars[hi];
        chars[hi--] = t;
      }
      start = i;
    }
  }
}

/**
 * Switch the fraction features on around a FRACTION SLASH: the digits before it
 * take the numerator form, the digits after it the denominator form, and the
 * slash itself the fraction form.
 */
static void gfnt_setup_fraction_masks(const GFNT_Plan * plan,
    GFNT_LBuffer * buf) {
  size_t i;

  if (!plan->has_frac) {
    return;
  }
  for (i = 0; i < buf->len; i++) {
    if (buf->info[i].unicode == 0x2044) {
      size_t start = i;
      size_t end = i + 1;
      size_t j;

      while (start && buf->info[start - 1].gc == GUNI_GC_DECIMAL_NUMBER) {
        start--;
      }
      while (end < buf->len && buf->info[end].gc == GUNI_GC_DECIMAL_NUMBER) {
        end++;
      }
      for (j = start; j < i; j++) {
        buf->info[j].mask |= plan->numr_mask | plan->frac_mask;
      }
      buf->info[i].mask |= plan->frac_mask;
      for (j = i + 1; j < end; j++) {
        buf->info[j].mask |= plan->dnom_mask | plan->frac_mask;
      }
      i = end;
    }
  }
}

/**
 * Take a mark's advance away. If no `GPOS` has placed it, it is moved back by the
 * advance first, so that it hangs over the glyph before it instead of after.
 */
/**
 * The vertical presentation form of a character, which is what HarfBuzz puts in
 * its place in a vertical run when the font has no `vert` feature of its own.
 */
static uint32_t gfnt_vertical_form(uint32_t c) {
  static const struct { uint16_t from, to; } forms[] = {
    {0x2013, 0xFE32}, {0x2014, 0xFE31}, {0x2025, 0xFE30}, {0x2026, 0xFE19},
    {0x3001, 0xFE11}, {0x3002, 0xFE12}, {0x3008, 0xFE3F}, {0x3009, 0xFE40},
    {0x300A, 0xFE3D}, {0x300B, 0xFE3E}, {0x300C, 0xFE41}, {0x300D, 0xFE42},
    {0x300E, 0xFE43}, {0x300F, 0xFE44}, {0x3010, 0xFE3B}, {0x3011, 0xFE3C},
    {0x3014, 0xFE39}, {0x3015, 0xFE3A}, {0x3016, 0xFE17}, {0x3017, 0xFE18},
    {0xFE4F, 0xFE34}, {0xFF01, 0xFE15}, {0xFF08, 0xFE35}, {0xFF09, 0xFE36},
    {0xFF0C, 0xFE10}, {0xFF1A, 0xFE13}, {0xFF1B, 0xFE14}, {0xFF1F, 0xFE16},
    {0xFF3B, 0xFE47}, {0xFF3D, 0xFE48}, {0xFF3F, 0xFE33}, {0xFF5B, 0xFE37},
    {0xFF5D, 0xFE38},
  };
  size_t i;

  for (i = 0; i < sizeof forms / sizeof forms[0]; i++) {
    if (forms[i].from == c) {
      return forms[i].to;
    }
  }
  return c;
}

static void gfnt_zero_mark_widths(GFNT_LBuffer * buf, bool adjust) {
  size_t i;

  for (i = 0; i < buf->len; i++) {
    if (gfnt_l_is_mark(&buf->info[i])) {
      if (adjust) {
        buf->pos[i].x_offset -= buf->pos[i].x_advance;
        buf->pos[i].y_offset -= buf->pos[i].y_advance;
      }
      buf->pos[i].x_advance = 0;
      buf->pos[i].y_advance = 0;
    }
  }
}

GFNT_Tag gfnt_shape_script_of(const uint32_t * codepoints, size_t count) {
  size_t i;

  for (i = 0; codepoints && i < count; i++) {
    uint32_t script = (uint32_t)guni_script(codepoints[i]);
    const char * tag;

    if (script >= GFNT_SCRIPT_TAG_COUNT) {
      continue;
    }
    tag = gfnt_script_tags[script];
    if (tag[0]) {
      return GFNT_TAG(tag[0], tag[1], tag[2], tag[3]);
    }
  }
  return 0;
}

GFNT_Direction gfnt_shape_script_direction(GFNT_Tag script) {
  return gfnt_script_native_rtl(script) ? GFNT_DIRECTION_RTL
                                        : GFNT_DIRECTION_LTR;
}

GFNT_Result gfnt_face_shape(const GFNT_Face * face, const uint32_t * codepoints,
    size_t count, const GFNT_ShapeOptions * options,
    const GFNT_Allocator * allocator, GFNT_ShapedRun * out_run,
    GFNT_Error * error) {
  static const GFNT_ShapeOptions defaults;
  GFNT_Plan plan;
  GFNT_ShapeCtx ctx;
  GFNT_ShapeOptions resolved;
  GFNT_NormHooks hooks;
  GFNT_LBuffer buf;
  GFNT_LayoutTable gsub_table;
  GFNT_LayoutTable gpos_table;
  GFNT_Gdef gdef;
  GFNT_LApply gsub;
  GFNT_LApply gpos;
  GFNT_Result result;
  GFNT_ShapedRun run;
  uint32_t space = 0;
  GFNT_LInfo * chars = NULL;
  size_t chars_len = 0;
  size_t chars_capacity = 0;
  bool native_rtl;
  bool reversed_first;
  bool vertical;
  size_t i;

  gfnt_error_clear(error);
  if (!face || !out_run || (count && !codepoints)) {
    return GFNT_ERR_INVALID;
  }
  if (!options) {
    options = &defaults;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  if (count > GFNT_LAYOUT_MAX_LEN) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "a run longer than the shaper takes");
  }
  memset(&run, 0, sizeof run);
  run.allocator = allocator;
  if (count == 0) {
    *out_run = run;
    return GFNT_OK;
  }
  vertical = options->direction == GFNT_DIRECTION_TTB
      || options->direction == GFNT_DIRECTION_BTT;
  if (vertical && options->variation && options->variation->count) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
        "vertical text at a location in the design space is not shaped: the "
        "vertical metrics would have to move with it");
  }
  // No script named: the text's own, as HarfBuzz guesses it.
  resolved = *options;
  if (!resolved.script) {
    resolved.script = gfnt_shape_script_of(codepoints, count);
  }
  options = &resolved;

  result = gfnt_plan_build(face, options, count, &plan, allocator, error);
  if (result != GFNT_OK) {
    return result;
  }
  memset(&ctx, 0, sizeof ctx);
  ctx.face = face;
  ctx.options = options;
  ctx.plan = &plan;
  ctx.buf = &buf;
  ctx.gdef = &gdef;
  ctx.allocator = allocator;
  memset(&buf, 0, sizeof buf);
  gfnt_gdef_open(face, &gdef);
  memset(&gsub, 0, sizeof gsub);
  memset(&gpos, 0, sizeof gpos);

  // The characters, with what the shaper needs to know about each. A mark, a ZWJ
  // and an emoji modifier belong to the grapheme before them, and a grapheme is
  // never split across clusters, so that a caret or a selection cannot land
  // between a letter and its accent.
  chars = allocator->calloc_fn(allocator->ctx, count, sizeof *chars);
  if (!chars) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the characters of the run");
    goto done;
  }
  chars_len = count;
  chars_capacity = count;
  for (i = 0; i < count; i++) {
    chars[i].unicode = codepoints[i];
    chars[i].cluster = (uint32_t)i;
    gfnt_u_set_props(&chars[i]);
  }
  gfnt_mark_continuations(chars, count);
  for (i = 1; i < count; i++) {
    if (chars[i].flags & GFNT_GF_CONTINUATION) {
      chars[i].cluster = chars[i - 1].cluster;
    }
  }

  // The shaper works in the script's own direction. Text the other way is turned
  // round first, a grapheme at a time so that each keeps its base before its
  // marks; a run in the script's own direction is turned round at the end.
  native_rtl = plan.native_rtl;
  ctx.native_rtl = native_rtl;
  reversed_first = (options->direction == GFNT_DIRECTION_RTL
                       || options->direction == GFNT_DIRECTION_BTT) != native_rtl;
  if (reversed_first) {
    gfnt_reverse_clusters(chars, count);
  }

  // The script's shaper may rewrite the text before it is normalised.
  if (plan.shaper->preprocess_text) {
    plan.shaper->preprocess_text(&ctx, &chars, &chars_len, &chars_capacity);
    if (ctx.oom) {
      result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory while preparing the run");
      goto done;
    }
  }

  // Text that runs backwards shows a bracket the other way round, if the font has
  // the glyph for its mirror image.
  if (options->direction == GFNT_DIRECTION_RTL
      || options->direction == GFNT_DIRECTION_BTT) {
    for (i = 0; i < chars_len; i++) {
      uint32_t mirrored = guni_bidi_mirror(chars[i].unicode);
      uint32_t glyph = 0;

      if (mirrored != chars[i].unicode
          && gfnt_face_glyph_for_codepoint(face, mirrored, &glyph, NULL)
              == GFNT_OK && glyph) {
        chars[i].unicode = mirrored;
      }
    }
  }

  if (vertical && !gfnt_plan_found_mask(&plan, GFNT_TAG('v', 'e', 'r', 't'))) {
    for (i = 0; i < chars_len; i++) {
      uint32_t form = gfnt_vertical_form(chars[i].unicode);
      uint32_t glyph = 0;

      if (form != chars[i].unicode
          && gfnt_face_glyph_for_codepoint(face, form, &glyph, NULL)
              == GFNT_OK && glyph) {
        chars[i].unicode = form;
      }
    }
  }

  // Normalise: the glyphs come from here, with the composites the font has made
  // and the characters it lacks taken apart.
  if (plan.shaper->normalization_hooks) {
    hooks = *plan.shaper->normalization_hooks;
    hooks.ctx = &ctx;
  }
  if (!gfnt_normalize(face, &chars, &chars_len, &chars_capacity, allocator,
          plan.shaper->normalization,
          plan.shaper->normalization_hooks ? &hooks : NULL)) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory while normalising the run");
    goto done;
  }
  if (!gfnt_lbuf_init(&buf, allocator, chars_len)) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the glyph buffer");
    goto done;
  }
  for (i = 0; i < chars_len; i++) {
    buf.info[i] = chars[i];
    buf.info[i].mask = plan.global_mask;
    buf.info[i].props = 0;
    buf.info[i].lig_props = 0;
  }
  (void)gfnt_face_glyph_for_codepoint(face, 0x20, &space, NULL);
  count = chars_len;

  // Which glyphs the fraction features apply to, and then the script's own masks.
  gfnt_setup_fraction_masks(&plan, &buf);
  if (plan.shaper->setup_masks) {
    plan.shaper->setup_masks(&ctx);
    if (ctx.oom) {
      result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory while setting up the masks");
      goto done;
    }
  }

  // Features with a range of their own: set their value on the code points in it.
  for (i = 0; i < options->feature_count; i++) {
    const GFNT_ShapeFeature * f = &options->features[i];
    const GFNT_PlanFeature * pf = gfnt_plan_find(&plan, f->tag);
    size_t k;

    if (!pf || !pf->mask || (pf->flags & GFNT_PF_GLOBAL)) {
      continue;
    }
    for (k = 0; k < count; k++) {
      if (buf.info[k].cluster >= f->start
          && (f->end == GFNT_SHAPE_END || buf.info[k].cluster < f->end)) {
        buf.info[k].mask = (buf.info[k].mask & ~pf->mask)
            | ((f->value << pf->shift) & pf->mask);
      }
    }
  }

  // GSUB.
  if (plan.tables[0].present) {
    gsub.buf = &buf;
    gsub.face = face;
    gsub.variation = options->variation;
    gsub.gdef = &gdef;
    gsub.is_gpos = false;
    gsub.rtl = native_rtl;
    result = gfnt_layout_open(face, GFNT_TAG_GSUB, &gsub_table, error);
    if (result != GFNT_OK) {
      goto done;
    }
    gsub.lt = &gsub_table;
    ctx.gsub = &gsub;
  }
  // Glyph properties, from GDEF where it has classes, from the character where
  // it does not.
  {
    GFNT_LApply props;

    memset(&props, 0, sizeof props);
    props.gdef = &gdef;
    for (i = 0; i < buf.len; i++) {
      GFNT_LInfo * info = &buf.info[i];

      if (gdef.has_glyph_classes) {
        info->props = gfnt_gdef_props(&props, info->glyph);
      }
      else {
        info->props = info->gc == GUNI_GC_NONSPACING_MARK
                && !(info->flags & GFNT_GF_DEFAULT_IGNORABLE)
            ? GFNT_PROP_MARK : GFNT_PROP_BASE;
      }
      info->lig_props = 0;
    }
  }
  if (plan.tables[0].present) {
    result = gfnt_shape_apply(&gsub, &plan.tables[0], plan.gsub_stages, &ctx,
        error, &run.gsub_lookups);
    if (result != GFNT_OK) {
      goto done;
    }
  }
  else {
    // No `GSUB`, but the shaper's steps between the stages are still its own work.
    size_t stage;

    for (stage = 0; stage < plan.gsub_stages; stage++) {
      if (plan.pause[stage]) {
        plan.pause[stage](&ctx);
        if (ctx.oom) {
          result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
              "no memory in a shaper's step");
          goto done;
        }
      }
    }
  }

  // Default-ignorables: drawn as nothing. The space glyph stands in for them if
  // the font has one, so that the run keeps its length and its clusters; a font
  // with no space glyph has them taken out instead.
  {
    size_t kept = 0;

    for (i = 0; i < buf.len; i++) {
      GFNT_LInfo * info = &buf.info[i];

      if ((info->flags & GFNT_GF_DEFAULT_IGNORABLE)
          && !(info->props & GFNT_PROP_SUBSTITUTED)) {
        if (space) {
          info->glyph = space;
        }
        else {
          // Its cluster survives in a neighbour, or is merged into one.
          uint32_t cluster = info->cluster;

          if (i + 1 < buf.len && cluster == buf.info[i + 1].cluster) {
            continue;
          }
          if (kept) {
            if (cluster < buf.info[kept - 1].cluster) {
              uint32_t old = buf.info[kept - 1].cluster;
              size_t k;

              for (k = kept; k && buf.info[k - 1].cluster == old; k--) {
                buf.info[k - 1].cluster = cluster;
              }
            }
            continue;
          }
          if (i + 1 < buf.len) {
            buf.info[i + 1].cluster = cluster < buf.info[i + 1].cluster
                ? cluster : buf.info[i + 1].cluster;
            cluster = buf.info[i + 1].cluster;
            (void)cluster;
          }
          continue;
        }
      }
      if (kept != i) {
        buf.info[kept] = buf.info[i];
      }
      kept++;
    }
    buf.len = kept;
  }

  // Default advances.
  for (i = 0; i < buf.len; i++) {
    int32_t advance = 0;

    if (gfnt_face_glyph_advance(face, buf.info[i].glyph, options->variation,
            &advance, NULL) != GFNT_OK) {
      advance = 0;
    }
    // Every field, not only the advance: the array grew with the run and holds
    // whatever the allocator last had in it.
    memset(&buf.pos[i], 0, sizeof buf.pos[i]);
    if (vertical) {
      int32_t origin_x = 0;
      int32_t origin_y = 0;

      if (!gfnt_vertical_advance(face, buf.info[i].glyph, options->variation,
              &buf.pos[i].y_advance)) {
        buf.pos[i].y_advance = 0;
      }
      // The glyph is drawn with its vertical origin where the pen is.
      if (gfnt_vertical_origin(face, buf.info[i].glyph, options->variation,
              &origin_x, &origin_y)) {
        buf.pos[i].x_offset -= origin_x;
        buf.pos[i].y_offset -= origin_y;
      }
    }
    else {
      buf.pos[i].x_advance = advance;
    }
  }
  gfnt_fallback_spaces(face, &buf, options->variation, vertical);

  // A shaper that wants marks taken out of the width before positioning says so.
  if (plan.shaper->zero_width_marks == 1) {
    gfnt_zero_mark_widths(&buf, !plan.tables[1].present && !native_rtl);
  }

  // GPOS.
  if (plan.tables[1].present) {
    gpos.buf = &buf;
    gpos.face = face;
    gpos.variation = options->variation;
    gpos.gdef = &gdef;
    gpos.is_gpos = true;
    gpos.rtl = native_rtl;
    gpos.vertical = vertical;
    result = gfnt_layout_open(face, GFNT_TAG_GPOS, &gpos_table, error);
    if (result != GFNT_OK) {
      goto done;
    }
    gpos.lt = &gpos_table;
    gfnt_gpos_position_start(&buf);
    result = gfnt_shape_apply(&gpos, &plan.tables[1], 1, NULL, error,
        &run.gpos_lookups);
    if (result != GFNT_OK) {
      goto done;
    }
  }

  if (plan.kern_fallback) {
    // The `kern` table's pairs are in visual order: a right-to-left run is turned
    // round for it, and back.
    if (native_rtl) {
      gfnt_lbuf_reverse(&buf);
    }
    result = gfnt_kern_apply(face, &buf, &gdef, plan.kern_mask, error);
    if (native_rtl) {
      gfnt_lbuf_reverse(&buf);
    }
    if (result != GFNT_OK) {
      goto done;
    }
  }

  // A mark takes no room of its own: its advance is folded into where it sits.
  // With a GPOS the font has already said where; without one, the mark is pulled
  // back over the base it follows.
  if (plan.shaper->zero_width_marks == 2) {
    gfnt_zero_mark_widths(&buf, !plan.tables[1].present && !native_rtl);
  }
  for (i = 0; i < buf.len; i++) {
    if ((buf.info[i].flags & GFNT_GF_DEFAULT_IGNORABLE)
        && !(buf.info[i].props & GFNT_PROP_SUBSTITUTED)) {
      buf.pos[i].x_advance = 0;
      buf.pos[i].y_advance = 0;
      buf.pos[i].x_offset = 0;
      buf.pos[i].y_offset = 0;
    }
  }
  if (plan.tables[1].present) {
    gfnt_gpos_position_finish_offsets(&buf, native_rtl, vertical);
  }
  // A font with no `GPOS` does not say where a mark goes, so the shaper does.
  if (!plan.tables[1].present && plan.shaper->fallback_position) {
    gfnt_fallback_mark_position(face, &buf, options->variation, !native_rtl,
        !native_rtl, !native_rtl);
  }
  if (native_rtl) {
    gfnt_lbuf_reverse(&buf);
  }

  run.glyphs = allocator->calloc_fn(allocator->ctx, buf.len ? buf.len : 1,
      sizeof *run.glyphs);
  if (!run.glyphs) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the shaped run");
    goto done;
  }
  for (i = 0; i < buf.len; i++) {
    run.glyphs[i] = (GFNT_ShapedGlyph){buf.info[i].glyph, buf.info[i].cluster,
        buf.pos[i].x_advance, buf.pos[i].y_advance, buf.pos[i].x_offset,
        buf.pos[i].y_offset};
  }
  run.count = buf.len;
  run.script = plan.tables[0].present ? plan.tables[0].script
                                      : plan.tables[1].script;
  run.language = plan.tables[0].present ? plan.tables[0].language
                                        : plan.tables[1].language;
  *out_run = run;
  result = GFNT_OK;

done:
  allocator->free_fn(allocator->ctx, chars);
  gfnt_lbuf_free(&buf);
  gfnt_plan_free(&plan);
  return result;
}

/* --- the dump ------------------------------------------------------------ */

static void gfnt_tag_text(GFNT_Tag tag, char out[5]) {
  size_t i;

  for (i = 0; i < 4; i++) {
    unsigned char ch = (unsigned char)(tag >> (24 - 8 * i));

    out[i] = ch >= 0x20 && ch < 0x7F ? (char)ch : '?';
  }
  out[4] = 0;
}

GFNT_Result gfnt_face_layout_dump(const GFNT_Face * face, GFNT_Tag table,
    FILE * out) {
  GFNT_LayoutTable lt;
  GFNT_Gdef gdef;
  GFNT_LApply c;
  GFNT_Error error;
  GFNT_Result result;
  uint32_t i;
  uint32_t scripts;

  if (!out) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_layout_open(face, table, &lt, &error);
  if (result != GFNT_OK) {
    return result;
  }
  gfnt_gdef_open(face, &gdef);
  memset(&c, 0, sizeof c);
  c.lt = &lt;
  c.gdef = &gdef;
  scripts = lt.script_list ? gfnt_lu16(&c, lt.script_list) : 0;
  fprintf(out, "%s: %u scripts, %u features, %u lookups\n",
      table == GFNT_TAG_GSUB ? "GSUB" : "GPOS", scripts, lt.feature_count,
      lt.lookup_count);
  for (i = 0; i < scripts && !c.fault.bad; i++) {
    size_t record = lt.script_list + 2 + 6 * (size_t)i;
    size_t script = gfnt_l_rel(lt.script_list, gfnt_lu16(&c, record + 4));
    char tag[5];
    uint32_t langs = script ? gfnt_lu16(&c, script + 2) : 0;
    uint32_t k;

    gfnt_tag_text(gfnt_lu32(&c, record), tag);
    fprintf(out, "  script %s: %u language systems%s\n", tag, langs,
        script && gfnt_lu16(&c, script) ? ", with a default" : "");
    for (k = 0; k < langs && !c.fault.bad; k++) {
      char lang[5];

      gfnt_tag_text(gfnt_lu32(&c, script + 4 + 6 * (size_t)k), lang);
      fprintf(out, "    language %s\n", lang);
    }
  }
  for (i = 0; i < lt.feature_count && !c.fault.bad; i++) {
    size_t record = lt.feature_list + 2 + 6 * (size_t)i;
    size_t feature = gfnt_l_rel(lt.feature_list, gfnt_lu16(&c, record + 4));
    char tag[5];
    uint32_t lookups = feature ? gfnt_lu16(&c, feature + 2) : 0;
    uint32_t k;

    gfnt_tag_text(gfnt_lu32(&c, record), tag);
    fprintf(out, "  feature %u %s:", i, tag);
    for (k = 0; k < lookups && !c.fault.bad; k++) {
      fprintf(out, " %u", gfnt_lu16(&c, feature + 4 + 2 * (size_t)k));
    }
    fputc('\n', out);
  }
  for (i = 0; i < lt.lookup_count && !c.fault.bad; i++) {
    size_t offset;
    uint16_t type;
    uint16_t flag;
    uint16_t subtables;

    if (gfnt_l_lookup(&c, i, &offset, &type, &flag, &subtables)) {
      fprintf(out, "  lookup %u: type %u flag 0x%04X, %u subtables\n", i, type,
          flag, subtables);
    }
  }
  if (c.fault.bad) {
    return GFNT_ERR_CORRUPT;
  }
  return GFNT_OK;
}
