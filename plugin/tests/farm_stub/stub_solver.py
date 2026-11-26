#!/usr/bin/env python3
# ruff: noqa
"""A stand-in for emfdtd, enough for the farm tests: --list-devices, --list-settings, --dry-run, and a run that
writes the results folder. Behaviour switches come from the snapshot file's "stub" key."""

import json
import os
import sys
import time

args = sys.argv[1:]
if args[:1] == ["--list-devices"]:
    print(
        "0  Stub GPU A  uuid aaaa1111bbbb2222cccc3333dddd4444  discrete  8.0 GiB  vulkan 1.4  driver Stub 1.0"
    )
    print(
        "1  Stub GPU B  uuid eeee5555ffff6666aaaa7777bbbb8888  discrete  8.0 GiB  vulkan 1.4  driver Stub 1.0"
    )
    print(
        "2  Old GPU  uuid 0000000000000000000000000000cafe  discrete  1.0 GiB  vulkan 1.1  driver Old  (below Vulkan 1.2: not usable)"
    )
    sys.exit(0)
if args[:1] == ["--version"]:
    print("emfdtd stub 0.0.0")
    sys.exit(0)
if args[:1] == ["--list-settings"]:
    print("mesh_preset                    essential  medium  [Mesh] Coarse is fast")
    print("decay_threshold                advanced  0.001  [Run] stop at")
    print("max_steps                      advanced  400000  [Run] cap")
    print("backend                        common  auto  [Run] backend")
    print("expert_knob                    expert  1  [Run] expert")
    sys.exit(0)

snapshot = args[0]
opts = {}
i = 1
while i < len(args):
    a = args[i]
    if a in (
        "--set",
        "--part",
        "--body",
        "--geom",
        "--nets",
        "--ground-nets",
        "--profile",
        "--device",
        "--project-dir",
        "--checkpoint-dir",
        "--checkpoint-every",
    ):
        opts.setdefault(a, []).append(args[i + 1])
        i += 2
    elif a == "--refine":
        i += 6
    else:
        opts.setdefault(a, []).append(True)
        i += 1
if "--help" in sys.argv[1:]:  # the worker reads this to see whether the solver can checkpoint
    print("usage: stub_solver [--checkpoint-dir DIR [--resume]] ...")
    sys.exit(0)
stub = json.load(open(snapshot)).get("stub", {})
if "--dry-run" in opts:
    print("the farm must not call --dry-run (it repeats the whole preparation)", file=sys.stderr)
    sys.exit(9)
if "--gpu" not in opts:
    print("emfdtd: stub requires --gpu", file=sys.stderr)
    sys.exit(2)
if stub.get("reject"):
    print("emfdtd: unknown setting", file=sys.stderr, flush=True)
    sys.exit(7)
print("backend: GPU (Stub)", flush=True)
pid_file = stub.get("pid_file")
if pid_file:
    open(pid_file, "w").write(str(os.getpid()))

# Checkpoints (the real solver: docs/checkpoint.md): --checkpoint-dir DIR [--resume]; a DIR/stop file makes the run
# write a checkpoint, print "paused: checkpoint at step N" and exit 75. The stub's state is just the line number.
import hashlib

ckdir = (opts.get("--checkpoint-dir") or [None])[0]
ident = hashlib.sha256(open(snapshot, "rb").read()).hexdigest()[:16]
start = 0
resumed_at = None


def write_checkpoint(step):
    names = ("ckpt.a", "ckpt.b")
    old = [
        (json.load(open(os.path.join(ckdir, n)))["step"], n)
        for n in names
        if os.path.exists(os.path.join(ckdir, n))
    ]
    target = names[0] if not old else (names[1] if max(old)[1] == names[0] else names[0])
    with open(os.path.join(ckdir, target), "w") as f:
        json.dump({"hash": ident, "step": step}, f)


if ckdir and "--resume" in opts:
    found = []
    for n in ("ckpt.a", "ckpt.b"):
        path = os.path.join(ckdir, n)
        try:
            found.append((json.load(open(path)), n))
        except (OSError, ValueError):
            continue
    if not found:
        print(f"emfdtd: the checkpoint in {ckdir} is missing", file=sys.stderr, flush=True)
        sys.exit(1)
    best, name = max(found, key=lambda x: x[0]["step"])
    if best["hash"] != ident:
        print(
            f"emfdtd: checkpoint {name} was written for other inputs (the board, the settings or the "
            "options differ)",
            file=sys.stderr,
            flush=True,
        )
        sys.exit(1)
    start = resumed_at = best["step"]
    try:
        os.remove(os.path.join(ckdir, "stop"))
    except OSError:
        pass
    print(f"resuming from the checkpoint at step {start}", flush=True)
elif ckdir:
    for n in os.listdir(ckdir):
        os.remove(os.path.join(ckdir, n))  # a fresh run clears the files of an earlier run
every = int(stub.get("checkpoint_lines", 0))
for n in range(start, int(stub.get("lines", 2))):
    if ckdir and os.path.exists(os.path.join(ckdir, "stop")):
        write_checkpoint(n)
        print(f"paused: checkpoint at step {n}", flush=True)
        sys.exit(75)
    print(f"step {n}", flush=True)
    time.sleep(float(stub.get("sleep", 0.0)))
    if ckdir and every and (n + 1) % every == 0:
        write_checkpoint(n + 1)
        print(f"checkpoint: step {n + 1} written (0 MB, 0.0 s)", flush=True)
if stub.get("crash"):
    print("boom", file=sys.stderr, flush=True)
    sys.exit(3)
if stub.get("hang"):
    time.sleep(600)
project = opts["--project-dir"][0]
run = os.path.join(project, "em-fdtd", "results", "run-20260101-000000")
os.makedirs(run, exist_ok=True)
open(os.path.join(run, "s11.s1p"), "w").write("# Hz S RI R 50\n2.4e9 -0.1 0.0\n")
open(os.path.join(run, "summary.txt"), "w").write(
    "device "
    + opts["--device"][0]
    + "\n"
    + (f"resumed at step {resumed_at}\n" if resumed_at is not None else "")
)
open(os.path.join(run, "settings.json"), "w").write(json.dumps({"set": opts.get("--set", [])}))
print("12448 steps of 1.03 ps in 3.2 s (1185 Mcells/s)", flush=True)
print("best match -6.043 dB at 2.2195 GHz", flush=True)
