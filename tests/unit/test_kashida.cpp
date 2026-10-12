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
 * Where a word is stretched for kashida justification. The cases are the test strings of
 * the Chromium kashida patch (CL 7957320) with the stretch points its published page shows,
 * so the priority scheme is held to a second implementation; the page numbers a place by the
 * letter it stretches in front of, as this library does, except that for a tatweel the author
 * wrote it names the place after the stroke and this library the place before it (the same
 * picture). notes/font/KASHIDA.md.
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/layout.h>

extern "C" {
#include "../../src/layout/kashida.h"
}

namespace {

std::vector<uint32_t> cps(const char * utf8) {
  std::vector<uint32_t> out;
  for (const unsigned char * p = reinterpret_cast<const unsigned char *>(utf8); *p;) {
    uint32_t c;
    int n;
    if (*p < 0x80) { c = *p; n = 1; }
    else if (*p < 0xE0) { c = *p & 0x1F; n = 2; }
    else if (*p < 0xF0) { c = *p & 0x0F; n = 3; }
    else { c = *p & 0x07; n = 4; }
    for (int i = 1; i < n; i++) c = (c << 6) | (p[i] & 0x3F);
    out.push_back(c);
    p += n;
  }
  return out;
}

// -1 if the word is left alone, else the index stretched in front of.
long point(const char * word, int * priority = nullptr) {
  std::vector<uint32_t> w = cps(word);
  size_t at = 0;
  GFNT_KashidaPriority pr;
  if (!gfnt_kashida_point(w.data(), w.size(), &at, &pr, nullptr, nullptr)) return -1;
  if (priority) *priority = pr;
  return static_cast<long>(at);
}

TEST(Kashida, ChromiumPatchWords) {
  EXPECT_EQ(point("سلسبيل"), 3);        // after the second seen
  EXPECT_EQ(point("بسم"), 2);           // after the seen
  EXPECT_EQ(point("كتب"), 2);           // before the final beh
  EXPECT_EQ(point("هلال"), 1);          // before the lam-alef ligature, which stays whole
  EXPECT_EQ(point("كتاب"), 2);          // before the final alef
  EXPECT_EQ(point("متن‌زا"), 2);   // a ZWNJ does not block it
  EXPECT_EQ(point("بانه"), 3);          // a final heh beats a final alef
  EXPECT_EQ(point("بانح"), 1);          // a final alef beats a final jeem
  EXPECT_EQ(point("بانع"), 1);          // ... and a final ain
}

TEST(Kashida, LeftAlone) {
  EXPECT_EQ(point("الله"), -1);         // the lam-lam-heh ligature
  EXPECT_EQ(point("زر"), -1);           // nothing connects
  EXPECT_EQ(point("ورد"), -1);
  EXPECT_EQ(point("ژژژ"), -1);
  EXPECT_EQ(point("لا"), -1);           // one ligature
  EXPECT_EQ(point("١٢٣٤٥"), -1);        // digits do not join
  EXPECT_EQ(point("۱۲۳۴۵"), -1);
  EXPECT_EQ(point("2.4"), -1);
  EXPECT_EQ(point(""), -1);
}

TEST(Kashida, AuthorsTatweelWinsOutright) {
  int priority = 0;
  EXPECT_EQ(point("كـتاب", &priority), 2);   // the stroke is extended: the place is after it
  EXPECT_EQ(priority, GFNT_KASHIDA_AUTHOR);
  EXPECT_EQ(point("كــــتاب"), 5);           // a run of strokes is one stroke
  EXPECT_EQ(point("بسـم"), 3);               // over the seen
  EXPECT_EQ(point("بسم\u0640"), 3);           // at the end of a word: in front of the stroke
}

TEST(Kashida, VowelMarksDoNotBlock) {
  // kataba, vowelled: each mark stays with its letter, the stretch goes before the final beh
  EXPECT_EQ(point("كَتَبَ"), 4);
  EXPECT_EQ(point("نَصٌّ"), 2);       // before the final sad, over its marks
}

TEST(Kashida, Priorities) {
  int p = 0;
  point("بسم", &p);   EXPECT_EQ(p, GFNT_KASHIDA_AFTER_SEEN);
  point("بانه", &p);  EXPECT_EQ(p, GFNT_KASHIDA_BEFORE_HEH);
  point("كتاب", &p);  EXPECT_EQ(p, GFNT_KASHIDA_BEFORE_ALEF);
  point("بانع", &p);  EXPECT_EQ(p, GFNT_KASHIDA_BEFORE_ALEF);   // the alef; the ain is class 6
  point("هلال", &p);  EXPECT_EQ(p, GFNT_KASHIDA_MEDIAL);
  point("كتب", &p);   EXPECT_EQ(p, GFNT_KASHIDA_BEFORE_FINAL);
}

TEST(Kashida, AVetoFallsBackToTheNextBest) {
  std::vector<uint32_t> w = cps("بانه");
  size_t at = 0;
  GFNT_KashidaPriority pr;
  auto no3 = [](void *, size_t before) { return before != 3; };
  ASSERT_TRUE(gfnt_kashida_point(w.data(), w.size(), &at, &pr, no3, nullptr));
  EXPECT_EQ(at, 1u);                          // the alef, once the heh is refused
  EXPECT_EQ(pr, GFNT_KASHIDA_BEFORE_ALEF);
  auto none = [](void *, size_t) { return false; };
  EXPECT_FALSE(gfnt_kashida_point(w.data(), w.size(), &at, &pr, none, nullptr));
}

TEST(Kashida, MedialBehBeforeRehOrYeh) {
  int p = 0;
  // beh-beh-reh: the middle beh is medial and followed by a reh
  EXPECT_EQ(point("ببر", &p), 1);
  EXPECT_EQ(p, GFNT_KASHIDA_BEFORE_BEH_RA);
}


TEST(Kashida, TheHehClassHoldsDalAndTehMarbuta) {
  // The IE connection-priority list (khtt.net, "The Big Kashida Secret"): class 3 is the final
  // teh marbuta, heh and dal; each beats a final alef (class 4) in the same word.
  int p = 0;
  point("بيد", &p);   EXPECT_EQ(p, GFNT_KASHIDA_BEFORE_HEH);      // final dal
  point("بة", &p);    EXPECT_EQ(p, GFNT_KASHIDA_BEFORE_HEH);      // final teh marbuta
  point("كتبد", &p);  EXPECT_EQ(p, GFNT_KASHIDA_BEFORE_HEH);
}

TEST(Kashida, AlefMaqsuraCountsAsAYeh) {
  int p = 0;
  // beh-beh-alef maqsura: the middle beh is medial and followed by a yeh-group letter
  EXPECT_EQ(point("ببى", &p), 1);
  EXPECT_EQ(p, GFNT_KASHIDA_BEFORE_BEH_RA);
}


// ---- In a paragraph -------------------------------------------------------------------------
//
// `arabic-join.ttf`: every letter 34 pixels wide at size 100, a space 30, a tatweel 24, and
// lam + alef one 62-pixel glyph. Glyph 3 is the tatweel and glyph 1 the space.

constexpr GFNT_F26Dot6 px(int n) { return n * 64; }
constexpr uint32_t kTatweel = 3, kSpace = 1, kTatweelHeh = 68;

struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;

