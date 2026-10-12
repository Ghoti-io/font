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
#include <fstream>
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

  explicit Font(const std::vector<uint32_t> & chars, bool outlines = false) {
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
    std::vector<gfnttest::Table> extra;
    uint16_t loca_format = 0;
    if (outlines) {
      std::vector<std::vector<uint8_t>> shapes;
      shapes.push_back({});
      for (uint32_t c : chars) {
        shapes.push_back(c == ' ' ? std::vector<uint8_t>{}
            : gfnttest::build_glyf_glyph({{{50, 0, true}, {50, 700, true},
                                           {450, 700, true}, {450, 0, true}}}));
      }
      std::vector<uint8_t> glyf, loca;
      bool long_form = false;
      gfnttest::build_glyf_and_loca(shapes, &glyf, &loca, &long_form);
      loca_format = long_form ? 1 : 0;
      extra.push_back({GFNT_TAG('g', 'l', 'y', 'f'), glyf});
      extra.push_back({GFNT_TAG('l', 'o', 'c', 'a'), loca});
    }
    std::vector<gfnttest::Table> tables = {
        {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, loca_format)},
        {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, glyphs)},
        {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics)},
        {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(glyphs)},
        {GFNT_TAG('c', 'm', 'a', 'p'),
            gfnttest::build_cmap({{3, 1, gfnttest::build_cmap_format4(segments)}})},
    };
    tables.insert(tables.end(), extra.begin(), extra.end());
    bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
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
  EXPECT_EQ(line.extent, 3 * kLetter);
  EXPECT_EQ(line.ascent, 512);
  EXPECT_EQ(line.descent, 128);
  EXPECT_EQ(line.baseline, 512);
  EXPECT_EQ(line.box.height, 640);
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
    EXPECT_EQ(l.lines[i].extent, 2 * kLetter) << i;
    EXPECT_EQ(l.lines[i].box.y, static_cast<GFNT_F26Dot6>(i) * 640) << i;
  }
  EXPECT_EQ(l.height, 3 * 640);
  EXPECT_EQ(l.width, 900);
}

TEST(Layout, AWordWiderThanTheLineOverflowsRatherThanSplits) {
  Font font(latin());
  Para para("ABCDEF", {font.face});
  ASSERT_EQ(para.lay(500), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_EQ(para.layout.lines[0].extent, 6 * kLetter);
}

TEST(Layout, AWordTooWideIsBrokenAtTheNextOpportunity) {
  Font font(latin());
  Para para("ABCDEF AB", {font.face});
  ASSERT_EQ(para.lay(500), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[0].end, 7u);
  EXPECT_EQ(para.layout.lines[0].extent, 6 * kLetter);
  EXPECT_EQ(para.layout.lines[1].extent, 2 * kLetter);
}

TEST(Layout, ANewlineEndsALineAndATrailingOneStartsAnEmptyLine) {
  Font font(latin());
  Para para("AB\nC\n", {font.face});
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  const GFNT_Layout & l = para.layout;
  ASSERT_EQ(l.line_count, 3u);
  EXPECT_EQ(l.lines[0].end, 3u);
  EXPECT_EQ(l.lines[0].extent, 2 * kLetter);
  EXPECT_EQ(l.lines[1].start, 3u);
  EXPECT_EQ(l.lines[1].extent, kLetter);
  EXPECT_EQ(l.lines[2].start, 5u);
  EXPECT_EQ(l.lines[2].run_count, 0u);
  EXPECT_EQ(l.lines[2].box.height, 640);
  EXPECT_EQ(l.height, 3 * 640);
}

TEST(Layout, AnEmptyParagraphIsOneLineOfTheFirstFacesHeight) {
  Font font(latin());
  Para para("", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_EQ(para.layout.lines[0].run_count, 0u);
  EXPECT_EQ(para.layout.lines[0].box.height, 640);
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
    EXPECT_EQ(line.offset, c.x) << c.align;
    EXPECT_EQ(line.runs[0].pos, c.x) << c.align;
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
  EXPECT_EQ(line.offset, 1000 - 2 * kLetter);
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
    EXPECT_EQ(line.runs[i].pos, at) << i;
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
  EXPECT_EQ(line.extent, 2 * 320 + 2 * 640);
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
  EXPECT_EQ(line.runs[1].pos, kLetter);
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

TEST(LayoutContent, MinIsTheWidestUnbreakableStretchAndMaxIsTheWidestLine) {
  Font font(latin());
  Para para("AB CDE F", {font.face});
  GFNT_F26Dot6 min = 0, max = 0;
  ASSERT_EQ(gfnt_paragraph_min_content(para.p, &min), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(para.p, &max), GFNT_OK);
  EXPECT_EQ(min, 3 * kLetter);
  EXPECT_EQ(max, 6 * kLetter + 2 * kSpace);
  Para lines("AB\nCDE", {font.face});
  ASSERT_EQ(gfnt_paragraph_max_content(lines.p, &max), GFNT_OK);
  EXPECT_EQ(max, 3 * kLetter);
  EXPECT_EQ(gfnt_paragraph_min_content(nullptr, &min), GFNT_ERR_INVALID);
}

TEST(LayoutContent, NoLineIsWiderThanTheMaxOrNarrowerThanItsMinimumAllows) {
  Font font(latin());
  Para para("AB CDE F ABCDEF AB C\nDE FABC D", {font.face});
  GFNT_F26Dot6 min = 0, max = 0;
  ASSERT_EQ(gfnt_paragraph_min_content(para.p, &min), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(para.p, &max), GFNT_OK);
  for (GFNT_F26Dot6 width = 0; width <= max + 200; width += 37) {
    ASSERT_EQ(para.lay(width), GFNT_OK);
    for (size_t i = 0; i < para.layout.line_count; ++i) {
      const GFNT_F26Dot6 w = para.layout.lines[i].extent;
      EXPECT_LE(w, std::max(width, min)) << "width " << width << " line " << i;
      EXPECT_LE(w, max) << "width " << width;
    }
  }
  ASSERT_EQ(para.lay(min), GFNT_OK);
  for (size_t i = 0; i < para.layout.line_count; ++i) {
    EXPECT_LE(para.layout.lines[i].extent, min);
  }
  ASSERT_EQ(para.lay(max), GFNT_OK);
  EXPECT_EQ(para.layout.line_count, 2u);
}

TEST(LayoutHit, APointIsTheNearerEdgeOfTheCharacterUnderIt) {
  Font font(latin());
  Para para("ABC", {font.face});
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  GFNT_Hit h{};
  struct Case { GFNT_F26Dot6 x; size_t offset; GFNT_Affinity affinity; };
  for (const Case & c : {Case{-50, 0, GFNT_AFFINITY_DOWNSTREAM},
           Case{0, 0, GFNT_AFFINITY_DOWNSTREAM},
           Case{100, 0, GFNT_AFFINITY_DOWNSTREAM},
           Case{200, 1, GFNT_AFFINITY_UPSTREAM},
           Case{330, 1, GFNT_AFFINITY_DOWNSTREAM},
           Case{630, 2, GFNT_AFFINITY_UPSTREAM},
           Case{900, 3, GFNT_AFFINITY_UPSTREAM},
           Case{10000, 3, GFNT_AFFINITY_UPSTREAM}}) {
    ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, c.x, 10, &h), GFNT_OK);
    EXPECT_EQ(h.offset, c.offset) << c.x;
    EXPECT_EQ(h.affinity, c.affinity) << c.x;
    EXPECT_EQ(h.line, 0u);
  }
  // Above and below the lines is the first and the last.
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, 0, -500, &h), GFNT_OK);
  EXPECT_EQ(h.line, 0u);
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, 0, 99999, &h), GFNT_OK);
  EXPECT_EQ(h.line, 0u);
}

