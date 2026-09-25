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
 * Builds sfnt bytes for the tests: an offset table, a directory with correct
 * checksums, and whatever tables a test hands it.
 *
 * **Why this exists, given documentation/design.md section 14.5.** That
 * section says unit fixtures are built by `tools/fixtures/make_fixtures.py`
 * with fontTools, in the pinned container, and committed under
 * `tests/data/fonts/`; it also says hand-built byte arrays remain for the
 * refusal arms that fontTools will not emit - a directory entry past the blob,
 * a length that ends mid-entry, a checksum that does not match. Almost every
 * test in the container and table suites is one of those arms, and writing
 * each as a literal array would make them unreadable and their offsets
 * uncheckable.
 *
 * So this is the organised form of that hand-built array: it puts the bytes
 * where the specification says, and a test then breaks exactly one of them.
 * It is deliberately **not** a substitute for the fontTools fixtures or for
 * the real-font differentials - a corpus this library generates from its own
 * understanding of the format cannot find a place where that understanding is
 * wrong, which is the whole reason section 14.5 puts the conformance corpus
 * outside the repository and behind an oracle.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GFNT_TESTS_SFNT_BUILDER_H
#define GHOTI_IO_GFNT_TESTS_SFNT_BUILDER_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/face.h>

namespace gfnttest {

/** Bytes in an sfnt offset table. */
inline constexpr size_t kOffsetTableBytes = 12;
/** Bytes in one table directory entry. */
inline constexpr size_t kDirectoryEntryBytes = 16;
/** Bytes in a TTC header before its offset array. */
inline constexpr size_t kTtcHeaderBytes = 12;
/** Offset of `checkSumAdjustment` within `head`. */
inline constexpr size_t kHeadAdjustmentOffset = 8;

/** One table to place in a font. */
struct Table {
  GFNT_Tag tag;
  std::vector<uint8_t> data;
};

inline void put_u8(std::vector<uint8_t> & out, uint8_t value) {
  out.push_back(value);
}

inline void put_u16(std::vector<uint8_t> & out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

inline void put_s16(std::vector<uint8_t> & out, int16_t value) {
  put_u16(out, static_cast<uint16_t>(value));
}

inline void put_u32(std::vector<uint8_t> & out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
  }
}

inline void put_u64(std::vector<uint8_t> & out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
  }
}

/** Overwrite a big-endian uint16 already in a buffer. */
inline void patch_u16(std::vector<uint8_t> & out, size_t at, uint16_t value) {
  out[at] = static_cast<uint8_t>(value >> 8);
  out[at + 1] = static_cast<uint8_t>(value & 0xFF);
}

/** Overwrite a big-endian uint32 already in a buffer. */
inline void patch_u32(std::vector<uint8_t> & out, size_t at, uint32_t value) {
  for (int i = 0; i < 4; i++) {
    out[at + i] = static_cast<uint8_t>((value >> (24 - 8 * i)) & 0xFF);
  }
}

/**
 * The checksum of a table, as the OpenType specification computes one: the
 * sum of its big-endian uint32s, padded with zeros to a multiple of four.
 * `head` is checksummed with `checkSumAdjustment` zeroed, because its own
 * value would otherwise depend on itself.
 */
inline uint32_t table_checksum(const Table & table) {
  uint32_t sum = 0;
  size_t words = (table.data.size() + 3) / 4;

  for (size_t i = 0; i < words; i++) {
    uint32_t word = 0;
    for (size_t byte = 0; byte < 4; byte++) {
      size_t offset = i * 4 + byte;
      uint8_t value = 0;
      if (offset < table.data.size()) {
        value = table.data[offset];
        if (table.tag == GFNT_TAG('h', 'e', 'a', 'd')
            && offset >= kHeadAdjustmentOffset
            && offset < kHeadAdjustmentOffset + 4) {
          value = 0;
        }
      }
      word = (word << 8) | value;
    }
    sum += word;
  }
  return sum;
}

/**
 * Where the directory entry for the nth table sits, for a test that wants to
 * break one field of one entry.
 */
inline size_t entry_offset(size_t font_start, size_t index) {
  return font_start + kOffsetTableBytes + index * kDirectoryEntryBytes;
}

