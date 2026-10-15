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
 * Vertical paragraph layout (CSS Writing Modes): columns that stack from the right or the left,
 * characters upright or turned by UAX #50, the half turn of sideways-lr, and hit testing in
 * columns. `vertical-text.ttf` has an em of 1000, ascent 880 and descent 120 (so a line is as
 * thick as the font size), vertical advances unlike the horizontal ones, and vertical forms for
 * its CJK punctuation. At 100 pixels an ideograph is 100 wide and 100 tall, a Latin A 70 across
 * when turned and 90 down when upright. `make check-oracle-css` holds the same cases to Chromium.
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/layout.h>

namespace {

constexpr GFNT_F26Dot6 px(int n) {
  return n * 64;
}

constexpr GFNT_F26Dot6 kSize = px(100);

struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  Font() {
    std::ifstream in(gfnttest::data("fonts/vertical-text.ttf"), std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    EXPECT_FALSE(bytes.empty());
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED, nullptr,
                  nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, nullptr), GFNT_OK);
  }
  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;
  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
};

const char kJi[] = "\xE6\x97\xA5";     // 日
const char kHon[] = "\xE6\x9C\xAC";    // 本
const char kGo[] = "\xE8\xAA\x9E";     // 語

struct Para {
  GFNT_Paragraph * p = nullptr;
  GFNT_Layout layout{};
  const GFNT_Face * faces[1];
  GFNT_Result created = GFNT_ERR_INTERNAL;

  Para(const Font & font, const std::string & text, GFNT_WritingMode mode,
      GFNT_TextOrientation orientation = GFNT_ORIENT_MIXED,
      GFNT_BidiDirection direction = GFNT_BIDI_LTR, GFNT_Align align = GFNT_ALIGN_START) {
    faces[0] = font.face;
    GFNT_TextSpan span{};
    span.length = text.size();
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = kSize;
    GFNT_ParagraphOptions o{};
    o.writing_mode = mode;
    o.text_orientation = orientation;
    o.direction = direction;
    o.align = align;
    o.text_autospace = GFNT_TEXT_AUTOSPACE_NONE;
    created = gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o, nullptr, &p,
        nullptr);
  }
  GFNT_Result lay(GFNT_F26Dot6 room) {
    gfnt_layout_free(&layout);
    return gfnt_paragraph_layout(p, room, &layout, nullptr);
  }
  ~Para() {
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(p);
  }
  Para(const Para &) = delete;
  Para & operator=(const Para &) = delete;
};

std::string cjk(int times) {
  std::string s;
  for (int i = 0; i < times; ++i) {
    s += kJi;
    s += kHon;
    s += kGo;
  }
  return s;
}

TEST(LayoutVertical, ColumnsStackFromTheRightAndTheTextRunsDown) {
  Font font;
  Para para(font, cjk(2), GFNT_WRITING_VERTICAL_RL);
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(px(250)), GFNT_OK);
  const GFNT_Layout & l = para.layout;
  ASSERT_EQ(l.line_count, 3u) << "two ideographs fit in 250";
  EXPECT_EQ(l.writing_mode, GFNT_WRITING_VERTICAL_RL);
  EXPECT_EQ(l.width, px(300)) << "three columns of the font size";
  EXPECT_EQ(l.height, px(250)) << "the room along the lines";
  for (size_t i = 0; i < 3; ++i) {
    const GFNT_Line & line = l.lines[i];
    EXPECT_EQ(line.box.width, px(100)) << i;
    EXPECT_EQ(line.box.height, px(250)) << i;
    EXPECT_EQ(line.box.y, 0) << i;
    EXPECT_EQ(line.box.x, px(200 - 100 * static_cast<int>(i))) << "the first line is rightmost";
    EXPECT_EQ(line.baseline, line.box.x + px(50)) << "the central axis";
    EXPECT_EQ(line.extent, px(200)) << i;
    EXPECT_EQ(line.offset, 0) << i;
  }
  const GFNT_LineRun & run = l.lines[0].runs[0];
  EXPECT_EQ(run.orientation, GFNT_ORIENT_RUN_UPRIGHT);
  ASSERT_EQ(run.glyph_count, 2u);
  EXPECT_EQ(run.glyphs[0].pen_y, 0);
  EXPECT_EQ(run.glyphs[1].pen_y, px(100)) << "down the line";
  EXPECT_EQ(run.glyphs[0].advance, px(100)) << "the vertical advance, not the horizontal one";
  EXPECT_EQ(run.glyphs[0].pen_x, l.lines[0].baseline) << "on the axis";
  EXPECT_EQ(run.pos, 0);
  EXPECT_EQ(run.advance, px(200));
}

TEST(LayoutVertical, VerticalLrStacksFromTheLeft) {
  Font font;
  Para para(font, cjk(2), GFNT_WRITING_VERTICAL_LR);
  ASSERT_EQ(para.lay(px(250)), GFNT_OK);
  const GFNT_Layout & l = para.layout;
  ASSERT_EQ(l.line_count, 3u);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(l.lines[i].box.x, px(100 * static_cast<int>(i))) << i;
  }
  EXPECT_EQ(l.lines[0].runs[0].orientation, GFNT_ORIENT_RUN_UPRIGHT);
}

