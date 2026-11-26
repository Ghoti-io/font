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
    ("13A0-13FF", "NotoSansCherokee-Regular.ttf", 0.357),
    ("2C80-2CFF", "NotoSansCoptic-Regular.ttf", 0.277),
    ("1200-137F", "NotoSansEthiopic-Regular.ttf", 0.728),
    ("1400-167F", "NotoSansCanadianAboriginal-Regular.ttf", 0.261),
    ("2C00-2C5F", "NotoSansGlagolitic-Regular.ttf", 0.450),
    ("10400-1044F", "NotoSansDeseret-Regular.ttf", 0.376),
    ("104B0-104FF", "NotoSansOsage-Regular.ttf", 0.274),
    ("10330-1034F", "NotoSansGothic-Regular.ttf", 0.640),
    ("A4D0-A4FF", "NotoSansLisu-Regular.ttf", 0.973),
    ("1E900-1E95F", "NotoSansAdlam-Regular.ttf", 0.302),
    ("0700-074F", "NotoSansSyriac-Regular.ttf", 0.140),
    ("07C0-07FF", "NotoSansNKo-Regular.ttf", 0.164),
    ("0B80-0BFF", "NotoSansTamil-Regular.ttf", 0.652),
    ("0D80-0DFF", "NotoSansSinhala-Regular.ttf", 0.712),
    ("1780-17FF", "NotoSansKhmer-Regular.ttf", 0.445),
    ("0E80-0EFF", "NotoSansLao-Regular.ttf", 0.204),
    ("0E00-0E7F", "NotoLoopedThai-Regular.ttf", 0.321),
    ("1000-109F", "NotoSansMyanmar-Regular.ttf", 0.279),
    ("0900-097F", "NotoSansDevanagari-Regular.ttf", 0.125),
    ("0980-09FF", "NotoSansBengali-Regular.ttf", 0.129),
    ("0A80-0AFF", "NotoSansGujarati-Regular.ttf", 0.285),
    ("0A00-0A7F", "NotoSansGurmukhi-Regular.ttf", 0.182),
    ("0C80-0CFF", "NotoSansKannada-Regular.ttf", 0.687),
    ("0D00-0D7F", "NotoSansMalayalam-Regular.ttf", 0.801),
    ("0C00-0C7F", "NotoSansTelugu-Regular.ttf", 0.956),
    ("2D30-2D7F", "NotoSansTifinagh-Regular.ttf", 0.943),
    ("A500-A63F", "NotoSansVai-Regular.ttf", 0.943),
    ("A6A0-A6FF", "NotoSansBamum-Regular.ttf", 0.991),
    ("1C50-1C7F", "NotoSansOlChiki-Regular.ttf", 0.917),
    ("1B80-1BBF", "NotoSansSundanese-Regular.ttf", 0.921),
    ("A880-A8DF", "NotoSansSaurashtra-Regular.ttf", 0.722),
    ("10480-104AF", "NotoSansOsmanya-Regular.ttf", 0.895),
    ("AA80-AADF", "NotoSansTaiViet-Regular.ttf", 0.872),
    ("10B00-10B3F", "NotoSansAvestan-Regular.ttf", 0.672),
    ("10800-1083F", "NotoSansCypriot-Regular.ttf", 0.896),
    ("102A0-102DF", "NotoSansCarian-Regular.ttf", 0.993),
    ("11100-1114F", "NotoSansChakma-Regular.ttf", 0.824),
    ("A900-A92F", "NotoSansKayahLi-Regular.ttf", 0.274),
    ("1740-175F", "NotoSansBuhid-Regular.ttf", 0.435),
    ("0840-085F", "NotoSansMandaic-Regular.ttf", 0.189),
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
