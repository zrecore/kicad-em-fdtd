"""Everything the agent window decides, with no tkinter in it (so it is tested headless).

The window drives the farm agent ONLY through the command line (`emfdtd_farm.py agent ...`), run as a child
process through one runner function; tests pass a fake runner. Nothing here opens a socket.
"""

from __future__ import annotations

import json
import os
import platform
import shlex
import ssl
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

TIERS = ("essential", "common", "advanced", "expert")
REFRESH_SECONDS = 2.0
DEFAULT_TIMEOUT = 60.0

# The three ways a worker takes its GPU turn, in words for someone who has never read the design notes.
TURN_MODES = (
    (
        "lock",
        "Wait for my GPU's turn",
        "Another program on this machine may use the same GPU, so every GPU call goes through a lock command "
        "that waits for its turn first.",
    ),
    (
        "coordinator",
        "The coordinator holds the turn",
        "The GPU's turn lives on another machine; the coordinator takes it before it sends a job and "
        "gives it back after, and this worker refuses jobs that do not say so.",
    ),
    (
        "none",
        "Nobody else uses this GPU",
        "This GPU is only used by the farm, so no turn is needed. Choose this only if that is really true.",
    ),
)
TURN_IDS = tuple(t[0] for t in TURN_MODES)


# --- running the command line ---------------------------------------------------------------------


@dataclass
class Result:
    code: int = 0
    out: str = ""
    err: str = ""

    @property
    def ok(self):
        return self.code == 0

    def text(self):
        """What to show a person: the tool's message, whichever stream it used."""
        return (
            self.out.strip() or self.err.strip() or ("done" if self.ok else f"failed (exit {self.code})")
        ).strip()


def default_tool():
    """plugin/tools/emfdtd_farm.py, found relative to this file (works inside the packaged bundle too)."""
    return Path(__file__).resolve().parents[4] / "tools" / "emfdtd_farm.py"


def make_runner(tool=None, python=None):
    """A runner(args, stdin=None, timeout=60) -> Result that runs [python, tool, *args] with no shell."""
    tool = Path(tool) if tool else default_tool()
    python = python or sys.executable

    def run(args, stdin=None, timeout=DEFAULT_TIMEOUT):
        try:
            p = subprocess.run(
                [python, str(tool), *args],
                input=stdin,
                capture_output=True,
                text=True,
                timeout=timeout,
                stdin=None if stdin is not None else subprocess.DEVNULL,
            )
        except subprocess.TimeoutExpired:
            return Result(124, "", f"the farm tool did not finish within {int(timeout)} s")
        except OSError as e:
            return Result(127, "", f"cannot run the farm tool: {e}")
        return Result(p.returncode, p.stdout, p.stderr)

    return run


def redact(args):
    """A command line safe to keep or show: the one-time enrolment code is never written anywhere."""
    out, hide = [], False
    for a in args:
        out.append("********" if hide else a)
        hide = a == "--code"
    return out


# --- status ---------------------------------------------------------------------------------------


@dataclass
class Job:
    id: str = ""
    label: str = ""
    phase: str = ""
    percent: int | None = None


@dataclass
class Status:
    running: bool = False
    pid: int | None = None
    uptime_s: float | None = None
    connected: bool | None = None
    coordinator: str = ""
    job: Job | None = None
    device: str = ""
    last_error: str = ""
    log: str = ""
    error: str = ""  # why the status itself could not be read


def _num(v):
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def parse_status(text):
    """Status from `agent status --json`; anything unreadable becomes a Status that says so."""
    try:
        d = json.loads(text)
        if not isinstance(d, dict):
            raise ValueError("not an object")
    except ValueError as e:
        return Status(error=f"the agent status was not understood ({e})")
    job = None
    j = d.get("job")
    if isinstance(j, dict):
        pct = _num(j.get("percent"))
        job = Job(str(j.get("id", "")), str(j.get("label", "")), str(j.get("phase", "")),
                  None if pct is None else max(0, min(100, int(pct))))  # fmt: skip
    pid = _num(d.get("pid"))
    return Status(
        running=bool(d.get("running")),
        pid=None if pid is None else int(pid),
        uptime_s=_num(d.get("uptime_s")),
        connected=d.get("connected") if isinstance(d.get("connected"), bool) else None,
        coordinator=str(d.get("coordinator") or ""),
        job=job,
        device=str(d.get("device") or ""),
        last_error=str(d.get("last_error") or ""),
        log=str(d.get("log") or ""),
    )


