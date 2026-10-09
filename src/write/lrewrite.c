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
 * The encoders a rewritten layout table is made of. See lrewrite.h.
 */

#include <stdlib.h>
#include <string.h>
#include "../layout_tables/layout.h"
#include "lrewrite.h"

/* --- the output buffer --------------------------------------------------- */

void gfnt_lout_init(GFNT_LOut * o, const GFNT_Allocator * a) {
  memset(o, 0, sizeof *o);
  o->a = a;
  gfnt_wbuf_init(&o->b, a);
}

void gfnt_lout_free(GFNT_LOut * o) {
  gfnt_wbuf_free(&o->b);
  o->a->free_fn(o->a->ctx, o->kids);
  o->a->free_fn(o->a->ctx, o->table);
  o->a->free_fn(o->a->ctx, o->pend);
  o->kids = NULL;
  o->table = NULL;
  o->pend = NULL;
  o->nkids = o->capkids = o->table_cap = o->npend = o->cappend = 0;
}

bool gfnt_lout_failed(const GFNT_LOut * o) {
  return o->oom || o->b.oom;
}

static uint32_t fnv1a(const uint8_t * p, size_t n) {
  uint32_t h = 2166136261u;

  while (n--) {
    h ^= *p++;
    h *= 16777619u;
  }
  return h;
}

static void table_insert(uint32_t * table, size_t cap, uint32_t hash, uint32_t kid_plus_one) {
  size_t slot = hash & (cap - 1);

  while (table[slot]) {
    slot = (slot + 1) & (cap - 1);
  }
  table[slot] = kid_plus_one;
}

/** Make room in the kid list and the hash table for one more. */
static bool grow_kids(GFNT_LOut * o) {
  if (o->nkids == o->capkids) {
    size_t cap = o->capkids ? o->capkids * 2 : 16;
    GFNT_LKid * grown = o->a->calloc_fn(o->a->ctx, cap, sizeof *grown);

    if (!grown) {
      o->oom = true;
      return false;
    }
    if (o->nkids) {
      memcpy(grown, o->kids, o->nkids * sizeof *grown);
    }
    o->a->free_fn(o->a->ctx, o->kids);
    o->kids = grown;
    o->capkids = cap;
  }
  if ((o->nkids + 1) * 2 > o->table_cap) {
    size_t cap = o->table_cap ? o->table_cap * 2 : 32;
    uint32_t * table = o->a->calloc_fn(o->a->ctx, cap, sizeof *table);
    size_t i;

    if (!table) {
      o->oom = true;
      return false;
    }
    for (i = 0; i < o->nkids; ++i) {
      table_insert(table, cap, o->kids[i].hash, (uint32_t)(i + 1));
    }
    o->a->free_fn(o->a->ctx, o->table);
    o->table = table;
    o->table_cap = cap;
  }
  return true;
}

size_t gfnt_lout_add(GFNT_LOut * o, const uint8_t * data, size_t len) {
  uint32_t h = fnv1a(data, len);
  size_t pos;

  if (gfnt_lout_failed(o)) {
    return 0;
  }
  if (o->table_cap) {
    size_t slot = h & (o->table_cap - 1);

    while (o->table[slot]) {
      const GFNT_LKid * k = &o->kids[o->table[slot] - 1];

      if (k->hash == h && k->len == len && memcmp(o->b.data + k->pos, data, len) == 0) {
        return k->pos;
      }
      slot = (slot + 1) & (o->table_cap - 1);
    }
  }
  if (!grow_kids(o)) {
    return 0;
  }
  pos = o->b.length;
  gfnt_wbuf_bytes(&o->b, data, len);
  o->kids[o->nkids].pos = pos;
  o->kids[o->nkids].len = len;
  o->kids[o->nkids].hash = h;
  ++o->nkids;
  table_insert(o->table, o->table_cap, h, (uint32_t)o->nkids);
  return pos;
}

void gfnt_lout_link16(GFNT_LOut * o, size_t field, size_t origin, size_t pos) {
  if (gfnt_lout_failed(o) || field + 2 > o->b.length) {
    return;
  }
  if (pos < origin || pos - origin > 0xFFFFu) {
    o->overflow = true;
    return;
  }
  gfnt_write_put16(o->b.data + field, (uint32_t)(pos - origin));
}

void gfnt_lout_link32(GFNT_LOut * o, size_t field, size_t origin, size_t pos) {
  if (gfnt_lout_failed(o) || field + 4 > o->b.length) {
    return;
  }
  if (pos < origin || pos - origin > 0xFFFFFFFFu) {
    o->overflow = true;
    return;
  }
  gfnt_write_put32(o->b.data + field, (uint32_t)(pos - origin));
}

