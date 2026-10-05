#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
#
# Ghoti.io Font is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""What a line of text shapes to, this library against HarfBuzz.

documentation/design.md section 14. The shaper is the first part of this library
whose output is not a fact about a file but a decision about text: which lookups
run, in what order, what a context does after a nested lookup changed the length,
which glyph a mark attaches to. The specification leaves several of those open,
and a shaper that decides them differently produces different text from the one
every browser draws. So the reference is HarfBuzz, and the comparison is the
whole answer for every glyph: its index, the cluster it stands for, its advance
and its offset.

What it runs, per font and per group of (script, language, features):

  * every string of that group's corpus, shaped once by `hb-shape` in its image
    and once by `font-shape --batch` here, from the same bytes of the same file;
  * the two answers compared glyph by glyph, as JSON, so that a lost glyph is a
    length mismatch and not every later glyph disagreeing.

**What it can and cannot see.** The strings are chosen to be the ones the default
shaper's own pipeline decides: kerning pairs, ligature candidates, accented
letters, marks with no precomposed form, numerals, punctuation. A script that needs
a shaper of its own is held to HarfBuzz too, with texts made from the characters
each font has (the `auto` groups, which find the syllables a font can draw) and,
where a script's shaper works on what the font has and not on the text alone
(Hangul), a group of its own. What is still missing is listed in `GAPS` below, so
that the absence is a statement and not an omission.

A refusal on this library's side is counted and listed, never skipped: a font this
library will not shape is a font whose strings are in the denominator.

Usage:
    hb_diff.py [--quiet] [--fonts N] [--groups name,...] [font...]
