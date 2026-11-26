"""The farm agent: a worker that a person can configure, check, start and stop without an assistant.

`agent configure` saves the worker's settings (agent.json in the farm folder); `agent doctor` checks that
everything the worker needs is there, with a one-line fix for each problem; `agent start` runs the same
checks,
then starts `emfdtd_farm.py worker --from-agent` detached from the terminal; `agent stop` ends it and its
solver for good; `agent status` and `agent logs` say what it is doing. Autostart (autostart.py) is a separate,
explicit opt-in. Every status-like command has a `--json` form with a fixed shape, for the window.
"""

import asyncio
import contextlib
import json
import os
import shlex
import shutil
import signal
import ssl
import subprocess
import sys
import time
from pathlib import Path

from . import autostart, config, enroll, jobs, pki

TOOL = Path(__file__).resolve().parents[3] / "tools" / "emfdtd_farm.py"
STATUS_FRESH_S = 45.0  # a worker that has not touched its status file for this long is hung or gone
LOG_ROTATE_BYTES = 10 * 1024 * 1024
MIN_FREE_BYTES = 2 * (1 << 30)
CONFIG_KEYS = (
    "solver",
    "devices",
    "turn",
    "max_minutes",
    "max_steps",
    "max_input_mb",
    "max_tier",
    "watchdog",
    "work",
    "log",
)
CHECK_TITLES = {
    "tls13": "Python has TLS 1.3",
    "role": "The worker role is on",
    "identity": "This machine is enrolled as a worker",
    "coordinator": "The coordinator is reachable",
    "certificate": "The coordinator accepts our certificate",
    "solver": "The solver runs",
    "settings": "The solver lists its settings",
    "devices": "The GPUs to offer are recorded",
    "workdir": "The work folder is writable and has room",
    "config": "The agent is configured",
}


class AgentError(Exception):
    """An expected failure: reported with a message and exit code 1, never a traceback."""


# ---- files -------------------------------------------------------------------------------------------------


def config_file():
    return config.home() / "agent.json"


def pid_file():
    return config.home() / "agent.pid"


def status_file():
    return config.home() / "agent.status.json"


def _read_json(path):
    try:
        data = json.loads(Path(path).read_text())
        return data if isinstance(data, dict) else None
    except (OSError, ValueError):
        return None


def _write_json(path, data, private=True):
    config.ensure_home()
    path = Path(path)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2))
    if private:
        config.restrict(tmp)
    os.replace(str(tmp), str(path))


def work_dir(cfg=None):
    cfg = cfg or load() or {}
    return Path(cfg["work"]) if cfg.get("work") else config.home() / "work"


def log_path(cfg=None):
    cfg = cfg or load() or {}
    return Path(cfg["log"]) if cfg.get("log") else config.home() / "agent.log"


# ---- configuration -----------------------------------------------------------------------------------------


def _number(cfg, key, kind=float, low=0):
    v = cfg.get(key)
    if v is None:
        return None
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not v > low:
        raise AgentError(f"{key} must be a number greater than {low}")
    if kind is int and int(v) != v:
        raise AgentError(f"{key} must be a whole number")
    return kind(v)


def validate(cfg):
    """The normalised configuration, or AgentError. Used by the flags, by --json-in, and again by doctor."""
    if not isinstance(cfg, dict):
        raise AgentError("the configuration must be a JSON object")
    extra = sorted(set(cfg) - set(CONFIG_KEYS))
    if extra:
        raise AgentError("unknown setting(s): " + ", ".join(extra))
    solver = cfg.get("solver")
    if not isinstance(solver, str) or not solver:
        raise AgentError("solver is required (the path of the emfdtd program)")
    if not Path(solver).is_file():
        raise AgentError(f"the solver {solver} does not exist")
    devices = cfg.get("devices")
    if not isinstance(devices, list) or not devices or not all(isinstance(d, str) and d for d in devices):
        raise AgentError("devices must list the GPUs to offer (a UUID prefix or part of the name)")
    turn = cfg.get("turn")
    if not isinstance(turn, dict) or turn.get("mode") not in ("lock", "coordinator", "none"):
        raise AgentError("turn.mode must be exactly one of: lock, coordinator, none")
    lock = turn.get("lock_command") or []
    if turn["mode"] == "lock":
        if not (isinstance(lock, list) and lock and all(isinstance(a, str) and a for a in lock)):
            raise AgentError(
                "turn.mode lock needs a lock_command (the command that waits for the GPU's turn)"
            )
    elif lock:
        raise AgentError("a lock_command is only for turn.mode lock")
    tier = cfg.get("max_tier")
    if tier is not None and tier not in jobs.TIERS:
        raise AgentError("max_tier must be one of: " + ", ".join(jobs.TIERS))
    watchdog = cfg.get("watchdog", 20.0)
    if isinstance(watchdog, bool) or not isinstance(watchdog, (int, float)) or not 1 <= watchdog <= 600:
        raise AgentError("watchdog must be between 1 and 600 seconds")
    for key in ("work", "log"):
        if cfg.get(key) is not None and not (isinstance(cfg[key], str) and cfg[key]):
            raise AgentError(f"{key} must be a path")
    return {
        "solver": str(Path(solver)),
        "devices": list(devices),
        "turn": {"mode": turn["mode"], "lock_command": list(lock) if turn["mode"] == "lock" else []},
        "max_minutes": _number(cfg, "max_minutes"),
        "max_steps": _number(cfg, "max_steps", int),
        "max_input_mb": _number(cfg, "max_input_mb", int),
        "max_tier": tier,
        "watchdog": float(watchdog),
        "work": cfg.get("work"),
        "log": cfg.get("log"),
    }


