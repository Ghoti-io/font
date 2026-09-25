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

} // namespace gfnttest

#endif // GHOTI_IO_GFNT_TESTS_SFNT_BUILDER_H