TEST(LayoutVertical, LatinIsTurnedAndTakesItsHorizontalAdvance) {
  Font font;
  const std::string text = std::string(kJi) + "AB" + kHon;
  Para para(font, text, GFNT_WRITING_VERTICAL_RL);
  ASSERT_EQ(para.lay(px(600)), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_EQ(line.run_count, 3u) << "upright, turned, upright";
  EXPECT_EQ(line.runs[0].orientation, GFNT_ORIENT_RUN_UPRIGHT);
  EXPECT_EQ(line.runs[1].orientation, GFNT_ORIENT_RUN_CW);
  EXPECT_EQ(line.runs[2].orientation, GFNT_ORIENT_RUN_UPRIGHT);
  const GFNT_LineRun & turned = line.runs[1];
  ASSERT_EQ(turned.glyph_count, 2u);
  EXPECT_EQ(turned.glyphs[0].advance, px(70)) << "A across";
  EXPECT_EQ(turned.glyphs[1].advance, px(65)) << "B across";
  EXPECT_EQ(turned.glyphs[0].pen_y, px(100));
  EXPECT_EQ(turned.glyphs[1].pen_y, px(170));
  // The em box is centred on the axis: the baseline lies half of (ascent - descent) before it.
  EXPECT_EQ(turned.glyphs[0].pen_x, line.baseline - px(38));
  EXPECT_EQ(line.runs[2].pos, px(235));
}

TEST(LayoutVertical, UprightOrientationSetsLatinOnItsVerticalAdvance) {
  Font font;
  const std::string text = std::string(kJi) + "AB";
  Para para(font, text, GFNT_WRITING_VERTICAL_RL, GFNT_ORIENT_UPRIGHT);
  ASSERT_EQ(para.lay(px(600)), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_EQ(line.run_count, 2u) << "the script changes";
  EXPECT_EQ(line.runs[0].orientation, GFNT_ORIENT_RUN_UPRIGHT);
  EXPECT_EQ(line.runs[1].orientation, GFNT_ORIENT_RUN_UPRIGHT);
  ASSERT_EQ(line.runs[1].glyph_count, 2u);
  EXPECT_EQ(line.runs[1].glyphs[0].advance, px(90)) << "A down";
  EXPECT_EQ(line.runs[1].glyphs[1].advance, px(90)) << "B down";
}

TEST(LayoutVertical, SidewaysOrientationTurnsEverything) {
  Font font;
  const std::string text = std::string(kJi) + "AB";
  Para para(font, text, GFNT_WRITING_VERTICAL_RL, GFNT_ORIENT_SIDEWAYS);
  ASSERT_EQ(para.lay(px(600)), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_EQ(line.run_count, 2u) << "the script changes";
  EXPECT_EQ(line.runs[0].orientation, GFNT_ORIENT_RUN_CW);
  EXPECT_EQ(line.runs[1].orientation, GFNT_ORIENT_RUN_CW);
  EXPECT_EQ(line.runs[0].glyphs[0].advance, px(100)) << "the ideograph's horizontal advance";
  Para same(font, text, GFNT_WRITING_SIDEWAYS_RL);
  ASSERT_EQ(same.lay(px(600)), GFNT_OK);
  ASSERT_EQ(same.layout.lines[0].run_count, 2u);
  EXPECT_EQ(same.layout.lines[0].runs[0].orientation, GFNT_ORIENT_RUN_CW);
}

TEST(LayoutVertical, SidewaysLrIsSidewaysRlTurnedHalfAWay) {
  Font font;
  const std::string text = std::string(kJi) + "AB" + kHon + kGo + kJi;
  Para rl(font, text, GFNT_WRITING_SIDEWAYS_RL);
  Para lr(font, text, GFNT_WRITING_SIDEWAYS_LR);
  ASSERT_EQ(rl.lay(px(350)), GFNT_OK);
  ASSERT_EQ(lr.lay(px(350)), GFNT_OK);
  ASSERT_EQ(rl.layout.line_count, lr.layout.line_count);
  const GFNT_F26Dot6 w = rl.layout.width, h = rl.layout.height;
  EXPECT_EQ(lr.layout.width, w);
  EXPECT_EQ(lr.layout.height, h);
  for (size_t i = 0; i < rl.layout.line_count; ++i) {
    const GFNT_Line & a = rl.layout.lines[i];
    const GFNT_Line & b = lr.layout.lines[i];
    EXPECT_EQ(b.box.x, w - (a.box.x + a.box.width)) << "columns stack from the left";
    EXPECT_EQ(b.offset, h - (a.offset + a.extent)) << "the text begins at the bottom";
    ASSERT_EQ(a.run_count, b.run_count);
    for (size_t k = 0; k < a.run_count; ++k) {
      EXPECT_EQ(a.runs[k].orientation, GFNT_ORIENT_RUN_CW);
      EXPECT_EQ(b.runs[k].orientation, GFNT_ORIENT_RUN_CCW);
      ASSERT_EQ(a.runs[k].glyph_count, b.runs[k].glyph_count);
      for (size_t g = 0; g < a.runs[k].glyph_count; ++g) {
        EXPECT_EQ(b.runs[k].glyphs[g].pen_x, w - a.runs[k].glyphs[g].pen_x);
        EXPECT_EQ(b.runs[k].glyphs[g].pen_y, h - a.runs[k].glyphs[g].pen_y);
        EXPECT_EQ(b.runs[k].glyphs[g].x, w - a.runs[k].glyphs[g].x);
        EXPECT_EQ(b.runs[k].glyphs[g].y, h - a.runs[k].glyphs[g].y);
      }
    }
  }
  EXPECT_EQ(lr.layout.lines[0].runs[0].glyphs[0].pen_y, h) << "the first character is at the bottom";
}

TEST(LayoutVertical, ThePunctuationTakesItsVerticalFormAndStaysUpright) {
  Font font;
  const std::string text = std::string(kJi) + "\xE3\x80\x81" + "\xE3\x80\x8C" + "\xE3\x83\xBC";
  Para para(font, text, GFNT_WRITING_VERTICAL_RL);
  ASSERT_EQ(para.lay(px(600)), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_EQ(line.run_count, 1u);
  EXPECT_EQ(line.runs[0].orientation, GFNT_ORIENT_RUN_UPRIGHT);
  ASSERT_EQ(line.runs[0].glyph_count, 4u);
  // The ideographic comma's vertical form is another glyph from its horizontal one.
  Para row(font, "\xE3\x80\x81", GFNT_WRITING_HORIZONTAL_TB);
  ASSERT_EQ(row.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  EXPECT_NE(line.runs[0].glyphs[1].glyph, row.layout.lines[0].runs[0].glyphs[0].glyph);
}

TEST(LayoutVertical, ATransformedRotatedCharacterWithoutAVerticalFormIsTurned) {
  Font font;
  // U+2026 is Tr and the font has no vertical form for it; U+2014 is Tr and the font has one.
  Para para(font, std::string(kJi) + "\xE2\x80\xA6" + "\xE2\x80\x94", GFNT_WRITING_VERTICAL_RL);
  ASSERT_EQ(para.lay(px(600)), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_EQ(line.run_count, 2u);
  EXPECT_EQ(line.runs[0].orientation, GFNT_ORIENT_RUN_UPRIGHT) << "the ideograph";
  EXPECT_EQ(line.runs[0].glyph_count, 1u);
  EXPECT_EQ(line.runs[1].orientation, GFNT_ORIENT_RUN_CW) << "the ellipsis, which has no form";
  EXPECT_EQ(line.runs[1].glyph_count, 2u) << "and the dash, which has one but follows it";
}

TEST(LayoutVertical, PangoTurnsTheBracketsCssSetsUprightWhenTheFontHasAVerticalForm) {
  Font font;
  // U+300C has a vertical form in this font. CSS sets it upright; Pango's gravity turns it.
  const std::string text = std::string(kJi) + "\xE3\x80\x8C" + kHon;
  Para css(font, text, GFNT_WRITING_VERTICAL_RL);
  ASSERT_EQ(css.lay(px(600)), GFNT_OK);
  ASSERT_EQ(css.layout.lines[0].run_count, 1u);
  EXPECT_EQ(css.layout.lines[0].runs[0].orientation, GFNT_ORIENT_RUN_UPRIGHT);
  Para pango(font, text, GFNT_WRITING_VERTICAL_RL, GFNT_ORIENT_MIXED_PANGO);
  ASSERT_EQ(pango.lay(px(600)), GFNT_OK);
  const GFNT_Line & line = pango.layout.lines[0];
  ASSERT_EQ(line.run_count, 3u);
  EXPECT_EQ(line.runs[0].orientation, GFNT_ORIENT_RUN_UPRIGHT);
  EXPECT_EQ(line.runs[1].orientation, GFNT_ORIENT_RUN_CW);
  EXPECT_EQ(line.runs[2].orientation, GFNT_ORIENT_RUN_UPRIGHT);
  // The ideographic full stop is upright in both.
  Para stop(font, std::string(kJi) + "\xE3\x80\x82", GFNT_WRITING_VERTICAL_RL,
      GFNT_ORIENT_MIXED_PANGO);
  ASSERT_EQ(stop.lay(px(600)), GFNT_OK);
  ASSERT_EQ(stop.layout.lines[0].run_count, 1u);
  EXPECT_EQ(stop.layout.lines[0].runs[0].orientation, GFNT_ORIENT_RUN_UPRIGHT);
}

TEST(LayoutVertical, HorizontalMetricsAreNotTheVerticalOnes) {
  Font font;
  Para sideways(font, "AB", GFNT_WRITING_VERTICAL_RL);
  ASSERT_EQ(sideways.lay(px(600)), GFNT_OK);
  Para upright(font, "AB", GFNT_WRITING_VERTICAL_RL, GFNT_ORIENT_UPRIGHT);
  ASSERT_EQ(upright.lay(px(600)), GFNT_OK);
  EXPECT_EQ(sideways.layout.lines[0].extent, px(135));
  EXPECT_EQ(upright.layout.lines[0].extent, px(180));
}

TEST(LayoutVertical, TheLinesAreAlignedAlongTheirLength) {
  Font font;
  struct Case { GFNT_Align align; int offset; };
  for (const Case & c : {Case{GFNT_ALIGN_START, 0}, Case{GFNT_ALIGN_END, 100},
           Case{GFNT_ALIGN_CENTER, 50}, Case{GFNT_ALIGN_LEFT, 0}, Case{GFNT_ALIGN_RIGHT, 100}}) {
    Para para(font, std::string(kJi) + kHon, GFNT_WRITING_VERTICAL_RL, GFNT_ORIENT_MIXED,
        GFNT_BIDI_LTR, c.align);
    ASSERT_EQ(para.lay(px(300)), GFNT_OK);
    EXPECT_EQ(para.layout.lines[0].offset, px(c.offset)) << c.align;
    EXPECT_EQ(para.layout.lines[0].runs[0].pos, px(c.offset)) << c.align;
    EXPECT_EQ(para.layout.lines[0].runs[0].glyphs[0].pen_y, px(c.offset)) << c.align;
  }
}

TEST(LayoutVertical, RightToLeftSetsTheTextAgainstTheBottomButKeepsItReadingDown) {
  Font font;
  Para para(font, cjk(1), GFNT_WRITING_VERTICAL_RL, GFNT_ORIENT_MIXED, GFNT_BIDI_RTL);
  ASSERT_EQ(para.lay(px(400)), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_EQ(line.offset, px(100)) << "start is the bottom in a right-to-left paragraph";
  ASSERT_EQ(line.run_count, 1u);
  EXPECT_EQ(line.runs[0].glyphs[0].pen_y, px(100));
  EXPECT_EQ(line.runs[0].glyphs[2].pen_y, px(300));
}

TEST(LayoutVertical, ContentWidthsAreMeasuredAlongTheLine) {
  Font font;
  Para para(font, cjk(2), GFNT_WRITING_VERTICAL_RL);
  GFNT_F26Dot6 min = 0, max = 0;
  ASSERT_EQ(gfnt_paragraph_min_content(para.p, &min), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(para.p, &max), GFNT_OK);
  EXPECT_EQ(min, px(100)) << "an ideograph is the least that fits";
  EXPECT_EQ(max, px(600));
}

TEST(LayoutVertical, UnboundedLayoutIsAsLongAsTheLongestLine) {
  Font font;
  Para para(font, std::string(kJi) + kHon + "\n" + kGo, GFNT_WRITING_VERTICAL_RL);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  EXPECT_EQ(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.height, px(200));
  EXPECT_EQ(para.layout.width, px(200));
}

TEST(LayoutVertical, ACaretIsAHorizontalBarAcrossTheColumn) {
  Font font;
  Para para(font, cjk(2), GFNT_WRITING_VERTICAL_RL);
  ASSERT_EQ(para.lay(px(300)), GFNT_OK);
  GFNT_Caret c;
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 3, GFNT_AFFINITY_DOWNSTREAM, &c), GFNT_OK);
  EXPECT_TRUE(c.horizontal);
  EXPECT_EQ(c.line, 0u) << "the offset after the first ideograph";
  EXPECT_EQ(c.y, px(100));
  EXPECT_EQ(c.x, para.layout.lines[0].box.x);
  EXPECT_EQ(c.length, px(100));
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 9, GFNT_AFFINITY_DOWNSTREAM, &c), GFNT_OK);
  EXPECT_EQ(c.line, 1u) << "the fourth ideograph begins the second column";
  EXPECT_EQ(c.y, 0);
  EXPECT_EQ(c.x, para.layout.lines[1].box.x);
}

TEST(LayoutVertical, AHitTestFindsTheColumnByXAndTheCharacterByY) {
  Font font;
  Para para(font, cjk(2), GFNT_WRITING_VERTICAL_RL);
  ASSERT_EQ(para.lay(px(300)), GFNT_OK);
  GFNT_Hit hit;
  // The right column holds the first three ideographs (nine bytes).
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, px(250), px(180), &hit), GFNT_OK);
  EXPECT_EQ(hit.line, 0u);
  EXPECT_EQ(hit.offset, 6u) << "the lower half of the second ideograph is nearer its end";
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, px(250), px(110), &hit), GFNT_OK);
  EXPECT_EQ(hit.offset, 3u) << "and the upper half its start";
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, px(50), px(10), &hit), GFNT_OK);
  EXPECT_EQ(hit.line, 1u) << "left of the first column is the next one";
  EXPECT_EQ(hit.offset, 9u);
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, px(900), px(10), &hit), GFNT_OK);
  EXPECT_EQ(hit.line, 0u) << "beyond the right edge is the first column";
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, px(250), px(900), &hit), GFNT_OK);
  EXPECT_EQ(hit.offset, 9u) << "past the end of a column the offset is the end of its text";
}

