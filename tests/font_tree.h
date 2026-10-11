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
 * A temporary directory of small faces, scanned into a set, for the matching and fallback
 * tests: each face is written from a few fields (family, label, weight, width, slant, axes and
 * the characters its `cmap` maps), so a test says what a face is and nothing else.
 */

#ifndef GHOTI_IO_GFNT_TESTS_FONT_TREE_H
#define GHOTI_IO_GFNT_TESTS_FONT_TREE_H

#include "sfnt_builder.h"

#include <cstdio>
#include <string>
#include <vector>

#include <unistd.h>

#include <ghoti.io/cutil/dir.h>
#include <ghoti.io/cutil/file.h>
#include <ghoti.io/font/discover.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/match.h>

#include <gtest/gtest.h>

namespace {

using namespace gfnttest;

constexpr GFNT_F16Dot16 kOne = 65536;

constexpr GFNT_F16Dot16 fx(double v) { return static_cast<GFNT_F16Dot16>(v * 65536.0); }

struct Axis {
  const char * tag;
  double min, def, max;
};

/** One face to write: the fields matching reads, and a label to name it by. */
struct Spec {
  std::string family;
  std::string label;                   ///< Written as the style; what the tests compare.
  std::string legacy;                  ///< Name ID 1 when it differs from the family.
  std::string full;                    ///< Name ID 4 when not empty.
  std::string postscript;              ///< Name ID 6 when not empty.
  uint16_t weight = 400;
  uint16_t width = 5;
  bool italic = false;
  bool oblique = false;                ///< Sets the fsSelection oblique bit.
  double lean = 0;                     ///< post.italicAngle, degrees (negative leans right).
  /** The characters the `cmap` maps, as inclusive ranges; each maps to glyph 1, 2, ... in turn. */
  std::vector<std::pair<uint32_t, uint32_t>> maps = {{0x41, 0x5A}};
  /** The `OS/2` Unicode range words; the default sets none, which claims everything. */
  std::vector<int> claim_bits;         ///< Bits to set in ulUnicodeRange when not empty.
  std::vector<Axis> axes;
};

/** UTF-16BE for a UTF-8 string (two-byte sequences are enough for these names). */
std::vector<uint8_t> utf16(const std::string & text) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < text.size();) {
    uint32_t cp = static_cast<uint8_t>(text[i]);
    size_t len = 1;
    if (cp >= 0xE0) {
      cp = ((cp & 0x0F) << 12) | ((static_cast<uint8_t>(text[i + 1]) & 0x3F) << 6)
          | (static_cast<uint8_t>(text[i + 2]) & 0x3F);
      len = 3;
    } else if (cp >= 0xC0) {
      cp = ((cp & 0x1F) << 6) | (static_cast<uint8_t>(text[i + 1]) & 0x3F);
      len = 2;
    }
    put_u16(out, static_cast<uint16_t>(cp));
    i += len;
  }
  return out;
}

std::vector<uint8_t> fvar_of(const std::vector<Axis> & axes) {
  std::vector<uint8_t> t;
  put_u16(t, 1);
  put_u16(t, 0);
  put_u16(t, 16);
  put_u16(t, 2);
  put_u16(t, static_cast<uint16_t>(axes.size()));
  put_u16(t, 20);
  put_u16(t, 0);
  put_u16(t, static_cast<uint16_t>(4 + 4 * axes.size()));   // instanceSize, though there are none
  for (const Axis & a : axes) {
    put_u32(t, GFNT_TAG(a.tag[0], a.tag[1], a.tag[2], a.tag[3]));
    put_u32(t, static_cast<uint32_t>(fx(a.min)));
    put_u32(t, static_cast<uint32_t>(fx(a.def)));
    put_u32(t, static_cast<uint32_t>(fx(a.max)));
    put_u16(t, 0);
    put_u16(t, 256);
  }
  return t;
}

