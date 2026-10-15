#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Collect line-break observations from the pinned Chromium oracle.

The input corpus is first-party text. Only rendered break positions are saved;
the browser's model, source, and weights are never read or copied.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / "tools/layout/phrase_corpus_ja.json"
OUTPUT = ROOT / "tools/layout/phrase_observations_ja.json"
ORACLE = ROOT / "tools/oracle/css_layout_diff.py"
WIDTHS = range(40, 601, 10)
BATCH_SENTENCES = 12


def load_oracle():
    spec = importlib.util.spec_from_file_location("css_layout_diff", ORACLE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def line_ends(answer):
    chars = answer["chars"]
    result = set()
    for i in range(len(chars) - 1):
        left, right = chars[i], chars[i + 1]
        if left is not None and right is not None and abs(left[1] - right[1]) > 1:
            result.add(i + 1)
    return result


def line_end_map(ends, cp_count):
    result = {}
    start = 0
    for end in sorted(set(ends) | {cp_count}):
        result[start] = end
        start = end
    return result


def collect(sentences, oracle, discover_suffixes=False):
    observations = []
    suffixes = {}
    for first in range(0, len(sentences), BATCH_SENTENCES):
        batch = sentences[first:first + BATCH_SENTENCES]
        cases = []
        for sentence in batch:
            for word_break in ("normal", "auto-phrase"):
                for width in WIDTHS:
                    cases.append((
                        "%s-%s-%d" % (sentence["id"], word_break, width),
                        sentence["text"], "horizontal", "mixed", width, "start", "ltr", "auto",
                        {"wordBreak": word_break, "lang": "ja"}))
        answers = oracle.chrome_rects(cases)
        width_count = len(WIDTHS)
        for offset, sentence in enumerate(batch):
            normal_rows = answers[offset * 2 * width_count:
                                  (offset * 2 + 1) * width_count]
            phrase_rows = answers[(offset * 2 + 1) * width_count:
                                  (offset * 2 + 2) * width_count]
            normal = set().union(*(line_ends(row) for row in normal_rows))
            phrase = set().union(*(line_ends(row) for row in phrase_rows))
            candidates = sorted(normal | phrase)
            # Compare the two wrapping modes at each identical width before
            # aggregating. Unioning first would erase a preferred break if it
            # also appeared as a forced narrow-width break under normal.
            preferred = set()
            line_choices = set()
            for width_index, (normal_row, phrase_row) in enumerate(zip(normal_rows, phrase_rows)):
                width_px = WIDTHS.start + width_index * WIDTHS.step
                normal_ends = line_ends(normal_row)
                phrase_ends = line_ends(phrase_row)
                preferred.update(phrase_ends - normal_ends)
                normal_lines = line_end_map(normal_ends, len(sentence["text"]))
                phrase_lines = line_end_map(phrase_ends, len(sentence["text"]))
                if discover_suffixes and sentence["split"] == "train":
                    for start in sorted(phrase_lines.keys() - normal_lines.keys()):
                        if start and start < len(sentence["text"]):
                            suffix = sentence["text"][start:]
                            suffixes.setdefault(suffix, {
                                "id": "%s-suffix-%03d" % (sentence["id"], start),
                                "split": "train", "text": suffix,
                                "source_id": sentence["id"], "source_offset": start
                            })
                for start in normal_lines.keys() & phrase_lines.keys():
                    normal_end = normal_lines[start]
                    phrase_end = phrase_lines[start]
                    if phrase_end < len(sentence["text"]):
                        line_choices.add((start, phrase_end, normal_end, width_px))
            observations.append({
                "id": sentence["id"],
                "split": sentence["split"],
                "text": sentence["text"],
                "candidate_boundaries": candidates,
                "preferred_boundaries": sorted(preferred),
                "line_choices": [list(choice) for choice in sorted(line_choices)],
                "width_range_px": [WIDTHS.start, WIDTHS.stop - WIDTHS.step, WIDTHS.step]
            })
        print("observed %d/%d sentences%s" % (
            min(first + BATCH_SENTENCES, len(sentences)), len(sentences),
            " and found %d shifted line starts" % len(suffixes) if discover_suffixes else ""),
            flush=True)
    return observations, list(suffixes.values())


def main():
    corpus_bytes = CORPUS.read_bytes()
    corpus = json.loads(corpus_bytes)
    oracle = load_oracle()
    observations, suffixes = collect(corpus["sentences"], oracle, discover_suffixes=True)
    if suffixes:
        print("probing %d first-party suffix contexts from shifted line starts" % len(suffixes),
              flush=True)
        suffix_observations, _ = collect(suffixes, oracle)
        source_by_id = {item["id"]: item for item in suffixes}
        for row in suffix_observations:
            source = source_by_id[row["id"]]
            row["source_text"] = corpus["sentences"][
                next(i for i, sentence in enumerate(corpus["sentences"])
                     if sentence["id"] == source["source_id"])
            ]["text"]
            row["source_offset"] = source["source_offset"]
            row["source_id"] = source["source_id"]
            row["line_choices"] = [
                [start + source["source_offset"], phrase_end + source["source_offset"],
                 normal_end + source["source_offset"], width_px]
                for start, phrase_end, normal_end, width_px in row["line_choices"]
            ]
        observations.extend(suffix_observations)

    result = {
        "format": 3,
        "corpus_sha256": hashlib.sha256(corpus_bytes).hexdigest(),
        "oracle": oracle.oracle_env.provenance(["chrome"]),
        "method": "Matched-width line choices label browser-selected endpoints against normal endpoints. Shifted-start suffix probes map choices back into their original first-party sentence.",
        "observations": observations
    }
    OUTPUT.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("wrote %s" % OUTPUT)


if __name__ == "__main__":
    main()
