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
 * The fallback: a face for a character the primary face lacks, chosen from a scanned set
 * (design.md section 11), and the provider that hands it to paragraph layout. Each test builds
 * faces that differ in what their `cmap` maps and what their `OS/2` claims, and says which one
 * the order of families, the preference lists and the style rules choose.
 */

#include "test_helpers.h"
#include "failing_allocator.h"
#include "font_tree.h"

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/layout.h>
#include <ghoti.io/font/provider.h>

#include <gtest/gtest.h>

namespace {

/** A face mapping the given characters (each its own range), labelled. */
Spec face(const char * family, const char * label, std::vector<uint32_t> chars) {
  Spec s;
  s.family = family;
  s.label = label;
  s.maps.clear();
  for (uint32_t c : chars) {
    s.maps.push_back({c, c});
  }
  return s;
}

struct Fallback {
  GFNT_FontFallback * f = nullptr;
  ~Fallback() { gfnt_fontfallback_free(f); }
  GFNT_Result make(const GFNT_FontSet * set, const GFNT_FontFallbackOptions * o = nullptr,
      const GFNT_Allocator * a = nullptr) {
    return gfnt_fontfallback_create(set, o, a, &f, nullptr);
  }
  /** The label of the face that maps @p cp, or "". */
  std::string label(const GFNT_FontSet * set, uint32_t cp, GFNT_Tag script = 0,
      GFNT_Tag language = 0, const GFNT_FontQuery * wanted = nullptr) {
    const GFNT_Face * out = nullptr;
    size_t index = 0;
    bool found = false;
    EXPECT_EQ(gfnt_fontfallback_face(f, cp, script, language, wanted, &out, &index, &found,
                  nullptr), GFNT_OK);
    if (!found) {
      return "";
    }
    EXPECT_NE(out, nullptr);
    return gfnt_fontset_at(set, index)->style;
  }
};

}  // namespace

TEST(Fallback, AFaceThatMapsTheCharacterIsFoundAndOthersAreNot) {
  Fonts fonts;
  fonts.add(face("Latin", "latin", {'A', 'B'}));
  fonts.add(face("Greek", "greek", {0x3B1, 0x3B2}));
  const GFNT_FontSet * set = fonts.scan();
  Fallback fb;
  ASSERT_EQ(fb.make(set), GFNT_OK);
  EXPECT_EQ(fb.label(set, 'B'), "latin");
  EXPECT_EQ(fb.label(set, 0x3B2), "greek");
  EXPECT_EQ(fb.label(set, 0x4E00), "") << "nothing maps it";
}

TEST(Fallback, TheFaceItReturnsIsOpenAndStaysTheSameOneAcrossCalls) {
  Fonts fonts;
  fonts.add(face("Latin", "latin", {'A', 'B'}));
  const GFNT_FontSet * set = fonts.scan();
  Fallback fb;
  ASSERT_EQ(fb.make(set), GFNT_OK);
  const GFNT_Face * a = nullptr;
  const GFNT_Face * b = nullptr;
  bool found = false;
  ASSERT_EQ(gfnt_fontfallback_face(fb.f, 'A', 0, 0, nullptr, &a, nullptr, &found, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_fontfallback_face(fb.f, 'B', 0, 0, nullptr, &b, nullptr, &found, nullptr), GFNT_OK);
  ASSERT_TRUE(found);
  EXPECT_EQ(a, b) << "one open face, not one per call";
  uint32_t glyph = 0;
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(a, 'B', &glyph, nullptr), GFNT_OK);
  EXPECT_NE(glyph, 0u);
}