def format_uptime(seconds):
    if seconds is None:
        return "unknown"
    s = int(seconds)
    if s < 60:
        return f"{s} s"
    m = s // 60
    if m < 60:
        return f"{m} min"
    h, m = divmod(m, 60)
    if h < 48:
        return f"{h} h {m} min"
    return f"{h // 24} days {h % 24} h"


def describe(st):
    """The status panel's text, every fact in words (colour is never the only carrier)."""
    if st.error:
        return {
            "state": "Unknown",
            "uptime": "",
            "connection": "",
            "job": "",
            "device": "",
            "problem": st.error,
            "percent": None,
        }
    connection = {True: "Connected to the coordinator", False: "Not connected to the coordinator", None: ""}[
        st.connected
    ]
    if st.running and st.coordinator and st.connected is not None:
        connection += f" ({st.coordinator})"
    job = "No job right now"
    if st.job:
        job = f"Job: {st.job.label or st.job.id[:8]}"
        if st.job.phase:
            job += f" - {st.job.phase}"
        if st.job.percent is not None:
            job += f" (about {st.job.percent}%)"
    if not st.running:
        job = ""
        connection = ""
    return {
        "state": "Running" if st.running else "Stopped",
        "uptime": f"Running for {format_uptime(st.uptime_s)}" if st.running else "",
        "connection": connection,
        "job": job,
        "device": f"GPU: {st.device}" if st.device and st.running else "",
        "problem": f"Last problem: {st.last_error}" if st.last_error else "",
        "percent": st.job.percent if st.job else None,
    }


# --- checks ---------------------------------------------------------------------------------------


@dataclass
class Check:
    id: str
    title: str
    ok: bool
    detail: str = ""
    fix: str = ""


@dataclass
class DoctorReport:
    ok: bool = False
    checks: list = field(default_factory=list)
    error: str = ""


def parse_doctor(text):
    try:
        d = json.loads(text)
        rows = d["checks"]
        checks = [
            Check(str(c.get("id", "")), str(c.get("title", "")), bool(c.get("ok")), str(c.get("detail", "")),
                  str(c.get("fix", "")))
            for c in rows
        ]  # fmt: skip
        return DoctorReport(bool(d.get("ok")) and all(c.ok for c in checks), checks)
    except (ValueError, KeyError, TypeError, AttributeError) as e:
        return DoctorReport(False, [], f"the checks were not understood ({e})")


def check_word(c):
    return "OK" if c.ok else "Problem"


# --- settings -------------------------------------------------------------------------------------


@dataclass
class Settings:
    solver: str = ""
    device: str = ""
    turn: str = "lock"
    lock_command: str = ""  # one command line, split on spaces by the tool
    max_minutes: str = "120"
    max_steps: str = "1000000"
    max_input_mb: str = "64"
    max_tier: str = "advanced"
    watchdog: str = "20"
    work: str = ""
    recorded_devices: list = field(
        default_factory=list
    )  # names saved by `devices --save` (read from its file)

    def to_json(self):
        """The document `agent configure --json-in` takes (the agent's own shape; it refuses unknown keys):
        {solver, devices[], turn{mode, lock_command[]}, max_minutes, max_steps, max_input_mb, max_tier,
        watchdog, work?}. Numbers are numbers; a blank folder is left out."""
        lock = _split_command(self.lock_command) if self.turn == "lock" else []
        d = {
            "solver": self.solver.strip(),
            "devices": [x.strip() for x in self.device.split(",") if x.strip()],
            "turn": {"mode": self.turn, "lock_command": lock},
            "max_minutes": _to_number(self.max_minutes),
            "max_steps": _to_number(self.max_steps),
            "max_input_mb": _to_number(self.max_input_mb),
            "max_tier": self.max_tier,
            "watchdog": _to_number(self.watchdog),
        }
        if self.work.strip():
            d["work"] = self.work.strip()
        return d

    @classmethod
    def from_json(cls, d):
        def s(k, default=""):
            v = d.get(k, default)
            return default if v is None else str(v)

        turn = d.get("turn")
        mode = turn.get("mode") if isinstance(turn, dict) else None
        lock = turn.get("lock_command", []) if isinstance(turn, dict) else []
        devices = d.get("devices", d.get("device", ""))
        if isinstance(devices, list):
            devices = ", ".join(str(x) for x in devices)
        if isinstance(lock, list):
            lock = " ".join(shlex.quote(str(x)) if " " in str(x) else str(x) for x in lock)
        return cls(
            solver=s("solver"),
            device=str(devices or ""),
            turn=mode if mode in TURN_IDS else "lock",
            lock_command=str(lock or ""),
            max_minutes=s("max_minutes", "120"),
            max_steps=s("max_steps", "1000000"),
            max_input_mb=s("max_input_mb", "64"),
            max_tier=s("max_tier", "advanced") if s("max_tier", "advanced") in TIERS else "advanced",
            watchdog=s("watchdog", "20"),
            work=s("work"),
        )