def load():
    """The saved configuration as written, or None."""
    return _read_json(config_file())


def save(cfg):
    cfg = validate(cfg)
    _write_json(config_file(), cfg)
    return cfg


def config_from_args(args):
    """The configuration the `agent configure` flags describe (before validation)."""
    modes = [bool(args.lock_command), bool(args.no_lock), bool(args.turn_from_coordinator)]
    if sum(modes) != 1:
        raise AgentError(
            "give exactly one of --lock-command, --turn-from-coordinator, --no-lock (how this worker "
            "takes its "
            "GPU's turn)"
        )
    if args.lock_command:
        turn = {"mode": "lock", "lock_command": shlex.split(args.lock_command, posix=os.name != "nt")}
    else:
        turn = {"mode": "coordinator" if args.turn_from_coordinator else "none", "lock_command": []}
    return {
        "solver": args.solver,
        "devices": args.device,
        "turn": turn,
        "max_minutes": args.max_minutes,
        "max_steps": args.max_steps,
        "max_input_mb": args.max_input_mb,
        "max_tier": args.max_tier,
        "watchdog": args.watchdog if args.watchdog is not None else 20.0,
        "work": args.work,
        "log": args.log,
    }


# ---- processes ---------------------------------------------------------------------------------------------


def _run(argv, timeout=15):
    """The one place the agent runs a short external command (ps, tasklist, taskkill, powershell)."""
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, check=False)
    except (OSError, subprocess.SubprocessError) as e:
        return 127, str(e)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


_spawned = {}  # pid -> Popen of agents started by THIS process (so a finished one is reaped, not a zombie)


def pid_alive(pid):
    if not isinstance(pid, int) or pid <= 0:
        return False
    proc = _spawned.get(pid)
    if proc is not None:
        if proc.poll() is None:
            return True
        del _spawned[pid]  # reaped by poll(): it has ended
        return False
    if sys.platform == "win32":
        code, out = _run(["tasklist", "/FI", f"PID eq {pid}", "/NH"])
        return code == 0 and str(pid) in out
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def cmdline(pid):
    """The command line of a process, or None when it cannot be read."""
    if sys.platform == "win32":
        ps = f"(Get-CimInstance Win32_Process -Filter 'ProcessId={pid}').CommandLine"
        code, out = _run(["powershell", "-NoProfile", "-Command", ps])
    else:
        code, out = _run(["ps", "-o", "command=", "-p", str(pid)])
    return out.strip() if code == 0 and out.strip() else None


def _is_ours(pid, state):
    """Is this live pid our agent, and not an unrelated process that was given a recycled pid? A worker proves
    itself by writing the status file with its own pid; a hung one (stale file) is recognised by its
    command."""
    if not pid_alive(pid):
        return False
    if state and state.get("pid") == pid and time.time() - state.get("updated", 0) < STATUS_FRESH_S:
        return True
    line = cmdline(pid)
    return bool(line and "emfdtd_farm.py" in line and "--from-agent" in line)


def _coordinator_address():
    c = _read_json(config.home() / "coordinator.json")
    return f"{c['host']}:{c['port']}" if c and "host" in c and "port" in c else None