"""

import hashlib
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env
import unskippable

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-shape")
SCRATCH = os.path.join(ROOT, "build", "oracle", "hb")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")

# What this library does not do that HarfBuzz does, so that a disagreement in one
# of these areas is read as the gap and not as a defect - and so that the corpus
# below visibly avoids them rather than quietly omitting them.
GAPS = {
    "bidi text": "one direction per run",
    "AAT layout": "a font with morx or kerx is shaped by Apple's state machines, "
                  "not by GSUB and GPOS",
    "Indic character details": "a handful of characters in Gujarati, Oriya and Telugu "
                               "that HarfBuzz sorts or syllabifies differently from "
                               "the Unicode data: found by the generated texts, not "
                               "yet traced to a rule",
    "USE character categories": "a handful of characters in Grantha, Javanese, Khudawadi, "
                                "Mongolian, Tai Viet and Tirhuta that HarfBuzz sorts into "
                                "a different Universal Shaping Engine category than the "
                                "Unicode data and Microsoft's overrides give: found by the "
                                "generated texts, not yet traced to a rule",
    "outline HarfBuzz does not read": "a Type 1 charstring in a CFF table, or a cubic glyf "
                                      "outline: HarfBuzz measures them wrongly or not at all, "
                                      "so a mark placed from their box differs",
}

# The Indic scripts in which a few characters still differ.
INDIC_DETAIL_SCRIPTS = set("gujr orya telu".split())

# The Universal Shaping Engine scripts in which a few characters still differ.
USE_DETAIL_SCRIPTS = set("gran java sind mong tavt tirh".split())

# Scripts whose shaper this library has not got: none. A text in one of them would
# be generated from the font (`auto` groups) and shaped as if it were Latin, and the
# differential holds the scripts that have a shaper.
UNSHAPED_SCRIPTS = set()

# Fixtures whose outlines are of a kind HarfBuzz does not read. A mark is placed
# against its base's box when the font has no GPOS, and the box is the one thing
# these two cannot agree on.
UNREAD_OUTLINES = {"cff-type1.otf", "outline-cubic.ttf", "outline-cubic-flag.ttf"}

LATIN = [
    "The quick brown fox jumps over the lazy dog",
    "AVATAR To Wa Yo VA LT P. F, r. 7.1",
    "Wavy waves, TAWAY; VAT: Ty. Te. Yo. Fa. Pa. Lo",
    "office affluent difficult fluff fi fl ffi ffl ff",
    "ij IJ st ct sp Th fj ft fb fh fk",
    "0123456789 1/2 3/4 1st 2nd 10% $5 #1 @x &",
    "( ) [ ] { } « » – — … ‘ ’ “ ”",
    "   spaced    out  ",
    "x́ q̇ j̣ x̣ q́ q̇́",
    "Héllo wörld ÀÉÎÕÜ çñß ÿ",
    "ÅÄÖ åäö Ææ Øø Þþ Ðð Łł Šš Žž",
    "ı İ ii II",
    "a‍b a‌b f‌f f‍f",
]
# Text whose shape depends on normalisation: a precomposed character the font may
# lack, a base and mark the font may have a composite for, marks that arrive out of
# canonical order, singletons that decompose to another character, and the spaces
# and joiners a shaper must size or skip. Written as escapes so that the file says
# what it means.
FORMS = [
    "\u00e9 e\u0301 \u00c5 A\u030a \u212b \u2126 \u0391\u0342",
    "a\u0323\u0302 a\u0302\u0323 \u1ea1\u0302 \u1ec7 e\u0323\u0302 e\u0302\u0323",
    "\u1ebf \u1ec1 \u1ec3 \u1ec5 o\u031b\u0301 \u01a1\u0301 u\u031b\u0323",
    "a\u0301\u0300\u0302 a\u0300\u0301 o\u0308\u0304 o\u0304\u0308 \u01d8 \u01d7",
    "a\u0345\u0301 \u03b1\u0345 \u1f80 \u1f71 \u03ac \u0385 \u03b9\u0308\u0301",
    "a\u2002b a\u2003b a\u2004b a\u2005b a\u2006b a\u2007b a\u2008b a\u2009b a\u200ab",
    "a\u00a0b a\u202fb a\u205fb a\u3000b a\u2011b a\u00adb a\u2010b",
    "e\u034f\u0301 e\u0301\u034f\u0300 a\u034fb x\u034f\u0323\u0301",
    "a\u200db a\u200cb \u0e01\u0e33 \u1100\u1161 \uac00 \uac01",
    "\u0627\u0301 \u05d0\u05b7 e\u20dd \u0301a \u0301\u0301",
    "\u00c0\u00c1\u00c2\u00c3\u00c4\u00c5\u00c6\u00c7\u00c8\u00c9\u00ca\u00cb",
    "\u0100\u0101\u0102\u0103\u0104\u0105\u0106\u0107\u010c\u010d\u0158\u0159",
]
# The scripts that have a shaper of their own. Each corpus is real words and the
# characters a shaper has rules for, written as escapes so the file says what it
# means. A group names a probe code point, and only fonts that map it are shaped:
# a font with no Arabic shapes every Arabic text to .notdef the same way twice.
ARABIC = [
    "(مرحبا) [بالعالم] {جميل} «نعم»",
    "\u0645\u0631\u062d\u0628\u0627 \u0628\u0627\u0644\u0639\u0627\u0644\u0645",
    "\u0627\u0644\u0633\u0644\u0627\u0645 \u0639\u0644\u064a\u0643\u0645",
    "\u0628\u0633\u0645 \u0627\u0644\u0644\u0647 \u0627\u0644\u0631\u062d\u0645\u0646 \u0627\u0644\u0631\u062d\u064a\u0645",
    "\u0644\u0627 \u0644\u0623 \u0644\u0625 \u0644\u0622 \u0627\u0644\u0644\u0647",
    "\u0645\u064f\u062d\u064e\u0645\u0651\u064e\u062f \u0643\u0650\u062a\u064e\u0627\u0628",
    "\u0640\u0640\u0640 \u0628\u0640\u0628 \u0633\u0640\u0640\u0640\u0645",
    "\u06af\u0686\u067e\u0698\u06a4 \u06ba\u06be\u06d2 \u0679\u0688\u0691 \u06cc\u06d3",
    "\u0661\u0662\u0663 \u06f1\u06f2\u06f3 abc \u0645\u0631\u062d\u0628\u0627 123",
    "\ufefb \ufdf2 \ufe8d\ufe8e \ufe91\ufe92\ufe93",
    "\u0628\u200d\u0628 \u0628\u200c\u0628 \u0628\u200d \u200d\u0628",
    "\u0623\u0625\u0622\u0671\u0674\u0675\u0676\u0677 \u0624\u0626\u0621",
    "\u0628\u0651\u064e\u064b\u0650 \u0628\u064e\u0651 \u0644\u0651\u0670\u0627",
]
HEBREW = [
    "(שלום) [עולם] {טוב} «כן»",
    "\u05e9\u05dc\u05d5\u05dd \u05e2\u05d5\u05dc\u05dd",
    "\u05d1\u05b0\u05bc\u05e8\u05b5\u05d0\u05e9\u05b4\u05c1\u05d9\u05ea \u05d1\u05b8\u05bc\u05e8\u05b8\u05d0",
    "\u05d0\u05b1\u05dc\u05b9\u05d4\u05b4\u05d9\u05dd \u05d4\u05b7\u05e9\u05b8\u05c1\u05de\u05b7\u05d9\u05b4\u05dd",
    "\u05e9\u05c1 \u05e9\u05c2 \ufb2c \ufb2d \ufb2a \ufb2b \ufb49 \ufb4a",
    "\u05d0\u05b7 \u05d0\u05b8 \ufb30 \u05d1\u05bf \u05db\u05bc \u05e4\u05bc",
    "\u05e9\u05b0\u05c1\u05dc\u05b9\u05de\u05b9\u05d4 \u05d9\u05b0\u05d4\u05d5\u05bc\u05d3\u05b8\u05d4",
    "\u05d1\u05b8\u05be\u05d0\u05b8\u05d3\u05b8\u05dd \u05d0\u05b7\u05e8\u05b0\u05d1\u05bc\u05b8\u05e2\u05b8\u05d4",
    "\u05f2\u05b7 \u05f0 \u05f1 \u05f2 \u05f3 \u05f4 abc \u05e9\u05dc\u05d5\u05dd 123",
    "\u05d0\u05b9\u05b7 \u05d0\u05b7\u05b9 \u05d0\u05bc\u05b7 \u05d0\u05b7\u05bc \u05d0\u05c7\u05b8",
]
SYRIAC = [
    "\u0710\u0723\u0718\u072a\u071d\u0710 \u0712\u0720\u0710",
    "\u0710\u0712\u0718\u0722 \u0715\u0712\u0328\u0308\u072c\u0710",
    "\u0710\u0720\u0729 \u0715\u0719 \u072a\u0712 \u0715\u0710 \u0717\u0718",
    "\u0712\u0710\u0718\u0720 \u0720\u0710\u0718",
    "\u0710\u0723\u0718\u072a\u071d\u0710\u0711\u0308 \u0715\u0714\u0715",
]
THAANA = [
    "\u078b\u07a8\u0788\u07ac\u0780\u07a8 \u0784\u07a6\u0790\u07b0",
    "\u0780\u07a8\u0783\u07a6\u078e\u07a6\u0782\u07b0 \u0787\u07a6\u0787\u07a8",
]
THAI = [
    "\u0e2a\u0e27\u0e31\u0e2a\u0e14\u0e35\u0e04\u0e23\u0e31\u0e1a",
    "\u0e19\u0e49\u0e33 \u0e1b\u0e35\u0e4b \u0e02\u0e36\u0e49\u0e19 \u0e01\u0e35\u0e48 \u0e40\u0e01\u0e4b \u0e01\u0e47 \u0e01\u0e48\u0e32",
    "\u0e2a\u0e33\u0e19\u0e31\u0e01\u0e07\u0e32\u0e19 \u0e15\u0e33\u0e23\u0e27\u0e08 \u0e01\u0e33 \u0e01\u0e49\u0e33 \u0e01\u0e4a\u0e33",
    "\u0e0d \u0e10 \u0e0e \u0e0f \u0e0d\u0e39 \u0e10\u0e38 \u0e0e\u0e38 \u0e0f\u0e39",
    "\u0e1c\u0e39\u0e49\u0e43\u0e2b\u0e0d\u0e48 \u0e40\u0e14\u0e47\u0e01\u0e46 \u0e1b\u0e0f\u0e34\u0e1a\u0e31\u0e15\u0e34",
    "\u0e1b\u0e32\u0e01\u0e1b\u0e39\u0e48 \u0e1d\u0e31\u0e48\u0e07 \u0e1b\u0e48\u0e32 \u0e1f\u0e35\u0e48 \u0e1d\u0e37\u0e19 \u0e1b\u0e34\u0e4a\u0e01",
    "\u0e01\u0e33\u0e48 \u0e01\u0e48\u0e33 \u0e01\u0e4d\u0e32 \u0e01\u0e4d\u0e49\u0e32 \u0e01\u0e33\u0e4a \u0e01\u0e34\u0e4d\u0e32",
    "abc \u0e2a\u0e27\u0e31\u0e2a\u0e14\u0e35 123 \u0e3f\u0e52\u0e53",
]
LAO = [
    "\u0eaa\u0eb0\u0e9a\u0eb2\u0e8d\u0e94\u0eb5",
    "\u0e99\u0ec9\u0eb3 \u0e81\u0eb3 \u0e81\u0ec9\u0eb3 \u0e81\u0ecd\u0ec9\u0eb2 \u0e81\u0eb4\u0ecd\u0eb2",
    "\u0eab\u0ebc\u0ea7\u0e87 \u0e9e\u0eb0\u0e9a\u0eb2\u0e87 \u0eab\u0ea1\u0eb2 \u0eab\u0e99\u0eb2 \u0eab\u0ea5\u0eb2",
    "\u0e81\u0ebb\u0e99 \u0e81\u0eb8 \u0e81\u0eb9 \u0e81\u0ec8\u0eb2 \u0e81\u0eb9\u0ec9",
]
HANGUL = [
    # Precomposed, fully decomposed, and the half-way forms in between.
    "\ud55c\uae00 \uc548\ub155\ud558\uc138\uc694",
    "\u1112\u1161\u11ab \u1100\u1173\u11af \u110b\u1161\u11ab\u1102\u1167\u11bc",
    "\ud558\u11ab \uac00\u11a8 \ud55c\u11ab \uac01\u11a8 \ud558\u11ab\u11ab",
    "\u1100\u1161 \u1112\u1161\u1100 \u1100\u1100\u1161 \u1161\u11a8 \u11a8",
    "\u1100\u1161\u11a8\u11a8 \ud558\u1161 \uac00\u1161\u11a8",
    # Tone marks, with a syllable and without.
    "\ud55c\u302e \ud55c\u302f \u302e \u1112\u1161\u11ab\u302e \u302f\u1100\u1161",
    "abc \ud55c\uae00 123 \u1112\u1161\u11ab",
]
GREEK = [
    "Αλφάβητο Τάξη Υ ωΩ",
    "ΑΒΓΔΕ αβγδε ΤΑ ΓΑ",
]
CYRILLIC = [
    "Привет мир ДЖ ёЁ",
    "Таким образом ЛТ ГА ТА та",
]

CJK = [
    "日本語のテキスト、これは「例」です。",
    "漢字（かんじ）と、ひらがな・カタカナ！",
    "ABC 日本 123 ー。",
    "ＡＢＣ　全角　スペース",
    "「あ」「い」（う）〜…―",
]

# (name, ISO 15924 script, OpenType script, BCP 47 language or None, OpenType
# language system or 0, HarfBuzz feature list, corpus). The two language tags are
# the same language spelled the two ways each side takes it.
GROUPS = [
    ("latin", "Latn", "latn", None, "", "", LATIN),
    ("latin -kern", "Latn", "latn", None, "", "-kern", LATIN),
    ("latin -liga", "Latn", "latn", None, "", "-liga,-clig", LATIN),
    ("latin discretionary", "Latn", "latn", None, "",
        "+dlig,+smcp,+c2sc,+hlig", LATIN),
    ("latin figures", "Latn", "latn", None, "",
        "+onum,+pnum,+tnum,+lnum,+zero,+frac,+case,+sups,+subs", LATIN),
    ("latin alternates", "Latn", "latn", None, "",
        "+salt=1,+ss01,+ss02,+cv01=1,+aalt=2", LATIN),
    ("latin ranges", "Latn", "latn", None, "", "liga[2:6],kern[3:9]=0,-ccmp[1]",
        LATIN),
    ("turkish", "Latn", "latn", "tr", "TRK ", "", LATIN),
    ("romanian", "Latn", "latn", "ro", "ROM ", "", LATIN),
    ("dutch", "Latn", "latn", "nl", "NLD ", "", LATIN),
    ("forms", "Latn", "latn", None, "", "", FORMS),
    ("forms -ccmp", "Latn", "latn", None, "", "-ccmp", FORMS),
    ("arabic", "Arab", "arab", None, "", "", ARABIC, "", 0x627),
    ("arabic -liga", "Arab", "arab", None, "", "-liga,-rlig,-calt", ARABIC, "", 0x627),
    ("hebrew", "Hebr", "hebr", None, "", "", HEBREW, "", 0x5d0),
    ("syriac", "Syrc", "syrc", None, "", "", SYRIAC, "", 0x710),
    ("thaana", "Thaa", "thaa", None, "", "", THAANA, "", 0x780),
    ("thai", "Thai", "thai", None, "", "", THAI, "", 0xe01),
    ("lao", "Laoo", "lao ", None, "", "", LAO, "", 0xe81),
    ("hangul", "Hang", "hang", None, "", "", HANGUL, "", 0xac00),
    ("vertical latin", "Latn", "latn", None, "", "", LATIN, "", 0, "ttb"),
    ("vertical cjk", "Hani", "hani", None, "", "", CJK, "", 0x3001, "ttb"),
    ("vertical cjk btt", "Hani", "hani", None, "", "", CJK, "", 0x3001, "btt"),
    ("vertical -vert", "Hani", "hani", None, "", "-vert,+vkrn", CJK, "", 0x3001,
        "ttb"),
    ("greek", "Grek", "grek", None, "", "", GREEK),
    ("cyrillic", "Cyrl", "cyrl", None, "", "", CYRILLIC),
]


# The BCP 47 tag HarfBuzz takes for an OpenType language system a case names.
BCP47 = {"TRK ": "tr", "ROM ": "ro", "NLD ": "nl", "DEU ": "de", "FRA ": "fr",
         "ENG ": "en", "CAT ": "ca", "PLK ": "pl", "VIT ": "vi", "AZE ": "az"}


def cases_for(font):
    """The groups a fixture's own cases file asks for, or None if it has none.

    A synthetic fixture has a handful of glyphs, so the Latin corpus below would
    shape to .notdef and prove nothing; its cases file names the runs that reach
    its lookups, one per line, and they are grouped here by what HarfBuzz is told
    about them (script, language, features) so that each group is one batch.
    """
    path = os.path.splitext(font)[0] + ".shape"
    if not os.path.exists(path):
        return None
    groups = {}
    order = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            if not line.strip() or line.startswith("#"):
                continue
            script, language, features, location, points, _note = (
                line.rstrip("\n").split("\t") + [""] * 6)[:6]
            key = (None if script == "-" else script,
                   None if language == "-" else language,
                   None if features == "-" else features,
                   None if location == "-" else location)
            if key not in groups:
                groups[key] = []
                order.append(key)
            groups[key].append("".join(chr(int(p, 16)) for p in points.split()))
    out = []
    for key in order:
        script, language, features, location = key
        name = "%s %s %s%s" % (os.path.basename(font), script or "auto",
                               features or "default",
                               " at " + location if location else "")
        out.append((name, script.capitalize() if script else None, script,
                    BCP47.get(language) if language else None,
                    language or "", features or "", groups[key],
                    location or ""))
    return out


INFO = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                    "font-info")

_variable = None


def variable_fonts():
    """The corpus fonts that have a design space (file names)."""
    global _variable
    if _variable is None:
        # By file name: the variable corpus is materialised beside the sfnt one, so
        # the same font is two paths.
        _variable = {os.path.basename(p) for kind in ("variable", "cff2")
                     for p in corpus.fonts(kind)}
    return _variable


def location_groups(font):
    """A variable font at the corners and the middle of its design space.

    One group per location, over the same Latin corpus the default instance is
    held to. What it reaches that the default does not: advances through `HVAR` (or
    through `gvar`'s phantom points), a kern moved by a variation index in `GDEF`'s
    store, and the lookups a `FeatureVariations` record substitutes.
    """
    if os.path.basename(font) not in variable_fonts():
        return []
    finished = subprocess.run([INFO, font], capture_output=True, text=True)
    axes = []
    for line in finished.stdout.splitlines():
        if line.startswith("axis "):
            parts = line.split()
            # axis 0: wght  100 .. 400 .. 900  Weight
            axes.append((parts[2], float(parts[3]), float(parts[5]),
                         float(parts[7])))
    if not axes:
        return []
    places = []
    for tag, low, default, high in axes:
        places.append("%s=%g" % (tag, low))
        places.append("%s=%g" % (tag, high))
        places.append("%s=%g" % (tag, (default + high) / 2))
    places.append(",".join("%s=%g" % (t, h) for t, l, d, h in axes))
    places.append(",".join("%s=%g" % (t, l) for t, l, d, h in axes))
    out = []
    for place in places:
        out.append(("%s at %s" % (os.path.basename(font), place), "Latn",
                    "latn", None, "", "", LATIN, place))
    return out


def feature_groups(font, limit=48):
    """Every feature the font has, one at a time, over the Latin corpus.

    The default groups turn on the features a shaper turns on by itself, which is
    a handful; a font has dozens more (small capitals, figures, stylistic sets,
    alternates, fractions), and each is a lookup of some type this library has to
    apply as HarfBuzz does. Turning each on alone reaches them one by one, so a
    disagreement names the feature.
    """
    finished = subprocess.run([DRIVER, "--layout", font], capture_output=True,
                              text=True)
    tags = []
    for line in finished.stdout.splitlines():
        line = line.strip()
        if line.startswith("feature ") and line.endswith(":") or ": " in line \
                and line.startswith("feature "):
            parts = line.split()
            if len(parts) >= 3:
                tag = parts[2].rstrip(":")
                if len(tag) == 4 and tag not in tags:
                    tags.append(tag)
    out = []
    for tag in tags[:limit]:
        out.append(("%s +%s" % (os.path.basename(font), tag), "Latn", "latn",
                    None, "", "+" + tag, LATIN))
        out.append(("%s +%s=2" % (os.path.basename(font), tag), "Latn", "latn",
                    None, "", "%s=2" % tag, LATIN))
    return out


def auto_groups(font):
    """The group of text made out of what this font maps, in the script it is for.

    `font-samples` builds syllables from the characters the font has and says which
    script they are in; the same font always gives the same text. It is what lets
    the differential reach the hundred and thirty scripts the corpus has fonts for
    without a hand-written corpus for each.
    """
    finished = subprocess.run([SAMPLES, font], capture_output=True, text=True)
    lines = finished.stdout.split("\n")
    if finished.returncode != 0 or not lines or not lines[0].startswith("#script "):
        return []
    _, iso, ot = lines[0].split(" ", 2)
    texts = [t for t in lines[1:] if t]
    if not texts:
        return []
    return [("auto %s" % iso, iso, ot, None, "", "", texts)]


class Skip(Exception):
    """This font cannot be compared, with a reason worth printing."""

    def __init__(self, why, expected=False):
        super().__init__(why)
        self.expected = expected


def tag_arg(tag):
    return tag if len(tag) == 4 else tag.ljust(4)


_covered = {}


def covered(font, probes):
    """The probe code points the font maps, by asking this library's driver once."""
    key = (font, tuple(sorted(probes)))
    if key not in _covered:
        text = "".join(chr(p) + "\n" for p in sorted(probes))
        finished = subprocess.run([DRIVER, "--batch", "--script", "latn", font],
                                  input=text, capture_output=True, text=True)
        have = set()
        for probe, line in zip(sorted(probes),
                               finished.stdout.split("\n")):
            try:
                glyphs = json.loads(line)
            except ValueError:
                continue
            if isinstance(glyphs, list) and glyphs and glyphs[0]["g"] != 0:
                have.add(probe)
        _covered[key] = have
    return _covered[key]


# Real fonts with a layout table taken out, so that the paths for a font without
# one are held to HarfBuzz too: Arabic from presentation forms, marks from their
# boxes, glyph classes from the characters. (source file, what to remove.)
DERIVED = [
    ("DejaVuSans.ttf", ("nogsub", "nogpos", "nogdef", "nolayout")),
    ("DejaVuSerif.ttf", ("nogsub", "nolayout")),
    ("tahoma.ttf", ("nogsub", "nogpos", "nogdef", "nolayout")),
    ("titr.ttf", ("nogsub",)),
    ("UKIJTor.ttf", ("nogsub", "nolayout")),
]
DERIVED_DIR = os.path.join(ROOT, "build", "oracle", "hb-derived")


def derived_fonts(found):
    """The fonts DERIVED names, made in the fontTools image and listed here."""
    by_name = {os.path.basename(f): f for f in found}
    wanted = []
    for name, variants in DERIVED:
        if name in by_name:
            for variant in variants:
                wanted.append("%s:%s" % (by_name[name], variant))
    if not wanted:
        return []
    os.makedirs(DERIVED_DIR, exist_ok=True)
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "derive_fonts.py")
    argv = oracle_env.command("fonttools",
        ["python3", script, DERIVED_DIR] + wanted, scratch=DERIVED_DIR)
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        sys.stderr.write("deriving fonts failed: %s\n" % finished.stderr[-400:])
        return []
    out = []
    for item in wanted:
        source, variant = item.rsplit(":", 1)
        path = os.path.join(DERIVED_DIR, "%s.%s.ttf" % (
            os.path.splitext(os.path.basename(source))[0], variant))
        if os.path.exists(path):
            out.append(path)
    return out


