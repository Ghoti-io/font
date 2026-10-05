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
letters, marks with no precomposed form, numerals, punctuation. Scripts that need
a script shaper of their own (Arabic, the Indic scripts, Hangul) are *not* in the
corpus, because HarfBuzz would shape them with a shaper this library does not
have, and a comparison where one side declines to do the work is not a comparison.
They are listed in `GAPS` below, with what is missing, so that the absence is a
statement and not an omission.

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
    "normalisation": "HarfBuzz composes a base and a mark when the font has the "
                     "composite, and decomposes a character the font lacks",
    "script shapers": "Arabic joining, Indic reordering, Hangul jamo, the "
                      "Universal Shaping Engine",
    "fallback mark positioning": "a combining mark in a font whose GPOS has no mark feature, "
                                 "which HarfBuzz places from the glyph's extents",
    "bidi and vertical text": "one direction, horizontal, per run",
    "AAT layout": "a font with morx or kerx is shaped by Apple's state machines, "
                  "not by GSUB and GPOS",
    "variation rounding": "a variation delta that is exactly a half rounds up "
                          "in HarfBuzz and away from zero here: a unit for an "
                          "advance and a unit for a kern, and no more",
}

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
GREEK = [
    "Αλφάβητο Τάξη Υ ωΩ",
    "ΑΒΓΔΕ αβγδε ΤΑ ΓΑ",
]
CYRILLIC = [
    "Привет мир ДЖ ёЁ",
    "Таким образом ЛТ ГА ТА та",
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


class Skip(Exception):
    """This font cannot be compared, with a reason worth printing."""

    def __init__(self, why, expected=False):
        super().__init__(why)
        self.expected = expected


def tag_arg(tag):
    return tag if len(tag) == 4 else tag.ljust(4)


def corpus_fonts(args):
    if args:
        return [os.path.abspath(a) for a in args]
    found = list(corpus.fonts("sfnt"))
    if os.path.isdir(FIXTURES):
        for name in sorted(os.listdir(FIXTURES)):
            if name.endswith((".ttf", ".otf")):
                found.append(os.path.join(FIXTURES, name))
    return found


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
    parts.append("--text-file=%s" % lines)
    return " ".join("'%s'" % p.replace("'", "'\\''") for p in parts) + \
        " > '%s' 2> '%s.err'" % (out, out)


def ours(font, group, lines):
    name, iso, ot, lang, ot_lang, features, texts = group[:7]
    variations = group[7] if len(group) > 7 else ""
    argv = [DRIVER, "--batch"]
    if variations:
        argv += ["--location", variations]
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


MARKS = range(0x0300, 0x0370)


def has_gpos_mark(font):
    """Whether the font's GPOS has a `mark` feature at all.

    HarfBuzz positions a combining mark itself, from the glyph's extents and the
    character's combining class, whenever the font does not say where marks go: a
    font with no GPOS, and also one whose GPOS has kerning and nothing else. The
    question this answers is the second half of that, which is the one that is
    easy to miss. Asked of this library's own layout dump, so the answer is about
    the same bytes both shapers read.
    """
    finished = subprocess.run([DRIVER, "--layout", font], capture_output=True,
                              text=True)
    gpos = finished.stdout.split("\nGPOS:", 1)
    return len(gpos) == 2 and " mark:" in gpos[1]


DUMP = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                    "font-dump")


def has_aat(font):
    """Whether the font carries Apple's layout tables, which HarfBuzz prefers.

    A font with `morx` is shaped by its state machines and not by `GSUB`, and one
    with `kerx` is kerned by that and not by `GPOS`: a different engine, which
    this library does not have, answering for the font.
    """
    finished = subprocess.run([DUMP, font], capture_output=True, text=True)
    return "table 'morx'" in finished.stdout or "table 'kerx'" in finished.stdout