def status():
    """The agent's state, in the shape `agent status --json` prints."""
    cfg = load()
    info = _read_json(pid_file()) or {}
    state = _read_json(status_file())
    pid = info.get("pid")
    running = _is_ours(pid, state)
    mine = state if running and state and state.get("pid") == pid else None
    started = (mine or {}).get("started") or info.get("started") if running else None
    fresh = bool(mine and time.time() - mine.get("updated", 0) < STATUS_FRESH_S)
    return {
        "running": running,
        "pid": pid if running else None,
        "started": started,
        "uptime_s": round(time.time() - started, 1) if started else None,
        "connected": bool(fresh and mine.get("connected")),
        "coordinator": (mine or {}).get("coordinator") or _coordinator_address(),
        "job": mine.get("job") if mine else None,
        "device": mine.get("device") if mine else None,
        "last_error": mine.get("last_error") if mine else None,
        "log": str(log_path(cfg)),
    }


def _rotate(path):
    with contextlib.suppress(OSError):
        if path.stat().st_size > LOG_ROTATE_BYTES:
            os.replace(str(path), str(path) + ".1")


def start(wait_s=10.0):
    """Check everything, then start the agent detached. Returns the status. AgentError says why not."""
    st = status()
    if st["running"]:
        raise AgentError(f"the agent is already running (pid {st['pid']}); stop it first or use restart")
    report = doctor()
    failed = [c for c in report["checks"] if not c["ok"]]
    if failed:
        first = failed[0]
        more = f" (and {len(failed) - 1} more: run `agent doctor`)" if len(failed) > 1 else ""
        raise AgentError(f"cannot start: {first['title']}: {first['detail']}. Fix: {first['fix']}{more}")
    cfg = load()
    with contextlib.suppress(OSError):  # a stale pid file of a dead agent
        pid_file().unlink()
    with contextlib.suppress(OSError):
        status_file().unlink()
    log = log_path(cfg)
    log.parent.mkdir(parents=True, exist_ok=True)
    _rotate(log)
    argv = [
        sys.executable,
        "-u",
        str(TOOL),
        "worker",
        "--from-agent",
    ]  # -u: the log shows lines as they happen
    kwargs = {}
    if sys.platform == "win32":
        kwargs["creationflags"] = (
            subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
        )
    else:
        kwargs["start_new_session"] = True
    with open(log, "ab") as out:
        proc = subprocess.Popen(
            argv,
            cwd=str(config.home()),
            stdin=subprocess.DEVNULL,
            stdout=out,
            stderr=subprocess.STDOUT,
            close_fds=True,
            **kwargs,
        )
    _spawned[proc.pid] = proc
    _write_json(pid_file(), {"pid": proc.pid, "started": time.time()})
    end = time.time() + wait_s
    while time.time() < end:
        state = _read_json(status_file())
        if state and state.get("pid") == proc.pid:
            return status()  # it is alive and has said so; whether it is connected yet is in the status
        if proc.poll() is not None:
            break
        time.sleep(0.1)
    if proc.poll() is not None:
        tail = "\n".join(_tail(log, 5))
        with contextlib.suppress(OSError):
            pid_file().unlink()
        raise AgentError(
            f"the agent stopped right after starting (exit {proc.returncode}). Last log lines:\n{tail}"
        )
    return status()


def _kill_pids(pids, force):
    for pid in pids:
        if not pid_alive(pid):
            continue
        if sys.platform == "win32":
            _run(["taskkill", "/PID", str(pid), "/T"] + (["/F"] if force else []))
        else:
            with contextlib.suppress(ProcessLookupError, PermissionError):
                os.killpg(pid, signal.SIGKILL if force else signal.SIGTERM)  # a session leader: its group


def stop(timeout=20.0):
    """End the agent and its solver. Returns {"was_running", "stopped", "forced"}."""
    info = _read_json(pid_file()) or {}
    state = _read_json(status_file())
    pid = info.get("pid")
    solver_pids = list((state or {}).get("solver_pids") or [])
    if not _is_ours(pid, state):
        with contextlib.suppress(OSError):
            pid_file().unlink()
        return {"was_running": False, "stopped": True, "forced": False}
    forced = False
    if sys.platform == "win32":
        _run(["taskkill", "/PID", str(pid), "/T"])
    else:
        with contextlib.suppress(ProcessLookupError, PermissionError):
            os.kill(pid, signal.SIGTERM)  # the worker ends its solver and exits
    end = time.time() + timeout
    while time.time() < end and pid_alive(pid):
        time.sleep(0.1)
    if pid_alive(pid):
        forced = True
        _kill_pids([pid], force=True)
        time.sleep(0.3)
    _kill_pids(solver_pids, force=True)  # a solver left behind by a killed worker
    for p in solver_pids:
        for _ in range(20):
            if not pid_alive(p):
                break
            time.sleep(0.1)
    with contextlib.suppress(OSError):
        pid_file().unlink()
    if state:
        _write_json(status_file(), {**state, "connected": False, "job": None, "solver_pids": []})
    return {"was_running": True, "stopped": not pid_alive(pid), "forced": forced}