TEST(LayoutVertical, HitTestingAndCaretsAgreeInSidewaysLr) {
  Font font;
  const std::string text = std::string(kJi) + "AB" + kHon + kGo + kJi;
  Para para(font, text, GFNT_WRITING_SIDEWAYS_LR);
  ASSERT_EQ(para.lay(px(350)), GFNT_OK);
  const GFNT_F26Dot6 h = para.layout.height;
  GFNT_Caret c;
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 0, GFNT_AFFINITY_DOWNSTREAM, &c), GFNT_OK);
  EXPECT_EQ(c.y, h) << "the text begins at the bottom";
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 3, GFNT_AFFINITY_DOWNSTREAM, &c), GFNT_OK);
  EXPECT_EQ(c.y, h - px(100));
  for (size_t off = 0; off <= text.size(); ++off) {
    GFNT_Caret down;
    GFNT_Hit hit;
    if (gfnt_layout_caret(para.p, &para.layout, off, GFNT_AFFINITY_DOWNSTREAM, &down) != GFNT_OK) {
      continue;
    }
    ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, down.x + px(50),
                  down.y == h ? down.y - 1 : down.y + 1, &hit), GFNT_OK);
    GFNT_Caret back;
    ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, hit.offset, hit.affinity, &back), GFNT_OK);
    EXPECT_EQ(back.y, down.y) << "offset " << off;
    EXPECT_EQ(back.line, down.line) << "offset " << off;
  }
}