TEST(LayoutHit, TheLineIsTheOneUnderThePoint) {
  Font font(latin());
  Para para("AB CD EF", {font.face});
  ASSERT_EQ(para.lay(900), GFNT_OK);
  GFNT_Hit h{};
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, 10, 640 + 10, &h), GFNT_OK);
  EXPECT_EQ(h.line, 1u);
  EXPECT_EQ(h.offset, 3u);
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, 10, 99999, &h), GFNT_OK);
  EXPECT_EQ(h.line, 2u);
  EXPECT_EQ(h.offset, 6u);
}

TEST(LayoutHit, NothingLandsAfterANewline) {
  Font font(latin());
  Para para("AB\nC", {font.face});
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  GFNT_Hit h{};
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, 5000, 10, &h), GFNT_OK);
  EXPECT_EQ(h.offset, 2u);
  EXPECT_EQ(h.line, 0u);
  GFNT_Caret c{};
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 3, GFNT_AFFINITY_DOWNSTREAM, &c),
      GFNT_OK);
  EXPECT_EQ(c.line, 1u);
  EXPECT_EQ(c.x, 0);
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 2, GFNT_AFFINITY_DOWNSTREAM, &c),
      GFNT_OK);
  EXPECT_EQ(c.line, 0u);
  EXPECT_EQ(c.x, 2 * kLetter);
}

TEST(LayoutCaret, ASoftBreakHasTwoPlacesAndTheAffinityChoosesOne) {
  Font font(latin());
  Para para("AB CD", {font.face});
  ASSERT_EQ(para.lay(700), GFNT_OK);
  GFNT_Caret c{};
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 3, GFNT_AFFINITY_DOWNSTREAM, &c),
      GFNT_OK);
  EXPECT_EQ(c.line, 1u);
  EXPECT_EQ(c.x, 0);
  EXPECT_EQ(c.y, 640);
  EXPECT_EQ(c.length, 640);
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 3, GFNT_AFFINITY_UPSTREAM, &c),
      GFNT_OK);
  EXPECT_EQ(c.line, 0u);
  EXPECT_EQ(c.x, 2 * kLetter + kSpace);
  // The end of the text, and past it.
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 5, GFNT_AFFINITY_DOWNSTREAM, &c),
      GFNT_OK);
  EXPECT_EQ(c.line, 1u);
  EXPECT_EQ(c.x, 2 * kLetter);
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 99, GFNT_AFFINITY_UPSTREAM, &c),
      GFNT_OK);
  EXPECT_EQ(c.x, 2 * kLetter);
  EXPECT_EQ(gfnt_layout_caret(para.p, &para.layout, 0, GFNT_Affinity(7), &c),
      GFNT_ERR_INVALID);
}

TEST(LayoutCaret, AnEmptyParagraphHasACaretAtItsAlignedStart) {
  Font font(latin());
  GFNT_ParagraphOptions o{};
  o.align = GFNT_ALIGN_RIGHT;
  Para para("", {font.face}, o);
  ASSERT_EQ(para.lay(1000), GFNT_OK);
  GFNT_Caret c{};
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 0, GFNT_AFFINITY_DOWNSTREAM, &c),
      GFNT_OK);
  EXPECT_EQ(c.line, 0u);
  EXPECT_EQ(c.length, 640);
}

TEST(LayoutHit, RightToLeftTextIsHitFromTheRight) {
  Font font(latin_and_hebrew());
  GFNT_ParagraphOptions o{};
  o.direction = GFNT_BIDI_RTL;
  Para para("\xD7\x90\xD7\x91", {font.face}, o);   // alef bet
  ASSERT_EQ(para.lay(1000), GFNT_OK);
  // bet occupies [360, 680) and alef [680, 1000).
  GFNT_Hit h{};
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, 990, 5, &h), GFNT_OK);
  EXPECT_EQ(h.offset, 0u);
  EXPECT_EQ(h.affinity, GFNT_AFFINITY_DOWNSTREAM);
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, 700, 5, &h), GFNT_OK);
  EXPECT_EQ(h.offset, 2u);
  EXPECT_EQ(h.affinity, GFNT_AFFINITY_UPSTREAM);
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, 370, 5, &h), GFNT_OK);
  EXPECT_EQ(h.offset, 4u);
  EXPECT_EQ(h.affinity, GFNT_AFFINITY_UPSTREAM);
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, 5, 5, &h), GFNT_OK);
  EXPECT_EQ(h.offset, 4u);
  GFNT_Caret c{};
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 0, GFNT_AFFINITY_DOWNSTREAM, &c),
      GFNT_OK);
  EXPECT_EQ(c.x, 1000);
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 4, GFNT_AFFINITY_UPSTREAM, &c),
      GFNT_OK);
  EXPECT_EQ(c.x, 360);
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 2, GFNT_AFFINITY_UPSTREAM, &c),
      GFNT_OK);
  EXPECT_EQ(c.x, 680);
}

