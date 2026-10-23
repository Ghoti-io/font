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
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ghoti.io/font/layout.h>

namespace {

constexpr GFNT_F26Dot6 kSize = 10 * 64;
constexpr GFNT_F26Dot6 kLetter = 320;
constexpr GFNT_F26Dot6 kSpace = 160;

void snapshot_layout(const std::string & text, GFNT_F26Dot6 width, const GFNT_Layout & layout) {
  const char * path = std::getenv("GHOTI_UNIT_LAYOUT_SNAPSHOT");
  if (!path || !*path) return;
  std::ofstream out(path, std::ios::app);
  if (!out) return;
  out << "{\"text\":\"";
  for (unsigned char c : text) out << "0123456789abcdef"[c >> 4] << "0123456789abcdef"[c & 15];
  out << "\",\"width\":" << width << ",\"result\":{\"w\":" << layout.width
      << ",\"h\":" << layout.height << ",\"truncated\":" << (layout.truncated ? "true" : "false")
      << ",\"mode\":" << static_cast<int>(layout.writing_mode) << ",\"ink\":["
      << layout.ink.x << ',' << layout.ink.y << ',' << layout.ink.width << ',' << layout.ink.height
      << "],\"lines\":[";
  for (size_t i = 0; i < layout.line_count; ++i) {
    const GFNT_Line & line = layout.lines[i];
    if (i) out << ',';
    out << "{\"s\":" << line.start << ",\"e\":" << line.end << ",\"offset\":"
        << line.offset << ",\"extent\":" << line.extent << ",\"baseline\":" << line.baseline
        << ",\"ascent\":" << line.ascent << ",\"descent\":" << line.descent
        << ",\"justified\":" << (line.justified ? "true" : "false")
        << ",\"truncated\":" << (line.truncated ? "true" : "false") << ",\"box\":["
        << line.box.x << ',' << line.box.y << ',' << line.box.width << ',' << line.box.height
        << "],\"ink\":[" << line.ink.x << ',' << line.ink.y << ',' << line.ink.width << ','
        << line.ink.height << "],\"runs\":[";
    for (size_t j = 0; j < line.run_count; ++j) {
      const GFNT_LineRun & run = line.runs[j];
      if (j) out << ',';
      out << "{\"span\":" << run.span << ",\"face_index\":" << run.face_index
          << ",\"fallback\":" << (run.fallback ? "true" : "false")
          << ",\"ellipsis\":" << (run.ellipsis ? "true" : "false")
          << ",\"hyphen\":" << (run.hyphen ? "true" : "false") << ",\"level\":"
          << static_cast<unsigned>(run.level) << ",\"orientation\":"
          << static_cast<int>(run.orientation) << ",\"start\":" << run.start << ",\"end\":"
          << run.end << ",\"pos\":" << run.pos << ",\"advance\":" << run.advance
          << ",\"glyphs\":[";
      for (size_t k = 0; k < run.glyph_count; ++k) {
        const GFNT_PlacedGlyph & glyph = run.glyphs[k];
        if (k) out << ',';
        out << '[' << glyph.glyph << ',' << glyph.cluster << ',' << glyph.pen_x << ','
            << glyph.pen_y << ',' << glyph.x << ',' << glyph.y << ',' << glyph.advance << ']';
      }
      out << "]}";
    }
    out << "]}";
  }
  out << "]}}\n";
}

/** A font whose glyph n (from 1) is the n-th code point of `chars`. */
struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  explicit Font(const std::vector<uint32_t> & chars, bool outlines = false,
      int16_t x_height = 0, int16_t line_gap = 0) {
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
    // The glyphs that have outlines are squares from x = 50, so their left side
    // bearing is 50: a glyph is placed with its origin (xMin less the bearing) at
    // zero, and a bearing that contradicts the outline would move the ink.
    std::vector<std::pair<uint16_t, int16_t>> metrics(glyphs,
        {500, static_cast<int16_t>(outlines ? 50 : 0)});
    metrics[0].second = 0;
    for (size_t i = 0; i < chars.size(); ++i) {
      if (chars[i] == ' ') {
        metrics[i + 1].first = 250;
        metrics[i + 1].second = 0;
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
        {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, line_gap, glyphs)},
        {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics)},
        {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(glyphs)},
        {GFNT_TAG('c', 'm', 'a', 'p'),
            gfnttest::build_cmap({{3, 1, gfnttest::build_cmap_format4(segments)}})},
    };
    if (x_height > 0) {
      gfnttest::Os2Spec os2;
      os2.version = 2;
      os2.x_height = x_height;
      tables.push_back({GFNT_TAG('O', 'S', '/', '2'), gfnttest::build_os2(os2)});
    }
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
  std::string text;

  Para(const std::string & text, std::vector<const GFNT_Face *> list,
      const GFNT_ParagraphOptions & options = {}, GFNT_F26Dot6 size = kSize)
      : text(text) {
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
      : spans(std::move(given)), text(text) {
    created = gfnt_paragraph_create(text.data(), text.size(), spans.data(),
        spans.size(), &options, nullptr, &p, nullptr);
  }
  GFNT_Result lay(GFNT_F26Dot6 width) {
    gfnt_layout_free(&layout);
    GFNT_Result result = gfnt_paragraph_layout(p, width, &layout, nullptr);
    if (result == GFNT_OK) snapshot_layout(text, width, layout);
    return result;
  }
  ~Para() {
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(p);
  }
  Para(const Para &) = delete;
  Para & operator=(const Para &) = delete;
};

struct FlowBands {
  size_t calls = 0;
  bool offset = true;
  GFNT_F26Dot6 width = 800;
  bool zero_width = false;
  bool retry_by_height = false;
  bool shrink_by_height = false;
  bool clear = false;
  bool clear_on_height = false;
  bool saw_clear_retry = false;
  GFNT_Result result = GFNT_OK;
  bool invalid = false;
};

GFNT_Result flow_band(void * context, GFNT_F26Dot6 top, GFNT_F26Dot6 height,
    GFNT_Band * out) {
  auto & bands = *static_cast<FlowBands *>(context);
  ++bands.calls;
  if (bands.invalid) {
    out->start = 0;
    out->width = -1;
    out->next_top = 0;
    return GFNT_OK;
  }
  out->start = bands.offset ? (top == 0 ? 64 : 128) : 0;
  out->width = bands.width;
  if (bands.zero_width) out->width = 0;
  out->next_top = 0;
  if (bands.retry_by_height && height > 0) out->width = 320;
  if (bands.shrink_by_height && height > 0 && out->width > 0) out->width--;
  if (bands.clear_on_height && top == 1000 && height == 0) bands.saw_clear_retry = true;
  if (bands.clear_on_height && top < 1000 && height > 0) {
    out->width = 1000;
    out->next_top = 1000;
  }
  if (bands.clear && top < 1000) {
    out->width = 400;
    out->next_top = 1000;
  }
  return bands.result;
}

GFNT_Result negative_start_band(void *, GFNT_F26Dot6, GFNT_F26Dot6, GFNT_Band * out) {
  *out = {-128, 800, 0};
  return GFNT_OK;
}

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

TEST(Layout, UppercaseExpansionKeepsSourceByteClusters) {
  Font font({'A', 'S'});
  const GFNT_Face * faces[] = {font.face};
  const std::string text = "a\xC3\x9F";  // aß → ASS
  GFNT_TextSpan span{};
  span.length = text.size();
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;
  span.style.text_transform = GFNT_TEXT_TRANSFORM_UPPERCASE;
  GFNT_Paragraph * paragraph = nullptr;
  GFNT_Layout layout{};
  ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, nullptr,
                nullptr, &paragraph, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(paragraph, GFNT_LAYOUT_UNBOUNDED, &layout, nullptr), GFNT_OK);
  ASSERT_EQ(layout.line_count, 1u);
  EXPECT_EQ(layout.lines[0].start, 0u);
  EXPECT_EQ(layout.lines[0].end, text.size());
  ASSERT_EQ(layout.lines[0].run_count, 1u);
  const GFNT_LineRun & run = layout.lines[0].runs[0];
  ASSERT_EQ(run.glyph_count, 3u);
  EXPECT_EQ(run.glyphs[0].glyph, 1u);
  EXPECT_EQ(run.glyphs[0].cluster, 0u);
  EXPECT_EQ(run.glyphs[1].glyph, 2u);
  EXPECT_EQ(run.glyphs[1].cluster, 1u);
  EXPECT_EQ(run.glyphs[2].glyph, 2u);
  EXPECT_EQ(run.glyphs[2].cluster, 1u);
  GFNT_Rect rects[2]{};
  size_t rect_count = 0;
  ASSERT_EQ(gfnt_layout_range_rects(paragraph, &layout, 1, text.size(), rects, 2,
                &rect_count), GFNT_OK);
  ASSERT_EQ(rect_count, 1u);
  EXPECT_EQ(rects[0].width, 2 * kLetter)
      << "the transformed expansion must remain selectable as its source character";
  gfnt_layout_free(&layout);
  gfnt_paragraph_free(paragraph);
}

TEST(Layout, LowercaseUsesContextAndLanguageTailoring) {
  {
    Font font({0x03BF, 0x03C2});
    const GFNT_Face * faces[] = {font.face};
    const std::string text = "\xCE\x9F\xCE\xA3";  // ΟΣ → ος (final sigma)
    GFNT_TextSpan span{};
    span.length = text.size();
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = kSize;
    span.style.text_transform = GFNT_TEXT_TRANSFORM_LOWERCASE;
    GFNT_Paragraph * paragraph = nullptr;
    GFNT_Layout layout{};
    ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, nullptr,
                  nullptr, &paragraph, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(paragraph, GFNT_LAYOUT_UNBOUNDED, &layout, nullptr), GFNT_OK);
    ASSERT_EQ(layout.lines[0].runs[0].glyph_count, 2u);
    EXPECT_EQ(layout.lines[0].runs[0].glyphs[1].glyph, 2u);
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(paragraph);
  }
  {
    Font font({0x0131});
    const GFNT_Face * faces[] = {font.face};
    const std::string text = "I";
    GFNT_TextSpan span{};
    span.length = text.size();
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = kSize;
    span.style.text_transform = GFNT_TEXT_TRANSFORM_LOWERCASE;
    span.style.case_tailoring = GFNT_CASE_TAILORING_TURKIC;
    GFNT_Paragraph * paragraph = nullptr;
    GFNT_Layout layout{};
    ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, nullptr,
                  nullptr, &paragraph, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(paragraph, GFNT_LAYOUT_UNBOUNDED, &layout, nullptr), GFNT_OK);
    ASSERT_EQ(layout.lines[0].runs[0].glyph_count, 1u);
    EXPECT_EQ(layout.lines[0].runs[0].glyphs[0].glyph, 1u);
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(paragraph);
  }
}

TEST(Layout, FullWidthAndFullSizeKanaTransformations) {
  {
    Font font({0xff21});
    const GFNT_Face * faces[] = {font.face};
    GFNT_TextSpan span{};
    span.length = 1;
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = kSize;
    span.style.text_transform = GFNT_TEXT_TRANSFORM_FULL_WIDTH;
    GFNT_Paragraph * paragraph = nullptr;
    GFNT_Layout layout{};
    ASSERT_EQ(gfnt_paragraph_create("A", 1, &span, 1, nullptr, nullptr,
                  &paragraph, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(paragraph, GFNT_LAYOUT_UNBOUNDED, &layout, nullptr), GFNT_OK);
    ASSERT_EQ(layout.lines[0].runs[0].glyph_count, 1u);
    EXPECT_EQ(layout.lines[0].runs[0].glyphs[0].glyph, 1u);
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(paragraph);
  }
  {
    Font font({0xff21});
    const GFNT_Face * faces[] = {font.face};
    GFNT_TextSpan span{};
    span.length = 1;
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = kSize;
    span.style.text_transform = static_cast<GFNT_TextTransform>(
        GFNT_TEXT_TRANSFORM_UPPERCASE | GFNT_TEXT_TRANSFORM_FULL_WIDTH);
    GFNT_Paragraph * paragraph = nullptr;
    GFNT_Layout layout{};
    ASSERT_EQ(gfnt_paragraph_create("a", 1, &span, 1, nullptr, nullptr,
                  &paragraph, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(paragraph, GFNT_LAYOUT_UNBOUNDED, &layout, nullptr), GFNT_OK);
    ASSERT_EQ(layout.lines[0].runs[0].glyph_count, 1u);
    EXPECT_EQ(layout.lines[0].runs[0].glyphs[0].glyph, 1u);
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(paragraph);
  }
  {
    Font font({0x30ab});
    const GFNT_Face * faces[] = {font.face};
    const std::string text = "\xef\xbd\xb6";  // U+FF76 halfwidth KA
    GFNT_TextSpan span{};
    span.length = text.size();
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = kSize;
    span.style.text_transform = GFNT_TEXT_TRANSFORM_FULL_WIDTH;
    GFNT_Paragraph * paragraph = nullptr;
    GFNT_Layout layout{};
    ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, nullptr,
                  nullptr, &paragraph, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(paragraph, GFNT_LAYOUT_UNBOUNDED, &layout, nullptr), GFNT_OK);
    ASSERT_EQ(layout.lines[0].runs[0].glyph_count, 1u);
    EXPECT_EQ(layout.lines[0].runs[0].glyphs[0].glyph, 1u);
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(paragraph);
  }
  {
    Font font({0xff71});
    const GFNT_Face * faces[] = {font.face};
    const std::string text = "\xef\xbd\xa7";  // U+FF67 halfwidth small A
    GFNT_TextSpan span{};
    span.length = text.size();
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = kSize;
    span.style.text_transform = GFNT_TEXT_TRANSFORM_FULL_SIZE_KANA;
    GFNT_Paragraph * paragraph = nullptr;
    GFNT_Layout layout{};
    ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, nullptr,
                  nullptr, &paragraph, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(paragraph, GFNT_LAYOUT_UNBOUNDED, &layout, nullptr), GFNT_OK);
    ASSERT_EQ(layout.lines[0].runs[0].glyph_count, 1u);
    EXPECT_EQ(layout.lines[0].runs[0].glyphs[0].glyph, 1u);
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(paragraph);
  }
}

TEST(LayoutAnnotation, RubyWidensItsBaseAndProducesASeparatePaintTrack) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation ruby{};
  ruby.base_start = 0;
  ruby.base_end = 2;
  ruby.kind = GFNT_ANNOTATION_RUBY;
  ruby.position = GFNT_ANNOTATION_OVER;
  ruby.style.faces = faces;
  ruby.style.face_count = 1;
  ruby.style.size = kSize;
  ruby.text = "ABC";
  ruby.text_length = 3;
  ruby.align = GFNT_ANNOTATION_ALIGN_CENTER;
  ruby.group_break = true;
  GFNT_Annotation nested{};
  nested.base_start = 0;
  nested.base_end = 1;
  nested.kind = GFNT_ANNOTATION_RUBY;
  nested.position = GFNT_ANNOTATION_UNDER;
  nested.style = ruby.style;
  nested.text = "A";
  nested.text_length = 1;
  ruby.children = &nested;
  ruby.child_count = 1;
  GFNT_ParagraphOptions options{};
  options.annotations = &ruby;
  options.annotation_count = 1;
  Para para("AB", {font.face}, options);

  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_F26Dot6 minimum = 0;
  ASSERT_EQ(gfnt_paragraph_min_content(para.p, &minimum), GFNT_OK);
  EXPECT_EQ(minimum, 3 * kLetter);
  ASSERT_EQ(para.lay(3 * kLetter), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_EQ(line.extent, 3 * kLetter);
  ASSERT_EQ(line.annotation_count, 1u);
  const GFNT_LineAnnotation & output = line.annotations[0];
  EXPECT_EQ(output.kind, GFNT_ANNOTATION_RUBY);
  EXPECT_EQ(output.base_start, 0u);
  EXPECT_EQ(output.base_end, 2u);
  EXPECT_EQ(output.box.width, 3 * kLetter);
  ASSERT_EQ(output.child_count, 1u);
  EXPECT_EQ(output.children[0].kind, GFNT_ANNOTATION_RUBY);
  ASSERT_EQ(output.run_count, 1u);
  ASSERT_EQ(output.runs[0].glyph_count, 3u);
  EXPECT_EQ(output.runs[0].start, 0u);
  EXPECT_EQ(output.runs[0].end, 3u);
  EXPECT_LT(output.box.y + output.box.height, line.baseline);
  GFNT_Hit hit{};
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout,
      output.box.x + output.box.width / 2, output.box.y + output.box.height / 2, &hit), GFNT_OK);
  EXPECT_GE(hit.offset, output.base_start);
  EXPECT_LE(hit.offset, output.base_end);
  EXPECT_EQ(hit.offset, 2u);
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout,
      output.children[0].box.x + output.children[0].box.width / 2,
      output.children[0].box.y + output.children[0].box.height / 2, &hit), GFNT_OK);
  EXPECT_LE(hit.offset, 2u);
  GFNT_Rect range{};
  size_t range_count = 0;
  ASSERT_EQ(gfnt_layout_range_rects(para.p, &para.layout, 0, 2, &range, 1, &range_count), GFNT_OK);
  ASSERT_EQ(range_count, 1u);
  EXPECT_GT(range.y, output.box.y);
  EXPECT_EQ(range.y, output.box.y + output.box.height);
}

TEST(LayoutAnnotation, RubyGroupBreakingIsControlledByTheDescriptor) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation ruby{};
  ruby.base_start = 0;
  ruby.base_end = 3;
  ruby.kind = GFNT_ANNOTATION_RUBY;
  ruby.position = GFNT_ANNOTATION_OVER;
  ruby.style.faces = faces;
  ruby.style.face_count = 1;
  ruby.style.size = kSize;
  ruby.text = "abc";
  ruby.text_length = 3;
  GFNT_ParagraphOptions options{};
  options.annotations = &ruby;
  options.annotation_count = 1;

  Para breakable("A B", {font.face}, options);
  ASSERT_EQ(breakable.created, GFNT_OK);
  ASSERT_EQ(breakable.lay(kLetter), GFNT_OK);
  EXPECT_EQ(breakable.layout.line_count, 2u);
  ASSERT_EQ(breakable.layout.lines[0].annotation_count, 1u);
  ASSERT_EQ(breakable.layout.lines[1].annotation_count, 1u);
  EXPECT_EQ(breakable.layout.lines[0].annotations[0].base_start, 0u);
  EXPECT_EQ(breakable.layout.lines[0].annotations[0].base_end, 2u);
  EXPECT_EQ(breakable.layout.lines[1].annotations[0].base_start, 2u);
  EXPECT_EQ(breakable.layout.lines[1].annotations[0].base_end, 3u);
  EXPECT_EQ(breakable.layout.lines[0].annotations[0].runs[0].glyph_count, 2u);
  EXPECT_EQ(breakable.layout.lines[1].annotations[0].runs[0].glyph_count, 1u);

  ruby.group_break = true;
  Para grouped("A B", {font.face}, options);
  ASSERT_EQ(grouped.created, GFNT_OK);
  ASSERT_EQ(grouped.lay(kLetter), GFNT_OK);
  ASSERT_EQ(grouped.layout.line_count, 1u);
  EXPECT_GT(grouped.layout.lines[0].extent, kLetter);
}