def _split_command(text):
    """A command line as the list the agent stores (quotes respected; Windows backslashes are kept)."""
    try:
        return shlex.split(text, posix=os.name != "nt")
    except ValueError:
        return text.split()


def _to_number(text):
    t = str(text).strip()
    try:
        f = float(t)
    except ValueError:
        return t
    return int(f) if f == int(f) else f


def validate_settings(s):
    """Problems a person can fix, in plain words (the tool validates again when saving)."""
    problems = []
    if not s.solver.strip():
        problems.append("Choose the emfdtd program (the solver) this worker runs.")
    if not s.device.strip():
        problems.append("Choose which GPU to offer (its name, or the start of its UUID).")
    if s.turn not in TURN_IDS:
        problems.append("Choose how this worker takes its GPU's turn.")
    if s.turn == "lock" and not s.lock_command.strip():
        problems.append("Give the lock command that waits for your GPU's turn, or choose another way.")
    for label, value in (
        ("Longest job (minutes)", s.max_minutes),
        ("Most steps", s.max_steps),
        ("Largest input (MB)", s.max_input_mb),
        ("Silence before the solver is stopped (seconds)", s.watchdog),
    ):
        n = _to_number(value)
        if not isinstance(n, (int, float)) or n <= 0:
            problems.append(f"{label} must be a number above zero.")
    if s.max_tier not in TIERS:
        problems.append("Choose a settings level.")
    return problems


# --- autostart ------------------------------------------------------------------------------------


def autostart_description(system=None):
    """What switching on 'start when I log in' installs, and where, for this operating system."""
    system = system or platform.system()
    if system == "Darwin":
        return (
            "a launchd agent (a small file in ~/Library/LaunchAgents) that starts the farm agent when you "
            "log "
            "in. Stopping the agent from here stays stopped until the next log in."
        )
    if system == "Windows":
        return (
            "a scheduled task for your account (Task Scheduler) that starts the farm agent when you log in. "
            "It needs no administrator rights."
        )
    return (
        "a systemd user unit (a small file in ~/.config/systemd/user) that starts the farm agent when you "
        "log in. It needs no administrator rights."
    )


@dataclass
class Autostart:
    enabled: bool = False
    detail: str = ""
    error: str = ""
    mechanism: str = ""


def parse_autostart(text):
    try:
        d = json.loads(text)
        return Autostart(bool(d.get("enabled")), str(d.get("detail", "")), "", str(d.get("mechanism", "")))
    except (ValueError, AttributeError) as e:
        return Autostart(False, "", f"the autostart status was not understood ({e})")


# --- refresh scheduling ---------------------------------------------------------------------------


class Refresher:
    """Decides when to refresh: only while active (window open and, for the log, its tab showing)."""

    def __init__(self, interval=REFRESH_SECONDS, clock=time.monotonic):
        self.interval = interval
        self.clock = clock
        self.active = False
        self._last = None

    def start(self):
        self.active = True
        self._last = None  # refresh at once

    def stop(self):
        self.active = False

    def due(self):
        if not self.active:
            return False
        now = self.clock()
        if self._last is None or now - self._last >= self.interval:
            self._last = now
            return True
        return False


# --- the controller -------------------------------------------------------------------------------


def recorded_device_names(work=None):
    """The GPUs `devices --save` recorded on this machine (names), read from its file; [] if none."""
    try:
        from .. import config

        base = Path(work) if work else config.home() / "work"
        rows = json.loads((base / "devices.json").read_text())
        return [str(r.get("name") or r.get("uuid", "")) for r in rows if isinstance(r, dict)]
    except (OSError, ValueError, ImportError, AttributeError):
        return []


