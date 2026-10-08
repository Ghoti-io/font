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
"""Vertical text in the variable fixtures at several locations, here and in HarfBuzz.

The fixtures have no `vmtx`, so a location moves what vertical text stands on: the
origin is the top phantom point, the glyph's stated top plus what `gvar` adds. Both
directions, five locations, four strings per run, every glyph compared.

Usage: variable_vertical_diff.py [-v]
"""
import json
import os
import random
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import var_random_diff as V

ROOT = oracle_env.ROOT
SCRATCH = os.path.join(ROOT, "build", "oracle", "variable-vertical")
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-shape")
FONTS = ["with-vvar/wvariable-gvar.ttf", "with-vvar/wvariable-hvar.ttf", "with-vvar/wvariable-avar2.ttf",
         "with-vmtx/vvariable-gvar.ttf", "with-vmtx/vvariable-hvar.ttf", "with-vmtx/vvariable-avar2.ttf",
         "with-vmtx/vvariable-featurevars.ttf", "with-vmtx/vvariable-stat.ttf", "variable-gvar.ttf", "variable-hvar.ttf", "variable-avar2.ttf", "variable-cvar.ttf",
         "variable-featurevars.ttf", "variable-stat.ttf", "variable-cff2.otf"]
LOCATIONS = ["wght=400", "wght=900", "wght=100,wdth=125", "wdth=75", "wght=575"]


def sfnt_tables(path):
    with open(path, "rb") as h:
        d = h.read()
    n = struct.unpack(">H", d[4:6])[0]
    t = {}
    for i in range(n):
        tag, _, off, ln = struct.unpack(">4sIII", d[12 + 16 * i:28 + 16 * i])
        t[tag] = d[off:off + ln]
    return d[:4], t


def sfnt_build(ver, t):
    n = len(t)
    sr = 1 << (n.bit_length() - 1)
    out = ver + struct.pack(">HHHH", n, sr * 16, n.bit_length() - 1, n * 16 - sr * 16)
    off = 12 + 16 * n
    body = b""
    ents = b""
    for tag in sorted(t):
        data = t[tag]
        ents += struct.pack(">4sIII", tag, 0, off + len(body), len(data))
        body += data + b"\0" * ((-len(data)) % 4)
    return out + ents + body


def with_vmtx(src, dst, seed):
    """A copy of a glyf fixture with a random vhea and vmtx added."""
    ver, t = sfnt_tables(src)
    n = struct.unpack(">H", t[b"maxp"][4:6])[0]
    r = random.Random(seed)
    nv = max(1, n - 2)
    s16 = lambda *v: b"".join(struct.pack(">h", x) for x in v)
    u16 = lambda *v: b"".join(struct.pack(">H", x) for x in v)
    t[b"vhea"] = (struct.pack(">I", 0x10000) + s16(900, -300, 0) + u16(1500) + s16(0, 0, 0, 1, 0, 0)
                  + s16(0, 0, 0, 0, 0) + u16(nv))
    t[b"vmtx"] = (b"".join(u16(r.randint(500, 1500)) + s16(r.randint(-50, 200)) for _ in range(nv))
                  + b"".join(s16(r.randint(-50, 200)) for _ in range(n - nv)))
    with open(dst, "wb") as h:
        h.write(sfnt_build(ver, t))


def with_vvar(src, dst, seed):
    """A copy of a glyf fixture with a vhea, vmtx and a random VVAR (all four mappings)."""
    with_vmtx(src, dst, seed)
    ver, t = sfnt_tables(dst)
    n = struct.unpack(">H", t[b"maxp"][4:6])[0]
    axes = struct.unpack(">H", t[b"fvar"][8:10])[0]
    u16 = lambda *v: b"".join(struct.pack(">H", x) for x in v)
    u32 = lambda *v: b"".join(struct.pack(">I", x) for x in v)
    vg = V.VarGen(seed * 5 + 1, axes)
    store, counts = vg.store(n)
    inner_bits = max(1, (counts[0] - 1).bit_length())
    head = 24
    body = b""
    maps = []
    for mul in (1, 3, 5, 7):
        entries = bytes(min((g * mul) % n, counts[0] - 1) for g in range(n))
        maps.append(head + len(store) + len(body))
        body += u16(inner_bits - 1) + u16(n) + entries
    t[b"VVAR"] = u16(1, 0) + u32(head) + u32(*maps) + store + body
    with open(dst, "wb") as h:
        h.write(sfnt_build(ver, t))