def corpus_fonts(args):
    if args:
        return [os.path.abspath(a) for a in args]
    found = list(corpus.fonts("sfnt"))
    if os.path.isdir(FIXTURES):
        for name in sorted(os.listdir(FIXTURES)):
            if name.endswith((".ttf", ".otf")):
                found.append(os.path.join(FIXTURES, name))
    return found + derived_fonts(found)


def write_inputs(key, group):
    """The group's lines, one text per line, as a file both sides read."""
    path = os.path.join(SCRATCH, "lines-%s.txt" % key)
    if not os.path.exists(path):
        with open(path, "w", encoding="utf-8") as handle:
            for text in group[6]:
                handle.write(text + "\n")
    return path


def hb_command(font, group, lines, out):
    name, iso, ot, lang, ot_lang, features, texts = group[:7]
    variations = group[7] if len(group) > 7 else ""
    parts = ["hb-shape", "--font-file=%s" % font, "--output-format=json",
             "--no-glyph-names"]
    if iso:
        parts.append("--script=%s" % iso)
    if lang:
        parts.append("--language=%s" % lang)
    if features:
        parts.append("--features=%s" % features)
    if variations:
        parts.append("--variations=%s" % variations)
    if len(group) > 9 and group[9]:
        parts.append("--direction=%s" % group[9])
    parts.append("--text-file=%s" % lines)
    return " ".join("'%s'" % p.replace("'", "'\\''") for p in parts) + \
        " > '%s' 2> '%s.err'" % (out, out)