void gfnt_lout_child16(GFNT_LOut * o, size_t field, size_t origin, const GFNT_WBuf * child) {
  if (child->oom) {
    o->oom = true;
    return;
  }
  if (child->length == 0) {
    return;
  }
  gfnt_lout_link16(o, field, origin, gfnt_lout_add(o, child->data, child->length));
}

/* --- reading the source -------------------------------------------------- */

uint16_t gfnt_rw_u16(GFNT_RW * rw, size_t offset) {
  return gfnt_lr_u16(rw->r, offset, &rw->bad);
}

uint32_t gfnt_rw_u32(GFNT_RW * rw, size_t offset) {
  return gfnt_lr_u32(rw->r, offset, &rw->bad);
}

bool gfnt_rw_kept(const GFNT_RW * rw, uint32_t old_glyph) {
  return old_glyph < rw->count && rw->shape[old_glyph];
}

bool gfnt_rw_first(const GFNT_RW * rw, uint32_t old_glyph) {
  return gfnt_rw_kept(rw, old_glyph) && old_glyph >= rw->win_lo && old_glyph < rw->win_hi;
}

/* --- coverage ------------------------------------------------------------ */

#define MAX_COVERAGE 65536u

GFNT_CovEnt * gfnt_rw_cov_read(GFNT_RW * rw, size_t coverage, size_t * n) {
  GFNT_CovEnt * out;
  size_t total = 0;
  uint16_t format;
  uint16_t count;
  size_t i;

  *n = 0;
  if (!coverage) {
    return NULL;
  }
  format = gfnt_rw_u16(rw, coverage);
  count = gfnt_rw_u16(rw, coverage + 2);
  if (rw->bad) {
    return NULL;
  }
  if (format == 1) {
    total = count;
  }
  else if (format == 2) {
    for (i = 0; i < count; ++i) {
      uint32_t start = gfnt_rw_u16(rw, coverage + 4 + 6 * i);
      uint32_t end = gfnt_rw_u16(rw, coverage + 6 + 6 * i);

      if (rw->bad) {
        return NULL;
      }
      if (end >= start) {
        total += end - start + 1;
      }
      if (total > MAX_COVERAGE) {
        rw->bad = true;
        return NULL;
      }
    }
  }
  else {
    rw->bad = true;
    return NULL;
  }
  if (total == 0) {
    return NULL;
  }
  out = rw->a->calloc_fn(rw->a->ctx, total, sizeof *out);
  if (!out) {
    rw->oom = true;
    return NULL;
  }
  if (format == 1) {
    for (i = 0; i < count; ++i) {
      out[i].glyph = gfnt_rw_u16(rw, coverage + 4 + 2 * i);
      out[i].index = (uint32_t)i;
    }
    *n = total;
  }
  else {
    size_t k = 0;

    for (i = 0; i < count; ++i) {
      size_t at = coverage + 4 + 6 * i;
      uint32_t start = gfnt_rw_u16(rw, at);
      uint32_t end = gfnt_rw_u16(rw, at + 2);
      uint32_t first = gfnt_rw_u16(rw, at + 4);
      uint32_t g;

      for (g = start; g <= end && k < total; ++g) {
        out[k].glyph = g;
        out[k].index = first + (g - start);
        ++k;
      }
    }
    *n = k;
  }
  if (rw->bad) {
    rw->a->free_fn(rw->a->ctx, out);
    *n = 0;
    return NULL;
  }
  return out;
}

void gfnt_rw_cov_write(GFNT_WBuf * out, const uint32_t * ids, size_t n) {
  size_t ranges = 0;
  size_t i;

  for (i = 0; i < n; ++i) {
    if (i == 0 || ids[i] != ids[i - 1] + 1) {
      ++ranges;
    }
  }
  if (4 + 6 * ranges < 4 + 2 * n) {
    uint32_t cindex = 0;

    gfnt_wbuf_u16(out, 2);
    gfnt_wbuf_u16(out, (uint32_t)ranges);
    for (i = 0; i < n;) {
      size_t j = i;

      while (j + 1 < n && ids[j + 1] == ids[j] + 1) {
        ++j;
      }
      gfnt_wbuf_u16(out, ids[i]);
      gfnt_wbuf_u16(out, ids[j]);
      gfnt_wbuf_u16(out, cindex);
      cindex += (uint32_t)(j - i + 1);
      i = j + 1;
    }
    return;
  }
  gfnt_wbuf_u16(out, 1);
  gfnt_wbuf_u16(out, (uint32_t)n);
  for (i = 0; i < n; ++i) {
    gfnt_wbuf_u16(out, ids[i]);
  }
}

