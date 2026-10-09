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
"""Subset every corpus font, and hold the subset to the original.

documentation/design.md section 12.5. A subsetter is right when text shapes the
same in the subset as in the font it came from, and when every glyph that shaping
used still draws. This writes each corpus font's subset for the characters of a
sample text with the glyph ids retained (the mode that keeps the layout tables,
and so the only one HarfBuzz can shape both of) and then asks:

  * **HarfBuzz**, shaping the text with the original and with the subset: the two
    runs must agree in every glyph, cluster, advance and offset. This is the
    whole of what retaining the ids promises, and it is HarfBuzz's reading of the
    tables this library wrote or carried over, not this library's.
  * **fontTools**, which must open the subset; whose reading of each glyph the
    text shaped to must be the original's; and whose own subsetter, asked for the
    same characters with every layout feature, gives the glyph set this one is
    scored against. Glyphs it keeps that this library did not are a closure that
    missed something; glyphs this library kept that it did not are the
    over-approximation, reported as a count.

A font this library declines to subset (a CFF outline, a variable font) is counted
and named, never dropped from the denominator.

Usage: subset_diff.py [--fonts N] [--groups a,b] [--quiet]
"""

import json
import os
import unicodedata
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import hb_diff
import oracle_env

ROOT = oracle_env.ROOT
SCRATCH = os.path.join(ROOT, "build", "oracle", "subset")
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-subset")
DUMP = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-dump")
# Shaped by these and not by GSUB/GPOS: the subsetter does not carry them (design.md
# section 12.3), so HarfBuzz shapes the subset differently by construction.
AAT = ("morx", "mort", "kerx", "trak")


def aat_tables(font):
    run = subprocess.run([DUMP, font], capture_output=True, text=True)
    return [t for t in AAT if "table '%s'" % t in run.stdout]
KEYS = ("g", "cl", "ax", "ay", "dx", "dy")


def norm(run):
    return [tuple(x.get(k) for k in KEYS) for x in run]


def parse_hb(path):
    with open(path, encoding="utf-8") as handle:
        return [json.loads(l) if l.startswith("[") else [] for l in handle.read().split("\n")[:-1]]