def ours(font, group, lines):
    name, iso, ot, lang, ot_lang, features, texts = group[:7]
    variations = group[7] if len(group) > 7 else ""
    argv = [DRIVER, "--batch"]
    if variations:
        argv += ["--location", variations]
    if len(group) > 9 and group[9]:
        argv.append("--" + group[9])
    if ot:
        argv += ["--script", ot]
    if ot_lang:
        argv += ["--language", ot_lang]
    if features:
        argv += ["--features", features]
    argv.append(font)
    with open(lines, "rb") as handle:
        finished = subprocess.run(argv, stdin=handle, capture_output=True,
                                  text=True)
    if finished.returncode != 0:
        raise Skip(finished.stderr.strip().splitlines()[0]
                   if finished.stderr.strip() else "the driver refused")
    return finished.stdout.split("\n")[:-1]


def parse(line):
    line = line.strip()
    if not line:
        return []
    value = json.loads(line)
    if isinstance(value, dict):
        return value
    return [(g["g"], g["cl"], g["ax"], g["ay"], g["dx"], g["dy"])
            for g in value]


def ours_parse(line):
    value = json.loads(line)
    if isinstance(value, dict):
        return value
    return [(g["g"], g["cl"], g["ax"], g["ay"], g["dx"], g["dy"])
            for g in value]