def _tail(path, n):
    try:
        lines = Path(path).read_text(errors="replace").splitlines()
    except OSError:
        return []
    return lines[-n:] if n > 0 else lines


def logs(n=50, follow=False, out=None, poll=0.5, iterations=None):
    """Print the last n lines; with follow, keep printing new ones until interrupted."""
    out = out or sys.stdout
    path = log_path()
    for line in _tail(path, n):
        print(line, file=out)
    if not follow:
        return
    try:
        pos = path.stat().st_size
    except OSError:
        pos = 0
    count = 0
    with contextlib.suppress(KeyboardInterrupt):
        while iterations is None or count < iterations:
            time.sleep(poll)
            count += 1
            try:
                size = path.stat().st_size
            except OSError:
                continue
            if size < pos:
                pos = 0  # rotated
            if size > pos:
                with open(path, "rb") as f:
                    f.seek(pos)
                    print(f.read().decode("utf-8", "replace"), end="", file=out)
                    pos = f.tell()
                out.flush()


# ---- doctor ------------------------------------------------------------------------------------------------


def _check(cid, ok, detail, fix=""):
    return {"id": cid, "title": CHECK_TITLES[cid], "ok": bool(ok), "detail": detail, "fix": "" if ok else fix}


def _cert_days_left(pem):
    """Days until the certificate expires, or None if it cannot be read (no openssl on this machine)."""
    exe = shutil.which("openssl")
    if not exe:
        return None
    try:
        r = subprocess.run(
            [exe, "x509", "-noout", "-enddate"], input=pem, capture_output=True, text=True, timeout=10
        )
    except (OSError, subprocess.SubprocessError):
        return None
    text = r.stdout.strip()
    if r.returncode != 0 or not text.startswith("notAfter="):
        return None
    try:
        return (ssl.cert_time_to_seconds(text.split("=", 1)[1]) - time.time()) / 86400.0
    except ValueError:
        return None


async def _probe(host, port, bundle, timeout=5.0):
    """("ok"|"unreachable"|"rejected", detail): can we connect, and does the coordinator take our
    certificate?"""
    try:
        ctx = pki.client_context(bundle)
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port, ssl=ctx), timeout)
    except ssl.SSLError as e:
        return "rejected", (
            "rejected: the TLS handshake failed (this machine's certificate authority does not match the "
            f"coordinator's, or our certificate was refused): {e}"
        )
    except (OSError, asyncio.TimeoutError) as e:
        return "unreachable", f"{type(e).__name__}: {e}" if str(e) else type(e).__name__
    try:
        ident = pki.peer_identity(writer.get_extra_info("ssl_object"))
        if ident is None or ident[0] != "coordinator":
            return "rejected", "rejected: the server is not a coordinator of this farm"
        # TLS 1.3 reports a rejected client certificate when we first read: wait briefly for an alert
        with contextlib.suppress(asyncio.TimeoutError):
            data = await asyncio.wait_for(reader.read(1), 0.5)
            if data == b"":
                return "rejected", "rejected: the coordinator closed the connection after the handshake"
        return "ok", f"connected to the coordinator as {ident[0]}:{ident[1]}"
    except ssl.SSLError as e:
        return "rejected", f"rejected: the coordinator refused our certificate: {e}"
    except ConnectionError as e:
        return "rejected", f"rejected: the coordinator dropped the connection: {e}"
    finally:
        writer.close()


def _solver_check(solver, flag):
    """Run `solver FLAG` (never a Vulkan call): (exit code, output)."""
    return _run([solver, flag], timeout=30)