TEST(LayoutAnnotation, MergeConcatenatesAdjacentCompatibleRubyRecords) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation ruby[2]{};
  for (size_t i = 0; i < 2; i++) {
    ruby[i].base_start = i;
    ruby[i].base_end = i + 1;
    ruby[i].kind = GFNT_ANNOTATION_RUBY;
    ruby[i].position = GFNT_ANNOTATION_OVER;
    ruby[i].style.faces = faces;
    ruby[i].style.face_count = 1;
    ruby[i].style.size = kSize;
    ruby[i].align = GFNT_ANNOTATION_ALIGN_CENTER;
    ruby[i].merge = true;
    ruby[i].group_break = true;
  }
  ruby[0].text = "x";
  ruby[0].text_length = 1;
  ruby[1].text = "y";
  ruby[1].text_length = 1;
  GFNT_ParagraphOptions options{};
  options.annotations = ruby;
  options.annotation_count = 2;
  Para para("AB", {font.face}, options);
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(para.layout.lines[0].annotation_count, 1u);
  const GFNT_LineAnnotation & merged = para.layout.lines[0].annotations[0];
  EXPECT_EQ(merged.base_start, 0u);
  EXPECT_EQ(merged.base_end, 2u);
  ASSERT_EQ(merged.run_count, 1u);
  EXPECT_EQ(merged.runs[0].glyph_count, 2u);
}

TEST(LayoutAnnotation, EmphasisAddsOneMarkPerEligibleCluster) {
  Font font({'A', 'B', 'C', 'D', 'E', 'F', ' ', 0x2022}, true);
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation emphasis{};
  emphasis.base_start = 0;
  emphasis.base_end = 5;
  emphasis.kind = GFNT_ANNOTATION_EMPHASIS;
  emphasis.position = GFNT_ANNOTATION_OVER;
  emphasis.style.faces = faces;
  emphasis.style.face_count = 1;
  emphasis.style.size = 2 * kSize;
  emphasis.mark_kind = GFNT_EMPHASIS_MARK_DOT;
  emphasis.filled = true;
  GFNT_ParagraphOptions options{};
  options.annotations = &emphasis;
  options.annotation_count = 1;
  Para para("A B C", {font.face}, options);
  Para plain("A B C", {font.face});

  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(plain.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(plain.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_EQ(line.annotation_count, 3u);
  EXPECT_GT(line.ascent, 512);
  EXPECT_EQ(line.annotations[0].base_start, 0u);
  EXPECT_EQ(line.annotations[1].base_start, 2u);
  EXPECT_EQ(line.annotations[2].base_start, 4u);
  EXPECT_EQ(line.annotations[0].kind, GFNT_ANNOTATION_EMPHASIS);
  EXPECT_EQ(para.layout.ink.x, line.ink.x);
  EXPECT_EQ(para.layout.ink.y, line.ink.y);
  EXPECT_EQ(para.layout.ink.width, line.ink.width);
  EXPECT_EQ(para.layout.ink.height, line.ink.height);
  EXPECT_GT(para.layout.ink.height, plain.layout.ink.height)
      << "larger emphasis glyphs must contribute to aggregate ink bounds";
}

TEST(LayoutAnnotation, DefaultEmphasisSelectsMarkPerVerticalScriptCluster) {
  Font font({'A', 0x4E00, 0x2022, 0xFE45});
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation emphasis{};
  emphasis.base_start = 0;
  emphasis.base_end = 4;
  emphasis.kind = GFNT_ANNOTATION_EMPHASIS;
  emphasis.position = GFNT_ANNOTATION_OVER;
  emphasis.style.faces = faces;
  emphasis.style.face_count = 1;
  emphasis.style.size = kSize;
  emphasis.mark_kind = GFNT_EMPHASIS_MARK_DEFAULT;
  GFNT_ParagraphOptions options{};
  options.writing_mode = GFNT_WRITING_VERTICAL_RL;
  options.annotations = &emphasis;
  options.annotation_count = 1;
  Para para("A中", {font.face}, options);

  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_EQ(line.annotation_count, 2u);
  ASSERT_EQ(line.annotations[0].run_count, 1u);
  ASSERT_EQ(line.annotations[1].run_count, 1u);
  ASSERT_EQ(line.annotations[0].runs[0].glyph_count, 1u);
  ASSERT_EQ(line.annotations[1].runs[0].glyph_count, 1u);
  EXPECT_EQ(line.annotations[0].runs[0].glyphs[0].glyph, 3u)
      << "the Latin cluster must use the configured filled dot glyph";
  EXPECT_EQ(line.annotations[1].runs[0].glyphs[0].glyph, 4u)
      << "the vertical CJK cluster must use the configured filled sesame glyph";
}

TEST(LayoutAnnotation, RubyAutoOverhangStaysWithinTheRenderedLine) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation ruby{};
  ruby.base_start = 0;
  ruby.base_end = 2;
  ruby.kind = GFNT_ANNOTATION_RUBY;
  ruby.position = GFNT_ANNOTATION_OVER;
  ruby.style.faces = faces;
  ruby.style.face_count = 1;
  ruby.style.size = kSize;
  ruby.text = "ABC";
  ruby.text_length = 3;
  ruby.align = GFNT_ANNOTATION_ALIGN_CENTER;
  ruby.overhang = GFNT_RUBY_OVERHANG_AUTO;
  GFNT_ParagraphOptions options{};
  options.annotations = &ruby;
  options.annotation_count = 1;
  Para para("AB C", {font.face}, options);

  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(7 * kLetter / 2), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[0].annotation_count, 1u);
  const GFNT_Rect ruby_box = para.layout.lines[0].annotations[0].box;
  const GFNT_Rect line_box = para.layout.lines[0].box;
  EXPECT_GE(ruby_box.x, line_box.x);
  EXPECT_LE(ruby_box.x + ruby_box.width, line_box.x + line_box.width);
}

TEST(LayoutAnnotation, RubyAutoOverhangReclaimsAdvanceAndPullsFollowingTextUp) {
  Font font({0x4E00, 0x4E8C});
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation ruby{};
  ruby.base_start = 0;
  ruby.base_end = 3;
  ruby.kind = GFNT_ANNOTATION_RUBY;
  ruby.position = GFNT_ANNOTATION_OVER;
  ruby.style.faces = faces;
  ruby.style.face_count = 1;
  ruby.style.size = kSize;
  ruby.text = "二二";
  ruby.text_length = 6;
  ruby.align = GFNT_ANNOTATION_ALIGN_CENTER;
  ruby.overhang = GFNT_RUBY_OVERHANG_AUTO;
  GFNT_ParagraphOptions options{};
  options.annotations = &ruby;
  options.annotation_count = 1;
  Para para("一二", {font.face}, options);

  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(700), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_EQ(line.end, 6u);
  EXPECT_EQ(line.extent, 2 * kLetter);
  ASSERT_EQ(line.run_count, 1u);
  ASSERT_EQ(line.runs[0].glyph_count, 2u);
  EXPECT_EQ(line.runs[0].glyphs[0].pen_x, 0);
  EXPECT_EQ(line.runs[0].glyphs[1].pen_x, kLetter);
  ASSERT_EQ(line.annotation_count, 1u);
  EXPECT_EQ(line.annotations[0].box.width, 2 * kLetter);
}

TEST(LayoutAnnotation, RubyAutoOverhangReclaimsAdvanceFromVisualLeftInRtl) {
  Font font({0x05D0, 0x05D1});
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation ruby{};
  ruby.base_start = 0;
  ruby.base_end = 2;
  ruby.kind = GFNT_ANNOTATION_RUBY;
  ruby.position = GFNT_ANNOTATION_OVER;
  ruby.style.faces = faces;
  ruby.style.face_count = 1;
  ruby.style.size = kSize;
  ruby.text = "אב";
  ruby.text_length = 4;
  ruby.align = GFNT_ANNOTATION_ALIGN_CENTER;
  ruby.overhang = GFNT_RUBY_OVERHANG_AUTO;
  GFNT_ParagraphOptions options{};
  options.direction = GFNT_BIDI_RTL;
  options.annotations = &ruby;
  options.annotation_count = 1;
  Para para("אב", {font.face}, options);

  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(700), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_EQ(line.end, 4u);
  EXPECT_EQ(line.extent, 2 * kLetter);
  ASSERT_EQ(line.run_count, 1u);
  ASSERT_EQ(line.runs[0].glyph_count, 2u);
  const GFNT_PlacedGlyph * base_glyph = nullptr;
  const GFNT_PlacedGlyph * following_glyph = nullptr;
  for (size_t i = 0; i < line.runs[0].glyph_count; i++) {
    const GFNT_PlacedGlyph & glyph = line.runs[0].glyphs[i];
    if (glyph.cluster == 0) base_glyph = &glyph;
    else if (glyph.cluster == 2) following_glyph = &glyph;
  }
  ASSERT_NE(base_glyph, nullptr);
  ASSERT_NE(following_glyph, nullptr);
  EXPECT_EQ(following_glyph->pen_x + following_glyph->advance, base_glyph->pen_x);
  ASSERT_EQ(line.annotation_count, 1u);
  EXPECT_EQ(line.annotations[0].box.width, 2 * kLetter);
}

TEST(LayoutAnnotation, RubyAutoOverhangUsesAdjacentMixedBidiText) {
  Font font(latin_and_hebrew());
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation ruby{};
  ruby.base_start = 0;
  ruby.base_end = 4;
  ruby.kind = GFNT_ANNOTATION_RUBY;
  ruby.position = GFNT_ANNOTATION_OVER;
  ruby.style.faces = faces;
  ruby.style.face_count = 1;
  ruby.style.size = kSize;
  ruby.text = "אבאב";
  ruby.text_length = 8;
  ruby.align = GFNT_ANNOTATION_ALIGN_CENTER;
  ruby.overhang = GFNT_RUBY_OVERHANG_AUTO;
  GFNT_ParagraphOptions options{};
  options.direction = GFNT_BIDI_RTL;
  options.annotations = &ruby;
  options.annotation_count = 1;
  // The LTR A is logically after the Hebrew ruby base and visually adjacent to its left edge.
  Para para("אבAג", {font.face}, options);

  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_GE(line.run_count, 2u);
  ASSERT_EQ(line.annotation_count, 1u);
  EXPECT_EQ(line.extent, 4 * kLetter);

  const GFNT_PlacedGlyph * a_glyph = nullptr;
  const GFNT_PlacedGlyph * base_edge_glyph = nullptr;
  for (size_t ri = 0; ri < line.run_count; ri++) {
    for (size_t gi = 0; gi < line.runs[ri].glyph_count; gi++) {
      const GFNT_PlacedGlyph & glyph = line.runs[ri].glyphs[gi];
      if (glyph.cluster == 4) a_glyph = &glyph;
      if (glyph.cluster == 2) base_edge_glyph = &glyph;
    }
  }
  ASSERT_NE(a_glyph, nullptr);
  ASSERT_NE(base_edge_glyph, nullptr);
  EXPECT_EQ(a_glyph->pen_x + a_glyph->advance, base_edge_glyph->pen_x)
      << "logical successor must remain the visual neighbor at this bidi boundary";
  EXPECT_EQ(line.annotations[0].box.x, a_glyph->pen_x + a_glyph->advance)
      << "the over-wide ruby stays attached to its adjacent visual neighbor";
  EXPECT_EQ(line.annotations[0].box.width, line.box.width);
  EXPECT_GT(line.annotations[0].box.x + line.annotations[0].box.width,
      line.box.x + line.box.width);
}

TEST(LayoutAnnotation, RubyAutoOverhangReservesTabAdvanceWithoutRejectingLayout) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation ruby{};
  ruby.base_start = 1;
  ruby.base_end = 2;
  ruby.kind = GFNT_ANNOTATION_RUBY;
  ruby.position = GFNT_ANNOTATION_OVER;
  ruby.style.faces = faces;
  ruby.style.face_count = 1;
  ruby.style.size = kSize;
  ruby.text = "AB";
  ruby.text_length = 2;
  ruby.align = GFNT_ANNOTATION_ALIGN_CENTER;
  ruby.overhang = GFNT_RUBY_OVERHANG_AUTO;
  GFNT_ParagraphOptions options{};
  options.tab_width = kLetter;
  options.annotations = &ruby;
  options.annotation_count = 1;
  Para para("A\tB", {font.face}, options);

  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(1600), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_EQ(para.layout.lines[0].end, 3u);
  EXPECT_EQ(para.layout.lines[0].extent, 3 * kLetter);
}

TEST(LayoutAnnotation, RejectsRangesInsideUtf8AndOverlappingSiblings) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation ruby{};
  ruby.base_start = 1;
  ruby.base_end = 2;
  ruby.kind = GFNT_ANNOTATION_RUBY;
  ruby.position = GFNT_ANNOTATION_OVER;
  ruby.style.faces = faces;
  ruby.style.face_count = 1;
  ruby.style.size = kSize;
  ruby.text = "A";
  ruby.text_length = 1;
  GFNT_ParagraphOptions options{};
  options.annotations = &ruby;
  options.annotation_count = 1;
  Para split("éA", {font.face}, options);
  EXPECT_EQ(split.created, GFNT_ERR_INVALID);

  GFNT_Annotation siblings[2] = {ruby, ruby};
  siblings[0].base_start = 0;
  siblings[0].base_end = 2;
  siblings[1].base_start = 1;
  siblings[1].base_end = 3;
  options.annotations = siblings;
  options.annotation_count = 2;
  Para overlap("ABC", {font.face}, options);
  EXPECT_EQ(overlap.created, GFNT_ERR_INVALID);

  siblings[0] = ruby;
  siblings[0].base_start = 0;
  siblings[0].base_end = 1;
  siblings[0].merge = true;
  options.annotations = siblings;
  options.annotation_count = 1;
  Para unsupported_merge("ABC", {font.face}, options);
  EXPECT_EQ(unsupported_merge.created, GFNT_ERR_INVALID);

  siblings[0].merge = false;
  siblings[0].position = GFNT_ANNOTATION_INTER_CHARACTER;
  Para unsupported_position("ABC", {font.face}, options);
  EXPECT_EQ(unsupported_position.created, GFNT_ERR_INVALID);

  GFNT_Annotation child = ruby;
  child.base_start = 0;
  child.base_end = 3;
  child.text = "A";
  child.text_length = 1;
  GFNT_Annotation grandchild = child;
  grandchild.base_start = 0;
  grandchild.base_end = 1;
  grandchild.text = "B";
  grandchild.position = GFNT_ANNOTATION_UNDER;
  child.children = &grandchild;
  child.child_count = 1;
  siblings[0].merge = false;
  siblings[0].position = GFNT_ANNOTATION_OVER;
  siblings[0].group_break = false;
  siblings[0].base_start = 0;
  siblings[0].base_end = 3;
  siblings[0].text = "ABC";
  siblings[0].text_length = 3;
  siblings[0].children = &child;
  siblings[0].child_count = 1;
  Para breakable_nested("A B", {font.face}, options);
  ASSERT_EQ(breakable_nested.created, GFNT_OK);
  grandchild.text = nullptr;
  child.children = nullptr;
  child.text = nullptr;
  siblings[0].children = nullptr;
  siblings[0].style.faces = nullptr;
  ASSERT_EQ(breakable_nested.lay(kLetter), GFNT_OK);
  ASSERT_EQ(breakable_nested.layout.line_count, 2u);
  ASSERT_EQ(breakable_nested.layout.lines[0].annotation_count, 1u);
  EXPECT_EQ(breakable_nested.layout.lines[0].annotations[0].child_count, 0u);
  ASSERT_EQ(breakable_nested.layout.lines[1].annotation_count, 1u);
  ASSERT_EQ(breakable_nested.layout.lines[1].annotations[0].child_count, 1u);
  EXPECT_EQ(breakable_nested.layout.lines[1].annotations[0].children[0].base_start, 0u);
  EXPECT_EQ(breakable_nested.layout.lines[1].annotations[0].children[0].base_end, 1u);
  ASSERT_EQ(breakable_nested.layout.lines[1].annotations[0].children[0].run_count, 1u);
  ASSERT_EQ(breakable_nested.layout.lines[1].annotations[0].children[0].runs[0].glyph_count, 1u);
  EXPECT_EQ(breakable_nested.layout.lines[1].annotations[0].children[0].runs[0].glyphs[0].glyph, 1u);
  ASSERT_EQ(breakable_nested.layout.lines[1].annotations[0].children[0].child_count, 1u);
  EXPECT_EQ(breakable_nested.layout.lines[1].annotations[0].children[0].children[0].base_start, 0u);
  EXPECT_EQ(breakable_nested.layout.lines[1].annotations[0].children[0].children[0].base_end, 1u);
  ASSERT_EQ(breakable_nested.layout.lines[1].annotations[0].children[0].children[0].run_count, 1u);
  ASSERT_EQ(breakable_nested.layout.lines[1].annotations[0].children[0].children[0].runs[0].glyph_count, 1u);
  EXPECT_EQ(breakable_nested.layout.lines[1].annotations[0].children[0].children[0].runs[0].glyphs[0].glyph, 2u);
}