TEST(LayoutRects, ARangeIsOneRectangleWhereItIsContiguous) {
  Font font(latin());
  Para para("ABCD", {font.face});
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  GFNT_Rect r[4];
  size_t n = 0;
  ASSERT_EQ(gfnt_layout_range_rects(para.p, &para.layout, 1, 3, r, 4, &n), GFNT_OK);
  ASSERT_EQ(n, 1u);
  EXPECT_EQ(r[0].x, kLetter);
  EXPECT_EQ(r[0].width, 2 * kLetter);
  EXPECT_EQ(r[0].y, 0);
  EXPECT_EQ(r[0].height, 640);
  ASSERT_EQ(gfnt_layout_range_rects(para.p, &para.layout, 2, 2, r, 4, &n), GFNT_OK);
  EXPECT_EQ(n, 0u);
  ASSERT_EQ(gfnt_layout_range_rects(para.p, &para.layout, 0, 99, nullptr, 0, &n),
      GFNT_OK);
  EXPECT_EQ(n, 1u);
}

TEST(LayoutRects, ARangeAcrossLinesGivesARectanglePerLine) {
  Font font(latin());
  Para para("AB CD", {font.face});
  ASSERT_EQ(para.lay(700), GFNT_OK);
  GFNT_Rect r[4];
  size_t n = 0;
  ASSERT_EQ(gfnt_layout_range_rects(para.p, &para.layout, 1, 4, r, 4, &n), GFNT_OK);
  ASSERT_EQ(n, 2u);
  EXPECT_EQ(r[0].x, kLetter);
  EXPECT_EQ(r[0].width, kLetter + kSpace);
  EXPECT_EQ(r[0].y, 0);
  EXPECT_EQ(r[1].x, 0);
  EXPECT_EQ(r[1].width, kLetter);
  EXPECT_EQ(r[1].y, 640);
}

TEST(LayoutRects, ABidirectionalRangeCanBeTwoSeparateRectangles) {
  Font font(latin_and_hebrew());
  GFNT_ParagraphOptions o{};
  o.direction = GFNT_BIDI_LTR;
  // A, space, then alef and bet read right to left: bet is on the left of alef.
  Para para("A \xD7\x90\xD7\x91 B", {font.face}, o);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  GFNT_Rect r[4];
  size_t n = 0;
  // The bytes of "A ", and alef: the space is at [320, 480), bet at [480, 800)
  // and alef at [800, 1120).
  ASSERT_EQ(gfnt_layout_range_rects(para.p, &para.layout, 0, 4, r, 4, &n), GFNT_OK);
  ASSERT_EQ(n, 2u);
  EXPECT_EQ(r[0].x, 0);
  EXPECT_EQ(r[0].width, kLetter + kSpace);
  EXPECT_EQ(r[1].x, kLetter + kSpace + kLetter);
  EXPECT_EQ(r[1].width, kLetter);
}

TEST(LayoutJustify, SpacesGrowSoAFullLineFillsTheWidth) {
  Font font(latin());
  GFNT_ParagraphOptions o{};
  o.align = GFNT_ALIGN_JUSTIFY;
  Para para("AB CD EF", {font.face}, o);
  ASSERT_EQ(para.lay(1500), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);
  const GFNT_Line & l0 = para.layout.lines[0];
  EXPECT_TRUE(l0.justified);
  EXPECT_EQ(l0.extent, 1500);
  EXPECT_EQ(l0.offset, 0);
  const GFNT_LineRun & run = l0.runs[0];
  ASSERT_GE(run.glyph_count, 5u);
  EXPECT_EQ(run.glyphs[2].advance, kSpace + 60);    // the one space that counts
  EXPECT_EQ(run.glyphs[3].pen_x, 2 * kLetter + kSpace + 60);
  EXPECT_EQ(run.glyphs[5].advance, kSpace);         // the hanging one is left
  const GFNT_Line & l1 = para.layout.lines[1];
  EXPECT_FALSE(l1.justified);
  EXPECT_EQ(l1.extent, 2 * kLetter);
}

TEST(LayoutJustify, ALineEndingInANewlineOrWithNoSpaceIsLeftAlone) {
  Font font(latin());
  GFNT_ParagraphOptions o{};
  o.align = GFNT_ALIGN_JUSTIFY;
  Para hard("AB CD\nEF", {font.face}, o);
  ASSERT_EQ(hard.lay(1500), GFNT_OK);
  EXPECT_FALSE(hard.layout.lines[0].justified);
  EXPECT_EQ(hard.layout.lines[0].extent, 2 * 640 + kSpace);
  Para word("ABCD EF", {font.face}, o);
  ASSERT_EQ(word.lay(1400), GFNT_OK);
  ASSERT_EQ(word.layout.line_count, 2u);
  EXPECT_FALSE(word.layout.lines[0].justified);   // only a hanging space
  Para unbounded("AB CD", {font.face}, o);
  ASSERT_EQ(unbounded.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  EXPECT_FALSE(unbounded.layout.lines[0].justified);
}

TEST(LayoutJustify, TheRemainderIsSharedOutOneUnitAtATime) {
  Font font(latin());
  GFNT_ParagraphOptions o{};
  o.align = GFNT_ALIGN_JUSTIFY;
  Para para("A B C D E", {font.face}, o);
  // Letters 320 and spaces 160: "A B C" is 1280 and D would make 1760.
  ASSERT_EQ(para.lay(1441), GFNT_OK);
  const GFNT_Line & l = para.layout.lines[0];
  EXPECT_TRUE(l.justified);
  EXPECT_EQ(l.extent, 1441);   // 161 over two spaces: 81 and 80
  EXPECT_EQ(l.runs[0].glyphs[1].advance, kSpace + 81);
  EXPECT_EQ(l.runs[0].glyphs[3].advance, kSpace + 80);
}

std::vector<uint32_t> latin_ellipsis() {
  return {'A', 'B', 'C', 'D', 'E', 'F', ' ', 0x2026};
}

GFNT_ParagraphOptions truncating(size_t lines, const char * mark = "\xE2\x80\xA6") {
  GFNT_ParagraphOptions o{};
  o.max_lines = lines;
  o.ellipsis = mark;
  return o;
}

TEST(LayoutTruncate, TheLastLineIsCutToMakeRoomForTheMark) {
  Font font(latin_ellipsis());
  Para para("AB CD EF", {font.face}, truncating(1));
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(900), GFNT_OK);
  const GFNT_Layout & l = para.layout;
  ASSERT_EQ(l.line_count, 1u);
  EXPECT_TRUE(l.truncated);
  const GFNT_Line & line = l.lines[0];
  EXPECT_TRUE(line.truncated);
  // Room for 580 beside a 320 mark: one letter, then the mark.
  EXPECT_EQ(line.extent, 2 * kLetter);
  EXPECT_EQ(line.end, 1u);
  ASSERT_EQ(line.run_count, 2u);
  EXPECT_FALSE(line.runs[0].ellipsis);
  EXPECT_TRUE(line.runs[1].ellipsis);
  EXPECT_EQ(line.runs[1].pos, kLetter);
  EXPECT_EQ(line.runs[1].glyph_count, 1u);
  EXPECT_EQ(line.runs[1].glyphs[0].glyph, 8u);
  EXPECT_EQ(line.runs[1].start, line.runs[1].end);
}

