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
    ):
        opts.setdefault(a, []).append(args[i + 1])
        i += 2
    elif a == "--refine":
        i += 6
    else:
        opts.setdefault(a, []).append(True)
        i += 1
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
for n in range(int(stub.get("lines", 2))):
    print(f"step {n}", flush=True)
    time.sleep(float(stub.get("sleep", 0.0)))
if stub.get("crash"):
    print("boom", file=sys.stderr, flush=True)
    sys.exit(3)
if stub.get("hang"):
    time.sleep(600)
project = opts["--project-dir"][0]
run = os.path.join(project, "em-fdtd", "results", "run-20260101-000000")
os.makedirs(run)
open(os.path.join(run, "s11.s1p"), "w").write("# Hz S RI R 50\n2.4e9 -0.1 0.0\n")
open(os.path.join(run, "summary.txt"), "w").write("device " + opts["--device"][0] + "\n")
open(os.path.join(run, "settings.json"), "w").write(json.dumps({"set": opts.get("--set", [])}))
print("12448 steps of 1.03 ps in 3.2 s (1185 Mcells/s)", flush=True)
print("best match -6.043 dB at 2.2195 GHz", flush=True)