DUMP = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                    "font-dump")
SAMPLES = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                       "font-samples")


def has_vertical(font):
    """Whether the file carries a `vmtx` (or a `VORG`): the fonts that vertical
    text means something for. A collection is searched whole."""
    with open(font, "rb") as handle:
        data = handle.read()
    return b"vmtx" in data or b"VORG" in data


def has_aat(font):
    """Whether the font carries Apple's layout tables, which HarfBuzz prefers.

    A font with `morx` is shaped by its state machines and not by `GSUB`, and one
    with `kerx` is kerned by that and not by `GPOS`: a different engine, which
    this library does not have, answering for the font.
    """
    finished = subprocess.run([DUMP, font], capture_output=True, text=True)
    return "table 'morx'" in finished.stdout or "table 'kerx'" in finished.stdout


def classify(text, font, want, got, aat=False, script=None):
    """Which known gap, if any, explains a disagreement - or None.

    These are the things HarfBuzz does that this library does not (see GAPS and
    shape.h), recognised by their cause and not by the font or the string, so that
    a *new* kind of disagreement in the same fonts still fails the run.
    """
    if aat:
        return "AAT layout"
    if script in UNSHAPED_SCRIPTS:
        return "script shaper not written"
    if script in USE_DETAIL_SCRIPTS:
        return "USE character categories"
    if script in INDIC_DETAIL_SCRIPTS:
        return "Indic character details"
    if os.path.basename(font) in UNREAD_OUTLINES and \
            any(0x0300 <= ord(c) < 0x0370 for c in text):
        return "outline HarfBuzz does not read"
    return None