TEST(LayoutTruncate, NothingIsCutWhenTheTextFits) {
  Font font(latin_ellipsis());
  Para para("AB CD", {font.face}, truncating(2));
  ASSERT_EQ(para.lay(700), GFNT_OK);
  EXPECT_EQ(para.layout.line_count, 2u);
  EXPECT_FALSE(para.layout.truncated);
  EXPECT_FALSE(para.layout.lines[1].truncated);
  Para two("AB CD EF", {font.face}, truncating(2));
  ASSERT_EQ(two.lay(700), GFNT_OK);
  ASSERT_EQ(two.layout.line_count, 2u);
  EXPECT_TRUE(two.layout.truncated);
  EXPECT_FALSE(two.layout.lines[0].truncated);
  EXPECT_TRUE(two.layout.lines[1].truncated);
}

TEST(LayoutTruncate, AWordTooWideForItsLineIsCutToo) {
  Font font(latin_ellipsis());
  Para para("ABCDEF", {font.face}, truncating(1));
  ASSERT_EQ(para.lay(1000), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_TRUE(line.truncated);
  EXPECT_EQ(line.end, 2u);               // 680 of room: two letters
  EXPECT_EQ(line.extent, 3 * kLetter);
}

TEST(LayoutTruncate, WithoutAMarkTheLinesAreSimplyCut) {
  Font font(latin_ellipsis());
  Para para("AB CD EF", {font.face}, truncating(1, nullptr));
  ASSERT_EQ(para.lay(900), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_TRUE(para.layout.truncated);
  EXPECT_FALSE(para.layout.lines[0].truncated);
  EXPECT_EQ(para.layout.lines[0].run_count, 2u);   // the word and its hanging space
  EXPECT_EQ(para.layout.lines[0].extent, 2 * kLetter);
}

TEST(LayoutTruncate, TheMarkOfARightToLeftParagraphIsOnTheLeft) {
  std::vector<uint32_t> chars = latin_and_hebrew();
  chars.push_back(0x2026);
  Font font(chars);
  GFNT_ParagraphOptions o = truncating(1);
  o.direction = GFNT_BIDI_RTL;
  Para para("\xD7\x90\xD7\x91 \xD7\x92\xD7\x90", {font.face}, o);
  ASSERT_EQ(para.lay(900), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_TRUE(line.truncated);
  ASSERT_GE(line.run_count, 2u);
  EXPECT_TRUE(line.runs[0].ellipsis);
  EXPECT_EQ(line.runs[0].pos, line.offset);
}

TEST(LayoutTruncate, EveryAllocationFailureWhileJustifyingAndTruncatingIsReported) {
  Font font(latin_ellipsis());
  const GFNT_Face * faces[1] = {font.face};
  GFNT_TextSpan span{};
  const std::string text = "AB CD EF AB CD EF";
  span.length = text.size();
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;
  GFNT_ParagraphOptions o = truncating(2);
  o.align = GFNT_ALIGN_JUSTIFY;
  size_t requests = 0;
  {
    gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
    GFNT_Paragraph * p = nullptr;
    GFNT_Layout l{};
    ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o,
                  counting.get(), &p, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(p, 700, &l, nullptr), GFNT_OK);
    EXPECT_TRUE(l.truncated);
    requests = counting.requests();
    gfnt_layout_free(&l);
    gfnt_paragraph_free(p);
    EXPECT_EQ(counting.live(), 0u);
  }
  for (size_t fail = 0; fail < requests; ++fail) {
    gfnttest::FailingAllocator failing(fail);
    GFNT_Paragraph * p = nullptr;
    GFNT_Layout l{};
    GFNT_Result r = gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o,
        failing.get(), &p, nullptr);
    if (r == GFNT_OK) {
      r = gfnt_paragraph_layout(p, 700, &l, nullptr);
    }
    EXPECT_TRUE(r == GFNT_OK || r == GFNT_ERR_OOM) << "request " << fail;
    gfnt_layout_free(&l);
    gfnt_paragraph_free(p);
    EXPECT_EQ(failing.live(), 0u) << "request " << fail << " leaked";
  }
}

GFNT_TextSpan plain_span(const Font & font, size_t length, GFNT_F26Dot6 size = kSize) {
  static const GFNT_Face * faces[1];
  faces[0] = font.face;
  GFNT_TextSpan span{};
  span.length = length;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = size;
  return span;
}

TEST(LayoutSpacing, EachCharacterIsFollowedByTheExtraSpace) {
  Font font(latin());
  GFNT_TextSpan span = plain_span(font, 3);
  span.style.letter_spacing = 64;
  Para para("ABC", {span});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  const GFNT_LineRun & run = para.layout.lines[0].runs[0];
  EXPECT_EQ(run.glyphs[1].pen_x, kLetter + 64);
  EXPECT_EQ(run.glyphs[2].pen_x, 2 * (kLetter + 64));
  EXPECT_EQ(para.layout.lines[0].extent, 3 * (kLetter + 64));
  span.style.letter_spacing = -32;
  Para tight("ABC", {span});
  ASSERT_EQ(tight.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  EXPECT_EQ(tight.layout.lines[0].extent, 3 * (kLetter - 32));
}

TEST(LayoutSpacing, ItCountsInTheLineBreakingToo) {
  Font font(latin());
  GFNT_TextSpan span = plain_span(font, 5);
  span.style.letter_spacing = 80;
  Para para("AB CD", {span});
  // With the spacing "AB" is 800, the space 240 and "CD" 800.
  GFNT_F26Dot6 max = 0;
  ASSERT_EQ(gfnt_paragraph_max_content(para.p, &max), GFNT_OK);
  EXPECT_EQ(max, 800 + 240 + 800);
  ASSERT_EQ(para.lay(1000), GFNT_OK);
  EXPECT_EQ(para.layout.line_count, 2u);
}

TEST(LayoutTabs, ATabGoesToTheNextStopFromTheStartOfTheLine) {
  Font font(latin());
  GFNT_ParagraphOptions o{};
  o.tab_width = 1000;
  Para para("A\tB\t\tC", {font.face}, o);
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  const GFNT_LineRun & run = para.layout.lines[0].runs[0];
  ASSERT_EQ(run.glyph_count, 6u);
  EXPECT_EQ(run.glyphs[1].advance, 680);     // 320 to 1000
  EXPECT_EQ(run.glyphs[2].pen_x, 1000);      // B
  EXPECT_EQ(run.glyphs[3].advance, 1000 - 320);
  EXPECT_EQ(run.glyphs[4].advance, 1000);    // a tab on a stop goes a whole width
  EXPECT_EQ(run.glyphs[5].pen_x, 3000);
  GFNT_F26Dot6 max = 0;
  ASSERT_EQ(gfnt_paragraph_max_content(para.p, &max), GFNT_OK);
  EXPECT_EQ(max, 3000 + kLetter);
}

TEST(LayoutTabs, TheDefaultStopIsEightSpacesAndALineBreaksAfterATab) {
  Font font(latin());
  Para para("A\tB", {font.face});
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  EXPECT_EQ(para.layout.lines[0].runs[0].glyphs[2].pen_x, 8 * kSpace);
  GFNT_ParagraphOptions o{};
  o.tab_width = 1000;
  Para wrap("AB\tCD", {font.face}, o);
  ASSERT_EQ(wrap.lay(700), GFNT_OK);
  ASSERT_EQ(wrap.layout.line_count, 2u);
  EXPECT_EQ(wrap.layout.lines[0].extent, 1000);
  EXPECT_EQ(wrap.layout.lines[1].start, 3u);
}

TEST(LayoutTabs, StopsRestartOnEachLine) {
  Font font(latin());
  GFNT_ParagraphOptions o{};
  o.tab_width = 1000;
  Para para("A\tB\nC\tD", {font.face}, o);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[1].runs[0].glyphs[2].pen_x, 1000);
}

TEST(LayoutInk, TheInkBoxIsTheOutlinesAtTheirPlacedPositions) {
  Font font(latin(), true);
  Para para("A B", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  // The square spans x 50..450 and y 0..700 of 1000 units: at 10 pixels, 32..288
  // across and 448 high above the baseline at 512.
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_EQ(line.ink.x, 32);
  EXPECT_EQ(line.ink.y, 512 - 448);
  EXPECT_EQ(line.ink.width, (320 + 160 + 288) - 32);
  EXPECT_EQ(line.ink.height, 448);
  EXPECT_EQ(para.layout.ink.x, line.ink.x);
  EXPECT_EQ(para.layout.ink.width, line.ink.width);
  // The typographic box is wider and taller: the two boxes differ.
  EXPECT_LT(para.layout.ink.height, para.layout.height);
}

TEST(LayoutInk, AlignmentMovesTheInkAndSpacesHaveNone) {
  Font font(latin(), true);
  GFNT_ParagraphOptions o{};
  o.align = GFNT_ALIGN_RIGHT;
  Para para("A \nB", {font.face}, o);
  ASSERT_EQ(para.lay(1000), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[0].ink.x, 1000 - 320 + 32);
  EXPECT_EQ(para.layout.lines[0].ink.width, 256);
  EXPECT_EQ(para.layout.lines[1].ink.y, 640 + 64);
  EXPECT_EQ(para.layout.ink.y, 64);
  EXPECT_EQ(para.layout.ink.height, 640 + 448);
}

TEST(LayoutInk, AFaceWithNoOutlinesHasNoInk) {
  Font font(latin());
  Para para("AB", {font.face});
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  EXPECT_EQ(para.layout.ink.width, 0);
  EXPECT_EQ(para.layout.ink.height, 0);
  EXPECT_EQ(para.layout.lines[0].ink.width, 0);
}

struct Asked {
  const GFNT_Face * answer = nullptr;
  std::vector<uint32_t> codepoints;
};

const GFNT_Face * provide(void * ctx, uint32_t cp, GFNT_Tag, GFNT_Tag,
    const GFNT_TextStyle *) {
  Asked * a = static_cast<Asked *>(ctx);
  a->codepoints.push_back(cp);
  return a->answer;
}

TEST(LayoutProvider, ACharacterNoFaceHasIsAskedAboutAndItsFaceUsed) {
  Font first({'A'});
  Font second({'B'});
  Asked asked;
  asked.answer = second.face;
  GFNT_ParagraphOptions o{};
  o.font_provider.ctx = &asked;
  o.font_provider.font_for = provide;
  Para para("A B", {first.face}, o);
  ASSERT_EQ(para.created, GFNT_OK);
  // The space is not asked about; B is, once.
  ASSERT_EQ(asked.codepoints.size(), 1u);
  EXPECT_EQ(asked.codepoints[0], uint32_t('B'));
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  bool found = false;
  for (size_t i = 0; i < line.run_count; ++i) {
    if (line.runs[i].face == second.face) {
      found = true;
      EXPECT_TRUE(line.runs[i].fallback);
      EXPECT_EQ(line.runs[i].face_index, 1u);
      EXPECT_EQ(line.runs[i].start, 2u);
      EXPECT_EQ(line.runs[i].glyphs[0].glyph, 1u);
    }
  }
  EXPECT_TRUE(found);
}

TEST(LayoutProvider, ARefusalLeavesTheFirstFacesNotdefAndTheListIsNotAskedForWhatItHas) {
  Font first({'A'});
  Asked asked;   // answers NULL
  GFNT_ParagraphOptions o{};
  o.font_provider.ctx = &asked;
  o.font_provider.font_for = provide;
  Para para("AAB", {first.face}, o);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  EXPECT_EQ(asked.codepoints.size(), 1u);
  const GFNT_LineRun & run = para.layout.lines[0].runs[0];
  EXPECT_EQ(run.face, first.face);
  EXPECT_EQ(run.glyphs[2].glyph, 0u);
}

TEST(LayoutProvider, EveryAllocationFailureWithInkTabsAndAProviderIsReported) {
  Font first({'A', 'B', ' '}, true);
  Font second({'C'}, true);
  Asked asked;
  asked.answer = second.face;
  GFNT_ParagraphOptions o{};
  o.font_provider.ctx = &asked;
  o.font_provider.font_for = provide;
  const GFNT_Face * faces[1] = {first.face};
  GFNT_TextSpan span{};
  const std::string text = "AB\tC AB C\tAB";
  span.length = text.size();
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;
  span.style.letter_spacing = 16;
  size_t requests = 0;
  {
    gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
    GFNT_Paragraph * p = nullptr;
    GFNT_Layout l{};
    ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o,
                  counting.get(), &p, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(p, 900, &l, nullptr), GFNT_OK);
    EXPECT_GT(l.ink.width, 0);
    requests = counting.requests();
    gfnt_layout_free(&l);
    gfnt_paragraph_free(p);
    EXPECT_EQ(counting.live(), 0u);
  }
  for (size_t fail = 0; fail < requests; ++fail) {
    gfnttest::FailingAllocator failing(fail);
    GFNT_Paragraph * p = nullptr;
    GFNT_Layout l{};
    GFNT_Result r = gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o,
        failing.get(), &p, nullptr);
    if (r == GFNT_OK) {
      r = gfnt_paragraph_layout(p, 900, &l, nullptr);
    }
    EXPECT_TRUE(r == GFNT_OK || r == GFNT_ERR_OOM) << "request " << fail << ": " << r;
    gfnt_layout_free(&l);
    gfnt_paragraph_free(p);
    EXPECT_EQ(failing.live(), 0u) << "request " << fail << " leaked";
  }
}