  Font() {
    std::ifstream in(gfnttest::data("fonts/arabic-join.ttf"), std::ios::binary);
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

struct Para {
  GFNT_Paragraph * p = nullptr;
  GFNT_Layout layout{};
  const GFNT_Face * faces[1];
  GFNT_Result created = GFNT_ERR_INTERNAL;

  Para(const Font & font, const std::string & text, GFNT_Justify how, GFNT_F16Dot16 space_share = 0,
      GFNT_BidiDirection direction = GFNT_BIDI_RTL, bool justify_last = false,
      GFNT_F26Dot6 letter_spacing = 0) {
    faces[0] = font.face;
    GFNT_TextSpan span{};
    span.length = text.size();
    span.style.faces = faces;
    span.style.face_count = 1;
    span.style.size = px(100);
    span.style.letter_spacing = letter_spacing;
    GFNT_ParagraphOptions o{};
    o.align = GFNT_ALIGN_JUSTIFY;
    o.justify = how;
    o.space_share = space_share;
    o.direction = direction;
    o.justify_last = justify_last;
    created = gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o, nullptr, &p, nullptr);
  }
  ~Para() {
    gfnt_layout_free(&layout);
    gfnt_paragraph_free(p);
  }
  Para(const Para &) = delete;
  Para & operator=(const Para &) = delete;
};

size_t count_glyph(const GFNT_Line & line, uint32_t glyph) {
  size_t n = 0;
  for (size_t r = 0; r < line.run_count; ++r) {
    for (size_t g = 0; g < line.runs[r].glyph_count; ++g) {
      n += line.runs[r].glyphs[g].glyph == glyph;
    }
  }
  return n;
}

// The run that holds the text: a hanging space at the end of the line is a run of its own.
const GFNT_LineRun & text_run(const GFNT_Line & line) {
  const GFNT_LineRun * widest = &line.runs[0];
  for (size_t r = 1; r < line.run_count; ++r) {
    if (line.runs[r].glyph_count > widest->glyph_count) {
      widest = &line.runs[r];
    }
  }
  return *widest;
}

// The space between the words (the trailing one hangs, in a run of its own, and is not widened).
GFNT_F26Dot6 space_advance(const GFNT_Line & line) {
  GFNT_F26Dot6 widest = -1;
  for (size_t r = 0; r < line.run_count; ++r) {
    for (size_t g = 0; g < line.runs[r].glyph_count; ++g) {
      if (line.runs[r].glyphs[g].glyph == kSpace && line.runs[r].glyphs[g].advance > widest) {
        widest = line.runs[r].glyphs[g].advance;
      }
    }
  }
  return widest;
}

// Line one is "بسم سلسبيل": 3 + 6 letters and a space, 336 of 400, so 64 to give.
const char kText[] = "بسم سلسبيل بسم";

TEST(KashidaLayout, TheStrokesTakeTheSlackWhenTheSpacesTakeNone) {
  Font font;
  Para para(font, kText, GFNT_JUSTIFY_KASHIDA, 0);
  ASSERT_EQ(para.created, GFNT_OK);
  // 96 to give: 48 to each word, which is two whole tatweels of 24.
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(432), &para.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_TRUE(line.justified);
  EXPECT_EQ(line.extent, px(432));
  EXPECT_EQ(space_advance(line), px(30)) << "the space is as it was";
  EXPECT_EQ(count_glyph(line, kTatweel), 4u);
}

TEST(KashidaLayout, StrokesAreWholeTatweelsAndTheRestGoesToTheSpaces) {
  Font font;
  Para para(font, kText, GFNT_JUSTIFY_KASHIDA, 0);
  // 64 to give: 32 to each word is one tatweel of 24 and 8 over, which the space takes.
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(400), &para.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_EQ(count_glyph(line, kTatweel), 2u);
  EXPECT_EQ(space_advance(line), px(30 + 16));
  EXPECT_EQ(line.extent, px(400));
  for (size_t r = 0; r < line.run_count; ++r) {
    for (size_t g = 0; g < line.runs[r].glyph_count; ++g) {
      if (line.runs[r].glyphs[g].glyph == kTatweel) {
        EXPECT_EQ(line.runs[r].glyphs[g].advance, px(24)) << "each at its own width, so they meet";
      }
    }
  }
}

TEST(KashidaLayout, LessThanOneTatweelIsNoStroke) {
  Font font;
  Para para(font, kText, GFNT_JUSTIFY_KASHIDA, 0);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(350), &para.layout, nullptr), GFNT_OK);   // 14 over
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_EQ(count_glyph(line, kTatweel), 0u);
  EXPECT_EQ(space_advance(line), px(30 + 14));
}

