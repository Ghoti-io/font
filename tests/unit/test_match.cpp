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
 * CSS Fonts Level 4 font matching (section 5.2) over a scanned set. Each test builds a
 * directory of small faces whose family, weight, width and slant are the point, scans it,
 * and names the face the specification's steps choose, with the step it follows. The
 * expectations are worked out from the specification's text, not from this
 * implementation's output.
 */

#include "test_helpers.h"
#include "failing_allocator.h"
#include "font_tree.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include <ghoti.io/cutil/dir.h>
#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/path.h>
#include <ghoti.io/font/discover.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/match.h>
#include <ghoti.io/font/variation.h>

#include <gtest/gtest.h>

// --- font-weight ----------------------------------------------------------------

namespace {

/** A family of upright faces at the given weights, each labelled by its weight. */
void weights(Fonts & f, std::initializer_list<int> ws) {
  for (int w : ws) {
    Spec s;
    s.family = "W";
    s.weight = static_cast<uint16_t>(w);
    s.label = "W" + std::to_string(w);
    f.add(s);
  }
  f.scan();
}

std::string weight_pick(Fonts & f, double want) { return f.pick(query("W", fx(want))); }

}  // namespace

TEST(MatchWeight, AFaceAtTheWeightAskedForIsChosen) {
  Fonts f;
  weights(f, {200, 400, 700});
  EXPECT_EQ(weight_pick(f, 400), "W400");
  EXPECT_EQ(weight_pick(f, 700), "W700");
  EXPECT_EQ(weight_pick(f, 200), "W200");
}

TEST(MatchWeight, BetweenFourAndFiveHundredLooksUpToFiveHundredBeforeDown) {
  Fonts f;
  weights(f, {300, 500, 600});
  EXPECT_EQ(weight_pick(f, 400), "W500") << "400 with no 400: up to 500 first";
  EXPECT_EQ(weight_pick(f, 450), "W500");
}

TEST(MatchWeight, AboveFiveHundredIsOnlyReachedAfterGoingDown) {
  Fonts f;
  weights(f, {300, 600, 700});
  EXPECT_EQ(weight_pick(f, 400), "W300") << "no 400 or 500: below, descending, before 600";
  EXPECT_EQ(weight_pick(f, 450), "W300");
}

TEST(MatchWeight, FourHundredWithOnlyHeavierFacesTakesTheLightestAboveFiveHundred) {
  Fonts f;
  weights(f, {600, 700, 900});
  EXPECT_EQ(weight_pick(f, 400), "W600");
  EXPECT_EQ(weight_pick(f, 500), "W600");
}

TEST(MatchWeight, FiveHundredWithNoFiveHundredTakesTheNearestBelowBeforeAnyAbove) {
  Fonts f;
  weights(f, {300, 400, 600});
  EXPECT_EQ(weight_pick(f, 500), "W400");
}

TEST(MatchWeight, BelowFourHundredLooksDownFirstThenUp) {
  Fonts f;
  weights(f, {100, 400, 700});
  EXPECT_EQ(weight_pick(f, 300), "W100");
  EXPECT_EQ(weight_pick(f, 200), "W100");
  Fonts g;
  weights(g, {400, 700});
  EXPECT_EQ(weight_pick(g, 300), "W400") << "nothing lighter: up, ascending";
}

TEST(MatchWeight, AboveFiveHundredLooksUpFirstThenDown) {
  Fonts f;
  weights(f, {400, 500, 800});
  EXPECT_EQ(weight_pick(f, 600), "W800");
  EXPECT_EQ(weight_pick(f, 900), "W800") << "nothing heavier: the nearest below";
  Fonts g;
  weights(g, {300, 400, 500});
  EXPECT_EQ(weight_pick(g, 700), "W500");
}

TEST(MatchWeight, ANumberBetweenTheHundredsIsMatchedAsItIs) {
  // Unlike CSS Fonts 3, nothing is rounded to a multiple of 100 first.
  Fonts f;
  weights(f, {300, 500, 600});
  EXPECT_EQ(weight_pick(f, 399), "W300") << "below 400: down first";
  EXPECT_EQ(weight_pick(f, 501), "W600");
  EXPECT_EQ(weight_pick(f, 401), "W500");
  EXPECT_EQ(weight_pick(f, 499), "W500");
}