TEST(LayoutVertical, SelectionRectanglesAreAsThickAsTheColumn) {
  Font font;
  Para para(font, cjk(2), GFNT_WRITING_VERTICAL_RL);
  ASSERT_EQ(para.lay(px(300)), GFNT_OK);
  GFNT_Rect r[4];
  size_t count = 0;
  ASSERT_EQ(gfnt_layout_range_rects(para.p, &para.layout, 3, 12, r, 4, &count), GFNT_OK);
  ASSERT_EQ(count, 2u) << "the rest of the first column, and the start of the second";
  EXPECT_EQ(r[0].x, para.layout.lines[0].box.x);
  EXPECT_EQ(r[0].width, px(100));
  EXPECT_EQ(r[0].y, px(100));
  EXPECT_EQ(r[0].height, px(200));
  EXPECT_EQ(r[1].x, para.layout.lines[1].box.x);
  EXPECT_EQ(r[1].y, 0);
  EXPECT_EQ(r[1].height, px(100));
}

TEST(LayoutVertical, SelectionRectanglesInSidewaysLrAreMeasuredFromTheBottom) {
  Font font;
  Para para(font, cjk(1), GFNT_WRITING_SIDEWAYS_LR);
  ASSERT_EQ(para.lay(px(400)), GFNT_OK);
  GFNT_Rect r;
  size_t count = 0;
  ASSERT_EQ(gfnt_layout_range_rects(para.p, &para.layout, 0, 3, &r, 1, &count), GFNT_OK);
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(r.y, px(300)) << "the first ideograph occupies the bottom 100";
  EXPECT_EQ(r.height, px(100));
}

TEST(LayoutVertical, AnEmptyColumnStillHasItsThickness) {
  Font font;
  Para para(font, std::string(kJi) + "\n\n" + kHon, GFNT_WRITING_VERTICAL_RL);
  ASSERT_EQ(para.lay(px(400)), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 3u);
  EXPECT_EQ(para.layout.width, px(300));
  EXPECT_EQ(para.layout.lines[1].box.width, px(100));
}

