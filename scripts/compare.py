#!/usr/bin/env python3
"""Compare curlbench JSON runs: one baseline against one or more variants.

usage:
  compare.py --baseline DIR_OR_FILE [--variant NAME]... [--drop-first-pass]
  compare.py results/            # auto: 'base' is the baseline, rest variants

Reads every <variant>.pass<N>.json in a directory, takes the median of the
per-pass medians for each case, and reports the change against the baseline.

On significance: a benchmark's spread is not Gaussian -- a stalled pass is a
one-sided outlier -- so this uses the pass-to-pass and rep-to-rep spread the
harness already measured (MAD) rather than a t-test that would assume a shape
the data does not have. A delta is called only when it exceeds the noise band
built from both sides' spread. Anything inside the band prints as "-", which
means "this run cannot tell", not "no effect".
"""

import argparse
import json
import os
import re
import statistics
import sys

PASS_RE = re.compile(r"^(?P<variant>.+)\.pass(?P<n>\d+)\.json$")


def load_dir(path):
    """-> {variant: [ {case: rec}, ... one dict per pass ] }"""
    runs = {}
    for name in sorted(os.listdir(path)):
        m = PASS_RE.match(name)
        if not m:
            continue
        full = os.path.join(path, name)
        # A pass still being written, or one whose process died, leaves a
        # truncated or empty file. Skip it loudly: crashing here would throw
        # away every complete pass alongside it, and silently dropping it would
        # let a comparison quietly rest on fewer passes than it claims.
        try:
            with open(full) as fh:
                doc = json.load(fh)
        except (json.JSONDecodeError, UnicodeDecodeError):
            sz = os.path.getsize(full)
            print("!! skipping %s: not valid JSON (%d bytes, run incomplete?)"
                  % (name, sz), file=sys.stderr)
            continue
        variant = doc.get("label") or m.group("variant")
        byname = {r["name"]: r for r in doc["results"]}
        runs.setdefault(variant, []).append((int(m.group("n")), byname, doc))
    for v in runs:
        runs[v].sort(key=lambda t: t[0])
    return runs


def collapse(passes, drop_first, key):
    """Median across passes of the per-pass median, plus a spread estimate."""
    use = passes[1:] if (drop_first and len(passes) > 1) else passes
    out = {}
    for _, byname, _ in use:
        for case, rec in byname.items():
            out.setdefault(case, []).append(rec)
    collapsed = {}
    for case, recs in out.items():
        vals = [r[key] for r in recs]
        med = statistics.median(vals)
        # spread: the larger of the across-pass spread and the worst
        # within-pass MAD, so neither source of noise is ignored
        across = max(vals) - min(vals) if len(vals) > 1 else 0.0
        within = max(r.get("ns_mad", 0.0) for r in recs)
        collapsed[case] = {
            "med": med,
            "spread": max(across, within),
            "passes": len(vals),
            "iters": recs[0].get("iters"),
            "targets": recs[0].get("targets", ""),
        }
    return collapsed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results", nargs="?", help="directory of *.passN.json")
    ap.add_argument("--baseline", default="base",
                    help="variant name treated as the baseline (default base)")
    ap.add_argument("--variant", action="append", default=[],
                    help="restrict to these variants (repeatable)")
    ap.add_argument("--drop-first-pass", action="store_true",
                    help="ignore pass 1 of every variant")
    ap.add_argument("--metric", default="ns_med",
                    choices=["ns_med", "ns_min", "cpu_ns_med"],
                    help="which number to compare (default ns_med)")
    args = ap.parse_args()

    path = args.results or "."
    if not os.path.isdir(path):
        sys.exit("not a directory: %s" % path)

    runs = load_dir(path)
    if not runs:
        sys.exit("no <variant>.pass<N>.json files in %s" % path)
    if args.baseline not in runs:
        sys.exit("baseline variant %r not found; have: %s"
                 % (args.baseline, ", ".join(sorted(runs))))

    base = collapse(runs[args.baseline], args.drop_first_pass, args.metric)
    variants = [v for v in sorted(runs) if v != args.baseline]
    if args.variant:
        variants = [v for v in variants if v in args.variant]

    machine = runs[args.baseline][0][2].get("machine", "?")
    print("# machine: %s   metric: %s   baseline: %s"
          % (machine, args.metric, args.baseline))
    print("# passes: %s%s"
          % (len(runs[args.baseline]),
             " (first dropped)" if args.drop_first_pass else ""))
    print()

    for v in variants:
        cur = collapse(runs[v], args.drop_first_pass, args.metric)
        print("=== %s" % v)
        print("%-28s %12s %12s %9s  %s"
              % ("case", "base ns", "%s ns" % v[:10], "delta", "verdict"))
        rows = []
        for case in sorted(set(base) & set(cur)):
            b, c = base[case], cur[case]
            if b["med"] <= 0:
                continue
            delta = 100.0 * (c["med"] - b["med"]) / b["med"]
            band = 100.0 * (b["spread"] + c["spread"]) / b["med"]
            # require the change to clear the combined spread, and to be at
            # least 1% -- below that the harness itself is the measurement
            if abs(delta) <= max(band, 1.0):
                verdict = "-"
            elif delta < 0:
                verdict = "FASTER"
            else:
                verdict = "SLOWER"
            rows.append((delta, case, b["med"], c["med"], band, verdict))
        rows.sort()
        for delta, case, bm, cm, band, verdict in rows:
            print("%-28s %12.1f %12.1f %+8.2f%%  %-6s (noise +-%.2f%%)"
                  % (case, bm, cm, delta, verdict, band))
        missing = sorted(set(base) ^ set(cur))
        if missing:
            print("!! cases present in only one side: %s" % ", ".join(missing))
        print()


if __name__ == "__main__":
    main()