TEST(MatchWeight, TheInitialWeightIsFourHundred) {
  Fonts f;
  weights(f, {300, 400, 700});
  EXPECT_EQ(f.pick(query("W")), "W400");
}

// --- font-width -----------------------------------------------------------------

namespace {

void widths(Fonts & f, std::initializer_list<int> classes) {
  for (int c : classes) {
    Spec s;
    s.family = "S";
    s.width = static_cast<uint16_t>(c);
    s.label = "C" + std::to_string(c);
    f.add(s);
  }
  f.scan();
}

std::string width_pick(Fonts & f, double percent) {
  return f.pick(query("S", 0, fx(percent)));
}

}  // namespace

TEST(MatchWidth, TheWidthClassesAreTheKeywordPercentages) {
  Fonts f;
  widths(f, {1, 2, 3, 4, 5, 6, 7, 8, 9});
  EXPECT_EQ(width_pick(f, 50), "C1");
  EXPECT_EQ(width_pick(f, 62.5), "C2");
  EXPECT_EQ(width_pick(f, 75), "C3");
  EXPECT_EQ(width_pick(f, 87.5), "C4");
  EXPECT_EQ(width_pick(f, 100), "C5");
  EXPECT_EQ(width_pick(f, 112.5), "C6");
  EXPECT_EQ(width_pick(f, 125), "C7");
  EXPECT_EQ(width_pick(f, 150), "C8");
  EXPECT_EQ(width_pick(f, 200), "C9");
}

TEST(MatchWidth, AtOrBelowAHundredPercentLooksNarrowerFirst) {
  Fonts f;
  widths(f, {3, 5, 7});            // 75, 100, 125
  EXPECT_EQ(width_pick(f, 90), "C3") << "narrower first, though 100 is nearer";
  EXPECT_EQ(width_pick(f, 60), "C3") << "nothing narrower: the next wider";
  Fonts g;
  widths(g, {5, 7});
  EXPECT_EQ(width_pick(g, 80), "C5");
}

TEST(MatchWidth, ExactlyAHundredPercentWithNoNormalFaceLooksNarrowerFirst) {
  Fonts f;
  widths(f, {3, 7});               // 75 and 125
  EXPECT_EQ(width_pick(f, 100), "C3") << "100 is 'at or below': narrower first";
  Fonts g;
  widths(g, {3, 7});
  EXPECT_EQ(width_pick(g, 100.5), "C7") << "just past 100: wider first";
}

TEST(MatchWidth, AboveAHundredPercentLooksWiderFirst) {
  Fonts f;
  widths(f, {3, 5, 7});
  EXPECT_EQ(width_pick(f, 110), "C7") << "wider first, though 100 is nearer";
  EXPECT_EQ(width_pick(f, 140), "C7") << "nothing wider: the nearest narrower";
}

TEST(MatchWidth, WidthIsDecidedBeforeWeight) {
  // A condensed face that is not the right weight beats a normal-width one that is.
  Fonts f;
  Spec a;
  a.family = "S";
  a.width = 3;
  a.weight = 300;
  a.label = "condensed-light";
  f.add(a);
  Spec b;
  b.family = "S";
  b.width = 5;
  b.weight = 700;
  b.label = "normal-bold";
  f.add(b);
  f.scan();
  EXPECT_EQ(f.pick(query("S", fx(700), fx(75))), "condensed-light");
}

// --- font-style -----------------------------------------------------------------

namespace {

Spec roman(const char * label) {
  Spec s;
  s.family = "T";
  s.label = label;
  return s;
}

Spec italic(const char * label) {
  Spec s = roman(label);
  s.italic = true;
  return s;
}

Spec oblique(const char * label, double degrees) {
  Spec s = roman(label);
  s.oblique = true;
  s.lean = -degrees;
  return s;
}

GFNT_FontQuery style(GFNT_Slant slant, double angle = 0, bool angle_set = false) {
  GFNT_FontQuery q = query("T", 0, 0, slant);
  q.oblique_angle = fx(angle);
  q.oblique_angle_set = angle_set;
  return q;
}

}  // namespace