TEST(LayoutVertical, InkIsWhereTheTurnedGlyphsAre) {
  Font font;
  Para para(font, "A", GFNT_WRITING_VERTICAL_RL);
  ASSERT_EQ(para.lay(px(300)), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  // A is a box from (40, 0) to (660, 700) in font units, turned a quarter clockwise: its
  // 700 of height lie across the line and its 620 of width along it.
  EXPECT_EQ(line.ink.width, px(70));
  EXPECT_EQ(line.ink.height, px(62));
  EXPECT_EQ(line.ink.y, px(4));
  EXPECT_EQ(line.ink.x, line.runs[0].glyphs[0].pen_x);
}

/** A paragraph with a justification mode, for the tests below. */
struct Justified {
  GFNT_Paragraph * p = nullptr;
  GFNT_Layout layout{};
  const GFNT_Face * faces[1];

  Justified(const Font & font, const std::string & text, GFNT_WritingMode mode, GFNT_Justify how,
      GFNT_BidiDirection direction = GFNT_BIDI_LTR) {
    faces[0] = font.face;
    GFNT_TextSpan span{};
    span.length = text.size();
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = kSize;
    GFNT_ParagraphOptions o{};
    o.writing_mode = mode;
    o.align = GFNT_ALIGN_JUSTIFY;
    o.justify = how;
    o.direction = direction;
    o.text_autospace = GFNT_TEXT_AUTOSPACE_NONE;
    EXPECT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o, nullptr, &p, nullptr),
        GFNT_OK);
  }
  ~Justified() {
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(p);
  }
  Justified(const Justified &) = delete;
  Justified & operator=(const Justified &) = delete;
};

TEST(LayoutJustifyIdeographs, AutoSpreadsTheSlackOverTheGapsBetweenIdeographs) {
  Font font;
  // Twelve ideographs of 100: five fit in 530, leaving 30 to share over four gaps.
  Justified para(font, cjk(4), GFNT_WRITING_VERTICAL_RL, GFNT_JUSTIFY_AUTO);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(530), &para.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_TRUE(line.justified);
  ASSERT_EQ(line.runs[0].glyph_count, 5u);
  for (size_t g = 0; g < 4; ++g) {
    EXPECT_EQ(line.runs[0].glyphs[g].advance, px(100) + 480) << g << ": 7.5 pixels more";
    EXPECT_EQ(line.runs[0].glyphs[g].pen_y, static_cast<GFNT_F26Dot6>(g) * (px(100) + 480)) << g;
  }
  EXPECT_EQ(line.runs[0].glyphs[4].advance, px(100)) << "none after the last of the line";
  EXPECT_EQ(line.extent, px(530));
  EXPECT_TRUE(para.layout.lines[1].justified);
  EXPECT_FALSE(para.layout.lines[2].justified) << "and none at all on a paragraph's last line";
}

TEST(LayoutJustifyIdeographs, TheOtherModesPlaceTheSlackWhereTheirNamesSay) {
  Font font;
  {
    Justified words(font, cjk(4), GFNT_WRITING_VERTICAL_RL, GFNT_JUSTIFY_INTER_WORD);
    ASSERT_EQ(gfnt_paragraph_layout(words.p, px(530), &words.layout, nullptr), GFNT_OK);
    EXPECT_FALSE(words.layout.lines[0].justified) << "there is no space to widen";
  }
  {
    Justified none(font, "A B A B A B A B A B A B", GFNT_WRITING_HORIZONTAL_TB, GFNT_JUSTIFY_NONE);
    ASSERT_EQ(gfnt_paragraph_layout(none.p, px(300), &none.layout, nullptr), GFNT_OK);
    EXPECT_FALSE(none.layout.lines[0].justified);
  }
  {
    // Every gap, Latin letters included: 300 holds "ABAB" at 70+65+70+65 = 270, 30 over three gaps.
    Justified every(font, "ABAB ABAB ABAB", GFNT_WRITING_HORIZONTAL_TB,
        GFNT_JUSTIFY_INTER_CHARACTER);
    ASSERT_EQ(gfnt_paragraph_layout(every.p, px(300), &every.layout, nullptr), GFNT_OK);
    const GFNT_Line & line = every.layout.lines[0];
    EXPECT_TRUE(line.justified);
    ASSERT_GE(line.runs[0].glyph_count, 4u);
    EXPECT_EQ(line.runs[0].glyphs[0].advance, px(70) + px(10));
    EXPECT_EQ(line.runs[0].glyphs[2].advance, px(70) + px(10));
    EXPECT_EQ(line.runs[0].glyphs[3].advance, px(65)) << "the last of the line has none after it";
  }
}

TEST(LayoutJustifyIdeographs, AGapBeforeAnIdeographFollowingLatinGoesBeforeItsGlyph) {
  Font font;
  // A and B turned (70 and 65), then six ideographs: five fit in 700 with 65 to spare, shared over
  // the gap before the first and the four between them.
  Justified para(font, std::string("AB") + cjk(2), GFNT_WRITING_VERTICAL_RL, GFNT_JUSTIFY_AUTO);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(700), &para.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_TRUE(line.justified);
  ASSERT_EQ(line.run_count, 2u);
  const GFNT_LineRun & upright = line.runs[1];
  ASSERT_GE(upright.glyph_count, 5u);
  EXPECT_EQ(upright.glyphs[0].pen_y, px(135)) << "the pen is where the previous glyph ended";
  // The glyph's origin is below the pen by the font's vertical origin (880 of 1000 = 88), and
  // by the gap as well for the first, which has one before it.
  EXPECT_EQ(upright.glyphs[0].y - upright.glyphs[0].pen_y, px(13) + px(88))
      << "the glyph itself comes after the gap";
  EXPECT_EQ(upright.glyphs[1].y - upright.glyphs[1].pen_y, px(88));
  EXPECT_EQ(upright.glyphs[0].advance, px(13) + px(100) + px(13)) << "the gap and the one after";
  EXPECT_EQ(line.runs[0].glyphs[0].advance, px(70)) << "A and B take none between them";
  EXPECT_EQ(line.runs[0].glyphs[1].advance, px(65));
}

TEST(LayoutJustifyIdeographs, HangulIsNotSpreadAndNeitherAreTheDashAndEllipsis) {
  Font font;
  // U+D55C and U+2014 / U+2026 are ordinary letters or punctuation here: no gaps next to them.
  const std::string hangul = "\xED\x95\x9C";
  Justified para(font, "A" + hangul + "A" + hangul + "A" + hangul + "A" + hangul + "A" + hangul
      + "A" + hangul, GFNT_WRITING_HORIZONTAL_TB, GFNT_JUSTIFY_AUTO);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(500), &para.layout, nullptr), GFNT_OK);
  EXPECT_FALSE(para.layout.lines[0].justified) << "no spaces, and Hangul is no ideograph";
}