/** The fixture with the `f i` ligature, opened from the data directory. */
struct Fixture {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  Fixture() {
    std::ifstream in(gfnttest::data("fonts/layout-gsub.ttf"), std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    GFNT_Error error;
    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
                  GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, &error), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
  }
  ~Fixture() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
};

TEST(LayoutHit, ALigatureIsDividedBetweenItsCharacters) {
  Fixture font;
  Para para("fi", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_EQ(line.run_count, 1u);
  ASSERT_EQ(line.runs[0].glyph_count, 1u) << "the fixture ligates f i";
  const GFNT_F26Dot6 width = line.runs[0].glyphs[0].advance;
  GFNT_Caret c{};
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 1, GFNT_AFFINITY_DOWNSTREAM, &c),
      GFNT_OK);
  EXPECT_GT(c.x, 0);
  EXPECT_LT(c.x, width);
  GFNT_Hit h{};
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, c.x - 1, 1, &h), GFNT_OK);
  EXPECT_EQ(h.offset, 1u);
  EXPECT_EQ(h.affinity, GFNT_AFFINITY_UPSTREAM);
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, c.x + 1, 1, &h), GFNT_OK);
  EXPECT_EQ(h.offset, 1u);
  EXPECT_EQ(h.affinity, GFNT_AFFINITY_DOWNSTREAM);
  GFNT_Rect r[2];
  size_t n = 0;
  ASSERT_EQ(gfnt_layout_range_rects(para.p, &para.layout, 1, 2, r, 2, &n), GFNT_OK);
  ASSERT_EQ(n, 1u);
  EXPECT_EQ(r[0].x, c.x);
  EXPECT_EQ(r[0].x + r[0].width, width);
}