TEST(KashidaLayout, TheSpacesTakeTheSlackWhenTheStrokesTakeNone) {
  Font font;
  Para kashida(font, kText, GFNT_JUSTIFY_KASHIDA, 0x10000);
  Para words(font, kText, GFNT_JUSTIFY_INTER_WORD);
  ASSERT_EQ(gfnt_paragraph_layout(kashida.p, px(400), &kashida.layout, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(words.p, px(400), &words.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = kashida.layout.lines[0];
  EXPECT_EQ(count_glyph(line, kTatweel), 0u);
  EXPECT_EQ(space_advance(line), px(94));
  EXPECT_EQ(space_advance(line), space_advance(words.layout.lines[0]));
  ASSERT_EQ(line.run_count, words.layout.lines[0].run_count);
  for (size_t r = 0; r < line.run_count; ++r) {
    ASSERT_EQ(line.runs[r].glyph_count, words.layout.lines[0].runs[r].glyph_count);
    for (size_t g = 0; g < line.runs[r].glyph_count; ++g) {
      EXPECT_EQ(line.runs[r].glyphs[g].x, words.layout.lines[0].runs[r].glyphs[g].x);
    }
  }
}

TEST(KashidaLayout, LetterSpacingBelongsToTheLettersAndNotToTheStrokes) {
  Font font;
  Para para(font, kText, GFNT_JUSTIFY_KASHIDA, 0, GFNT_BIDI_RTL, false, px(2));
  ASSERT_EQ(para.created, GFNT_OK);
  // Line one is 9 letters of 36, a space of 32 and the hanging one: 356 of 420, 64 to give. Each
  // word's share is 32: one tatweel of 24 (a stroke is not a character of the text, so it takes
  // no spacing) and 8 over, which the space takes.
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(420), &para.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_EQ(line.extent, px(420));
  EXPECT_EQ(count_glyph(line, kTatweel), 2u);
  for (size_t r = 0; r < line.run_count; ++r) {
    for (size_t g = 0; g < line.runs[r].glyph_count; ++g) {
      if (line.runs[r].glyphs[g].glyph == kTatweel) {
        EXPECT_EQ(line.runs[r].glyphs[g].advance, px(24)) << "a stroke takes no spacing";
      }
    }
  }
  EXPECT_EQ(space_advance(line), px(32 + 16));
}

TEST(KashidaLayout, TheSlackIsSharedByTheRatio) {
  Font font;
  GFNT_F26Dot6 previous_space = -1;
  size_t previous_strokes = 99;
  for (int q = 0; q <= 4; ++q) {
    Para para(font, kText, GFNT_JUSTIFY_KASHIDA, q * 0x4000);
    ASSERT_EQ(gfnt_paragraph_layout(para.p, px(432), &para.layout, nullptr), GFNT_OK);
    const GFNT_Line & line = para.layout.lines[0];
    EXPECT_EQ(line.extent, px(432)) << q << "/4";
    EXPECT_GE(space_advance(line), previous_space) << q << "/4";
    EXPECT_LE(count_glyph(line, kTatweel), previous_strokes) << q << "/4";
    previous_space = space_advance(line);
    previous_strokes = count_glyph(line, kTatweel);
  }
  EXPECT_EQ(previous_strokes, 0u) << "all to the spaces: none";
  EXPECT_EQ(previous_space, px(30 + 96));
}

TEST(KashidaLayout, AStrokeIsDrawnWhereTheWordIsAndAsOneWithIt) {
  Font font;
  Para para(font, kText, GFNT_JUSTIFY_KASHIDA, 0);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(432), &para.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  ASSERT_GE(line.run_count, 1u);
  const GFNT_LineRun & run = text_run(line);
  GFNT_F26Dot6 at = run.pos;
  for (size_t g = 0; g < run.glyph_count; ++g) {
    const GFNT_PlacedGlyph & pl = run.glyphs[g];
    EXPECT_EQ(pl.pen_x, at) << g << ": glyphs follow one another with no gap";
    at += pl.advance;
    if (pl.glyph == kTatweel) {
      // A stroke carries the cluster of the letter it extends, so a click on it lands there.
      bool shared = false;
      for (size_t h = 0; h < run.glyph_count; ++h) {
        shared = shared || (run.glyphs[h].glyph != kTatweel && run.glyphs[h].cluster == pl.cluster);
      }
      EXPECT_TRUE(shared) << g;
    }
  }
  EXPECT_EQ(at, run.pos + px(432));
}

TEST(KashidaLayout, AWordWithNoPlaceFallsBackToItsSpaces) {
  Font font;
  // Nothing in these words connects, or the only join is a lam-alef.
  Para para(font, "زر ور لا زر", GFNT_JUSTIFY_KASHIDA, 0);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(240), &para.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_EQ(count_glyph(line, kTatweel), 0u);
  EXPECT_TRUE(line.justified) << "the spaces took it";
  EXPECT_EQ(line.extent, px(240));
}

TEST(KashidaLayout, AWordIsStretchedAsFarAsTheLineNeeds) {
  Font font;
  // "بسم" alone on a line of 500, followed by a word too long to join it: 398 to give, of which
  // sixteen tatweels take 384 (the 14 over stay unspent: there is no space to put them in).
  Para para(font, "بسم AAAAAAAAAAAA", GFNT_JUSTIFY_KASHIDA, 0, GFNT_BIDI_LTR);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(500), &para.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_EQ(count_glyph(line, kTatweel), 16u);
  EXPECT_EQ(line.extent, px(102 + 384));
}

TEST(KashidaLayout, AStrokeHasALongestLength) {
  Font font;
  // 128 tatweels of 24 is 3072: a line of 4000 gives one word no more than that, and goes short.
  Para para(font, "بسم", GFNT_JUSTIFY_KASHIDA, 0, GFNT_BIDI_RTL, true);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(4000), &para.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_TRUE(line.justified);
  EXPECT_EQ(count_glyph(line, kTatweel), 128u);
  EXPECT_LT(line.extent, px(4000));
  EXPECT_GE(line.extent, px(3072));
}

TEST(KashidaLayout, TheLastLineIsJustifiedOnRequest) {
  Font font;
  Para off(font, "بسم", GFNT_JUSTIFY_KASHIDA, 0, GFNT_BIDI_RTL, false);
  Para on(font, "بسم", GFNT_JUSTIFY_KASHIDA, 0, GFNT_BIDI_RTL, true);
  ASSERT_EQ(gfnt_paragraph_layout(off.p, px(300), &off.layout, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_paragraph_layout(on.p, px(300), &on.layout, nullptr), GFNT_OK);
  EXPECT_FALSE(off.layout.lines[0].justified);
  EXPECT_TRUE(on.layout.lines[0].justified);
  EXPECT_EQ(on.layout.lines[0].extent, px(102 + 8 * 24)) << "eight tatweels; the rest has nowhere to go";
}

TEST(KashidaLayout, AnAuthorsTatweelIsTheOneStretched) {
  Font font;
  Para para(font, "كـتاب بسم كـتاب", GFNT_JUSTIFY_KASHIDA, 0);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(400), &para.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_TRUE(line.justified);
  EXPECT_EQ(line.extent, px(400));
  EXPECT_GE(count_glyph(line, kTatweel), 4u) << "the author's own, and more after it";
}

TEST(KashidaLayout, ALigatureIsNeverPulledApart) {
  Font font;
  // "هلال": the only place is in front of the lam-alef glyph (62 wide), which stays one glyph.
  Para para(font, "هلال هلال هلال", GFNT_JUSTIFY_KASHIDA, 0);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(340), &para.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_GT(count_glyph(line, kTatweel), 0u);
  EXPECT_EQ(count_glyph(line, 66), 2u) << "both words keep their lam-alef whole";
}

TEST(KashidaLayout, APairTheFontLigatesIsSkipped) {
  Font font;
  // "بن" is one glyph in this font, so the join inside it is no place for a tatweel.
  Para para(font, "بن بن بن بن", GFNT_JUSTIFY_KASHIDA, 0);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(200), &para.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  EXPECT_EQ(count_glyph(line, kTatweel), 0u);
  EXPECT_TRUE(line.justified);
  EXPECT_EQ(line.extent, px(200)) << "the spaces took it";
}

TEST(KashidaLayout, ALigatureWithTheTatweelIsDrawnAsTheFontMakesIt) {
  Font font;
  // The best place in "بانه" is in front of the heh, and this font draws tatweel + heh as one
  // glyph: the word is reshaped with the tatweels in it, so that glyph is what is drawn, and the
  // stroke's own cluster is the noon's, the letter it extends.
  Para para(font, "بانه بانه بانه", GFNT_JUSTIFY_KASHIDA, 0);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(400), &para.layout, nullptr), GFNT_OK);
  const GFNT_Line & line = para.layout.lines[0];
  size_t strokes = 0, ligatures = 0;
  for (size_t r = 0; r < line.run_count; ++r) {
    for (size_t g = 0; g < line.runs[r].glyph_count; ++g) {
      const GFNT_PlacedGlyph & pl = line.runs[r].glyphs[g];
      if (pl.glyph == kTatweel) {
        ++strokes;
        EXPECT_EQ(pl.cluster % 9, 4u) << "the noon: bytes 4 and 5 of a word of eight";
      }
      if (pl.glyph == kTatweelHeh) {
        ++ligatures;
        EXPECT_EQ(pl.cluster % 9, 4u) << "the tatweel in it belongs to the noon, too";
      }
    }
  }
  EXPECT_EQ(ligatures, 2u) << "one in each of the two words of the line";
  EXPECT_EQ(strokes, 2u) << "and a tatweel of its own before it, 98 to give: two tatweels each";
  EXPECT_EQ(line.extent, px(400));
}

TEST(KashidaLayout, TheStrokeLiesInTheGapItNames) {
  Font font;
  // "بسم" is stretched after the seen: in a right-to-left line the strokes lie between the meem
  // (to the left, later in the text) and the seen, and belong to the seen.
  Para para(font, "بسم بسم بسم", GFNT_JUSTIFY_KASHIDA, 0);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(300), &para.layout, nullptr), GFNT_OK);
  const GFNT_LineRun & run = text_run(para.layout.lines[0]);
  size_t seen = 0;
  for (size_t g = 0; g < run.glyph_count; ++g) {
    if (run.glyphs[g].glyph != kTatweel) {
      continue;
    }
    ++seen;
    ASSERT_GT(g, 0u);
    ASSERT_LT(g + 1, run.glyph_count);
    EXPECT_GT(run.glyphs[g - 1].cluster, run.glyphs[g].cluster) << "the meem, later in the text";
    EXPECT_TRUE(run.glyphs[g + 1].glyph == kTatweel || run.glyphs[g + 1].cluster == run.glyphs[g].cluster)
        << "and the seen, whose stroke it is";
  }
  EXPECT_GT(seen, 0u);
}