TEST(MatchStyle, NormalPrefersARomanFaceThenAnObliqueThenAnItalic) {
  {
    Fonts f;
    f.add(italic("i"));
    f.add(roman("r"));
    f.add(oblique("o", 12));
    f.scan();
    EXPECT_EQ(f.pick(style(GFNT_SLANT_ROMAN)), "r");
  }
  {
    Fonts f;
    f.add(italic("i"));
    f.add(oblique("o", 12));
    f.scan();
    EXPECT_EQ(f.pick(style(GFNT_SLANT_ROMAN)), "o") << "oblique is tried before italic";
  }
  {
    Fonts f;
    f.add(italic("i"));
    f.scan();
    EXPECT_EQ(f.pick(style(GFNT_SLANT_ROMAN)), "i");
  }
}

TEST(MatchStyle, ItalicPrefersItalicThenAnObliqueThenRoman) {
  {
    Fonts f;
    f.add(roman("r"));
    f.add(oblique("o", 14));
    f.add(italic("i"));
    f.scan();
    EXPECT_EQ(f.pick(style(GFNT_SLANT_ITALIC)), "i");
  }
  {
    Fonts f;
    f.add(roman("r"));
    f.add(oblique("o", 14));
    f.scan();
    EXPECT_EQ(f.pick(style(GFNT_SLANT_ITALIC)), "o");
  }
  {
    Fonts f;
    f.add(roman("r"));
    f.scan();
    EXPECT_EQ(f.pick(style(GFNT_SLANT_ITALIC)), "r");
  }
}

TEST(MatchStyle, ItalicChoosesAnObliqueAtOrAboveElevenDegreesAscendingBeforeOnesBelow) {
  Fonts f;
  f.add(oblique("o5", 5));
  f.add(oblique("o11", 11));
  f.add(oblique("o20", 20));
  f.add(oblique("o30", 30));
  f.scan();
  EXPECT_EQ(f.pick(style(GFNT_SLANT_ITALIC)), "o11");
  Fonts g;
  g.add(oblique("o5", 5));
  g.add(oblique("o8", 8));
  g.scan();
  EXPECT_EQ(g.pick(style(GFNT_SLANT_ITALIC)), "o8") << "none at 11 or more: below, descending";
}

TEST(MatchStyle, ObliqueAtOrAboveElevenDegreesChoosesTheNearestAngleAboveThenBelow) {
  Fonts f;
  f.add(oblique("o8", 8));
  f.add(oblique("o20", 20));
  f.add(oblique("o25", 25));
  f.scan();
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 14, true)), "o20");
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 30, true)), "o25") << "nothing above: the nearest below";
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE)), "o20") << "the keyword is 14 degrees";
}

TEST(MatchStyle, ElevenDegreesIsTheFirstAngleThatLooksUpward) {
  Fonts f;
  f.add(oblique("o8", 8));
  f.add(oblique("o20", 20));
  f.scan();
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 11, true)), "o20") << "11 and over: above first";
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 10.9, true)), "o8") << "under 11: below first";
}

TEST(MatchStyle, ObliqueBelowElevenDegreesChoosesTheNearestAngleBelowThenAbove) {
  Fonts f;
  f.add(oblique("o3", 3));
  f.add(oblique("o8", 8));
  f.add(oblique("o20", 20));
  f.scan();
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 6, true)), "o3");
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 9, true)), "o8");
  Fonts g;
  g.add(oblique("o20", 20));
  g.scan();
  EXPECT_EQ(g.pick(style(GFNT_SLANT_OBLIQUE, 9, true)), "o20");
}

TEST(MatchStyle, ObliqueAngleThatAFaceStatesIsMatchedExactly) {
  Fonts f;
  f.add(oblique("o10", 10));
  f.add(oblique("o14", 14));
  f.scan();
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 10, true)), "o10");
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 14, true)), "o14");
}

TEST(MatchStyle, ObliqueAboveElevenWithNoObliqueFacesTakesAnItalicBeforeRoman) {
  Fonts f;
  f.add(roman("r"));
  f.add(italic("i"));
  f.scan();
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 20, true)), "i");
}

TEST(MatchStyle, ObliqueBelowElevenWithNoObliqueFacesAlsoTakesAnItalicBeforeRoman) {
  // The specification's fourth step puts italic ahead of "oblique at or below zero", which is
  // what a roman face is, so a small oblique request prefers italic to roman when it must.
  Fonts f;
  f.add(roman("r"));
  f.add(italic("i"));
  f.scan();
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 5, true)), "i");
}

