#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Train first-party boundary weights from recorded browser observations."""
import argparse
import bisect
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OBSERVATIONS = ROOT / "tools/layout/phrase_observations_ja.json"
OUTPUT = ROOT / "src/layout/phrase_model_ja.h"

# Character windows around a candidate break. The same small feature extractor
# is used by the C scorer; tables are language-specific learned data.
WINDOWS = [(-3, 1), (-2, 1), (-1, 1), (0, 1), (1, 1), (2, 1),
           (-2, 2), (-1, 2), (0, 2), (-3, 3), (-2, 3), (-1, 3), (0, 3)]
LOCAL_FEATURE_COUNT = len(WINDOWS)
CLASS_OFFSET = LOCAL_FEATURE_COUNT
START_EXACT_OFFSET = CLASS_OFFSET + LOCAL_FEATURE_COUNT
START_CLASS_OFFSET = START_EXACT_OFFSET + 3
LENGTH_FEATURE = START_CLASS_OFFSET + 3
FIT_FEATURE = LENGTH_FEATURE + 1
WIDTH_FEATURE = FIT_FEATURE + 1
FEATURE_TABLE_COUNT = WIDTH_FEATURE + 1
SCALE = 1024
THRESHOLD = 0.0
EPOCHS = 150
LEARNING_RATE = 0.15
L2 = 0.0001


