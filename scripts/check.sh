#!/bin/sh
# One command for "is the tree healthy": build, CPU tests, formatting, Python lint and tests.
#   scripts/check.sh          CPU tests only (runs anywhere)
#   scripts/check.sh --gpu    also GPU tests, under the shared GPU lock
set -eu
cd "$(dirname "$0")/.."
FMT=${CLANG_FORMAT:-/opt/homebrew/opt/llvm/bin/clang-format}
LOCK=/Volumes/CrucialX9/Projects/mongo/poc/gpu/tools/with_gpu_lock.sh

scripts/version.sh check
cmake --preset ci >/dev/null
cmake --build --preset ci
ctest --preset ci -L '^cpu$' # anchored: -L is a regex and would also match cpu_slow (minutes under ASan)
cmake --preset release >/dev/null
cmake --build --preset release
ctest --preset release -L cpu_slow
if [ "${1:-}" = "--gpu" ]; then "$LOCK" nice -n 10 ctest --preset ci -L gpu; fi

find src apps tests -name '*.cpp' -o -name '*.hpp' | xargs "$FMT" --dry-run --Werror

if [ -x plugin/.venv/bin/pytest ]; then
  (cd plugin && .venv/bin/ruff check . && .venv/bin/ruff format --check . && .venv/bin/pytest -q)
fi
echo "check: ok"
