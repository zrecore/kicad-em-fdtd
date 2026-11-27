#!/bin/sh
# clang-tidy over src/ and apps/ (not tests, not fetched code), using the compile database of the ci preset
# (build/ci/compile_commands.json, written by `cmake --preset ci`). Checks and the ones that fail the run
# (WarningsAsErrors) are in .clang-tidy. Exits 1 if a WarningsAsErrors check fires. The caller takes the CPU lock.
#   scripts/tidy.sh            every .cpp in src/ and apps/
#   scripts/tidy.sh --changed  only the .cpp files that differ from HEAD (plus untracked); fast local loop
set -eu
cd "$(dirname "$0")/.."
TIDY_DIR=${TIDY_DIR:-/opt/homebrew/opt/llvm/bin}
RUN=${RUN_CLANG_TIDY:-$TIDY_DIR/run-clang-tidy}
[ -x "$RUN" ] || RUN=$(command -v run-clang-tidy || true)
[ -n "$RUN" ] || { echo "tidy.sh: run-clang-tidy not found (brew install llvm)" >&2; exit 2; }
[ -f build/ci/compile_commands.json ] || { echo "tidy.sh: run 'cmake --preset ci' first" >&2; exit 2; }
if [ "${1:-}" = "--changed" ]; then
  FILES=$( { git diff --name-only HEAD; git ls-files --others --exclude-standard; } | grep -E '^(src|apps)/.*\.cpp$' | sort -u || true)
  [ -n "$FILES" ] || { echo "tidy.sh: no changed .cpp files"; exit 0; }
  # shellcheck disable=SC2086
  "$RUN" -p build/ci -quiet -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" $FILES
else
  "$RUN" -p build/ci -quiet -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" '/(src|apps)/.*\.cpp$'
fi
