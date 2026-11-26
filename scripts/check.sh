#!/bin/sh
# One command for "is the tree healthy": build, CPU tests, formatting, Python lint and tests. The builds and
# tests take a slot of the CPU queue (scripts/with_cpu_lock.sh).
#   scripts/check.sh          CPU tests only (runs anywhere)
#   scripts/check.sh --gpu    also GPU tests, under the shared GPU lock
set -eu
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
  ctest --preset release -L cpu_slow
  scripts/ui_farm_check.sh # the Farm tab renders against a stand-in tool, every control named
'
if [ "${1:-}" = "--gpu" ]; then "$LOCK" nice -n 10 ctest --preset ci -L gpu; fi

find src apps tests -name '*.cpp' -o -name '*.hpp' | xargs "$FMT" --dry-run --Werror

if [ -x plugin/.venv/bin/pytest ]; then
  (cd plugin && .venv/bin/ruff check . && .venv/bin/ruff format --check . && .venv/bin/pytest -q)
fi
python3 scripts/antenna/selftest.py >/dev/null # closed-form checks of the antenna helper scripts
python3 scripts/gerber/selftest.py >/dev/null # Gerber and Excellon tools (skipped without numpy)
echo "check: ok"
