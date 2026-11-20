#!/bin/sh
# Mesh convergence of a one-port model: run the same snapshot at several cells-per-wavelength values on the GPU
# (under the shared lock) and print where the best match lands. Results go in out/convergence/.
#   scripts/convergence.sh SNAPSHOT.json [CELLS_PER_WAVELENGTH...]    (default: 20 30 45)
set -eu
cd "$(dirname "$0")/.."
snapshot="$1"; shift
[ $# -gt 0 ] || set -- 20 30 45
lock=/Volumes/CrucialX9/Projects/mongo/poc/gpu/tools/with_gpu_lock.sh
mkdir -p out/convergence
log="out/convergence/$(basename "$snapshot" .json)_$(date +%Y%m%d-%H%M%S).txt"
for c in "$@"; do
  echo "cells_per_wavelength=$c" | tee -a "$log"
  "$lock" nice -n 10 build/release/apps/emfdtd/emfdtd "$snapshot" --project-dir "$(dirname "$snapshot")" \
    --set cells_per_wavelength="$c" 2>&1 | grep -E "grid|steps of|best match" | tee -a "$log"
done
echo "log: $log"