TEST(MatchStyle, ObliqueWithOnlyARomanFaceTakesIt) {
  Fonts f;
  f.add(roman("r"));
  f.scan();
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 14, true)), "r");
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 0, true)), "r");
}

TEST(MatchStyle, ANegativeAngleMirrorsTheSearch) {
  // A face that leans the other way (a negative oblique angle) is found by a negative request.
  Fonts f;
  f.add(oblique("left20", -20));
  f.add(oblique("right20", 20));
  f.scan();
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, -14, true)), "left20");
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 14, true)), "right20");
}

TEST(MatchStyle, StyleIsDecidedBeforeWeight) {
  Fonts f;
  Spec a = italic("italic-light");
  a.weight = 300;
  f.add(a);
  Spec b = roman("roman-bold");
  b.weight = 700;
  f.add(b);
  f.scan();
  GFNT_FontQuery q = style(GFNT_SLANT_ITALIC);
  q.weight = fx(700);
  EXPECT_EQ(f.pick(q), "italic-light");
}

// --- families -------------------------------------------------------------------

TEST(MatchFamily, TheFirstFamilyThatHasAFaceIsUsedAndReported) {
  Fonts f;
  Spec a;
  a.family = "Beta";
  a.label = "beta";
  f.add(a);
  Spec b;
  b.family = "Gamma";
  b.label = "gamma";
  f.add(b);
  f.scan();
  const char * list[] = {"Alpha", "Gamma", "Beta"};
  GFNT_FontQuery q;
  gfnt_fontquery_init(&q);
  q.families = list;
  q.family_count = 3;
  GFNT_FontMatch m{};
  EXPECT_EQ(f.pick(q, &m), "gamma");
  EXPECT_EQ(m.family, 1u);
}

TEST(MatchFamily, NoFamilyOfTheListMeansNoMatch) {
  Fonts f;
  Spec a;
  a.family = "Beta";
  a.label = "beta";
  f.add(a);
  f.scan();
  EXPECT_EQ(f.pick(query("Nowhere")), "");
}

TEST(MatchFamily, NamesAreMatchedWithoutRegardToCase) {
  Fonts f;
  Spec a;
  a.family = "Straße Sans";
  a.label = "s";
  f.add(a);
  Spec b;
  b.family = "Sigma Σ";
  b.label = "sigma";
  f.add(b);
  f.scan();
  EXPECT_EQ(f.pick(query("STRASSE SANS")), "s") << "full case folding: ß folds to ss";
  EXPECT_EQ(f.pick(query("straße sans")), "s");
  EXPECT_EQ(f.pick(query("SIGMA σ")), "sigma");
}

TEST(MatchFamily, NamesAreNotNormalised) {
  // a + combining ring is not the precomposed å; the specification says they differ.
  Fonts f;
  Spec a;
  a.family = "\xC3\xA5";               // precomposed
  a.label = "pre";
  f.add(a);
  f.scan();
  EXPECT_EQ(f.pick(query("a\xCC\x8A")), "");
  EXPECT_EQ(f.pick(query("\xC3\x85")), "pre") << "the capital folds to it";
}

TEST(MatchFamily, TheLegacyFamilyIsASecondNameForTheFace) {
  Fonts f;
  Spec a;
  a.family = "Foo";
  a.legacy = "Foo Black";
  a.weight = 900;
  a.label = "black";
  f.add(a);
  Spec b;
  b.family = "Foo";
  b.weight = 400;
  b.label = "regular";
  f.add(b);
  f.scan();
  EXPECT_EQ(f.pick(query("Foo Black")), "black") << "the only face of that family";
  EXPECT_EQ(f.pick(query("Foo", fx(900))), "black");
  EXPECT_EQ(f.pick(query("Foo", fx(400))), "regular");
}

TEST(MatchFamily, ANameNoFamilyHasIsFollowedThroughTheAliasTable) {
  Fonts f;
  Spec a;
  a.family = "DejaVu Sans";
  a.label = "dv";
  f.add(a);
  f.scan({{"sans-serif", "DejaVu Sans"}});
  EXPECT_EQ(f.pick(query("sans-serif")), "dv");
  EXPECT_EQ(f.pick(query("monospace")), "");
}

