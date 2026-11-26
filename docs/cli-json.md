# Machine-readable output (`--json`)

`emfdtd` can print one JSON document instead of text, for scripts and for AI assistants that drive the solver. The text output is unchanged when `--json` is absent.

Every document starts with:

```json
{ "schema": 1, "tool": "emfdtd", "version": "0.1.0", ... }
```

**Stability.** Changes within a schema number are additive only: new keys may appear, existing keys keep their meaning and type. A breaking change bumps `schema`. Read the keys you need and ignore the rest. Units are SI (Hz, s, m, bytes) except where a key name says otherwise.

**stdout is exactly one document.** In `--json` mode everything else the program prints (progress lines, notes, the human summary) goes to stderr. On failure the document is `{"error": "..."}` (with the header) and the exit code is the same as without `--json` (1 for a failed run or bad input, 2 for a bad command line).

`--json` covers the five commands below. It is refused (exit 2) together with `--ui`, `--ui-screenshot`, `--view`, `--sweep` and `--prune-runs`.

## `emfdtd --list-settings --json`

```json
{ "settings": [
  { "key": "mesh_preset", "label": "...", "tier": "essential", "group": "Mesh", "unit": "", "help": "...",
    "kind": "choice", "value": "medium", "default": "medium", "choices": ["coarse", "medium", "fine"] },
  { "key": "decay_threshold", "kind": "number", "value": 0.001, "default": 0.001, "min": 1e-07, "max": 0.1, ... }
] }
```

`kind` is `number`, `choice` or `colour`. Numbers are JSON numbers; `min`/`max` are the registry's own limits (numbers only), `choices` is the valid list (choices only). Without a board, `value` equals `default`. Tiers: `essential`, `common`, `advanced`, `expert`.

## `emfdtd --list-devices --json`

```json
{ "devices": [
  { "index": 0, "name": "Apple M1 Pro", "uuid": "0000106b...", "memory_bytes": 17179869184,
    "vulkan": [1, 2], "driver": "MoltenVK 1.4.2", "discrete": false, "usable": true } ] }
```

`index` is the number `--device` accepts: usable devices only, in enumeration order; it is `null` for a device below Vulkan 1.2 (`usable: false`). `--device` also takes a UUID prefix. On a machine with no Vulkan loader the answer is an error document.

## `emfdtd SNAPSHOT.json [options] --dry-run --json`

The plan and the estimate, no solve (no GPU time beyond opening the device to read its memory).

```json
{ "mode": "dry_run", "profile": "bluetooth", "variant": "...", "variant_status": "verified",
  "band_hz": [2.4e9, 2.4835e9], "sweep_hz": [...], "excitation_hz": [...],
  "grid": { "cells": [474, 327, 127], "cell_count": 19684746, "smallest_cell_m": 3.5e-05 },
  "time_step_s": 7.75e-14,
  "estimate": { "steps": 120000, "memory_bytes": 2.0e9, "seconds": 900, "exceeds_memory": false },
  "backend": "gpu", "device": "Apple M1 Pro",
  "changed_settings": ["mesh_preset"], "notes": ["..."] }
```

`backend` is `gpu` or `cpu`; `device` is empty on the CPU. `estimate` is the same rough figure the text prints (it is labeled rough in the window too).

## `emfdtd SNAPSHOT.json [options] --json` (a headless run)

The same document with `"mode": "run"` and:

```json
{ "results_dir": "/path/em-fdtd/results/run-20261125-231912", "steps": 118000, "time_step_s": 7.75e-14,
  "wall_seconds": 906.2, "mcells_per_second": 1250.0,
  "best_match": { "freq_hz": 2.3664e9, "db": -33.2 }, "exit_status": 0 }
```

`best_match` is `null` if the run produced no S11. `best_match` is the deepest point of the sweep; it is not a resonance estimate (the sweep resolution limits it; see `--run-info` and `scripts/antenna/subbin.py`).

## `emfdtd --run-info RUNDIR [--s11] --json`

Summarizes a saved run folder without solving, with no GPU and no window. The folder is the first argument after `--run-info`; `--json` is accepted for symmetry (this command always prints JSON).

```json
{ "mode": "run_info", "run_dir": "/abs/path", "board": "cc2430db", "profile": "bluetooth",
  "variant": "...", "variant_status": "verified", "band_hz": [..] or null, "sweep_hz": [..] or null,
  "grid": { "cells": [474, 327, 127], "cell_count": 19684746, "smallest_cell_m": 3.5e-05 } or null,
  "steps": 128224, "time_step_s": 7.75e-14, "backend": "gpu",
  "changed_settings": ["mesh_preset"], "notes": ["..."],
  "settings": { ...the run's settings.json, the user's deviations... } or null,
  "s11": { "points": 101, "f_min_hz": ..., "f_max_hz": ...,
           "best": { "freq_hz": ..., "db": ... },
           "band_10db": { "low_hz": ..., "high_hz": ..., "width_hz": ... } or null,
           "table": [ { "f_hz": ..., "re": ..., "im": ... }, ... ] },
  "files": { "summary": true, "settings": true, "board": true, "s11": true } }
```

- Missing pieces are `null`: a folder with only `s11.s1p` still works. A folder with neither `summary.txt` nor `s11.s1p` is an error.
- `table` appears only with `--s11`.
- `best` is the deepest S11 point on the sweep grid. `band_10db` is the stretch around it that stays at or below -10 dB, with edges interpolated linearly in dB (`null` if the dip never reaches -10 dB). For the series resonance to better than one sweep bin use the reactance zero crossing (`scripts/antenna/subbin.py`).
- Older runs wrote `dt` and no band lines in `summary.txt`; both wordings are read.

## Using it

```
emfdtd --list-settings --json | python3 -c 'import json,sys; print([s["key"] for s in json.load(sys.stdin)["settings"] if s["tier"] == "essential"])'
emfdtd board.json --cpu --dry-run --json --set mesh_preset=coarse | jq .estimate
emfdtd --run-info em-fdtd/results/run-20261125-231912 --json | jq .s11.best
```

Tests: `tests/cpu/cli_json_test.cpp` (shapes, exact) and `scripts/cli_json_check.sh` (the built binary; run by `ctest -L cpu`).