def kinds(want, got):
    """What differs between two answers, coarsest first: a glyph count that does
    not match, then which of the six numbers differ in the first glyph that does."""
    if len(want) != len(got):
        return "count"
    for a, b in zip(want, got):
        if a == b:
            continue
        names = ["glyph", "cluster", "x_advance", "y_advance", "x_offset",
                 "y_offset"]
        return "+".join(n for n, x, y in zip(names, a, b) if x != y)
    return "none"


def batch_script(tasks):
    """The shell script that runs every hb-shape, one line per task."""
    lines = []
    for index, (font, group, lines_file) in enumerate(tasks):
        out = os.path.join(SCRATCH, "out", "%d.json" % index)
        lines.append(hb_command(font, group, lines_file, out) + "\n")
    return "".join(lines)


def run_reference(tasks):
    """One container, every hb-shape the run needs. Returns {task index: ok}."""
    os.makedirs(os.path.join(SCRATCH, "out"), exist_ok=True)
    script = os.path.join(SCRATCH, "run.sh")
    with open(script, "w", encoding="utf-8") as handle:
        handle.write(batch_script(tasks))
    argv = oracle_env.command("harfbuzz", ["sh", script], scratch=SCRATCH)
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode not in (0, 1):
        raise oracle_env.OracleUnavailable(
            "hb-shape batch failed: %s" % finished.stderr.strip()[:300])


