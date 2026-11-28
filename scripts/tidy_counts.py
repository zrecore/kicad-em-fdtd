#!/usr/bin/env python3
"""Per-check warning counts of a clang-tidy run (our code only) against scripts/tidy_baseline.json.

A ratchet, like scripts/bdd_check.py: a check's count may fall, never rise, and a check that is not in the
baseline may not appear. `--update` lowers the baseline (and drops checks that reached zero); it never raises
a count. Called by scripts/tidy.sh with the clang-tidy output and the root directories of this checkout (a Mac
checkout has more than one spelling of its path).

  tidy_counts.py LOG --roots A:B [--update]
"""

import argparse
import json
import os
import re
import sys
from collections import Counter
from pathlib import Path

BASE = Path(__file__).resolve().parent / "tidy_baseline.json"
WARN = re.compile(r"^(?P<file>/[^:\n]+):(?P<line>\d+):(?P<col>\d+): warning: .*\[(?P<checks>[A-Za-z0-9.,_-]+)\]$")


def count(log, roots):
    """Warnings in src/ and apps/ per check, each distinct file:line:col:check once."""
    seen = set()
    for line in Path(log).read_text(errors="replace").splitlines():
        m = WARN.match(line)
        if not m:
            continue
        path = os.path.normpath(m["file"])
        for r in roots:
            if path.startswith(r + "/") and path[len(r) + 1 :].split("/")[0] in ("src", "apps"):
                rel = path[len(r) + 1 :]
                for c in m["checks"].split(","):
                    seen.add((rel, m["line"], m["col"], c))
                break
    return Counter(c for _, _, _, c in seen)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--roots", required=True)
    ap.add_argument("--update", action="store_true")
    a = ap.parse_args()
    roots = [os.path.normpath(r) for r in a.roots.split(":") if r]
    now = count(a.log, roots)
    base = json.loads(BASE.read_text())["checks"] if BASE.exists() else None
    print(f"tidy: {sum(now.values())} warnings in our code, {len(now)} checks")
    if base is None:
        if not a.update:
            print("tidy: scripts/tidy_baseline.json is missing (run scripts/tidy.sh --update once)", file=sys.stderr)
            return 1
        base = dict(now)
    bad = [
        f"  {c}: baseline {base.get(c, 0)}, now {n}" + ("" if c in base else " (not in the baseline)")
        for c, n in sorted(now.items())
        if n > base.get(c, 0)
    ]
    if bad:
        print("tidy: more warnings than the baseline (fix them, or the ratchet is broken):", file=sys.stderr)
        print("\n".join(bad), file=sys.stderr)
        return 1
    lowered = {c: base[c] - now.get(c, 0) for c in base if now.get(c, 0) < base[c]}
    if a.update:
        BASE.write_text(json.dumps({"checks": dict(sorted(now.items()))}, indent=1) + "\n")
        print(f"tidy: baseline written ({len(lowered)} checks lowered)")
    elif lowered:
        print(f"tidy: {len(lowered)} checks are below the baseline: run scripts/tidy.sh --update to lock it in")
    return 0


if __name__ == "__main__":
    sys.exit(main())