std::vector<uint8_t> face_bytes(const Spec & s) {
  std::vector<NameRecord> names = {
      {3, 1, 0x409, 16, utf16(s.family)}, {3, 1, 0x409, 17, utf16(s.label)},
      {3, 1, 0x409, 2, utf16(s.label)}};
  names.push_back({3, 1, 0x409, 1, utf16(s.legacy.empty() ? s.family : s.legacy)});
  if (!s.full.empty()) {
    names.push_back({3, 1, 0x409, 4, utf16(s.full)});
  }
  if (!s.postscript.empty()) {
    names.push_back({3, 1, 0x409, 6, utf16(s.postscript)});
  }
  Os2Spec os2;
  os2.weight_class = s.weight;
  os2.width_class = s.width;
  os2.fs_selection = static_cast<uint16_t>((s.italic ? GFNT_OS2_FS_ITALIC : 0)
      | (s.oblique ? GFNT_OS2_FS_OBLIQUE : 0));
  for (int i = 0; i < 4; ++i) {
    os2.unicode_range[i] = 0;
  }
  for (int bit : s.claim_bits) {
    os2.unicode_range[bit / 32] |= 1u << (bit % 32);
  }
  const uint32_t angle = static_cast<uint32_t>(fx(s.lean));
  // Format 4 maps the Basic Multilingual Plane; a face that maps beyond it takes format 12 too.
  std::vector<Segment4> segments;
  std::vector<Group12> groups;
  bool wide = false;
  uint32_t glyph = 1;
  for (const auto & range : s.maps) {
    if (range.second > 0xFFFF) {
      wide = true;
    } else {
      segments.push_back({static_cast<uint16_t>(range.first), static_cast<uint16_t>(range.second),
          static_cast<int16_t>(1 - static_cast<int>(range.first))});
    }
    groups.push_back({range.first, range.second, glyph});
    glyph += range.second - range.first + 1;
  }
  std::vector<CmapRecord> records = {{3, 1, build_cmap_format4(segments)}};
  if (wide) {
    records.push_back({3, 10, build_cmap_format12(groups)});
  }
  std::vector<Table> tables = {
      {GFNT_TAG('O', 'S', '/', '2'), build_os2(os2)},
      {GFNT_TAG('c', 'm', 'a', 'p'), build_cmap(records)},
      {GFNT_TAG('h', 'e', 'a', 'd'), build_head()},
      {GFNT_TAG('h', 'h', 'e', 'a'), build_hhea(800, -200, 0, 1)},
      {GFNT_TAG('h', 'm', 't', 'x'), build_hmtx({{500, 0}})},
      {GFNT_TAG('m', 'a', 'x', 'p'), build_maxp(1)},
      {GFNT_TAG('n', 'a', 'm', 'e'), build_name(names)},
      {GFNT_TAG('p', 'o', 's', 't'), build_post(0x00030000, angle)}};
  if (!s.axes.empty()) {
    tables.push_back({GFNT_TAG('f', 'v', 'a', 'r'), fvar_of(s.axes)});
  }
  return build_sfnt(0x00010000, tables);
}

/** A temporary directory of faces, scanned into a set. */
class Fonts {
public:
  Fonts() {
    char * made = nullptr;
    if (gcu_dir_temp_create(nullptr, "gfnt-match", nullptr, &made) == GCU_FILE_OK) {
      dir_ = made;
      gcu_dir_free_path(nullptr, made);
    }
  }
  ~Fonts() {
    gfnt_fontset_free(set_);
    for (const std::string & f : files_) {
      unlink(f.c_str());
    }
    if (!dir_.empty()) {
      rmdir(dir_.c_str());
    }
  }
  Fonts(const Fonts &) = delete;
  Fonts & operator=(const Fonts &) = delete;

  void add(const Spec & s) {
    const std::string path = dir_ + "/face" + std::to_string(files_.size()) + ".ttf";
    const std::vector<uint8_t> bytes = face_bytes(s);
    FILE * f = fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    files_.push_back(path);
    dirty_ = true;
  }

  /** Delete a face's file (after a scan has listed it). */
  void remove_file(size_t i) { unlink(files_[i].c_str()); }   // the set still lists it: no rescan

  /** The scanned set: scanned once, and again only if a face was added or removed since. */
  /** Write a file of any kind (a bitmap font, say) into the directory. */
  void add_file(const std::string & name, const std::string & bytes) {
    const std::string path = dir_ + "/" + name;
    FILE * f = fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    files_.push_back(path);
    dirty_ = true;
  }

  const GFNT_FontSet * scan(const std::vector<GFNT_FontAlias> & aliases = {}) {
    if (set_ && !dirty_ && aliases.empty()) {
      return set_;
    }
    dirty_ = false;
    gfnt_fontset_free(set_);
    set_ = nullptr;
    const char * dirs[1] = {dir_.c_str()};
    EXPECT_EQ(gfnt_fontset_scan(dirs, 1, aliases.empty() ? nullptr : aliases.data(),
                  aliases.size(), nullptr, &set_, nullptr), GFNT_OK);
    return set_;
  }

  /** The label of the face the query chose, or "" for none. */
  std::string pick(const GFNT_FontQuery & q, GFNT_FontMatch * match = nullptr) {
    GFNT_FontMatch m{};
    bool found = false;
    EXPECT_EQ(gfnt_fontset_match(set_, &q, nullptr, &m, &found, nullptr), GFNT_OK);
    if (match) {
      *match = m;
    }
    return found ? gfnt_fontset_at(set_, m.index)->style : "";
  }

private:
  std::string dir_;
  std::vector<std::string> files_;
  GFNT_FontSet * set_ = nullptr;
  bool dirty_ = false;
};

GFNT_FontQuery query(const char * family, GFNT_F16Dot16 weight = 0, GFNT_F16Dot16 width = 0,
    GFNT_Slant slant = GFNT_SLANT_ROMAN) {
  static const char * families[1];
  families[0] = family;
  GFNT_FontQuery q;
  gfnt_fontquery_init(&q);
  q.families = families;
  q.family_count = 1;
  q.weight = weight;
  q.width = width;
  q.slant = slant;
  return q;
}

}  // namespace


#endif // GHOTI_IO_GFNT_TESTS_FONT_TREE_H
