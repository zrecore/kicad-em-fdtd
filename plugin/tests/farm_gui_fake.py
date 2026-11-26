"""A scripted stand-in for `emfdtd_farm.py agent ...` for the agent window's tests (no real agent, no
sockets).

It keeps a little state (running or not, saved settings, autostart) and answers the commands the window uses,
with the JSON shapes of the agent contract. `calls` records every command line (the one-time code redacted).
"""

import json

from emfdtd_plugin.farm.gui.controller import Result, redact

DOCTOR_OK = [
    {"id": "tls13", "title": "Python has TLS 1.3", "ok": True, "detail": "OpenSSL 3", "fix": ""},
    {"id": "role", "title": "The worker role is on", "ok": True, "detail": "", "fix": ""},
]
DOCTOR_BAD = DOCTOR_OK + [
    {"id": "coordinator", "title": "The coordinator is reachable", "ok": False,
     "detail": "connection refused", "fix": "Start the coordinator or check its address."},
]  # fmt: skip


class FakeAgent:
    def __init__(self):
        self.running = False
        self.job = None
        self.calls = []
        self.doctor_checks = DOCTOR_OK
        self.autostart = False
        self.settings = {
            "solver": "/opt/emfdtd/emfdtd", "devices": ["RTX 4060 Ti"],
            "turn": {"mode": "lock", "lock_command": ["/x/lock"]},
            "max_minutes": 120, "max_steps": 1000000, "max_input_mb": 64, "max_tier": "advanced",
            "watchdog": 20.0, "work": None, "log": None,
        }  # fmt: skip
        self.fail = {}  # command prefix text -> message: makes that command fail
        self.enrolled = False
        self.saved_documents = []  # every document sent to `agent configure --json-in`
        self.log_text = "2026-11-26 10:00:00 worker connected\n"

    def __call__(self, args, stdin=None, timeout=60):
        self.calls.append(redact(list(args)))
        key = " ".join(args[:2])
        for prefix, message in self.fail.items():
            if " ".join(args).startswith(prefix):
                return Result(1, "", message)
        a = list(args)
        if a[:3] == ["agent", "status", "--json"]:
            return Result(0, json.dumps(self.status()))
        if a[:2] == ["agent", "start"]:
            self.running = True
            return Result(0, "agent started (pid 4242)")
        if a[:2] == ["agent", "stop"]:
            self.running, self.job = False, None
            return Result(0, "agent stopped")
        if a[:2] == ["agent", "restart"]:
            self.running = True
            return Result(0, "agent restarted")
        if a[:3] == ["agent", "doctor", "--json"]:
            ok = all(c["ok"] for c in self.doctor_checks)
            return Result(0 if ok else 1, json.dumps({"ok": ok, "checks": self.doctor_checks}))
        if a[:3] == ["agent", "configure", "--show"]:
            return Result(0, json.dumps(self.settings))
        if a[:3] == ["agent", "configure", "--json-in"]:
            new = json.loads(stdin)
            self.saved_documents.append(new)
            self.settings = {**self.settings, **new}
            return Result(0, "saved /farm/agent.json")
        if a[:3] == ["agent", "autostart", "status"]:
            return Result(
                0,
                json.dumps({"enabled": self.autostart, "mechanism": "fake", "detail": "managed by the fake"}),
            )
        if a[:2] == ["agent", "autostart"] and len(a) > 2 and a[2] in ("on", "off"):
            self.autostart = a[2] == "on"
            return Result(0, f"autostart {a[2]}")
        if a[:2] == ["agent", "logs"]:
            return Result(0, self.log_text)
        if a[:2] == ["enable", "worker"]:
            return Result(0, "farm worker is on")
        if a[0] == "enroll":
            self.enrolled = True
            return Result(0, "enrolled as worker win-egpu. Next: emfdtd-farm worker")
        if a[0] == "devices":
            return Result(0, "recorded 1 device(s)")
        return Result(2, "", f"unknown command {key}")

    def status(self):
        d = {"running": self.running, "pid": 4242 if self.running else None,
             "started": 1.0 if self.running else None, "uptime_s": 3725 if self.running else None,
             "connected": True if self.running else None,
             "coordinator": "192.168.1.20:7420" if self.running else "",
             "job": self.job, "device": "NVIDIA GeForce RTX 4060 Ti" if self.running else "",
             "last_error": "", "log": "/farm/agent.log"}  # fmt: skip
        return d