def row_features(text, line_start, boundary, fit_end, width_px):
    points = [ord(char) for char in text]
    result = []
    for table, (offset, width) in enumerate(WINDOWS):
        start = max(0, min(len(points), boundary + offset))
        end = max(0, min(len(points), boundary + offset + width))
        if end < start:
            start, end = end, start
        result.append((table, tuple(points[start:end])))
        result.append((CLASS_OFFSET + table,
                       tuple(class_code(points[index]) for index in range(start, end))))
    for offset in range(3):
        at = min(len(points), line_start + offset)
        result.append((START_EXACT_OFFSET + offset,
                       tuple(points[at:at + 1])))
        result.append((START_CLASS_OFFSET + offset,
                       (class_code(points[at]),) if at < len(points) else ()))
    length_bucket = min(max(0, boundary - line_start - 1), 8)
    result.append((LENGTH_FEATURE, (0x110000 + length_bucket,)))
    fit_bucket = min(max(0, fit_end - line_start - 1), 32)
    result.append((FIT_FEATURE, (0x120000 + fit_bucket,)))
    result.append((WIDTH_FEATURE, (0x140000 + min(max(0, width_px // 10), 63),)))
    return result


def class_code(cp):
    if cp in (0x20, 0x09, 0x0A, 0x0D, 0x3000):
        category = 1
    elif (0x3400 <= cp <= 0x4DBF or 0x4E00 <= cp <= 0x9FFF
          or 0xF900 <= cp <= 0xFAFF or 0x20000 <= cp <= 0x323AF):
        category = 2
    elif 0x3040 <= cp <= 0x309F or 0x1B001 <= cp <= 0x1B11F:
        category = 3
    elif (0x30A0 <= cp <= 0x30FF or 0x31F0 <= cp <= 0x31FF
          or 0xFF66 <= cp <= 0xFF9F):
        category = 4
    elif 0x30 <= cp <= 0x39 or 0xFF10 <= cp <= 0xFF19:
        category = 5
    elif (0x41 <= cp <= 0x5A or 0x61 <= cp <= 0x7A
          or 0xFF21 <= cp <= 0xFF3A or 0xFF41 <= cp <= 0xFF5A):
        category = 6
    elif cp in (0x300C, 0x300E, 0xFF62, 0xFF08, 0xFF3B, 0x3010, 0x3008, 0x300A):
        category = 7
    elif cp in (0x300D, 0x300F, 0xFF63, 0xFF09, 0xFF3D, 0x3011, 0x3009, 0x300B):
        category = 8
    elif cp in (0x3001, 0xFF0C, 0x3002, 0xFF0E, 0x30FB, 0xFF1A, 0xFF1B):
        category = 9
    elif cp in (0x30FC, 0xFF70, 0x309D, 0x309E, 0x30FD, 0x30FE):
        category = 10
    else:
        category = 11
    return 0x130000 + category


def examples(observations, split):
    result = []

    def add(features, label, row_id, boundary, weight):
        if split == "train":
            key = (tuple(features), label)
            if key in result_by_key:
                result_by_key[key][4] += weight
            else:
                result_by_key[key] = [features, label, row_id, boundary, weight]
        else:
            result.append((features, label, row_id, boundary, weight))

    result_by_key = {}
    for row in observations:
        if row["split"] != split:
            continue
        text = row.get("source_text", row["text"])
        source_offset = row.get("source_offset", 0)
        candidates = {point + source_offset for point in row["candidate_boundaries"]}
        candidate_points = sorted(candidates)
        choices = row.get("line_choices", [])
        positives = {}
        for line_start, preferred, normal, width_px in choices:
            fit_end = max(preferred, normal)
            positives.setdefault((line_start, fit_end, width_px), set()).add(preferred)
        for line_start, preferred, normal, width_px in choices:
            fit_end = max(preferred, normal)
            add(row_features(text, line_start, preferred, fit_end, width_px), 1,
                row["id"], preferred, 1.0)
            alternatives = {normal}
            previous_index = bisect.bisect_left(candidate_points, preferred) - 1
            if previous_index >= 0 and candidate_points[previous_index] > line_start:
                alternatives.add(candidate_points[previous_index])
            for alternative in sorted(alternatives):
                if (line_start < alternative <= fit_end
                        and alternative not in positives[(line_start, fit_end, width_px)]):
                    add(row_features(text, line_start, alternative, fit_end, width_px), 0,
                        row["id"], alternative, 0.5)
    return list(result_by_key.values()) if split == "train" else result


def train(rows):
    weights = {}
    bias = 0.0
    aggregated = {}
    for features, label, sentence, boundary, sample_weight in rows:
        key = (tuple(features), label)
        if key not in aggregated:
            aggregated[key] = [features, label, sentence, boundary, 0.0]
        aggregated[key][4] += sample_weight
    rows = list(aggregated.values())
    for _ in range(EPOCHS):
        for features, label, _, _, sample_weight in rows:
            score = bias + sum(weights.get(feature, 0.0) for feature in features)
            probability = 1.0 / (1.0 + math.exp(-max(-30.0, min(30.0, score))))
            error = (label - probability) * sample_weight
            bias += LEARNING_RATE * error
            for feature in features:
                value = weights.get(feature, 0.0)
                weights[feature] = value + LEARNING_RATE * (error - L2 * value)
    return weights, bias


def classify(rows, weights, bias, threshold=0.0):
    output = []
    for features, label, sentence, boundary, sample_weight in rows:
        score = bias + sum(weights.get(feature, 0.0) for feature in features)
        output.append((score > threshold, bool(label), sentence, boundary, score,
                       sample_weight))
    return output


def metrics(rows):
    tp = sum(weight for predicted, actual, *_, weight in rows if predicted and actual)
    fp = sum(weight for predicted, actual, *_, weight in rows if predicted and not actual)
    fn = sum(weight for predicted, actual, *_, weight in rows if not predicted and actual)
    tn = sum(weight for predicted, actual, *_, weight in rows if not predicted and not actual)
    precision = tp / (tp + fp) if tp + fp else 0.0
    recall = tp / (tp + fn) if tp + fn else 0.0
    f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    return tp, fp, fn, tn, precision, recall, f1


def render(weights, bias, observation_sha):
    tables = [[] for _ in range(FEATURE_TABLE_COUNT)]
    for (table, units), value in weights.items():
        quantized = int(round(value * SCALE))
        if quantized:
            padded = units + (0,) * (3 - len(units))
            tables[table].append((len(units), units, padded, quantized))
    lines = [
        "/* Generated from first-party text and pinned Chromium break observations.",
        " * Observation data SHA-256: %s. Regenerate with tools/layout/train_phrase_model.py." % observation_sha,
        " * No third-party model source or weights are used. */",
        "static const PhraseFeature phrase_model_ja_features[%d][%d] = {" % (
            len(tables), max(1, max(len(rows) for rows in tables))),
    ]
    max_rows = max(1, max(len(rows) for rows in tables))
    for table in tables:
        table.sort(key=lambda row: (row[0], row[1]))
    for table in tables:
        lines.append("  {")
        for length, _, padded, weight in table:
            values = ", ".join("0x%04X" % value for value in padded)
            lines.append("    {%d, {%s}, %d}," % (length, values, weight))
        if not table:
            lines.append("    {0, {0, 0, 0}, 0},")
        lines.append("  },")
    lines.extend([
        "};",
        "static const size_t phrase_model_ja_feature_counts[%d] = {%s};" % (
            len(tables), ", ".join(str(len(table)) for table in tables)),
        "static const int32_t phrase_model_ja_bias = %d;" % int(round(bias * SCALE)),
        "static const int32_t phrase_model_ja_threshold = %d;" % int(round(THRESHOLD * SCALE)),
        "",
    ])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="check the generated header")
    parser.add_argument("--metrics", action="store_true", help="print train and held-out metrics")
    args = parser.parse_args()
    raw = OBSERVATIONS.read_bytes()
    import hashlib
    observation_sha = hashlib.sha256(raw).hexdigest()
    data = json.loads(raw)
    train_rows = examples(data["observations"], "train")
    heldout_rows = examples(data["observations"], "holdout")
    weights, bias = train(train_rows)
    if args.metrics:
        for name, rows in (("train", train_rows), ("holdout", heldout_rows)):
            values = metrics(classify(rows, weights, bias, THRESHOLD))
            print("%s: TP=%d FP=%d FN=%d TN=%d precision=%.3f recall=%.3f F1=%.3f" %
                  ((name,) + values))
    output = render(weights, bias, observation_sha)
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != output:
            raise SystemExit("phrase_model_ja.h is stale; regenerate it")
    else:
        OUTPUT.write_text(output, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