// --- hyphenation ---------------------------------------------------------------------------

/** A..F, a space, a soft hyphen and the two hyphens (glyphs 1..6, 7, 8, 9 and 10). */
std::vector<uint32_t> latin_hyphens() {
  return {'A', 'B', 'C', 'D', 'E', 'F', ' ', 0xAD, '-', 0x2010};
}

std::vector<uint32_t> latin_hyphen_ascii_only() {
  return {'A', 'B', 'C', 'D', 'E', 'F', ' ', 0xAD, '-'};
}

GFNT_ParagraphOptions hyphenating(GFNT_Hyphens how) {
  GFNT_ParagraphOptions o{};
  o.hyphens = how;
  return o;
}

/** Marks the byte offsets it is given, in every word it is asked about of at least four bytes. */
struct Points {
  std::vector<size_t> at;
  std::vector<std::string> asked;
  std::vector<GFNT_Tag> languages;
  GFNT_Result result = GFNT_OK;
};

GFNT_Result mark_points(void * ctx, const char * word, size_t length, GFNT_Tag language,
    uint8_t * allowed) {
  Points * points = static_cast<Points *>(ctx);

  points->asked.push_back(std::string(word, length));
  points->languages.push_back(language);
  if (points->result != GFNT_OK) {
    return points->result;
  }
  for (size_t k : points->at) {
    if (k < length) {
      allowed[k] = 1;
    }
  }
  return GFNT_OK;
}

TEST(LayoutHyphen, ALineThatBreaksAtASoftHyphenEndsInAHyphenThatTakesRoom) {
  Font font(latin_hyphen_ascii_only());
  Para para("ABC\xC2\xAD" "DEF", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(1300), GFNT_OK);   // "ABC" and a hyphen are 4 x 320.
  const GFNT_Layout & l = para.layout;
  ASSERT_EQ(l.line_count, 2u);
  const GFNT_Line & first = l.lines[0];
  EXPECT_EQ(first.extent, 4 * kLetter) << "the soft hyphen has no width and the hyphen has one";
  EXPECT_EQ(first.end, 5u) << "the soft hyphen is two bytes and stays on the line";
  EXPECT_EQ(l.lines[1].start, 5u);
  EXPECT_EQ(l.lines[1].extent, 3 * kLetter);
  ASSERT_EQ(first.run_count, 2u);
  EXPECT_FALSE(first.runs[0].hyphen);
  EXPECT_FALSE(first.runs[0].ellipsis);
  EXPECT_TRUE(first.runs[1].hyphen);
  EXPECT_EQ(first.runs[1].start, first.runs[1].end) << "it stands for no text";
  ASSERT_EQ(first.runs[1].glyph_count, 1u);
  EXPECT_EQ(first.runs[1].glyphs[0].glyph, 9u) << "U+002D, as the face has no U+2010";
  EXPECT_EQ(first.runs[1].pos, 3 * kLetter);
  for (size_t i = 0; i < l.lines[1].run_count; ++i) {
    EXPECT_FALSE(l.lines[1].runs[i].hyphen);
  }
}

TEST(LayoutHyphen, TheHyphenIsU2010WhenTheFaceHasIt) {
  Font font(latin_hyphens());
  Para para("ABC\xC2\xAD" "DEF", {font.face});
  ASSERT_EQ(para.lay(1300), GFNT_OK);
  const GFNT_Line & first = para.layout.lines[0];
  ASSERT_EQ(first.run_count, 2u);
  EXPECT_EQ(first.runs[1].glyphs[0].glyph, 10u);
}