/**
 * Lay out one font's directory and tables into @p out, at its current end.
 *
 * Tables are written in the order given, each aligned to four bytes, which is
 * what the specification requires of a writer. The directory is written in the
 * same order: a test that wants an unsorted directory passes unsorted tables,
 * because fonts in the wild have them and this library reads them.
 *
 * @return the offset at which the font's offset table begins.
 */
inline size_t append_font(std::vector<uint8_t> & out, GFNT_Tag flavour,
    const std::vector<Table> & tables) {
  size_t start = out.size();
  size_t count = tables.size();

  put_u32(out, flavour);
  put_u16(out, static_cast<uint16_t>(count));
  // searchRange, entrySelector, rangeShift: derived values this library never
  // reads, written here as the specification defines them so that a font built
  // by these tests is one fontTools would also accept.
  uint16_t search_range = 0;
  uint16_t entry_selector = 0;
  while ((1u << (entry_selector + 1)) <= count) {
    entry_selector++;
  }
  search_range = static_cast<uint16_t>(16u << entry_selector);
  put_u16(out, search_range);
  put_u16(out, entry_selector);
  put_u16(out, static_cast<uint16_t>(count * 16 - search_range));

  size_t directory = out.size();
  out.resize(directory + count * kDirectoryEntryBytes, 0);

  for (size_t i = 0; i < count; i++) {
    while (out.size() % 4 != 0) {
      out.push_back(0);
    }
    size_t offset = out.size();
    out.insert(out.end(), tables[i].data.begin(), tables[i].data.end());

    size_t entry = directory + i * kDirectoryEntryBytes;
    patch_u32(out, entry, tables[i].tag);
    patch_u32(out, entry + 4, table_checksum(tables[i]));
    patch_u32(out, entry + 8, static_cast<uint32_t>(offset));
    patch_u32(out, entry + 12,
        static_cast<uint32_t>(tables[i].data.size()));
  }
  return start;
}

/** A whole single-face font. */
inline std::vector<uint8_t> build_sfnt(GFNT_Tag flavour,
    const std::vector<Table> & tables) {
  std::vector<uint8_t> out;
  append_font(out, flavour, tables);
  return out;
}

/**
 * A `ttcf` collection whose faces each carry their own tables.
 *
 * Real collections share tables between faces, and this library must read
 * those; a test that wants sharing builds it by hand, because sharing is a
 * property of the offsets rather than of the container.
 */
inline std::vector<uint8_t> build_collection(
    const std::vector<std::vector<Table>> & fonts, uint16_t major = 1,
    GFNT_Tag flavour = GFNT_FLAVOUR_TRUETYPE) {
  std::vector<uint8_t> out;

  put_u32(out, GFNT_FLAVOUR_COLLECTION);
  put_u16(out, major);
  put_u16(out, 0);
  put_u32(out, static_cast<uint32_t>(fonts.size()));
  size_t offsets = out.size();
  out.resize(offsets + fonts.size() * 4, 0);
  if (major == 2) {
    // Version 2.0's DSIG fields, which this library skips.
    put_u32(out, 0);
    put_u32(out, 0);
    put_u32(out, 0);
  }

  for (size_t i = 0; i < fonts.size(); i++) {
    while (out.size() % 4 != 0) {
      out.push_back(0);
    }
    size_t start = append_font(out, flavour, fonts[i]);
    patch_u32(out, offsets + i * 4, static_cast<uint32_t>(start));
  }
  return out;
}

/**
 * The tables the metric suites need, each built to its specification's layout
 * so that a test can then break exactly one field.
 */

/** A `head` table: 54 bytes, with the fields a test varies as parameters. */
inline std::vector<uint8_t> build_head(uint16_t units_per_em = 1000,
    int16_t index_to_loc_format = 0, uint32_t magic = 0x5F0F3CF5u) {
  std::vector<uint8_t> head;
  put_u16(head, 1);                    // majorVersion
  put_u16(head, 0);                    // minorVersion
  put_u32(head, 0x00015000);           // fontRevision, 1.328125
  put_u32(head, 0xAABBCCDD);           // checkSumAdjustment
  put_u32(head, magic);                // magicNumber
  put_u16(head, 0x000B);               // flags
  put_u16(head, units_per_em);
  put_u64(head, 0xFFFFFFFFFFFFFFFFull); // created: -1 second
  put_u64(head, 3000000000ull);        // modified
  put_s16(head, -100);                 // xMin
  put_s16(head, -250);                 // yMin
  put_s16(head, 1200);                 // xMax
  put_s16(head, 900);                  // yMax
  put_u16(head, 0x0002);               // macStyle: italic
  put_u16(head, 8);                    // lowestRecPPEM
  put_s16(head, 2);                    // fontDirectionHint
  put_s16(head, index_to_loc_format);
  put_s16(head, 0);                    // glyphDataFormat
  return head;
}

