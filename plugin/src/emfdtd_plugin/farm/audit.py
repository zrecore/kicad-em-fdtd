"""An append-only record of every decision the coordinator makes: who asked for what, and the answer."""

import json
import os
import time

from . import config

MAX_BYTES = 10 * 1024 * 1024


def _file():
    return config.home() / "audit.log"


def record(identity, action, allowed, job=None, reason=""):
    path = _file()
    config.ensure_home()
    try:
        if path.stat().st_size > MAX_BYTES:
            os.replace(str(path), str(path) + ".1")
    except OSError:
        pass
    line = {
        "t": round(time.time(), 3),
        "who": "{}:{}".format(*identity) if identity else None,
        "action": action,
        "allowed": bool(allowed),
        "job": job,
        "reason": reason,
    }
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    with os.fdopen(fd, "a") as f:
        f.write(json.dumps(line) + "\n")