class Controller:
    def __init__(self, runner=None, recorded=None):
        self.runner = runner or make_runner()
        self.recorded_devices = recorded or recorded_device_names
        self.history = []  # (redacted command, exit code): for 'copy diagnostics', never contains a code

    def _run(self, args, stdin=None, timeout=DEFAULT_TIMEOUT):
        res = self.runner(args, stdin, timeout)
        self.history.append((redact(args), res.code))
        del self.history[:-50]
        return res

    def status(self):
        res = self._run(["agent", "status", "--json"], timeout=15)
        if res.out.strip():
            return parse_status(res.out)
        return Status(error=res.text())

    def start(self):
        return self._run(["agent", "start"], timeout=120)

    def stop_needs_confirmation(self, st):
        """Stopping a worker that has a job cancels it (it is requeued): ask first, and say so."""
        if st is not None and st.running and st.job is not None:
            label = st.job.label or st.job.id[:8] or "a job"
            return (
                f"The agent is running {label}. Stopping it cancels that job here; the farm will put it back "
                "in its queue and run it again on a free GPU. Stop the agent anyway?"
            )
        return None

    def stop(self):
        return self._run(["agent", "stop"], timeout=120)

    def restart(self):
        return self._run(["agent", "restart"], timeout=180)

    def doctor(self):
        res = self._run(["agent", "doctor", "--json"], timeout=90)
        if res.out.strip():
            return parse_doctor(res.out)
        return DoctorReport(False, [], res.text())

    def load_settings(self):
        """(Settings, message). No saved settings yet is normal on a new machine: defaults and no message."""
        res = self._run(["agent", "configure", "--show", "--json"], timeout=15)
        names = self.recorded_devices()
        try:
            d = json.loads(res.out)
        except ValueError:
            return Settings(recorded_devices=names), ("" if res.ok and not res.out.strip() else res.text())
        if not isinstance(d, dict):
            return Settings(recorded_devices=names), ""
        if "error" in d:
            message = str(d["error"])
            return Settings(recorded_devices=names), ("" if "no saved configuration" in message else message)
        s = Settings.from_json(d)
        s.recorded_devices = names
        return s, ""

    def save_settings(self, settings):
        problems = validate_settings(settings)
        if problems:
            return Result(2, "", "\n".join(problems))
        return self._run(
            ["agent", "configure", "--json-in"], stdin=json.dumps(settings.to_json()), timeout=30
        )

    def autostart(self):
        res = self._run(["agent", "autostart", "status", "--json"], timeout=15)
        if res.out.strip():
            return parse_autostart(res.out)
        return Autostart(False, "", res.text())

    def set_autostart(self, on):
        return self._run(["agent", "autostart", "on" if on else "off"], timeout=60)

    def log(self, lines=200):
        res = self._run(["agent", "logs", "-n", str(int(lines))], timeout=15)
        if res.out.strip():
            return res.out
        return "(the log is empty)" if res.ok else res.text()


# --- the set-up wizard ---------------------------------------------------------------------------------

STEPS = ("python", "role", "enroll", "devices", "configure", "doctor", "start")
STEP_TITLES = {
    "python": "Check Python",
    "role": "Turn the worker role on",
    "enroll": "Join the farm",
    "devices": "Record this machine's GPUs",
    "configure": "Save the settings",
    "doctor": "Check everything",
    "start": "Start the agent",
}
STEP_HELP = {
    "python": ("The farm needs Python with TLS 1.3 (encrypted connections that cannot be downgraded)."),
    "role": (
        "Turning the worker role on lets this machine connect OUT to a coordinator you enrol it "
        "with and run the solver on its GPU for that coordinator's jobs. It opens no port."
    ),
    "enroll": (
        "Type what the owner of the farm gave you: where the invitation is listening, the "
        "coordinator's fingerprint, the one-time code, and the coordinator's address. The code "
        "works once and is not saved."
    ),
    "devices": (
        "This makes one GPU call to list the GPUs. If the GPU is shared with other programs, do "
        "it while you hold the GPU's turn."
    ),
    "configure": ("Saves the solver path, the GPU and how this worker takes its turn (the Settings tab)."),
    "doctor": ("Runs every check and shows what, if anything, needs fixing."),
    "start": ("Starts the agent in the background. You can stop it here at any time."),
}


