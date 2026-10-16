#!/usr/bin/env python3
"""Ensure annotation metrics affect vertical line boxes."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys


sys.path.insert(0, str(Path(__file__).resolve().parent))
from layout_sources import object_of, source_with

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/layout/layout.c"   # replaced by the file holding each site
PREFIX = os.environ.get("PREFIX", "/home/corey/Documents/ghoti.io/.local")
BUILD = os.environ.get("BUILD", "linux/release")
MAKE_BUILD = BUILD.removeprefix("linux/")
APP = ROOT / "build" / BUILD / "apps" / "testLayout_vertical"
UNIT_APP = ROOT / "build" / BUILD / "apps" / "testLayout"
LD_PATH = f"{PREFIX}/lib/ghoti.io:{PREFIX}/lib"
ARTIFACTS = ()   # set in main: the apps, the library and the mutated files' objects


def run(command: list[str], *, expect_failure: bool = False) -> None:
    result = subprocess.run(command, cwd=ROOT, env={**os.environ, "LD_LIBRARY_PATH": LD_PATH})
    if (result.returncode != 0) != expect_failure:
        want = "failure" if expect_failure else "success"
        raise RuntimeError(f"expected {want}: {' '.join(command)} (exit {result.returncode})")


def rebuild(*apps: Path) -> None:
    for app in apps:
        run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(app.relative_to(ROOT))])


def rejects_mutant(needle: str, replacement: str, app: Path, test: str) -> None:
    """Change the one line holding needle, expect the test to fail, put the file back."""
    global SOURCE, ARTIFACTS
    SOURCE = source_with(ROOT, needle)
    original = SOURCE.read_text()
    ARTIFACTS = (APP, UNIT_APP, APP.parent / "libghoti.io-font-7.a", object_of(ROOT, BUILD, SOURCE))
    try:
        SOURCE.write_text(original.replace(needle, replacement))
        for artifact in ARTIFACTS:
            artifact.unlink(missing_ok=True)
        rebuild(app)
        run([str(app), f"--gtest_filter={test}", "--gtest_brief=1"], expect_failure=True)
    finally:
        SOURCE.write_text(original)
        for artifact in ARTIFACTS:
            artifact.unlink(missing_ok=True)
        rebuild(APP, UNIT_APP)


def main() -> int:
    try:
        rejects_mutant(
            "height = (int64_t)annotation->ascent + annotation->descent;", "height = 0;", APP,
            "LayoutVertical.RubyAndEmphasisTracksUseTheColumnCrossAxis")
        rejects_mutant(
            "const size_t first = (cp_start - source->cp_start) * annotation_cps / base_count;",
            "const size_t first = (cp_start - source->cp_start + 1) * annotation_cps / base_count;",
            UNIT_APP, "LayoutAnnotation.RubyGroupBreakingIsControlledByTheDescriptor")
    except (OSError, RuntimeError) as error:
        print(f"layout_l4_mutation_check: {error}", file=sys.stderr)
        return 1
    print("layout_l4_mutation_check: omitted annotation metrics and fragment partitioning rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
