#!/bin/sh
# The Farm tab against a stand-in for the farm tool (tests/farm_stub/farm_ui_stub.sh): at 720x520 it must render,
# every control must have a name (emfdtd exits 1 otherwise), and the jobs, progress bars and members must be
# in what a screen reader is told. Also the two-step confirmation when a role is turned on.
#   scripts/ui_farm_check.sh [path/to/emfdtd]
set -eu
cd "$(dirname "$0")/.."
BIN=${1:-build/release/apps/emfdtd/emfdtd}
OUT=out/ui-farm
mkdir -p "$OUT/project"
python3 plugin/tools/make_patch_snapshot.py "$OUT/patch.json" >/dev/null
export EMFDTD_FARM_COMMAND="$PWD/tests/farm_stub/farm_ui_stub.sh"
run() { # name extra-args...
  name=$1; shift
  "$BIN" "$OUT/patch.json" --project-dir "$OUT/project" --profile wifi --set mesh_preset=coarse --cpu \
    --ui-tab Farm --ui-size 720x520 --ui-screenshot "$OUT/$name.png" --ui-a11y-dump "$OUT/$name.txt" "$@" >/dev/null
}
run farm
for want in 'tab "Farm"' 'checkbox \[x\] "Coordinator"' 'checkbox \[ \] "Worker"' 'progressbar "Progress of nd_med_01-on-RTX" = "about 37 %"' \
            'button "Cancel nd_med_01-on-RTX"' 'text "step 41200, 312 s so far"' 'button "Revoke worker:win-egpu"' 'textinput "Name of the new device"'; do
  grep -q "$want" "$OUT/farm.txt" || { echo "ui_farm_check: missing in the accessibility dump: $want" >&2; exit 1; }
done
run farm_confirm --ui-click 20,229 # the Worker checkbox
for want in 'button "Turn on"' 'button "Keep it off"'; do
  grep -q "$want" "$OUT/farm_confirm.txt" || { echo "ui_farm_check: no confirmation step: $want" >&2; exit 1; }
done
echo "ui_farm_check: ok"
