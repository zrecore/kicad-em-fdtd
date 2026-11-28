"""A job is data, never a command line. The worker builds the solver's argv itself from a validated record:
a fixed binary, allow-listed settings, nothing that reaches outside the job directory."""

import math
import re

TIERS = ("essential", "common", "advanced", "expert")
DENIED_SETTINGS = {"backend"}  # the worker always forces the GPU backend
SAFE_WORD = re.compile(r"^[A-Za-z0-9_.+:-]{1,64}$")
PROFILE = re.compile(r"^[a-z0-9_]{1,32}(:[a-z0-9_]{1,32})?$")
REF = re.compile(r"^[A-Za-z0-9_.]{1,32}$")
GEOM_KEY = re.compile(r"^[A-Za-z0-9_.+/-]{1,64}\.[xy]$")
PART_TREATMENT = re.compile(r"^[a-z0-9.,]{1,40}$")
BODY_TREATMENT = re.compile(r"^(ignore|metal|dielectric:[0-9.]+(:[0-9.]+)?)$")
DEVICE = re.compile(r"^[0-9a-fA-F]{4,32}$|^[0-9]{1,3}$")
PIN = re.compile(r"^[A-Za-z0-9_.-]{1,40}(:[A-Za-z0-9 _.+-]{1,60})?$")
# Fields the scheduler uses and the worker never sees (an older worker would refuse an unknown field)
SCHEDULING_HINTS = ("estimate", "pin")
MAX_CELLS = 5e9
MAX_ESTIMATE_STEPS = 1e8


class JobError(Exception):
    pass


def megabytes(n):
    """A size for a message, in MB of 2^20 bytes (the unit of the policy's limits): 256 MB, 0.2 MB."""
    mb = n / (1 << 20)
    return f"{mb:.1f} MB" if mb < 10 else f"{mb:.0f} MB"


def parse_list_settings(text):
    """{key: tier} from the solver's own `--list-settings` (the allow-list cannot drift from the solver)."""
    out = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[1] in TIERS:
            out[parts[0]] = parts[1]
    return out


def _number(value, what):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise JobError(f"{what} must be a finite number")
    return value


def _word(value, what, pattern=SAFE_WORD):
    if not isinstance(value, str) or not pattern.match(value) or value.startswith("-"):
        raise JobError(f"{what} has a value that is not allowed")
    return value


def _net(value):
    if (
        not isinstance(value, str)
        or not 1 <= len(value) <= 128
        or value.startswith("-")
        or "," in value
        or any(ord(c) < 32 or ord(c) == 127 for c in value)
    ):
        raise JobError("a net name is not allowed")
    return value


def validate(spec, tiers, limits):  # noqa: C901  (complexity ratchet: split when next touched)
    """The normalised job, or JobError. `tiers` is parse_list_settings() of the worker's solver."""
    if not isinstance(spec, dict):
        raise JobError("a job must be an object")
    known = {"profile", "sets", "geoms", "refine", "parts", "bodies", "nets", "ground_nets", "keep_fields"}
    known |= set(SCHEDULING_HINTS)
    extra = set(spec) - known
    if extra:
        raise JobError("unknown job field(s): {}".format(", ".join(sorted(extra))))
    out = {}
    if "profile" in spec:
        out["profile"] = _word(spec["profile"], "profile", PROFILE)
    cap = TIERS.index(limits.get("max_tier", "advanced"))
    sets = {}
    for k, v in (spec.get("sets") or {}).items():
        if k in DENIED_SETTINGS or not SAFE_WORD.match(k):
            raise JobError(f"setting {k!r} is not allowed")
        if tiers is not None:  # the worker checks against its own solver; the coordinator only the shape
            if k not in tiers:
                raise JobError(f"setting {k!r} is not allowed")
            if TIERS.index(tiers[k]) > cap:
                raise JobError(f"setting {k!r} is above this device's tier")
        sets[k] = _number(v, "setting " + k) if not isinstance(v, str) else _word(v, "setting " + k)
    if (
        "max_steps" in sets
        and not isinstance(sets["max_steps"], str)
        and sets["max_steps"] > limits["max_steps"]
    ):
        raise JobError(f"max_steps exceeds the limit of {limits['max_steps']}")
    out["sets"] = sets
    out["geoms"] = {}
    for k, v in (spec.get("geoms") or {}).items():
        if not GEOM_KEY.match(k) or k.startswith("-"):
            raise JobError("a geometry key is not allowed")
        out["geoms"][k] = _number(v, "geometry " + k)
    out["refine"] = []
    for box in spec.get("refine") or []:
        if not isinstance(box, list) or len(box) != 5:
            raise JobError("a refine box is five numbers")
        out["refine"].append([_number(x, "refine box") for x in box])
    out["parts"] = {
        _word(r, "part reference", REF): _word(t, "part treatment", PART_TREATMENT)
        for r, t in (spec.get("parts") or {}).items()
    }
    out["bodies"] = {
        _word(r, "body reference", REF): _word(t, "body treatment", BODY_TREATMENT)
        for r, t in (spec.get("bodies") or {}).items()
    }
    out["nets"] = [_net(n) for n in spec.get("nets") or []]
    out["ground_nets"] = [_net(n) for n in spec.get("ground_nets") or []]
    if "keep_fields" in spec:  # asked for per job: the field data comes back with the result (docs/farm.md)
        if not isinstance(spec["keep_fields"], bool):
            raise JobError("keep_fields must be true or false")
        if spec["keep_fields"]:  # false is the default and is not recorded: a job without it is unchanged
            out["keep_fields"] = True
    if "estimate" in spec:
        out["estimate"] = _estimate(spec["estimate"])
    if "pin" in spec:
        out["pin"] = _word(spec["pin"], "pin", PIN)
    return out