TEST(LayoutVertical, ManyColumnsThatEndInAHyphenEachKeepTheirOwnRunsAndGlyphs) {
  Font font;
  // Found by the layout fuzzer: with every unit on a line of its own and a hyphen ending most of
  // them, the runs outnumbered the units and spilled into the glyphs after them.
  const std::string text = std::string("\xEF\xBC\x81\xEF\xBC\x81") + "\xC2\xAD\xC2\xAD\xC2\xAD\xC2\xAD\xC2\xAD"
      + "\xD7\x91" + "\xEF\xBC\x81\xEF\xBC\x81";
  Para para(font, text, GFNT_WRITING_VERTICAL_RL);
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(px(1)), GFNT_OK);
  size_t glyphs = 0;
  for (size_t i = 0; i < para.layout.line_count; ++i) {
    const GFNT_Line & line = para.layout.lines[i];
    for (size_t k = 0; k < line.run_count; ++k) {
      const GFNT_LineRun & run = line.runs[k];
      for (size_t g = 0; g < run.glyph_count; ++g, ++glyphs) {
        EXPECT_LT(run.glyphs[g].glyph, 64u) << "line " << i << " run " << k;
        if (!run.hyphen) {
          EXPECT_GE(run.glyphs[g].cluster, line.start) << i;
          EXPECT_LE(run.glyphs[g].cluster, line.end) << i;
        }
      }
    }
  }
  EXPECT_GT(glyphs, 8u);
}

TEST(LayoutVertical, UprightTextAtAnOddLevelIsStoredVisuallyWithItsClustersToMatch) {
  Font font;
  // Found by the layout fuzzer: upright exclamation marks between Hebrew letters in a right-to-left
  // paragraph sit at level 1 and read bottom to top, so their glyphs are in reverse text order,
  // and each cluster must still be the one its glyph stands for. If the marks were taken for one
  // unit a line could not break between them.
  const std::string text = std::string("\xD7\x91") + "\xEF\xBC\x81\xEF\xBC\x81\xEF\xBC\x81"
      + "\xD7\x91";
  {
    Para para(font, text, GFNT_WRITING_VERTICAL_RL, GFNT_ORIENT_MIXED, GFNT_BIDI_RTL);
    ASSERT_EQ(para.lay(px(600)), GFNT_OK);
    const GFNT_Line & line = para.layout.lines[0];
    const GFNT_LineRun * upright = nullptr;
    for (size_t k = 0; k < line.run_count; ++k) {
      if (line.runs[k].orientation == GFNT_ORIENT_RUN_UPRIGHT && line.runs[k].glyph_count == 3) {
        upright = &line.runs[k];
      }
    }
    ASSERT_NE(upright, nullptr);
    EXPECT_EQ(upright->level, 1);
    EXPECT_EQ(upright->glyphs[0].cluster, 8u) << "the last mark is first from the top";
    EXPECT_EQ(upright->glyphs[1].cluster, 5u);
    EXPECT_EQ(upright->glyphs[2].cluster, 2u);
    EXPECT_LT(upright->glyphs[0].pen_y, upright->glyphs[1].pen_y);
  }
  {
    // The minimal input the fuzzer reduced it to: five ill-formed bytes, five U+FFFD, upright and
    // neutral, so at level 1 in a right-to-left paragraph, with a break allowed anywhere.
    const std::string bad = "\xFF\xFF\xFF\xFF\xFF";
    const GFNT_Face * faces[1] = {font.face};
    GFNT_TextSpan span{};
    span.length = bad.size();
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = kSize;
    GFNT_ParagraphOptions o{};
    o.writing_mode = GFNT_WRITING_VERTICAL_LR;
    o.text_orientation = GFNT_ORIENT_MIXED_PANGO;
    o.direction = GFNT_BIDI_RTL;
    o.align = GFNT_ALIGN_LEFT;
    o.justify = GFNT_JUSTIFY_NONE;
    o.hyphens = GFNT_HYPHENS_NONE;
    o.line_break = GFNT_LINE_BREAK_ANYWHERE;
    GFNT_Paragraph * p = nullptr;
    GFNT_Layout l{};
    ASSERT_EQ(gfnt_paragraph_create(bad.data(), bad.size(), &span, 1, &o, nullptr, &p, nullptr),
        GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(p, px(415), &l, nullptr), GFNT_OK);
    EXPECT_EQ(l.line_count, 2u) << "four to a column, as the room allows";
    for (size_t i = 0; i < l.line_count; ++i) {
      const GFNT_Line & line = l.lines[i];
      for (size_t k = 0; k < line.run_count; ++k) {
        for (size_t g = 0; g < line.runs[k].glyph_count; ++g) {
          EXPECT_GE(line.runs[k].glyphs[g].cluster, line.start) << i;
          EXPECT_LE(line.runs[k].glyphs[g].cluster, line.end) << i;
        }
      }
    }
    gfnt_layout_free(&l);
    gfnt_paragraph_free(p);
  }
}

/** One column laid out with the given hyphen handling, for the tests below. */
struct Hyphenated {
  GFNT_Paragraph * p = nullptr;
  GFNT_Layout layout{};
  const GFNT_Face * faces[1];

  Hyphenated(const Font & font, const std::string & text, GFNT_WritingMode mode,
      GFNT_Hyphens how, GFNT_TextOrientation orientation = GFNT_ORIENT_MIXED) {
    faces[0] = font.face;
    GFNT_TextSpan span{};
    span.length = text.size();
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = kSize;
    GFNT_ParagraphOptions o{};
    o.writing_mode = mode;
    o.text_orientation = orientation;
    o.hyphens = how;
    EXPECT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o, nullptr, &p, nullptr),
        GFNT_OK);
  }
  ~Hyphenated() {
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(p);
  }
  Hyphenated(const Hyphenated &) = delete;
  Hyphenated & operator=(const Hyphenated &) = delete;
};