size_t gfnt_rw_cov_filtered(GFNT_RW * rw, size_t coverage, GFNT_WBuf * out) {
  size_t n = 0;
  size_t kept = 0;
  size_t i;
  GFNT_CovEnt * ents = gfnt_rw_cov_read(rw, coverage, &n);
  uint32_t * ids;

  if (!ents) {
    return 0;
  }
  ids = rw->a->calloc_fn(rw->a->ctx, n, sizeof *ids);
  if (!ids) {
    rw->oom = true;
    rw->a->free_fn(rw->a->ctx, ents);
    return 0;
  }
  for (i = 0; i < n; ++i) {
    if (gfnt_rw_kept(rw, ents[i].glyph)) {
      ids[kept++] = rw->new_of_old[ents[i].glyph];
    }
  }
  if (kept) {
    gfnt_rw_cov_write(out, ids, kept);
  }
  rw->a->free_fn(rw->a->ctx, ids);
  rw->a->free_fn(rw->a->ctx, ents);
  return kept;
}

/* --- class definitions --------------------------------------------------- */

void gfnt_rw_classdef(GFNT_RW * rw, size_t source, const uint16_t * map,
    const uint32_t * olds, size_t n, GFNT_WBuf * out) {
  uint32_t * ids;
  uint16_t * cls;
  size_t used = 0;
  size_t ranges = 0;
  size_t span;
  size_t total = olds ? n : rw->new_count;
  size_t i;

  ids = rw->a->calloc_fn(rw->a->ctx, total + 1, sizeof *ids);
  cls = rw->a->calloc_fn(rw->a->ctx, total + 1, sizeof *cls);
  if (!ids || !cls) {
    rw->oom = true;
    rw->a->free_fn(rw->a->ctx, ids);
    rw->a->free_fn(rw->a->ctx, cls);
    return;
  }
  for (i = 0; i < total; ++i) {
    uint32_t old = olds ? olds[i] : rw->old_of_new[i];
    uint32_t value;

    if (!gfnt_rw_kept(rw, old)) {
      continue;
    }
    value = gfnt_lr_class(rw->r, source, old, &rw->bad);
    if (map && value) {
      value = map[value];
    }
    if (value) {
      ids[used] = rw->new_of_old[old];
      cls[used] = (uint16_t)value;
      ++used;
    }
  }
  for (i = 0; i < used; ++i) {
    if (i == 0 || ids[i] != ids[i - 1] + 1 || cls[i] != cls[i - 1]) {
      ++ranges;
    }
  }
  span = used ? ids[used - 1] - ids[0] + 1 : 0;
  if (used == 0 || 4 + 6 * ranges <= 6 + 2 * span) {
    gfnt_wbuf_u16(out, 2);
    gfnt_wbuf_u16(out, (uint32_t)ranges);
    for (i = 0; i < used; ++i) {
      size_t j = i;

      while (j + 1 < used && ids[j + 1] == ids[j] + 1 && cls[j + 1] == cls[i]) {
        ++j;
      }
      gfnt_wbuf_u16(out, ids[i]);
      gfnt_wbuf_u16(out, ids[j]);
      gfnt_wbuf_u16(out, cls[i]);
      i = j;
    }
  }
  else {
    size_t k = 0;
    uint32_t g;

    gfnt_wbuf_u16(out, 1);
    gfnt_wbuf_u16(out, ids[0]);
    gfnt_wbuf_u16(out, (uint32_t)span);
    for (g = ids[0]; g < ids[0] + span; ++g) {
      if (k < used && ids[k] == g) {
        gfnt_wbuf_u16(out, cls[k]);
        ++k;
      }
      else {
        gfnt_wbuf_u16(out, 0);
      }
    }
  }
  rw->a->free_fn(rw->a->ctx, ids);
  rw->a->free_fn(rw->a->ctx, cls);
}

/* --- records, devices, anchors, values ----------------------------------- */

size_t gfnt_rw_records(GFNT_RW * rw, size_t at, uint16_t count, GFNT_WBuf * out) {
  size_t written = 0;
  uint16_t i;

  for (i = 0; i < count && !rw->bad; ++i) {
    uint16_t sequence = gfnt_rw_u16(rw, at + 4 * (size_t)i);
    uint16_t lookup = gfnt_rw_u16(rw, at + 4 * (size_t)i + 2);

    if (rw->bad) {
      break;
    }
    if (lookup < rw->lookup_count && rw->active[lookup]) {
      gfnt_wbuf_u16(out, sequence);
      gfnt_wbuf_u16(out, rw->lookup_map[lookup]);
      ++written;
    }
  }
  return written;
}

