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
 * The parts of the layout engine every lookup uses: opening `GSUB`, `GPOS` and
 * `GDEF`, the bounds-checked reads, coverage tables and class definitions.
 */

#include <string.h>
#include "layout.h"
#include "../sfnt/sfnt.h"

GFNT_Result gfnt_layout_open(const GFNT_Face * face, GFNT_Tag tag,
    GFNT_LayoutTable * out, GFNT_Error * error) {
  uint16_t major = 0;
  uint16_t minor = 0;
  uint16_t scripts = 0;
  uint16_t features = 0;
  uint16_t lookups = 0;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out || (tag != GFNT_TAG_GSUB && tag != GFNT_TAG_GPOS)) {
    return GFNT_ERR_INVALID;
  }
  memset(out, 0, sizeof *out);
  if (!gfnt_face_has_table(face, tag)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, 0, GFNT_GLYPH_NONE,
        "the face has no such layout table");
  }
  result = gfnt_face_table_reader(face, tag, &out->table, error);
  if (result != GFNT_OK) {
    return result;
  }
  out->table.error = NULL;
  out->tag = tag;
  if (gfnt_reader_u16_at(&out->table, 0, &major) != GFNT_OK
      || gfnt_reader_u16_at(&out->table, 2, &minor) != GFNT_OK
      || gfnt_reader_u16_at(&out->table, 4, &scripts) != GFNT_OK
      || gfnt_reader_u16_at(&out->table, 6, &features) != GFNT_OK
      || gfnt_reader_u16_at(&out->table, 8, &lookups) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0, GFNT_GLYPH_NONE,
        "a layout table shorter than its own header");
  }
  if (major != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, 0, GFNT_GLYPH_NONE,
        "a layout table version other than 1.x");
  }
  out->script_list = scripts;
  out->feature_list = features;
  out->lookup_list = lookups;
  if (minor >= 1) {
    uint32_t variations = 0;

    if (gfnt_reader_u32_at(&out->table, 10, &variations) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 10, GFNT_GLYPH_NONE,
          "a layout table 1.1 with no room for its FeatureVariations offset");
    }
    out->variations = variations;
  }
  // Both lists are offsets from the table; a list that is not there is a font
  // with no features, which is not corrupt, but an offset past the table is.
  if ((scripts && gfnt_reader_u16_at(&out->table, scripts, &major) != GFNT_OK)
      || (features
          && gfnt_reader_u16_at(&out->table, features, &minor) != GFNT_OK)
      || (lookups
          && gfnt_reader_u16_at(&out->table, lookups, &major) != GFNT_OK)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 4, GFNT_GLYPH_NONE,
        "a layout table's list offset points past the table");
  }
  if (features) {
    (void)gfnt_reader_u16_at(&out->table, features, &minor);
    out->feature_count = minor;
  }
  if (lookups) {
    (void)gfnt_reader_u16_at(&out->table, lookups, &major);
    out->lookup_count = major;
  }
  return GFNT_OK;
}

void gfnt_gdef_open(const GFNT_Face * face, GFNT_Gdef * out) {
  uint16_t minor = 0;
  uint16_t value = 0;
  uint32_t big = 0;
  bool bad = false;

  memset(out, 0, sizeof *out);
  if (!gfnt_face_has_table(face, GFNT_TAG_GDEF)
      || gfnt_face_table_reader(face, GFNT_TAG_GDEF, &out->table, NULL)
          != GFNT_OK) {
    return;
  }
  out->table.error = NULL;
  // A GDEF this engine cannot read is a GDEF that says nothing: the lookups run
  // without glyph classes, which is what HarfBuzz does with a table it rejects.
  if (gfnt_lr_u16(&out->table, 0, &bad) != 1 || bad) {
    memset(out, 0, sizeof *out);
    return;
  }
  minor = gfnt_lr_u16(&out->table, 2, &bad);
  value = gfnt_lr_u16(&out->table, 4, &bad);
  out->glyph_classes = value;
  value = gfnt_lr_u16(&out->table, 10, &bad);
  out->attach_classes = value;
  if (minor >= 2) {
    out->mark_sets = gfnt_lr_u16(&out->table, 12, &bad);
  }
  if (minor >= 3) {
    big = gfnt_lr_u32(&out->table, 14, &bad);
    out->var_store = big;
  }
  out->has_glyph_classes = out->glyph_classes != 0;
  if (bad) {
    memset(out, 0, sizeof *out);
  }
}

