#!/usr/bin/env python3
"""Ensure focused flow tests reject lost offsets, height retries, and clearing."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/layout/layout.c"
PREFIX = os.environ.get("PREFIX", "/home/corey/Documents/ghoti.io/.local")
BUILD = os.environ.get("BUILD", "linux/release")
MAKE_BUILD = BUILD.removeprefix("linux/")
APP = ROOT / "build" / BUILD / "apps" / "testLayout"
LD_PATH = f"{PREFIX}/lib/ghoti.io:{PREFIX}/lib"
ARTIFACTS = (APP, APP.parent / "libghoti.io-font-7.a",
             ROOT / "build" / BUILD / "objects/layout/layout.o")


def run(command: list[str], *, expect_failure: bool = False) -> None:
    result = subprocess.run(command, cwd=ROOT, env={**os.environ, "LD_LIBRARY_PATH": LD_PATH})
    if (result.returncode != 0) != expect_failure:
        want = "failure" if expect_failure else "success"
        raise RuntimeError(f"expected {want}: {' '.join(command)} (exit {result.returncode})")


def rejects_mutant(name: str, needle: str, replacement: str, test: str) -> None:
    original = SOURCE.read_text()
    if original.count(needle) != 1:
        raise RuntimeError(f"{name}: expected one mutation site, found {original.count(needle)}")
    try:
        SOURCE.write_text(original.replace(needle, replacement))
        for artifact in ARTIFACTS:
            artifact.unlink(missing_ok=True)
        run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(APP.relative_to(ROOT))])
        run([str(APP), f"--gtest_filter={test}", "--gtest_brief=1"], expect_failure=True)
    finally:
        SOURCE.write_text(original)
        for artifact in ARTIFACTS:
            artifact.unlink(missing_ok=True)
        run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(APP.relative_to(ROOT))])


def main() -> int:
    mutants = (
        ("band offset", "align_line(p, line, band_starts[li], available",
         "(void)band_starts; align_line(p, line, 0, available",
         "LayoutFlow.BandOffsetMovesLineBoxAndGlyphs"),
        ("measured-height retry", "constraint->band(constraint->ctx, top, height, &actual)",
         "constraint->band(constraint->ctx, top, 0, &actual)",
         "LayoutFlow.RebreaksWhenMeasuredHeightChangesTheBand"),
        ("clearing", "if (measured.natural > band.width && band.next_top > top)",
         "if (false && measured.natural > band.width && band.next_top > top)",
         "LayoutFlow.ClearsAnUnbreakableStretchToTheNextBand"),
    )
    try:
        for mutant in mutants:
            rejects_mutant(*mutant)
    except (OSError, RuntimeError) as error:
        print(f"layout_l3_mutation_check: {error}", file=sys.stderr)
        return 1
    print("layout_l3_mutation_check: offset, retry, and clearing mutants rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
