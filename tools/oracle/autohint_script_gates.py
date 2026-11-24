#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""The scripts whose zones are in src/hint/autofit_scripts.h, each compared with FreeType on one Noto face.

    autohint_script_gates.py

Each row names a script's code point range, its face, and the share of the face's points the automatic hinter must put where
FreeType does (measured when the zones were generated, less a margin). The exact-glyph share of these is low - the interior of
a complex letter is the part of the placement rules this library does not reproduce yet - so the gate is on points.
Run it where `autohint_diff.py` runs, with the fonttools and freetype oracles.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

ROWS = [
    ("13A0-13FF", "NotoSansCherokee-Regular.ttf", 0.28),
    ("2C80-2CFF", "NotoSansCoptic-Regular.ttf", 0.22),
    ("1200-137F", "NotoSansEthiopic-Regular.ttf", 0.65),
    ("1400-167F", "NotoSansCanadianAboriginal-Regular.ttf", 0.22),
    ("2C00-2C5F", "NotoSansGlagolitic-Regular.ttf", 0.38),
    ("10400-1044F", "NotoSansDeseret-Regular.ttf", 0.30),
    ("104B0-104FF", "NotoSansOsage-Regular.ttf", 0.22),
    ("10330-1034F", "NotoSansGothic-Regular.ttf", 0.55),
    ("A4D0-A4FF", "NotoSansLisu-Regular.ttf", 0.90),
    ("1E900-1E95F", "NotoSansAdlam-Regular.ttf", 0.25),
    ("0700-074F", "NotoSansSyriac-Regular.ttf", 0.12),
    ("07C0-07FF", "NotoSansNKo-Regular.ttf", 0.15),
    ("0B80-0BFF", "NotoSansTamil-Regular.ttf", 0.60),
    ("0D80-0DFF", "NotoSansSinhala-Regular.ttf", 0.65),
    ("1780-17FF", "NotoSansKhmer-Regular.ttf", 0.40),
    ("0E80-0EFF", "NotoSansLao-Regular.ttf", 0.17),
    ("0E00-0E7F", "NotoLoopedThai-Regular.ttf", 0.25),
    ("1000-109F", "NotoSansMyanmar-Regular.ttf", 0.25),
    ("0900-097F", "NotoSansDevanagari-Regular.ttf", 0.10),
    ("0980-09FF", "NotoSansBengali-Regular.ttf", 0.10),
    ("0A80-0AFF", "NotoSansGujarati-Regular.ttf", 0.25),
    ("0A00-0A7F", "NotoSansGurmukhi-Regular.ttf", 0.15),
    ("0C80-0CFF", "NotoSansKannada-Regular.ttf", 0.60),
    ("0D00-0D7F", "NotoSansMalayalam-Regular.ttf", 0.72),
    ("0C00-0C7F", "NotoSansTelugu-Regular.ttf", 0.90),
    ("2D30-2D7F", "NotoSansTifinagh-Regular.ttf", 0.85),
    ("A500-A63F", "NotoSansVai-Regular.ttf", 0.80),
    ("A6A0-A6FF", "NotoSansBamum-Regular.ttf", 0.90),
    ("1C50-1C7F", "NotoSansOlChiki-Regular.ttf", 0.82),
    ("1B80-1BBF", "NotoSansSundanese-Regular.ttf", 0.85),
    ("A880-A8DF", "NotoSansSaurashtra-Regular.ttf", 0.62),
    ("10480-104AF", "NotoSansOsmanya-Regular.ttf", 0.78),
    ("AA80-AADF", "NotoSansTaiViet-Regular.ttf", 0.77),
]


def main():
    failed = 0
    for script_range, font, floor in ROWS:
        done = subprocess.run(
            [sys.executable, os.path.join(HERE, "autohint_diff.py"), "--script", script_range, "--ppem", "12,17,24,32",
             "--points-floor", str(floor), "--show", "0", font],
            capture_output=True, text=True, stdin=subprocess.DEVNULL)
        summary = [line for line in done.stdout.splitlines() if line.startswith("compared")]
        print("%s %s (points floor %.2f): %s" % (script_range, font, floor, summary[0] if summary else done.stderr[-200:]))
        failed += done.returncode != 0
    if failed:
        print("autohint_script_gates: %d script(s) under their floor" % failed, file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