TEST(MatchFamily, AFaceThatTiesIsTheFirstInTheSet) {
  Fonts f;
  Spec a;
  a.family = "Twin";
  a.label = "first";
  f.add(a);
  Spec b = a;
  b.label = "second";
  f.add(b);
  f.scan();
  const std::string chosen = f.pick(query("Twin"));
  EXPECT_TRUE(chosen == "first" || chosen == "second");
  // The walk's order is the set's order; whichever it found first wins, every time.
  EXPECT_EQ(f.pick(query("Twin")), chosen);
  EXPECT_EQ(gfnt_fontset_at(f.scan(), 0)->style, chosen);
}

TEST(MatchFamily, TheIndexOfALateFaceInALargeSetIsItsOwn) {
  // A record is embedded in a larger struct in the set, so an index worked out by pointer
  // arithmetic drifts with the number of faces; this found it on a machine with 1,433.
  Fonts f;
  for (int i = 0; i < 80; ++i) {
    Spec s;
    s.family = "F" + std::to_string(i);
    s.label = "L" + std::to_string(i);
    f.add(s);
  }
  f.scan();
  for (int i : {0, 1, 13, 14, 15, 40, 79}) {
    GFNT_FontMatch m{};
    const std::string family = "F" + std::to_string(i);
    EXPECT_EQ(f.pick(query(family.c_str()), &m), "L" + std::to_string(i));
    EXPECT_EQ(std::string(gfnt_fontset_at(f.scan(), m.index)->family), family);
  }
}

TEST(MatchFamily, MatchFamilyIgnoresTheQuerysOwnList) {
  Fonts f;
  Spec a;
  a.family = "A";
  a.label = "a";
  f.add(a);
  Spec b;
  b.family = "B";
  b.label = "b";
  f.add(b);
  f.scan();
  GFNT_FontQuery q = query("A");
  GFNT_FontMatch m{};
  bool found = false;
  ASSERT_EQ(gfnt_fontset_match_family(f.scan(), "B", &q, nullptr, &m, &found, nullptr), GFNT_OK);
  ASSERT_TRUE(found);
  EXPECT_EQ(std::string(gfnt_fontset_at(f.scan(), m.index)->style), "b");
}