def main(argv):
    quiet = "--quiet" in argv
    # Reuse the last run's HarfBuzz answers, for iterating on this library's side:
    # only valid when the fonts, groups and corpus are the ones that wrote them.
    reuse = "--reuse" in argv
    # Every feature a font has, one at a time: a longer run, and the one that finds
    # the lookups the default features never reach.
    sweep = "--sweep" in argv
    # One line per disagreement: font, group, kind, text - for sorting and counting.
    listing = "--list" in argv
    # The fixtures' cases, with HarfBuzz's answer recorded, so that the unit suite
    # can hold this library to HarfBuzz with no container: --write-golden writes
    # them, and --check-golden fails if the committed file is not what HarfBuzz
    # says now.
    golden_out = None
    golden_check = None
    strict = set()
    auto = "--no-auto" not in argv
    rows = []
    limit = None
    only = None
    rest = []
    skip = False
    for index, a in enumerate(argv[1:], start=1):
        if skip:
            skip = False
        elif a == "--fonts":
            limit = int(argv[index + 1])
            skip = True
        elif a == "--groups":
            only = set(argv[index + 1].split(","))
            skip = True
        elif a == "--strict":
            # Count the named gaps as disagreements, to see them in detail while
            # one is being closed.
            strict = set(argv[index + 1].split(","))
            skip = True
        elif a == "--write-golden":
            golden_out = argv[index + 1]
            skip = True
        elif a == "--check-golden":
            golden_check = argv[index + 1]
            skip = True
        elif a.startswith("--"):
            continue
        else:
            rest.append(a)
    fonts = corpus_fonts(rest)
    if limit:
        fonts = fonts[:limit]
    groups = [g for g in GROUPS if not only or g[0] in only]

    os.makedirs(SCRATCH, exist_ok=True)
    for name in os.listdir(SCRATCH):
        if name.startswith("lines-"):
            os.remove(os.path.join(SCRATCH, name))
    tasks = []
    for font in fonts:
        own = cases_for(font)
        probes = {g[8] for g in groups if len(g) > 8 and g[8]}
        have = covered(font, probes) if own is None and probes else set()
        for group in (own if own is not None else groups):
            if own is None and len(group) > 8 and group[8] \
                    and group[8] not in have:
                continue
            if len(group) > 9 and group[9] and not has_vertical(font):
                continue
            # One lines file per distinct corpus: the shared groups reuse theirs.
            key = hashlib.sha1("\n".join(group[6]).encode("utf-8")).hexdigest()[:12]
            lines = write_inputs(key, group)
            tasks.append((font, group, lines))
        if own is None and sweep:
            for group in feature_groups(font):
                key = hashlib.sha1("\n".join(group[6]).encode("utf-8")).hexdigest()[:12]
                tasks.append((font, group, write_inputs(key, group)))
        if own is None and auto:
            for group in auto_groups(font):
                key = hashlib.sha1("\n".join(group[6]).encode("utf-8")).hexdigest()[:12]
                tasks.append((font, group, write_inputs(key, group)))
        if own is None:
            for group in location_groups(font):
                key = hashlib.sha1("\n".join(group[6]).encode("utf-8")).hexdigest()[:12]
                tasks.append((font, group, write_inputs(key, group)))
    wanted = batch_script(tasks)
    script_path = os.path.join(SCRATCH, "run.sh")
    # Reused only if the batch that wrote the answers is, byte for byte, this one:
    # the answers are files numbered by position, so a different set of fonts or
    # groups would read someone else's output as its own.
    same = os.path.exists(script_path) and \
        open(script_path, encoding="utf-8").read() == wanted
    if not (reuse and same and os.path.exists(
            os.path.join(SCRATCH, "out", "%d.json" % (len(tasks) - 1)))):
        run_reference(tasks)

    compared = 0
    glyphs = 0
    disagreements = 0
    refused = {}
    skipped = []
    shown = 0
    per_font = {}
    by_kind = {}
    known = {}
    aat_of = {}
    by_group = {}
    by_text = {}
    for index, (font, group, lines) in enumerate(tasks):
        out = os.path.join(SCRATCH, "out", "%d.json" % index)
        theirs = open(out, encoding="utf-8").read().split("\n")[:-1] \
            if os.path.exists(out) else []
        texts = group[6]
        if len(theirs) != len(texts):
            err = open(out + ".err", encoding="utf-8").read().strip() \
                if os.path.exists(out + ".err") else ""
            skipped.append((font, "HarfBuzz: %s" % (err.splitlines()[0]
                            if err else "no output")))
            continue
        try:
            mine = ours(font, group, lines)
        except Skip as why:
            skipped.append((font, str(why)))
            continue
        if len(mine) != len(texts):
            skipped.append((font, "the driver printed %d lines for %d texts"
                            % (len(mine), len(texts))))
            continue
        for text, want_line, got_line in zip(texts, theirs, mine):
            want = parse(want_line)
            got = ours_parse(got_line)
            if cases_for(font) is not None:
                rows.append("\t".join([
                    os.path.basename(font), group[2] or "-", group[4] or "-",
                    group[5] or "-", (group[7] if len(group) > 7 else "") or "-",
                    " ".join("%04X" % ord(c) for c in text),
                    "-",
                    " ".join("/".join(str(v) for v in g) for g in want) or "-"]))
            if isinstance(got, dict):
                refused[got.get("error")] = refused.get(got.get("error"), 0) + 1
                continue
            compared += 1
            glyphs += len(want)
            if got == want:
                continue
            if font not in aat_of:
                aat_of[font] = has_aat(font)
            gap = classify(text, font, want, got, aat_of[font],
                           group[2] if group[0].startswith("auto ") else None)
            if gap in strict:
                gap = None
            if gap:
                known[gap] = known.get(gap, 0) + 1
                continue
            disagreements += 1
            per_font[font] = per_font.get(font, 0) + 1
            kind = kinds(want, got)
            by_kind[kind] = by_kind.get(kind, 0) + 1
            by_group[group[0]] = by_group.get(group[0], 0) + 1
            by_text[text] = by_text.get(text, 0) + 1
            if listing:
                print("DIFF\t%s\t%s\t%s\t%s" % (os.path.basename(font), group[0], kind, text))
            if not quiet and shown < 40 and per_font[font] <= 3:
                shown += 1
                sys.stderr.write("  %s [%s] %r\n    HarfBuzz   %s\n    this lib   %s\n"
                    % (os.path.basename(font), group[0], text, want, got))

    print("hb_diff: %d fonts, %d groups, %d lines compared, %d glyphs, "
          "%d unexplained disagreements, %d lines refused, %d font-groups skipped"
          % (len(fonts), len(groups), compared, glyphs, disagreements,
             sum(refused.values()), len(skipped)))
    for why, n in sorted(refused.items(), key=lambda kv: -kv[1]):
        print("  refused %d: %s" % (n, why))
    unexplained = [(f, w) for f, w in skipped]
    if not quiet:
        for font, why in unexplained[:20]:
            print("  skipped %s: %s" % (os.path.basename(font), why))
    for gap, n in sorted(known.items(), key=lambda kv: -kv[1]):
        print("  known gap, %d lines: %s - %s" % (n, gap,
              GAPS[gap]))
    if disagreements:
        print("  by what differs first: %s" % ", ".join(
            "%s %d" % kv for kv in sorted(by_kind.items(), key=lambda kv: -kv[1])))
        print("  by group: %s" % ", ".join(
            "%s %d" % kv for kv in sorted(by_group.items(), key=lambda kv: -kv[1])))
        print("  by text: %s" % "; ".join(
            "%r %d" % kv for kv in sorted(by_text.items(), key=lambda kv: -kv[1])[:8]))
        worst = sorted(per_font.items(), key=lambda kv: -kv[1])[:10]
        for font, n in worst:
            print("  %s: %d" % (os.path.basename(font), n))
    status = 1 if disagreements else 0
    # A font this run could not compare, for a reason nobody declared, is a font
    # the differential has stopped reaching: see tools/oracle/unskippable.py. And
    # a run that compared nothing is not a clean run.
    if unskippable.check("hb_diff", unexplained):
        status = 1
    if compared == 0:
        sys.stderr.write("hb_diff: nothing was compared\n")
        status = 1
    if golden_out or golden_check:
        status = max(status, golden(rows, golden_out, golden_check))
    return status