TEST(LayoutAnnotation, DoubleSidedRubyAndVerticalTracksUseOppositeSides) {
  Font font({0x65e5, 0x672c, 0x306b, 0x307b, 0x3093, 0x006e, 0x0069, 0x0068, 0x006f});
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation tracks[3]{};
  for (size_t i = 0; i < 3; i++) {
    tracks[i].base_start = 0;
    tracks[i].base_end = 6;
    tracks[i].kind = GFNT_ANNOTATION_RUBY;
    tracks[i].position = i == 1 ? GFNT_ANNOTATION_UNDER : GFNT_ANNOTATION_OVER;
    tracks[i].style.faces = faces;
    tracks[i].style.face_count = 1;
    tracks[i].style.size = kSize;
    tracks[i].text = i == 1 ? "nihon" : i == 2 ? "に" : "にほん";
    tracks[i].text_length = i == 1 ? 5 : i == 2 ? 3 : 9;
    tracks[i].align = GFNT_ANNOTATION_ALIGN_CENTER;
    tracks[i].group_break = true;
  }

  GFNT_ParagraphOptions options{};
  options.annotations = tracks;
  options.annotation_count = 3;
  Para horizontal("日本", {font.face}, options);
  ASSERT_EQ(horizontal.created, GFNT_OK);
  ASSERT_EQ(horizontal.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(horizontal.layout.lines[0].annotation_count, 3u);
  GFNT_Rect base{};
  size_t rect_count = 0;
  ASSERT_EQ(gfnt_layout_range_rects(horizontal.p, &horizontal.layout, 0, 6,
      &base, 1, &rect_count), GFNT_OK);
  ASSERT_EQ(rect_count, 1u);
  EXPECT_LE(horizontal.layout.lines[0].annotations[0].box.y
      + horizontal.layout.lines[0].annotations[0].box.height, base.y);
  EXPECT_GE(horizontal.layout.lines[0].annotations[1].box.y, base.y + base.height);
  EXPECT_LE(horizontal.layout.lines[0].annotations[2].box.y
      + horizontal.layout.lines[0].annotations[2].box.height,
      horizontal.layout.lines[0].annotations[0].box.y);

  for (GFNT_WritingMode mode : {GFNT_WRITING_VERTICAL_RL, GFNT_WRITING_VERTICAL_LR}) {
    options.writing_mode = mode;
    Para vertical("日本", {font.face}, options);
    ASSERT_EQ(vertical.created, GFNT_OK);
    ASSERT_EQ(vertical.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
    ASSERT_EQ(vertical.layout.lines[0].annotation_count, 3u);
    rect_count = 0;
    ASSERT_EQ(gfnt_layout_range_rects(vertical.p, &vertical.layout, 0, 6,
        &base, 1, &rect_count), GFNT_OK);
    ASSERT_EQ(rect_count, 1u);
    EXPECT_GE(vertical.layout.lines[0].annotations[2].box.x,
        base.x + base.width);
    EXPECT_LE(vertical.layout.lines[0].annotations[1].box.x
        + vertical.layout.lines[0].annotations[1].box.width, base.x);
    EXPECT_LE(vertical.layout.lines[0].annotations[0].box.x
        + vertical.layout.lines[0].annotations[0].box.width,
        vertical.layout.lines[0].annotations[2].box.x);
  }
}

TEST(LayoutAnnotation, WrappedSameSideRubyStacksOnlyEmittedFragments) {
  Font font({0x65e5, 0x672c, 0x8a9e, 0x306b, 0x307b, 0x3093});
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation tracks[2]{};
  for (auto & track : tracks) {
    track.base_start = 0;
    track.base_end = 9;
    track.kind = GFNT_ANNOTATION_RUBY;
    track.position = GFNT_ANNOTATION_OVER;
    track.style.faces = faces;
    track.style.face_count = 1;
    track.style.size = kSize;
    track.align = GFNT_ANNOTATION_ALIGN_CENTER;
    track.group_break = false;
  }
  tracks[0].text = "にほん";
  tracks[0].text_length = 9;
  tracks[1].text = "日本";
  tracks[1].text_length = 6;
  GFNT_ParagraphOptions options{};
  options.annotations = tracks;
  options.annotation_count = 2;
  for (GFNT_WritingMode mode : {GFNT_WRITING_HORIZONTAL_TB,
          GFNT_WRITING_VERTICAL_RL, GFNT_WRITING_VERTICAL_LR}) {
    options.writing_mode = mode;
    Para para("日本語", {font.face}, options);
    ASSERT_EQ(para.created, GFNT_OK);
    ASSERT_EQ(para.lay(kLetter), GFNT_OK);
    ASSERT_EQ(para.layout.line_count, 3u);
    ASSERT_EQ(para.layout.lines[0].annotation_count, 1u);
    ASSERT_EQ(para.layout.lines[1].annotation_count, 2u);
    ASSERT_EQ(para.layout.lines[2].annotation_count, 2u);
    for (size_t li = 1; li < 3; li++) {
      const GFNT_Rect first = para.layout.lines[li].annotations[0].box;
      const GFNT_Rect second = para.layout.lines[li].annotations[1].box;
      if (mode == GFNT_WRITING_HORIZONTAL_TB) {
        EXPECT_TRUE(first.y + first.height <= second.y
            || second.y + second.height <= first.y);
      }
      else {
        EXPECT_TRUE(first.x + first.width <= second.x
            || second.x + second.width <= first.x);
      }
    }
  }
}

TEST(LayoutFlow, FullWidthBandMatchesTheExistingLayout) {
  Font font(latin());
  Para para("AB CD EF", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_Layout legacy{};
  GFNT_Layout flow{};
  ASSERT_EQ(gfnt_paragraph_layout(para.p, 800, &legacy, nullptr), GFNT_OK);
  FlowBands context;
  context.offset = false;
  GFNT_LineConstraint constraint{&context, flow_band};
  ASSERT_EQ(gfnt_paragraph_layout_flow(para.p, &constraint, &flow, nullptr), GFNT_OK);
  ASSERT_EQ(flow.line_count, legacy.line_count);
  EXPECT_EQ(flow.width, 800);
  EXPECT_EQ(flow.height, legacy.height);
  for (size_t li = 0; li < flow.line_count; ++li) {
    const GFNT_Line & actual = flow.lines[li];
    const GFNT_Line & expected = legacy.lines[li];
    EXPECT_EQ(actual.start, expected.start);
    EXPECT_EQ(actual.end, expected.end);
    EXPECT_EQ(actual.extent, expected.extent);
    EXPECT_EQ(actual.baseline, expected.baseline);
    EXPECT_EQ(actual.box.height, expected.box.height);
    ASSERT_EQ(actual.run_count, expected.run_count);
    ASSERT_EQ(actual.runs[0].glyph_count, expected.runs[0].glyph_count);
    for (size_t g = 0; g < actual.runs[0].glyph_count; ++g) {
      EXPECT_EQ(actual.runs[0].glyphs[g].pen_x, expected.runs[0].glyphs[g].pen_x);
      EXPECT_EQ(actual.runs[0].glyphs[g].pen_y, expected.runs[0].glyphs[g].pen_y);
    }
  }
  gfnt_layout_free(&flow);
  gfnt_layout_free(&legacy);
}

TEST(LayoutFlow, TextWrapBalanceUsesRepeatSafeCallbackBands) {
  Font font(latin());
  GFNT_ParagraphOptions balance_options{};
  balance_options.text_wrap_style = GFNT_TEXT_WRAP_BALANCE;
  const GFNT_F26Dot6 width = 6 * kLetter + 2 * kSpace;
  Para greedy("AB AB AB AB", {font.face});
  Para balanced("AB AB AB AB", {font.face}, balance_options);
  GFNT_Layout flow{};
  ASSERT_EQ(gfnt_paragraph_layout(greedy.p, width, &greedy.layout, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(balanced.p, width, &balanced.layout, nullptr), GFNT_OK);
  FlowBands context;
  context.offset = false;
  context.width = width;
  GFNT_LineConstraint constraint{&context, flow_band};

  ASSERT_EQ(gfnt_paragraph_layout_flow(balanced.p, &constraint, &flow, nullptr), GFNT_OK);
  ASSERT_EQ(flow.line_count, balanced.layout.line_count);
  ASSERT_EQ(flow.line_count, greedy.layout.line_count);
  EXPECT_NE(flow.lines[0].end, greedy.layout.lines[0].end);
  for (size_t i = 0; i < flow.line_count; i++) {
    EXPECT_EQ(flow.lines[i].start, balanced.layout.lines[i].start);
    EXPECT_EQ(flow.lines[i].end, balanced.layout.lines[i].end);
    EXPECT_EQ(flow.lines[i].extent, balanced.layout.lines[i].extent);
  }
  gfnt_layout_free(&flow);
}

TEST(LayoutFlow, TextWrapBalanceKeepsForcedBreakGroupsIndependent) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.text_wrap_style = GFNT_TEXT_WRAP_BALANCE;
  const GFNT_F26Dot6 width = 6 * kLetter + 2 * kSpace;
  Para greedy("AB AB AB AB\nAB AB AB AB", {font.face});
  Para balanced("AB AB AB AB\nAB AB AB AB", {font.face}, options);
  GFNT_Layout flow{};
  ASSERT_EQ(gfnt_paragraph_layout(greedy.p, width, &greedy.layout, nullptr), GFNT_OK);
  FlowBands context;
  context.offset = false;
  context.width = width;
  GFNT_LineConstraint constraint{&context, flow_band};

  ASSERT_EQ(gfnt_paragraph_layout_flow(balanced.p, &constraint, &flow, nullptr), GFNT_OK);
  ASSERT_EQ(flow.line_count, greedy.layout.line_count);
  ASSERT_EQ(flow.line_count, 4u);
  EXPECT_NE(flow.lines[0].end, greedy.layout.lines[0].end);
  EXPECT_NE(flow.lines[2].end, greedy.layout.lines[2].end);
  EXPECT_EQ(flow.lines[2].start, 12u);
  gfnt_layout_free(&flow);
}

TEST(LayoutFlow, TextWrapBalanceRetriesHypotheticalLinesAtNextTop) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.text_wrap_style = GFNT_TEXT_WRAP_BALANCE;
  Para para("A AB AB AB", {font.face}, options);
  GFNT_Layout flow{};
  FlowBands context;
  context.offset = false;
  context.width = 2240;
  context.clear_on_height = true;
  GFNT_LineConstraint constraint{&context, flow_band};

  ASSERT_EQ(gfnt_paragraph_layout_flow(para.p, &constraint, &flow, nullptr), GFNT_OK);
  EXPECT_TRUE(context.saw_clear_retry);
  gfnt_layout_free(&flow);
}

TEST(LayoutFlow, TextWrapPrettyUsesRepeatSafeCallbackBands) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.text_wrap_style = GFNT_TEXT_WRAP_PRETTY;
  const GFNT_F26Dot6 width = 6 * kLetter + 2 * kSpace;
  Para greedy("AB AB AB AB", {font.face});
  Para pretty("AB AB AB AB", {font.face}, options);
  GFNT_Layout flow{};
  ASSERT_EQ(gfnt_paragraph_layout(greedy.p, width, &greedy.layout, nullptr), GFNT_OK);
  FlowBands context;
  context.offset = false;
  context.width = width;
  GFNT_LineConstraint constraint{&context, flow_band};

  ASSERT_EQ(gfnt_paragraph_layout_flow(pretty.p, &constraint, &flow, nullptr), GFNT_OK);
  ASSERT_EQ(flow.line_count, greedy.layout.line_count);
  EXPECT_NE(flow.lines[0].end, greedy.layout.lines[0].end);
  gfnt_layout_free(&flow);
}

TEST(LayoutFlow, AvoidShortLastLineUsesRepeatSafeCallbackBands) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.text_wrap_style = GFNT_TEXT_WRAP_AVOID_SHORT_LAST_LINE;
  const GFNT_F26Dot6 width = 6 * kLetter + 2 * kSpace;
  Para para("AB AB AB AB", {font.face}, options);
  GFNT_Layout flow{};
  ASSERT_EQ(para.created, GFNT_OK);
  FlowBands context;
  context.offset = false;
  context.width = width;
  GFNT_LineConstraint constraint{&context, flow_band};

  ASSERT_EQ(gfnt_paragraph_layout_flow(para.p, &constraint, &flow, nullptr), GFNT_OK);
  ASSERT_EQ(flow.line_count, 2u);
  EXPECT_EQ(flow.lines[0].end, 6u);
  EXPECT_EQ(flow.lines[1].start, 6u);
  EXPECT_EQ(flow.lines[1].extent, 4 * kLetter + kSpace);
  gfnt_layout_free(&flow);
}

TEST(LayoutFlow, AvoidShortLastLineBoundsSpeculativeRetries) {
  constexpr size_t word_count = 1000;
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.text_wrap_style = GFNT_TEXT_WRAP_AVOID_SHORT_LAST_LINE;
  std::string text;
  text.reserve(word_count * 3);
  for (size_t i = 0; i < word_count; i++) {
    if (i) text.push_back(' ');
    text += "AB";
  }
  Para para(text, {font.face}, options);
  GFNT_Layout flow{};
  FlowBands context;
  context.offset = false;
  context.width = static_cast<GFNT_F26Dot6>(800 * (word_count - 1));
  context.shrink_by_height = true;
  GFNT_LineConstraint constraint{&context, flow_band};

  ASSERT_EQ(gfnt_paragraph_layout_flow(para.p, &constraint, &flow, nullptr), GFNT_OK);
  ASSERT_EQ(flow.line_count, 2u);
  EXPECT_LE(context.calls, 520u);
  gfnt_layout_free(&flow);
}

TEST(LayoutFlow, BandOffsetMovesLineBoxAndGlyphs) {
  Font font(latin());
  Para para("AB CD", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  FlowBands context;
  GFNT_LineConstraint constraint{&context, flow_band};
  ASSERT_EQ(gfnt_paragraph_layout_flow(para.p, &constraint, &para.layout, nullptr), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[0].box.x, 64);
  EXPECT_EQ(para.layout.lines[1].box.x, 128);
  EXPECT_EQ(para.layout.lines[0].runs[0].glyphs[0].pen_x, 64);
  EXPECT_EQ(para.layout.lines[1].runs[0].glyphs[0].pen_x, 128);
  GFNT_Hit hit{};
  GFNT_Caret caret{};
  EXPECT_EQ(gfnt_layout_hit_test(para.p, &para.layout, 70, 10, &hit), GFNT_OK);
  EXPECT_EQ(gfnt_layout_caret(para.p, &para.layout, hit.offset, hit.affinity, &caret), GFNT_OK);
  size_t rect_count = 0;
  EXPECT_EQ(gfnt_layout_range_rects(para.p, &para.layout, 0, 2, nullptr, 0, &rect_count), GFNT_OK);
  EXPECT_GT(rect_count, 0u);
}

TEST(LayoutFlow, NegativeBandStartFlowsThroughPlacementAndInteractionGeometry) {
  Font font(latin());
  Para para("AB", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_LineConstraint constraint{nullptr, negative_start_band};
  ASSERT_EQ(gfnt_paragraph_layout_flow(para.p, &constraint, &para.layout, nullptr), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_EQ(line.box.x, -128);
  ASSERT_GT(line.run_count, 0u);
  ASSERT_GT(line.runs[0].glyph_count, 0u);
  EXPECT_EQ(line.runs[0].glyphs[0].pen_x, -128);
  GFNT_Hit hit{};
  ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, -100, line.baseline, &hit), GFNT_OK);
  EXPECT_EQ(hit.offset, 0u);
  GFNT_Caret caret{};
  ASSERT_EQ(gfnt_layout_caret(para.p, &para.layout, 0, GFNT_AFFINITY_DOWNSTREAM, &caret),
      GFNT_OK);
  EXPECT_EQ(caret.x, -128);
  GFNT_Rect rect{};
  size_t rect_count = 0;
  ASSERT_EQ(gfnt_layout_range_rects(para.p, &para.layout, 0, 1, &rect, 1, &rect_count),
      GFNT_OK);
  ASSERT_EQ(rect_count, 1u);
  EXPECT_EQ(rect.x, -128);
}

TEST(LayoutFlow, RebreaksWhenMeasuredHeightChangesTheBand) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.line_break = GFNT_LINE_BREAK_ANYWHERE;
  Para para("AB CD EF", {font.face}, options);
  ASSERT_EQ(para.created, GFNT_OK);
  FlowBands context;
  context.retry_by_height = true;
  GFNT_LineConstraint constraint{&context, flow_band};
  ASSERT_EQ(gfnt_paragraph_layout_flow(para.p, &constraint, &para.layout, nullptr), GFNT_OK);
  ASSERT_GE(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[0].end, 1u);
}