TEST(LayoutHyphen, ASoftHyphenThatIsNotBrokenAtIsInvisible) {
  Font font(latin_hyphen_ascii_only());
  Para para("ABC\xC2\xAD" "DEF", {font.face});
  ASSERT_EQ(para.lay(2000), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_EQ(para.layout.lines[0].extent, 6 * kLetter);
  for (size_t i = 0; i < para.layout.lines[0].run_count; ++i) {
    EXPECT_FALSE(para.layout.lines[0].runs[i].hyphen);
  }
}

TEST(LayoutHyphen, TheHyphenCountsTowardTheWidthSoALaterPointIsNotTakenWhenItDoesNotFit) {
  // A B (soft) C D (soft) E F: with room for 1000, "AB-" is 960 and "ABCD-" is 1600.
  Font font(latin_hyphen_ascii_only());
  Para para("AB\xC2\xAD" "CD\xC2\xAD" "EF", {font.face});
  ASSERT_EQ(para.lay(1000), GFNT_OK);
  const GFNT_Layout & l = para.layout;
  ASSERT_EQ(l.line_count, 3u);
  EXPECT_EQ(l.lines[0].extent, 3 * kLetter);
  EXPECT_EQ(l.lines[1].extent, 3 * kLetter);
  EXPECT_EQ(l.lines[2].extent, 2 * kLetter);
  EXPECT_TRUE(l.lines[0].runs[l.lines[0].run_count - 1].hyphen);
  EXPECT_TRUE(l.lines[1].runs[l.lines[1].run_count - 1].hyphen);
  EXPECT_FALSE(l.lines[2].runs[l.lines[2].run_count - 1].hyphen);
  // Room for 960 exactly takes the hyphen; one less would still break there, as the only point.
  ASSERT_EQ(para.lay(960), GFNT_OK);
  EXPECT_EQ(para.layout.line_count, 3u);
}

TEST(LayoutHyphen, WhenTheHyphenWouldNotFitTheLineBreaksAtAnEarlierPlaceThatDoes) {
  // "AB CD" soft "EF": the hyphen after "AB CD" would end at 1440 + 320, past 1500, though "AB CD"
  // fits; so the line breaks at the space, and "CDEF" goes whole on the next.
  Font font(latin_hyphen_ascii_only());
  Para para("AB CD\xC2\xAD" "EF", {font.face});
  ASSERT_EQ(para.lay(1500), GFNT_OK);
  const GFNT_Layout & l = para.layout;
  ASSERT_EQ(l.line_count, 2u);
  EXPECT_FALSE(l.lines[0].runs[l.lines[0].run_count - 1].hyphen);
  EXPECT_EQ(l.lines[1].start, 3u);
}

TEST(LayoutHyphen, HyphensNoneMakesASoftHyphenNoPlaceToBreak) {
  Font font(latin_hyphen_ascii_only());
  Para para("ABC\xC2\xAD" "DEF", {font.face}, hyphenating(GFNT_HYPHENS_NONE));
  ASSERT_EQ(para.lay(1300), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_EQ(para.layout.lines[0].extent, 6 * kLetter);
}

TEST(LayoutHyphen, AHyphenatorsPointsAreUsedOnlyWhenAsked) {
  Font font(latin_hyphen_ascii_only());
  Points points;
  points.at = {2, 4};
  GFNT_ParagraphOptions o = hyphenating(GFNT_HYPHENS_AUTO);
  o.hyphenator.ctx = &points;
  o.hyphenator.points = mark_points;
  Para para("ABCDEF", {font.face}, o);
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(points.asked.size(), 1u);
  EXPECT_EQ(points.asked[0], "ABCDEF");
  ASSERT_EQ(para.lay(1000), GFNT_OK);
  const GFNT_Layout & l = para.layout;
  ASSERT_EQ(l.line_count, 3u);
  EXPECT_EQ(l.lines[0].end, 2u);
  EXPECT_EQ(l.lines[1].end, 4u);
  EXPECT_TRUE(l.lines[0].runs[l.lines[0].run_count - 1].hyphen);
  EXPECT_TRUE(l.lines[1].runs[l.lines[1].run_count - 1].hyphen);
  // The same hyphenator under the default is not consulted, and the word does not split.
  points.asked.clear();
  GFNT_ParagraphOptions manual = o;
  manual.hyphens = GFNT_HYPHENS_MANUAL;
  Para off("ABCDEF", {font.face}, manual);
  ASSERT_EQ(off.created, GFNT_OK);
  EXPECT_TRUE(points.asked.empty());
  ASSERT_EQ(off.lay(1000), GFNT_OK);
  EXPECT_EQ(off.layout.line_count, 1u);
}

TEST(LayoutHyphen, TheHyphenatorIsAskedAboutWordsOfLettersWithTheSpansLanguageOnly) {
  Font font(latin_hyphen_ascii_only());
  Points points;
  points.at = {2};
  GFNT_ParagraphOptions o = hyphenating(GFNT_HYPHENS_AUTO);
  o.hyphenator.ctx = &points;
  o.hyphenator.points = mark_points;
  GFNT_TextSpan a{}, b{};
  const GFNT_Face * faces[1] = {font.face};
  a.style.faces = faces;
  a.style.face_count = 1;
  a.style.size = kSize;
  a.style.language = GFNT_TAG('E', 'N', 'G', ' ');
  b = a;
  b.style.language = GFNT_TAG('D', 'E', 'U', ' ');
  // "ABCD" in English, "EF" and a soft-hyphened word and a one-letter word in German.
  const std::string text = "ABCD EF A AB\xC2\xAD" "CD";
  a.length = 4;
  b.length = text.size() - 4;
  Para para(text, std::vector<GFNT_TextSpan>{a, b}, o);
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(points.asked.size(), 2u) << "not the one-letter word or the one with a soft hyphen";
  EXPECT_EQ(points.asked[0], "ABCD");
  EXPECT_EQ(points.languages[0], a.style.language);
  EXPECT_EQ(points.asked[1], "EF");
  EXPECT_EQ(points.languages[1], b.style.language);
}

TEST(LayoutHyphen, AFailingHyphenatorEndsTheParagraphWithItsError) {
  Font font(latin_hyphen_ascii_only());
  Points points;
  points.result = GFNT_ERR_LIMIT;
  GFNT_ParagraphOptions o = hyphenating(GFNT_HYPHENS_AUTO);
  o.hyphenator.ctx = &points;
  o.hyphenator.points = mark_points;
  Para para("ABCDEF", {font.face}, o);
  EXPECT_EQ(para.created, GFNT_ERR_LIMIT);
}

TEST(LayoutHyphen, MinContentIsTheWidestStretchBetweenPointsHyphenIncluded) {
  Font font(latin_hyphen_ascii_only());
  Points points;
  points.at = {2, 4};
  GFNT_ParagraphOptions o = hyphenating(GFNT_HYPHENS_AUTO);
  o.hyphenator.ctx = &points;
  o.hyphenator.points = mark_points;
  Para auto_para("ABCDEF", {font.face}, o);
  Para plain("ABCDEF", {font.face});
  GFNT_F26Dot6 narrow = 0, whole = 0;
  ASSERT_EQ(gfnt_paragraph_min_content(auto_para.p, &narrow), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_min_content(plain.p, &whole), GFNT_OK);
  EXPECT_EQ(narrow, 3 * kLetter) << "AB, a hyphen";
  EXPECT_EQ(whole, 6 * kLetter);
}

TEST(LayoutHyphen, ALastLineThatTheLimitCutShowsTheMarkAndNotAHyphen) {
  Font font({'A', 'B', 'C', 'D', 'E', 'F', ' ', 0xAD, '-', 0x2026});
  GFNT_ParagraphOptions o = truncating(1);
  Para para("AB\xC2\xAD" "CDEF", {font.face}, o);
  ASSERT_EQ(para.lay(1000), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_TRUE(line.truncated);
  for (size_t i = 0; i < line.run_count; ++i) {
    EXPECT_FALSE(line.runs[i].hyphen);
  }
  EXPECT_TRUE(line.runs[line.run_count - 1].ellipsis);
}

TEST(LayoutHyphen, AHyphenatedLineIsJustifiedAsAnyOtherAndAHitOnItsHyphenLandsAtTheLineEnd) {
  Font font(latin_hyphen_ascii_only());
  GFNT_ParagraphOptions o = hyphenating(GFNT_HYPHENS_MANUAL);
  o.align = GFNT_ALIGN_JUSTIFY;
  Para para("A B CD\xC2\xAD" "EFGH", {font.face}, o);
  ASSERT_EQ(para.lay(2000), GFNT_OK);
  const GFNT_Layout & l = para.layout;
  ASSERT_GE(l.line_count, 2u);
  const GFNT_Line & first = l.lines[0];
  ASSERT_TRUE(first.runs[first.run_count - 1].hyphen);
  EXPECT_TRUE(first.justified);
  EXPECT_EQ(first.extent, 2000) << "the spaces took up the slack, hyphen and all";
  GFNT_Hit hit{};
  const GFNT_LineRun & hy = first.runs[first.run_count - 1];
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &l, hy.pos + hy.advance / 2, first.box.y + 10, &hit), GFNT_OK);
  EXPECT_EQ(hit.line, 0u);
  EXPECT_EQ(hit.offset, first.end);
  GFNT_Caret caret{};
  ASSERT_EQ(gfnt_layout_caret(para.p, &l, first.end, GFNT_AFFINITY_UPSTREAM, &caret), GFNT_OK);
  EXPECT_EQ(caret.line, 0u);
  EXPECT_EQ(caret.x, hy.pos) << "an upstream caret at the break stands before the hyphen";
}

TEST(LayoutHyphen, EveryAllocationFailureWhileHyphenatingIsReportedAndNothingLeaks) {
  Font font(latin_hyphen_ascii_only());
  const GFNT_Face * faces[1] = {font.face};
  GFNT_TextSpan span{};
  const std::string text = "ABCDEF AB\xC2\xAD" "CDEF ABCDEF";
  span.length = text.size();
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;
  Points points;
  points.at = {2, 4};
  GFNT_ParagraphOptions o = hyphenating(GFNT_HYPHENS_AUTO);
  o.hyphenator.ctx = &points;
  o.hyphenator.points = mark_points;
  o.align = GFNT_ALIGN_JUSTIFY;
  size_t requests = 0;
  {
    gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
    GFNT_Paragraph * p = nullptr;
    GFNT_Layout l{};
    ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o,
                  counting.get(), &p, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(p, 1000, &l, nullptr), GFNT_OK);
    EXPECT_GT(l.line_count, 3u);
    requests = counting.requests();
    gfnt_layout_free(&l);
    gfnt_paragraph_free(p);
    EXPECT_EQ(counting.live(), 0u);
  }
  for (size_t fail = 0; fail < requests; ++fail) {
    gfnttest::FailingAllocator failing(fail);
    GFNT_Paragraph * p = nullptr;
    GFNT_Layout l{};
    GFNT_Result r = gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o,
        failing.get(), &p, nullptr);
    if (r == GFNT_OK) {
      r = gfnt_paragraph_layout(p, 1000, &l, nullptr);
    }
    EXPECT_TRUE(r == GFNT_OK || r == GFNT_ERR_OOM) << "request " << fail;
    gfnt_layout_free(&l);
    gfnt_paragraph_free(p);
    EXPECT_EQ(failing.live(), 0u) << "request " << fail << " leaked";
  }
}

TEST(LayoutHyphen, WhenNoPointHasRoomForItsHyphenTheEarliestIsTakenAndOverflows) {
  // Two soft hyphens in a row after one letter: neither has room for a hyphen in 500, and the
  // line is the shortest that has a place to end, so the first of them.
  Font font(latin_hyphen_ascii_only());
  Para para("A\xC2\xAD\xC2\xAD" "BCD", {font.face});
  ASSERT_EQ(para.lay(500), GFNT_OK);
  const GFNT_Layout & l = para.layout;
  ASSERT_GE(l.line_count, 2u);
  EXPECT_EQ(l.lines[0].end, 3u);
  EXPECT_TRUE(l.lines[0].runs[l.lines[0].run_count - 1].hyphen);
  EXPECT_EQ(l.lines[0].extent, 2 * kLetter);
}

TEST(LayoutHyphen, AnOptionOutOfRangeIsRefused) {
  Font font(latin_hyphen_ascii_only());
  GFNT_ParagraphOptions o{};
  o.hyphens = static_cast<GFNT_Hyphens>(7);
  Para para("AB", {font.face}, o);
  EXPECT_EQ(para.created, GFNT_ERR_INVALID);
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
