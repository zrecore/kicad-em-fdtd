#!/bin/sh
# End to end, on the CPU backend, for `emfdtd --keep-fields` (docs/ux.md, "Saved runs"): a run without the flag
# leaves exactly the files it always left; a run with it also leaves the field data and a manifest, with the
# same S11 to the byte; the saved run reopens; a run resumed from a checkpoint keeps none and says why.
# Usage: keep_fields_check.sh EMFDTD SCRATCH_DIR (ctest runs it; the scratch folder is recreated).
set -eu
BIN=$1
T=$2
rm -rf "$T"
mkdir -p "$T"
fail() { echo "keep_fields_check: $*" >&2; exit 1; }

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
for d in plain kept ck js; do mkdir -p "$T/$d"; write_board "$T/$d"; done

# 1. Without the flag: the files a headless run has always written, and nothing else.
"$BIN" "$T/plain/snap.json" $FLAGS > "$T/plain.out" 2>&1 || fail "the plain run failed: $(tail -3 "$T/plain.out")"
PLAIN=$(ls -d "$T"/plain/em-fdtd/results/run-*)
[ "$(ls "$PLAIN" | tr '\n' ' ')" = "board.json s11.s1p settings.json summary.txt " ] ||
  fail "a run without --keep-fields wrote: $(ls "$PLAIN" | tr '\n' ' ')"
grep -q "fields" "$PLAIN/summary.txt" && fail "a run without --keep-fields mentions fields in summary.txt"

# 2. With the flag: the same S11, plus the field files and a manifest that says so.
"$BIN" "$T/kept/snap.json" $FLAGS --keep-fields > "$T/kept.out" 2>&1 || fail "the --keep-fields run failed: $(tail -3 "$T/kept.out")"
KEPT=$(ls -d "$T"/kept/em-fdtd/results/run-*)
cmp "$PLAIN/s11.s1p" "$KEPT/s11.s1p" || fail "--keep-fields changed the S11"
for f in slice_e volume_e power_eh; do [ -s "$KEPT/fields/$f.f16" ] || fail "missing fields/$f.f16"; done
grep -q '"fields": true' "$KEPT/run.json" || fail "run.json does not say fields: true"
grep -q "^fields: kept, " "$KEPT/summary.txt" || fail "summary.txt does not say the fields were kept"
grep -q "^fields: kept" "$T/kept.out" || fail "the log does not say the fields were kept"
"$BIN" --run-info "$KEPT" --json > "$T/info.json" || fail "--run-info failed on the kept run"
# the kept run reopens without solving (a window screenshot on the CPU path; exit 1 if a control is unnamed)
"$BIN" "$T/kept/snap.json" --cpu --open-run "$KEPT" --ui-screenshot "$T/open.png" --ui-a11y-dump "$T/open.txt" \
  > "$T/open.out" 2>&1 || fail "--open-run failed on the kept run: $(tail -3 "$T/open.out")"
[ -s "$T/open.png" ] || fail "--open-run wrote no screenshot"

# 3. The dry run says how big the field data would be, and the run's JSON says what happened.
"$BIN" "$T/js/snap.json" $FLAGS --dry-run --json > "$T/dry.json" 2> /dev/null || fail "the dry run failed"
"$BIN" "$T/js/snap.json" $FLAGS --keep-fields --json > "$T/run.json" 2> /dev/null || fail "the JSON run failed"
python3 -I - "$T/dry.json" "$T/run.json" "$T/js/em-fdtd/results" <<'PY' || fail "the JSON documents are wrong"
import glob, json, os, sys
dry, run, results = (json.load(open(sys.argv[1])), json.load(open(sys.argv[2])), sys.argv[3])
size = dry["estimate"]["field_bytes"]
assert size > 0, dry
folder = glob.glob(os.path.join(results, "run-*", "fields"))[0]
real = sum(os.path.getsize(os.path.join(folder, f)) for f in os.listdir(folder))
assert 0.9 * real - 1e6 <= size <= 1.1 * real + 1e6, (size, real)
assert run["fields"]["kept"] is True and run["fields"]["bytes"] == real, run["fields"]
PY
"$BIN" "$T/js/snap.json" $FLAGS --json > "$T/run2.json" 2> /dev/null || fail "the second JSON run failed"
python3 -I -c "import json,sys; assert json.load(open(sys.argv[1]))['fields'] is None" "$T/run2.json" ||
  fail "a run without the flag reports fields"

# 4. Checkpoints and field accumulators exclude each other (the solver refuses phasor lists with a checkpoint):
# a run with --checkpoint-dir keeps no fields and says so, fresh or resumed; it must not write a wrong file.
mkdir -p "$T/ck/ck"
( sleep 1; touch "$T/ck/ck/stop" ) &
set +e
"$BIN" "$T/ck/snap.json" $FLAGS --keep-fields --checkpoint-dir "$T/ck/ck" --checkpoint-every 0.05 > "$T/pause.out" 2>&1
code=$?
set -e
wait
[ "$code" = 75 ] || fail "the paused run exited $code, not 75: $(tail -3 "$T/pause.out")"
"$BIN" "$T/ck/snap.json" $FLAGS --keep-fields --checkpoint-dir "$T/ck/ck" --resume > "$T/resume.out" 2>&1 ||
  fail "the resume failed: $(tail -3 "$T/resume.out")"
RES=$(ls -d "$T"/ck/em-fdtd/results/run-*)
[ ! -e "$RES/fields" ] || fail "a resumed run wrote field files"
grep -q '"fields": false' "$RES/run.json" || fail "the resumed run's manifest does not say fields: false"
grep -q "fields requested but not kept: the run was resumed" "$RES/summary.txt" || fail "no note in summary.txt"

# 5. Refusals: the window and sweeps have their own rules.
set +e
"$BIN" "$T/plain/snap.json" $FLAGS --keep-fields --sweep sweep_points=11,21 > "$T/sweep.out" 2>&1
[ $? = 2 ] || { set -e; fail "--keep-fields with --sweep was not exit 2"; }
set -e
echo "keep_fields_check: ok (plain run unchanged, same S11 with fields, resumed run keeps none)"