@dataclass
class StepResult:
    ok: bool
    message: str


def _hostport_ok(text):
    host, _, port = text.strip().rpartition(":")
    return bool(host) and port.isdigit() and 0 < int(port) < 65536


class Wizard:
    """Each step runs only after the one before it succeeded; every result is kept to show."""

    def __init__(self, controller, tls_available=None):
        self.c = controller
        self.tls_available = tls_available or (lambda: bool(ssl.HAS_TLSv1_3))
        self.results = {}

    def reset(self):
        self.results.clear()

    def done(self, step):
        r = self.results.get(step)
        return bool(r and r.ok)

    def can_run(self, step):
        i = STEPS.index(step)
        return all(self.done(s) for s in STEPS[:i])

    def next_step(self):
        for s in STEPS:
            if not self.done(s):
                return s
        return None

    def run(self, step, **v):
        if step not in STEPS:
            raise ValueError(f"no such step {step!r}")
        if not self.can_run(step):
            return StepResult(False, "Finish the earlier steps first.")
        res = getattr(self, "_" + step)(**v)
        self.results[step] = res
        for later in STEPS[STEPS.index(step) + 1 :]:  # a redone step invalidates what depended on it
            self.results.pop(later, None)
        return res

    def _cli(self, args, stdin=None, timeout=DEFAULT_TIMEOUT):
        r = self.c._run(args, stdin=stdin, timeout=timeout)
        return StepResult(r.ok, r.text())

    def _python(self, **_):
        ok = self.tls_available()
        version = sys.version.split()[0]
        if ok:
            return StepResult(True, f"Python {version} with TLS 1.3: good.")
        return StepResult(
            False,
            f"Python {version} here has no TLS 1.3, which the farm requires. Install a newer Python "
            "(built with OpenSSL 1.1.1 or later) and open this window with it.",
        )

    def _role(self, confirmed=False, **_):
        if not confirmed:
            return StepResult(False, "Confirm that you want to turn the worker role on.")
        return self._cli(["enable", "worker", "--yes"])

    def _enroll(self, invitation="", fingerprint="", code="", coordinator="", **_):
        if not _hostport_ok(invitation):
            return StepResult(
                False, "The invitation address must look like host:port, for example 192.168.1.20:7421."
            )
        if not _hostport_ok(coordinator):
            return StepResult(
                False, "The coordinator address must look like host:port, for example 192.168.1.20:7420."
            )
        if not fingerprint.strip() or not code.strip():
            return StepResult(False, "Type the coordinator's fingerprint and the one-time code.")
        return self._cli(
            ["enroll", invitation.strip(), "--fingerprint", fingerprint.strip(), "--code", code.strip(),
             "--coordinator", coordinator.strip()]
        )  # fmt: skip

    def _devices(self, solver="", device="", turn="lock", lock_command="", holding_turn=False, **_):
        if not solver.strip() or not device.strip():
            return StepResult(False, "Choose the solver program and the GPU's name first.")
        args = ["devices", "--solver", solver.strip(), "--save", "--device", device.strip()]
        if turn == "lock":
            if not lock_command.strip():
                return StepResult(False, "Give the lock command (it waits for your GPU's turn).")
            args += ["--lock-command", *lock_command.split()]
        else:
            if turn == "coordinator" and not holding_turn:
                return StepResult(False, "Hold the GPU's turn first, then confirm that you hold it.")
            args += ["--no-lock"]
        return self._cli(args, timeout=60 * 60 * 6 if turn == "lock" else 120)

    def _configure(self, settings=None, **_):
        res = self.c.save_settings(settings if settings is not None else Settings())
        return StepResult(res.ok, res.text())

    def _doctor(self, **_):
        rep = self.c.doctor()
        if rep.error:
            return StepResult(False, rep.error)
        bad = [c for c in rep.checks if not c.ok]
        if rep.ok:
            return StepResult(True, f"All {len(rep.checks)} checks passed.")
        return StepResult(
            False, "; ".join(f"{c.title}: {c.fix or c.detail}" for c in bad) or "A check failed."
        )

    def _start(self, **_):
        return self._cli(["agent", "start"], timeout=120)