TEST(LayoutVerticalHyphen, TheHyphenAtTheEndOfAColumnIsUprightInCssAndTurnedInPango) {
  Font font;
  // "AB" turned, 135 across, then a soft hyphen; the room holds one AB and the hyphen.
  const std::string text = "AB\xC2\xAD" "AB";
  Hyphenated css(font, text, GFNT_WRITING_VERTICAL_RL, GFNT_HYPHENS_MANUAL);
  ASSERT_EQ(gfnt_paragraph_layout(css.p, px(200), &css.layout, nullptr), GFNT_OK);
  ASSERT_EQ(css.layout.line_count, 2u);
  const GFNT_LineRun * hyphen = nullptr;
  for (size_t k = 0; k < css.layout.lines[0].run_count; ++k) {
    if (css.layout.lines[0].runs[k].hyphen) {
      hyphen = &css.layout.lines[0].runs[k];
    }
  }
  ASSERT_NE(hyphen, nullptr);
  EXPECT_EQ(hyphen->orientation, GFNT_ORIENT_RUN_UPRIGHT) << "on its vertical advance, as Chromium";
  EXPECT_EQ(hyphen->advance, px(60));
  Hyphenated pango(font, text, GFNT_WRITING_VERTICAL_RL, GFNT_HYPHENS_MANUAL_PANGO,
      GFNT_ORIENT_MIXED_PANGO);
  ASSERT_EQ(gfnt_paragraph_layout(pango.p, px(200), &pango.layout, nullptr), GFNT_OK);
  hyphen = nullptr;
  for (size_t k = 0; k < pango.layout.lines[0].run_count; ++k) {
    if (pango.layout.lines[0].runs[k].hyphen) {
      hyphen = &pango.layout.lines[0].runs[k];
    }
  }
  ASSERT_NE(hyphen, nullptr);
  EXPECT_EQ(hyphen->orientation, GFNT_ORIENT_RUN_CW) << "turned with the text, as Pango";
  EXPECT_EQ(hyphen->advance, px(33));
}

TEST(LayoutVerticalHyphen, ASoftHyphenAfterAnIdeographIsInvisibleAndBreaksOnlyInPango) {
  Font font;
  // 日本 SHY 語日 SHY 本語 with room for two ideographs and a bit.
  const std::string text = std::string(kJi) + kHon + "\xC2\xAD" + kGo + kJi + "\xC2\xAD" + kHon
      + kGo;
  Hyphenated css(font, text, GFNT_WRITING_VERTICAL_RL, GFNT_HYPHENS_MANUAL);
  ASSERT_EQ(gfnt_paragraph_layout(css.p, px(250), &css.layout, nullptr), GFNT_OK);
  // The break falls between ideographs as it would without the soft hyphens: 日本 語日 本語 does
  // not, 日 本語 日本 語 does (the soft hyphen after 本 is no place to break).
  EXPECT_EQ(css.layout.line_count, 4u);
  for (size_t i = 0; i < css.layout.line_count; ++i) {
    for (size_t k = 0; k < css.layout.lines[i].run_count; ++k) {
      EXPECT_FALSE(css.layout.lines[i].runs[k].hyphen) << "no hyphen after an ideograph";
    }
  }
  Hyphenated pango(font, text, GFNT_WRITING_VERTICAL_RL, GFNT_HYPHENS_MANUAL_PANGO);
  ASSERT_EQ(gfnt_paragraph_layout(pango.p, px(250), &pango.layout, nullptr), GFNT_OK);
  EXPECT_EQ(pango.layout.line_count, 3u) << "the soft hyphen is a place to break, with no hyphen";
  for (size_t i = 0; i < pango.layout.line_count; ++i) {
    for (size_t k = 0; k < pango.layout.lines[i].run_count; ++k) {
      EXPECT_FALSE(pango.layout.lines[i].runs[k].hyphen);
    }
  }
}

TEST(LayoutVertical, RubyAndEmphasisTracksUseTheColumnCrossAxis) {
  Font font;
  const GFNT_Face * faces[] = {font.face};
  const std::string text = std::string(kJi) + "AB" + kHon;
  GFNT_Annotation annotations[2]{};
  annotations[0].base_start = 0;
  annotations[0].base_end = text.size();
  annotations[0].kind = GFNT_ANNOTATION_RUBY;
  annotations[0].position = GFNT_ANNOTATION_OVER;
  annotations[0].style.faces = faces;
  annotations[0].style.face_count = 1;
  annotations[0].style.size = px(40);
  annotations[0].text = "R";
  annotations[0].text_length = 1;
  annotations[1] = annotations[0];
  annotations[1].kind = GFNT_ANNOTATION_EMPHASIS;
  annotations[1].position = GFNT_ANNOTATION_UNDER;
  annotations[1].text = nullptr;
  annotations[1].text_length = 0;
  annotations[1].mark_kind = GFNT_EMPHASIS_MARK_DOT;
  GFNT_TextSpan span{};
  span.length = text.size();
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;

  for (GFNT_WritingMode mode : {GFNT_WRITING_VERTICAL_RL, GFNT_WRITING_VERTICAL_LR,
      GFNT_WRITING_SIDEWAYS_RL, GFNT_WRITING_SIDEWAYS_LR}) {
    GFNT_ParagraphOptions options{};
    options.writing_mode = mode;
    options.text_orientation = GFNT_ORIENT_MIXED;
    options.annotations = annotations;
    options.annotation_count = 2;
    GFNT_Paragraph * paragraph = nullptr;
    GFNT_Layout layout{};
    ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, &options, nullptr,
        &paragraph, nullptr), GFNT_OK) << mode;
    ASSERT_EQ(gfnt_paragraph_layout(paragraph, GFNT_LAYOUT_UNBOUNDED, &layout, nullptr), GFNT_OK)
        << mode;
    ASSERT_EQ(layout.line_count, 1u) << mode;
    const GFNT_Line & line = layout.lines[0];
    ASSERT_EQ(line.annotation_count, 5u) << mode;
    EXPECT_GT(line.box.width, px(100)) << mode;
    EXPECT_EQ(line.annotations[0].kind, GFNT_ANNOTATION_RUBY) << mode;
    EXPECT_GT(line.annotations[0].box.height, 0) << mode;
    for (size_t i = 1; i < line.annotation_count; ++i) {
      EXPECT_EQ(line.annotations[i].kind, GFNT_ANNOTATION_EMPHASIS) << mode;
      EXPECT_GT(line.annotations[i].box.height, 0) << mode;
    }
    GFNT_Hit hit{};
    const GFNT_Rect ruby_box = line.annotations[0].box;
    ASSERT_EQ(gfnt_layout_hit_test(paragraph, &layout,
        ruby_box.x + ruby_box.width / 2, ruby_box.y + ruby_box.height / 2,
        &hit), GFNT_OK) << mode;
    EXPECT_LE(hit.offset, text.size()) << mode;
    GFNT_Rect selection{};
    size_t selection_count = 0;
    ASSERT_EQ(gfnt_layout_range_rects(paragraph, &layout, 0, text.size(),
        &selection, 1, &selection_count), GFNT_OK) << mode;
    ASSERT_EQ(selection_count, 1u) << mode;
    const bool disjoint = selection.x + selection.width <= ruby_box.x
        || ruby_box.x + ruby_box.width <= selection.x
        || selection.y + selection.height <= ruby_box.y
        || ruby_box.y + ruby_box.height <= selection.y;
    EXPECT_TRUE(disjoint) << "base selection should exclude ruby track in mode " << mode;
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(paragraph);
  }
}