TEST(LayoutFlow, ClearsAnUnbreakableStretchToTheNextBand) {
  Font font(latin());
  Para para("AB", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  FlowBands context;
  context.clear = true;
  GFNT_LineConstraint constraint{&context, flow_band};
  ASSERT_EQ(gfnt_paragraph_layout_flow(para.p, &constraint, &para.layout, nullptr), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_EQ(para.layout.lines[0].box.y, 1000);
  EXPECT_EQ(para.layout.lines[0].extent, 2 * kLetter);
}

TEST(LayoutFlow, FirstAndHangingIndentsChangeTheBandsAndBreaks) {
  Font font(latin());
  GFNT_ParagraphOptions first{};
  first.text_indent = 200;
  Para para("A B C D E", {font.face}, first);
  ASSERT_EQ(para.created, GFNT_OK);
  FlowBands context;
  context.offset = false;
  GFNT_LineConstraint constraint{&context, flow_band};
  ASSERT_EQ(gfnt_paragraph_layout_flow(para.p, &constraint, &para.layout, nullptr), GFNT_OK);
  ASSERT_GE(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[0].box.x, 200);
  EXPECT_EQ(para.layout.lines[0].box.width, 600);
  EXPECT_EQ(para.layout.lines[1].box.x, 0);
  GFNT_ParagraphOptions fixed_first{};
  fixed_first.text_indent = 200;
  Para fixed("A B C D E", {font.face}, fixed_first);
  ASSERT_EQ(fixed.lay(800), GFNT_OK);
  ASSERT_GE(fixed.layout.line_count, 2u);
  EXPECT_EQ(fixed.layout.lines[0].box.x, 200);
  EXPECT_EQ(fixed.layout.lines[0].box.width, 600);
  EXPECT_EQ(fixed.layout.lines[1].box.x, 0);

  GFNT_ParagraphOptions hanging{};
  hanging.text_indent = 200;
  hanging.hanging_indent = true;
  Para hung("A B C D E", {font.face}, hanging);
  ASSERT_EQ(hung.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout_flow(hung.p, &constraint, &hung.layout, nullptr), GFNT_OK);
  ASSERT_GE(hung.layout.line_count, 2u);
  EXPECT_EQ(hung.layout.lines[0].box.x, 0);
  EXPECT_EQ(hung.layout.lines[1].box.x, 200);
  EXPECT_EQ(hung.layout.lines[1].box.width, 600);
}

TEST(LayoutFlow, ZeroWidthWithoutALaterBandOverflowsOneStretch) {
  Font font(latin());
  Para para("AB CD", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  FlowBands context;
  context.zero_width = true;
  context.offset = false;
  GFNT_LineConstraint constraint{&context, flow_band};
  ASSERT_EQ(gfnt_paragraph_layout_flow(para.p, &constraint, &para.layout, nullptr), GFNT_OK);
  ASSERT_GE(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[0].end, 3u); // The trailing space hangs on the line.
  EXPECT_EQ(para.layout.lines[0].extent, 2 * kLetter);
}

TEST(LayoutFlow, VerticalBandsUseTheAlongLineAxis) {
  Font font(latin());
  for (GFNT_WritingMode mode : {GFNT_WRITING_VERTICAL_RL, GFNT_WRITING_VERTICAL_LR,
           GFNT_WRITING_SIDEWAYS_RL, GFNT_WRITING_SIDEWAYS_LR}) {
    GFNT_ParagraphOptions options{};
    options.writing_mode = mode;
    Para para("AB", {font.face}, options);
    ASSERT_EQ(para.created, GFNT_OK);
    FlowBands context;
    GFNT_LineConstraint constraint{&context, flow_band};
    ASSERT_EQ(gfnt_paragraph_layout_flow(para.p, &constraint, &para.layout, nullptr), GFNT_OK);
    ASSERT_EQ(para.layout.line_count, 1u);
    EXPECT_EQ(para.layout.lines[0].box.height, 800);
    const GFNT_PlacedGlyph & glyph = para.layout.lines[0].runs[0].glyphs[0];
    EXPECT_GE(glyph.pen_y, para.layout.lines[0].box.y);
    EXPECT_LE(glyph.pen_y, para.layout.lines[0].box.y + para.layout.lines[0].box.height);
  }
}

TEST(LayoutFlow, PropagatesCallbackErrorsAndRejectsInvalidBands) {
  Font font(latin());
  Para para("AB", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_Layout layout{};
  FlowBands context;
  context.result = GFNT_ERR_OOM;
  GFNT_LineConstraint constraint{&context, flow_band};
  EXPECT_EQ(gfnt_paragraph_layout_flow(para.p, &constraint, &layout, nullptr), GFNT_ERR_OOM);
  EXPECT_EQ(layout.lines, nullptr);
  context.result = GFNT_OK;
  context.invalid = true;
  EXPECT_EQ(gfnt_paragraph_layout_flow(para.p, &constraint, &layout, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(layout.lines, nullptr);
}

TEST(LayoutFlow, BuiltInExclusionsSupportRectanglesCirclesAndPolygons) {
  GFNT_Exclusion exclusion{};
  GFNT_Exclusions exclusions{&exclusion, 1, 0, 300};
  GFNT_Band band{};

  exclusion.shape = GFNT_EXCLUSION_RECTANGLE;
  exclusion.side = GFNT_EXCLUSION_TEXT_RIGHT;
  exclusion.margin = 5;
  exclusion.geometry.rectangle = {10, 10, 20, 30};
  ASSERT_EQ(gfnt_exclusions_band(&exclusions, 15, 10, &band), GFNT_OK);
  EXPECT_EQ(band.start, 35);
  EXPECT_EQ(band.width, 265);
  EXPECT_EQ(band.next_top, 45);

  exclusion = {};
  exclusion.shape = GFNT_EXCLUSION_CIRCLE;
  exclusion.side = GFNT_EXCLUSION_TEXT_LEFT;
  exclusion.geometry.circle = {{100, 50}, 20};
  ASSERT_EQ(gfnt_exclusions_band(&exclusions, 50, 0, &band), GFNT_OK);
  EXPECT_EQ(band.start, 0);
  EXPECT_EQ(band.width, 80);

  const GFNT_FlowPoint points[] = {{0, 0}, {100, 0}, {50, 100}};
  exclusion = {};
  exclusion.shape = GFNT_EXCLUSION_POLYGON;
  exclusion.side = GFNT_EXCLUSION_TEXT_RIGHT;
  exclusion.geometry.polygon = {points, 3};
  ASSERT_EQ(gfnt_exclusions_band(&exclusions, 50, 0, &band), GFNT_OK);
  EXPECT_EQ(band.start, 75);
  EXPECT_EQ(band.width, 225);
  EXPECT_EQ(band.next_top, 100);
}

TEST(LayoutFlow, BuiltInExclusionsRechecksAtNearestBlockerBoundary) {
  GFNT_Exclusion items[2]{};
  items[0].shape = GFNT_EXCLUSION_RECTANGLE;
  items[0].side = GFNT_EXCLUSION_TEXT_RIGHT;
  items[0].geometry.rectangle = {10, 10, 20, 10};
  items[1].shape = GFNT_EXCLUSION_RECTANGLE;
  items[1].side = GFNT_EXCLUSION_TEXT_RIGHT;
  items[1].geometry.rectangle = {30, 10, 20, 30};
  GFNT_Exclusions exclusions{items, 2, 0, 300};
  GFNT_Band band{};

  ASSERT_EQ(gfnt_exclusions_band(&exclusions, 12, 2, &band), GFNT_OK);
  EXPECT_EQ(band.start, 50);
  EXPECT_EQ(band.next_top, 20);
  ASSERT_EQ(gfnt_exclusions_band(&exclusions, band.next_top, 10, &band), GFNT_OK);
  EXPECT_EQ(band.start, 50);
  EXPECT_EQ(band.next_top, 40);
}

TEST(LayoutFlow, ParagraphClearsSuccessiveTooNarrowExclusionBands) {
  GFNT_Exclusion items[2]{};
  items[0].shape = GFNT_EXCLUSION_RECTANGLE;
  items[0].side = GFNT_EXCLUSION_TEXT_RIGHT;
  items[0].geometry.rectangle = {0, 0, 750, 800};
  items[1].shape = GFNT_EXCLUSION_RECTANGLE;
  items[1].side = GFNT_EXCLUSION_TEXT_RIGHT;
  items[1].geometry.rectangle = {0, 800, 750, 800};
  GFNT_Exclusions exclusions{items, 2, 0, 1000};
  GFNT_LineConstraint constraint{&exclusions, gfnt_exclusions_band};
  Font font(latin());
  Para para("AB", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout_flow(para.p, &constraint, &para.layout, nullptr), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_EQ(para.layout.lines[0].box.y, 1600);
  EXPECT_EQ(para.layout.lines[0].start, 0u);
  EXPECT_EQ(para.layout.lines[0].end, 2u);
}

TEST(LayoutColumns, BalanceSplitsWholeLinesAcrossEqualColumns) {
  Font font(latin());
  Para para("A\nB\nC\nD", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_ColumnOptions options{};
  options.inline_size = 640;
  options.block_extent = 1280;
  options.count = 2;
  GFNT_ColumnLayout columns{};
  ASSERT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_OK);
  EXPECT_TRUE(columns.complete);
  EXPECT_EQ(columns.next_byte, 7u);
  EXPECT_EQ(columns.column_count, 2u);
  ASSERT_EQ(columns.layout.line_count, 4u);
  ASSERT_NE(columns.column_line_starts, nullptr);
  EXPECT_EQ(columns.column_line_starts[0], 0u);
  EXPECT_EQ(columns.column_line_starts[1], 2u);
  EXPECT_EQ(columns.column_line_starts[2], 4u);
  EXPECT_EQ(columns.layout.lines[0].box.x, 0);
  EXPECT_EQ(columns.layout.lines[0].box.y, 0);
  EXPECT_EQ(columns.layout.lines[1].box.y, 640);
  EXPECT_EQ(columns.layout.lines[2].box.x, 640);
  EXPECT_EQ(columns.layout.lines[2].box.y, 0);
  EXPECT_EQ(columns.layout.width, 1280);
  EXPECT_EQ(columns.layout.height, 1280);
  gfnt_column_layout_free(&columns);
}

TEST(LayoutColumns, BalanceMinimizesUnequalLineBoxHeights) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_TextSpan spans[4]{};
  const size_t lengths[] = {2, 2, 2, 1};
  const GFNT_F26Dot6 heights[] = {10 * 64, 30 * 64, 10 * 64, 30 * 64};
  for (size_t i = 0; i < 4; i++) {
    spans[i].length = lengths[i];
    spans[i].style.faces = faces;
    spans[i].style.face_count = 1;
    spans[i].style.size = kSize;
    spans[i].style.line_height.kind = GFNT_LINE_HEIGHT_LENGTH;
    spans[i].style.line_height.value = heights[i];
  }
  Para para("A\nB\nC\nD", {spans[0], spans[1], spans[2], spans[3]});
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_ColumnOptions options{};
  options.inline_size = 640;
  options.block_extent = 100 * 64;
  options.count = 2;
  GFNT_ColumnLayout columns{};
  ASSERT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_OK);
  ASSERT_TRUE(columns.complete);
  ASSERT_EQ(columns.layout.line_count, 4u);
  EXPECT_EQ(columns.column_line_starts[1], 2u);
  EXPECT_EQ(columns.column_line_starts[2], 4u);
  EXPECT_EQ(columns.layout.lines[0].box.height, 10 * 64);
  EXPECT_EQ(columns.layout.lines[1].box.height, 30 * 64);
  EXPECT_EQ(columns.layout.lines[2].box.height, 10 * 64);
  EXPECT_EQ(columns.layout.lines[3].box.height, 30 * 64);
  gfnt_column_layout_free(&columns);

  const GFNT_F26Dot6 minimax_heights[] = {40 * 64, 10 * 64, 10 * 64, 10 * 64};
  for (size_t i = 0; i < 4; i++) spans[i].style.line_height.value = minimax_heights[i];
  Para minimax("A\nB\nC\nD", {spans[0], spans[1], spans[2], spans[3]});
  ASSERT_EQ(minimax.created, GFNT_OK);
  options.orphans = 1;
  options.widows = 1;
  options.block_extent = 100 * 64;
  ASSERT_EQ(gfnt_paragraph_layout_columns(minimax.p, &options, &columns, nullptr), GFNT_OK);
  ASSERT_TRUE(columns.complete);
  ASSERT_EQ(columns.column_line_starts[1], 1u);
  ASSERT_EQ(columns.column_line_starts[2], 4u);
  EXPECT_EQ(columns.layout.lines[0].box.height, 40 * 64);
  EXPECT_EQ(columns.layout.lines[1].box.height + columns.layout.lines[2].box.height
      + columns.layout.lines[3].box.height, 30 * 64);
  gfnt_column_layout_free(&columns);
}

TEST(LayoutColumns, HorizontalRtlProgressesFromTheRightmostColumn) {
  Font font(latin());
  GFNT_ParagraphOptions paragraph_options{};
  paragraph_options.direction = GFNT_BIDI_RTL;
  Para para("A\nB\nC\nD", {font.face}, paragraph_options);
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_ColumnOptions options{};
  options.inline_size = 640;
  options.block_extent = 1280;
  options.inline_gap = 64;
  options.count = 2;
  GFNT_ColumnLayout columns{};
  ASSERT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_OK);
  ASSERT_TRUE(columns.complete);
  ASSERT_EQ(columns.layout.line_count, 4u);
  EXPECT_EQ(columns.layout.lines[0].box.x, 704);
  EXPECT_EQ(columns.layout.lines[1].box.x, 704);
  EXPECT_EQ(columns.layout.lines[2].box.x, 0);
  EXPECT_EQ(columns.layout.lines[3].box.x, 0);
  gfnt_column_layout_free(&columns);
}

TEST(LayoutColumns, BalanceDoesNotTreatOversizedLineProgressAsAValidSmallerExtent) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_TextSpan span{};
  span.length = 3;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = 2 * kSize;
  Para para("A\nB", {span});
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_ColumnOptions options{};
  options.inline_size = 640;
  options.block_extent = 640;
  options.count = 2;
  GFNT_ColumnLayout columns{};
  ASSERT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_OK);
  ASSERT_TRUE(columns.complete);
  ASSERT_EQ(columns.layout.line_count, 2u);
  EXPECT_GT(columns.layout.lines[0].box.height, options.block_extent);
  EXPECT_GT(columns.layout.lines[1].box.height, options.block_extent);
  EXPECT_EQ(columns.column_line_starts[1], 1u);
  EXPECT_EQ(columns.column_line_starts[2], 2u);
  gfnt_column_layout_free(&columns);

  /* Balance may choose a smaller extent only when every line box fits that
   * extent. Overflow=error continues to judge against the configured extent. */
  options.overflow = GFNT_COLUMN_OVERFLOW_ERROR;
  EXPECT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_ERR_LIMIT);
  EXPECT_EQ(columns.layout.line_count, 0u);
}

TEST(LayoutColumns, BalanceKeepsConfiguredExtentWhenContentNeedsContinuation) {
  Font font(latin());
  Para para("A\nB\nC\nD\nE", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_ColumnOptions options{};
  options.inline_size = 640;
  options.block_extent = 640;
  options.count = 2;
  GFNT_ColumnLayout columns{};
  ASSERT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_OK);
  EXPECT_FALSE(columns.complete);
  ASSERT_EQ(columns.layout.line_count, 2u);
  EXPECT_EQ(columns.column_line_starts[0], 0u);
  EXPECT_EQ(columns.column_line_starts[1], 1u);
  EXPECT_EQ(columns.column_line_starts[2], 2u);
  EXPECT_EQ(columns.next_byte, 4u);
  EXPECT_EQ(columns.layout.height, options.block_extent);
  gfnt_column_layout_free(&columns);
}

TEST(LayoutColumns, NestedRubyCoordinatesShiftWithTheirSecondColumnBase) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation ruby{};
  ruby.base_start = 2;
  ruby.base_end = 3;
  ruby.kind = GFNT_ANNOTATION_RUBY;
  ruby.position = GFNT_ANNOTATION_OVER;
  ruby.style.faces = faces;
  ruby.style.face_count = 1;
  ruby.style.size = kSize;
  ruby.text = "A";
  ruby.text_length = 1;
  ruby.group_break = true;
  GFNT_Annotation nested{};
  nested.base_start = 0;
  nested.base_end = 1;
  nested.kind = GFNT_ANNOTATION_RUBY;
  nested.position = GFNT_ANNOTATION_UNDER;
  nested.style = ruby.style;
  nested.text = "B";
  nested.text_length = 1;
  ruby.children = &nested;
  ruby.child_count = 1;
  GFNT_ParagraphOptions paragraph_options{};
  paragraph_options.annotations = &ruby;
  paragraph_options.annotation_count = 1;
  Para para("A\nB", {font.face}, paragraph_options);
  ASSERT_EQ(para.created, GFNT_OK);

  GFNT_ColumnOptions options{};
  options.inline_size = 640;
  options.block_extent = 3 * 640;
  options.count = 2;
  options.fill = GFNT_COLUMN_FILL_AUTO;
  GFNT_ColumnLayout columns{};
  ASSERT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_OK);
  ASSERT_TRUE(columns.complete);
  ASSERT_EQ(columns.column_line_starts[1], 1u);
  const GFNT_Line & line = columns.layout.lines[1];
  ASSERT_EQ(line.annotation_count, 1u);
  const GFNT_LineAnnotation & top = line.annotations[0];
  ASSERT_EQ(top.child_count, 1u);
  ASSERT_EQ(top.run_count, 1u);
  ASSERT_EQ(top.runs[0].glyph_count, 1u);
  const GFNT_LineAnnotation & child = top.children[0];
  ASSERT_EQ(child.run_count, 1u);
  ASSERT_EQ(child.runs[0].glyph_count, 1u);
  EXPECT_EQ(top.box.x, options.inline_size);
  EXPECT_EQ(top.runs[0].pos, options.inline_size);
  EXPECT_EQ(top.runs[0].glyphs[0].x, options.inline_size);
  EXPECT_EQ(child.box.x, options.inline_size);
  EXPECT_EQ(child.runs[0].pos, options.inline_size);
  EXPECT_EQ(child.runs[0].glyphs[0].x, options.inline_size);
  gfnt_column_layout_free(&columns);
}

TEST(LayoutColumns, AutoReportsTheFirstByteThatDidNotFit) {
  Font font(latin());
  Para para("A\nB\nC\nD", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_ColumnOptions options{};
  options.inline_size = 640;
  options.block_extent = 640;
  options.count = 2;
  options.fill = GFNT_COLUMN_FILL_AUTO;
  GFNT_ColumnLayout columns{};
  ASSERT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_OK);
  EXPECT_FALSE(columns.complete);
  EXPECT_EQ(columns.next_byte, 4u);
  ASSERT_EQ(columns.layout.line_count, 2u);
  EXPECT_EQ(columns.column_line_starts[1], 1u);
  EXPECT_EQ(columns.column_line_starts[2], 2u);
  EXPECT_EQ(columns.layout.lines[1].box.x, 640);
  EXPECT_EQ(columns.layout.lines[1].box.y, 0);
  gfnt_column_layout_free(&columns);
}

TEST(LayoutColumns, AutoHonorsWidowsWhenAWholeLineCanMove) {
  Font font(latin());
  Para para("A\nB\nC\nD", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_ColumnOptions options{};
  options.inline_size = 640;
  options.block_extent = 3 * 640;
  options.count = 2;
  options.fill = GFNT_COLUMN_FILL_AUTO;
  GFNT_ColumnLayout columns{};
  ASSERT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_OK);
  ASSERT_TRUE(columns.complete);
  EXPECT_EQ(columns.column_line_starts[1], 2u);
  EXPECT_EQ(columns.column_line_starts[2], 4u);
  gfnt_column_layout_free(&columns);
}

TEST(LayoutColumns, AutoHonorsConfiguredOrphansAndWidows) {
  Font font(latin());
  Para para("A\nB\nC\nD\nE", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_ColumnOptions options{};
  options.inline_size = 640;
  options.block_extent = 3 * 640;
  options.count = 2;
  options.fill = GFNT_COLUMN_FILL_AUTO;
  options.orphans = 3;
  options.widows = 2;
  GFNT_ColumnLayout columns{};
  ASSERT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_OK);
  ASSERT_TRUE(columns.complete);
  EXPECT_EQ(columns.column_line_starts[1], 3u);
  EXPECT_EQ(columns.column_line_starts[2], 5u);
  gfnt_column_layout_free(&columns);

  Para impossible("A\nB\nC\nD", {font.face});
  ASSERT_EQ(impossible.created, GFNT_OK);
  options.block_extent = 3 * 640;
  GFNT_ColumnLayout conflict{};
  ASSERT_EQ(gfnt_paragraph_layout_columns(impossible.p, &options, &conflict, nullptr), GFNT_OK);
  ASSERT_TRUE(conflict.complete);
  EXPECT_EQ(conflict.column_line_starts[1], 3u)
      << "preserve the configured orphan count when four lines cannot satisfy 3/2";
  EXPECT_EQ(conflict.column_line_starts[2], 4u);
  gfnt_column_layout_free(&conflict);
}

TEST(LayoutColumns, OversizedFirstLineMakesProgressUnlessConfiguredToError) {
  Font font(latin());
  Para para("A", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_ColumnOptions options{};
  options.inline_size = 640;
  options.block_extent = 100;
  options.count = 1;
  GFNT_ColumnLayout columns{};
  ASSERT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_OK);
  EXPECT_TRUE(columns.complete);
  ASSERT_EQ(columns.layout.line_count, 1u);
  EXPECT_GT(columns.layout.lines[0].box.height, options.block_extent);
  gfnt_column_layout_free(&columns);
  options.overflow = GFNT_COLUMN_OVERFLOW_ERROR;
  EXPECT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_ERR_LIMIT);
  EXPECT_EQ(columns.layout.line_count, 0u);
}

TEST(LayoutColumns, OverflowErrorIgnoresLinesThatWillNotBePlaced) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_TextSpan spans[2]{};
  spans[0].length = 2;
  spans[0].style.faces = faces;
  spans[0].style.face_count = 1;
  spans[0].style.size = kSize;
  spans[1].length = 1;
  spans[1].style.faces = faces;
  spans[1].style.face_count = 1;
  spans[1].style.size = 30 * 64;
  Para para("A\nB", {spans[0], spans[1]});
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_ColumnOptions options{};
  options.inline_size = 640;
  options.block_extent = 640;
  options.count = 1;
  options.fill = GFNT_COLUMN_FILL_AUTO;
  options.overflow = GFNT_COLUMN_OVERFLOW_ERROR;
  GFNT_ColumnLayout columns{};
  ASSERT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_OK);
  EXPECT_FALSE(columns.complete);
  ASSERT_EQ(columns.layout.line_count, 1u);
  EXPECT_EQ(columns.next_byte, 2u);
  gfnt_column_layout_free(&columns);
}

TEST(LayoutColumns, MaxLinesIsAllowedUnlessItActuallyTruncates) {
  Font font(latin());
  GFNT_ParagraphOptions paragraph_options{};
  paragraph_options.max_lines = 4;
  Para fits("A", {font.face}, paragraph_options);
  ASSERT_EQ(fits.created, GFNT_OK);
  GFNT_ColumnOptions options{};
  options.inline_size = 640;
  options.block_extent = 640;
  options.count = 1;
  GFNT_ColumnLayout columns{};
  EXPECT_EQ(gfnt_paragraph_layout_columns(fits.p, &options, &columns, nullptr), GFNT_OK);
  gfnt_column_layout_free(&columns);

  paragraph_options.max_lines = 1;
  Para truncates("A\nB", {font.face}, paragraph_options);
  ASSERT_EQ(truncates.created, GFNT_OK);
  EXPECT_EQ(gfnt_paragraph_layout_columns(truncates.p, &options, &columns, nullptr),
      GFNT_ERR_INVALID);
}

TEST(LayoutColumns, RejectsUnknownPoliciesWithoutWritingOutput) {
  Font font(latin());
  Para para("A", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_ColumnOptions options{};
  options.inline_size = 640;
  options.block_extent = 640;
  options.count = 1;
  options.fill = static_cast<GFNT_ColumnFill>(-1);
  GFNT_ColumnLayout output{};
  output.next_byte = 123;
  EXPECT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &output, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(output.next_byte, 123u);
  options.fill = GFNT_COLUMN_FILL_BALANCE;
  options.overflow = static_cast<GFNT_ColumnOverflow>(-1);
  EXPECT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &output, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(output.next_byte, 123u);
}

TEST(LayoutColumns, VerticalWritingKeepsInlineProgressionAcrossFragments) {
  Font font({0x4E00, 0x4E8C, 0x4E09, 0x56DB});
  GFNT_ParagraphOptions paragraph_options{};
  paragraph_options.writing_mode = GFNT_WRITING_VERTICAL_RL;
  Para para("一二三四", {font.face}, paragraph_options);
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_ColumnOptions options{};
  options.inline_size = 640;
  options.block_extent = 640;
  options.count = 4;
  options.fill = GFNT_COLUMN_FILL_AUTO;
  GFNT_ColumnLayout columns{};
  ASSERT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_OK);
  EXPECT_TRUE(columns.complete);
  ASSERT_EQ(columns.layout.line_count, 4u);
  EXPECT_EQ(columns.column_line_starts[1], 1u);
  EXPECT_EQ(columns.column_line_starts[4], 4u);
  EXPECT_EQ(columns.layout.lines[1].box.y - columns.layout.lines[0].box.y, 640);
  EXPECT_EQ(columns.layout.width, 640);
  EXPECT_EQ(columns.layout.height, 640);
  gfnt_column_layout_free(&columns);
}

