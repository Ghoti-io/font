#!/usr/bin/env python3
"""Ensure annotation metrics affect vertical line boxes."""
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
APP = ROOT / "build" / BUILD / "apps" / "testLayout_vertical"
UNIT_APP = ROOT / "build" / BUILD / "apps" / "testLayout"
LD_PATH = f"{PREFIX}/lib/ghoti.io:{PREFIX}/lib"
ARTIFACTS = (APP, UNIT_APP, APP.parent / "libghoti.io-font-1.a",
             ROOT / "build" / BUILD / "objects/layout/layout.o")


def run(command: list[str], *, expect_failure: bool = False) -> None:
    result = subprocess.run(command, cwd=ROOT, env={**os.environ, "LD_LIBRARY_PATH": LD_PATH})
    if (result.returncode != 0) != expect_failure:
        want = "failure" if expect_failure else "success"
        raise RuntimeError(f"expected {want}: {' '.join(command)} (exit {result.returncode})")


def main() -> int:
    original = SOURCE.read_text()
    needle = "height = (int64_t)annotation->ascent + annotation->descent;"
    fragment_needle = "const size_t first = (cp_start - source->cp_start) * annotation_cps / base_count;"
    if original.count(needle) != 1 or original.count(fragment_needle) != 1:
        print("layout_l4_mutation_check: expected one annotation metric and fragment site", file=sys.stderr)
        return 1
    try:
        SOURCE.write_text(original.replace(needle, "height = 0;"))
        for artifact in ARTIFACTS:
            artifact.unlink(missing_ok=True)
        run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(APP.relative_to(ROOT))])
        run([str(APP), "--gtest_filter=LayoutVertical.RubyAndEmphasisTracksUseTheColumnCrossAxis",
             "--gtest_brief=1"], expect_failure=True)
        SOURCE.write_text(original.replace(fragment_needle,
            "const size_t first = (cp_start - source->cp_start + 1) * annotation_cps / base_count;"))
        for artifact in ARTIFACTS:
            artifact.unlink(missing_ok=True)
        run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(UNIT_APP.relative_to(ROOT))])
        run([str(UNIT_APP), "--gtest_filter=LayoutAnnotation.RubyGroupBreakingIsControlledByTheDescriptor",
             "--gtest_brief=1"], expect_failure=True)
    except (OSError, RuntimeError) as error:
        print(f"layout_l4_mutation_check: {error}", file=sys.stderr)
        return 1
    finally:
        SOURCE.write_text(original)
        for artifact in ARTIFACTS:
            artifact.unlink(missing_ok=True)
        try:
            run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}",
                 str(APP.relative_to(ROOT))])
            run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}",
                 str(UNIT_APP.relative_to(ROOT))])
        except (OSError, RuntimeError) as error:
            print(f"layout_l4_mutation_check: restore build failed: {error}", file=sys.stderr)
            return 1
    print("layout_l4_mutation_check: omitted annotation metrics and fragment partitioning rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
