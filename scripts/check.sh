#!/bin/sh
# One command for "is the tree healthy": build, CPU tests, formatting, Python lint and tests. The builds and
# tests take a slot of the CPU queue (scripts/with_cpu_lock.sh).
#   scripts/check.sh          CPU tests only (runs anywhere)
#   scripts/check.sh --gpu    also GPU tests, under the shared GPU lock
#   scripts/check.sh --fast   skip the [slow] solver suite (about 6 minutes), and clang-tidy unless a C++ file under
#                             src/ or apps/ differs from origin/main: for changes that do not touch the solver
#                             (UI, farm, plugin, docs, CLI output). Everything else still runs. The flags can
#                             be combined; the default, CI and releases run everything.
set -eu
GPU=0
FAST=0
for arg in "$@"; do
  case "$arg" in
    --gpu) GPU=1 ;;
    --fast) FAST=1 ;;
    *) echo "check.sh: unknown option $arg (use --gpu, --fast)" >&2; exit 2 ;;
  esac
done
export FAST
cd "$(dirname "$0")/.."
FMT=${CLANG_FORMAT:-/opt/homebrew/opt/llvm/bin/clang-format}
LOCK=/Volumes/CrucialX9/Projects/mongo/poc/gpu/tools/with_gpu_lock.sh

scripts/version.sh check
sh scripts/with_cpu_lock_selftest.sh >/dev/null # the CPU queue itself
# The heavy CPU phases take a slot of the CPU queue (scripts/with_cpu_lock.sh), held for the whole phase and
# released before the GPU tests wait for their own lock.
scripts/with_cpu_lock.sh -l "check.sh: builds and CPU tests" sh -ec '
  cmake --preset ci >/dev/null
  cmake --build --preset ci
  ctest --preset ci -L "^cpu$" # anchored: -L is a regex and would also match cpu_slow (minutes under ASan)
  cmake --preset release >/dev/null
  cmake --build --preset release
  if [ "$FAST" = 1 ]; then echo "check.sh --fast: the [slow] suite is skipped"; else ctest --preset release -L cpu_slow; fi
  scripts/ui_farm_check.sh # the Farm tab renders against a stand-in tool, every control named
  scripts/ui_font_check.sh # the same tabs with the system font at 100 % and 150 % text size
'
if [ "$GPU" = 1 ]; then "$LOCK" nice -n 10 ctest --preset ci -L gpu; fi

# clang-tidy: always, except in --fast when no C++ file under src/ or apps/ differs from origin/main (committed,
# uncommitted or new): a push that adds a tidy finding fails CI (the ratchet), and --fast hid it once.
TIDY=1
if [ "$FAST" = 1 ]; then
  CPP_CHANGED=$( { git diff --name-only origin/main 2>/dev/null || echo src/unknown.cpp; git ls-files --others --exclude-standard; } | grep -E '^(src|apps)/.*\.(cpp|hpp|mm)$' || true)
  if [ -z "$CPP_CHANGED" ]; then TIDY=0; echo "check.sh --fast: clang-tidy is skipped (no C++ under src/ or apps/ changed)"; fi
fi
if [ "$TIDY" = 1 ]; then
  scripts/with_cpu_lock.sh -l "check.sh: clang-tidy" scripts/tidy.sh # needs build/ci from the build above
fi

find src apps tests -name '*.cpp' -o -name '*.hpp' | xargs "$FMT" --dry-run --Werror

if [ -x plugin/.venv/bin/pytest ]; then
  (cd plugin && .venv/bin/ruff check . && .venv/bin/ruff format --check . && .venv/bin/pytest -q --cov=emfdtd_plugin --cov-fail-under=72)
fi
python3 scripts/bdd_check.py # BDD conformance: every scenario has a Then, no new bare tests (ratchet)
python3 scripts/antenna/selftest.py >/dev/null # closed-form checks of the antenna helper scripts
python3 scripts/gerber/selftest.py >/dev/null # Gerber and Excellon tools (skipped without numpy)
if [ "$FAST" = 1 ]; then echo "check: ok (fast: the [slow] solver suite was not run)"; else echo "check: ok"; fi