def doctor():
    """Every check, in the shape `agent doctor --json` prints: {"ok": bool, "checks": [...]}."""
    checks = []
    tls = pki.tls13_available()
    checks.append(
        _check(
            "tls13",
            tls,
            ssl.OPENSSL_VERSION if tls else f"{ssl.OPENSSL_VERSION} has no TLS 1.3",
            "use a Python built with OpenSSL 1.1.1 or newer (the farm has no fallback)",
        )
    )
    role = config.enabled("worker")
    checks.append(_check("role", role, "on" if role else "off", "run: emfdtd_farm.py enable worker"))
    bundle = None
    try:
        bundle = pki.load_identity("worker")
        days = _cert_days_left(bundle["cert"])
        if days is not None and days < 0:
            checks.append(
                _check("identity", False, "the worker certificate has expired", "enrol this machine again")
            )
        else:
            note = "" if days is None else f", {days:.0f} days left"
            checks.append(_check("identity", True, f"identity/worker.json present{note}", ""))
    except pki.PkiError:
        checks.append(
            _check("identity", False, "no worker identity", "run: emfdtd_farm.py enroll ... (or setup)")
        )
    addr = _coordinator_address()
    reach, detail = None, ""
    if not addr:
        checks.append(
            _check("coordinator", False, "no coordinator address is stored", "run: emfdtd_farm.py enroll ...")
        )
    elif not (tls and bundle):
        checks.append(
            _check(
                "coordinator",
                False,
                f"{addr}: not checked (needs TLS 1.3 and an identity)",
                "fix the checks above",
            )
        )
    else:
        host, _, port = addr.rpartition(":")
        reach, detail = asyncio.run(_probe(host, int(port), bundle))
        checks.append(
            _check(
                "coordinator",
                reach != "unreachable",
                f"{addr}: {'reachable' if reach != 'unreachable' else detail}",
                "check the address, that the coordinator is running, and the firewall",
            )
        )
    if reach is None or reach == "unreachable":
        checks.append(
            _check(
                "certificate",
                False,
                "not checked: the coordinator is not reachable",
                "fix the coordinator check first",
            )
        )
    else:
        checks.append(
            _check(
                "certificate",
                reach == "ok",
                detail,
                "enrol this machine again (the certificate may be revoked or expired)",
            )
        )
    saved = load()
    cfg, cfg_error = None, ""
    if saved is None:
        cfg_error = "no saved configuration"
    else:
        try:
            cfg = validate(saved)
        except AgentError as e:
            cfg_error = str(e)
    fix_cfg = (
        "run: emfdtd_farm.py agent configure --solver PATH --device NAME "
        "(--no-lock | --lock-command CMD | --turn-from-coordinator)"
    )
    if cfg is None:
        for cid in ("solver", "settings", "devices", "workdir"):
            checks.append(_check(cid, False, f"not checked: {cfg_error}", fix_cfg))
        checks.append(_check("config", False, cfg_error, fix_cfg))
        return {"ok": False, "checks": checks}
    code, out = _solver_check(cfg["solver"], "--version")
    checks.append(
        _check(
            "solver",
            code == 0,
            out.strip().splitlines()[0]
            if code == 0 and out.strip()
            else f"{cfg['solver']}: {out.strip()[:120] or 'did not run'}",
            "build or install emfdtd for this machine and point `agent configure --solver` at it",
        )
    )
    code, out = _solver_check(cfg["solver"], "--list-settings")
    names = jobs.parse_list_settings(out) if code == 0 else {}
    checks.append(
        _check(
            "settings",
            bool(names),
            f"{len(names)} settings" if names else "the solver printed no settings",
            "the solver must be a build of this project (emfdtd --list-settings)",
        )
    )
    work = work_dir(cfg)
    devs_file = work / "devices.json"
    recorded = _read_json_list(devs_file)
    if recorded is None:
        checks.append(
            _check(
                "devices",
                False,
                f"no recorded devices in {devs_file}",
                "run: emfdtd_farm.py devices --solver PATH --save --device NAME "
                "(once, while you hold the GPU's turn)",
            )
        )
    else:
        from .worker import choose

        picked = choose(recorded, cfg["devices"])
        names = ", ".join(d["name"] for d in picked)
        checks.append(
            _check(
                "devices",
                bool(picked),
                names if picked else f"none of {cfg['devices']} match the recorded devices",
                "run `devices --save` again, or change the device names in `agent configure`",
            )
        )
    checks.append(_workdir_check(work))
    checks.append(
        _check("config", True, f"turn: {cfg['turn']['mode']}, devices: {', '.join(cfg['devices'])}")
    )
    order = list(CHECK_TITLES)
    checks.sort(key=lambda c: order.index(c["id"]))
    return {"ok": all(c["ok"] for c in checks), "checks": checks}


