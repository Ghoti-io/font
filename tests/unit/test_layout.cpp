/**
 * @file
 *
 * Paragraph layout on a font made for the purpose: every letter 500 units wide,
 * the space 250, ascent 800, descent 200, so each expectation is arithmetic that
 * can be done by hand. At 10 pixels (640 in 26.6) a letter is 320 wide, a space
 * 160, the ascent 512 and the descent 128.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"
#include "failing_allocator.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <ghoti.io/font/layout.h>

namespace {

constexpr GFNT_F26Dot6 kSize = 10 * 64;
constexpr GFNT_F26Dot6 kLetter = 320;
constexpr GFNT_F26Dot6 kSpace = 160;

/** A font whose glyph n (from 1) is the n-th code point of `chars`. */
struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  explicit Font(const std::vector<uint32_t> & chars) {
    std::vector<gfnttest::Segment4> segments;
    std::vector<std::pair<uint32_t, uint16_t>> order;
    for (size_t i = 0; i < chars.size(); ++i) {
      order.push_back({chars[i], static_cast<uint16_t>(i + 1)});
    }
    std::sort(order.begin(), order.end());
    for (const auto & o : order) {
      segments.push_back({static_cast<uint16_t>(o.first),
          static_cast<uint16_t>(o.first),
          static_cast<int16_t>(o.second - o.first), {}});
    }
    segments.push_back({0xFFFF, 0xFFFF, 1, {}});
    const uint16_t glyphs = static_cast<uint16_t>(chars.size() + 1);
    std::vector<std::pair<uint16_t, int16_t>> metrics(glyphs, {500, 0});
    for (size_t i = 0; i < chars.size(); ++i) {
      if (chars[i] == ' ') {
        metrics[i + 1].first = 250;
      }
    }
    bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {
        {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 0)},
        {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, glyphs)},
        {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics)},
        {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(glyphs)},
        {GFNT_TAG('c', 'm', 'a', 'p'),
            gfnttest::build_cmap({{3, 1, gfnttest::build_cmap_format4(segments)}})},
    });
    GFNT_Error error;
    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                  GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, &error), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
  }
  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;
  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
};

std::vector<uint32_t> latin() {
  return {'A', 'B', 'C', 'D', 'E', 'F', ' '};
}

std::vector<uint32_t> latin_and_hebrew() {
  return {'A', 'B', 'C', 'D', 'E', 'F', ' ', 0x5D0, 0x5D1, 0x5D2};
}

struct Para {
  GFNT_Paragraph * p = nullptr;
  GFNT_Layout layout{};
  const GFNT_Face * faces[2] = {nullptr, nullptr};
  std::vector<GFNT_TextSpan> spans;
  GFNT_Result created = GFNT_ERR_INTERNAL;

  Para(const std::string & text, std::vector<const GFNT_Face *> list,
      const GFNT_ParagraphOptions & options = {}, GFNT_F26Dot6 size = kSize) {
    GFNT_TextSpan span{};
    span.length = text.size();
    for (size_t i = 0; i < list.size() && i < 2; ++i) {
      faces[i] = list[i];
    }
    span.style.faces = faces;
    span.style.face_count = list.size();
    span.style.size = size;
    spans.push_back(span);
    created = gfnt_paragraph_create(text.data(), text.size(), spans.data(),
        spans.size(), &options, nullptr, &p, nullptr);
  }
  Para(const std::string & text, std::vector<GFNT_TextSpan> given,
      const GFNT_ParagraphOptions & options = {})
      : spans(std::move(given)) {
    created = gfnt_paragraph_create(text.data(), text.size(), spans.data(),
        spans.size(), &options, nullptr, &p, nullptr);
  }
  GFNT_Result lay(GFNT_F26Dot6 width) {
    gfnt_layout_free(&layout);
    return gfnt_paragraph_layout(p, width, &layout, nullptr);
  }
  ~Para() {
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(p);
  }
  Para(const Para &) = delete;
  Para & operator=(const Para &) = delete;
};

