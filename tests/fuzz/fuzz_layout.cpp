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
 * Fuzz harness for paragraph layout: text, a writing mode, an orientation, an alignment, a
 * justification mode, a direction and a room, laid out and then asked every question the layout
 * answers (carets, hit tests, selection rectangles, content widths).
 *
 * The input is two option bytes, a byte for the room, and the text. The text is, by default,
 * one alphabet character per byte (Latin, Hebrew, Arabic, ideographs, kana, CJK punctuation,
 * brackets, dashes, digits, spaces, newlines, a soft hyphen and a combining mark), so every byte
 * reaches a rule; with the top bit of the first option byte set it is raw UTF-8, which reaches
 * the ill-formed cases. The face is built here: every alphabet character has a glyph, 500 wide
 * and 700 down, with vertical metrics.
 *
 * Properties, beyond that nothing crashes or reads outside its arrays: **the lines tile the
 * text** (each starts where the last ended, and the first starts at 0), **every glyph's cluster
 * lies inside its line**, **the geometry is finite and inside the box** (a line's box lies inside
 * the layout's across the lines), **a caret and a hit test agree** (the offset a
 * hit test at a caret returns has a caret at the same place), **a range's rectangles lie in the
 * box across the lines**, and **laying out twice gives one answer**.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/layout.h>

#include "../sfnt_builder.h"

namespace {

#define check(ok) do { if (!(ok)) { fprintf(stderr, "fuzz_layout: check failed at line %d\n", __LINE__); abort(); } } while (0)

const uint32_t kAlphabet[] = {
    'A', 'B', 'C', 'a', 'b', 'c', '0', '1', ' ', ' ', '\n', '-', '(', ')', 0xAD, 0x301,
    0x5D0, 0x5D1, 0x627, 0x628, 0x62C, 0x4E00, 0x672C, 0x8A9E, 0x3042, 0x30FC, 0x3001, 0x3002,
    0x3008, 0x300C, 0x300D, 0x2014, 0x2026, 0xFF01, 0xFF08, 0xFF21, 0xD55C, 0x261D, 0x200D,
    0xFE0F, 0x3000, '\t'};
constexpr size_t kAlphabetSize = sizeof kAlphabet / sizeof *kAlphabet;

void utf8(uint32_t cp, std::string * out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  }
  else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  else {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// The letters of tests/data/fonts/arabic-join.ttf, a tatweel, a mark, a space and a ZWNJ.
const uint32_t kArabic[] = {
    0x628, 0x62A, 0x62D, 0x633, 0x635, 0x639, 0x643, 0x644, 0x645, 0x646, 0x647, 0x64A, 0x627,
    0x62F, 0x631, 0x648, 0x629, 0x698, 0x640, 0x64E, ' ', ' ', 0x200C, 'A'};
constexpr size_t kArabicSize = sizeof kArabic / sizeof *kArabic;

/** The Arabic fixture, for kashida; null (and the other face is used) if it cannot be read. */
struct ArabicFace {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  ArabicFace() {
    std::ifstream in("tests/data/fonts/arabic-join.ttf", std::ios::binary);   // run from the root
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (bytes.empty() ||
        gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED, nullptr, nullptr,
            &blob, nullptr) != GFNT_OK ||
        gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr) != GFNT_OK) {
      face = nullptr;
    }
  }
};

