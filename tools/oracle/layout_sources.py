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
"""Find the layout source file that holds a mutation site.

The layout is several files under src/layout/, and a mutation script names the line it
changes, not the file. `source_with()` returns the one file that holds the text exactly once
and refuses anything else, so a site that moved or was duplicated fails loudly rather than
mutating nothing.
"""
from pathlib import Path


def source_with(root: Path, *needles: str) -> Path:
    """The unique file under src/layout/ holding every needle exactly once."""
    hits = []
    for path in sorted((root / "src/layout").glob("*.c")):
        text = path.read_text()
        if all(text.count(needle) == 1 for needle in needles):
            hits.append(path)
        elif any(text.count(needle) > 1 for needle in needles):
            raise RuntimeError(f"{path.name} holds a mutation site more than once")
    if len(hits) != 1:
        raise RuntimeError(
            f"expected one layout file holding the mutation site, found {len(hits)}: {needles[0]!r}")
    return hits[0]


def object_of(root: Path, build: str, source: Path) -> Path:
    """The object file the Makefile builds from a layout source."""
    return root / "build" / build / "objects/layout" / (source.stem + ".o")