TEST(LayoutColumns, VerticalRlRestartsBlockPositionsInEachInlineFragment) {
  Font font({0x4E00, 0x4E8C, 0x4E09, 0x56DB});
  GFNT_ParagraphOptions paragraph_options{};
  paragraph_options.writing_mode = GFNT_WRITING_VERTICAL_RL;
  Para para("一\n二\n三\n四", {font.face}, paragraph_options);
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_ColumnOptions options{};
  options.inline_size = 40 * 64;
  options.block_extent = 40 * 64;
  options.count = 2;
  options.fill = GFNT_COLUMN_FILL_BALANCE;
  GFNT_ColumnLayout columns{};
  ASSERT_EQ(gfnt_paragraph_layout_columns(para.p, &options, &columns, nullptr), GFNT_OK);
  ASSERT_EQ(columns.layout.line_count, 4u);
  ASSERT_EQ(columns.column_line_starts[1], 2u);
  EXPECT_GT(columns.layout.lines[0].box.x, columns.layout.lines[1].box.x);
  EXPECT_EQ(columns.layout.lines[0].box.y / 64, 0);
  EXPECT_EQ(columns.layout.lines[1].box.y / 64, 0);
  EXPECT_EQ(columns.layout.lines[2].box.x, columns.layout.lines[0].box.x);
  EXPECT_EQ(columns.layout.lines[3].box.x, columns.layout.lines[1].box.x);
  EXPECT_EQ(columns.layout.lines[2].box.y / 64, 40);
  EXPECT_EQ(columns.layout.lines[3].box.y / 64, 40);
  gfnt_column_layout_free(&columns);
}

TEST(LayoutFlow, BuiltInExclusionsClearAtTheirExactBottomEdge) {
  GFNT_Exclusion item{};
  item.shape = GFNT_EXCLUSION_RECTANGLE;
  item.side = GFNT_EXCLUSION_TEXT_RIGHT;
  item.geometry.rectangle = {10, 10, 20, 10};
  GFNT_Exclusions exclusions{&item, 1, 0, 300};
  GFNT_Band band{};

  ASSERT_EQ(gfnt_exclusions_band(&exclusions, 20, 4, &band), GFNT_OK);
  EXPECT_EQ(band.start, 0);
  EXPECT_EQ(band.width, 300);
  EXPECT_EQ(band.next_top, 20);
}

TEST(LayoutFlow, ExclusionsOutsideContainerDoNotTriggerClearing) {
  GFNT_Exclusion items[2]{};
  items[0].shape = GFNT_EXCLUSION_RECTANGLE;
  items[0].side = GFNT_EXCLUSION_TEXT_RIGHT;
  items[0].geometry.rectangle = {0, 10, 10, 30};
  items[1].shape = GFNT_EXCLUSION_RECTANGLE;
  items[1].side = GFNT_EXCLUSION_TEXT_LEFT;
  items[1].geometry.rectangle = {400, 10, 20, 30};
  GFNT_Exclusions exclusions{items, 2, 20, 300};
  GFNT_Band band{};

  ASSERT_EQ(gfnt_exclusions_band(&exclusions, 15, 10, &band), GFNT_OK);
  EXPECT_EQ(band.start, 20);
  EXPECT_EQ(band.width, 300);
  EXPECT_EQ(band.next_top, 15);
}

TEST(LayoutFlow, BuiltInExclusionsRejectInvalidGeometry) {
  GFNT_Exclusion exclusion{};
  exclusion.shape = GFNT_EXCLUSION_CIRCLE;
  exclusion.geometry.circle.radius = -1;
  GFNT_Exclusions exclusions{&exclusion, 1, 0, 300};
  GFNT_Band band{};
  EXPECT_EQ(gfnt_exclusions_band(&exclusions, 0, 10, &band), GFNT_ERR_INVALID);
}

TEST(LayoutLineBox, ExplicitLengthLineHeightCentersLeadingAroundTheBaseline) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_TextSpan span{};
  span.length = 1;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = 20 * 64;
  span.style.line_height = {GFNT_LINE_HEIGHT_LENGTH, 20 * 64 + 3};
  Para para("A", {span});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, GFNT_LAYOUT_UNBOUNDED, &para.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_EQ(line.box.height, 20 * 64 + 3);
  EXPECT_EQ(line.ascent + line.descent, line.box.height);
  EXPECT_EQ(line.ascent, 16 * 64);
  EXPECT_EQ(line.descent, 4 * 64 + 3);
  EXPECT_EQ(line.runs[0].glyphs[0].y, line.baseline);
}

TEST(LayoutLineBox, MultipleLineHeightUsesFontSizeDespiteLineGap) {
  Font font(latin(), false, 0, 300);
  const GFNT_Face * faces[] = {font.face};
  GFNT_LineMetrics metrics{};
  ASSERT_EQ(gfnt_face_line_metrics(font.face, GFNT_LINE_METRICS_FONT, nullptr,
              &metrics, nullptr), GFNT_OK);
  ASSERT_GT(metrics.line_gap, 0);
  GFNT_TextSpan span{};
  span.length = 1;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = 20 * 64;
  span.style.line_metrics = GFNT_LINE_METRICS_FONT;
  span.style.line_height = {GFNT_LINE_HEIGHT_MULTIPLE, 0x18000};
  Para para("A", {span});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, GFNT_LAYOUT_UNBOUNDED, &para.layout, nullptr), GFNT_OK);
  EXPECT_EQ(para.layout.lines[0].box.height, 30 * 64);
}

TEST(LayoutLineBox, StrutContributesToSmallAndEmptyLines) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_TextSpan large{}, small{};
  large.length = 1;
  large.style.faces = faces;
  large.style.face_count = 1;
  large.style.size = 20 * 64;
  large.style.line_height = {GFNT_LINE_HEIGHT_LENGTH, 60 * 64};
  small = large;
  small.length = 4;
  small.style.size = 10 * 64;
  small.style.line_height = {GFNT_LINE_HEIGHT_LENGTH, 10 * 64};
  GFNT_ParagraphOptions options{};
  options.strut = true;
  Para para("A\n\nB\n", {large, small}, options);
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, GFNT_LAYOUT_UNBOUNDED, &para.layout, nullptr), GFNT_OK);
  GFNT_ParagraphOptions no_strut{};
  Para without("A\n\nB\n", {large, small}, no_strut);
  ASSERT_EQ(without.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(without.p, GFNT_LAYOUT_UNBOUNDED, &without.layout, nullptr), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 4u);
  EXPECT_EQ(para.layout.lines[1].box.height, 60 * 64);
  EXPECT_EQ(para.layout.lines[2].box.height, 60 * 64);
  EXPECT_EQ(para.layout.lines[3].box.height, 60 * 64);
  EXPECT_GT(para.layout.lines[1].box.height, without.layout.lines[1].box.height);
  EXPECT_GT(para.layout.lines[2].box.height, without.layout.lines[2].box.height);
  EXPECT_GT(para.layout.lines[3].box.height, without.layout.lines[3].box.height);
  const GFNT_PlacedGlyph * strut_glyph = nullptr, * plain_glyph = nullptr;
  for (size_t r = 0; r < para.layout.lines[2].run_count; r++) {
    for (size_t g = 0; g < para.layout.lines[2].runs[r].glyph_count; g++) {
      if (para.layout.lines[2].runs[r].glyphs[g].cluster == 3) {
        strut_glyph = &para.layout.lines[2].runs[r].glyphs[g];
      }
    }
  }
  for (size_t r = 0; r < without.layout.lines[2].run_count; r++) {
    for (size_t g = 0; g < without.layout.lines[2].runs[r].glyph_count; g++) {
      if (without.layout.lines[2].runs[r].glyphs[g].cluster == 3) {
        plain_glyph = &without.layout.lines[2].runs[r].glyphs[g];
      }
    }
  }
  ASSERT_NE(strut_glyph, nullptr);
  ASSERT_NE(plain_glyph, nullptr);
  EXPECT_NE(strut_glyph->y, plain_glyph->y);
}

TEST(LayoutLineBox, TopAndBottomAlignmentUnionMixedSizeInlineBoxes) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_TextSpan small{}, large{};
  small.length = large.length = 1;
  small.style.faces = large.style.faces = faces;
  small.style.face_count = large.style.face_count = 1;
  small.style.size = 10 * 64;
  large.style.size = 20 * 64;
  for (const auto & [align, expected_shift] : {
           std::pair{GFNT_VERTICAL_ALIGN_TOP, 8 * 64},
           std::pair{GFNT_VERTICAL_ALIGN_BOTTOM, -2 * 64}}) {
    large.style.vertical_align = align;
    Para para("AB", {small, large});
    ASSERT_EQ(para.created, GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(para.p, GFNT_LAYOUT_UNBOUNDED, &para.layout, nullptr), GFNT_OK);
    const GFNT_Line & line = para.layout.lines[0];
    ASSERT_EQ(line.run_count, 2u);
    EXPECT_EQ(line.box.height, 20 * 64);
    EXPECT_EQ(line.runs[1].glyphs[0].y - line.runs[0].glyphs[0].y, expected_shift);
  }
}

TEST(LayoutLineBox, MiddleUsesOs2XHeightRatherThanParentAscent) {
  Font font(latin(), false, 300);
  const GFNT_Face * faces[] = {font.face};
  GFNT_TextSpan baseline{}, middle{};
  baseline.length = middle.length = 1;
  baseline.style.faces = middle.style.faces = faces;
  baseline.style.face_count = middle.style.face_count = 1;
  baseline.style.size = middle.style.size = 20 * 64;
  middle.style.vertical_align = GFNT_VERTICAL_ALIGN_MIDDLE;
  Para para("AB", {baseline, middle});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, GFNT_LAYOUT_UNBOUNDED, &para.layout, nullptr), GFNT_OK);
  ASSERT_EQ(para.layout.lines[0].run_count, 2u);
  EXPECT_EQ(para.layout.lines[0].runs[1].glyphs[0].y
      - para.layout.lines[0].runs[0].glyphs[0].y, 3 * 64);
}

TEST(LayoutLineBox, BaselineShiftMovesGlyphsAcrossTheLineAxisInEveryWritingMode) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_TextSpan base{}, raised{};
  base.length = raised.length = 1;
  base.style.faces = raised.style.faces = faces;
  base.style.face_count = raised.style.face_count = 1;
  base.style.size = raised.style.size = kSize;
  raised.style.vertical_align = GFNT_VERTICAL_ALIGN_LENGTH;
  raised.style.vertical_align_value = 64;
  for (GFNT_WritingMode mode : {GFNT_WRITING_HORIZONTAL_TB, GFNT_WRITING_VERTICAL_RL,
           GFNT_WRITING_VERTICAL_LR, GFNT_WRITING_SIDEWAYS_RL, GFNT_WRITING_SIDEWAYS_LR}) {
    GFNT_ParagraphOptions options{};
    options.writing_mode = mode;
    Para para("AB", {base, raised}, options);
    ASSERT_EQ(para.created, GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(para.p, GFNT_LAYOUT_UNBOUNDED, &para.layout, nullptr), GFNT_OK);
    const GFNT_PlacedGlyph & a = para.layout.lines[0].runs[0].glyphs[0];
    const GFNT_PlacedGlyph & b = para.layout.lines[0].runs[1].glyphs[0];
    if (mode == GFNT_WRITING_HORIZONTAL_TB) EXPECT_EQ(std::abs(a.y - b.y), 64);
    else EXPECT_EQ(std::abs(b.pen_x - a.pen_x), 64);
  }
}

TEST(LayoutLineBox, RangeRectPreservesNegativeDescentFromLargeBaselineShift) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_TextSpan raised{};
  raised.length = 2;
  raised.style.faces = faces;
  raised.style.face_count = 1;
  raised.style.size = kSize;
  raised.style.vertical_align = GFNT_VERTICAL_ALIGN_LENGTH;
  raised.style.vertical_align_value = 30 * 64;
  Para para("AB", {raised});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_LT(para.layout.lines[0].descent, 0);

  GFNT_Rect rect{};
  size_t count = 0;
  ASSERT_EQ(gfnt_layout_range_rects(para.p, &para.layout, 0, 2, &rect, 1, &count), GFNT_OK);
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(rect.y, para.layout.lines[0].box.y);
  EXPECT_EQ(rect.height, para.layout.lines[0].box.height);
}

TEST(LayoutLineBox, RangeRectPreservesNegativeAscentFromLargeBaselineShift) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_TextSpan lowered{};
  lowered.length = 2;
  lowered.style.faces = faces;
  lowered.style.face_count = 1;
  lowered.style.size = kSize;
  lowered.style.vertical_align = GFNT_VERTICAL_ALIGN_LENGTH;
  lowered.style.vertical_align_value = -30 * 64;
  Para para("AB", {lowered});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_LT(para.layout.lines[0].ascent, 0);

  GFNT_Rect rect{};
  size_t count = 0;
  ASSERT_EQ(gfnt_layout_range_rects(para.p, &para.layout, 0, 2, &rect, 1, &count), GFNT_OK);
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(rect.y, para.layout.lines[0].box.y);
  EXPECT_EQ(rect.height, para.layout.lines[0].box.height);
}

TEST(LayoutLineBox, InvalidSelectorsAreRejectedAtParagraphCreation) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_TextSpan span{};
  span.length = 1;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;
  span.style.line_height.kind = static_cast<GFNT_LineHeightKind>(99);
  GFNT_Paragraph * paragraph = nullptr;
  EXPECT_EQ(gfnt_paragraph_create("A", 1, &span, 1, nullptr, nullptr, &paragraph, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(paragraph, nullptr);
  span.style.line_height = {GFNT_LINE_HEIGHT_LENGTH, -1};
  EXPECT_EQ(gfnt_paragraph_create("A", 1, &span, 1, nullptr, nullptr, &paragraph, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(paragraph, nullptr);
  span.style.line_height = {GFNT_LINE_HEIGHT_NORMAL, 0};
  span.style.vertical_align = static_cast<GFNT_VerticalAlign>(99);
  EXPECT_EQ(gfnt_paragraph_create("A", 1, &span, 1, nullptr, nullptr, &paragraph, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(paragraph, nullptr);
}

TEST(LayoutLineBox, ComputedMetricsOutsidePublicRangeAreRejected) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_TextSpan spans[2]{};
  spans[0].length = spans[1].length = 1;
  spans[0].style.faces = spans[1].style.faces = faces;
  spans[0].style.face_count = spans[1].style.face_count = 1;
  spans[0].style.size = spans[1].style.size = kSize;
  spans[1].style.line_height = {GFNT_LINE_HEIGHT_MULTIPLE, INT32_MAX};
  spans[1].style.size = 65537;
  GFNT_Paragraph * paragraph = nullptr;
  EXPECT_EQ(gfnt_paragraph_create("AB", 2, spans, 2, nullptr, nullptr, &paragraph, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(paragraph, nullptr);

  paragraph = nullptr;
  spans[1].style.line_height = {GFNT_LINE_HEIGHT_NORMAL, 0};
  spans[1].style.size = INT32_MAX;
  EXPECT_EQ(gfnt_paragraph_create("AB", 2, spans, 2, nullptr, nullptr, &paragraph, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(paragraph, nullptr);
}

TEST(LayoutInlineObject, OneReplacementCharacterBecomesAnAtomicCallerOwnedRun) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  int identity = 7;
  GFNT_InlineObject object{};
  object.advance = 900;
  object.ascent = 640;
  object.descent = 128;
  object.user = &identity;
  GFNT_TextSpan span{};
  span.length = 3;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;
  span.style.inline_object = &object;
  const std::string text = "\xEF\xBF\xBC";
  GFNT_Paragraph * paragraph = nullptr;
  ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, nullptr, nullptr,
                  &paragraph, nullptr), GFNT_OK);
  GFNT_Layout layout{};
  ASSERT_EQ(gfnt_paragraph_layout(paragraph, 1200, &layout, nullptr), GFNT_OK);
  ASSERT_EQ(layout.line_count, 1u);
  ASSERT_EQ(layout.lines[0].run_count, 1u);
  const GFNT_LineRun & run = layout.lines[0].runs[0];
  const GFNT_F26Dot6 baseline_y = run.glyphs[0].y;
  EXPECT_EQ(run.face, nullptr);
  EXPECT_EQ(run.object, &identity);
  ASSERT_EQ(run.glyph_count, 1u);
  EXPECT_EQ(run.glyphs[0].glyph, GFNT_GLYPH_NONE);
  EXPECT_EQ(run.glyphs[0].advance, 900);
  EXPECT_EQ(run.advance, 900);
  EXPECT_EQ(layout.lines[0].ascent, object.ascent);
  EXPECT_EQ(layout.lines[0].descent, object.descent);
  EXPECT_EQ(layout.ink.width, 0);
  GFNT_F26Dot6 min_content = 0;
  ASSERT_EQ(gfnt_paragraph_min_content(paragraph, &min_content), GFNT_OK);
  EXPECT_EQ(min_content, object.advance);
  GFNT_Hit hit{};
  ASSERT_EQ(gfnt_layout_hit_test(paragraph, &layout, 400, 300, &hit), GFNT_OK);
  EXPECT_TRUE(hit.offset == 0 || hit.offset == text.size());
  GFNT_Rect rect{};
  size_t count = 0;
  ASSERT_EQ(gfnt_layout_range_rects(paragraph, &layout, 0, text.size(), &rect, 1, &count),
      GFNT_OK);
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(rect.width, object.advance);
  gfnt_layout_free(&layout);
  gfnt_paragraph_free(paragraph);

  object.align = GFNT_VERTICAL_ALIGN_LENGTH;
  object.align_value = 64;
  paragraph = nullptr;
  ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, nullptr, nullptr,
                  &paragraph, nullptr), GFNT_OK);
  layout = {};
  ASSERT_EQ(gfnt_paragraph_layout(paragraph, 1200, &layout, nullptr), GFNT_OK);
  ASSERT_EQ(layout.lines[0].run_count, 1u);
  EXPECT_EQ(layout.lines[0].baseline, baseline_y + 64);
  EXPECT_EQ(layout.lines[0].runs[0].glyphs[0].y, baseline_y);
  EXPECT_EQ(layout.lines[0].ascent, object.ascent + 64);
  EXPECT_EQ(layout.lines[0].descent, object.descent - 64);
  gfnt_layout_free(&layout);
  gfnt_paragraph_free(paragraph);
}

TEST(LayoutInlineObject, InvalidDescriptorOrRangeIsRejected) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_InlineObject object{};
  object.advance = -1;
  GFNT_TextSpan span{};
  span.length = 3;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;
  span.style.inline_object = &object;
  const std::string marker = "\xEF\xBF\xBC";
  GFNT_Paragraph * paragraph = nullptr;
  EXPECT_EQ(gfnt_paragraph_create(marker.data(), marker.size(), &span, 1, nullptr, nullptr,
                  &paragraph, nullptr), GFNT_ERR_INVALID);
  object.advance = 10;
  const std::string wrong = "A";
  span.length = wrong.size();
  EXPECT_EQ(gfnt_paragraph_create(wrong.data(), wrong.size(), &span, 1, nullptr, nullptr,
                  &paragraph, nullptr), GFNT_ERR_INVALID);
}

TEST(LayoutInlineObject, RejectsAccumulatedAdvanceOutsideCoordinateRange) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_InlineObject objects[2]{};
  GFNT_TextSpan spans[2]{};
  const std::string text = "\xEF\xBF\xBC\xEF\xBF\xBC";
  for (size_t i = 0; i < 2; i++) {
    objects[i].advance = INT32_MAX / 2 + 1;
    objects[i].ascent = 32;
    spans[i].length = 3;
    spans[i].style.faces = faces;
    spans[i].style.face_count = 1;
    spans[i].style.size = kSize;
    spans[i].style.inline_object = &objects[i];
  }
  GFNT_Paragraph * paragraph = nullptr;
  ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), spans, 2, nullptr, nullptr,
                  &paragraph, nullptr), GFNT_OK);
  GFNT_F26Dot6 width = 0;
  EXPECT_EQ(gfnt_paragraph_max_content(paragraph, &width), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_paragraph_min_content(paragraph, &width), GFNT_ERR_INVALID);
  GFNT_Layout layout{};
  EXPECT_EQ(gfnt_paragraph_layout(paragraph, GFNT_LAYOUT_UNBOUNDED, &layout, nullptr),
      GFNT_ERR_INVALID);
  gfnt_layout_free(&layout);
  gfnt_paragraph_free(paragraph);
}