TEST(KashidaLayout, TheRatioIsChecked) {
  Font font;
  Para fine(font, kText, GFNT_JUSTIFY_KASHIDA, 0x10000);
  EXPECT_EQ(fine.created, GFNT_OK);
  Para over(font, kText, GFNT_JUSTIFY_KASHIDA, 0x10001);
  EXPECT_EQ(over.created, GFNT_ERR_INVALID);
  Para under(font, kText, GFNT_JUSTIFY_KASHIDA, -1);
  EXPECT_EQ(under.created, GFNT_ERR_INVALID);
}

TEST(KashidaLayout, HitTestingAStrokeFindsTheLetterItExtends) {
  Font font;
  Para para(font, kText, GFNT_JUSTIFY_KASHIDA, 0);
  ASSERT_EQ(gfnt_paragraph_layout(para.p, px(400), &para.layout, nullptr), GFNT_OK);
  const GFNT_LineRun & run = text_run(para.layout.lines[0]);
  for (size_t g = 0; g < run.glyph_count; ++g) {
    if (run.glyphs[g].glyph != kTatweel) {
      continue;
    }
    GFNT_Hit hit{};
    ASSERT_EQ(gfnt_layout_hit_test(para.p, &para.layout, run.glyphs[g].pen_x + run.glyphs[g].advance / 2,
                  px(40), &hit), GFNT_OK);
    EXPECT_LT(hit.offset, strlen(kText));
  }
}