TEST(Fallback, AScriptPreferenceChoosesItsFamilyOverAnEarlierOne) {
  Fonts fonts;
  fonts.add(face("First", "first", {0x4E00}));
  fonts.add(face("Second", "second", {0x4E00}));
  const GFNT_FontSet * set = fonts.scan();
  Fallback plain;
  ASSERT_EQ(plain.make(set), GFNT_OK);
  const std::string unpreferred = plain.label(set, 0x4E00, GFNT_TAG('h', 'a', 'n', 'i'));
  const char * families[] = {"Second"};
  GFNT_FontScriptPreference pref{GFNT_TAG('h', 'a', 'n', 'i'), 0, families, 1};
  GFNT_FontFallbackOptions options{&pref, 1};
  Fallback fb;
  ASSERT_EQ(fb.make(set, &options), GFNT_OK);
  EXPECT_EQ(fb.label(set, 0x4E00, GFNT_TAG('h', 'a', 'n', 'i')), "second");
  EXPECT_EQ(fb.label(set, 0x4E00, GFNT_TAG('l', 'a', 't', 'n')), unpreferred)
      << "another script keeps the set's order";
  EXPECT_TRUE(unpreferred == "first" || unpreferred == "second");
}

TEST(Fallback, ALanguagePreferenceBeatsTheScriptsAndOnlyAppliesToThatLanguage) {
  Fonts fonts;
  fonts.add(face("Gothic", "gothic", {0x4E00}));
  fonts.add(face("Song", "song", {0x4E00}));
  fonts.add(face("Mincho", "mincho", {0x4E00}));
  const GFNT_FontSet * set = fonts.scan();
  const char * any[] = {"Song"};
  const char * ja[] = {"Mincho"};
  GFNT_FontScriptPreference prefs[2] = {
      {GFNT_TAG('h', 'a', 'n', 'i'), 0, any, 1},
      {GFNT_TAG('h', 'a', 'n', 'i'), GFNT_TAG('J', 'A', 'N', ' '), ja, 1}};
  GFNT_FontFallbackOptions options{prefs, 2};
  Fallback fb;
  ASSERT_EQ(fb.make(set, &options), GFNT_OK);
  const GFNT_Tag hani = GFNT_TAG('h', 'a', 'n', 'i');
  EXPECT_EQ(fb.label(set, 0x4E00, hani, GFNT_TAG('J', 'A', 'N', ' ')), "mincho");
  EXPECT_EQ(fb.label(set, 0x4E00, hani, GFNT_TAG('Z', 'H', 'S', ' ')), "song");
  EXPECT_EQ(fb.label(set, 0x4E00, hani, 0), "song");
}

TEST(Fallback, APreferredFamilyThatLacksTheCharacterIsPassedOver) {
  Fonts fonts;
  fonts.add(face("Preferred", "preferred", {'A'}));
  fonts.add(face("Other", "other", {0x4E00}));
  const GFNT_FontSet * set = fonts.scan();
  const char * families[] = {"Preferred", "Missing"};
  GFNT_FontScriptPreference pref{GFNT_TAG('h', 'a', 'n', 'i'), 0, families, 2};
  GFNT_FontFallbackOptions options{&pref, 1};
  Fallback fb;
  ASSERT_EQ(fb.make(set, &options), GFNT_OK);
  EXPECT_EQ(fb.label(set, 0x4E00, GFNT_TAG('h', 'a', 'n', 'i')), "other");
}

TEST(Fallback, TheWantedFamilyIsTriedBeforeTheRest) {
  Fonts fonts;
  fonts.add(face("Alpha", "alpha", {0x3B1}));
  fonts.add(face("Beta", "beta", {0x3B1}));
  const GFNT_FontSet * set = fonts.scan();
  Fallback fb;
  ASSERT_EQ(fb.make(set), GFNT_OK);
  const char * beta[] = {"Beta"};
  GFNT_FontQuery wanted;
  gfnt_fontquery_init(&wanted);
  wanted.families = beta;
  wanted.family_count = 1;
  EXPECT_EQ(fb.label(set, 0x3B1, 0, 0, &wanted), "beta");
  const char * alpha[] = {"alpha"};
  wanted.families = alpha;
  EXPECT_EQ(fb.label(set, 0x3B1, 0, 0, &wanted), "alpha") << "caselessly";
}

