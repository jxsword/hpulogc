#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Performance regression gate for the nightly bench job (D-R13).

Parses bench_log/bench_sink output text, extracts the gate metrics and
compares each against the committed baseline
(tests/bench/baseline_linux.json). Any metric regressing more than the
relative threshold (default 25%) fails the gate.

Usage:
    bench_gate.py --baseline tests/bench/baseline_linux.json \
                  [--threshold 25] bench_log.txt bench_sink.txt

Baseline bootstrap: until the baseline file carries metrics, the gate
warns and passes (exit 0) so the first nightly run can establish it.

Exit codes: 0 = pass (or no baseline yet), 1 = regression detected,
2 = usage / input error.
"""

import argparse
import json
import re
import sys

# Lower-is-better gate metrics, extracted from the bench output text.
#   bench_log amortized line: "amortized logs=N avg=X ns"
#   bench_sink scenario line: "  <name> ops=N avg=X ns/op"
AMORTIZED_RE = re.compile(r"^\s*amortized logs=\d+ avg=([0-9.]+) ns$", re.M)
SINK_RE = re.compile(r"^\s{2}([a-z0-9_]+) ops=\d+ avg=([0-9.]+) ns/op$", re.M)


def parse_metrics(paths):
    """Extract gate metrics (ns, lower is better) from bench outputs."""
    metrics = {}
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        m = AMORTIZED_RE.search(text)
        if m:
            metrics["bench_log_amortized_avg_ns"] = float(m.group(1))
        for name, value in SINK_RE.findall(text):
            metrics["bench_sink_%s_avg_ns" % name] = float(value)
    return metrics


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", required=True,
                    help="path to baseline_linux.json")
    ap.add_argument("--threshold", type=float, default=25.0,
                    help="relative regression threshold in percent")
    ap.add_argument("outputs", nargs="+",
                    help="bench_log/bench_sink output text files")
    args = ap.parse_args()

    try:
        with open(args.baseline, "r", encoding="utf-8") as fh:
            baseline = json.load(fh).get("metrics", {})
    except (OSError, ValueError) as exc:
        print("bench-gate: cannot read baseline (%s): %s" %
              (args.baseline, exc))
        return 2

    if not baseline:
        print("bench-gate: WARNING baseline has no metrics yet; "
              "establish it from this run's artifacts "
              "(see docs/perf_report_sinks.md) and commit it")
        return 0

    metrics = parse_metrics(args.outputs)
    if not metrics:
        print("bench-gate: ERROR no gate metrics found in bench output")
        return 2

    limit = 1.0 + args.threshold / 100.0
    failed = []
    for key in sorted(baseline):
        base = float(baseline[key])
        if key not in metrics:
            print("bench-gate: WARNING metric %s missing from run output "
                  "(baseline %.0f ns)" % (key, base))
            continue
        now = metrics[key]
        status = "ok"
        if now > base * limit:
            status = "REGRESSION"
            failed.append(key)
        print("bench-gate: %-36s baseline=%9.0f ns  now=%9.0f ns  %s" %
              (key, base, now, status))

    if failed:
        print("bench-gate: FAIL %d metric(s) regressed more than %.0f%%: %s"
              % (len(failed), args.threshold, ", ".join(failed)))
        return 1
    print("bench-gate: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