def main(argv):
    os.makedirs(SCRATCH, exist_ok=True)
    txt = os.path.join(SCRATCH, "text.txt")
    with open(txt, "w") as h:
        h.write("ABA\nAB\nB\nBA\n")
    fixtures = os.path.join(ROOT, "tests", "data", "fonts")
    # Fixtures given a vmtx, in a directory of their own, so that the same names below
    # reach them: the glyf ones, whose origin is the top phantom point plus the top
    # bearing.
    fontpath = lambda f: os.path.join(SCRATCH if f.startswith("with-v") else fixtures, f)
    with_dir = os.path.join(SCRATCH, "with-vmtx")
    os.makedirs(with_dir, exist_ok=True)
    for k, f in enumerate(["variable-gvar.ttf", "variable-hvar.ttf", "variable-avar2.ttf",
                           "variable-featurevars.ttf", "variable-stat.ttf"]):
        with_vmtx(os.path.join(fixtures, f), os.path.join(with_dir, "v" + f), k)
    os.makedirs(os.path.join(SCRATCH, "with-vvar"), exist_ok=True)
    for k, f in enumerate(["variable-gvar.ttf", "variable-hvar.ttf", "variable-avar2.ttf"]):
        with_vvar(os.path.join(fixtures, f), os.path.join(SCRATCH, "with-vvar", "w" + f), 40 + k)
    lines = []
    for f in FONTS:
        for i, loc in enumerate(LOCATIONS):
            for d in ("ttb", "btt"):
                lines.append("hb-shape --font-file='%s' --output-format=json --no-glyph-names "
                             "--direction=%s --variations='%s' --text-file='%s' > '%s/%s_%d_%s.hb' 2>/dev/null"
                             % (fontpath(f), d, loc, txt, SCRATCH, f.replace("/", "_"), i, d))
    runner = os.path.join(SCRATCH, "run.sh")
    with open(runner, "w") as h:
        h.write("\n".join(lines) + "\n")
    ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", runner], scratch=SCRATCH),
                         capture_output=True, text=True)
    if ref.returncode not in (0, 1):
        sys.stderr.write("hb-shape failed: %s\n" % ref.stderr[-300:])
        return 2
    keys = ("g", "cl", "ax", "ay", "dx", "dy")
    norm = lambda g: [tuple(x.get(k) for k in keys) for x in g]
    bad = total = 0
    for f in FONTS:
        for i, loc in enumerate(LOCATIONS):
            for d in ("ttb", "btt"):
                with open("%s/%s_%d_%s.hb" % (SCRATCH, f.replace("/", "_"), i, d), encoding="utf-8") as h:
                    hb = [json.loads(x) if x.startswith("[") else [] for x in h.read().split("\n")[:-1]]
                with open(txt, "rb") as h:
                    out = subprocess.run([DRIVER, "--batch", "--script", "latn", "--" + d, "--location", loc,
                                          fontpath(f)], stdin=h, capture_output=True,
                                         text=True).stdout
                mine = [json.loads(x) for x in out.split("\n")[:-1]]
                for a, b in zip(hb, mine):
                    total += 1
                    if not isinstance(b, list) or norm(a) != norm(b):
                        bad += 1
                        if "-v" in argv:
                            print(f, loc, d, norm(a)[:3], b if not isinstance(b, list) else norm(b)[:3])
    print("variable_vertical_diff: %d strings, %d differ" % (total, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