TEST(Fallback, WithinAFamilyTheFaceNearestTheWantedStyleIsChosen) {
  Fonts fonts;
  Spec regular = face("Sans", "regular", {0x3B1});
  fonts.add(regular);
  Spec bold = face("Sans", "bold", {0x3B1});
  bold.weight = 700;
  fonts.add(bold);
  Spec italic = face("Sans", "italic", {0x3B1});
  italic.italic = true;
  fonts.add(italic);
  const GFNT_FontSet * set = fonts.scan();
  Fallback fb;
  ASSERT_EQ(fb.make(set), GFNT_OK);
  GFNT_FontQuery wanted;
  gfnt_fontquery_init(&wanted);
  wanted.weight = fx(700);
  EXPECT_EQ(fb.label(set, 0x3B1, 0, 0, &wanted), "bold");
  wanted.weight = 0;
  wanted.slant = GFNT_SLANT_ITALIC;
  EXPECT_EQ(fb.label(set, 0x3B1, 0, 0, &wanted), "italic");
  wanted.slant = GFNT_SLANT_ROMAN;
  EXPECT_EQ(fb.label(set, 0x3B1, 0, 0, &wanted), "regular");
}

TEST(Fallback, AFaceThatLacksTheCharacterIsSkippedForOneInTheSameFamilyThatHasIt) {
  // The bold face matches the style best but does not map the character.
  Fonts fonts;
  Spec regular = face("Sans", "regular", {0x3B1});
  fonts.add(regular);
  Spec bold = face("Sans", "bold", {'A'});
  bold.weight = 700;
  fonts.add(bold);
  const GFNT_FontSet * set = fonts.scan();
  Fallback fb;
  ASSERT_EQ(fb.make(set), GFNT_OK);
  GFNT_FontQuery wanted;
  gfnt_fontquery_init(&wanted);
  wanted.weight = fx(700);
  EXPECT_EQ(fb.label(set, 0x3B1, 0, 0, &wanted), "regular");
}

TEST(Fallback, AClaimIsOnlyAHint) {
  // Bit 7 is Greek and Coptic. One face claims it and does not map the character; another
  // claims nothing at all and does.
  Fonts fonts;
  Spec liar = face("Liar", "liar", {'A'});
  liar.claim_bits = {7};
  fonts.add(liar);
  Spec honest = face("Honest", "honest", {0x3B1});
  fonts.add(honest);
  const GFNT_FontSet * set = fonts.scan();
  Fallback fb;
  ASSERT_EQ(fb.make(set), GFNT_OK);
  EXPECT_EQ(fb.label(set, 0x3B1), "honest");
}

TEST(Fallback, AFaceThatClaimsOtherRangesIsTriedOnlyWhenNoClaimantMapsTheCharacter) {
  // The Latin-only claimant maps Greek too (a mistake in its claims); a Greek claimant exists.
  Fonts fonts;
  Spec latin = face("Latin", "latin-but-maps-greek", {'A', 0x3B1});
  latin.claim_bits = {0};
  fonts.add(latin);
  Spec greek = face("Greek", "greek", {0x3B1});
  greek.claim_bits = {7};
  fonts.add(greek);
  const GFNT_FontSet * set = fonts.scan();
  Fallback fb;
  ASSERT_EQ(fb.make(set), GFNT_OK);
  EXPECT_EQ(fb.label(set, 0x3B1), "greek") << "the claimant is preferred";
  EXPECT_EQ(fb.label(set, 'A'), "latin-but-maps-greek");
  // A character neither claims is still found, by the face that maps it.
  Fonts only;
  Spec stray = face("Stray", "stray", {0x4E00});
  stray.claim_bits = {0};
  only.add(stray);
  const GFNT_FontSet * set2 = only.scan();
  Fallback fb2;
  ASSERT_EQ(fb2.make(set2), GFNT_OK);
  EXPECT_EQ(fb2.label(set2, 0x4E00), "stray");
}