TEST(Layout, ALineOfLettersIsPlacedFromTheLeftOnTheBaseline) {
  Font font(latin());
  Para para("ABC", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  const GFNT_Layout & l = para.layout;
  ASSERT_EQ(l.line_count, 1u);
  const GFNT_Line & line = l.lines[0];
  EXPECT_EQ(line.width, 3 * kLetter);
  EXPECT_EQ(line.ascent, 512);
  EXPECT_EQ(line.descent, 128);
  EXPECT_EQ(line.baseline, 512);
  EXPECT_EQ(line.height, 640);
  EXPECT_EQ(l.height, 640);
  EXPECT_EQ(l.width, 3 * kLetter);
  EXPECT_EQ(line.start, 0u);
  EXPECT_EQ(line.end, 3u);
  ASSERT_EQ(line.run_count, 1u);
  const GFNT_LineRun & run = line.runs[0];
  EXPECT_EQ(run.face, font.face);
  EXPECT_FALSE(run.fallback);
  ASSERT_EQ(run.glyph_count, 3u);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(run.glyphs[i].glyph, i + 1);
    EXPECT_EQ(run.glyphs[i].cluster, i);
    EXPECT_EQ(run.glyphs[i].pen_x, static_cast<GFNT_F26Dot6>(i) * kLetter);
    EXPECT_EQ(run.glyphs[i].y, 512);
  }
}

TEST(Layout, LinesBreakAfterSpacesAndTheSpacesHang) {
  Font font(latin());
  Para para("AB CD EF", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  // Letters 320, space 160: "AB " is 800 with the space, 640 without.
  ASSERT_EQ(para.lay(900), GFNT_OK);
  const GFNT_Layout & l = para.layout;
  ASSERT_EQ(l.line_count, 3u);
  const size_t starts[] = {0, 3, 6}, ends[] = {3, 6, 8};
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(l.lines[i].start, starts[i]) << i;
    EXPECT_EQ(l.lines[i].end, ends[i]) << i;
    EXPECT_EQ(l.lines[i].width, 2 * kLetter) << i;
    EXPECT_EQ(l.lines[i].top, static_cast<GFNT_F26Dot6>(i) * 640) << i;
  }
  EXPECT_EQ(l.height, 3 * 640);
  EXPECT_EQ(l.width, 900);
}

