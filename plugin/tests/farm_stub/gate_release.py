#!/usr/bin/env python3
# ruff: noqa
"""A stand-in for `win_gpu_lock.sh release`."""

import sys
import time

log, label, flag = sys.argv[1:4]
with open(log, "a") as f:
    f.write(f"release {label} {time.time():.3f}\n")
open(flag, "w").close()