TEST(Fallback, ACharacterBeyondThePlaneZeroIsClaimedByBitFiftySeven) {
  // Two faces map the same character in plane 1. One claims only the Latin range, the other
  // sets bit 57 (any character beyond the Basic Multilingual Plane); the second is listed
  // later and is still the one chosen, because it claims the character and the first does not.
  Fonts fonts;
  Spec bmp = face("Bmp", "bmp", {0x1F600});
  bmp.claim_bits = {0};
  fonts.add(bmp);
  Spec emoji = face("Emoji", "emoji", {0x1F600});
  emoji.claim_bits = {57};
  fonts.add(emoji);
  const GFNT_FontSet * set = fonts.scan();
  Fallback fb;
  ASSERT_EQ(fb.make(set), GFNT_OK);
  EXPECT_EQ(fb.label(set, 0x1F600), "emoji");
}

TEST(Fallback, AFaceThatSetsNoRangeBitsClaimsEverythingAndKeepsItsPlaceInTheOrder) {
  Fonts fonts;
  fonts.add(face("Silent", "silent", {0x3B1}));          // sets no bits: claims every character
  Spec claimant = face("Claimant", "claimant", {0x3B1});
  claimant.claim_bits = {7};                              // Greek and Coptic
  fonts.add(claimant);
  const GFNT_FontSet * set = fonts.scan();
  Fallback fb;
  ASSERT_EQ(fb.make(set), GFNT_OK);
  EXPECT_EQ(fb.label(set, 0x3B1), "silent") << "listed first, and it claims the character too";
}

TEST(Fallback, AFileThatWentAwayAfterTheScanIsSkipped) {
  Fonts fonts;
  fonts.add(face("Gone", "gone", {0x3B1}));
  fonts.add(face("Here", "here", {0x3B1}));
  const GFNT_FontSet * set = fonts.scan();
  fonts.remove_file(0);
  Fallback fb;
  ASSERT_EQ(fb.make(set), GFNT_OK);
  EXPECT_EQ(fb.label(set, 0x3B1), "here");
}

TEST(Fallback, AnEmptySetFindsNothing) {
  Fonts fonts;
  const GFNT_FontSet * set = fonts.scan();
  Fallback fb;
  ASSERT_EQ(fb.make(set), GFNT_OK);
  EXPECT_EQ(fb.label(set, 'A'), "");
}

