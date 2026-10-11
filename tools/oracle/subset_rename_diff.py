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
"""Rename a subset of every corpus font and read the names back through fontTools.

    subset_rename_diff.py [--quiet] [--fonts N]

For each sfnt font (AAT-table fonts and the ones the subsetter declines are left out): subset a
few of its characters under a new family name, then check in fontTools that

  * the subset opens, and its Windows English family, full and PostScript names (and the
    typographic family where the original had one) are the new ones;
  * no `name` record that states a family (IDs 1, 3, 4, 6, 16, 18, 20, 21, 25) is left from the
    original, in any language or platform, and the original's family name occurs in none of them;
  * every other record, copyright and licence included, is carried unchanged;
  * a `CFF ` is named for the new PostScript name, and its Top DICT states no FullName or
    FamilyName and no Font DICT a FontName.

Nothing here judges how the new names are chosen; that is the unit tests'. This holds the
rewrite to a reader that is not this library over fonts whose `name` tables nobody here wrote.
"""

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-subset")
SCRATCH = os.path.join(ROOT, "build", "oracle", "subset-rename")
NEW = "Reserved Test Name"

# Run in the fontTools image: the characters of each font worth keeping.
PICK = r"""
import json, sys
from fontTools.ttLib import TTFont
out = {}
for path in sys.argv[2:]:
    try:
        f = TTFont(path, lazy=True, fontNumber=0 if path.endswith((".ttc", ".otc")) else -1)
        cmap = f.getBestCmap() or {}
        cps = [c for c in sorted(cmap) if c >= 0x20][:4]
        out[path] = cps
    except Exception as why:
        out[path] = []
json.dump(out, open(sys.argv[1], "w"))
"""

CHECK = r"""
import json, sys
from fontTools.ttLib import TTFont
FAMILY = {1, 3, 4, 6, 16, 18, 20, 21, 25}
pairs = json.load(open(sys.argv[1]))
new = sys.argv[3]
results = []
for original_path, subset_path in pairs:
    problems = []
    try:
        o = TTFont(original_path, lazy=True, fontNumber=0 if original_path.endswith((".ttc", ".otc")) else -1)
        s = TTFont(subset_path, lazy=True)
        old_family = None
        for nid in (16, 1):
            r = o["name"].getName(nid, 3, 1, 0x409) or o["name"].getName(nid, 3, 1)
            if r is not None:
                old_family = r.toUnicode()
                break
        def rec(font, r):
            return (r.platformID, r.platEncID, r.langID, r.nameID, r.toUnicode() if r.isUnicode() else r.string)
        carried = sorted(rec(o, r) for r in o["name"].names if r.nameID not in FAMILY)
        got = sorted(rec(s, r) for r in s["name"].names if r.nameID not in FAMILY)
        if carried != got:
            problems.append("a record that does not state the family changed")
        for r in s["name"].names:
            if r.nameID in FAMILY:
                text = r.toUnicode() if r.isUnicode() else r.string.decode("latin-1")
                if old_family and len(old_family) >= 4 and old_family in text and old_family not in new:
                    problems.append("old family left in id %d" % r.nameID)
        for nid in (1, 4):
            r = s["name"].getName(nid, 3, 1, 0x409)
            if r is None or new not in r.toUnicode():
                problems.append("id %d is not the new family" % nid)
        r = s["name"].getName(6, 3, 1, 0x409)
        if r is None or not r.toUnicode().startswith("ReservedTestName-"):
            problems.append("id 6 is not the new PostScript name")
        had16 = any(r.nameID == 16 for r in o["name"].names)
        if had16 and s["name"].getName(16, 3, 1, 0x409) is None:
            problems.append("id 16 not written")
        if not had16 and any(r.nameID == 16 for r in s["name"].names):
            problems.append("id 16 invented")
        if "CFF " in s:
            c = s["CFF "].cff
            if not c.fontNames[0].startswith("ReservedTestName-"):
                problems.append("CFF name %r" % c.fontNames[0])
            top = c.topDictIndex[0]
            for key in ("FullName", "FamilyName"):
                if key in top.rawDict:
                    problems.append("CFF Top DICT still has " + key)
            fda = getattr(top, "FDArray", None)
            if fda is not None:
                for fd in fda:
                    if "FontName" in fd.rawDict:
                        problems.append("a Font DICT still has FontName")
    except Exception as why:
        problems.append("%s: %s" % (type(why).__name__, why))
    results.append(problems)
json.dump(results, open(sys.argv[2], "w"))
"""

AAT = ("morx", "mort", "kerx", "trak")


def main(argv):
    quiet = "--quiet" in argv
    limit = None
    for i, a in enumerate(argv):
        if a == "--fonts":
            limit = int(argv[i + 1])
    os.makedirs(SCRATCH, exist_ok=True)
    fonts = corpus.fonts("sfnt")
    if limit:
        fonts = fonts[:limit]
    fixtures = [os.path.join(ROOT, "tests", "data", "fonts", n) for n in
                ("cff-names.otf", "cff-cid-names.otf", "variable-gvar.ttf", "variable-cff2.otf")]
    fonts = fonts + fixtures
    pick_py = os.path.join(SCRATCH, "pick.py")
    with open(pick_py, "w") as h:
        h.write(PICK)
    picks_json = os.path.join(SCRATCH, "picks.json")
    ran = subprocess.run(oracle_env.command("fonttools", ["python3", pick_py, picks_json] + fonts,
                                            scratch=SCRATCH), capture_output=True, text=True)
    if ran.returncode != 0:
        raise oracle_env.OracleUnavailable("pick failed: %s" % ran.stderr[-300:])
    picks = json.load(open(picks_json))
    pairs, declined, refused = [], 0, []
    for number, font in enumerate(fonts):
        cps = picks.get(font) or []
        if not cps:
            continue
        out = os.path.join(SCRATCH, "r%d.ttf" % number)
        run = subprocess.run([DRIVER, "--unicodes", ",".join("%X" % c for c in cps),
                              "--rename", NEW, font, out], capture_output=True, text=True)
        if run.returncode != 0:
            if "nsupported" in run.stderr:
                declined += 1
            else:
                refused.append((os.path.basename(font), run.stderr.strip()[:160]))
            continue
        pairs.append([font, out])
    pairs_json = os.path.join(SCRATCH, "pairs.json")
    json.dump(pairs, open(pairs_json, "w"))
    check_py = os.path.join(SCRATCH, "check.py")
    with open(check_py, "w") as h:
        h.write(CHECK)
    results_json = os.path.join(SCRATCH, "results.json")
    ran = subprocess.run(oracle_env.command("fonttools", ["python3", check_py, pairs_json,
                                                          results_json, NEW], scratch=SCRATCH),
                         capture_output=True, text=True)
    if ran.returncode != 0:
        raise oracle_env.OracleUnavailable("check failed: %s" % ran.stderr[-300:])
    results = json.load(open(results_json))
    bad = [(os.path.basename(p[0]), r) for p, r in zip(pairs, results) if r]
    print("subset_rename_diff: %d fonts renamed and read back, %d declined (unsupported), "
          "%d refused otherwise, %d with a problem" % (len(pairs), declined, len(refused), len(bad)))
    if not quiet:
        for item in bad[:20]:
            print("  problem", *item)
        for item in refused[:10]:
            print("  refused", *item)
    return 1 if (bad or refused) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