TEST(MatchFamily, BadArgumentsAreRefused) {
  Fonts f;
  Spec a;
  a.family = "A";
  a.label = "a";
  f.add(a);
  f.scan();
  GFNT_FontQuery q = query("A");
  GFNT_FontMatch m{};
  bool found = false;
  EXPECT_EQ(gfnt_fontset_match(nullptr, &q, nullptr, &m, &found, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_fontset_match(f.scan(), nullptr, nullptr, &m, &found, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_fontset_match(f.scan(), &q, nullptr, nullptr, &found, nullptr), GFNT_ERR_INVALID);
  q.families = nullptr;
  EXPECT_EQ(gfnt_fontset_match(f.scan(), &q, nullptr, &m, &found, nullptr), GFNT_ERR_INVALID);
  gfnt_fontquery_init(&q);
  EXPECT_EQ(gfnt_fontset_match(f.scan(), &q, nullptr, &m, &found, nullptr), GFNT_OK) << "no families";
  EXPECT_FALSE(found);
}

TEST(MatchFamily, AnAllocationFailureIsReportedAndLeaksNothing) {
  Fonts f;
  Spec a;
  a.family = "A";
  a.label = "a";
  f.add(a);
  f.scan();
  GFNT_FontQuery q = query("A");
  GFNT_FontMatch m{};
  bool found = false;
  gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
  ASSERT_EQ(gfnt_fontset_match(f.scan(), &q, counting.get(), &m, &found, nullptr), GFNT_OK);
  EXPECT_TRUE(found);
  EXPECT_EQ(counting.live(), 0u);
  ASSERT_GT(counting.requests(), 0u);
  for (size_t fail = 0; fail < counting.requests(); ++fail) {
    gfnttest::FailingAllocator failing(fail);
    GFNT_Error error;
    EXPECT_EQ(gfnt_fontset_match(f.scan(), &q, failing.get(), &m, &found, &error), GFNT_ERR_OOM);
    EXPECT_EQ(failing.live(), 0u);
  }
}

// --- variable faces -------------------------------------------------------------

TEST(MatchVariable, AFaceWhoseAxisContainsTheValueIsAnExactMatchAtThatValue) {
  Fonts f;
  Spec fixed;
  fixed.family = "V";
  fixed.label = "static400";
  f.add(fixed);
  Spec var;
  var.family = "V";
  var.label = "variable";
  var.axes = {{"wght", 100, 400, 900}};
  f.add(var);
  f.scan();
  GFNT_FontMatch m{};
  EXPECT_EQ(f.pick(query("V", fx(650)), &m), "variable");
  EXPECT_EQ(m.weight, fx(650)) << "the axis is to be set to what was asked";
  EXPECT_EQ(f.pick(query("V", fx(400)), &m), "static400") << "both contain 400: the first wins";
}

TEST(MatchVariable, AnAxisRangeStandsInForEveryWeightItCovers) {
  Fonts f;
  Spec light;
  light.family = "V";
  light.weight = 300;
  light.label = "light";
  f.add(light);
  Spec var;
  var.family = "V";
  var.label = "variable";
  var.axes = {{"wght", 500, 500, 800}};
  f.add(var);
  f.scan();
  GFNT_FontMatch m{};
  // 400 is in neither; weights from 400 up to 500 are tried first and the axis begins at 500.
  EXPECT_EQ(f.pick(query("V", fx(400)), &m), "variable");
  EXPECT_EQ(m.weight, fx(500)) << "clamped to the axis";
  EXPECT_EQ(f.pick(query("V", fx(900)), &m), "variable");
  EXPECT_EQ(m.weight, fx(800));
  EXPECT_EQ(f.pick(query("V", fx(200)), &m), "light");
}

TEST(MatchVariable, AWidthAxisIsTreatedTheSameWay) {
  Fonts f;
  Spec var;
  var.family = "V";
  var.label = "variable";
  var.axes = {{"wdth", 75, 100, 125}};
  f.add(var);
  Spec wide;
  wide.family = "V";
  wide.width = 8;
  wide.label = "extra-expanded";
  f.add(wide);
  f.scan();
  GFNT_FontMatch m{};
  EXPECT_EQ(f.pick(query("V", 0, fx(110)), &m), "variable");
  EXPECT_EQ(m.width, fx(110));
  EXPECT_EQ(f.pick(query("V", 0, fx(140)), &m), "extra-expanded");
}

TEST(MatchVariable, ASlantAxisIsAnObliqueRangeThatContainsWhatItsNegationCovers) {
  Fonts f;
  Spec var = roman("variable");
  var.axes = {{"slnt", -12, 0, 0}};     // leans right by up to 12 degrees
  f.add(var);
  Spec fixed = oblique("o20", 20);
  f.add(fixed);
  f.scan();
  GFNT_FontMatch m{};
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 10, true), &m), "variable");
  EXPECT_EQ(m.oblique, fx(10)) << "the axis is to be set to -10";
  // 14 degrees is past the axis and the static face at 20 is the nearest above.
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 14, true), &m), "o20");
  // An upright request is satisfied by the axis at 0 (a range of oblique angles that includes 0).
  EXPECT_EQ(f.pick(style(GFNT_SLANT_ROMAN), &m), "variable");
  EXPECT_EQ(m.oblique, 0);
}

TEST(MatchVariable, ASlantAxisThatLeansLeftDoesNotSatisfyARightLeaningRequest) {
  Fonts f;
  Spec var = roman("leftward");
  var.axes = {{"slnt", 0, 0, 12}};      // positive slnt leans left: oblique angles -12 to 0
  f.add(var);
  f.add(oblique("o20", 20));
  f.scan();
  GFNT_FontMatch m{};
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, 10, true), &m), "o20");
  EXPECT_EQ(f.pick(style(GFNT_SLANT_OBLIQUE, -10, true), &m), "leftward");
  EXPECT_EQ(m.oblique, fx(-10)) << "to be set as slnt 10";
}

TEST(MatchVariable, AnItalicAxisIsAnItalicRangeAndCoversBothRomanAndItalic) {
  Fonts f;
  Spec var = roman("variable");
  var.axes = {{"ital", 0, 0, 1}};
  f.add(var);
  Spec fixed = oblique("o14", 14);
  f.add(fixed);
  f.scan();
  GFNT_FontMatch m{};
  EXPECT_EQ(f.pick(style(GFNT_SLANT_ITALIC), &m), "variable");
  EXPECT_EQ(m.italic, fx(1));
  EXPECT_EQ(f.pick(style(GFNT_SLANT_ROMAN), &m), "variable");
  EXPECT_EQ(m.italic, 0);
}