static size_t device_size(GFNT_RW * rw, size_t source) {
  uint32_t start = gfnt_rw_u16(rw, source);
  uint32_t end = gfnt_rw_u16(rw, source + 2);
  uint32_t format = gfnt_rw_u16(rw, source + 4);
  uint32_t per;
  uint32_t n;

  if (rw->bad) {
    return 0;
  }
  if (format < 1 || format > 3) {
    return 6;   // A variation index, or a format this reader does not know.
  }
  if (end < start) {
    rw->bad = true;
    return 0;
  }
  per = format == 1 ? 8 : format == 2 ? 4 : 2;
  n = end - start + 1;
  return 6 + 2 * (size_t)((n + per - 1) / per);
}

void gfnt_rw_device(GFNT_RW * rw, size_t source, GFNT_WBuf * out) {
  size_t size = device_size(rw, source);
  size_t i;

  for (i = 0; i < size && !rw->bad; i += 2) {
    gfnt_wbuf_u16(out, gfnt_rw_u16(rw, source + i));
  }
}

void gfnt_rw_anchor(GFNT_RW * rw, size_t source, GFNT_WBuf * out) {
  uint16_t format = gfnt_rw_u16(rw, source);
  size_t start = out->length;

  if (rw->bad) {
    return;
  }
  if (format == 1 || format == 2) {
    size_t words = format == 1 ? 3 : 4;
    size_t i;

    for (i = 0; i < words; ++i) {
      gfnt_wbuf_u16(out, gfnt_rw_u16(rw, source + 2 * i));
    }
  }
  else if (format == 3) {
    uint16_t xdev = gfnt_rw_u16(rw, source + 6);
    uint16_t ydev = gfnt_rw_u16(rw, source + 8);
    size_t xat = 0;

    gfnt_wbuf_u16(out, 3);
    gfnt_wbuf_u16(out, gfnt_rw_u16(rw, source + 2));
    gfnt_wbuf_u16(out, gfnt_rw_u16(rw, source + 4));
    gfnt_wbuf_u16(out, 0);
    gfnt_wbuf_u16(out, 0);
    if (xdev && !rw->bad) {
      xat = out->length - start;
      gfnt_rw_device(rw, source + xdev, out);
      if (out->length - start > 0xFFFFu) {
        rw->overflow = true;
      }
      else if (!out->oom) {
        gfnt_write_put16(out->data + start + 6, (uint32_t)xat);
      }
    }
    if (ydev && !rw->bad) {
      if (ydev == xdev) {
        if (!out->oom) {
          gfnt_write_put16(out->data + start + 8, (uint32_t)xat);
        }
      }
      else {
        size_t yat = out->length - start;

        gfnt_rw_device(rw, source + ydev, out);
        if (out->length - start > 0xFFFFu) {
          rw->overflow = true;
        }
        else if (!out->oom) {
          gfnt_write_put16(out->data + start + 8, (uint32_t)yat);
        }
      }
    }
  }
  else {
    rw->bad = true;
  }
}

size_t gfnt_rw_value_size(uint16_t format) {
  size_t n = 0;
  uint16_t f;

  for (f = format & 0xFFu; f; f &= (uint16_t)(f - 1)) {
    ++n;
  }
  return 2 * n;
}

void gfnt_rw_value(GFNT_RW * rw, GFNT_LOut * o, size_t source, uint16_t format,
    size_t origin) {
  unsigned bit;
  size_t at = source;

  for (bit = 0x01; bit <= 0x80; bit <<= 1) {
    uint16_t v;

    if (!(format & bit)) {
      continue;
    }
    v = gfnt_rw_u16(rw, at);
    at += 2;
    if (bit < 0x10 || v == 0) {
      gfnt_wbuf_u16(&o->b, bit < 0x10 ? v : 0);
      continue;
    }
    if (o->npend == o->cappend) {
      size_t cap = o->cappend ? o->cappend * 2 : 16;
      GFNT_LPending * grown = o->a->calloc_fn(o->a->ctx, cap, sizeof *grown);

      if (!grown) {
        o->oom = true;
        return;
      }
      if (o->npend) {
        memcpy(grown, o->pend, o->npend * sizeof *grown);
      }
      o->a->free_fn(o->a->ctx, o->pend);
      o->pend = grown;
      o->cappend = cap;
    }
    o->pend[o->npend].field = o->b.length;
    o->pend[o->npend].source = origin + v;
    ++o->npend;
    gfnt_wbuf_u16(&o->b, 0);
  }
}

void gfnt_rw_flush_devices(GFNT_RW * rw, GFNT_LOut * o) {
  size_t i;

  for (i = 0; i < o->npend && !rw->bad; ++i) {
    GFNT_WBuf dev;

    gfnt_wbuf_init(&dev, o->a);
    gfnt_rw_device(rw, o->pend[i].source, &dev);
    gfnt_lout_child16(o, o->pend[i].field, 0, &dev);
    gfnt_wbuf_free(&dev);
  }
  o->npend = 0;
}
