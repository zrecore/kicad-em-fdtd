#!/usr/bin/env python3
# ruff: noqa
"""A stand-in for a GPU lock: waits DELAY seconds (a busy GPU), logs the command it is asked to run, then runs it.
lockwrap.py LOG DELAY CMD..."""

import os
import sys
import time

log, delay, cmd = sys.argv[1], float(sys.argv[2]), sys.argv[3:]
with open(log + ".pid", "w") as f:
    f.write(str(os.getpid()))
time.sleep(delay)
with open(log, "a") as f:
    f.write(" ".join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd)) + "\n")
os.execv(cmd[0], cmd)
