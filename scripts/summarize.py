#!/usr/bin/env python3
"""Per-patch summary across one or more architectures.

usage: summarize.py <label>=<results-dir> [<label>=<dir> ...] [--drop-first]

Significance uses an EMPIRICAL floor, not a guess. The matrix includes a
`base2` variant that is byte-identical to `base`, so whatever delta shows up
between them for a given case is exactly what that setup cannot resolve for
that case. A patch's delta must clear that per-case floor, the combined
pass-to-pass spread of both sides, and a 1% absolute minimum before it is
reported. Anything else is "not resolvable here", which is not the same claim
as "no effect".

Patches that build on each other are compared against their prerequisite, so
each number is that patch's own contribution rather than the stack's total.
"""

import json
import os
import statistics
import sys

# variant prefix -> the variant it must be compared against.
#
# A patch that only applies on top of an earlier one has to be measured
# against that earlier one, not against base, or it gets credited with the
# whole stack. Add an entry per dependent variant; both patches here are
# independent, so this is empty.
BASELINE_OF = {}
FLOOR_VARIANT = "base2"
DEFAULT_BASE = "base"
MIN_PCT = 1.0


def load(path, drop_first):
    """-> {variant: {case: {'med':x,'spread':y}}}"""
    passes = {}
    for name in sorted(os.listdir(path)):
        if not name.endswith(".json"):
            continue
        stem = name[: -len(".json")]
        if ".pass" not in stem:
            continue
        variant, _, pas = stem.rpartition(".pass")
        try:
            with open(os.path.join(path, name)) as fh:
                doc = json.load(fh)
        except (json.JSONDecodeError, UnicodeDecodeError, OSError):
            print("!! skipping unreadable %s" % name, file=sys.stderr)
            continue
        passes.setdefault(variant, []).append((int(pas), doc))

    out = {}
    for variant, plist in passes.items():
        plist.sort(key=lambda t: t[0])
        use = plist[1:] if (drop_first and len(plist) > 1) else plist
        per_case = {}
        for _, doc in use:
            for r in doc["results"]:
                per_case.setdefault(r["name"], []).append(r)
        out[variant] = {}
        for case, recs in per_case.items():
            vals = [r["ns_med"] for r in recs]
            out[variant][case] = {
                "med": statistics.median(vals),
                "spread": max(max(vals) - min(vals) if len(vals) > 1 else 0.0,
                              max(r.get("ns_mad", 0.0) for r in recs)),
                "targets": recs[0].get("targets", ""),
            }
    return out


def short(variant):
    return variant.split("-")[0]


def layout_floor(path, drop_first):
    """Per-case |delta| that dead code alone produces: the cross-binary floor.

    base-vs-base2 measures the same binary twice and so sees only run-to-run
    noise. Two different binaries also differ in code layout, which moves
    unrelated code by percent-scale amounts on these parts. The null variants
    measure exactly that, and it is the bar a patch's delta has to clear.
    """
    data = load(path, drop_first)
    if DEFAULT_BASE not in data:
        return {}
    base = data[DEFAULT_BASE]
    nulls = [v for v in data if v.startswith("null")]
    out = {}
    for case, b in base.items():
        if b["med"] <= 0:
            continue
        worst = 0.0
        for n in nulls:
            c = data[n].get(case)
            if c:
                worst = max(worst, abs(100.0 * (c["med"] - b["med"]) / b["med"]))
        out[case] = worst
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    drop_first = "--drop-first" in sys.argv
    lf_dirs = {}
    for a in sys.argv[1:]:
        if a.startswith("--layout="):
            label, _, path = a[len("--layout="):].partition("=")
            lf_dirs[label] = path
    if not args:
        sys.exit(__doc__)

    archs = []
    for a in args:
        label, _, path = a.partition("=")
        if not path:
            sys.exit("expected <label>=<dir>, got %r" % a)
        archs.append((label, load(path, drop_first)))

    for label, data in archs:
        byshort = {short(v): v for v in data}
        base = byshort.get(DEFAULT_BASE)
        floor_v = byshort.get(FLOOR_VARIANT)
        if not base:
            print("== %s: no base variant, skipping\n" % label)
            continue

        print("=" * 74)
        print("== %s" % label)
        print("=" * 74)

        # Empirical per-case floor from base vs base2.
        floor = {}
        if floor_v:
            for case, b in data[base].items():
                f = data[floor_v].get(case)
                if f and b["med"] > 0:
                    floor[case] = abs(100.0 * (f["med"] - b["med"]) / b["med"])
            if floor:
                fl = sorted(floor.values())
                print("noise floor (identical code, two runs): median %.2f%%, "
                      "worst %.2f%% over %d cases"
                      % (fl[len(fl) // 2], fl[-1], len(fl)))
        else:
            print("no base2 control present; falling back to spread only")

        # Fold in the cross-binary layout floor where a null-control run for
        # this architecture was supplied. It dominates the run-to-run floor by
        # an order of magnitude on these parts, so without it small deltas read
        # as findings when they are only shifted object code.
        if label in lf_dirs:
            lf = layout_floor(lf_dirs[label], drop_first)
            if lf:
                vals = sorted(lf.values())
                print("layout floor (dead code, no behaviour change): "
                      "median %.2f%%, p90 %.2f%%, worst %.2f%%"
                      % (vals[len(vals) // 2], vals[int(len(vals) * 0.9)],
                         vals[-1]))
                for case, v in lf.items():
                    floor[case] = max(floor.get(case, 0.0), v)
        print()

        for v in sorted(data):
            s = short(v)
            if s in (DEFAULT_BASE, FLOOR_VARIANT):
                continue
            base_v = byshort.get(BASELINE_OF.get(s, DEFAULT_BASE))
            if not base_v:
                print("-- %s: baseline missing, skipped" % s)
                continue

            rows = []
            for case, cur in data[v].items():
                b = data[base_v].get(case)
                if not b or b["med"] <= 0:
                    continue
                delta = 100.0 * (cur["med"] - b["med"]) / b["med"]
                band = max(
                    floor.get(case, 0.0),
                    100.0 * (b["spread"] + cur["spread"]) / b["med"],
                    MIN_PCT,
                )
                if abs(delta) > band:
                    rows.append((delta, case, b["med"], cur["med"], band))
            rows.sort()

            vs = "" if base_v == base else "  (vs %s)" % short(base_v)
            wins = [r for r in rows if r[0] < 0]
            regr = [r for r in rows if r[0] > 0]
            print("-- %s%s: %d faster, %d slower, of %d cases"
                  % (s, vs, len(wins), len(regr), len(data[v])))
            for delta, case, bm, cm, band in rows[:8]:
                print("     %-26s %10.1f -> %10.1f  %+7.2f%%  (>%.2f%%)"
                      % (case, bm, cm, delta, band))
            if len(rows) > 8:
                print("     ... and %d more" % (len(rows) - 8))
            if not rows:
                print("     nothing clears the floor on this machine")
            print()


if __name__ == "__main__":
    main()