def _estimate(value):
    """{"cells", "steps"}: how big the job is expected to be, for placement only (the worker enforces its own
    ceilings whatever this says)."""
    if not isinstance(value, dict) or set(value) - {
        "cells",
        "steps",
        "bound_steps",
        "memory_bytes",
        "field_bytes",
    }:
        raise JobError("an estimate is {cells, steps}")
    out = {}
    for key, cap in (
        ("cells", MAX_CELLS),
        ("steps", MAX_ESTIMATE_STEPS),
        ("bound_steps", MAX_ESTIMATE_STEPS),
        ("memory_bytes", 1 << 46),
        ("field_bytes", 1 << 46),
    ):
        if key in value:
            n = _number(value[key], "estimate " + key)
            if not 0 < n <= cap:
                raise JobError(f"estimate {key} is outside the allowed range")
            out[key] = n
    if "cells" not in out or "steps" not in out:
        raise JobError("an estimate is {cells, steps}")
    return out


def worker_spec(spec):
    """The job as the worker gets it: without the hints only the scheduler reads."""
    return {k: v for k, v in spec.items() if k not in SCHEDULING_HINTS}


def build_argv(solver, snapshot, job, project_dir, device, checkpoint=None):
    """The full command line. Every element is its own argv entry (no shell). The job is validated first.
    `checkpoint` is (folder, seconds between checkpoints, resume?) chosen by the worker, never by the job."""
    if not DEVICE.match(str(device)):
        raise JobError("the device selector is not allowed")
    argv = [solver, str(snapshot)]
    if "profile" in job:
        argv += ["--profile", job["profile"]]
    for k, v in sorted(job["sets"].items()):
        argv += ["--set", f"{k}={v}"]
    for k, v in sorted(job["geoms"].items()):
        argv += ["--geom", f"{k}={v}"]
    for box in job["refine"]:
        argv += ["--refine"] + [f"{x:g}" for x in box]
    for r, t in sorted(job["parts"].items()):
        argv += ["--part", f"{r}={t}"]
    for r, t in sorted(job["bodies"].items()):
        argv += ["--body", f"{r}={t}"]
    if job["nets"]:
        argv += ["--nets", ",".join(job["nets"])]
    if job["ground_nets"]:
        argv += ["--ground-nets", ",".join(job["ground_nets"])]
    if job.get("keep_fields"):
        argv.append("--keep-fields")
    argv += ["--gpu", "--device", str(device), "--project-dir", str(project_dir)]
    if checkpoint:
        folder, every, resume = checkpoint
        argv += ["--checkpoint-dir", str(folder), "--checkpoint-every", f"{every:g}"]
        if resume:
            argv.append("--resume")
    return argv


def expand_sweep(job, key, values):
    """One single-point job per value (a settings key, or `geom.NET.x` for a geometry stretch)."""
    points = []
    for v in values:
        p = {**job, "sets": dict(job.get("sets", {})), "geoms": dict(job.get("geoms", {}))}
        if key.startswith("geom."):
            p["geoms"][key[5:]] = v
        else:
            p["sets"][key] = v
        points.append(p)
    return points
