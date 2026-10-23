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
"""The real COLR version 1 fonts a lowering is measured on, fetched by digest.

documentation/design.md section 14.5 keeps every third-party font out of this
repository. The oracle image already holds the COLR version 1 fonts of
googlefonts/color-fonts (`containers/fonttools/fonttools-fetch-color-fonts`: Google's
test-glyph fonts, three producers' samples, Twemoji and a Noto-derived build). The two
here are the fonts that the shape family's success signal names and the image does not
hold, staged on the host under `build/oracle/colr-real/` (a cache, removed with `build/`):

  * **Noto Color Emoji, COLR version 1** - Google's own build, the font the browsers ship.
    googlefonts/noto-emoji at the commit below, `2D/fonts/Noto-COLRv1.ttf`. SIL Open Font
    License 1.1 (the font's `name` table, ID 13). 42,330 glyphs, 4,012 color glyphs: linear and
    radial gradients, solids, composites, transforms, a clip box for every one. No variations.
  * **Nabla** - a variable COLR version 1 font by the Nabla Project Authors.
    google/fonts at the commit below, `ofl/nabla/Nabla[EDPT,EHLT].ttf`. SIL Open Font License
    1.1 (the repository's `OFL.txt`). 3,655 glyphs, 381 color glyphs, two axes (EDPT, EHLT).
    Its outlines vary, its paints do not: it holds no `PaintVar*` (the `COLR` table has no
    item variation store), so the per-field coverage of the variable paints comes from Google's
    variable test-glyph font, which the image holds, one glyph per variable field. Nabla is
    here for what it is: a real font whose gradients sit on outlines that move with the instance.

Each file is pinned by the commit that names its tree and by its SHA-256, so a moved branch or
a rewritten file fails here rather than changing the corpus. Nothing is committed to the library.

Usage: colr_real_fonts.py            fetch (if absent) and verify; print the paths
"""

import hashlib
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DEST = os.path.join(ROOT, "build", "oracle", "colr-real")

PINS = [
    ("Noto-COLRv1.ttf",
     "https://raw.githubusercontent.com/googlefonts/noto-emoji/1ffdd21391dd1f25c081fa93a9dea0c7c029442b/2D/fonts/Noto-COLRv1.ttf",
     "b8e25ea68db82f9e4d0aee921f4420be2be39887bd5c893a2ad98710531f9d0c", 5038236),
    ("Nabla-EDPT-EHLT.ttf",
     "https://raw.githubusercontent.com/google/fonts/34a2c3daeeb2715576af0786ae4e9dc6c7d8ca2a/ofl/nabla/Nabla%5BEDPT%2CEHLT%5D.ttf",
     "e45cec60eb2099b4b4ffee8ebe005d1d3060771071ede1caeeeb12e37a2c00ae", 1643616),
]


class FetchFailed(Exception):
    pass


def ensure():
    """The pinned fonts' paths, fetched if absent; raises FetchFailed when a font cannot be had or does not match."""
    os.makedirs(DEST, exist_ok=True)
    paths = []
    for name, url, digest, size in PINS:
        path = os.path.join(DEST, name)
        if os.path.exists(path):
            with open(path, "rb") as handle:
                data = handle.read()
        else:
            try:
                data = urllib.request.urlopen(url, timeout=300).read()
            except OSError as why:
                raise FetchFailed("%s: %s" % (name, why))
        if hashlib.sha256(data).hexdigest() != digest or len(data) != size:
            if os.path.exists(path):
                os.unlink(path)
            raise FetchFailed("%s does not match its pin" % name)
        if not os.path.exists(path):
            with open(path, "wb") as out:
                out.write(data)
        paths.append(path)
    return paths


if __name__ == "__main__":
    try:
        for p in ensure():
            print(p)
    except FetchFailed as why:
        sys.exit("colr_real_fonts: %s" % why)
