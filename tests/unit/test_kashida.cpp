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

#include <string>
#include <vector>

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
  if (!gfnt_kashida_point(w.data(), w.size(), &at, &pr)) return -1;
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
  EXPECT_EQ(point("كـتاب", &priority), 1);   // in front of the stroke
  EXPECT_EQ(priority, GFNT_KASHIDA_AUTHOR);
  EXPECT_EQ(point("كــــتاب"), 1);           // a run of strokes is one stroke
  EXPECT_EQ(point("بسـم"), 2);               // over the seen
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

TEST(Kashida, MedialBehBeforeRehOrYeh) {
  int p = 0;
  // beh-beh-reh: the middle beh is medial and followed by a reh
  EXPECT_EQ(point("ببر", &p), 1);
  EXPECT_EQ(p, GFNT_KASHIDA_BEFORE_BEH_RA);
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