TEST(KashidaLayout, EveryAllocationFailsCleanly) {
  Font font;
  const std::string text = kText;
  GFNT_TextSpan span{};
  const GFNT_Face * faces[1] = {font.face};
  span.length = text.size();
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = px(100);
  GFNT_ParagraphOptions o{};
  o.align = GFNT_ALIGN_JUSTIFY;
  o.justify = GFNT_JUSTIFY_KASHIDA;
  o.direction = GFNT_BIDI_RTL;
  size_t requests = 0;
  {
    gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
    GFNT_Paragraph * p = nullptr;
    GFNT_Layout l{};
    ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o, counting.get(), &p,
                  nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_paragraph_layout(p, px(400), &l, nullptr), GFNT_OK);
    requests = counting.requests();
    gfnt_layout_free(&l);
    gfnt_paragraph_free(p);
    EXPECT_EQ(counting.live(), 0u);
  }
  for (size_t fail = 0; fail < requests; ++fail) {
    gfnttest::FailingAllocator failing(fail);
    GFNT_Paragraph * p = nullptr;
    GFNT_Layout l{};
    GFNT_Result r = gfnt_paragraph_create(text.data(), text.size(), &span, 1, &o, failing.get(), &p,
        nullptr);
    if (r == GFNT_OK) {
      r = gfnt_paragraph_layout(p, px(400), &l, nullptr);
    }
    if (r != GFNT_OK) {
      EXPECT_EQ(r, GFNT_ERR_OOM) << "request " << fail;
    }
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
