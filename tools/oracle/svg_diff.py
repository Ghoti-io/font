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
"""Hold the `SVG ` table reader to fontTools over every font that has one.

For each font in the fixtures and the image's corpus whose directory names `SVG `: the index
(first glyph, last glyph, compressed or not), each document's inflated text (compared by its
SHA-256 with fontTools' own decoding), and which document every glyph of the font finds.

Usage:
    svg_diff.py [--quiet] [font...]
"""

import hashlib
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-svg")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")
SCRATCH = os.path.join(ROOT, "build", "oracle", "svg")


def has_svg(path):
    try:
        with open(path, "rb") as handle:
            head = handle.read(12)
            count = int.from_bytes(head[4:6], "big")
            tags = {handle.read(16)[:4] for _ in range(count)}
    except (OSError, ValueError):
        return False
    return b"SVG " in tags


def main(argv):
    rest = [a for a in argv[1:] if not a.startswith("--")]
    if rest:
        fonts, real = [os.path.abspath(a) for a in rest], 0
    else:
        corpus_fonts = [f for f in corpus.fonts("sfnt") if has_svg(f)]
        real = len(corpus_fonts)
        fixtures = [os.path.join(FIXTURES, n) for n in sorted(os.listdir(FIXTURES))
                    if n.endswith((".ttf", ".otf")) and has_svg(os.path.join(FIXTURES, n))]
        fonts = fixtures + corpus_fonts
    os.makedirs(SCRATCH, exist_ok=True)
    ours = {}
    docs_made = 0
    for number, font in enumerate(fonts):
        folder = os.path.join(SCRATCH, str(number))
        os.makedirs(folder, exist_ok=True)
        run = subprocess.run([DRIVER, folder, font], capture_output=True, text=True)
        try:
            got = json.loads(run.stdout)
        except ValueError:
            got = {"error": run.stderr.strip()[:200] or "no answer"}
        if "documents" in got:
            for i, doc in enumerate(got["documents"]):
                with open(os.path.join(folder, "%d.svg" % i), "rb") as handle:
                    data = handle.read()
                doc["text_ok"] = len(data) == doc["text"]
                doc["sha256"] = hashlib.sha256(data).hexdigest()
                del doc["length"]
        ours[font] = got
    out = os.path.join(SCRATCH, "fonttools.json")
    argv_ref = oracle_env.command("fonttools", [
        "python3", os.path.join(ROOT, "tools", "oracle", "fonttools_svg.py"), out] + list(ours),
        scratch=SCRATCH)
    finished = subprocess.run(argv_ref, capture_output=True, text=True)
    if finished.returncode != 0:
        raise oracle_env.OracleUnavailable("fonttools_svg failed: %s" % finished.stderr[-300:])
    with open(out, encoding="utf-8") as handle:
        want = json.load(handle)
    failed = 0
    glyphs = 0
    for font, got in ours.items():
        ref = want[font]
        for d in got.get("documents", []):
            d.pop("text_ok", None)
        docs_made += len(ref["documents"])
        glyphs += len(ref["glyph_document"])
        if got != ref:
            failed += 1
            print("DIFF %s" % os.path.relpath(font, ROOT))
            print("     this library %s" % json.dumps(got)[:300])
            print("     fontTools    %s" % json.dumps(ref)[:300])
    print("fonts %d (%d from the image's corpus), %d documents, %d glyph lookups, %d differing"
          % (len(fonts), real, docs_made, glyphs, failed))
    return 1 if failed or not fonts else 0


if __name__ == "__main__":
    try:
        oracle_env.provenance(["fonttools"])
        sys.exit(main(sys.argv))
    except oracle_env.OracleUnavailable as why:
        sys.stderr.write("SKIPPED svg_diff: %s\n" % why)
        sys.exit(1 if os.environ.get("GHOTI_ORACLE_REQUIRED") == "1" else 0)