def main(argv):
    quiet = "--quiet" in argv
    limit = None
    only = None
    for i, a in enumerate(argv[1:], start=1):
        if a == "--fonts":
            limit = int(argv[i + 1])
        elif a == "--groups":
            only = set(argv[i + 1].split(","))
    os.makedirs(SCRATCH, exist_ok=True)
    fonts = corpus.fonts("sfnt")
    if limit:
        fonts = fonts[:limit]
    groups = [g for g in hb_diff.GROUPS if not g[5] and (only is None or g[0] in only)]

    tasks = []          # (font, group name, subset path, text file, iso, unicodes)
    declined = {}
    refused = []
    aat = []
    for fi, font in enumerate(fonts):
        if aat_tables(font):
            aat.append(font)
            continue
        for gi, group in enumerate(groups):
            texts = group[6]
            # The shaper composes and decomposes before it looks at the font, a pair
            # at a time, so the characters it looks up include the intermediate
            # compositions: the NFC and NFD forms of the whole text and of every pair
            # of its characters. A caller who subsets by text has to ask for them,
            # as with fontTools.
            wanted = set()
            for t in texts:
                for form in (t, unicodedata.normalize("NFC", t), unicodedata.normalize("NFD", t)):
                    wanted.update(form)
                # A base composes with a mark that is not next to it (the marks between
                # them are not blocking), so every pair of the text's characters is
                # tried, not only the neighbours.
                chars = sorted(set(t) | set(unicodedata.normalize("NFD", t))
                               | set(unicodedata.normalize("NFC", t)))
                for x in chars:
                    for y in chars:
                        pair = x + y
                        wanted.update(unicodedata.normalize("NFC", pair))
                        wanted.update(unicodedata.normalize("NFD", pair))
            # And the characters whose glyphs the shaper borrows when text reaches
            # something the font lacks: the space, the digit and the full stop that
            # size the figure and punctuation spaces, the hyphen a non-breaking
            # hyphen falls back to, and the dotted circle a broken cluster is set on.
            cps = sorted({ord(c) for c in wanted if ord(c) >= 0x20}
                         | {0x20, 0x30, 0x2E, 0x2010, 0x25CC})
            out = os.path.join(SCRATCH, "s-%d-%d.ttf" % (fi, gi))
            run = subprocess.run([DRIVER, "--retain-gids", "--unicodes",
                                  ",".join("%X" % c for c in cps), font, out],
                                 capture_output=True, text=True)
            if run.returncode != 0:
                why = run.stderr.strip().splitlines()[0] if run.stderr.strip() else "?"
                if "nsupported" in why:
                    declined[font] = why
                else:
                    refused.append((font, group[0], run.stderr.strip()[:200]))
                if "nsupported" in why:
                    break      # the same font fails the same way for every group
                continue
            text = os.path.join(SCRATCH, "t-%d.txt" % gi)
            if not os.path.exists(text):
                with open(text, "w", encoding="utf-8") as h:
                    for t in texts:
                        h.write(t + "\n")
            tasks.append((font, group, out, text, cps))

    # HarfBuzz, original and subset, in one container.
    os.makedirs(os.path.join(SCRATCH, "out"), exist_ok=True)
    script = os.path.join(SCRATCH, "run.sh")
    with open(script, "w", encoding="utf-8") as h:
        for ti, (font, group, sub, text, _cps) in enumerate(tasks):
            for tag, path in (("o", font), ("s", sub)):
                out = os.path.join(SCRATCH, "out", "%d%s.json" % (ti, tag))
                parts = ["hb-shape", "--font-file=%s" % path, "--output-format=json",
                         "--no-glyph-names"]
                if group[1]:
                    parts.append("--script=%s" % group[1])
                parts.append("--text-file=%s" % text)
                h.write(" ".join("'%s'" % p for p in parts) + " > '%s' 2>/dev/null\n" % out)
    argv_hb = oracle_env.command("harfbuzz", ["sh", script], scratch=SCRATCH)
    subprocess.run(argv_hb, capture_output=True, text=True)

    shaped = disagree = skipped = 0
    bad = []
    jobs = []
    meta = []
    for ti, (font, group, sub, text, cps) in enumerate(tasks):
        a = parse_hb(os.path.join(SCRATCH, "out", "%do.json" % ti))
        b = parse_hb(os.path.join(SCRATCH, "out", "%ds.json" % ti))
        flat = [g for run in a for g in run]
        mapped = [g for g in flat if g.get("g")]
        if not flat or len(mapped) * 2 < len(flat):
            skipped += 1      # the font lacks most of this group: nothing to compare
            continue
        shaped += 1
        if [norm(r) for r in a] != [norm(r) for r in b]:
            disagree += 1
            bad.append((os.path.basename(font), group[0]))
        jobs.append({"original": font, "subset": sub, "unicodes": cps,
                     "gids": sorted({g["g"] for g in flat})})
        meta.append((os.path.basename(font), group[0]))

    jobs_path = os.path.join(SCRATCH, "jobs.json")
    results_path = os.path.join(SCRATCH, "results.json")
    with open(jobs_path, "w", encoding="utf-8") as h:
        json.dump(jobs, h)
    check = os.path.join(ROOT, "tools", "oracle", "fonttools_subset_check.py")
    argv_ft = oracle_env.command("fonttools", ["python3", check, jobs_path, results_path],
                                 scratch=SCRATCH)
    finished = subprocess.run(argv_ft, capture_output=True, text=True)
    unopened = drawn = missed = 0
    extra = theirs = ours = 0
    problems = []
    if finished.returncode == 0:
        with open(results_path, encoding="utf-8") as h:
            results = json.load(h)
        for (name, grp), r in zip(meta, results):
            if not r.get("opens") or "error" in r:
                unopened += 1
                problems.append((name, grp, r.get("error")))
                continue
            if r.get("different"):
                drawn += 1
                problems.append((name, grp, "glyphs differ: %s" % r["different"][:3]))
            if r.get("missing"):
                missed += 1
                problems.append((name, grp, "fontTools keeps %s, this does not" % r["missing"][:5]))
            extra += r.get("extra", 0)
            theirs += r.get("theirs", 0)
            ours += r.get("ours", 0)
    else:
        problems.append(("fontTools", "", finished.stderr[-300:]))

    print("fonts %d, groups %d: %d subsets compared (%d groups skipped for lack of glyphs)"
          % (len(fonts), len(groups), shaped, skipped))
    print("declined (unsupported): %d fonts; AAT shaping tables, not carried: %d fonts; "
          "refused otherwise: %d" % (len(declined), len(aat), len(refused)))
    print("HarfBuzz: %d of %d shaped differently in the subset" % (disagree, shaped))
    print("fontTools: %d did not open, %d with a glyph read differently, "
          "%d with a glyph its closure keeps and this one lacks"
          % (unopened, drawn, missed))
    if theirs:
        print("glyphs with outlines kept: fontTools %d, this library %d (%d it did not)"
              % (theirs, ours, extra))
    by_group = {}
    for name, grp in bad:
        by_group[grp] = by_group.get(grp, 0) + 1
    if by_group:
        print("HarfBuzz differences by group:",
              ", ".join("%s %d" % kv for kv in sorted(by_group.items())))
    with open(os.path.join(SCRATCH, "bad.txt"), "w", encoding="utf-8") as h:
        for name, grp in bad:
            h.write("%s\t%s\n" % (name, grp))
    if not quiet:
        for item in bad[:20]:
            print("  hb  ", *item)
        for item in problems[:20]:
            print("  ft  ", *item)
        for item in refused[:10]:
            print("  refused", *item)
    return 1 if (disagree or unopened or drawn or missed or refused) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