TEST(LayoutInlineObject, ReplacementCharacterWithoutDescriptorStaysText) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_TextSpan span{};
  span.length = 3;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;
  const std::string marker = "\xEF\xBF\xBC";
  GFNT_Paragraph * paragraph = nullptr;
  ASSERT_EQ(gfnt_paragraph_create(marker.data(), marker.size(), &span, 1, nullptr, nullptr,
                  &paragraph, nullptr), GFNT_OK);
  GFNT_Layout layout{};
  ASSERT_EQ(gfnt_paragraph_layout(paragraph, 1000, &layout, nullptr), GFNT_OK);
  ASSERT_EQ(layout.lines[0].run_count, 1u);
  EXPECT_NE(layout.lines[0].runs[0].face, nullptr);
  EXPECT_EQ(layout.lines[0].runs[0].object, nullptr);
  gfnt_layout_free(&layout);
  gfnt_paragraph_free(paragraph);
}

TEST(LayoutInlineObject, PlacedInEveryWritingModeAndKeepsBaseByteGeometry) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_InlineObject object{};
  object.advance = 480;
  object.ascent = 320;
  object.descent = 160;
  GFNT_TextSpan span{};
  span.length = 3;
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;
  span.style.inline_object = &object;
  const std::string text = "\xEF\xBF\xBC";
  const GFNT_WritingMode modes[] = {GFNT_WRITING_HORIZONTAL_TB, GFNT_WRITING_VERTICAL_RL,
      GFNT_WRITING_VERTICAL_LR, GFNT_WRITING_SIDEWAYS_RL, GFNT_WRITING_SIDEWAYS_LR};
  for (GFNT_WritingMode mode : modes) {
    GFNT_ParagraphOptions options{};
    options.writing_mode = mode;
    GFNT_Paragraph * paragraph = nullptr;
    ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, &options, nullptr,
                    &paragraph, nullptr), GFNT_OK);
    GFNT_Layout layout{};
    ASSERT_EQ(gfnt_paragraph_layout(paragraph, 1000, &layout, nullptr), GFNT_OK);
    ASSERT_EQ(layout.lines[0].run_count, 1u);
    EXPECT_EQ(layout.lines[0].runs[0].glyphs[0].advance, object.advance);
    if (mode == GFNT_WRITING_HORIZONTAL_TB) {
      EXPECT_EQ(layout.lines[0].ascent, object.ascent);
      EXPECT_EQ(layout.lines[0].descent, object.descent);
    }
    GFNT_Rect rect{};
    size_t count = 0;
    ASSERT_EQ(gfnt_layout_range_rects(paragraph, &layout, 0, text.size(), &rect, 1, &count),
        GFNT_OK);
    ASSERT_EQ(count, 1u);
    if (mode == GFNT_WRITING_HORIZONTAL_TB) {
      EXPECT_EQ(rect.width, object.advance);
      EXPECT_EQ(rect.height, object.ascent + object.descent);
      EXPECT_EQ(rect.y, layout.lines[0].box.y);
      EXPECT_EQ(layout.lines[0].runs[0].glyphs[0].y, layout.lines[0].baseline);
    }
    else {
      EXPECT_EQ(rect.height, object.advance);
      EXPECT_EQ(rect.width, object.ascent + object.descent);
      EXPECT_EQ(rect.x, layout.lines[0].box.x);
      EXPECT_EQ(rect.y, layout.lines[0].runs[0].pos);
      EXPECT_EQ(layout.lines[0].runs[0].glyphs[0].x, layout.lines[0].baseline);
    }
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(paragraph);
  }
}

TEST(LayoutInlineObject, BreakBeforeOverridesWordJoinerIndependently) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_InlineObject object{};
  object.advance = 400;
  object.ascent = 320;
  object.descent = 160;
  object.break_before = true;
  object.break_after = false;
  const std::string word_joiner = "\xE2\x81\xA0";
  const std::string text = std::string("A") + word_joiner + "\xEF\xBF\xBC" + word_joiner + "B";
  GFNT_TextSpan spans[5]{};
  spans[0].length = spans[4].length = 1;
  spans[1].length = spans[3].length = 3;
  spans[2].length = 3;
  for (GFNT_TextSpan & span : spans) {
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = kSize;
  }
  spans[2].style.inline_object = &object;
  Para para(text, {spans[0], spans[1], spans[2], spans[3], spans[4]});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, 350, &para.layout, nullptr), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[0].start, 0u);
  EXPECT_EQ(para.layout.lines[0].end, 4u);
  EXPECT_EQ(para.layout.lines[1].start, 4u);
}

TEST(LayoutInlineObject, BreakAfterCreatesAnOpportunityIndependently)
{
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_InlineObject object{};
  object.advance = 400;
  object.ascent = 320;
  object.descent = 160;
  object.break_after = true;
  const std::string word_joiner = "\xE2\x81\xA0";
  const std::string text = std::string("A") + word_joiner + "\xEF\xBF\xBC" + word_joiner + "B";
  GFNT_TextSpan spans[5]{};
  spans[0].length = spans[4].length = 1;
  spans[1].length = spans[3].length = 3;
  spans[2].length = 3;
  for (GFNT_TextSpan & span : spans) {
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = kSize;
  }
  spans[2].style.inline_object = &object;
  Para para(text, {spans[0], spans[1], spans[2], spans[3], spans[4]});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, 350, &para.layout, nullptr), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[0].start, 0u);
  EXPECT_EQ(para.layout.lines[0].end, 7u);
  EXPECT_EQ(para.layout.lines[1].start, 7u);
}

TEST(LayoutInlineObject, GapAfterObjectJustifiesOnlyInterCharacter) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_InlineObject object{};
  object.advance = 400;
  object.ascent = 320;
  object.descent = 160;
  const std::string text = std::string("\xEF\xBF\xBC") + "A";
  GFNT_TextSpan spans[2]{};
  spans[0].length = 3;
  spans[1].length = 1;
  for (GFNT_TextSpan & span : spans) {
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = kSize;
  }
  spans[0].style.inline_object = &object;
  for (GFNT_Justify justify : {GFNT_JUSTIFY_AUTO, GFNT_JUSTIFY_INTER_CHARACTER}) {
    GFNT_ParagraphOptions options{};
    options.align = GFNT_ALIGN_JUSTIFY;
    options.justify = justify;
    options.justify_last = true;
    Para para(text, {spans[0], spans[1]}, options);
    ASSERT_EQ(para.created, GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(para.p, 1200, &para.layout, nullptr), GFNT_OK);
    ASSERT_EQ(para.layout.line_count, 1u);
    EXPECT_EQ(para.layout.lines[0].justified, justify == GFNT_JUSTIFY_INTER_CHARACTER);
    if (justify == GFNT_JUSTIFY_INTER_CHARACTER) {
      EXPECT_GT(para.layout.lines[0].runs[0].advance, object.advance);
    }
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
  bad = {};
  bad.text_wrap_style = static_cast<GFNT_TextWrapStyle>(99);
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

TEST(LayoutContent, OverflowWrapCreatesEmergencyBreaksWithSpecifiedMinContent) {
  Font font(latin());
  GFNT_F26Dot6 anywhere_min = 0, break_word_min = 0;
  GFNT_ParagraphOptions anywhere{};
  anywhere.overflow_wrap = GFNT_OVERFLOW_WRAP_ANYWHERE;
  GFNT_ParagraphOptions break_word{};
  break_word.overflow_wrap = GFNT_OVERFLOW_WRAP_BREAK_WORD;
  Para wrap_anywhere("ABCD", {font.face}, anywhere);
  Para wrap_word("ABCD", {font.face}, break_word);

  ASSERT_EQ(gfnt_paragraph_min_content(wrap_anywhere.p, &anywhere_min), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_min_content(wrap_word.p, &break_word_min), GFNT_OK);
  EXPECT_EQ(anywhere_min, kLetter);
  EXPECT_EQ(break_word_min, 4 * kLetter);
  ASSERT_EQ(wrap_anywhere.lay(2 * kLetter), GFNT_OK);
  ASSERT_EQ(wrap_word.lay(2 * kLetter), GFNT_OK);
  EXPECT_EQ(wrap_anywhere.layout.line_count, 2u);
  EXPECT_EQ(wrap_word.layout.line_count, 2u);
}

TEST(LayoutContent, WordBreakBreakAllTurnsLetterBoundariesIntoOrdinaryBreaks) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.word_break = GFNT_WORD_BREAK_BREAK_ALL;
  Para para("ABCD", {font.face}, options);
  GFNT_F26Dot6 minimum = 0;

  ASSERT_EQ(gfnt_paragraph_min_content(para.p, &minimum), GFNT_OK);
  EXPECT_EQ(minimum, kLetter);
  ASSERT_EQ(para.lay(2 * kLetter), GFNT_OK);
  EXPECT_EQ(para.layout.line_count, 2u);
}

TEST(LayoutContent, WordBreakBreakWordAliasesAnywhereIncludingMinContent) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.word_break = GFNT_WORD_BREAK_BREAK_WORD;
  Para para("ABCD", {font.face}, options);
  GFNT_F26Dot6 minimum = 0;

  ASSERT_EQ(gfnt_paragraph_min_content(para.p, &minimum), GFNT_OK);
  EXPECT_EQ(minimum, kLetter);
  ASSERT_EQ(para.lay(2 * kLetter), GFNT_OK);
  EXPECT_EQ(para.layout.line_count, 2u);
}

TEST(LayoutContent, WordBreakBreakWordOverridesOverflowWrapForMinContent) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.overflow_wrap = GFNT_OVERFLOW_WRAP_BREAK_WORD;
  options.word_break = GFNT_WORD_BREAK_BREAK_WORD;
  Para para("ABCD", {font.face}, options);
  GFNT_F26Dot6 minimum = 0;

  ASSERT_EQ(gfnt_paragraph_min_content(para.p, &minimum), GFNT_OK);
  EXPECT_EQ(minimum, kLetter);
}

TEST(LayoutContent, WordBreakBreakWordKeepsAnAllowedSpaceOpportunity) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.word_break = GFNT_WORD_BREAK_BREAK_WORD;
  Para para("AB CD", {font.face}, options);

  ASSERT_EQ(para.lay(2 * kLetter + kSpace), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[0].end, 3u);
  EXPECT_EQ(para.layout.lines[1].start, 3u);
}

TEST(LayoutContent, AutoPhraseWithoutLanguageUsesNormalBreaking) {
  const std::string text = "今日は図書館で本を借りてから、友達と公園へ行く予定です。";
  std::vector<uint32_t> chars = {
      0x4ECA, 0x65E5, 0x306F, 0x56F3, 0x66F8, 0x9928, 0x3067, 0x672C, 0x3092,
      0x501F, 0x308A, 0x3066, 0x304B, 0x3089, 0x3001, 0x53CB, 0x9054, 0x3068,
      0x516C, 0x5712, 0x3078, 0x884C, 0x304F, 0x4E88, 0x5B9A, 0x3067, 0x3059, 0x3002};
  Font font(chars);
  GFNT_ParagraphOptions normal_options{};
  GFNT_ParagraphOptions phrase_options{};
  phrase_options.word_break = GFNT_WORD_BREAK_AUTO_PHRASE;
  const GFNT_Face * japanese_faces[] = {font.face};
  GFNT_TextSpan japanese_span{};
  japanese_span.length = text.size();
  japanese_span.style.faces = japanese_faces;
  japanese_span.style.face_count = 1;
  japanese_span.style.size = kSize;
  japanese_span.style.content_language = GFNT_CONTENT_LANGUAGE_JAPANESE;
  Para normal(text, {font.face}, normal_options);
  Para phrase_without_language(text, {font.face}, phrase_options);
  Para phrase_with_language(text, {japanese_span}, phrase_options);

  ASSERT_EQ(normal.created, GFNT_OK);
  ASSERT_EQ(phrase_without_language.created, GFNT_OK);
  ASSERT_EQ(phrase_with_language.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(normal.p, 4 * kLetter, &normal.layout, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(phrase_without_language.p, 4 * kLetter,
      &phrase_without_language.layout, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(phrase_with_language.p, 4 * kLetter,
      &phrase_with_language.layout, nullptr), GFNT_OK);
  ASSERT_EQ(normal.layout.line_count, phrase_without_language.layout.line_count);
  for (size_t i = 0; i < normal.layout.line_count; i++) {
    EXPECT_EQ(normal.layout.lines[i].start, phrase_without_language.layout.lines[i].start);
    EXPECT_EQ(normal.layout.lines[i].end, phrase_without_language.layout.lines[i].end);
  }
  EXPECT_NE(normal.layout.line_count, phrase_with_language.layout.line_count);
}

TEST(LayoutContent, TextWrapBalanceKeepsLineCountWithinForcedBreakGroups) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.text_wrap_style = GFNT_TEXT_WRAP_BALANCE;
  Para para("AB AB AB AB\nAB AB AB AB", {font.face}, options);
  Para greedy("AB AB AB AB\nAB AB AB AB", {font.face});
  const GFNT_F26Dot6 width = 6 * kLetter + 2 * kSpace;

  ASSERT_EQ(gfnt_paragraph_layout(greedy.p, width, &greedy.layout, nullptr), GFNT_OK);
  ASSERT_EQ(greedy.layout.line_count, 4u);
  EXPECT_EQ(greedy.layout.lines[0].start, 0u);
  EXPECT_EQ(greedy.layout.lines[0].end, 9u);
  EXPECT_EQ(greedy.layout.lines[1].start, 9u);
  EXPECT_EQ(greedy.layout.lines[2].start, 12u);
  EXPECT_EQ(greedy.layout.lines[2].end, 21u);
  EXPECT_EQ(greedy.layout.lines[3].start, 21u);

  ASSERT_EQ(gfnt_paragraph_layout(para.p, width,
      &para.layout, nullptr), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 4u);
  EXPECT_EQ(para.layout.lines[0].start, 0u);
  EXPECT_EQ(para.layout.lines[0].end, 6u);
  EXPECT_EQ(para.layout.lines[1].start, 6u);
  EXPECT_EQ(para.layout.lines[1].end, 12u);
  EXPECT_EQ(para.layout.lines[2].start, 12u);
  EXPECT_EQ(para.layout.lines[2].end, 18u);
  EXPECT_EQ(para.layout.lines[3].start, 18u);
  EXPECT_EQ(para.layout.lines[3].end, 23u);
}

TEST(LayoutContent, TextWrapPrettyUsesTheDocumentedBalancedPolicy) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.text_wrap_style = GFNT_TEXT_WRAP_PRETTY;
  const GFNT_F26Dot6 width = 6 * kLetter + 2 * kSpace;
  Para automatic("AB AB AB AB", {font.face});
  Para pretty("AB AB AB AB", {font.face}, options);

  ASSERT_EQ(gfnt_paragraph_layout(automatic.p, width, &automatic.layout, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(pretty.p, width, &pretty.layout, nullptr), GFNT_OK);
  ASSERT_EQ(automatic.layout.line_count, 2u);
  ASSERT_EQ(pretty.layout.line_count, 2u);
  EXPECT_EQ(automatic.layout.lines[0].end, 9u);
  EXPECT_EQ(pretty.layout.lines[0].start, 0u);
  EXPECT_EQ(pretty.layout.lines[0].end, 6u);
  EXPECT_EQ(pretty.layout.lines[1].start, 6u);
  EXPECT_EQ(pretty.layout.lines[1].end, 11u);
}

TEST(LayoutContent, TextWrapAddedPoliciesKeepShortAndUnboundedParagraphsValid) {
  Font font(latin());
  const GFNT_TextWrapStyle styles[] = {
      GFNT_TEXT_WRAP_PRETTY, GFNT_TEXT_WRAP_AVOID_SHORT_LAST_LINE};
  for (const GFNT_TextWrapStyle style : styles) {
    GFNT_ParagraphOptions options{};
    options.text_wrap_style = style;
    Para short_text("AB", {font.face}, options);
    Para wrapped("AB AB AB AB\nAB AB AB AB", {font.face}, options);
    ASSERT_EQ(short_text.created, GFNT_OK);
    ASSERT_EQ(wrapped.created, GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(short_text.p, 20 * kLetter,
        &short_text.layout, nullptr), GFNT_OK);
    EXPECT_EQ(short_text.layout.line_count, 1u);
    ASSERT_EQ(gfnt_paragraph_layout(wrapped.p, 6 * kLetter + 2 * kSpace,
        &wrapped.layout, nullptr), GFNT_OK);
    EXPECT_EQ(wrapped.layout.line_count, 4u);
    gfnt_layout_free(&wrapped.layout); /* an out layout is written, not released: the first one is the caller's */
    ASSERT_EQ(gfnt_paragraph_layout(wrapped.p, GFNT_LAYOUT_UNBOUNDED,
        &wrapped.layout, nullptr), GFNT_OK);
    EXPECT_EQ(wrapped.layout.line_count, 2u);
  }
}

TEST(LayoutContent, TextWrapAvoidShortLastLineMovesBreakWhenTheLastLineIsShort) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.text_wrap_style = GFNT_TEXT_WRAP_AVOID_SHORT_LAST_LINE;
  const GFNT_F26Dot6 width = 6 * kLetter + 2 * kSpace;
  Para greedy("AB AB AB AB", {font.face});
  Para avoiding("AB AB AB AB", {font.face}, options);

  ASSERT_EQ(gfnt_paragraph_layout(greedy.p, width, &greedy.layout, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(avoiding.p, width, &avoiding.layout, nullptr), GFNT_OK);
  ASSERT_EQ(greedy.layout.line_count, 2u);
  ASSERT_EQ(avoiding.layout.line_count, greedy.layout.line_count);
  EXPECT_EQ(greedy.layout.lines[0].end, 9u);
  EXPECT_EQ(avoiding.layout.lines[0].end, 6u);
  EXPECT_EQ(avoiding.layout.lines[1].start, 6u);
}

TEST(LayoutContent, TextWrapAvoidShortLastLineHandlesForcedBreakGroupsIndependently) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.text_wrap_style = GFNT_TEXT_WRAP_AVOID_SHORT_LAST_LINE;
  const GFNT_F26Dot6 width = 6 * kLetter + 2 * kSpace;
  Para para("AB AB AB AB\nAB AB AB AB", {font.face}, options);

  ASSERT_EQ(gfnt_paragraph_layout(para.p, width, &para.layout, nullptr), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 4u);
  EXPECT_EQ(para.layout.lines[0].start, 0u);
  EXPECT_EQ(para.layout.lines[0].end, 6u);
  EXPECT_EQ(para.layout.lines[1].start, 6u);
  EXPECT_EQ(para.layout.lines[1].end, 12u);
  EXPECT_EQ(para.layout.lines[2].start, 12u);
  EXPECT_EQ(para.layout.lines[2].end, 18u);
  EXPECT_EQ(para.layout.lines[3].start, 18u);
  EXPECT_EQ(para.layout.lines[3].end, 23u);
}

TEST(LayoutContent, TextWrapAvoidShortLastLineLeavesNoWordBoundaryMovesAlone) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.text_wrap_style = GFNT_TEXT_WRAP_AVOID_SHORT_LAST_LINE;
  options.overflow_wrap = GFNT_OVERFLOW_WRAP_ANYWHERE;
  options.line_break = GFNT_LINE_BREAK_ANYWHERE;
  GFNT_ParagraphOptions greedy_options = options;
  greedy_options.text_wrap_style = GFNT_TEXT_WRAP_AUTO;
  Para greedy("ABABA", {font.face}, greedy_options);
  Para avoiding("ABABA", {font.face}, options);

  ASSERT_EQ(gfnt_paragraph_layout(greedy.p, 2 * kLetter, &greedy.layout, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(avoiding.p, 2 * kLetter,
      &avoiding.layout, nullptr), GFNT_OK);
  ASSERT_EQ(avoiding.layout.line_count, greedy.layout.line_count);
  for (size_t i = 0; i < greedy.layout.line_count; i++) {
    EXPECT_EQ(avoiding.layout.lines[i].start, greedy.layout.lines[i].start);
    EXPECT_EQ(avoiding.layout.lines[i].end, greedy.layout.lines[i].end);
  }
}