struct Face {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  Face() {
    std::vector<uint32_t> chars(kAlphabet, kAlphabet + kAlphabetSize);
    std::vector<std::pair<uint32_t, uint16_t>> order;
    for (size_t i = 0; i < chars.size(); ++i) {
      order.push_back({chars[i], static_cast<uint16_t>(i + 1)});
    }
    std::sort(order.begin(), order.end());
    std::vector<gfnttest::Segment4> segments;
    uint32_t last = 0xFFFFFFFFu;
    for (const auto & o : order) {
      if (o.first == last) {
        continue;
      }
      last = o.first;
      segments.push_back({static_cast<uint16_t>(o.first), static_cast<uint16_t>(o.first),
          static_cast<int16_t>(o.second - o.first), {}});
    }
    segments.push_back({0xFFFF, 0xFFFF, 1, {}});
    const uint16_t glyphs = static_cast<uint16_t>(chars.size() + 1);
    std::vector<std::pair<uint16_t, int16_t>> metrics(glyphs, {500, 0});
    std::vector<std::vector<uint8_t>> shapes;
    shapes.push_back({});
    for (size_t i = 0; i < chars.size(); ++i) {
      shapes.push_back(gfnttest::build_glyf_glyph(
          {{{50, 0, true}, {50, 700, true}, {450, 700, true}, {450, 0, true}}}));
    }
    std::vector<uint8_t> glyf, loca;
    bool long_form = false;
    gfnttest::build_glyf_and_loca(shapes, &glyf, &loca, &long_form);
    // vhea (36 bytes) and vmtx: every glyph 900 down, so that upright and turned text differ.
    std::vector<uint8_t> vhea, vmtx;
    gfnttest::put_u32(vhea, 0x00011000);
    gfnttest::put_u16(vhea, 500);
    gfnttest::put_u16(vhea, static_cast<uint16_t>(-500));
    for (int i = 0; i < 12; ++i) {
      gfnttest::put_u16(vhea, 0);
    }
    gfnttest::put_u16(vhea, 0);
    gfnttest::put_u16(vhea, 0);
    gfnttest::put_u16(vhea, glyphs);
    for (uint16_t g = 0; g < glyphs; ++g) {
      gfnttest::put_u16(vmtx, 900);
      gfnttest::put_u16(vmtx, 100);
    }
    std::vector<gfnttest::Table> tables = {
        {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, long_form ? 1 : 0)},
        {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, glyphs)},
        {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics)},
        {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(glyphs)},
        {GFNT_TAG('c', 'm', 'a', 'p'),
            gfnttest::build_cmap({{3, 1, gfnttest::build_cmap_format4(segments)}})},
        {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
        {GFNT_TAG('l', 'o', 'c', 'a'), loca},
        {GFNT_TAG('v', 'h', 'e', 'a'), vhea},
        {GFNT_TAG('v', 'm', 't', 'x'), vmtx},
    };
    bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
    if (gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED, nullptr, nullptr,
            &blob, nullptr) != GFNT_OK
        || gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr) != GFNT_OK) {
      abort();   // the harness's own face must load
    }
  }
  ~Face() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
};

/**
 * Whether a rectangle lies inside the layout across its lines (a row's height, a column's
 * width). Along the lines text that does not fit overflows the room, as it does in a browser, so
 * that is not checked.
 */
bool inside(const GFNT_Rect & r, const GFNT_Layout & l) {
  const int64_t slack = 64;   // a hair for rounding at the edges
  const bool vertical = l.writing_mode != GFNT_WRITING_HORIZONTAL_TB;

  if (r.width < 0 || r.height < 0) {
    return false;
  }
  return vertical ? r.x >= -slack && static_cast<int64_t>(r.x) + r.width <= l.width + slack
                  : r.y >= -slack && static_cast<int64_t>(r.y) + r.height <= l.height + slack;
}