/** A `maxp` table. Version 0.5 is six bytes; 1.0 pads to its 32. */
inline std::vector<uint8_t> build_maxp(uint16_t num_glyphs,
    uint32_t version = 0x00010000) {
  std::vector<uint8_t> maxp;
  put_u32(maxp, version);
  put_u16(maxp, num_glyphs);
  if (version != 0x00005000) {
    maxp.resize(32, 0);
  }
  return maxp;
}

/** An `hhea` table: 36 bytes. */
inline std::vector<uint8_t> build_hhea(int16_t ascender, int16_t descender,
    int16_t line_gap, uint16_t number_of_h_metrics,
    int16_t metric_data_format = 0) {
  std::vector<uint8_t> hhea;
  put_u16(hhea, 1);                 // majorVersion
  put_u16(hhea, 0);                 // minorVersion
  put_s16(hhea, ascender);
  put_s16(hhea, descender);
  put_s16(hhea, line_gap);
  put_u16(hhea, 1500);              // advanceWidthMax
  put_s16(hhea, -30);               // minLeftSideBearing
  put_s16(hhea, -40);               // minRightSideBearing
  put_s16(hhea, 1250);              // xMaxExtent
  put_s16(hhea, 1);                 // caretSlopeRise
  put_s16(hhea, 0);                 // caretSlopeRun
  put_s16(hhea, 0);                 // caretOffset
  for (int i = 0; i < 4; i++) {
    put_s16(hhea, 0);               // reserved
  }
  put_s16(hhea, metric_data_format);
  put_u16(hhea, number_of_h_metrics);
  return hhea;
}

/**
 * An `hmtx` table: @p metrics long entries of (advance, lsb), then one short
 * entry per trailing glyph.
 */
inline std::vector<uint8_t> build_hmtx(
    const std::vector<std::pair<uint16_t, int16_t>> & metrics,
    const std::vector<int16_t> & trailing_bearings = {}) {
  std::vector<uint8_t> hmtx;
  for (const auto & entry : metrics) {
    put_u16(hmtx, entry.first);
    put_s16(hmtx, entry.second);
  }
  for (int16_t bearing : trailing_bearings) {
    put_s16(hmtx, bearing);
  }
  return hmtx;
}

/** The fields of `OS/2` the tests vary. */
struct Os2Spec {
  uint16_t version = 4;
  uint16_t fs_type = 0;
  uint16_t fs_selection = 0;
  uint16_t weight_class = 400;
  int16_t typo_ascender = 800;
  int16_t typo_descender = -200;
  int16_t typo_line_gap = 100;
  uint16_t win_ascent = 900;
  uint16_t win_descent = 250;
  int16_t x_height = 500;
  int16_t cap_height = 700;
};