TEST(LayoutVertical, InterCharacterRubyUsesTheOuterSideOfTheColumn) {
  Font font;
  const GFNT_Face * faces[] = {font.face};
  const std::string text = kJi;
  GFNT_Annotation ruby{};
  ruby.base_start = 0;
  ruby.base_end = text.size();
  ruby.kind = GFNT_ANNOTATION_RUBY;
  ruby.position = GFNT_ANNOTATION_INTER_CHARACTER;
  ruby.style.faces = faces;
  ruby.style.face_count = 1;
  ruby.style.size = px(40);
  ruby.text = "に";
  ruby.text_length = std::strlen(ruby.text);
  GFNT_TextSpan span{};
  span.length = text.size();
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;
  for (GFNT_WritingMode mode : {GFNT_WRITING_VERTICAL_RL, GFNT_WRITING_VERTICAL_LR,
      GFNT_WRITING_SIDEWAYS_RL, GFNT_WRITING_SIDEWAYS_LR}) {
    GFNT_ParagraphOptions options{};
    options.writing_mode = mode;
    options.text_orientation = GFNT_ORIENT_MIXED;
    options.annotations = &ruby;
    options.annotation_count = 1;
    GFNT_Paragraph * paragraph = nullptr;
    GFNT_Layout layout{};
    ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, &options, nullptr,
        &paragraph, nullptr), GFNT_OK) << mode;
    ASSERT_EQ(gfnt_paragraph_layout(paragraph, GFNT_LAYOUT_UNBOUNDED, &layout, nullptr), GFNT_OK)
        << mode;
    ASSERT_EQ(layout.line_count, 1u) << mode;
    ASSERT_EQ(layout.lines[0].annotation_count, 1u) << mode;
    const GFNT_Line & line = layout.lines[0];
    EXPECT_GE(line.annotations[0].box.x, line.box.x + line.box.width) << mode;
    EXPECT_GT(line.annotations[0].box.width, 0) << mode;
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(paragraph);
  }
}

TEST(LayoutVertical, ABadModeIsRefused) {
  Font font;
  const GFNT_Face * faces[1] = {font.face};
  GFNT_TextSpan span{};
  span.length = 1;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;
  GFNT_ParagraphOptions o{};
  GFNT_Paragraph * p = nullptr;
  o.writing_mode = static_cast<GFNT_WritingMode>(9);
  EXPECT_EQ(gfnt_paragraph_create("A", 1, &span, 1, &o, nullptr, &p, nullptr), GFNT_ERR_INVALID);
  o.writing_mode = GFNT_WRITING_VERTICAL_RL;
  o.text_orientation = static_cast<GFNT_TextOrientation>(9);
  EXPECT_EQ(gfnt_paragraph_create("A", 1, &span, 1, &o, nullptr, &p, nullptr), GFNT_ERR_INVALID);
}

TEST(LayoutVertical, EveryAllocationFailureIsSurvived) {
  Font font;
  const std::string text = std::string(kJi) + "AB\xE3\x80\x81" + kHon + "\xE2\x80\xA6" + kGo;
  for (GFNT_WritingMode mode : {GFNT_WRITING_VERTICAL_RL, GFNT_WRITING_SIDEWAYS_LR}) {
    const GFNT_Face * faces[1] = {font.face};
    GFNT_TextSpan span{};
    span.length = text.size();
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = kSize;
    GFNT_ParagraphOptions o{};
    o.writing_mode = mode;
    o.max_lines = 1;
    o.ellipsis = "A";
    size_t requests = 0;
    {
      gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
      GFNT_Paragraph * p = nullptr;
      GFNT_Layout l{};
      ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o, counting.get(), &p,
                    nullptr), GFNT_OK);
      ASSERT_EQ(gfnt_paragraph_layout(p, px(250), &l, nullptr), GFNT_OK);
      requests = counting.requests();
      gfnt_layout_free(&l);
      gfnt_paragraph_free(p);
      EXPECT_EQ(counting.live(), 0u);
    }
    ASSERT_GT(requests, 10u);
    for (size_t fail = 0; fail < requests; ++fail) {
      gfnttest::FailingAllocator failing(fail);
      GFNT_Paragraph * p = nullptr;
      GFNT_Layout l{};
      GFNT_Result r = gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o, failing.get(),
          &p, nullptr);
      if (r == GFNT_OK) {
        r = gfnt_paragraph_layout(p, px(250), &l, nullptr);
      }
      if (r != GFNT_OK) {
        EXPECT_EQ(r, GFNT_ERR_OOM) << "request " << fail;
      }
      gfnt_layout_free(&l);
      gfnt_paragraph_free(p);
      EXPECT_EQ(failing.live(), 0u) << "request " << fail << " leaked";
    }
  }
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
