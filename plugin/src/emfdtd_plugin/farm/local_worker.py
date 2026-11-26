"""The coordinator machine's own GPU as a farm device, with one explicit command.

`local-worker enable` does what a person would do by hand on any other machine: turn the worker role on,
enroll this machine with the farm's own certificate authority (no network step: the CA is right here), pick
the
GPU(s), record them, save the agent's settings and start the agent. It is never automatic; the farm stays
off until the user runs it (or clicks the Farm tab's button). `disable` stops the agent and turns the worker
role off.
`status --json` has a fixed shape for the window.
"""

import asyncio
import json
import re
import socket
import sys
from pathlib import Path

from . import agent, config, pki, policy

DEFAULT_MAX_MINUTES = 45.0


def _slug(text):
    text = re.sub(r"[^A-Za-z0-9._-]+", "-", text).strip("-._")
    return text[:40] or "this-computer"


def _worker_name():
    """The name of this machine's worker identity, or None if it is not enrolled."""
    try:
        bundle = pki.load_identity("worker")
    except pki.PkiError:
        return None
    mine = pki.fingerprint(bundle["cert"])
    for key, info in pki.members().items():
        if key.startswith("worker:") and info.get("fingerprint") == mine:
            return key.split(":", 1)[1]
    return None


def _unavailable():
    """Why this machine cannot offer its GPU now ("" if it can)."""
    if not config.enabled("coordinator"):
        return "turn on the coordinator role first"
    if not pki.ca_exists():
        return "the farm is not set up yet: run `emfdtd-farm init` first"
    if not pki.tls13_available():
        return "this Python has no TLS 1.3, which the farm requires"
    return ""


def status():
    reason = _unavailable()
    cfg = agent.load()
    st = agent.status()
    name = _worker_name()
    enabled = bool(config.enabled("worker") and name and cfg)
    turn = (cfg or {}).get("turn", {}).get("mode", "none")
    return {
        "available": not reason,
        "reason": reason,
        "enabled": enabled,
        "running": bool(st["running"]),
        "connected": bool(st["connected"]),
        "name": name or "",
        "device": st.get("device") or ", ".join((cfg or {}).get("devices") or []),
        "solver": (cfg or {}).get("solver") or "",
        "turn": turn,
    }


def _find_solver(given):
    if given:
        return str(given)
    from .. import launcher

    try:
        return launcher.find_solver(Path(__file__).resolve().parents[3])
    except RuntimeError as e:
        raise agent.AgentError(f"{e}; or pass --solver PATH") from None


def _discover_and_record(solver, lock_command, turn_mode):
    """List the GPUs through the solver (one Vulkan call, behind the lock command if any), keep one per
    physical GPU, record them for the worker's `hello`. Returns the chosen devices."""
    from .worker import Worker, choose, parse_devices

    async def go():
        w = Worker(
            {},
            "",
            0,
            solver,
            agent.work_dir(),
            [],
            lock_command=lock_command,
            no_lock=turn_mode in ("none", "coordinator"),
        )
        _, text = await w._capture("--list-devices", gpu=True)
        return w, parse_devices(text)

    w, found = asyncio.run(go())
    usable = [d for d in found if d["usable"]]
    chosen = choose(found, [d["uuid"] for d in usable])
    if not chosen:
        raise agent.AgentError(
            "the solver found no usable GPU on this computer (Vulkan 1.2 is required"
            + (
                ": " + ", ".join(f"{d['name']} (vulkan {d['vulkan'][0]}.{d['vulkan'][1]})" for d in found)
                if found
                else ""
            )
            + ")"
        )
    w.work_root.mkdir(parents=True, exist_ok=True)
    w.save_devices(chosen)
    return chosen