TEST(LayoutContent, TextWrapAvoidShortLastLineLeavesAnUnmovableWordAlone) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.text_wrap_style = GFNT_TEXT_WRAP_AVOID_SHORT_LAST_LINE;
  Para greedy("ABCDEFG H", {font.face});
  Para avoiding("ABCDEFG H", {font.face}, options);

  ASSERT_EQ(gfnt_paragraph_layout(greedy.p, 5 * kLetter, &greedy.layout, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(avoiding.p, 5 * kLetter,
      &avoiding.layout, nullptr), GFNT_OK);
  ASSERT_EQ(greedy.layout.line_count, 2u);
  ASSERT_EQ(avoiding.layout.line_count, greedy.layout.line_count);
  EXPECT_EQ(greedy.layout.lines[0].end, 8u);
  EXPECT_EQ(avoiding.layout.lines[0].end, 8u);
  EXPECT_EQ(avoiding.layout.lines[1].start, 8u);
}

TEST(LayoutTabs, TextWrapAvoidShortLastLineReflowsAroundPositionedTabs) {
  Font font(latin());
  GFNT_ParagraphOptions greedy_options{};
  greedy_options.tab_width = 800;
  GFNT_ParagraphOptions avoiding_options = greedy_options;
  avoiding_options.text_wrap_style = GFNT_TEXT_WRAP_AVOID_SHORT_LAST_LINE;
  const GFNT_F26Dot6 width = 2000;
  const std::string text = "AB\tAB AB AB AB AB AB";
  Para greedy(text, {font.face}, greedy_options);
  Para avoiding(text, {font.face}, avoiding_options);

  ASSERT_EQ(gfnt_paragraph_layout(greedy.p, width, &greedy.layout, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(avoiding.p, width, &avoiding.layout, nullptr), GFNT_OK);
  ASSERT_GE(greedy.layout.line_count, 2u);
  ASSERT_EQ(avoiding.layout.line_count, greedy.layout.line_count);
  EXPECT_LT(greedy.layout.lines[greedy.layout.line_count - 1].extent, width / 3);
  EXPECT_LT(avoiding.layout.lines[avoiding.layout.line_count - 1].start,
      greedy.layout.lines[greedy.layout.line_count - 1].start);
  EXPECT_GT(avoiding.layout.lines[avoiding.layout.line_count - 1].extent,
      greedy.layout.lines[greedy.layout.line_count - 1].extent);
  for (size_t i = 0; i < avoiding.layout.line_count; i++) {
    EXPECT_LE(avoiding.layout.lines[i].extent, width);
  }
}

TEST(LayoutTabs, TextWrapBalanceMeasuresTabStopsAtEachCandidatePosition) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.text_wrap_style = GFNT_TEXT_WRAP_BALANCE;
  options.tab_width = 800;
  options.tabs_from_container = true;
  const GFNT_F26Dot6 width = 2000;
  Para para("AB\tAB AB AB AB AB", {font.face}, options);

  ASSERT_EQ(gfnt_paragraph_layout(para.p, width, &para.layout, nullptr), GFNT_OK);
  ASSERT_GE(para.layout.line_count, 2u);
  for (size_t i = 0; i < para.layout.line_count; i++) {
    EXPECT_LE(para.layout.lines[i].extent, width);
  }
}

TEST(LayoutAnnotation, TextWrapAvoidShortLastLineReflowsAnnotatedText) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation ruby{};
  ruby.base_start = 0;
  ruby.base_end = 2;
  ruby.kind = GFNT_ANNOTATION_RUBY;
  ruby.position = GFNT_ANNOTATION_OVER;
  ruby.style.faces = faces;
  ruby.style.face_count = 1;
  ruby.style.size = kSize;
  ruby.text = "A";
  ruby.text_length = 1;
  ruby.align = GFNT_ANNOTATION_ALIGN_CENTER;
  GFNT_ParagraphOptions greedy_options{};
  greedy_options.annotations = &ruby;
  greedy_options.annotation_count = 1;
  GFNT_ParagraphOptions avoiding_options = greedy_options;
  avoiding_options.text_wrap_style = GFNT_TEXT_WRAP_AVOID_SHORT_LAST_LINE;
  const GFNT_F26Dot6 width = 6 * kLetter + 2 * kSpace;
  Para greedy("AB AB AB AB", {font.face}, greedy_options);
  Para avoiding("AB AB AB AB", {font.face}, avoiding_options);

  ASSERT_EQ(gfnt_paragraph_layout(greedy.p, width, &greedy.layout, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(avoiding.p, width, &avoiding.layout, nullptr), GFNT_OK);
  ASSERT_EQ(greedy.layout.line_count, 2u);
  ASSERT_EQ(avoiding.layout.line_count, greedy.layout.line_count);
  EXPECT_LT(greedy.layout.lines[1].extent, width / 3);
  EXPECT_LT(avoiding.layout.lines[avoiding.layout.line_count - 1].start,
      greedy.layout.lines[greedy.layout.line_count - 1].start);
  EXPECT_GT(avoiding.layout.lines[avoiding.layout.line_count - 1].extent,
      greedy.layout.lines[greedy.layout.line_count - 1].extent);
  for (size_t i = 0; i < avoiding.layout.line_count; i++) {
    EXPECT_LE(avoiding.layout.lines[i].extent, width);
  }
}

TEST(LayoutAnnotation, TextWrapBalanceMeasuresRubyReservedAdvance) {
  Font font(latin());
  const GFNT_Face * faces[] = {font.face};
  GFNT_Annotation ruby{};
  ruby.base_start = 0;
  ruby.base_end = 2;
  ruby.kind = GFNT_ANNOTATION_RUBY;
  ruby.position = GFNT_ANNOTATION_OVER;
  ruby.style.faces = faces;
  ruby.style.face_count = 1;
  ruby.style.size = kSize;
  ruby.text = "ABCD";
  ruby.text_length = 4;
  ruby.align = GFNT_ANNOTATION_ALIGN_CENTER;
  GFNT_ParagraphOptions options{};
  options.text_wrap_style = GFNT_TEXT_WRAP_BALANCE;
  options.annotations = &ruby;
  options.annotation_count = 1;
  const GFNT_F26Dot6 width = 6 * kLetter + 2 * kSpace;
  Para para("AB AB AB AB", {font.face}, options);

  ASSERT_EQ(gfnt_paragraph_layout(para.p, width, &para.layout, nullptr), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);
  for (size_t i = 0; i < para.layout.line_count; i++) {
    EXPECT_LE(para.layout.lines[i].extent, width);
  }
}

TEST(LayoutContent, WhiteSpacePreKeepsSoftLineBreaksAndTrailingSpaces) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.white_space = GFNT_WHITE_SPACE_PRE;
  options.overflow_wrap = GFNT_OVERFLOW_WRAP_ANYWHERE;
  options.line_break = GFNT_LINE_BREAK_ANYWHERE;
  Para para("AB  \nC", {font.face}, options);
  GFNT_F26Dot6 minimum = 0, maximum = 0;

  ASSERT_EQ(gfnt_paragraph_min_content(para.p, &minimum), GFNT_OK);
  EXPECT_EQ(minimum, 2 * kLetter + 2 * kSpace);
  ASSERT_EQ(gfnt_paragraph_max_content(para.p, &maximum), GFNT_OK);
  EXPECT_EQ(maximum, 2 * kLetter + 2 * kSpace);
  ASSERT_EQ(para.lay(2 * kLetter), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[0].extent, 2 * kLetter + 2 * kSpace);
}

TEST(LayoutContent, WhiteSpacePreSuppressesOrdinarySpaceWrapping) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.white_space = GFNT_WHITE_SPACE_PRE;
  Para para("AB CD", {font.face}, options);

  ASSERT_EQ(para.lay(2 * kLetter + kSpace), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_EQ(para.layout.lines[0].end, 5u);
  EXPECT_EQ(para.layout.lines[0].extent, 4 * kLetter + kSpace);
}

TEST(LayoutContent, WhiteSpaceBreakSpacesWrapsAndCountsEachPreservedSpace) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.white_space = GFNT_WHITE_SPACE_BREAK_SPACES;
  Para para("AB  CD", {font.face}, options);
  GFNT_F26Dot6 minimum = 0, maximum = 0;

  ASSERT_EQ(gfnt_paragraph_min_content(para.p, &minimum), GFNT_OK);
  EXPECT_EQ(minimum, 2 * kLetter + kSpace);
  ASSERT_EQ(gfnt_paragraph_max_content(para.p, &maximum), GFNT_OK);
  EXPECT_EQ(maximum, 4 * kLetter + 2 * kSpace);
  ASSERT_EQ(para.lay(2 * kLetter + kSpace), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[0].end, 3u);
  EXPECT_EQ(para.layout.lines[1].start, 3u);
  EXPECT_EQ(para.layout.lines[0].extent, 2 * kLetter + kSpace);
  EXPECT_EQ(para.layout.lines[1].extent, 2 * kLetter + kSpace);
}

TEST(LayoutContent, WhiteSpaceBreakSpacesCountsTrailingSpaceBeforeHardBreak) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.white_space = GFNT_WHITE_SPACE_BREAK_SPACES;
  Para para("AB  \nA", {font.face}, options);

  ASSERT_EQ(para.lay(2 * kLetter + kSpace), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 3u);
  EXPECT_EQ(para.layout.lines[0].extent, 2 * kLetter + kSpace);
  EXPECT_EQ(para.layout.lines[1].extent, kSpace);
  EXPECT_EQ(para.layout.lines[2].extent, kLetter);
}

TEST(LayoutContent, WhiteSpaceNormalCollapsesAndWrapsWhitespace) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.white_space = GFNT_WHITE_SPACE_NORMAL;
  Para para(" A   B ", {font.face}, options);
  GFNT_F26Dot6 minimum = 0, maximum = 0;

  ASSERT_EQ(gfnt_paragraph_min_content(para.p, &minimum), GFNT_OK);
  EXPECT_EQ(minimum, kLetter);
  ASSERT_EQ(gfnt_paragraph_max_content(para.p, &maximum), GFNT_OK);
  EXPECT_EQ(maximum, 2 * kLetter + kSpace);
  ASSERT_EQ(para.lay(2 * kLetter + kSpace), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_EQ(para.layout.lines[0].extent, 2 * kLetter + kSpace);
}

TEST(LayoutContent, WhiteSpaceNowrapCollapsesAndKeepsOneLine) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.white_space = GFNT_WHITE_SPACE_NOWRAP;
  Para para("A   AB\nB", {font.face}, options);
  GFNT_F26Dot6 minimum = 0, maximum = 0;

  ASSERT_EQ(gfnt_paragraph_min_content(para.p, &minimum), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(para.p, &maximum), GFNT_OK);
  EXPECT_EQ(minimum, 2 * kLetter);
  EXPECT_EQ(maximum, 4 * kLetter + 2 * kSpace);
  ASSERT_EQ(para.lay(2 * kLetter), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_EQ(para.layout.lines[0].extent, maximum);
}

TEST(LayoutContent, WhiteSpacePreLineCollapsesSpacesAndPreservesBreaks) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.white_space = GFNT_WHITE_SPACE_PRE_LINE;
  Para para("A   B\n C", {font.face}, options);
  GFNT_F26Dot6 minimum = 0, maximum = 0;

  ASSERT_EQ(gfnt_paragraph_min_content(para.p, &minimum), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(para.p, &maximum), GFNT_OK);
  EXPECT_EQ(minimum, kLetter);
  EXPECT_EQ(maximum, 2 * kLetter + kSpace);
  ASSERT_EQ(para.lay(maximum), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);
  EXPECT_EQ(para.layout.lines[0].extent, maximum);
  EXPECT_EQ(para.layout.lines[1].extent, kLetter);
}

TEST(LayoutContent, WhiteSpacePreKeepsConsecutiveAndTrailingHardBreaks) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.white_space = GFNT_WHITE_SPACE_PRE;
  options.overflow_wrap = GFNT_OVERFLOW_WRAP_ANYWHERE;
  Para para("A\n\nB\n", {font.face}, options);

  ASSERT_EQ(para.lay(kLetter), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 4u);
  EXPECT_EQ(para.layout.lines[1].extent, 0);
  EXPECT_EQ(para.layout.lines[3].run_count, 0u);
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

TEST(LayoutJustify, WhiteSpacePreAndAlignLastJustifyIncludeTrailingSpaces) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.white_space = GFNT_WHITE_SPACE_PRE;
  options.align = GFNT_ALIGN_JUSTIFY;
  options.align_last = GFNT_ALIGN_LAST_JUSTIFY;
  Para para("AB  \nCD  \nEF", {font.face}, options);
  const GFNT_F26Dot6 width = 200 * 64;

  ASSERT_EQ(para.lay(width), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 3u);
  EXPECT_TRUE(para.layout.lines[0].justified);
  EXPECT_EQ(para.layout.lines[0].extent, width);
  EXPECT_TRUE(para.layout.lines[1].justified);
  EXPECT_EQ(para.layout.lines[1].extent, width);
  EXPECT_FALSE(para.layout.lines[2].justified);
  EXPECT_EQ(para.layout.lines[2].extent, 2 * kLetter);
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