uint16_t gfnt_lr_u16(const GFNT_Reader * r, size_t offset, bool * bad) {
  uint16_t value = 0;

  if (gfnt_reader_u16_at(r, offset, &value) != GFNT_OK) {
    *bad = true;
    return 0;
  }
  return value;
}

uint32_t gfnt_lr_u32(const GFNT_Reader * r, size_t offset, bool * bad) {
  uint32_t value = 0;

  if (gfnt_reader_u32_at(r, offset, &value) != GFNT_OK) {
    *bad = true;
    return 0;
  }
  return value;
}

static void gfnt_l_fault(GFNT_LApply * c, size_t offset) {
  if (!c->fault.bad) {
    c->fault.bad = true;
    c->fault.table = c->lt->tag;
    c->fault.offset = offset;
  }
}

uint16_t gfnt_lu16(GFNT_LApply * c, size_t offset) {
  uint16_t value = 0;

  if (gfnt_reader_u16_at(&c->lt->table, offset, &value) != GFNT_OK) {
    gfnt_l_fault(c, offset);
    return 0;
  }
  return value;
}

int16_t gfnt_ls16(GFNT_LApply * c, size_t offset) {
  return (int16_t)gfnt_lu16(c, offset);
}

uint32_t gfnt_lu32(GFNT_LApply * c, size_t offset) {
  uint32_t value = 0;

  if (gfnt_reader_u32_at(&c->lt->table, offset, &value) != GFNT_OK) {
    gfnt_l_fault(c, offset);
    return 0;
  }
  return value;
}

int32_t gfnt_lr_coverage(const GFNT_Reader * r, size_t coverage,
    uint32_t glyph, bool * bad) {
  uint16_t format;
  uint32_t count;
  uint32_t low = 0;
  uint32_t high;

  if (!coverage) {
    return GFNT_LAYOUT_NOT_COVERED;
  }
  format = gfnt_lr_u16(r, coverage, bad);
  count = gfnt_lr_u16(r, coverage + 2, bad);
  if (*bad) {
    return GFNT_LAYOUT_NOT_COVERED;
  }
  high = count;
  if (format == 1) {
    while (low < high) {
      uint32_t mid = low + (high - low) / 2;
      uint32_t value = gfnt_lr_u16(r, coverage + 4 + 2 * (size_t)mid, bad);

      if (*bad) {
        return GFNT_LAYOUT_NOT_COVERED;
      }
      if (glyph < value) {
        high = mid;
      }
      else if (glyph > value) {
        low = mid + 1;
      }
      else {
        return (int32_t)mid;
      }
    }
    return GFNT_LAYOUT_NOT_COVERED;
  }
  if (format == 2) {
    while (low < high) {
      uint32_t mid = low + (high - low) / 2;
      size_t at = coverage + 4 + 6 * (size_t)mid;
      uint32_t start = gfnt_lr_u16(r, at, bad);
      uint32_t end = gfnt_lr_u16(r, at + 2, bad);

      if (*bad) {
        return GFNT_LAYOUT_NOT_COVERED;
      }
      if (glyph < start) {
        high = mid;
      }
      else if (glyph > end) {
        low = mid + 1;
      }
      else {
        return (int32_t)(gfnt_lr_u16(r, at + 4, bad) + (glyph - start));
      }
    }
    return GFNT_LAYOUT_NOT_COVERED;
  }
  *bad = true;
  return GFNT_LAYOUT_NOT_COVERED;
}