GOLDEN_HEADER = """\
# What HarfBuzz says each fixture case shapes to.
#
# Written by `tools/oracle/hb_diff.py --write-golden` from the harfbuzz image, and
# checked against it by `make check-oracle-hb`; test_shape.cpp holds this library
# to it with no container, so a change to the engine that moves an answer fails
# `make test` and not only the differential. Tab separated:
#   font  script  language  features  location  code points  flag  glyphs
# where each glyph is glyph/cluster/x_advance/y_advance/x_offset/y_offset in font
# units. The flag column is `-`: it once marked a case this engine was held to
# less than the whole answer on, and none is left.
"""


def golden(rows, out_path, check_path):
    """Write the golden file, or check the committed one against these rows."""
    # Ordered by font, and in the cases file's order within one, so that the file
    # is the same whichever fonts the run covered first.
    rows = sorted(rows, key=lambda row: row.split("\t", 1)[0])
    text = GOLDEN_HEADER + "\n".join(rows) + "\n"
    if out_path:
        with open(out_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        print("wrote %d golden rows to %s" % (len(rows), out_path))
    if check_path:
        with open(check_path, encoding="utf-8") as handle:
            committed = handle.read()
        if committed != text:
            have = set(committed.splitlines())
            want = set(text.splitlines())
            for line in sorted(want - have)[:5]:
                sys.stderr.write("  HarfBuzz says, not committed: %s\n" % line[:200])
            for line in sorted(have - want)[:5]:
                sys.stderr.write("  committed, HarfBuzz no longer says: %s\n" % line[:200])
            sys.stderr.write("golden: %s is not what HarfBuzz says now\n" % check_path)
            return 1
        print("golden: %d rows, all as HarfBuzz says" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
