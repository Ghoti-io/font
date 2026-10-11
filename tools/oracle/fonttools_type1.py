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
"""fontTools' side of the Type 1 write differential, run inside the oracle image.

    fonttools_type1.py <scratch> <font>...

For each font, `<scratch>/<key>.pfa` and `<scratch>/<key>.pfb` are what `font-convert`
wrote. fontTools' `t1Lib` reads the original and each of them, and everything it
extracts must match: the font dictionary (name, matrix, bounding box, encoding), the
private dictionary, and every charstring, which `t1Lib` returns decrypted.
"""

import json
import os
import sys

from fontTools.t1Lib import T1Font


def key(path):
    return path.replace("/", "_")


def view(path):
    font = T1Font(path)
    font.parse()
    data = font.font
    private = {k: v for k, v in data["Private"].items() if k != "Subrs"}
    return {
        "FontName": data.get("FontName"),
        "FontMatrix": list(data.get("FontMatrix", [])),
        "FontBBox": list(data.get("FontBBox", [])),
        "Encoding": list(data["Encoding"]) if not isinstance(data["Encoding"], str)
        else data["Encoding"],
        # The decrypted bytecode: the object's own repr carries its address.
        "CharStrings": {k: bytes(v.bytecode).hex()
                        for k, v in data["CharStrings"].items()},
        "Subrs": [bytes(s.bytecode).hex() if hasattr(s, "bytecode") else str(s)
                  for s in data["Private"].get("Subrs", [])],
        "Private": {k: str(v) for k, v in private.items()},
        "FontInfo": {k: str(v) for k, v in data.get("FontInfo", {}).items()},
    }


def main(scratch, fonts):
    problems = []
    checked = 0
    for path in fonts:
        try:
            want = view(path)
        except Exception as why:
            problems.append("%s: fontTools cannot read the original: %s" % (path, why))
            continue
        for extension in ("pfa", "pfb"):
            target = os.path.join(scratch, key(path) + "." + extension)
            if not os.path.exists(target):
                problems.append("%s: no %s was written" % (path, extension))
                continue
            try:
                got = view(target)
            except Exception as why:
                problems.append("%s -> %s: fontTools cannot read it: %s"
                                % (os.path.basename(path), extension, why))
                continue
            checked += 1
            for field in want:
                if want[field] != got[field]:
                    problems.append("%s -> %s: %s differs" % (
                        os.path.basename(path), extension, field))
    print(json.dumps({"checked": checked, "problems": problems}))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