def _read_json_list(path):
    try:
        data = json.loads(Path(path).read_text())
        return data if isinstance(data, list) else None
    except (OSError, ValueError):
        return None


def _workdir_check(work):
    try:
        work.mkdir(parents=True, exist_ok=True)
        probe = work / ".write-test"
        probe.write_text("x")
        probe.unlink()
        free = shutil.disk_usage(str(work)).free
    except OSError as e:
        return _check(
            "workdir", False, f"{work}: {e}", "choose a writable folder with `agent configure --work`"
        )
    gib = free / (1 << 30)
    return _check(
        "workdir",
        free >= MIN_FREE_BYTES,
        f"{work}: {gib:.1f} GiB free",
        "free some disk space or choose another folder with `agent configure --work`",
    )


# ---- the command line --------------------------------------------------------------------------------------


def add_parsers(sub):
    """The `agent` and `setup` sub-commands (called by cli._parser)."""
    import argparse

    p = sub.add_parser("agent", help="a worker you can configure, check, start and stop")
    a = p.add_subparsers(dest="agent_cmd", required=True)
    s = a.add_parser("configure", help="save this machine's worker settings")
    s.add_argument("--solver")
    s.add_argument(
        "--device", action="append", default=[], help="UUID prefix or part of a GPU's name; repeatable"
    )
    s.add_argument("--lock-command", help="the command that waits for your GPU's turn, as one quoted string")
    s.add_argument("--no-lock", action="store_true")
    s.add_argument("--turn-from-coordinator", action="store_true")
    s.add_argument("--max-minutes", type=float)
    s.add_argument("--max-steps", type=int)
    s.add_argument("--max-input-mb", type=int)
    s.add_argument("--max-tier", choices=jobs.TIERS)
    s.add_argument("--watchdog", type=float)
    s.add_argument("--work")
    s.add_argument("--log")
    s.add_argument("--show", action="store_true", help="print the saved configuration")
    s.add_argument("--json-in", action="store_true", help="read the new configuration as JSON on stdin")
    s.add_argument("--json", action="store_true")
    for name in ("start", "restart"):
        s = a.add_parser(name)
        s.add_argument("--json", action="store_true")
    s = a.add_parser("stop")
    s.add_argument("--timeout", type=float, default=20.0)
    s.add_argument("--json", action="store_true")
    a.add_parser("status").add_argument("--json", action="store_true")
    a.add_parser("doctor").add_argument("--json", action="store_true")
    s = a.add_parser("logs")
    s.add_argument("-n", type=int, default=50)
    s.add_argument("--follow", action="store_true")
    s = a.add_parser("autostart", help="start the agent when you log in (off unless you turn it on)")
    s.add_argument("action", choices=("on", "off", "status"))
    s.add_argument("--json", action="store_true")

    s = sub.add_parser("setup", help="walk a new machine through becoming a worker")
    s.add_argument("--yes", action="store_true", help="do not ask for confirmations")
    s.add_argument("--invitation", help="HOST:PORT of the invitation (from `invite` on the coordinator)")
    s.add_argument("--fingerprint")
    s.add_argument("--code")
    s.add_argument("--coordinator", help="HOST:PORT of the running coordinator")
    s.add_argument("--solver")
    s.add_argument("--device", action="append", default=[])
    s.add_argument("--lock-command")
    s.add_argument("--no-lock", action="store_true")
    s.add_argument("--turn-from-coordinator", action="store_true")
    s.add_argument("--max-minutes", type=float)
    s.add_argument("--max-steps", type=int)
    s.add_argument("--max-input-mb", type=int)
    s.add_argument("--max-tier", choices=jobs.TIERS)
    s.add_argument("--watchdog", type=float)
    s.add_argument("--work")
    s.add_argument("--log")
    s.add_argument("--start", action="store_true", help="start the agent at the end")
    del argparse


def _emit(args, out, data, text):
    if getattr(args, "json", False):
        print(json.dumps(data), file=out)
    else:
        print(text, file=out)


def _doctor_text(report):
    lines = []
    for c in report["checks"]:
        lines.append(f"{'ok  ' if c['ok'] else 'FAIL'} {c['title']}: {c['detail']}")
        if not c["ok"]:
            lines.append(f"       fix: {c['fix']}")
    lines.append("all checks passed" if report["ok"] else "some checks failed")
    return "\n".join(lines)


