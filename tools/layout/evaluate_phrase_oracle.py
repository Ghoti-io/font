#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Compare held-out Japanese auto-phrase line breaks with pinned Chromium."""
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / "tools/layout/phrase_corpus_ja.json"
ORACLE = ROOT / "tools/oracle/css_layout_diff.py"
WIDTHS = (60, 80, 100, 120, 140, 160, 180, 200, 240, 300)


def load_oracle():
    spec = importlib.util.spec_from_file_location("css_layout_diff", ORACLE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def own_line_ends(chars):
    result = []
    for i in range(len(chars) - 1):
        left, right = chars[i], chars[i + 1]
        if left is not None and right is not None:
            if abs(left[1] / 64.0 - right[1] / 64.0) > 1:
                result.append(i + 1)
    return result


def oracle_line_ends(answer):
    result = []
    chars = answer["chars"]
    for i in range(len(chars) - 1):
        left, right = chars[i], chars[i + 1]
        if left is not None and right is not None and abs(left[1] - right[1]) > 1:
            result.append(i + 1)
    return result


def main():
    corpus = json.loads(CORPUS.read_text(encoding="utf-8"))
    sentences = [row for row in corpus["sentences"] if row["split"] == "holdout"]
    cases = []
    for sentence in sentences:
        for width in WIDTHS:
            name = "%s-%d" % (sentence["id"], width)
            cases.append((name, sentence["text"], "horizontal", "mixed", width,
                          "start", "ltr", "auto",
                          {"wordBreak": "auto-phrase", "lang": "ja",
                           "driver": ("--word-break", "auto-phrase",
                                      "--content-language", "ja")}))

    oracle = load_oracle()
    expected = oracle.chrome_rects(cases)
    mismatches = 0
    compared = 0
    true_positive = false_positive = false_negative = 0
    samples = []
    for case, answer in zip(cases, expected):
        actual = oracle.ours(case)
        expected_ends = oracle_line_ends(answer)
        actual_ends = own_line_ends(actual["chars"])
        expected_set, actual_set = set(expected_ends), set(actual_ends)
        true_positive += len(expected_set & actual_set)
        false_positive += len(actual_set - expected_set)
        false_negative += len(expected_set - actual_set)
        compared += 1
        if actual_ends != expected_ends:
            mismatches += 1
            if len(samples) < 12:
                samples.append((case[0], expected_ends, actual_ends))
    print("Japanese auto-phrase held-out oracle: %d/%d line-break layouts match (%d differ)" %
          (compared - mismatches, compared, mismatches))
    for name, expected_ends, actual_ends in samples:
        print("%s differs: Chromium %s, this %s" %
              (name, expected_ends, actual_ends))
    precision = true_positive / (true_positive + false_positive) if true_positive + false_positive else 0
    recall = true_positive / (true_positive + false_negative) if true_positive + false_negative else 0
    f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0
    print("held-out break endpoint agreement: precision %.3f, recall %.3f, F1 %.3f" %
          (precision, recall, f1))
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())
