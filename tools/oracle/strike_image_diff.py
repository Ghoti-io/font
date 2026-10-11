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
"""Color bitmap strikes (`CBDT`, `sbix`), this library against fontTools and Pillow.

documentation/design.md sections 7.5 and 14. For every font with a `CBLC` or an
`sbix` - the committed fixtures and the image's own - `font-strike-images
--decode` writes every picture the strikes hold, bytes and decoded pixels, and
`fonttools_strike_images.py` holds the bytes, the boxes and the advances to
fontTools and the pixels to Pillow. The decoded comparison is the one between two
independent PNG decoders: `image`'s, as this library calls it, and Pillow's.

**The population is one real font and the fixtures.** Debian has one font with
`CBLC`, Noto Color Emoji, and none with `sbix`; Apple's are not redistributable. So
the `sbix` reader, and the PNG forms Noto does not use (grey, palette, RGB, 16-bit),
rest on fixtures that fontTools wrote, and the report says how many real fonts it
compared.

Usage:
    strike_image_diff.py [--quiet] [font...]
"""

import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-strike-images")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")
SCRATCH = os.path.join(ROOT, "build", "oracle", "strike-images")


def has_strike_tables(path):
    """Whether the sfnt directory names CBLC or sbix, read here, not by either side."""
    try:
        with open(path, "rb") as handle:
            head = handle.read(12)
            count = int.from_bytes(head[4:6], "big")
            tags = {handle.read(16)[:4] for _ in range(count)}
    except (OSError, ValueError):
        return False
    return b"CBLC" in tags or b"sbix" in tags


def main(argv):
    rest = [a for a in argv[1:] if not a.startswith("--")]
    if rest:
        fonts = [os.path.abspath(a) for a in rest]
        real = 0
    else:
        corpus_fonts = [f for f in corpus.fonts("sfnt") if has_strike_tables(f)]
        real = len(corpus_fonts)
        fixtures = [os.path.join(FIXTURES, n) for n in sorted(os.listdir(FIXTURES))
                    if n.endswith((".ttf", ".otf")) and has_strike_tables(
                        os.path.join(FIXTURES, n))]
        fonts = fixtures + corpus_fonts
    if os.path.isdir(SCRATCH):
        shutil.rmtree(SCRATCH)
    os.makedirs(SCRATCH)
    failed = compared = pixels = 0
    for number, font in enumerate(fonts):
        folder = os.path.join(SCRATCH, str(number))
        os.makedirs(folder)
        lines = os.path.join(folder, "ours.jsonl")
        with open(lines, "w", encoding="utf-8") as handle:
            run = subprocess.run([DRIVER, "--decode", folder, font], stdout=handle,
                                 stderr=subprocess.PIPE, text=True)
        if run.returncode != 0:
            print("DIFF %s: font-strike-images failed: %s"
                  % (os.path.relpath(font, ROOT), run.stderr.strip()[:200]))
            failed += 1
            continue
        out = os.path.join(folder, "fonttools.json")
        argv_ref = oracle_env.command("fonttools", [
            "python3", os.path.join(ROOT, "tools", "oracle",
                                    "fonttools_strike_images.py"),
            font, folder, lines, out], scratch=SCRATCH)
        finished = subprocess.run(argv_ref, capture_output=True, text=True)
        if finished.returncode != 0:
            # A failure of the reference script is a failure, not an absent
            # reference: reading it as "skipped" would let a broken comparison
            # pass where a comparison should be.
            print("DIFF %s: the comparison itself failed: %s"
                  % (os.path.relpath(font, ROOT), finished.stderr.strip()[-400:]))
            failed += 1
            continue
        with open(out, encoding="utf-8") as handle:
            answer = json.load(handle)
        compared += answer["compared"]
        pixels += answer["pixels"]
        if answer["differences"] or not answer["compared"]:
            failed += 1
            print("DIFF %s (%d compared)" % (os.path.relpath(font, ROOT),
                                             answer["compared"]))
            for text in answer["differences"]:
                print("     " + text)
    print("fonts %d (%d from the image's corpus), %d pictures compared byte for "
          "byte, %d decoded and compared to Pillow, %d fonts differing"
          % (len(fonts), real, compared, pixels, failed))
    return 1 if failed or not fonts else 0


if __name__ == "__main__":
    try:
        oracle_env.provenance(["fonttools"])
        sys.exit(main(sys.argv))
    except oracle_env.OracleUnavailable as why:
        sys.stderr.write("SKIPPED strike_image_diff: %s\n" % why)
        sys.exit(1 if os.environ.get("GHOTI_ORACLE_REQUIRED") == "1" else 0)
