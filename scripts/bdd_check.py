#!/usr/bin/env python3
"""BDD conformance check (the CLAUDE.md rule: scenario first, tolerance in the THEN).

What a tool can enforce, and does here:
  * Gherkin: every scenario in plugin/tests/features/*.feature has a Then step (pytest-bdd already fails a
    scenario whose steps have no code behind them, so features and tests cannot drift apart).
  * Python: a test is a bound scenario, not a bare `def test_...`. Bare tests that existed when this check was
    added are listed per file in scripts/bdd_baseline.json; a file may never gain bare tests (it may lose them).
  * C++: every Catch2 SCENARIO has a THEN (where the tolerance goes), and a bare TEST_CASE is a ratchet like the Python one.
What it cannot enforce: that the THEN states a sensible tolerance. That stays a review rule.
Run `scripts/bdd_check.py --update` to lower the baseline after converting tests (it never raises it).
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = ROOT / "scripts" / "bdd_baseline.json"


def gherkin_problems():
    out = []
    for f in sorted((ROOT / "plugin/tests/features").glob("*.feature")):
        name, has_then, steps = None, False, 0
        for n, line in enumerate(f.read_text().splitlines() + ["  Scenario: end"], 1):
            t = line.strip()
            if t.startswith(("Scenario:", "Scenario Outline:")):
                if name and not has_then:
                    out.append(f"{f.name}: scenario '{name}' has no Then step")
                name, has_then = t.split(":", 1)[1].strip(), False
            elif t.startswith(("Then ", "And ")) and name and steps >= 0:
                if t.startswith("Then "):
                    has_then = True
        # And-steps after a Then belong to it; a scenario that only has Given/When/And lacks a Then
    return out


def bare_counts():
    counts = {}
    for f in sorted((ROOT / "plugin/tests").glob("test_*.py")):
        text = f.read_text()
        n = len(re.findall(r"^def test_", text, re.M))
        if n and "scenarios(" not in text:
            counts[str(f.relative_to(ROOT))] = n
        elif n:  # a file with scenarios may also hold bare tests
            counts[str(f.relative_to(ROOT))] = n
    for f in sorted((ROOT / "tests").rglob("*.cpp")):
        text = f.read_text()
        n = len(re.findall(r"^\s*TEST_CASE\(", text, re.M))
        if n:
            counts[str(f.relative_to(ROOT))] = n
    return counts


def cpp_problems():
    out = []
    for f in sorted((ROOT / "tests").rglob("*.cpp")):
        text = f.read_text()
        blocks = re.split(r"^\s*SCENARIO\(", text, flags=re.M)[1:]
        for b in blocks:
            title = b.split(")", 1)[0][:60]
            body = re.split(r"^\s*(?:SCENARIO|TEST_CASE)\(", b, flags=re.M)[0]
            if '"[.' in b.split("{", 1)[0]:
                continue  # a hidden report (not a test): it measures and prints
            if "THEN(" not in body:  # a scenario without a WHEN is fine; one without a THEN checks nothing
                out.append(f"{f.relative_to(ROOT)}: SCENARIO {title} has no THEN")
    return out


def main():
    problems = gherkin_problems() + cpp_problems()
    now = bare_counts()
    base = json.loads(BASE.read_text()) if BASE.exists() else None
    if "--update" in sys.argv or base is None:
        merged = now if base is None else {k: min(v, base.get(k, v)) for k, v in now.items() if k in base}
        BASE.write_text(json.dumps(dict(sorted(merged.items())), indent=1) + "\n")
        print(f"baseline written: {sum(merged.values())} bare tests in {len(merged)} files")
        return 0
    for k, v in now.items():
        if v > base.get(k, 0):
            problems.append(f"{k}: {v} bare tests, baseline {base.get(k, 0)}: write it as a scenario")
    for p in problems:
        print("bdd_check:", p)
    print(f"bdd_check: {sum(now.values())} bare tests left (baseline {sum(base.values())}); {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
