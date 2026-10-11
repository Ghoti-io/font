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
"""The `SVG ` documents of fonts as fontTools reads them: usage `fonttools_svg.py out.json font...`.

For each font: the documents in the table's order (first glyph, last glyph, whether it was
compressed, the text's length and SHA-256) and, for every glyph, the index of the first document
whose range holds it. Runs inside the fontTools image.
"""
import hashlib
import json
import sys

from fontTools.ttLib import TTFont


def main(argv):
    out, fonts = argv[1], argv[2:]
    answer = {}
    for path in fonts:
        font = TTFont(path, lazy=True)
        n = font["maxp"].numGlyphs
        docs = []
        for doc in font["SVG "].docList:
            data = doc.data if isinstance(doc.data, bytes) else doc.data.encode("utf-8")
            docs.append({"first": doc.startGlyphID, "last": doc.endGlyphID,
                         "compressed": bool(doc.compressed), "text": len(data),
                         "sha256": hashlib.sha256(data).hexdigest()})
        lookup = []
        for g in range(n):
            lookup.append(next((i for i, d in enumerate(docs)
                                if d["first"] <= g <= d["last"]), -1))
        answer[path] = {"documents": docs, "glyph_document": lookup}
    with open(out, "w", encoding="utf-8") as handle:
        json.dump(answer, handle)


if __name__ == "__main__":
    main(sys.argv)