def run(args, out, stdin, err=None):
    """Run an `agent` or `setup` command. Returns the exit code: 0 success, 1 an expected failure (the message
    goes to stderr, or to stdout as {"error": ...} with --json)."""
    err = err or sys.stderr
    as_json = getattr(args, "json", False)
    try:
        if args.cmd == "setup":
            return _setup(args, out, stdin)
        return _agent(args, out, stdin)
    except AgentError as e:
        if as_json:
            print(json.dumps({"error": str(e)}), file=out)
        else:
            print(f"error: {e}", file=err)
        return 1


def _agent(args, out, stdin):
    cmd = args.agent_cmd
    if cmd == "configure":
        if args.show:
            cfg = load()
            if cfg is None:
                raise AgentError("no saved configuration")
            _emit(args, out, cfg, json.dumps(cfg, indent=2))
            return 0
        if args.json_in:
            try:
                new = json.loads(stdin.read())
            except ValueError as e:
                raise AgentError(f"stdin is not JSON: {e}") from None
        else:
            new = config_from_args(args)
        cfg = save(new)
        _emit(args, out, cfg, "saved " + str(config_file()))
        return 0
    if cmd == "doctor":
        report = doctor()
        _emit(args, out, report, _doctor_text(report))
        return 0 if report["ok"] else 1
    if cmd == "status":
        st = status()
        text = (
            f"running (pid {st['pid']}, up {st['uptime_s']:.0f} s), "
            f"{'connected to ' + str(st['coordinator']) if st['connected'] else 'not connected'}"
            + (f", job {st['job']['id'][:8]}: {st['job']['phase']}" if st["job"] else "")
            if st["running"]
            else "not running"
        )
        _emit(args, out, st, text + (f". Last error: {st['last_error']}" if st["last_error"] else ""))
        return 0
    if cmd == "logs":
        logs(args.n, args.follow, out)
        return 0
    if cmd == "start":
        st = start()
        _emit(args, out, st, f"agent started (pid {st['pid']}); log: {st['log']}")
        return 0
    if cmd == "stop":
        r = stop(args.timeout)
        text = (
            "the agent was not running"
            if not r["was_running"]
            else "the agent stopped" + (" (it had to be killed)" if r["forced"] else "")
        )
        if r["was_running"] and not r["stopped"]:
            raise AgentError(
                "the agent is still running after being killed: check pid " + str(status()["pid"])
            )
        _emit(args, out, r, text)
        return 0
    if cmd == "restart":
        stop()
        st = start()
        _emit(args, out, st, f"agent restarted (pid {st['pid']})")
        return 0
    if cmd == "autostart":
        return _autostart(args, out)
    raise AgentError(f"unknown agent command {cmd}")


def _autostart(args, out):
    cfg = load()
    if args.action == "status":
        st = autostart.status()
        _emit(
            args,
            out,
            st,
            f"autostart is {'on' if st['enabled'] else 'off'} ({st['mechanism']}: {st['detail']})",
        )
        return 0
    if args.action == "on":
        if cfg is None:
            raise AgentError(
                "configure the agent first (autostart would only start something that cannot run)"
            )
        try:
            detail = autostart.on(sys.executable, TOOL, log_path(cfg))
        except RuntimeError as e:
            raise AgentError(str(e)) from None
    else:
        detail = autostart.off()
    st = autostart.status()
    _emit(args, out, st, detail)
    return 0


# ---- setup -------------------------------------------------------------------------------------------------


def _ask(stdin, out, prompt, yes):
    if yes:
        raise AgentError(f"missing value: {prompt.strip().rstrip(':')} (give it as a flag; --yes never asks)")
    print(prompt, end="", file=out)
    out.flush()
    return stdin.readline().strip()


def _hostport(text):
    host, _, port = (text or "").rpartition(":")
    if not host or not port.isdigit():
        raise AgentError(f"expected HOST:PORT, got {text!r}")
    return host, int(port)


def record_devices(cfg):
    """`devices --save` for this configuration: one Vulkan call (through the lock command if there is one)."""
    from .worker import Worker, choose, parse_devices

    turn = cfg["turn"]

    async def go():
        w = Worker(
            {},
            "",
            0,
            cfg["solver"],
            work_dir(cfg),
            [],
            lock_command=turn["lock_command"],
            no_lock=turn["mode"] in ("none", "coordinator"),
        )
        _, text = await w._capture("--list-devices", gpu=True)
        return w, parse_devices(text)

    w, found = asyncio.run(go())
    chosen = choose(found, cfg["devices"])
    if not chosen:
        raise AgentError(
            "none of the GPUs matches "
            + ", ".join(cfg["devices"])
            + "; the solver sees: "
            + ", ".join(d["name"] for d in found)
        )
    w.work_root.mkdir(parents=True, exist_ok=True)
    w.save_devices(chosen)
    return chosen