GFNT_Result fuzz_band(void * context, GFNT_F26Dot6 top, GFNT_F26Dot6,
    GFNT_Band * out) {
  const GFNT_F26Dot6 width = *static_cast<const GFNT_F26Dot6 *>(context);
  out->start = (top / (20 * 64) & 1) ? 32 : 0;
  out->width = width;
  out->next_top = 0;
  return GFNT_OK;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  static Face * face = new Face();
  static ArabicFace * arabic = new ArabicFace();
  if (size < 4) {
    return 0;
  }
  const uint8_t a = data[0], b = data[1], room_byte = data[2];
  std::string text;
  if (a & 0x80) {
    text.assign(reinterpret_cast<const char *>(data + 3), size - 3);
  }
  else {
    const bool use_arabic = (data[1] & 0x03) == 1 && (data[2] & 0x80) && arabic->face;
    for (size_t i = 3; i < size; ++i) {
      utf8(use_arabic ? kArabic[data[i] % kArabicSize] : kAlphabet[data[i] % kAlphabetSize],
          &text);
    }
  }
  if (text.size() > 600) {
    text.resize(600);
  }
  GFNT_ParagraphOptions o{};
  o.writing_mode = static_cast<GFNT_WritingMode>((a & 0x07) % 5);
  o.text_orientation = static_cast<GFNT_TextOrientation>(((a >> 3) & 0x03) % 4);
  o.align = static_cast<GFNT_Align>(((a >> 5) & 0x03) + ((b >> 7) ? 3 : 0));
  if (o.align > GFNT_ALIGN_JUSTIFY) {
    o.align = GFNT_ALIGN_JUSTIFY;
  }
  static const GFNT_Justify kJustify[] = {GFNT_JUSTIFY_AUTO, GFNT_JUSTIFY_INTER_WORD,
      GFNT_JUSTIFY_INTER_CHARACTER, GFNT_JUSTIFY_NONE};
  o.justify = kJustify[b & 0x03];
  // Inter-word with the top bit of the room set is kashida, in the Arabic fixture, with the
  // rest of the room byte as the share of the slack the spaces take.
  const bool kashida = (b & 0x03) == 1 && (room_byte & 0x80);
  if (kashida) {
    o.justify = GFNT_JUSTIFY_KASHIDA;
    o.space_share = static_cast<GFNT_F16Dot16>((room_byte & 0x7F) * 0x200);
  }
  o.direction = static_cast<GFNT_BidiDirection>(((b >> 2) & 0x03) % 3);
  o.line_break = static_cast<GFNT_LineBreak>(((b >> 4) & 0x03));
  o.hyphens = static_cast<GFNT_Hyphens>(((b >> 6) & 0x03) % 4 == 3 ? GFNT_HYPHENS_MANUAL_PANGO
      : ((b >> 6) & 0x03) % 3);
  if (((a >> 6) & 1) && !text.empty()) {
    o.max_lines = 1 + (b & 1);
    o.ellipsis = "A";
  }
  const GFNT_F26Dot6 room = room_byte == 0 ? GFNT_LAYOUT_UNBOUNDED
                                           : static_cast<GFNT_F26Dot6>(room_byte) * 64;
  const GFNT_Face * faces[1] = {kashida && arabic->face ? arabic->face : face->face};
  GFNT_TextSpan span{};
  span.length = text.size();
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = 20 * 64;
  GFNT_Annotation annotation{};
  GFNT_Annotation nested{};
  GFNT_Annotation nested_child{};
  if (!(a & 0x80) && !text.empty() && (data[3] & 0x01)) {
    annotation.base_start = 0;
    annotation.base_end = text.size();
    annotation.kind = (data[3] & 0x02) ? GFNT_ANNOTATION_EMPHASIS : GFNT_ANNOTATION_RUBY;
    annotation.position = (data[3] & 0x04) ? GFNT_ANNOTATION_UNDER : GFNT_ANNOTATION_OVER;
    annotation.style = span.style;
    if (annotation.kind == GFNT_ANNOTATION_RUBY) {
      annotation.text = "A";
      annotation.text_length = 1;
      if (data[3] & 0x10) {
        annotation.text = "AB";
        annotation.text_length = 2;
        nested.base_end = 2;
        nested.kind = GFNT_ANNOTATION_RUBY;
        nested.position = GFNT_ANNOTATION_UNDER;
        nested.style = span.style;
        nested.text = "A";
        nested.text_length = 1;
        if (data[3] & 0x20) {
          nested_child.base_end = 1;
          nested_child.kind = GFNT_ANNOTATION_RUBY;
          nested_child.position = GFNT_ANNOTATION_OVER;
          nested_child.style = span.style;
          nested_child.text = "B";
          nested_child.text_length = 1;
          nested.children = &nested_child;
          nested.child_count = 1;
        }
        annotation.children = &nested;
        annotation.child_count = 1;
      }
    }
    annotation.group_break = (data[3] & 0x08) != 0;
    annotation.mark_kind = annotation.kind == GFNT_ANNOTATION_EMPHASIS
        ? GFNT_EMPHASIS_MARK_DOT : GFNT_EMPHASIS_MARK_DEFAULT;
    annotation.filled = true;
    o.annotations = &annotation;
    o.annotation_count = 1;
  }
  GFNT_Paragraph * p = nullptr;
  const GFNT_Result created = gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o,
      nullptr, &p, nullptr);
  if (created != GFNT_OK) {
    check(created == GFNT_ERR_INVALID || created == GFNT_ERR_LIMIT || created == GFNT_ERR_OOM);
    return 0;
  }
  GFNT_Layout l{}, again{}, flow{}, flow_again{};
  GFNT_F26Dot6 min = 0, max = 0;

  check(gfnt_paragraph_layout(p, room, &l, nullptr) == GFNT_OK);
  check(gfnt_paragraph_layout(p, room, &again, nullptr) == GFNT_OK);
  GFNT_F26Dot6 flow_width = room == GFNT_LAYOUT_UNBOUNDED ? 8 * 20 * 64
      : std::max<GFNT_F26Dot6>(64, room / 2);
  GFNT_LineConstraint constraint{&flow_width, fuzz_band};
  check(gfnt_paragraph_layout_flow(p, &constraint, &flow, nullptr) == GFNT_OK);
  check(gfnt_paragraph_layout_flow(p, &constraint, &flow_again, nullptr) == GFNT_OK);
  check(gfnt_paragraph_min_content(p, &min) == GFNT_OK);
  check(gfnt_paragraph_max_content(p, &max) == GFNT_OK);
  check(min >= 0 && max >= min);
  check(l.line_count == again.line_count && l.width == again.width && l.height == again.height);
  check(l.line_count >= 1 && l.width >= 0 && l.height >= 0);
  check(l.writing_mode == o.writing_mode);
  check(flow.line_count >= 1 && flow.width >= 0 && flow.height >= 0);
  check(flow.line_count == flow_again.line_count && flow.width == flow_again.width
      && flow.height == flow_again.height);

  GFNT_ColumnOptions column_options{};
  column_options.inline_size = std::max<GFNT_F26Dot6>(64, flow_width);
  column_options.inline_gap = static_cast<GFNT_F26Dot6>(data[3] & 0x1F) * 32;
  column_options.block_extent = static_cast<GFNT_F26Dot6>(1 + ((data[3] >> 2) & 0x3F)) * 32;
  column_options.count = 1 + ((data[3] >> 1) & 0x03);
  column_options.fill = (data[3] & 0x40) ? GFNT_COLUMN_FILL_AUTO : GFNT_COLUMN_FILL_BALANCE;
  column_options.overflow = (data[3] & 0x80)
      ? GFNT_COLUMN_OVERFLOW_ERROR : GFNT_COLUMN_OVERFLOW_PROGRESS;
  column_options.orphans = 1 + ((a >> 3) & 0x03);
  column_options.widows = 1 + ((b >> 3) & 0x03);
  GFNT_ColumnLayout columns{}, columns_again{};
  const GFNT_Result column_result = gfnt_paragraph_layout_columns(p, &column_options,
      &columns, nullptr);
  if (column_result == GFNT_OK) {
    check(columns.column_count <= column_options.count);
    check(columns.column_line_starts != nullptr);
    check(columns.column_line_starts[0] == 0);
    check(columns.column_line_starts[columns.column_count] == columns.layout.line_count);
    check(columns.next_byte <= text.size());
    check(columns.complete ? columns.next_byte == text.size() : columns.next_byte <= text.size());
    check(gfnt_paragraph_layout_columns(p, &column_options, &columns_again, nullptr) == GFNT_OK);
    check(columns.column_count == columns_again.column_count
        && columns.next_byte == columns_again.next_byte && columns.complete == columns_again.complete
        && columns.layout.line_count == columns_again.layout.line_count);
    for (size_t ci = 0; ci <= columns.column_count; ci++) {
      check(columns.column_line_starts[ci] == columns_again.column_line_starts[ci]);
    }
    for (size_t li = 0; li < columns.layout.line_count; li++) {
      const GFNT_Line & line = columns.layout.lines[li];
      const GFNT_Line & repeated = columns_again.layout.lines[li];
      check(line.start == repeated.start && line.end == repeated.end
          && line.extent == repeated.extent && line.baseline == repeated.baseline
          && line.box.x == repeated.box.x && line.box.y == repeated.box.y
          && line.box.width == repeated.box.width && line.box.height == repeated.box.height);
    }
  }
  else {
    check(column_result == GFNT_ERR_LIMIT || column_result == GFNT_ERR_INVALID
        || column_result == GFNT_ERR_OOM);
  }
  gfnt_column_layout_free(&columns_again);
  gfnt_column_layout_free(&columns);

  size_t at = 0;
  for (size_t i = 0; i < l.line_count; ++i) {
    const GFNT_Line & line = l.lines[i];
    check(line.start == at || (o.max_lines && i + 1 == l.line_count) || i == 0);
    check(line.start <= line.end && line.end <= text.size());
    at = line.end;
    check(inside(line.box, l));
    check(line.extent >= 0);
    check(line.annotation_count == again.lines[i].annotation_count);
    for (size_t ai = 0; ai < line.annotation_count; ai++) {
      check(line.annotations[ai].base_start <= line.annotations[ai].base_end);
      check(line.annotations[ai].run_count > 0 && line.annotations[ai].runs != nullptr);
      check(line.annotations[ai].kind == again.lines[i].annotations[ai].kind);
    }
    for (size_t k = 0; k < line.run_count; ++k) {
      const GFNT_LineRun & run = line.runs[k];
      check(run.advance >= -64 * 1024 * 1024);
      for (size_t g = 0; g < run.glyph_count; ++g) {
        const GFNT_PlacedGlyph & pg = run.glyphs[g];
        if (!run.ellipsis && !run.hyphen && !(pg.cluster >= line.start && pg.cluster <= line.end)) {
          fprintf(stderr, "line %zu [%zu,%zu] run %zu [%zu,%zu] level %u glyph %zu cluster %u\n", i,
              line.start, line.end, k, run.start, run.end, run.level, g, pg.cluster);
        }
        if (!run.ellipsis && !run.hyphen) {
          check(pg.cluster >= line.start && pg.cluster <= line.end);
        }
      }
    }
    check(again.lines[i].start == line.start && again.lines[i].end == line.end
        && again.lines[i].extent == line.extent);
  }
  size_t flow_at = 0;
  for (size_t i = 0; i < flow.line_count; ++i) {
    const GFNT_Line & line = flow.lines[i];
    const GFNT_Line & repeated = flow_again.lines[i];
    check(line.start == flow_at || (o.max_lines && i + 1 == flow.line_count) || i == 0);
    check(line.start <= line.end && line.end <= text.size());
    check(inside(line.box, flow));
    check(line.start == repeated.start && line.end == repeated.end
        && line.extent == repeated.extent && line.baseline == repeated.baseline
        && line.box.x == repeated.box.x && line.box.y == repeated.box.y
        && line.box.width == repeated.box.width && line.box.height == repeated.box.height
        && line.run_count == repeated.run_count
        && line.annotation_count == repeated.annotation_count);
    if (o.writing_mode == GFNT_WRITING_HORIZONTAL_TB) {
      const GFNT_F26Dot6 expected_start = ((line.box.y / (20 * 64)) & 1) ? 32 : 0;
      check(line.box.x == expected_start && line.box.width == flow_width);
    }
    for (size_t j = 0; j < line.run_count; ++j) {
      const GFNT_LineRun & run = line.runs[j];
      check(j < repeated.run_count);
      check(run.start == repeated.runs[j].start && run.end == repeated.runs[j].end
          && run.pos == repeated.runs[j].pos && run.advance == repeated.runs[j].advance
          && run.glyph_count == repeated.runs[j].glyph_count);
      check(run.glyph_count == repeated.runs[j].glyph_count);
      for (size_t g = 0; g < run.glyph_count; ++g) {
        const GFNT_PlacedGlyph & glyph = run.glyphs[g];
        const GFNT_PlacedGlyph & repeat_glyph = repeated.runs[j].glyphs[g];
        check(run.ellipsis || run.hyphen
            || (glyph.cluster >= line.start && glyph.cluster <= line.end));
        check(glyph.cluster == repeat_glyph.cluster && glyph.pen_x == repeat_glyph.pen_x
            && glyph.pen_y == repeat_glyph.pen_y && glyph.x == repeat_glyph.x
            && glyph.y == repeat_glyph.y && glyph.advance == repeat_glyph.advance);
      }
    }
    flow_at = line.end;
  }
  if (!flow.truncated) {
    check(flow_at == text.size());
  }

  // The questions the layout answers.
  for (size_t off = 0; off <= text.size(); off += 1 + text.size() / 24) {
    GFNT_Caret c;
    GFNT_Hit hit;
    if (gfnt_layout_caret(p, &l, off, GFNT_AFFINITY_DOWNSTREAM, &c) != GFNT_OK) {
      continue;
    }
    check(c.line < l.line_count && c.length >= 0);
    check(c.horizontal == (o.writing_mode != GFNT_WRITING_HORIZONTAL_TB));
    // The middle of the bar, nudged into the line.
    const GFNT_F26Dot6 px = c.horizontal ? c.x + c.length / 2 : c.x;
    const GFNT_F26Dot6 py = c.horizontal ? c.y : c.y + c.length / 2;
    check(gfnt_layout_hit_test(p, &l, px, py, &hit) == GFNT_OK);
    check(hit.offset <= text.size() && hit.line < l.line_count);
    GFNT_Caret back;
    check(gfnt_layout_caret(p, &l, hit.offset, hit.affinity, &back) == GFNT_OK);
  }
  {
    size_t count = 0;
    check(gfnt_layout_range_rects(p, &l, 0, text.size(), nullptr, 0, &count) == GFNT_OK);
    std::vector<GFNT_Rect> rects(count < 4096 ? count : 4096);
    size_t got = 0;
    check(gfnt_layout_range_rects(p, &l, 0, text.size(), rects.data(), rects.size(), &got)
        == GFNT_OK);
    check(got == count);
    for (const GFNT_Rect & r : rects) {
      check(inside(r, l));
    }
  }
  for (size_t off = 0; off <= text.size(); off += 1 + text.size() / 24) {
    GFNT_Caret caret{};
    GFNT_Hit hit{};
    if (gfnt_layout_caret(p, &flow, off, GFNT_AFFINITY_DOWNSTREAM, &caret) != GFNT_OK) continue;
    check(caret.line < flow.line_count && caret.length >= 0);
    const GFNT_F26Dot6 x = caret.horizontal ? caret.x + caret.length / 2 : caret.x;
    const GFNT_F26Dot6 y = caret.horizontal ? caret.y : caret.y + caret.length / 2;
    check(gfnt_layout_hit_test(p, &flow, x, y, &hit) == GFNT_OK);
    check(hit.line < flow.line_count && hit.offset <= text.size());
  }
  {
    size_t count = 0;
    check(gfnt_layout_range_rects(p, &flow, 0, text.size(), nullptr, 0, &count) == GFNT_OK);
    std::vector<GFNT_Rect> rects(count < 4096 ? count : 4096);
    size_t got = 0;
    check(gfnt_layout_range_rects(p, &flow, 0, text.size(), rects.data(), rects.size(), &got)
        == GFNT_OK);
    check(got == count);
    for (const GFNT_Rect & rect : rects) check(inside(rect, flow));
  }
  // Exercise an explicit atomic object on every seed as well as the arbitrary text above.
  {
    static const char marker[] = "\xEF\xBF\xBC";
    GFNT_InlineObject object{};
    GFNT_TextSpan object_span{};
    GFNT_ParagraphOptions object_options = o;
    GFNT_Paragraph * object_paragraph = nullptr;
    GFNT_Layout object_layout{};
    GFNT_F26Dot6 object_min = 0, object_max = 0;
    object.advance = static_cast<GFNT_F26Dot6>(1 + (b & 0x3F)) * 64;
    object.ascent = static_cast<GFNT_F26Dot6>(1 + ((a >> 2) & 0x1F)) * 32;
    object.descent = static_cast<GFNT_F26Dot6>(1 + ((a >> 4) & 0x0F)) * 16;
    object.break_before = (a & 1) != 0;
    object.break_after = (b & 1) != 0;
    object.user = &object;
    object_span.length = 3;
    object_span.style.faces = faces;
    object_span.style.face_count = 1;
    object_span.style.size = 20 * 64;
    object_span.style.inline_object = &object;
    object_options.max_lines = 0;
    object_options.ellipsis = nullptr;
    object_options.annotations = nullptr;
    object_options.annotation_count = 0;
    check(gfnt_paragraph_create(marker, 3, &object_span, 1, &object_options, nullptr,
        &object_paragraph, nullptr) == GFNT_OK);
    check(gfnt_paragraph_layout(object_paragraph, room, &object_layout, nullptr) == GFNT_OK);
    check(object_layout.line_count >= 1 && object_layout.lines[0].run_count == 1);
    check(object_layout.lines[0].runs[0].object == &object);
    check(gfnt_paragraph_min_content(object_paragraph, &object_min) == GFNT_OK);
    check(gfnt_paragraph_max_content(object_paragraph, &object_max) == GFNT_OK);
    check(object_min >= 0 && object_max >= object_min);
    size_t object_rect_count = 0;
    check(gfnt_layout_range_rects(object_paragraph, &object_layout, 0, 3,
        nullptr, 0, &object_rect_count) == GFNT_OK);
    GFNT_Hit object_hit{};
    check(gfnt_layout_hit_test(object_paragraph, &object_layout, 0, 0, &object_hit) == GFNT_OK);
    GFNT_Caret object_caret{};
    check(gfnt_layout_caret(object_paragraph, &object_layout, object_hit.offset,
        object_hit.affinity, &object_caret) == GFNT_OK);
    gfnt_layout_free(&object_layout);
    gfnt_paragraph_free(object_paragraph);
  }
  gfnt_layout_free(&l);
  gfnt_layout_free(&again);
  gfnt_layout_free(&flow);
  gfnt_layout_free(&flow_again);
  gfnt_paragraph_free(p);
  return 0;
}
