#!/bin/sh
# End to end, on the CPU backend: a run that is paused by DIR/stop, then resumed, writes exactly the S11 an
# uninterrupted run writes; and the refusals say why. Usage: checkpoint_check.sh EMFDTD SCRATCH_DIR
# (ctest runs it; the scratch folder is recreated). The board is the small two-pad load of the unit tests.
set -eu
BIN=$1
T=$2
rm -rf "$T"
mkdir -p "$T"
fail() { echo "checkpoint_check: $*" >&2; exit 1; }

# The board: two pads, a 50 ohm port and a 100 ohm load (the same JSON as tests/checkpoint_model.hpp).
write_board() {
  cat > "$1/snap.json" <<'J'
{"format":"kicad-em-fdtd.snapshot","version":1,"board":{"name":"load"},
 "outline":[[0,0],[0.01,0],[0.01,0.01],[0,0.01]],
 "stackup":[{"name":"F.Cu","kind":"copper"},
            {"name":"core","kind":"dielectric","thickness":0.0008,"epsilon_r":4.4,"loss_tangent":0.0}],
 "copper":[{"layer":"F.Cu","owner":"","net":"A","polygon":[[0.0012,0.0012],[0.0029,0.0012],[0.0029,0.0088],[0.0012,0.0088]]},
           {"layer":"F.Cu","owner":"","net":"B","polygon":[[0.0051,0.0012],[0.0088,0.0012],[0.0088,0.0088],[0.0051,0.0088]]}],
 "vias":[],"components":[],
 "ports":[{"name":"P1","layer":"F.Cu","a":[0.003,0.004],"b":[0.005,0.004],"width":0.0022,"impedance":50},
          {"name":"load","layer":"F.Cu","a":[0.003,0.007],"b":[0.005,0.007],"width":0.0022,"impedance":100}]}
J
}
FLAGS="--cpu --set mesh_preset=coarse --set max_steps=800"

mkdir -p "$T/ref" "$T/pr"
write_board "$T/ref"
write_board "$T/pr"
"$BIN" "$T/ref/snap.json" $FLAGS > "$T/ref.out" 2>&1 || fail "the reference run failed: $(tail -3 "$T/ref.out")"
REF=$(ls -d "$T"/ref/em-fdtd/results/run-*)/s11.s1p

# A run asked to pause by the stop file (created while it runs) writes a checkpoint and exits 75.
( sleep 1; touch "$T/pr/ck/stop" ) &
mkdir -p "$T/pr/ck"
set +e
"$BIN" "$T/pr/snap.json" $FLAGS --checkpoint-dir "$T/pr/ck" --checkpoint-every 0.05 > "$T/pause.out" 2>&1
code=$?
set -e
wait
[ "$code" = 75 ] || fail "the paused run exited $code, not 75: $(tail -3 "$T/pause.out")"
grep -q "^paused: checkpoint at step " "$T/pause.out" || fail "no 'paused:' line: $(tail -3 "$T/pause.out")"
[ -f "$T/pr/ck/ckpt.a" ] || [ -f "$T/pr/ck/ckpt.b" ] || fail "no checkpoint file was written"
STEP=$(sed -n 's/^paused: checkpoint at step \([0-9]*\).*/\1/p' "$T/pause.out")
[ "$STEP" -gt 0 ] && [ "$STEP" -lt 800 ] || fail "paused at step $STEP, expected between 1 and 799"

# Resuming finishes the run in the same results folder and gives the reference S11 exactly.
"$BIN" "$T/pr/snap.json" $FLAGS --checkpoint-dir "$T/pr/ck" --resume > "$T/resume.out" 2>&1 || fail "the resume failed: $(tail -3 "$T/resume.out")"
grep -q "^resuming from the checkpoint at step $STEP" "$T/resume.out" || fail "no 'resuming' line"
GOT=$(ls -d "$T"/pr/em-fdtd/results/run-*)/s11.s1p
[ "$(ls -d "$T"/pr/em-fdtd/results/run-* | wc -l | tr -d " ")" = 1 ] || fail "the resume made a second results folder"
cmp "$REF" "$GOT" || fail "the resumed S11 differs from the uninterrupted run's"
grep -q "^resumed at step $STEP" "$(dirname "$GOT")/summary.txt" || fail "summary.txt does not say it was resumed"

# Refusals, each with the reason.
set +e
"$BIN" "$T/pr/snap.json" $FLAGS --set max_steps=801 --checkpoint-dir "$T/pr/ck" --resume > "$T/other.out" 2>&1
[ $? = 1 ] && grep -q "other inputs" "$T/other.out" || { set -e; fail "a changed setting was not refused: $(tail -2 "$T/other.out")"; }
"$BIN" "$T/pr/snap.json" $FLAGS --resume > "$T/nodir.out" 2>&1
[ $? = 2 ] || { set -e; fail "--resume without --checkpoint-dir was not exit 2"; }
"$BIN" "$T/pr/snap.json" $FLAGS --view --checkpoint-dir "$T/pr/ck" > "$T/view.out" 2>&1
[ $? = 2 ] || { set -e; fail "--checkpoint-dir with --view was not exit 2"; }
"$BIN" "$T/pr/snap.json" $FLAGS --checkpoint-dir "$T/empty" --resume > "$T/none.out" 2>&1
[ $? = 1 ] && grep -q "no checkpoint can be resumed" "$T/none.out" || { set -e; fail "resuming from an empty folder was not refused"; }
set -e

# JSON mode: a paused run is one document with "paused": true and the exit status 75 ...
mkdir -p "$T/js/ck"
write_board "$T/js"
( sleep 1; touch "$T/js/ck/stop" ) &
set +e
"$BIN" "$T/js/snap.json" $FLAGS --json --checkpoint-dir "$T/js/ck" --checkpoint-every 0.05 > "$T/js.out" 2> "$T/js.err"
code=$?
set -e
wait
[ "$code" = 75 ] || fail "the paused JSON run exited $code, not 75"
python3 -c "
import json
d=json.load(open('$T/js.out'))
assert d['paused'] is True and d['exit_status']==75 and d['checkpoint_step']>0 and d['schema']==1, d
" || fail "the paused JSON document is wrong: $(head -c 300 "$T/js.out")"
# ... and the resumed run's document says where it resumed.
set +e
"$BIN" "$T/js/snap.json" $FLAGS --json --checkpoint-dir "$T/js/ck" --resume > "$T/js2.out" 2> "$T/js2.err"
code=$?
set -e
[ "$code" = 0 ] || fail "the resumed JSON run exited $code"
python3 -c "
import json
d=json.load(open('$T/js2.out'))
assert d['mode']=='run' and d['resumed_from_step']>0 and d['exit_status']==0, d
" || fail "the resumed JSON run document is wrong"

# A stop file left by an earlier run does not pause a fresh run: it is cleared.
touch "$T/js/ck/stop"
"$BIN" "$T/js/snap.json" $FLAGS --checkpoint-dir "$T/js/ck" > "$T/fresh.out" 2>&1 || fail "a fresh run with an old stop file did not finish: $(tail -2 "$T/fresh.out")"
echo "checkpoint_check: ok (paused at step $STEP, resumed, identical S11)"