def _setup(args, out, stdin):
    def say(text=""):
        print(text, file=out)

    say("1. Python: " + ssl.OPENSSL_VERSION)
    if not pki.tls13_available():
        raise AgentError(
            "this Python has no TLS 1.3; the farm requires it. Use a Python built with OpenSSL 1.1.1+"
        )
    if not config.enabled("worker"):
        say(
            "2. Turning on the worker role. This machine will connect OUT to a coordinator you enrol it with "
            "and run "
            "the solver on its GPU for that coordinator's jobs. It opens no port."
        )
        if not args.yes and _ask(stdin, out, "Type 'enable worker' to confirm: ", False) != "enable worker":
            raise AgentError("not enabled")
        config.set_enabled("worker", True)
    else:
        say("2. The worker role is already on.")
    config.remember_python()
    try:
        pki.load_identity("worker")
        say("3. Already enrolled as a worker.")
    except pki.PkiError:
        inv = args.invitation or _ask(
            stdin, out, "3. Invitation address (HOST:PORT from `invite`): ", args.yes
        )
        fp = args.fingerprint or _ask(stdin, out, "   The coordinator's fingerprint: ", args.yes)
        code = args.code or _ask(stdin, out, "   The one-time code: ", args.yes)
        coord = args.coordinator or _ask(
            stdin, out, "   The coordinator's address (HOST:PORT of `serve`): ", args.yes
        )
        host, port = _hostport(inv)
        chost, cport = _hostport(coord)
        try:
            bundle = asyncio.run(enroll.enroll(host, port, fp, code))
        except enroll.EnrollError as e:
            raise AgentError(f"enrolment failed: {e}") from None
        if bundle.get("role") != "worker":
            raise AgentError(f"that invitation is for a {bundle.get('role')}, not a worker") from None
        pki.save_identity("worker", bundle)
        _write_json(config.home() / "coordinator.json", {"host": chost, "port": cport}, private=False)
        say(f"   enrolled as worker {bundle.get('name', '')}")
    solver = args.solver or _ask(stdin, out, "4. Path of the emfdtd program: ", args.yes)
    devices = args.device or _ask(stdin, out, "   GPU(s) to offer (a name or UUID prefix): ", args.yes).split(
        ","
    )
    args.solver, args.device = solver, [d.strip() for d in devices if d.strip()]
    if not (args.lock_command or args.no_lock or args.turn_from_coordinator):
        if args.yes:
            raise AgentError("give one of --lock-command, --turn-from-coordinator, --no-lock")
        answer = _ask(stdin, out, "   Does anyone else use this GPU? [y/N]: ", False).lower()
        if answer.startswith("y"):
            args.lock_command = _ask(stdin, out, "   The command that waits for the GPU's turn: ", False)
        else:
            args.no_lock = True
    cfg = validate(config_from_args(args))
    say(
        "5. Recording the GPUs. This makes ONE call to the GPU driver (a Vulkan device listing)"
        + (
            "; it goes through your lock command, so it waits for the GPU's turn."
            if cfg["turn"]["mode"] == "lock"
            else "."
            if cfg["turn"]["mode"] == "none"
            else ". The coordinator holds this GPU's turn for jobs: do this only while you hold it yourself."
        )
    )
    if cfg["turn"]["mode"] == "coordinator" and not args.yes:
        holding = _ask(stdin, out, "   Do you hold the GPU's turn right now? [y/N]: ", False)
        if not holding.lower().startswith("y"):
            raise AgentError("get the GPU's turn first, then run setup again")
    chosen = record_devices(cfg)
    say("   recorded: " + ", ".join(d["name"] for d in chosen))
    save(cfg)
    say("6. Saved " + str(config_file()))
    report = doctor()
    say("7. Checks:\n" + _doctor_text(report))
    if not report["ok"]:
        return 1
    if args.start or (
        not args.yes and _ask(stdin, out, "Start the agent now? [y/N]: ", False).lower().startswith("y")
    ):
        st = start()
        say(f"agent started (pid {st['pid']}); log: {st['log']}")
    else:
        say("Done. Start it with: emfdtd_farm.py agent start")
    return 0