def enable(solver=None, lock_command=None, max_minutes=None, start=True):
    """Make this machine a worker of its own farm. Returns {"name", "device"}; raises AgentError."""
    reason = _unavailable()
    if reason:
        raise agent.AgentError(reason)
    coordinator = agent._coordinator_address()
    if not coordinator:
        raise agent.AgentError(
            "this computer does not know its coordinator's address yet: start the coordinator once "
            "(`emfdtd-farm serve --listen ADDRESS`) and enroll or save `coordinator.json`"
        )
    config.set_enabled("worker", True)
    config.remember_python()
    name = _worker_name()
    if name is None:  # enroll against our own CA: no network step, same certificate and grants as any worker
        name = _slug(socket.gethostname().split(".")[0])
        if f"worker:{name}" in pki.members():
            raise agent.AgentError(
                f"a worker named {name!r} already exists; revoke it first or enroll by hand"
            )
        bundle = pki.issue("worker", name)
        policy.add_device("worker", name)
        pki.record_member("worker", name, pki.fingerprint(bundle["cert"]))
        pki.save_identity("worker", bundle)
    cfg = agent.load() or {}
    solver = _find_solver(solver or cfg.get("solver"))
    turn = cfg.get("turn") or {"mode": "none", "lock_command": []}
    if lock_command:
        import os
        import shlex

        turn = {"mode": "lock", "lock_command": shlex.split(lock_command, posix=os.name != "nt")}
    chosen = _discover_and_record(solver, turn["lock_command"], turn["mode"])
    new = {
        **dict.fromkeys(agent.CONFIG_KEYS),
        "watchdog": 20.0,
        **cfg,
        "solver": solver,
        "devices": [d["uuid"] for d in chosen],
        "turn": turn,
        "max_minutes": max_minutes or cfg.get("max_minutes") or DEFAULT_MAX_MINUTES,
    }
    new = agent.validate(new)
    agent.save(new)
    if start and not agent.status()["running"]:
        agent.start()
    return {"name": name, "device": ", ".join(d["name"] for d in chosen)}


def disable():
    """Stop the agent and turn the worker role off. The identity stays, so enabling again is instant."""
    st = agent.stop()
    config.set_enabled("worker", False)
    return {"was_running": bool(st["was_running"])}


def add_parsers(sub):
    p = sub.add_parser(
        "local-worker",
        help="coordinator: also offer this computer's own GPU to the farm (off until you run it)",
    )
    a = p.add_subparsers(dest="local_cmd", required=True)
    s = a.add_parser("status", help="is this computer's GPU in the farm?")
    s.add_argument("--json", action="store_true")
    s = a.add_parser("enable", help="enroll this computer as a worker, record its GPU, start the agent")
    s.add_argument("--solver", help="the emfdtd program (default: the one the plugin uses)")
    s.add_argument("--lock-command", help="the command that waits for this GPU's turn, if others share it")
    s.add_argument("--max-minutes", type=float, help=f"longest job (default {DEFAULT_MAX_MINUTES:g})")
    s.add_argument("--yes", action="store_true", help="do not ask for the typed confirmation")
    s.add_argument("--no-start", action="store_true", help="set everything up but do not start the agent")
    s.add_argument("--json", action="store_true")
    s = a.add_parser("disable", help="stop the agent and turn the worker role off")
    s.add_argument("--json", action="store_true")


CONSEQUENCE = (
    "This computer's GPU will join the farm like any other device: the worker role turns on, the computer\n"
    "is enrolled as a worker of its own coordinator, the solver lists the GPU (one Vulkan call, through the\n"
    "GPU lock command if you gave one), and the agent starts. Jobs submitted by enrolled clients can then\n"
    "run on this GPU, up to the longest-job limit. `local-worker disable` stops it again."
)


def run(args, out, stdin, err=None):
    err = err or sys.stderr
    as_json = getattr(args, "json", False)

    def emit(data, text):
        print(json.dumps(data) if as_json else text, file=out)

    try:
        if args.local_cmd == "status":
            st = status()
            emit(st, "\n".join(f"{k}: {v}" for k, v in st.items()))
            return 0
        if args.local_cmd == "disable":
            r = disable()
            emit(
                {"ok": True, **r},
                "stopped" if r["was_running"] else "was not running; the worker role is off",
            )
            return 0
        if not args.yes:
            print(CONSEQUENCE, file=out)
            print("Type 'enable local worker' to confirm: ", end="", file=out)
            out.flush()
            if stdin.readline().strip() != "enable local worker":
                print("not enabled", file=out)
                return 1
        r = enable(args.solver, args.lock_command, args.max_minutes, start=not args.no_start)
        emit({"ok": True, **r}, f"this computer's GPU ({r['device']}) is in the farm as worker {r['name']}")
        return 0
    except (agent.AgentError, pki.PkiError) as e:
        if as_json:
            print(json.dumps({"ok": False, "error": str(e)}), file=out)
        else:
            print(f"error: {e}", file=err)
        return 1
