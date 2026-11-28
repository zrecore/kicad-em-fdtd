"""How big a job is, before it is sent: the submitting machine asks its own solver for a dry run on the CPU
(`--cpu --dry-run --json`: no GPU turn, nothing written next to the board) and scales the solver's step count
to what runs really take, so the scheduler can skip workers whose time ceiling the job would outrun.

The solver's estimate of steps is an upper bound: three times the excitation pulse, capped by max_steps. A run
stops earlier, when the ring-down has decayed below `decay_threshold`. Measured on the cc2430db antenna model
(medium preset, 2026-11-25), as a share of that bound:

  decay_threshold 1e-3  : 0.34 to 0.47  (117,952 steps of 278,043; 129,840 of about 274,000; Dk 4.2: 0.47)
  decay_threshold 3e-4  : 0.66          (182,672 steps)
  decay_threshold 1e-4  : 0.70          (195,360 steps)

so the share used here is 0.5 at the default threshold or looser, 0.7 down to 3e-4 and 1.0 (the bound itself)
for anything stricter. The scheduler then adds its own 25 % margin on the time. An estimate is a hint for
placement only: the worker enforces its own ceilings whatever it says.
"""

import json
import subprocess
import tempfile
from pathlib import Path

from . import config, jobs

DRY_RUN_TIMEOUT_S = 180.0
DEFAULT_DECAY = 1e-3


def step_share(decay_threshold):
    """The share of the solver's upper bound on steps that a run takes, from the measurements above."""
    if decay_threshold >= 1e-3:
        return 0.5
    if decay_threshold >= 3e-4:
        return 0.7
    return 1.0


def _model_args(spec):
    """The model's flags from a job spec (what the worker builds too, minus the GPU and project flags)."""
    argv = []
    if "profile" in spec:
        argv += ["--profile", spec["profile"]]
    for k, v in sorted(spec.get("sets", {}).items()):
        argv += ["--set", f"{k}={v}"]
    for k, v in sorted(spec.get("geoms", {}).items()):
        argv += ["--geom", f"{k}={v}"]
    for box in spec.get("refine", []):
        argv += ["--refine"] + [f"{x:g}" for x in box]
    for r, t in sorted(spec.get("parts", {}).items()):
        argv += ["--part", f"{r}={t}"]
    for r, t in sorted(spec.get("bodies", {}).items()):
        argv += ["--body", f"{r}={t}"]
    if spec.get("nets"):
        argv += ["--nets", ",".join(spec["nets"])]
    if spec.get("ground_nets"):
        argv += ["--ground-nets", ",".join(spec["ground_nets"])]
    return argv


def estimate_job(solver, snapshot, spec, timeout=DRY_RUN_TIMEOUT_S):
    """(estimate, why): {"cells", "steps", "bound_steps", "memory_bytes"} for the job, or (None, why not).
    Never raises: a job with no estimate is simply placed as before."""
    try:
        jobs.validate(spec, None, {"max_steps": 10**9, "max_tier": "expert"})
    except jobs.JobError as e:
        return None, f"the job is not valid: {e}"
    config.ensure_home()
    work = config.home() / "estimate"
    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=str(work)) as project:
        argv = [str(solver), str(snapshot), "--cpu", "--dry-run", "--json", "--project-dir", project]
        argv += _model_args(spec)
        try:
            done = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, check=False)
        except (OSError, subprocess.TimeoutExpired) as e:
            return None, f"the solver's dry run could not run: {e}"
    if done.returncode != 0:
        tail = (done.stderr or done.stdout).strip().splitlines()[-1:] or [f"exit {done.returncode}"]
        return None, f"the solver's dry run failed: {tail[0][:160]}"
    try:
        doc = json.loads(done.stdout)
        cells = float(doc["grid"]["cell_count"])
        bound = float(doc["estimate"]["steps"])
    except (ValueError, KeyError, TypeError):
        return None, "the solver's dry run did not print the expected JSON"
    if not (cells > 0 and bound > 0):
        return None, "the solver's dry run gave an empty estimate"
    decay = spec.get("sets", {}).get("decay_threshold", DEFAULT_DECAY)
    if isinstance(decay, bool) or not isinstance(decay, (int, float)) or decay <= 0:
        decay = DEFAULT_DECAY
    est = {"cells": cells, "steps": round(bound * step_share(decay)), "bound_steps": round(bound)}
    memory = doc["estimate"].get("memory_bytes")  # the solver's own estimate of the device memory it needs
    if isinstance(memory, (int, float)) and not isinstance(memory, bool) and memory > 0:
        est["memory_bytes"] = float(memory)
    fields = doc["estimate"].get("field_bytes")  # what --keep-fields would add to the result
    if isinstance(fields, (int, float)) and not isinstance(fields, bool) and fields > 0:
        est["field_bytes"] = float(fields)
    return est, ""


def default_solver():
    """The solver this machine would run a dry run with: EMFDTD_BIN, else the plugin's usual places, else the
    solver the worker agent is configured with. RuntimeError if there is none (the farm bundle has no
    launcher.py: a client that only has the bundle submits without an estimate, or gives EMFDTD_BIN)."""
    import os

    if os.environ.get("EMFDTD_BIN"):
        return os.environ["EMFDTD_BIN"]
    try:
        from .. import launcher
    except ImportError:
        launcher = None
    if launcher is not None:
        return launcher.find_solver(Path(__file__).resolve().parents[3])
    from . import agent

    cfg = agent.load()
    if cfg and cfg.get("solver"):
        return cfg["solver"]
    raise RuntimeError("no solver to run a dry run with (set EMFDTD_BIN to the emfdtd program)")