TEST(MatchVariable, TheLocationOfAMatchSetsTheNamedAxesAndLeavesTheRestAtDefault) {
  Fonts f;
  Spec var;
  var.family = "V";
  var.label = "variable";
  var.axes = {{"wght", 100, 400, 900}, {"opsz", 8, 14, 144}, {"slnt", -12, 0, 0}};
  f.add(var);
  f.scan();
  GFNT_FontQuery q = query("V", fx(700));
  q.slant = GFNT_SLANT_OBLIQUE;
  q.oblique_angle = fx(8);
  q.oblique_angle_set = true;
  GFNT_FontMatch m{};
  ASSERT_EQ(f.pick(q, &m), "variable");
  EXPECT_EQ(m.oblique, fx(8));
  // Open the chosen face and ask for coordinates.
  const GFNT_FontRecord * r = gfnt_fontset_at(f.scan(), m.index);
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  ASSERT_EQ(gfnt_blob_create_file(r->path, nullptr, nullptr, &blob, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_load(blob, r->index, nullptr, nullptr, &face, nullptr), GFNT_OK);
  GFNT_F2Dot14 coords[8];
  size_t count = 0;
  ASSERT_EQ(gfnt_fontmatch_location(face, &m, coords, 8, &count, nullptr), GFNT_OK);
  ASSERT_EQ(count, 3u);
  GFNT_F2Dot14 want[3];
  const GFNT_F16Dot16 user[3] = {fx(700), fx(14), fx(-8)};
  ASSERT_EQ(gfnt_face_normalize(face, user, 3, want, 3, nullptr), GFNT_OK);
  EXPECT_EQ(coords[0], want[0]);
  EXPECT_EQ(coords[1], 0) << "opsz stays at its default";
  EXPECT_EQ(coords[2], want[2]);
  EXPECT_NE(coords[0], 0);
  EXPECT_NE(coords[2], 0);
  size_t small = 99;
  EXPECT_EQ(gfnt_fontmatch_location(face, &m, coords, 2, &small, nullptr), GFNT_ERR_LIMIT);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
}

// --- the record -----------------------------------------------------------------

TEST(MatchRecord, AVariableFaceStatesItsAxesAndAStaticOneStatesNone) {
  Fonts f;
  Spec var;
  var.family = "V";
  var.label = "variable";
  var.axes = {{"wght", 100, 400, 900}, {"wdth", 75, 100, 125}};
  f.add(var);
  Spec fixed;
  fixed.family = "F";
  fixed.label = "static";
  fixed.oblique = true;
  fixed.lean = -9;
  f.add(fixed);
  f.scan();
  const GFNT_FontRecord * a = nullptr;
  const GFNT_FontRecord * b = nullptr;
  for (size_t i = 0; i < gfnt_fontset_count(f.scan()); ++i) {
    const GFNT_FontRecord * r = gfnt_fontset_at(f.scan(), i);
    (std::string(r->family) == "V" ? a : b) = r;
  }
  ASSERT_TRUE(a && b);
  EXPECT_TRUE(a->wght_axis.present);
  EXPECT_EQ(a->wght_axis.min, fx(100));
  EXPECT_EQ(a->wght_axis.max, fx(900));
  EXPECT_TRUE(a->wdth_axis.present);
  EXPECT_FALSE(a->slnt_axis.present);
  EXPECT_FALSE(b->wght_axis.present);
  EXPECT_EQ(b->oblique_angle, fx(9)) << "post.italicAngle, negated";
  EXPECT_EQ(b->legacy_family, nullptr) << "the legacy family equals the typographic one";
}

// --- local() ---------------------------------------------------------------------

namespace {

size_t local_pick(const GFNT_FontSet * set, const char * name, bool * found) {
  size_t index = 99;
  EXPECT_EQ(gfnt_fontset_match_local(set, name, &index, found, nullptr), GFNT_OK);
  return index;
}

}  // namespace

TEST(MatchLocal, AFullNameOrAPostScriptNameFindsItsFaceWithoutRegardToCase) {
  Fonts f;
  Spec a;
  a.family = "Alpha";
  a.label = "Regular";
  a.full = "Alpha Regular";
  a.postscript = "Alpha-Regular";
  Spec b;
  b.family = "Alpha";
  b.label = "Bold";
  b.weight = 700;
  b.full = "Alpha Bold";
  b.postscript = "Alpha-Bold";
  f.add(a);
  f.add(b);
  const GFNT_FontSet * set = f.scan();
  bool found = false;
  size_t i = local_pick(set, "alpha bold", &found);
  ASSERT_TRUE(found);
  EXPECT_STREQ(gfnt_fontset_at(set, i)->style, "Bold");
  i = local_pick(set, "ALPHA-REGULAR", &found);
  ASSERT_TRUE(found);
  EXPECT_STREQ(gfnt_fontset_at(set, i)->style, "Regular");
  EXPECT_STREQ(gfnt_fontset_at(set, i)->postscript_name, "Alpha-Regular");
}

TEST(MatchLocal, ANameNoFaceHasIsNotFoundAndAFamilyAloneIsNotAFullName) {
  Fonts f;
  Spec a;
  a.family = "Alpha";
  a.label = "Regular";
  a.full = "Alpha Regular";
  f.add(a);
  const GFNT_FontSet * set = f.scan();
  bool found = true;
  local_pick(set, "Alpha", &found);
  EXPECT_FALSE(found);
  local_pick(set, "", &found);
  EXPECT_FALSE(found) << "a face with no PostScript name is not found by the empty name";
  local_pick(set, "Alpha Regular Italic", &found);
  EXPECT_FALSE(found);
}

TEST(MatchLocal, AFullNameOutranksAPostScriptNameAndTheFirstFaceWinsAmongEquals) {
  Fonts f;
  Spec a;
  a.family = "A";
  a.label = "one";
  a.postscript = "Shared";
  Spec b;
  b.family = "B";
  b.label = "two";
  b.full = "Shared";
  Spec c;
  c.family = "C";
  c.label = "three";
  c.full = "Shared";
  f.add(a);
  f.add(b);
  f.add(c);
  const GFNT_FontSet * set = f.scan();
  bool found = false;
  const size_t i = local_pick(set, "shared", &found);
  ASSERT_TRUE(found);
  const std::string style = gfnt_fontset_at(set, i)->style;
  EXPECT_TRUE(style == "two" || style == "three") << "a full name, not the PostScript name";
  EXPECT_NE(style, "one");
  // Among the full-name matches the set's own order decides.
  for (size_t k = 0; k < gfnt_fontset_count(set); ++k) {
    const GFNT_FontRecord * r = gfnt_fontset_at(set, k);
    if (std::strcmp(r->full_name, "Shared") == 0) {
      EXPECT_EQ(k, i);
      break;
    }
  }
}

TEST(MatchLocal, TheNamesSurviveASerialisedSet) {
  Fonts f;
  Spec a;
  a.family = "Alpha";
  a.label = "Regular";
  a.full = "Alpha Regular";
  a.postscript = "Alpha-Regular";
  f.add(a);
  const GFNT_FontSet * set = f.scan();
  size_t need = 0;
  ASSERT_EQ(gfnt_fontset_serialize(set, nullptr, 0, &need, nullptr), GFNT_ERR_LIMIT);
  std::vector<uint8_t> bytes(need);
  ASSERT_EQ(gfnt_fontset_serialize(set, bytes.data(), need, &need, nullptr), GFNT_OK);
  GFNT_FontSet * back = nullptr;
  ASSERT_EQ(gfnt_fontset_deserialize(bytes.data(), bytes.size(), nullptr, &back, nullptr),
      GFNT_OK);
  bool found = false;
  const size_t i = local_pick(back, "alpha-regular", &found);
  EXPECT_TRUE(found);
  EXPECT_STREQ(gfnt_fontset_at(back, i)->full_name, "Alpha Regular");
  gfnt_fontset_free(back);
}

TEST(MatchLocal, ABadCallIsRefused) {
  size_t i = 0;
  bool found = false;
  EXPECT_EQ(gfnt_fontset_match_local(nullptr, "x", &i, &found, nullptr), GFNT_ERR_INVALID);
  Fonts f;
  Spec a;
  a.family = "A";
  a.label = "one";
  f.add(a);
  EXPECT_EQ(gfnt_fontset_match_local(f.scan(), nullptr, &i, &found, nullptr), GFNT_ERR_INVALID);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