TEST(Fallback, BadArgumentsAreRefused) {
  Fonts fonts;
  fonts.add(face("A", "a", {'A'}));
  const GFNT_FontSet * set = fonts.scan();
  GFNT_FontFallback * f = nullptr;
  EXPECT_EQ(gfnt_fontfallback_create(nullptr, nullptr, nullptr, &f, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_fontfallback_create(set, nullptr, nullptr, nullptr, nullptr), GFNT_ERR_INVALID);
  GFNT_FontFallbackOptions bad{nullptr, 1};
  EXPECT_EQ(gfnt_fontfallback_create(set, &bad, nullptr, &f, nullptr), GFNT_ERR_INVALID);
  const GFNT_Face * face_out = nullptr;
  bool found = false;
  EXPECT_EQ(gfnt_fontfallback_face(nullptr, 'A', 0, 0, nullptr, &face_out, nullptr, &found, nullptr),
      GFNT_ERR_INVALID);
  gfnt_fontfallback_free(nullptr);
}

TEST(Fallback, EveryAllocationFailureIsReportedAndNothingLeaks) {
  Fonts fonts;
  fonts.add(face("A", "a", {'A'}));
  fonts.add(face("B", "b", {'B'}));
  const GFNT_FontSet * set = fonts.scan();
  const char * families[] = {"B"};
  GFNT_FontScriptPreference pref{GFNT_TAG('l', 'a', 't', 'n'), 0, families, 1};
  GFNT_FontFallbackOptions options{&pref, 1};
  size_t requests = 0;
  {
    gfnttest::FailingAllocator counting(static_cast<size_t>(-1));
    Fallback fb;
    ASSERT_EQ(fb.make(set, &options, counting.get()), GFNT_OK);
    EXPECT_EQ(fb.label(set, 'B', GFNT_TAG('l', 'a', 't', 'n')), "b");
    requests = counting.requests();
    gfnt_fontfallback_free(fb.f);
    fb.f = nullptr;
    EXPECT_EQ(counting.live(), 0u);
  }
  ASSERT_GT(requests, 5u);
  for (size_t fail = 0; fail < requests; ++fail) {
    gfnttest::FailingAllocator failing(fail);
    GFNT_FontFallback * f = nullptr;
    GFNT_Result r = gfnt_fontfallback_create(set, &options, failing.get(), &f, nullptr);
    if (r == GFNT_OK) {
      const GFNT_Face * out = nullptr;
      bool found = false;
      r = gfnt_fontfallback_face(f, 'B', GFNT_TAG('l', 'a', 't', 'n'), 0, nullptr, &out, nullptr,
          &found, nullptr);
    }
    if (r != GFNT_OK) {
      EXPECT_EQ(r, GFNT_ERR_OOM) << "request " << fail;
    }
    gfnt_fontfallback_free(f);
    EXPECT_EQ(failing.live(), 0u) << "request " << fail << " leaked";
  }
}

// --- the provider -----------------------------------------------------------------

TEST(FallbackProvider, ParagraphLayoutDrawsACharacterTheSpansFacesLackWithTheSetsFace) {
  Fonts fonts;
  fonts.add(face("Primary", "primary", {'A'}));
  fonts.add(face("Backup", "backup", {'B'}));
  const GFNT_FontSet * set = fonts.scan();
  Fallback fb;
  ASSERT_EQ(fb.make(set), GFNT_OK);
  // The span's own face: the set's "Primary", opened by the caller.
  const GFNT_FontRecord * prim = nullptr;
  for (size_t i = 0; i < gfnt_fontset_count(set); ++i) {
    if (std::string(gfnt_fontset_at(set, i)->family) == "Primary") {
      prim = gfnt_fontset_at(set, i);
    }
  }
  ASSERT_NE(prim, nullptr);
  GFNT_Blob * blob = nullptr;
  GFNT_Face * primary = nullptr;
  ASSERT_EQ(gfnt_blob_create_file(prim->path, nullptr, nullptr, &blob, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_load(blob, prim->index, nullptr, nullptr, &primary, nullptr), GFNT_OK);

  GFNT_ParagraphOptions options{};
  options.font_provider = gfnt_fontfallback_provider(fb.f);
  const GFNT_Face * faces[1] = {primary};
  const std::string text = "AB";
  GFNT_TextSpan span{};
  span.length = text.size();
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = 16 * 64;
  GFNT_Paragraph * paragraph = nullptr;
  ASSERT_EQ(gfnt_paragraph_create(text.data(), text.size(), &span, 1, &options, nullptr,
                &paragraph, nullptr), GFNT_OK);
  GFNT_Layout layout{};
  ASSERT_EQ(gfnt_paragraph_layout(paragraph, GFNT_LAYOUT_UNBOUNDED, &layout, nullptr), GFNT_OK);
  bool backup_run = false;
  for (size_t i = 0; i < layout.lines[0].run_count; ++i) {
    const GFNT_LineRun & run = layout.lines[0].runs[i];
    if (run.fallback) {
      backup_run = true;
      EXPECT_NE(run.face, primary);
      EXPECT_EQ(run.start, 1u);
      EXPECT_NE(run.glyphs[0].glyph, 0u) << "drawn with a face that maps B";
    }
  }
  EXPECT_TRUE(backup_run);
  gfnt_layout_free(&layout);
  gfnt_paragraph_free(paragraph);
  gfnt_face_free(primary);
  gfnt_blob_destroy(blob);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