/** An `OS/2` table of the spec's version, exactly as long as that version. */
inline std::vector<uint8_t> build_os2(const Os2Spec & spec = Os2Spec{}) {
  std::vector<uint8_t> os2;
  put_u16(os2, spec.version);
  put_s16(os2, 600);                 // xAvgCharWidth
  put_u16(os2, spec.weight_class);
  put_u16(os2, 5);                   // usWidthClass
  put_u16(os2, spec.fs_type);
  put_s16(os2, 650);                 // ySubscriptXSize
  put_s16(os2, 600);                 // ySubscriptYSize
  put_s16(os2, 0);                   // ySubscriptXOffset
  put_s16(os2, 75);                  // ySubscriptYOffset
  put_s16(os2, 650);                 // ySuperscriptXSize
  put_s16(os2, 600);                 // ySuperscriptYSize
  put_s16(os2, 0);                   // ySuperscriptXOffset
  put_s16(os2, 350);                 // ySuperscriptYOffset
  put_s16(os2, 50);                  // yStrikeoutSize
  put_s16(os2, 250);                 // yStrikeoutPosition
  put_s16(os2, 0x0801);              // sFamilyClass
  for (int i = 0; i < 10; i++) {
    put_u8(os2, static_cast<uint8_t>(i + 1)); // panose
  }
  for (int i = 0; i < 4; i++) {
    put_u32(os2, 0x10000000u * (i + 1)); // ulUnicodeRange1..4
  }
  for (char c : std::string("GHTI")) {
    put_u8(os2, static_cast<uint8_t>(c));
  }
  put_u16(os2, spec.fs_selection);
  put_u16(os2, 0x0020);              // usFirstCharIndex
  put_u16(os2, 0xFFFD);              // usLastCharIndex
  put_s16(os2, spec.typo_ascender);
  put_s16(os2, spec.typo_descender);
  put_s16(os2, spec.typo_line_gap);
  put_u16(os2, spec.win_ascent);
  put_u16(os2, spec.win_descent);
  if (spec.version >= 1) {
    put_u32(os2, 0x0000001Fu);       // ulCodePageRange1
    put_u32(os2, 0u);                // ulCodePageRange2
  }
  if (spec.version >= 2) {
    put_s16(os2, spec.x_height);
    put_s16(os2, spec.cap_height);
    put_u16(os2, 0);                 // usDefaultChar
    put_u16(os2, 0x0020);            // usBreakChar
    put_u16(os2, 3);                 // usMaxContext
  }
  if (spec.version >= 5) {
    put_u16(os2, 80);                // usLowerOpticalPointSize
    put_u16(os2, 240);               // usUpperOpticalPointSize
  }
  return os2;
}

/** A `post` table: the 32-byte header, plus whatever a version adds. */
inline std::vector<uint8_t> build_post(uint32_t version = 0x00030000) {
  std::vector<uint8_t> post;
  put_u32(post, version);
  put_u32(post, 0xFFF40000);         // italicAngle: -12.0
  put_s16(post, -75);                // underlinePosition
  put_s16(post, 50);                 // underlineThickness
  put_u32(post, 1);                  // isFixedPitch
  put_u32(post, 0);                  // minMemType42
  put_u32(post, 0);                  // maxMemType42
  put_u32(post, 0);                  // minMemType1
  put_u32(post, 0);                  // maxMemType1
  if (version == 0x00020000) {
    // numberOfGlyphs and one index, which this library does not read yet.
    put_u16(post, 1);
    put_u16(post, 0);
  }
  return post;
}

/**
 * `cmap` subtables. Format 4's idRangeOffset is computed by the builder rather
 * than by the test, because a test that computed it would be checking the
 * parser against the same arithmetic it is meant to be checking (M10).
 */

/** One format 4 segment: a delta run, or an explicit glyph array. */
struct Segment4 {
  uint16_t start;
  uint16_t end;
  int16_t delta = 0;                  ///< Used when `glyphs` is empty.
  std::vector<uint16_t> glyphs = {};  ///< Used when it is not: the array path.
};

/** A `cmap` format 4 subtable over the given segments, in order. */
inline std::vector<uint8_t> build_cmap_format4(
    const std::vector<Segment4> & segments) {
  size_t count = segments.size();
  std::vector<uint8_t> out;

  size_t glyph_bytes = 0;
  for (const auto & segment : segments) {
    glyph_bytes += segment.glyphs.size() * 2;
  }
  size_t length = 14 + count * 8 + 2 + glyph_bytes;

  put_u16(out, 4);
  put_u16(out, static_cast<uint16_t>(length));
  put_u16(out, 0);                                   // language
  put_u16(out, static_cast<uint16_t>(count * 2));    // segCountX2
  uint16_t entry_selector = 0;
  while ((1u << (entry_selector + 1)) <= count) {
    entry_selector++;
  }
  put_u16(out, static_cast<uint16_t>(2u << entry_selector)); // searchRange
  put_u16(out, entry_selector);
  put_u16(out, static_cast<uint16_t>(count * 2 - (2u << entry_selector)));

  for (const auto & segment : segments) {
    put_u16(out, segment.end);
  }
  put_u16(out, 0);                                   // reservedPad
  for (const auto & segment : segments) {
    put_u16(out, segment.start);
  }
  for (const auto & segment : segments) {
    put_s16(out, segment.delta);
  }

  size_t id_range_offsets = out.size();
  out.resize(id_range_offsets + count * 2, 0);
  size_t glyph_at = out.size();
  for (size_t i = 0; i < count; i++) {
    if (segments[i].glyphs.empty()) {
      continue;
    }
    // The offset the format defines: from this idRangeOffset entry's own
    // address to the segment's first glyph.
    patch_u16(out, id_range_offsets + i * 2,
        static_cast<uint16_t>(glyph_at - (id_range_offsets + i * 2)));
    for (uint16_t glyph : segments[i].glyphs) {
      put_u16(out, glyph);
    }
    glyph_at = out.size();
  }
  return out;
}

