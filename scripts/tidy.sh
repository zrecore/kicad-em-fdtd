#!/bin/sh
# clang-tidy over src/ and apps/ (not tests, not fetched code), using the compile database of the ci preset
# (build/ci/compile_commands.json, written by `cmake --preset ci`). Checks, and the ones that fail the run at
# once (WarningsAsErrors), are in .clang-tidy; the checks that still have findings are a ratchet against
# scripts/tidy_baseline.json (counts may fall, never rise; scripts/tidy_counts.py). The caller takes the CPU lock
# (scripts/with_cpu_lock.sh).
#   scripts/tidy.sh            every .cpp in src/ and apps/, then the baseline comparison
#   scripts/tidy.sh --update   the same, then lowers the baseline (never raises it)
#   scripts/tidy.sh --changed  only the .cpp files that differ from HEAD (plus untracked); no baseline comparison
# A silent "clean" is made impossible: the binary must run; a canary .cpp and a canary header of ours, each with a
# known problem, must be reported (this also proves the header filter, which is derived from this checkout's root,
# not from its folder name); the number of files analyzed must equal the number asked for. Exit 1 on any failure.
set -eu
MODE=${1:-full}
case "$MODE" in full | --update | --changed) ;; *) echo "tidy.sh: unknown option $MODE" >&2; exit 2 ;; esac
cd "$(dirname "$0")/.."
TIDY=${CLANG_TIDY:-/opt/homebrew/opt/llvm/bin/clang-tidy}
[ -x "$TIDY" ] || TIDY=$(command -v clang-tidy || true)
if [ -z "$TIDY" ] || ! "$TIDY" --version >/dev/null 2>&1; then
  echo "tidy.sh: clang-tidy cannot be run (looked for ${CLANG_TIDY:-/opt/homebrew/opt/llvm/bin/clang-tidy} and PATH; brew install llvm)" >&2
  exit 2
fi
[ -f build/ci/compile_commands.json ] || { echo "tidy.sh: run 'cmake --preset ci' first" >&2; exit 2; }

# Our roots: every spelling of this checkout's path (the build tree remembers the one cmake was given; on this
# Mac /Users/... and /Volumes/... name the same folder). TIDY_ROOTS (colon list) replaces them: for testing only.
HOME_DIR=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' build/ci/CMakeCache.txt 2>/dev/null || true)
ROOTS=${TIDY_ROOTS:-$(for d in "$PWD" "$(pwd -P)" "$HOME_DIR" "$(cd "${HOME_DIR:-.}" 2>/dev/null && pwd -P)"; do
  [ -n "$d" ] && printf '%s\n' "$d"
done | sort -u | paste -sd: -)}
ALT=$(printf '%s' "$ROOTS" | sed 's/[][\.*^$+?(){}|\\]/\\&/g; s/:/|/g')
FILTER="^($ALT)/(src|apps|scripts/tidy_canary)/"
CANARY="$(pwd -P)/scripts/tidy_canary/canary.cpp"

# Canary: both the .cpp and its header must be reported, else the check or the filter is not working.
CAN=$("$TIDY" --quiet --checks='-*,readability-braces-around-statements' --header-filter="$FILTER" "$CANARY" -- -std=c++20 2>&1 || true)
if ! printf '%s\n' "$CAN" | grep -q 'canary\.cpp.*readability-braces-around-statements' ||
  ! printf '%s\n' "$CAN" | grep -q 'canary_header\.hpp.*readability-braces-around-statements'; then
  echo "tidy.sh: clang-tidy found nothing in a file that has a known problem: the check is not working (or the header filter hides our own headers: roots $ROOTS)" >&2
  exit 1
fi

if [ "$MODE" = "--changed" ]; then
  FILES=$( { git diff --name-only HEAD; git ls-files --others --exclude-standard; } | grep -E '^(src|apps)/.*\.cpp$' | sort -u || true)
  [ -n "$FILES" ] || { echo "tidy.sh: no changed .cpp files"; exit 0; }
else
  FILES=$(find src apps -name '*.cpp' | sort)
fi
# Windows-only sources need windows.h and cannot be analyzed on macOS or Linux: left out, listed here so the
# count stays honest.
SKIP='_win\.cpp$'
FILES=$(printf '%s\n' "$FILES" | grep -Ev "$SKIP" || true)
WANT=$(printf '%s\n' "$FILES" | grep -c . || true)
[ "$WANT" -gt 0 ] || { echo "tidy.sh: no .cpp files to check" >&2; exit 1; }

mkdir -p out/checks
LOG=$(mktemp out/checks/tidy.XXXXXX)
trap 'rm -f "$LOG"' EXIT
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
# One clang-tidy per file; each prints its output and a marker line with its exit status.
printf '%s\n' "$FILES" | xargs -P "$JOBS" -I{} sh -c '
  out=$("$1" -p build/ci --quiet --header-filter="$3" "$2" 2>&1); rc=$?
  printf "%s\ntidy-analyzed %s rc=%s\n" "$out" "$2" "$rc"' _ "$TIDY" {} "$FILTER" >"$LOG" 2>&1 || true
grep -v '^tidy-analyzed ' "$LOG" || true
DONE=$(grep -c '^tidy-analyzed ' "$LOG" || true)
BAD=$(grep '^tidy-analyzed ' "$LOG" | grep -vc 'rc=0$' || true)
echo "tidy.sh: $DONE of $WANT files analyzed, $BAD files with errors"
if [ "$DONE" -eq 0 ] || [ "$DONE" -ne "$WANT" ]; then
  echo "tidy.sh: analyzed file count ($DONE) is not the number asked for ($WANT): the run is not trustworthy" >&2
  exit 1
fi
RC=0
[ "$BAD" -eq 0 ] || { echo "tidy.sh: $BAD files failed (WarningsAsErrors or compile errors)" >&2; RC=1; }
case "$MODE" in
  --changed) python3 scripts/tidy_counts.py "$LOG" --roots "$ROOTS" >/dev/null 2>&1 || true ;;
  --update) python3 scripts/tidy_counts.py "$LOG" --roots "$ROOTS" --update || RC=1 ;;
  *) python3 scripts/tidy_counts.py "$LOG" --roots "$ROOTS" || RC=1 ;;
esac
exit $RC