uint32_t gfnt_lr_class(const GFNT_Reader * r, size_t classdef, uint32_t glyph,
    bool * bad) {
  uint16_t format;

  if (!classdef) {
    return 0;
  }
  format = gfnt_lr_u16(r, classdef, bad);
  if (*bad) {
    return 0;
  }
  if (format == 1) {
    uint32_t start = gfnt_lr_u16(r, classdef + 2, bad);
    uint32_t count = gfnt_lr_u16(r, classdef + 4, bad);

    if (glyph < start || glyph - start >= count) {
      return 0;
    }
    return gfnt_lr_u16(r, classdef + 6 + 2 * (size_t)(glyph - start), bad);
  }
  if (format == 2) {
    uint32_t count = gfnt_lr_u16(r, classdef + 2, bad);
    uint32_t low = 0;
    uint32_t high = count;

    while (low < high && !*bad) {
      uint32_t mid = low + (high - low) / 2;
      size_t at = classdef + 4 + 6 * (size_t)mid;
      uint32_t start = gfnt_lr_u16(r, at, bad);
      uint32_t end = gfnt_lr_u16(r, at + 2, bad);

      if (glyph < start) {
        high = mid;
      }
      else if (glyph > end) {
        low = mid + 1;
      }
      else {
        return gfnt_lr_u16(r, at + 4, bad);
      }
    }
    return 0;
  }
  *bad = true;
  return 0;
}

int32_t gfnt_l_coverage(GFNT_LApply * c, size_t coverage, uint32_t glyph) {
  bool bad = false;
  int32_t index = gfnt_lr_coverage(&c->lt->table, coverage, glyph, &bad);

  if (bad) {
    gfnt_l_fault(c, coverage);
    return GFNT_LAYOUT_NOT_COVERED;
  }
  return index;
}

uint32_t gfnt_l_class(GFNT_LApply * c, size_t classdef, uint32_t glyph) {
  bool bad = false;
  uint32_t klass = gfnt_lr_class(&c->lt->table, classdef, glyph, &bad);

  if (bad) {
    gfnt_l_fault(c, classdef);
    return 0;
  }
  return klass;
}

bool gfnt_l_mark_set_covers(GFNT_LApply * c, uint32_t set, uint32_t glyph) {
  const GFNT_Gdef * gdef = c->gdef;
  bool bad = false;
  uint32_t count;
  uint32_t offset;
  int32_t index;

  if (!gdef->mark_sets) {
    return false;
  }
  if (gfnt_lr_u16(&gdef->table, gdef->mark_sets, &bad) != 1) {
    return false;
  }
  count = gfnt_lr_u16(&gdef->table, gdef->mark_sets + 2, &bad);
  if (bad || set >= count) {
    return false;
  }
  offset = gfnt_lr_u32(&gdef->table, gdef->mark_sets + 4 + 4 * (size_t)set, &bad);
  if (bad || !offset) {
    return false;
  }
  index = gfnt_lr_coverage(&gdef->table, gdef->mark_sets + offset, glyph, &bad);
  return !bad && index != GFNT_LAYOUT_NOT_COVERED;
}

uint16_t gfnt_gdef_props(GFNT_LApply * c, uint32_t glyph) {
  const GFNT_Gdef * gdef = c->gdef;
  bool bad = false;
  uint32_t klass;

  if (!gdef->glyph_classes) {
    return 0;
  }
  klass = gfnt_lr_class(&gdef->table, gdef->glyph_classes, glyph, &bad);
  if (bad) {
    return 0;
  }
  switch (klass) {
    case 1:
      return GFNT_PROP_BASE;
    case 2:
      return GFNT_PROP_LIGATURE;
    case 3: {
      uint32_t attach = gfnt_lr_class(&gdef->table, gdef->attach_classes, glyph,
          &bad);

      return (uint16_t)(GFNT_PROP_MARK | ((attach & 0xFFu) << 8));
    }
    default:
      return 0;
  }
}

bool gfnt_l_lookup(GFNT_LApply * c, uint32_t index, size_t * out_offset,
    uint16_t * out_type, uint16_t * out_flag, uint16_t * out_count) {
  size_t list = c->lt->lookup_list;
  size_t offset;

  if (!list || index >= c->lt->lookup_count) {
    return false;
  }
  offset = gfnt_lu16(c, list + 2 + 2 * (size_t)index);
  if (!offset) {
    return false;
  }
  offset += list;
  *out_offset = offset;
  *out_type = gfnt_lu16(c, offset);
  *out_flag = gfnt_lu16(c, offset + 2);
  *out_count = gfnt_lu16(c, offset + 4);
  return !c->fault.bad;
}
