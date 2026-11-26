#!/usr/bin/env python3
# ruff: noqa
"""A stand-in for `win_gpu_lock.sh hold`: waits, prints HOLDING, keeps the turn until released or killed
(or, with a 4th argument, until that many seconds have passed: a hold command that ends on its own)."""

import os
import sys
import time

log, label, flag = sys.argv[1:4]
lifetime = float(sys.argv[4]) if len(sys.argv) > 4 else None
with open(log, "a") as f:
    f.write(f"hold {label} {time.time():.3f}\n")
time.sleep(0.4)
print("HOLDING", flush=True)
start = time.time()
while not os.path.exists(flag):
    if lifetime is not None and time.time() - start > lifetime:
        sys.exit(0)
    time.sleep(0.1)
# released: a real lock script removes its own state here (it cannot if it is killed first)
with open(log, "a") as f:
    f.write(f"hold-exit {label} {time.time():.3f}\n")
