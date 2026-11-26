#!/bin/sh
# End-to-end check of `emfdtd --json` against the built binary (docs/cli-json.md): the list commands, a saved
# run folder made in the scratch folder, and the error documents. No GPU, no window. Usage:
#   scripts/cli_json_check.sh PATH/TO/emfdtd SCRATCH_DIR
set -eu
EMFDTD=${1:?path to emfdtd}
SCRATCH=${2:?scratch folder}
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH/run"
cat > "$SCRATCH/run/summary.txt" <<'EOF'
kicad-em-fdtd run summary
board: fixture
profile: wifi / Wi-Fi 2.4 GHz [verified]
grid: 10 x 10 x 10 cells, smallest cell 0.5 mm
steps: 100, dt 1 ps, backend CPU
changed from default: mesh_preset
note: fixture
EOF
cat > "$SCRATCH/run/s11.s1p" <<'EOF'
! fixture
# Hz S RI R 50
2.0e9 -0.5 0
2.2e9 -0.2 0
2.4e9 -0.1 0
2.6e9 -0.3 0
2.8e9 -0.6 0
EOF
EMFDTD="$EMFDTD" SCRATCH="$SCRATCH" python3 -I - <<'EOF'
import json, os, subprocess, sys

exe, scratch = os.environ["EMFDTD"], os.environ["SCRATCH"]

def run(*args, code=0):
    p = subprocess.run([exe, *args], capture_output=True, text=True)
    assert code is None or p.returncode == code, (args, p.returncode, p.stderr)
    doc = json.loads(p.stdout)  # stdout is exactly one document
    assert doc["schema"] == 1 and doc["tool"] == "emfdtd" and doc["version"], doc
    return doc

settings = run("--list-settings", "--json")["settings"]
text = subprocess.run([exe, "--list-settings"], capture_output=True, text=True, check=True).stdout
assert len(settings) == len(text.strip().splitlines()), "one JSON entry per text line"
assert all("key" in s and "kind" in s and "default" in s for s in settings)

# A machine with no Vulkan loader (CI) answers with an error document: still exactly one JSON document.
doc = run("--list-devices", "--json", code=None)
assert isinstance(doc.get("devices"), list) or "error" in doc, doc

info = run("--run-info", os.path.join(scratch, "run"), "--json", "--s11")
assert info["board"] == "fixture" and info["steps"] == 100 and info["backend"] == "cpu"
s = info["s11"]
assert s["points"] == 5 and len(s["table"]) == 5
assert abs(s["best"]["freq_hz"] - 2.4e9) < 1 and abs(s["best"]["db"] + 20.0) < 1e-6
assert s["band_10db"] is None or s["band_10db"]["low_hz"] < 2.4e9 < s["band_10db"]["high_hz"]

assert "error" in run("--run-info", os.path.join(scratch, "nope"), "--json", code=1)
assert "error" in run(os.path.join(scratch, "no-such-snapshot.json"), "--json", code=1)
print("cli_json_check: ok")
EOF