def within_rounding(want, got):
    """Whether two answers differ only by the rounding of a variation delta.

    At a location between a variable font's corners a variation delta is a
    fraction of a unit - an advance, a kern, an anchor. The two sides round it
    differently *where it is exactly a half*: HarfBuzz (and fontTools) round half
    up, so -37.5 is -37, and this library rounds half away from zero, so it is
    -38 (design.md section 5.2, which believed HarfBuzz did the same; at Inter's
    opsz=23 every one of 23 negative ties went HarfBuzz's way, and every
    non-tie agreed). An advance is a delta from `HVAR` and a kern may be another,
    so a glyph can be two units apart and no more. Glyphs and clusters are never
    allowed to differ. The corners of the design space, where every scalar is 0 or
    1 and there is nothing to round, are held exactly.
    """
    if len(want) != len(got):
        return False
    for a, b in zip(want, got):
        if a[0] != b[0] or a[1] != b[1]:
            return False
        if any(abs(x - y) > 2 for x, y in zip(a[2:], b[2:])):
            return False
    return True


def classify(text, want, got, mark_feature, aat=False, varied=False):
    """Which known gap, if any, explains a disagreement - or None.

    These are the things HarfBuzz does that this library does not (see GAPS and
    shape.h), recognised by their cause and not by the font or the string, so that
    a *new* kind of disagreement in the same fonts still fails the run:

      * normalisation: a character the font has no glyph for, that HarfBuzz then
        takes apart into base and mark (and this library leaves as .notdef);
      * fallback mark positioning: a combining mark in a font whose GPOS does not
        say where marks go, where HarfBuzz places the mark from the glyph's extents
        and its combining class.
    """
    if aat:
        return "AAT layout"
    if varied and within_rounding(want, got):
        return "variation rounding"
    codepoints = [ord(c) for c in text]
    for glyph in got:
        cluster = glyph[1]
        if glyph[0] == 0 and cluster < len(codepoints) and \
                codepoints[cluster] >= 0xC0:
            return "normalisation"
    if not mark_feature and any(c in MARKS for c in codepoints):
        # Only what happens to a mark is the gap: HarfBuzz re-derives the glyph
        # classes of the marks from Unicode, takes their advance away, and places
        # them. The glyphs and the clusters are still held to the reference, so
        # that a line with a mark in it is not excused for everything else.
        if len(want) == len(got) \
                and all(a[0] == b[0] and a[1] == b[1] for a, b in zip(want, got)):
            return "fallback mark positioning"
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
        for group in (own if own is not None else groups):
            # One lines file per distinct corpus: the shared groups reuse theirs.
            key = hashlib.sha1("\n".join(group[6]).encode("utf-8")).hexdigest()[:12]
            lines = write_inputs(key, group)
            tasks.append((font, group, lines))
        if own is None and sweep:
            for group in feature_groups(font):
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
    gpos_of = {}
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
                if font not in gpos_of:
                    gpos_of[font] = has_gpos_mark(font)
                marks = (not gpos_of[font]) and any(ord(c) in MARKS for c in text)
                rows.append("\t".join([
                    os.path.basename(font), group[2] or "-", group[4] or "-",
                    group[5] or "-", (group[7] if len(group) > 7 else "") or "-",
                    " ".join("%04X" % ord(c) for c in text),
                    "marks" if marks else "-",
                    " ".join("/".join(str(v) for v in g) for g in want) or "-"]))
            if isinstance(got, dict):
                refused[got.get("error")] = refused.get(got.get("error"), 0) + 1
                continue
            compared += 1
            glyphs += len(want)
            if got == want:
                continue
            if font not in gpos_of:
                gpos_of[font] = has_gpos_mark(font)
            if font not in aat_of:
                aat_of[font] = has_aat(font)
            gap = classify(text, want, got, gpos_of[font], aat_of[font],
                           bool(len(group) > 7 and group[7])
                           and cases_for(font) is None)
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
              GAPS.get(gap, GAPS.get("normalisation"))))
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
# units, and the flag `marks` says the case has a combining mark in a font whose
# GPOS has no mark feature, where HarfBuzz places the mark itself (the engine is
# held to the glyphs and clusters there, not to the mark's position).
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