/** Where a format 4 subtable's idRangeOffset array starts. */
inline size_t format4_id_range_offsets(size_t segment_count) {
  return 14 + segment_count * 2 + 2 + segment_count * 2 + segment_count * 2;
}

/** One format 12 group. */
struct Group12 {
  uint32_t start;
  uint32_t end;
  uint32_t start_glyph;
};

/** A `cmap` format 12 subtable over the given groups, in order. */
inline std::vector<uint8_t> build_cmap_format12(
    const std::vector<Group12> & groups) {
  std::vector<uint8_t> out;
  put_u16(out, 12);
  put_u16(out, 0);                                       // reserved
  put_u32(out, static_cast<uint32_t>(16 + groups.size() * 12)); // length
  put_u32(out, 0);                                       // language
  put_u32(out, static_cast<uint32_t>(groups.size()));
  for (const auto & group : groups) {
    put_u32(out, group.start);
    put_u32(out, group.end);
    put_u32(out, group.start_glyph);
  }
  return out;
}

/** A `cmap` format 0 subtable: 256 single-byte mappings. */
inline std::vector<uint8_t> build_cmap_format0(
    const std::vector<std::pair<uint8_t, uint8_t>> & mappings) {
  std::vector<uint8_t> out;
  put_u16(out, 0);
  put_u16(out, 262);  // length
  put_u16(out, 0);    // language
  out.resize(6 + 256, 0);
  for (const auto & mapping : mappings) {
    out[6 + mapping.first] = mapping.second;
  }
  return out;
}

/** A `cmap` format 6 subtable: a trimmed array. */
inline std::vector<uint8_t> build_cmap_format6(uint16_t first,
    const std::vector<uint16_t> & glyphs) {
  std::vector<uint8_t> out;
  put_u16(out, 6);
  put_u16(out, static_cast<uint16_t>(10 + glyphs.size() * 2));
  put_u16(out, 0);    // language
  put_u16(out, first);
  put_u16(out, static_cast<uint16_t>(glyphs.size()));
  for (uint16_t glyph : glyphs) {
    put_u16(out, glyph);
  }
  return out;
}

/** One encoding record and the subtable it points at. */
struct CmapRecord {
  uint16_t platform_id;
  uint16_t encoding_id;
  std::vector<uint8_t> subtable;
};

/** A whole `cmap` table: the record list, then the subtables. */
inline std::vector<uint8_t> build_cmap(const std::vector<CmapRecord> & records) {
  std::vector<uint8_t> out;
  put_u16(out, 0);                                        // version
  put_u16(out, static_cast<uint16_t>(records.size()));
  size_t offsets = out.size();
  out.resize(offsets + records.size() * 8, 0);

  for (size_t i = 0; i < records.size(); i++) {
    size_t at = offsets + i * 8;
    patch_u16(out, at, records[i].platform_id);
    patch_u16(out, at + 2, records[i].encoding_id);
    patch_u32(out, at + 4, static_cast<uint32_t>(out.size()));
    out.insert(out.end(), records[i].subtable.begin(),
        records[i].subtable.end());
  }
  return out;
}

/** Where the nth encoding record sits within a `cmap` table. */
inline size_t cmap_record_offset(size_t index) {
  return 4 + index * 8;
}

} // namespace gfnttest

#endif // GHOTI_IO_GFNT_TESTS_SFNT_BUILDER_H