TEST(LayoutSpacing, WordSpacingIsAppliedToSpaceBeforeBreakingAndMeasurement) {
  Font font(latin());
  GFNT_TextSpan span = plain_span(font, 3);
  Para ordinary("A B", {span});
  ASSERT_EQ(ordinary.created, GFNT_OK);
  GFNT_F26Dot6 ordinary_max = 0;
  ASSERT_EQ(gfnt_paragraph_max_content(ordinary.p, &ordinary_max), GFNT_OK);
  ASSERT_EQ(ordinary.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);

  span.style.word_spacing = 64;
  Para spaced("A B", {span});
  ASSERT_EQ(spaced.created, GFNT_OK);
  GFNT_F26Dot6 spaced_max = 0;
  ASSERT_EQ(gfnt_paragraph_max_content(spaced.p, &spaced_max), GFNT_OK);
  ASSERT_EQ(spaced.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  EXPECT_EQ(spaced_max, ordinary_max + 64);
  EXPECT_EQ(spaced.layout.lines[0].extent, ordinary.layout.lines[0].extent + 64);
  EXPECT_EQ(spaced.layout.lines[0].runs[0].glyphs[2].pen_x,
      ordinary.layout.lines[0].runs[0].glyphs[2].pen_x + 64);

  span.style.word_spacing = -32;
  Para tight("A B", {span});
  ASSERT_EQ(tight.created, GFNT_OK);
  GFNT_F26Dot6 tight_max = 0;
  ASSERT_EQ(gfnt_paragraph_max_content(tight.p, &tight_max), GFNT_OK);
  ASSERT_EQ(tight.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  EXPECT_EQ(tight_max, ordinary_max - 32);
  EXPECT_EQ(tight.layout.lines[0].extent, ordinary.layout.lines[0].extent - 32);
}

TEST(LayoutSpacing, TextAutospaceIsAddedOnlyWhenThePairStaysOnTheLine) {
  Font font({0x65E5, 'A', '1'});
  GFNT_ParagraphOptions normal_options{};
  Para normal("日A", {font.face}, normal_options);
  ASSERT_EQ(normal.created, GFNT_OK);
  GFNT_F26Dot6 minimum = 0, maximum = 0;
  ASSERT_EQ(gfnt_paragraph_min_content(normal.p, &minimum), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(normal.p, &maximum), GFNT_OK);
  EXPECT_EQ(minimum, kLetter);
  EXPECT_EQ(maximum, 2 * kLetter + kLetter / 8);
  ASSERT_EQ(normal.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(normal.layout.lines[0].run_count, 2u);
  EXPECT_EQ(normal.layout.lines[0].extent, maximum);
  EXPECT_EQ(normal.layout.lines[0].runs[0].glyphs[0].advance, kLetter + kLetter / 8);
  EXPECT_EQ(normal.layout.lines[0].runs[1].glyphs[0].pen_x, kLetter + kLetter / 8);

  ASSERT_EQ(normal.lay(kLetter), GFNT_OK);
  ASSERT_EQ(normal.layout.line_count, 2u);
  EXPECT_EQ(normal.layout.lines[0].extent, kLetter);
  EXPECT_EQ(normal.layout.lines[0].runs[0].glyphs[0].advance, kLetter);

  GFNT_ParagraphOptions disabled_options{};
  disabled_options.text_autospace = GFNT_TEXT_AUTOSPACE_NONE;
  Para disabled("日A", {font.face}, disabled_options);
  ASSERT_EQ(disabled.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(disabled.p, &maximum), GFNT_OK);
  EXPECT_EQ(maximum, 2 * kLetter);
  ASSERT_EQ(disabled.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  EXPECT_EQ(disabled.layout.lines[0].extent, 2 * kLetter);
}

TEST(LayoutSpacing, TextAutospaceCanSelectTheLetterAndNumberClasses) {
  Font font({0x65E5, 'A', '1'});
  GFNT_ParagraphOptions alpha_options{};
  alpha_options.text_autospace = GFNT_TEXT_AUTOSPACE_ALPHA;
  GFNT_F26Dot6 width = 0;
  Para alpha("日A", {font.face}, alpha_options);
  ASSERT_EQ(alpha.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(alpha.p, &width), GFNT_OK);
  EXPECT_EQ(width, 2 * kLetter + kLetter / 8);
  Para alpha_number("日1", {font.face}, alpha_options);
  ASSERT_EQ(alpha_number.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(alpha_number.p, &width), GFNT_OK);
  EXPECT_EQ(width, 2 * kLetter);

  GFNT_ParagraphOptions numeric_options{};
  numeric_options.text_autospace = GFNT_TEXT_AUTOSPACE_NUMERIC;
  Para numeric("日1", {font.face}, numeric_options);
  ASSERT_EQ(numeric.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(numeric.p, &width), GFNT_OK);
  EXPECT_EQ(width, 2 * kLetter + kLetter / 8);
  Para numeric_alpha("日A", {font.face}, numeric_options);
  ASSERT_EQ(numeric_alpha.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(numeric_alpha.p, &width), GFNT_OK);
  EXPECT_EQ(width, 2 * kLetter);
}

TEST(LayoutSpacing, TextSpacingTrimCollapsesAdjacentPunctuationAndHonorsSpaceAll) {
  Font font({0x65E5, 0x300C, 0x300D});
  GFNT_F26Dot6 width = 0;

  Para normal("「「日」」", {font.face});
  ASSERT_EQ(normal.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(normal.p, &width), GFNT_OK);
  EXPECT_EQ(width, 4 * kLetter);
  ASSERT_EQ(normal.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(normal.layout.line_count, 1u);
  const GFNT_LineRun & normal_run = normal.layout.lines[0].runs[0];
  ASSERT_EQ(normal_run.glyph_count, 5u);
  EXPECT_EQ(normal_run.glyphs[0].advance, kLetter);
  EXPECT_EQ(normal_run.glyphs[1].advance, kLetter / 2);
  EXPECT_EQ(normal_run.glyphs[2].advance, kLetter);
  EXPECT_EQ(normal_run.glyphs[3].advance, kLetter / 2);
  EXPECT_EQ(normal_run.glyphs[4].advance, kLetter);

  GFNT_ParagraphOptions options{};
  options.text_spacing_trim = GFNT_TEXT_SPACING_TRIM_SPACE_ALL;
  Para all("「「日」」", {font.face}, options);
  ASSERT_EQ(all.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(all.p, &width), GFNT_OK);
  EXPECT_EQ(width, 5 * kLetter);
  ASSERT_EQ(all.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(all.layout.line_count, 1u);
  EXPECT_EQ(all.layout.lines[0].extent, 5 * kLetter);
}

TEST(LayoutSpacing, TextSpacingTrimStartAppliesAtParagraphAndForcedLineStarts) {
  Font font({0x65E5, 0x300C});
  GFNT_ParagraphOptions trim_options{};
  trim_options.text_spacing_trim = GFNT_TEXT_SPACING_TRIM_TRIM_START;
  Para trim("日\n「日", {font.face}, trim_options);
  ASSERT_EQ(trim.created, GFNT_OK);
  ASSERT_EQ(trim.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(trim.layout.line_count, 2u);
  ASSERT_EQ(trim.layout.lines[1].runs[0].glyph_count, 2u);
  EXPECT_EQ(trim.layout.lines[1].runs[0].glyphs[0].advance, kLetter / 2);
  EXPECT_EQ(trim.layout.lines[1].runs[0].glyphs[1].pen_x, kLetter / 2);

  GFNT_ParagraphOptions first_options{};
  first_options.text_spacing_trim = GFNT_TEXT_SPACING_TRIM_SPACE_FIRST;
  Para first("日\n「日", {font.face}, first_options);
  ASSERT_EQ(first.created, GFNT_OK);
  ASSERT_EQ(first.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(first.layout.line_count, 2u);
  EXPECT_EQ(first.layout.lines[1].runs[0].glyphs[0].advance, kLetter);
}

TEST(LayoutSpacing, TextSpacingTrimSpaceFirstKeepsForcedBreaksButTrimsWrappedStarts) {
  Font font({0x65E5, 0x300C});
  GFNT_ParagraphOptions first_options{};
  first_options.text_spacing_trim = GFNT_TEXT_SPACING_TRIM_SPACE_FIRST;
  Para first("日日「日", {font.face}, first_options);
  ASSERT_EQ(first.created, GFNT_OK);
  ASSERT_EQ(first.lay(2 * kLetter), GFNT_OK);
  ASSERT_EQ(first.layout.line_count, 2u);
  ASSERT_EQ(first.layout.lines[1].runs[0].glyph_count, 2u);
  EXPECT_EQ(first.layout.lines[1].runs[0].glyphs[0].advance, kLetter / 2);

  Para normal("日日「日", {font.face});
  ASSERT_EQ(normal.created, GFNT_OK);
  ASSERT_EQ(normal.lay(2 * kLetter), GFNT_OK);
  ASSERT_EQ(normal.layout.line_count, 2u);
  EXPECT_EQ(normal.layout.lines[1].runs[0].glyphs[0].advance, kLetter);
}

TEST(LayoutSpacing, TextSpacingTrimNormalUsesHalfWidthClosingMarkOnlyToFit) {
  Font font({0x65E5, 0x300D});
  const std::string text = "日日日」";
  Para normal(text, {font.face});
  ASSERT_EQ(normal.created, GFNT_OK);
  GFNT_F26Dot6 maximum = 0;
  ASSERT_EQ(gfnt_paragraph_max_content(normal.p, &maximum), GFNT_OK);
  EXPECT_EQ(maximum, 4 * kLetter);
  ASSERT_EQ(normal.lay(3 * kLetter + kLetter / 2), GFNT_OK);
  ASSERT_EQ(normal.layout.line_count, 1u);
  EXPECT_EQ(normal.layout.lines[0].extent, 3 * kLetter + kLetter / 2);
  ASSERT_EQ(normal.layout.lines[0].runs[0].glyph_count, 4u);
  EXPECT_EQ(normal.layout.lines[0].runs[0].glyphs[3].advance, kLetter / 2);

  GFNT_ParagraphOptions all_options{};
  all_options.text_spacing_trim = GFNT_TEXT_SPACING_TRIM_SPACE_ALL;
  Para all(text, {font.face}, all_options);
  ASSERT_EQ(all.created, GFNT_OK);
  ASSERT_EQ(all.lay(3 * kLetter + kLetter / 2), GFNT_OK);
  EXPECT_EQ(all.layout.line_count, 2u);
}

TEST(LayoutSpacing, TextSpacingTrimUsesJapaneseColonClass) {
  Font font({0x300D, 0xFF1A});
  const std::string text = "日：」";
  const GFNT_Face * faces[] = {font.face};
  GFNT_TextSpan span{};
  span.length = text.size();
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = kSize;
  span.style.content_language = GFNT_CONTENT_LANGUAGE_JAPANESE;
  Para japanese(text, {span});
  ASSERT_EQ(japanese.created, GFNT_OK);
  ASSERT_EQ(japanese.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(japanese.layout.line_count, 1u);
  EXPECT_EQ(japanese.layout.lines[0].extent, 3 * kLetter);

  span.style.content_language = GFNT_CONTENT_LANGUAGE_UNSPECIFIED;
  Para unspecified(text, {span});
  ASSERT_EQ(unspecified.created, GFNT_OK);
  ASSERT_EQ(unspecified.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(unspecified.layout.line_count, 1u);
  EXPECT_EQ(unspecified.layout.lines[0].extent, 5 * kLetter / 2);
}

TEST(LayoutSpacing, TextSpacingTrimKeepsACombiningMarkClusterPlaceable) {
  Font font({0x65E5, 0x300D, 0x0301});
  const std::string text = "日日」\xCC\x81";
  Para para(text, {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_Error error{};
  GFNT_Result result = gfnt_paragraph_layout(para.p, 2 * kLetter + kLetter / 2,
      &para.layout, &error);
  ASSERT_EQ(result, GFNT_OK) << (error.message ? error.message : "no diagnostic");
  snapshot_layout(text, 2 * kLetter + kLetter / 2, para.layout);
  ASSERT_EQ(para.layout.line_count, 1u);
  EXPECT_EQ(para.layout.lines[0].extent, 2 * kLetter + kLetter / 2);
}

TEST(LayoutDecoration, ResolvesRangesAndSkipsGlyphInk) {
  Font font(latin(), true);
  Para para("A B", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);

  GFNT_DecorationLayout continuous{};
  ASSERT_EQ(gfnt_paragraph_decoration_layout(para.p, &para.layout, 0, para.text.size(),
                GFNT_DECORATION_UNDERLINE, -2 * 64, 64,
                GFNT_DECORATION_SKIP_INK_NONE, &continuous, nullptr), GFNT_OK);
  ASSERT_EQ(continuous.segment_count, 1u);
  EXPECT_EQ(continuous.segments[0].line, 0u);
  EXPECT_EQ(continuous.segments[0].box.x, 0);
  EXPECT_EQ(continuous.segments[0].box.y, para.layout.lines[0].baseline - 2 * 64);
  EXPECT_EQ(continuous.segments[0].box.width, 5 * kLetter / 2);
  EXPECT_EQ(continuous.segments[0].box.height, 64);

  GFNT_DecorationLayout skipped{};
  ASSERT_EQ(gfnt_paragraph_decoration_layout(para.p, &para.layout, 0, para.text.size(),
                GFNT_DECORATION_UNDERLINE, -2 * 64, 64,
                GFNT_DECORATION_SKIP_INK_ALL, &skipped, nullptr), GFNT_OK);
  ASSERT_GT(skipped.segment_count, 0u);
  GFNT_F26Dot6 skipped_width = 0;
  for (size_t i = 0; i < skipped.segment_count; i++) {
    skipped_width += skipped.segments[i].box.width;
  }
  EXPECT_LT(skipped_width, continuous.segments[0].box.width);

  GFNT_DecorationLayout strike{};
  ASSERT_EQ(gfnt_paragraph_decoration_layout(para.p, &para.layout, 0, para.text.size(),
                GFNT_DECORATION_LINE_THROUGH, -2 * 64, 64,
                GFNT_DECORATION_SKIP_INK_ALL, &strike, nullptr), GFNT_OK);
  ASSERT_EQ(strike.segment_count, 1u);
  EXPECT_EQ(strike.segments[0].box.width, continuous.segments[0].box.width);

  GFNT_DecorationLayout range{};
  ASSERT_EQ(gfnt_paragraph_decoration_layout(para.p, &para.layout, 2, 3,
                GFNT_DECORATION_OVERLINE, -8 * 64, 64,
                GFNT_DECORATION_SKIP_INK_NONE, &range, nullptr), GFNT_OK);
  ASSERT_EQ(range.segment_count, 1u);
  EXPECT_EQ(range.segments[0].box.x, 3 * kLetter / 2);
  EXPECT_EQ(range.segments[0].box.width, kLetter);

  GFNT_DecorationLayout space_range{};
  ASSERT_EQ(gfnt_paragraph_decoration_layout(para.p, &para.layout, 1, 2,
                GFNT_DECORATION_UNDERLINE, -2 * 64, 64,
                GFNT_DECORATION_SKIP_INK_NONE, &space_range, nullptr), GFNT_OK);
  ASSERT_EQ(space_range.segment_count, 1u);
  EXPECT_EQ(space_range.segments[0].box.x, kLetter);
  EXPECT_EQ(space_range.segments[0].box.width, kSpace);

  gfnt_decoration_layout_free(&continuous);
  gfnt_decoration_layout_free(&skipped);
  gfnt_decoration_layout_free(&strike);
  gfnt_decoration_layout_free(&range);
  gfnt_decoration_layout_free(&space_range);
}

TEST(LayoutDecoration, RejectsMismatchedLayoutsAndInvalidRanges) {
  Font font({0x65E5, 'A'});
  Para para("日A", {font.face});
  Para other("日A", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(other.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(other.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  GFNT_DecorationLayout result{};

  EXPECT_EQ(gfnt_paragraph_decoration_layout(para.p, &other.layout, 0, 2,
                GFNT_DECORATION_UNDERLINE, 0, 64,
                GFNT_DECORATION_SKIP_INK_NONE, &result, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_paragraph_decoration_layout(para.p, &para.layout, 1, 4,
                GFNT_DECORATION_UNDERLINE, 0, 64,
                GFNT_DECORATION_SKIP_INK_NONE, &result, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_paragraph_decoration_layout(para.p, &para.layout, 0, 2,
                GFNT_DECORATION_UNDERLINE, 0, 0,
                GFNT_DECORATION_SKIP_INK_NONE, &result, nullptr), GFNT_ERR_INVALID);

  GFNT_DecorationLayout japanese{};
  ASSERT_EQ(gfnt_paragraph_decoration_layout(para.p, &para.layout, 0, 3,
                GFNT_DECORATION_UNDERLINE, 0, 64,
                GFNT_DECORATION_SKIP_INK_NONE, &japanese, nullptr), GFNT_OK);
  ASSERT_EQ(japanese.segment_count, 1u);
  GFNT_F26Dot6 japanese_advance = 0;
  for (size_t ri = 0; ri < para.layout.lines[0].run_count; ri++) {
    for (size_t gi = 0; gi < para.layout.lines[0].runs[ri].glyph_count; gi++) {
      const GFNT_PlacedGlyph & glyph = para.layout.lines[0].runs[ri].glyphs[gi];
      if (glyph.cluster == 0) japanese_advance = glyph.advance;
    }
  }
  ASSERT_GT(japanese_advance, 0);
  EXPECT_EQ(japanese.segments[0].box.x, 0);
  EXPECT_EQ(japanese.segments[0].box.width, japanese_advance);
  gfnt_decoration_layout_free(&japanese);
}

TEST(LayoutDecoration, MapsVerticalAndSidewaysInkToTheInlineAxis) {
  Font font(latin(), true);
  for (const auto & mode : {std::pair{GFNT_WRITING_VERTICAL_RL, GFNT_ORIENT_UPRIGHT},
           std::pair{GFNT_WRITING_VERTICAL_RL, GFNT_ORIENT_SIDEWAYS},
           std::pair{GFNT_WRITING_SIDEWAYS_LR, GFNT_ORIENT_MIXED}}) {
    GFNT_ParagraphOptions options{};
    options.writing_mode = mode.first;
    options.text_orientation = mode.second;
    Para para("AB", {font.face}, options);
    ASSERT_EQ(para.created, GFNT_OK);
    ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
    ASSERT_EQ(para.layout.line_count, 1u);

    GFNT_DecorationLayout continuous{};
    ASSERT_EQ(gfnt_paragraph_decoration_layout(para.p, &para.layout, 0, para.text.size(),
                  GFNT_DECORATION_UNDERLINE, 64, 64,
                  GFNT_DECORATION_SKIP_INK_NONE, &continuous, nullptr), GFNT_OK);
    ASSERT_EQ(continuous.segment_count, 1u);
    EXPECT_EQ(continuous.segments[0].box.x, para.layout.lines[0].baseline + 64);
    EXPECT_EQ(continuous.segments[0].box.width, 64);
    EXPECT_GT(continuous.segments[0].box.height, 0);

    GFNT_DecorationLayout skipped{};
    ASSERT_EQ(gfnt_paragraph_decoration_layout(para.p, &para.layout, 0, para.text.size(),
                  GFNT_DECORATION_UNDERLINE, 64, 64,
                  GFNT_DECORATION_SKIP_INK_ALL, &skipped, nullptr), GFNT_OK);
    GFNT_F26Dot6 visible = 0;
    for (size_t i = 0; i < skipped.segment_count; i++) {
      EXPECT_EQ(skipped.segments[i].box.x, para.layout.lines[0].baseline + 64);
      EXPECT_EQ(skipped.segments[i].box.width, 64);
      visible += skipped.segments[i].box.height;
    }
    EXPECT_LT(visible, continuous.segments[0].box.height);
    gfnt_decoration_layout_free(&continuous);
    gfnt_decoration_layout_free(&skipped);
  }
}

TEST(LayoutDecoration, KeepsBidiRangeSegmentsDisjoint) {
  Font font(latin_and_hebrew());
  GFNT_ParagraphOptions options{};
  options.direction = GFNT_BIDI_LTR;
  Para para("ABאב", {font.face}, options);
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 1u);

  const GFNT_PlacedGlyph * alef = nullptr, * bet = nullptr;
  for (size_t ri = 0; ri < para.layout.lines[0].run_count; ri++) {
    const GFNT_LineRun & run = para.layout.lines[0].runs[ri];
    for (size_t gi = 0; gi < run.glyph_count; gi++) {
      const GFNT_PlacedGlyph & glyph = run.glyphs[gi];
      if (glyph.cluster == 2) alef = &glyph;
      if (glyph.cluster == 4) bet = &glyph;
    }
  }
  ASSERT_NE(alef, nullptr);
  ASSERT_NE(bet, nullptr);
  EXPECT_LT(bet->pen_x, alef->pen_x);

  GFNT_DecorationLayout selected{};
  ASSERT_EQ(gfnt_paragraph_decoration_layout(para.p, &para.layout, 0, 4,
                GFNT_DECORATION_UNDERLINE, 0, 64,
                GFNT_DECORATION_SKIP_INK_NONE, &selected, nullptr), GFNT_OK);
  ASSERT_EQ(selected.segment_count, 2u);
  EXPECT_EQ(selected.segments[0].box.x, 0);
  EXPECT_EQ(selected.segments[0].box.width, 2 * kLetter);
  EXPECT_EQ(selected.segments[1].box.x, alef->pen_x);
  EXPECT_EQ(selected.segments[1].box.width, alef->advance);
  EXPECT_EQ(selected.segments[0].box.x + selected.segments[0].box.width, bet->pen_x);
  EXPECT_EQ(bet->pen_x + bet->advance, selected.segments[1].box.x);
  gfnt_decoration_layout_free(&selected);
}

TEST(LayoutDecoration, ResetsGlyphInkStateBetweenLines) {
  Font font(latin(), true);
  Para para("ABX A B", {font.face});
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(3 * kLetter), GFNT_OK);
  ASSERT_EQ(para.layout.line_count, 2u);

  GFNT_DecorationLayout skipped{};
  ASSERT_EQ(gfnt_paragraph_decoration_layout(para.p, &para.layout, 0, para.text.size(),
                GFNT_DECORATION_UNDERLINE, -2 * 64, 64,
                GFNT_DECORATION_SKIP_INK_ALL, &skipped, nullptr), GFNT_OK);
  GFNT_F26Dot6 second_line_visible = 0;
  for (size_t i = 0; i < skipped.segment_count; i++) {
    if (skipped.segments[i].line == 1) {
      second_line_visible += skipped.segments[i].box.width;
    }
  }
  EXPECT_GT(second_line_visible, 0);
  gfnt_decoration_layout_free(&skipped);
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

TEST(LayoutTabs, ContainerOriginIncludesTheFirstLineIndent) {
  Font font(latin());
  GFNT_ParagraphOptions options{};
  options.tab_width = 1000;
  options.text_indent = 200;
  Para line_origin("A\tB", {font.face}, options);
  ASSERT_EQ(line_origin.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  const auto line_tab = line_origin.layout.lines[0].runs[0].glyphs[1].advance;

  options.tabs_from_container = true;
  Para container_origin("A\tB", {font.face}, options);
  ASSERT_EQ(container_origin.lay(GFNT_LAYOUT_UNBOUNDED), GFNT_OK);
  const auto container_tab = container_origin.layout.lines[0].runs[0].glyphs[1].advance;
  EXPECT_EQ(line_tab, 680);
  EXPECT_EQ(container_tab, 480);
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

TEST(LayoutHyphen, MinContentNeverCountsAHyphenAtTheEndOfTheTextAndNeverExceedsMaxContent) {
  Font font(latin_hyphen_ascii_only());
  // The soft hyphen after C is the last character: there is no line after it to break to.
  Para para("ABC\xC2\xAD", {font.face}, hyphenating(GFNT_HYPHENS_MANUAL));
  ASSERT_EQ(para.created, GFNT_OK);
  GFNT_F26Dot6 min = 0, max = 0;
  ASSERT_EQ(gfnt_paragraph_min_content(para.p, &min), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(para.p, &max), GFNT_OK);
  EXPECT_EQ(max, 3 * kLetter);
  EXPECT_EQ(min, 3 * kLetter) << "no hyphen is shown there";
  // A tab at the start of a stretch is a full stop wide there, and the way to the next stop only
  // in the unbroken line; the narrowest width that does not overflow cannot be more than that.
  Font tabbed({'A', 'B', ' ', '\t'});
  Para tab("AB \tA", {tabbed.face});
  ASSERT_EQ(tab.created, GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_min_content(tab.p, &min), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_max_content(tab.p, &max), GFNT_OK);
  EXPECT_LE(min, max);
}

TEST(LayoutHyphen, ThirtyLinesThatEndInAHyphenHaveRoomForTheirHyphenGlyphs) {
  Font font(latin_hyphen_ascii_only());
  // More hyphen glyphs than the layout once had room for beyond the text's own: found by the layout
  // fuzzer, as an out-of-bounds write that only a sanitizer sees.
  std::string text;
  for (int i = 0; i < 40; ++i) {
    text += "A\xC2\xAD";
  }
  Para para(text, {font.face}, hyphenating(GFNT_HYPHENS_MANUAL));
  ASSERT_EQ(para.created, GFNT_OK);
  ASSERT_EQ(para.lay(kLetter + kLetter / 2), GFNT_OK);
  size_t hyphens = 0;
  for (size_t i = 0; i < para.layout.line_count; ++i) {
    for (size_t k = 0; k < para.layout.lines[i].run_count; ++k) {
      hyphens += para.layout.lines[i].runs[k].hyphen ? para.layout.lines[i].runs[k].glyph_count : 0u;
    }
  }
  EXPECT_EQ(hyphens, para.layout.line_count - 1);
  EXPECT_GE(para.layout.line_count, 30u);
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
