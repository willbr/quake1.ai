#!/usr/bin/env python3
"""
Compare two perf capture summaries.

Usage:
    scripts/perf_diff.py profiles/perf_<old>_summary.json profiles/perf_<new>_summary.json

Highlights per-scope regressions and wins, sorted by absolute change in
avg-ms. Prints a compact table you can paste into a PR.
"""
import json
import sys

def load(p):
    with open(p) as f:
        return json.load(f)

def main(old_path, new_path):
    old = load(old_path)
    new = load(new_path)
    rows = []
    scopes = set(old["scopes"]) | set(new["scopes"])
    for s in scopes:
        o = old["scopes"].get(s, {})
        n = new["scopes"].get(s, {})
        oa = o.get("avg_ms", 0.0)
        na = n.get("avg_ms", 0.0)
        delta = na - oa
        pct = (delta / oa * 100.0) if oa > 1e-6 else float("inf")
        rows.append((delta, s, oa, na, pct,
                     o.get("p95_ms", 0.0), n.get("p95_ms", 0.0),
                     o.get("calls", 0), n.get("calls", 0)))

    # Sort by absolute change descending.
    rows.sort(key=lambda r: -abs(r[0]))

    print(f"{'scope':<28} {'old avg':>9} {'new avg':>9} {'Δ ms':>9} {'Δ %':>8}"
          f"   {'old p95':>9} {'new p95':>9}   {'old N':>7} {'new N':>7}")
    print("-" * 110)
    for delta, s, oa, na, pct, op, np_, oc, nc in rows:
        if abs(delta) < 0.01:
            continue
        flag = ""
        if delta >  0.25: flag = "  ↑ slower"
        if delta < -0.25: flag = "  ↓ faster"
        pct_str = f"{pct:+.1f}%" if pct != float("inf") else "  new"
        print(f"{s:<28} {oa:>9.3f} {na:>9.3f} {delta:>+9.3f} {pct_str:>8}"
              f"   {op:>9.3f} {np_:>9.3f}   {oc:>7d} {nc:>7d}{flag}")

    if "frames" in old and "frames" in new:
        print(f"\nFrames: {old['frames']} -> {new['frames']}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__.strip())
        sys.exit(2)
    main(sys.argv[1], sys.argv[2])