TEST(Layout, AWordWiderThanTheLineOverflowsRatherThanSplits) {
  Font font(latin());
  Para para("ABCDEF", {font.face});
  ASSERT_EQ(para.lay(500), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_EQ(para.layout.lines[0].width, 6 * kLetter);
}

TEST(Layout, AWordTooWideIsBrokenAtTheNextOpportunity) {
  Font font(latin());
  Para para("ABCDEF AB", {font.face});
  ASSERT_EQ(para.lay(500), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[0].end, 7u);
  EXPECT_EQ(para.layout.lines[0].width, 6 * kLetter);
  EXPECT_EQ(para.layout.lines[1].width, 2 * kLetter);
}

TEST(Layout, ANewlineEndsALineAndATrailingOneStartsAnEmptyLine) {
  Font font(latin());
  Para para("AB\nC\n", {font.face});
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  const GFNT_Layout & l = para.layout;
  ASSERT_EQ(l.line_count, 3u);
  EXPECT_EQ(l.lines[0].end, 3u);
  EXPECT_EQ(l.lines[0].width, 2 * kLetter);
  EXPECT_EQ(l.lines[1].start, 3u);
  EXPECT_EQ(l.lines[1].width, kLetter);
  EXPECT_EQ(l.lines[2].start, 5u);
  EXPECT_EQ(l.lines[2].run_count, 0u);
  EXPECT_EQ(l.lines[2].height, 640);
  EXPECT_EQ(l.height, 3 * 640);
}

TEST(Layout, AnEmptyParagraphIsOneLineOfTheFirstFacesHeight) {
  Font font(latin());
  Para para("", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_EQ(para.layout.lines[0].run_count, 0u);
  EXPECT_EQ(para.layout.lines[0].height, 640);
}

TEST(Layout, ALineIsAlignedInTheWidthItWasLaidOutTo) {
  Font font(latin());
  struct Case { GFNT_Align align; GFNT_F26Dot6 x; };
  for (const Case & c : {Case{GFNT_ALIGN_START, 0}, Case{GFNT_ALIGN_LEFT, 0},
           Case{GFNT_ALIGN_CENTER, 180}, Case{GFNT_ALIGN_RIGHT, 360},
           Case{GFNT_ALIGN_END, 360}}) {
    GFNT_ParagraphOptions o{};
    o.align = c.align;
    Para para("AB", {font.face}, o);
    ASSERT_EQ(para.lay(1000), GFNT_OK);
    const GFNT_Line & line = para.layout.lines[0];
    EXPECT_EQ(line.x, c.x) << c.align;
    EXPECT_EQ(line.runs[0].x, c.x) << c.align;
    EXPECT_EQ(line.runs[0].glyphs[0].pen_x, c.x) << c.align;
    EXPECT_EQ(line.runs[0].glyphs[1].pen_x, c.x + kLetter) << c.align;
  }
}

TEST(Layout, ARightToLeftParagraphIsInVisualOrderAndStartsOnTheRight) {
  Font font(latin_and_hebrew());
  GFNT_ParagraphOptions o{};
  o.direction = GFNT_BIDI_AUTO;
  Para para("\xD7\x90\xD7\x91", {font.face}, o);   // alef bet
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(1000), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_EQ(line.run_count, 1u);
  const GFNT_LineRun & run = line.runs[0];
  EXPECT_EQ(run.level, 1);
  ASSERT_EQ(run.glyph_count, 2u);
  // Visual order is bet, alef: the later letter is on the left.
  EXPECT_EQ(run.glyphs[0].cluster, 2u);
  EXPECT_EQ(run.glyphs[1].cluster, 0u);
  // START in a right-to-left paragraph is the right edge.
  EXPECT_EQ(line.x, 1000 - 2 * kLetter);
  EXPECT_EQ(run.glyphs[0].pen_x, 1000 - 2 * kLetter);
}

TEST(Layout, MixedTextIsReorderedByLevelRuns) {
  Font font(latin_and_hebrew());
  GFNT_ParagraphOptions o{};
  o.direction = GFNT_BIDI_LTR;
  // A, space, alef, bet, space, B: the Hebrew pair reads right to left inside.
  Para para("A \xD7\x90\xD7\x91 B", {font.face}, o);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_GE(line.run_count, 3u);
  size_t rtl = 0;
  for (size_t i = 0; i < line.run_count; ++i) {
    if (line.runs[i].level & 1) {
      ++rtl;
      EXPECT_EQ(line.runs[i].glyph_count, 2u);
      EXPECT_EQ(line.runs[i].glyphs[0].cluster, 4u);   // bet comes first
    }
  }
  EXPECT_EQ(rtl, 1u);
  // Runs tile the line without a gap.
  GFNT_F26Dot6 at = 0;
  for (size_t i = 0; i < line.run_count; ++i) {
    EXPECT_EQ(line.runs[i].x, at) << i;
    at += line.runs[i].advance;
  }
  EXPECT_EQ(at, 4 * kLetter + 2 * kSpace);
}

TEST(Layout, ALineTakesTheTallestSpansMetrics) {
  Font font(latin());
  const GFNT_Face * faces[1] = {font.face};
  GFNT_TextSpan a{}, b{};
  a.length = 2;
  a.style.faces = faces;
  a.style.face_count = 1;
  a.style.size = 10 * 64;
  b = a;
  b.length = 2;
  b.style.size = 20 * 64;
  Para para("ABCD", {a, b});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_EQ(line.ascent, 1024);
  EXPECT_EQ(line.descent, 256);
  EXPECT_EQ(line.width, 2 * 320 + 2 * 640);
  ASSERT_EQ(line.run_count, 2u);
  EXPECT_EQ(line.runs[0].span, 0u);
  EXPECT_EQ(line.runs[1].span, 1u);
  EXPECT_EQ(line.runs[1].glyphs[0].advance, 640);
  EXPECT_EQ(line.runs[1].glyphs[0].y, line.baseline);
}

TEST(Layout, ACharacterTheFirstFaceLacksComesFromTheNextAndIsFlagged) {
  Font first({'A'});
  Font second({'B'});
  Para para("AB", std::vector<const GFNT_Face *>{first.face, second.face});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_EQ(line.run_count, 2u);
  EXPECT_EQ(line.runs[0].face, first.face);
  EXPECT_FALSE(line.runs[0].fallback);
  EXPECT_EQ(line.runs[1].face, second.face);
  EXPECT_TRUE(line.runs[1].fallback);
  EXPECT_EQ(line.runs[1].face_index, 1u);
  EXPECT_EQ(line.runs[1].x, kLetter);
  EXPECT_EQ(line.runs[1].start, 1u);
  EXPECT_EQ(line.runs[1].end, 2u);
}

TEST(Layout, ALayoutAtAnotherWidthNeedsNoNewParagraph) {
  Font font(latin());
  Para para("AB CD", {font.face});
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  EXPECT_EQ(para.layout.line_count, 1u);
  ASSERT_EQ(para.lay(700), GFNT_OK);
  EXPECT_EQ(para.layout.line_count, 2u);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  EXPECT_EQ(para.layout.line_count, 1u);
}

TEST(Layout, BadArgumentsAreRefused) {
  Font font(latin());
  const GFNT_Face * faces[1] = {font.face};
  GFNT_TextSpan span{};
  span.length = 2;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;
  GFNT_Paragraph * p = nullptr;
  // The span does not cover the text.
  EXPECT_EQ(gfnt_paragraph_create("ABC", 3, &span, 1, nullptr, nullptr, &p, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(p, nullptr);
  span.length = 3;
  span.style.size = 0;
  EXPECT_EQ(gfnt_paragraph_create("ABC", 3, &span, 1, nullptr, nullptr, &p, nullptr),
      GFNT_ERR_INVALID);
  span.style.size = kSize;
  span.style.faces = nullptr;
  EXPECT_EQ(gfnt_paragraph_create("ABC", 3, &span, 1, nullptr, nullptr, &p, nullptr),
      GFNT_ERR_INVALID);
  span.style.faces = faces;
  EXPECT_EQ(gfnt_paragraph_create("ABC", 3, nullptr, 0, nullptr, nullptr, &p, nullptr),
      GFNT_ERR_INVALID);
  // A span boundary inside a two-byte character.
  GFNT_TextSpan two[2] = {span, span};
  two[0].length = 1;
  two[1].length = 1;
  EXPECT_EQ(gfnt_paragraph_create("\xD7\x90", 2, two, 2, nullptr, nullptr, &p,
                nullptr), GFNT_ERR_INVALID);
  GFNT_ParagraphOptions bad{};
  bad.align = static_cast<GFNT_Align>(99);
  span.length = 3;
  EXPECT_EQ(gfnt_paragraph_create("ABC", 3, &span, 1, &bad, nullptr, &p, nullptr),
      GFNT_ERR_INVALID);
  GFNT_Layout layout{};
  EXPECT_EQ(gfnt_paragraph_layout(nullptr, 100, &layout, nullptr), GFNT_ERR_INVALID);
  gfnt_layout_free(&layout);
  gfnt_paragraph_free(nullptr);
}

TEST(Layout, AnIllFormedByteIsOneReplacementCharacter) {
  Font font(latin());
  Para para("A\xFF" "B", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_EQ(para.layout.lines[0].runs[0].glyph_count, 3u);
  EXPECT_EQ(para.layout.lines[0].end, 3u);
}

TEST(Layout, EveryAllocationFailureIsReportedAndNothingLeaks) {
  Font font(latin_and_hebrew());
  const GFNT_Face * faces[1] = {font.face};
  GFNT_TextSpan span{};
  const std::string text = "AB CD \xD7\x90\xD7\x91 EF\nAB";
  span.length = text.size();
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;
  size_t requests = 0;
  {
    gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
    GFNT_Paragraph * p = nullptr;
    GFNT_Layout l{};
    ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, nullptr,
                  counting.get(), &p, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(p, 700, &l, nullptr), GFNT_OK);
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
    GFNT_Result r = gfnt_paragraph_create(text.data(), text.size(), &span, 1,
        nullptr, failing.get(), &p, nullptr);
    if (r == GFNT_OK) {
      r = gfnt_paragraph_layout(p, 700, &l, nullptr);
    }
    EXPECT_TRUE(r == GFNT_OK || r == GFNT_ERR_OOM) << "request " << fail << ": " << r;
    gfnt_layout_free(&l);
    gfnt_paragraph_free(p);
    EXPECT_EQ(failing.live(), 0u) << "request " << fail << " leaked";
  }
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
