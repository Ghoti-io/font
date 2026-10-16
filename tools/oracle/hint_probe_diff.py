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
"""Random hinting programs, this library against FreeType.

documentation/design.md section 8.5. hint_probe_font.py writes fonts whose glyphs
run small random programs of valid point instructions; this runs each at several
sizes in both interpreter versions through this library and through FreeType and
compares every point in 26.6, with no tolerance. With --steps 1 every program is
one instruction (after a reference-point setup), so a difference names the
instruction; with more, the interactions show.

Usage:
    hint_probe_diff.py [--seed N] [--fonts N] [--glyphs N] [--steps N]
                       [--version 35|40] [--ppem N,N] [--show N]
"""

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hint_diff
import oracle_env

ROOT = oracle_env.ROOT


def differs(sidecar, name, asm, version, ppem):
    """Whether this program on this glyph's points still disagrees with FreeType."""
    scratch = os.path.join(ROOT, "build", "oracle", "probe")
    spec = {"upem": sidecar["upem"], "cvt": sidecar["cvt"],
            "cases": [{"points": sidecar["points"][name],
                       "ends": [5, 11], "asm": asm}]}
    with open(os.path.join(scratch, "shrink.json"), "w") as handle:
        json.dump(spec, handle)
    made = subprocess.run(oracle_env.command("fonttools", [
        "python3", os.path.join(ROOT, "tools", "oracle", "hint_case_font.py"),
        os.path.join(scratch, "shrink.ttf"), os.path.join(scratch, "shrink.json")],
        scratch=scratch), capture_output=True, text=True)
    if made.returncode != 0:
        return False
    font = os.path.join(scratch, "shrink.ttf")
    mine, _ = hint_diff.ours(font, version, ppem, 1)
    yours, _ = hint_diff.theirs(font, version, ppem, 1)
    a, b = (mine or {}).get(1), (yours or {}).get(1)
    return (a is not None and b is not None and not isinstance(a, str)
            and not isinstance(b, str) and a[1] != b[1])


def shrink(sidecar, name, version, ppem):
    """Drop (push, instruction) groups while the disagreement remains."""
    asm = list(sidecar["programs"][name])
    changed = True
    while changed:
        changed = False
        for i in range(len(asm)):
            if not asm[i].startswith("PUSH"):
                continue
            j = i + 1
            while j < len(asm) and asm[j].startswith("PUSH"):
                j += 1
            trial = asm[:i] + asm[j + 1:]
            if j < len(asm) and trial and differs(sidecar, name, trial, version, ppem):
                asm = trial
                changed = True
                break
    return asm


def main(argv):
    opts = {"--seed": "1", "--fonts": "2", "--glyphs": "60", "--steps": "1",
            "--version": "35", "--ppem": "12,16", "--show": "6", "--vectors": "any", "--shrink": "0"}
    args = argv[1:]
    while args:
        key = args.pop(0)
        if key not in opts:
            print("unknown option %s" % key)
            return 2
        opts[key] = args.pop(0)
    scratch = os.path.join(ROOT, "build", "oracle", "probe")
    os.makedirs(scratch, exist_ok=True)
    gen = subprocess.run(oracle_env.command("fonttools", [
        "python3", os.path.join(ROOT, "tools", "oracle", "hint_probe_font.py"),
        scratch, opts["--seed"], opts["--fonts"], opts["--glyphs"], opts["--steps"]]
        + (["axis"] if opts["--vectors"] == "axis" else []),
        scratch=scratch), capture_output=True, text=True)
    if gen.returncode != 0:
        print(gen.stderr[-500:])
        return 2
    version = int(opts["--version"])
    shown = 0
    same = different = declined = 0
    by_instruction = {}
    notes = {}
    shortest = []
    for n in range(int(opts["--fonts"])):
        path = os.path.join(scratch, "probe-%s-%d.ttf" % (opts["--seed"], n))
        with open(path + ".json") as handle:
            sidecar = json.load(handle)
        listing = sidecar["programs"]
        names = list(listing)
        for ppem in [int(v) for v in opts["--ppem"].split(",")]:
            mine, _ = hint_diff.ours(path, version, ppem, 1)
            yours, _ = hint_diff.theirs(path, version, ppem, 1)
            if mine is None or yours is None:
                print("a driver failed on %s" % path)
                return 2
            for gid, answer in sorted(mine.items()):
                other = yours.get(gid)
                if isinstance(answer, str) or other is None or isinstance(other, str):
                    declined += 1
                    note = "ours: %s" % answer if isinstance(answer, str) else "ours hinted"
                    note += "; FreeType: %s" % (other if isinstance(other, str) else "hinted" if other else "absent")
                    notes[note] = notes.get(note, 0) + 1
                    continue
                if answer[1] == other[1]:
                    same += 1
                    continue
                different += 1
                asm = [l for l in listing.get(names[gid], []) if not l.startswith("PUSH")] if gid < len(names) else []
                full = listing.get(names[gid], []) if gid < len(names) else []
                key = " ".join(l.split()[0] for l in full if not l.startswith("PUSH") and not l.startswith("SRP") and not l.startswith("SVTCA") and not l.startswith("IUP"))
                by_instruction[key] = by_instruction.get(key, 0) + 1
                shortest.append((len(full), path, ppem, gid, answer, other, full))
                if int(opts["--shrink"]) and len(shortest) <= int(opts["--shrink"]):
                    shrunk = shrink(sidecar, names[gid], version, ppem)
                    print("shrunk %s ppem %d glyph %d: %s" % (os.path.basename(path), ppem, gid, " ; ".join(shrunk)))
                if False:
                    shown += 1
                    where = next((i for i, (a, b) in enumerate(zip(answer[1], other[1])) if a != b), None)
                    print("%s ppem %d glyph %d point %s: ours %s theirs %s" % (
                        os.path.basename(path), ppem, gid, where,
                        answer[1][where][:2] if where is not None else "-",
                        other[1][where][:2] if where is not None else "-"))
                    print("    " + " ; ".join(full))
    for size, path, ppem, gid, answer, other, full in sorted(shortest, key=lambda e: e[0])[:int(opts["--show"])]:
        where = next((i for i, (a, b) in enumerate(zip(answer[1], other[1])) if a != b), None)
        print("%s ppem %d glyph %d point %s: ours %s theirs %s" % (
            os.path.basename(path), ppem, gid, where,
            answer[1][where][:2] if where is not None else "-",
            other[1][where][:2] if where is not None else "-"))
        print("    " + " ; ".join(full))
    print("%d identical, %d different, %d not compared" % (same, different, declined))
    for note, count in sorted(notes.items(), key=lambda kv: -kv[1])[:6]:
        print("  not compared %4d x %s" % (count, note))
    for key, count in sorted(by_instruction.items(), key=lambda kv: -kv[1])[:15]:
        print("  %4d x %s" % (count, key))
    return 1 if different else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
