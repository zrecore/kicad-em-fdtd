#!/bin/sh
# The Farm tab and the "Run on" choice against a stand-in for the farm tool (tests/farm_stub/farm_ui_stub.sh):
# at 720x520 they must render, every control must have a name (emfdtd exits 1 otherwise), and the jobs, progress
# bars, elapsed time, Fetch button, members and the Run-on choice must be in what a screen reader is told. Also the
# two-step confirmation when a role is turned on, the cap on the jobs list, and showing a fetched result.
#   scripts/ui_farm_check.sh [path/to/emfdtd]
set -eu
cd "$(dirname "$0")/.."
BIN=${1:-build/release/apps/emfdtd/emfdtd}
OUT=out/ui-farm
rm -rf "$OUT/farm-home" "$OUT/project"
mkdir -p "$OUT/project" "$OUT/farm-home"
python3 plugin/tools/make_patch_snapshot.py "$OUT/patch.json" >/dev/null
export EMFDTD_FARM_COMMAND="$PWD/tests/farm_stub/farm_ui_stub.sh"
# The window reads the client role from the farm folder (no tool run) to decide whether to offer "Run on".
export EMFDTD_FARM_HOME="$PWD/$OUT/farm-home"
echo '{"enabled":{"coordinator":true,"worker":false,"client":true}}' > "$OUT/farm-home/farm.json"
run() { # name tab size extra-args...
  name=$1; tab=$2; size=$3; shift 3
  "$BIN" "$OUT/patch.json" --project-dir "$OUT/project" --profile wifi --set mesh_preset=coarse --cpu \
    --ui-tab "$tab" --ui-size "$size" --ui-screenshot "$OUT/$name.png" --ui-a11y-dump "$OUT/$name.txt" "$@" >/dev/null
}
need() { # file pattern what
  grep -q "$2" "$OUT/$1.txt" || { echo "ui_farm_check: missing in the accessibility dump of $1: $3" >&2; exit 1; }
}
run farm Farm 720x1100
need farm 'tab "Farm"' "the Farm tab"
need farm 'checkbox \[x\] "Coordinator"' "role switches"
need farm 'checkbox \[ \] "Worker"' "role switches"
need farm 'progressbar "Progress of nd_med_01-on-RTX" = "about 37 %"' "a progress bar"
need farm 'text "nd_med_01-on-RTX: running on win-egpu for 6 min"' "elapsed time"
need farm 'button "Cancel nd_med_01-on-RTX"' "Cancel"
need farm 'text "step 41200, 312 s so far"' "phase text"
need farm 'text "Solved on win-egpu (NVIDIA GeForce RTX 4060 Ti)."' "where a done job ran"
need farm 'button "Fetch and open the results of patch-farm-1"' "the Fetch button"
need farm 'button "Revoke worker:win-egpu"' "Revoke"
need farm 'textinput "Name of the new device"' "the invitation form"
# This computer's GPU (coordinator only): off but available shows the button, a click asks first, on shows state
# and Stop, unavailable shows why.
need farm 'group "This computer.s GPU"' "the section for this computer's GPU"
need farm 'button "Also use this computer.s GPU for the farm"' "the one-click option"
run local_confirm Farm 720x1100 --ui-click 150,427
need local_confirm 'button "Use this GPU"' "the confirmation for this computer's GPU"
need local_confirm 'button "Keep it off"' "the confirmation for this computer's GPU"
(EMFDTD_FARM_STUB_LOCAL=on; export EMFDTD_FARM_STUB_LOCAL; run local_on Farm 720x1100)
need local_on 'Apple M1 Pro is offered to the farm: running and connected' "the running state"
need local_on 'button "Stop using this computer.s GPU"' "the Stop button"
(EMFDTD_FARM_STUB_LOCAL=unavailable; export EMFDTD_FARM_STUB_LOCAL; run local_unavailable Farm 720x1100)
need local_unavailable 'no Vulkan GPU that the solver can use' "why this computer's GPU cannot be offered"
if grep -q 'button "Also use this computer' "$OUT/local_unavailable.txt"; then echo "ui_farm_check: the button is shown although the GPU is unavailable" >&2; exit 1; fi
(EMFDTD_FARM_STUB_MANY=25; export EMFDTD_FARM_STUB_MANY; run farm_many Farm 720x520) # a subshell: the variable must not leak
need farm_many 'older finished jobs are not listed' "the cap on the jobs list"
run farm_confirm Farm 720x700 --ui-click 20,229 # the Worker checkbox
need farm_confirm 'button "Turn on"' "a confirmation step"
need farm_confirm 'button "Keep it off"' "a confirmation step"
# Clicking "Fetch and open" on the done job: the tool fetches into <project>/em-fdtd/results/run-farm-<id8> and the
# window shows the S11 and says it came from the farm.
run fetch_click Farm 720x1100 --ui-click 70,777
need fetch_click 'Fetched from the farm' "the window shows a fetched farm result"
[ -f "$OUT/project/em-fdtd/results/run-farm-d0d0d0d0/s11.s1p" ] || { echo "ui_farm_check: the results were not fetched into the project's results folder" >&2; exit 1; }
run preview Preview 720x520
need preview 'combobox "Where the run goes" = "this GPU"' "the Run-on choice (default this GPU)"
need preview 'button "Run simulation"' "the Run button"
echo '{"run_on":"farm"}' > "$OUT/farm-home/window.json"
run preview_farm Preview 720x520
need preview_farm 'combobox "Where the run goes" = "the farm"' "the Run-on choice remembered as the farm"
need preview_farm 'button "Run on the farm"' "the Run button names where it goes"
rm -f "$OUT/farm-home/window.json"
# A fetched farm result shows its S11 and says where it came from.
mkdir -p "$OUT/fetched"
printf '# Hz S RI R 50\n2.4e9 -0.3 0.1\n2.45e9 -0.4 0.2\n2.5e9 -0.5 0.2\n' > "$OUT/fetched/s11.s1p"
run fetched "S11 plot" 720x520 --ui-farm-result "$OUT/fetched"
need fetched 'Fetched from the farm' "the farm-result status"
echo "ui_farm_check: ok"
