"""The `emfdtd-farm` command line. For now: switch roles on and off and show the state; the coordinator,
worker and job commands arrive with the scheduler (docs/PLAN.md, GPU farm). Nothing here starts a process
or opens a port."""

import argparse
import asyncio
import contextlib
import json
import signal
import ssl
import sys
from pathlib import Path

from . import agent, client, config, enroll, estimate, jobs, local_worker, pki, policy

CONSEQUENCE = {
    "client": (
        "This machine may send simulation jobs to a farm coordinator you enrol it with. Your board snapshot "
        "is uploaded to that coordinator and its workers; results come back to you."
    ),
    "coordinator": (
        "This machine may act as the farm coordinator: it will be able to create invitations, keep a "
        "private certificate authority in {home}, and (once the scheduler exists) listen on one LAN port "
        "for enrolled devices and send them jobs."
    ),
    "worker": (
        "This machine may act as a farm worker: it will be able to connect OUT to a coordinator you enrol "
        "it with (it never listens) and run the solver on this machine's GPU for jobs that coordinator sends."
    ),
}


def state():
    return {role: config.enabled(role) for role in config.ROLES}


def _hostport(text):
    host, _, port = text.rpartition(":")
    if not host or not port.isdigit():
        raise SystemExit(f"expected HOST:PORT, got {text!r}")
    return host, int(port)


def _connection():
    try:
        c = json.loads((config.home() / "coordinator.json").read_text())
        return c["host"], int(c["port"])
    except (OSError, ValueError, KeyError):
        raise SystemExit(
            "this machine has no coordinator address: enrol it first (emfdtd-farm enroll ...)"
        ) from None


class GatesError(ValueError):
    pass


def read_gates():
    """gates.json in the farm folder: {"worker-name": {"hold": [command...], "release": [command...]}}. The
    coordinator then holds that GPU's turn for every job it offers the worker (see docs/farm.md). Raises
    GatesError for a file that is not usable; a missing file means no gates."""
    try:
        gates = json.loads((config.home() / "gates.json").read_text())
    except OSError:
        return {}
    except ValueError:
        raise GatesError("gates.json is not valid JSON") from None
    if not isinstance(gates, dict):
        raise GatesError("gates.json must be an object of worker names")
    for name, gate in gates.items():
        if not (
            isinstance(gate, dict)
            and isinstance(gate.get("hold"), list)
            and isinstance(gate.get("release"), list)
        ):
            raise GatesError(f"gates.json: {name} needs a hold and a release command list")
    return gates


def _load_gates():
    """At start-up a broken gates.json stops the coordinator with a message (a running coordinator keeps the
    last good gates instead: see Coordinator.gates_loader)."""
    try:
        return read_gates()
    except GatesError as e:
        raise SystemExit(str(e)) from None


def _need(role):
    try:
        config.require_enabled(role)
    except config.FarmDisabled as e:
        raise SystemExit(str(e)) from None


def _parser():
    p = argparse.ArgumentParser(prog="emfdtd-farm", description="GPU job farm (off by default).")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("gui", help="open the agent window (tkinter): status, start/stop, settings, set-up")
    for name in ("enable", "disable"):
        s = sub.add_parser(name, help=name + " a role on this machine")
        s.add_argument("role", choices=config.ROLES)
        if name == "enable":
            s.add_argument("--yes", action="store_true", help="confirm without asking")
    st = sub.add_parser("status", help="show which roles are on")
    st.add_argument("--json", action="store_true")

    s = sub.add_parser(
        "init", help="coordinator: create the farm's certificate authority and server identity"
    )
    s.add_argument("--host", action="append", required=True, help="a name or IP address workers will use")
    s = sub.add_parser("invite", help="coordinator: let one new device join (prints a code and fingerprint)")
    s.add_argument("name")
    s.add_argument("--role", choices=("worker", "client"), default="worker")
    s.add_argument("--listen", required=True, help="LAN address to listen on while the invitation is open")
    s.add_argument("--port", type=int, default=0)
    s.add_argument("--minutes", type=int, default=10)
    s = sub.add_parser("serve", help="coordinator: run the farm")
    s.add_argument("--listen", required=True, help="the one LAN address to listen on")
    s.add_argument("--port", type=int, default=7420)
    s.add_argument(
        "--checkpoint-every", type=float, default=300.0, metavar="SECONDS",
        help="how often workers that can checkpoint write one (default 300)",
    )  # fmt: skip
    s.add_argument(
        "--hold-minutes", type=float, default=60.0,
        help="how long a job whose worker went away waits for it (and its checkpoint) before starting "
        "again from step 0 on any worker (default 60; 0 = never wait)",
    )  # fmt: skip
    sub.add_parser("members", help="coordinator: list enrolled devices").add_argument(
        "--json", action="store_true"
    )
    s = sub.add_parser("revoke", help="coordinator: remove a device for good")
    s.add_argument("role", choices=("worker", "client"))
    s.add_argument("name")

    s = sub.add_parser("enroll", help="this device: join a farm with the code from `invite`")
    s.add_argument("enrolment", help="HOST:PORT of the invitation")
    s.add_argument("--fingerprint", required=True, help="the coordinator's fingerprint from its screen")
    s.add_argument("--code", required=True)
    s.add_argument("--coordinator", required=True, help="HOST:PORT of the running coordinator (serve)")
    s = sub.add_parser("devices", help="worker: list this machine's GPUs as the solver sees them")
    s.add_argument("--solver", required=True)
    s.add_argument("--lock-command", nargs="+", default=[], help="wraps the Vulkan call (your GPU turn)")
    s.add_argument("--no-lock", action="store_true", help="nobody else uses this GPU (you hold the turn now)")
    s.add_argument(
        "--save",
        action="store_true",
        help="record the devices a worker on this machine offers when it connects (needed once, in every "
        "turn mode: a worker makes no Vulkan call to connect)",
    )
    s.add_argument("--device", action="append", default=[], help="with --save: the GPUs to record")
    s.add_argument("--work", help="with --save: the worker's job folder (default: inside the farm folder)")
    s = sub.add_parser("worker", help="worker: offer GPUs and run jobs (connects out; never listens)")
    s.add_argument("--solver", help="the emfdtd binary")
    s.add_argument(
        "--from-agent",
        action="store_true",
        help="take every setting from `agent configure` (this is how `agent start` runs the worker)",
    )
    s.add_argument(
        "--device", action="append", default=[], help="UUID prefix or part of the name; repeatable"
    )
    s.add_argument(
        "--lock-command",
        nargs="+",
        default=[],
        help="a command that wraps every GPU use, e.g. a script that waits for your GPU turn (put in front "
        "of the solver: COMMAND SOLVER ARGS...)",
    )
    s.add_argument(
        "--no-lock", action="store_true", help="nobody else uses this GPU: run without a lock command"
    )
    s.add_argument(
        "--turn-from-coordinator",
        action="store_true",
        help="the coordinator holds this GPU's turn for each job (gates.json) and says so in the offer",
    )
    s.add_argument(
        "--max-minutes",
        type=float,
        help="this worker's own ceiling per job (a coordinator may only lower it)",
    )
    s.add_argument(
        "--watchdog",
        type=float,
        default=20.0,
        help="seconds of silence from the coordinator after which the solver is killed (default 20)",
    )
    s.add_argument("--max-steps", type=int, help="this worker's own ceiling on max_steps")
    s.add_argument("--max-input-mb", type=int, help="this worker's own ceiling on the snapshot size")
    s.add_argument("--max-tier", choices=jobs.TIERS, help="highest settings tier this worker accepts")
    s.add_argument("--work", help="folder for job files (default: inside the farm folder)")

    s = sub.add_parser("submit", help="client: send a snapshot to the farm")
    s.add_argument("snapshot")
    s.add_argument("--profile")
    s.add_argument("--set", action="append", default=[], metavar="KEY=VALUE")
    s.add_argument("--sweep", metavar="KEY=a,b,c", help="one job per value, all on one kind of device")
    s.add_argument("--refine", action="append", default=[], nargs=5, type=float, metavar="MM")
    s.add_argument("--part", action="append", default=[], metavar="REF=TREATMENT")
    s.add_argument("--body", action="append", default=[], metavar="REF=TREATMENT")
    s.add_argument("--nets")
    s.add_argument("--ground-nets")
    s.add_argument("--label", default="")
    s.add_argument(
        "--device",
        metavar="WORKER[:GPU]",
        help="send the job only to this worker (and a GPU whose name contains GPU), for example mac-m1",
    )
    s.add_argument(
        "--no-estimate",
        action="store_true",
        help="do not ask the local solver for a dry run (the estimate keeps a long job off a worker "
        "whose time ceiling it would outrun)",
    )
    s.add_argument("--json", action="store_true", help='print {"jobs": [ids]} instead of one id per line')
    s.add_argument("--wait", metavar="DEST", help="wait for the jobs and save their results in DEST")
    sub.add_parser("queue", help="client: my jobs and the farm's devices").add_argument(
        "--json", action="store_true"
    )
    s = sub.add_parser("resume", help="client: continue a paused job from its checkpoint")
    s.add_argument("job")
    s.add_argument(
        "--max-minutes",
        type=float,
        help="stop this run after this long (at most what the worker allows); it pauses again if needed",
    )
    s.add_argument("--json", action="store_true")
    s = sub.add_parser(
        "restart", help="client: drop a paused job's checkpoint and start it again from step 0"
    )
    s.add_argument("job")
    s.add_argument("--json", action="store_true")
    s = sub.add_parser("cancel", help="client: cancel one of my jobs")
    s.add_argument("job")
    s = sub.add_parser("results", help="client: download a finished job's results")
    s.add_argument("job")
    s.add_argument("--dest", required=True)
    agent.add_parsers(sub)
    local_worker.add_parsers(sub)
    return p


def _num(text):
    try:
        return float(text) if any(c in text for c in ".eE") else int(text)
    except ValueError:
        return text


def _spec(args):
    spec = {"sets": {}, "geoms": {}, "parts": {}, "bodies": {}, "refine": [list(r) for r in args.refine]}
    if args.profile:
        spec["profile"] = args.profile
    for item in args.set:
        k, _, v = item.partition("=")
        spec["sets"][k] = _num(v)
    for item in args.part:
        k, _, v = item.partition("=")
        spec["parts"][k] = v
    for item in args.body:
        k, _, v = item.partition("=")
        spec["bodies"][k] = v
    if args.nets:
        spec["nets"] = args.nets.split(",")
    if args.ground_nets:
        spec["ground_nets"] = args.ground_nets.split(",")
    if getattr(args, "device", None):
        spec["pin"] = args.device
    return spec


def _run(coro):
    try:
        return asyncio.run(coro)
    except client.ClientError as e:
        raise SystemExit(f"error: {e}") from None


def main(argv=None, stdin=None, out=None):
    out = out or sys.stdout
    stdin = stdin or sys.stdin
    args = _parser().parse_args(argv)
    if args.cmd == "gui":  # the window runs this tool as child processes; it needs no TLS itself
        from .gui import main as gui_main

        return gui_main()

    def say(text=""):
        print(text, file=out)

    if args.cmd == "status":
        s = state()
        if args.json:
            say(json.dumps({"home": str(config.home()), "enabled": s}))
        else:
            say(f"farm state folder: {config.home()}")
            for role in config.ROLES:
                say(f"  {role:<12} {'on' if s[role] else 'off'}")
        return 0
    if args.cmd == "disable":
        config.set_enabled(args.role, False)
        say(f"farm {args.role} is off")
        return 0
    if args.cmd == "enable":
        say(CONSEQUENCE[args.role].format(home=config.home()))
        if not pki.tls13_available():
            say(
                "cannot enable: this Python has no TLS 1.3 ("
                + ssl.OPENSSL_VERSION
                + "); the farm requires it. "
                "Use a Python built with OpenSSL 1.1.1 or newer."
            )
            return 1
        if not args.yes:
            print(f"Type 'enable {args.role}' to confirm: ", end="", file=out)
            out.flush()
            if stdin.readline().strip() != "enable " + args.role:
                say("not enabled")
                return 1
        config.set_enabled(args.role, True)
        config.remember_python()
        say(f"farm {args.role} is on")
        return 0
    if args.cmd in ("agent", "setup"):  # they report a missing TLS 1.3 themselves, as a failed check
        return agent.run(args, out, stdin)
    if args.cmd == "local-worker":
        return local_worker.run(args, out, stdin)
    try:
        pki.min_tls()  # every other command talks TLS: stop early if this Python cannot do 1.3
    except pki.PkiError as e:
        raise SystemExit(f"error: {e}") from None
    try:
        return _farm_command(args, say)
    except pki.PkiError as e:  # a missing identity, no TLS 1.3, no openssl: one plain line, not a traceback
        raise SystemExit(f"error: {e}") from None


def _args_from_agent(args):
    """The `worker` arguments as `agent configure` saved them."""
    import types

    cfg = agent.load()
    if cfg is None:
        raise SystemExit("no agent configuration: run `emfdtd_farm.py agent configure ...` first")
    try:
        cfg = agent.validate(cfg)
    except agent.AgentError as e:
        raise SystemExit(f"the agent configuration is not valid: {e}") from None
    turn = cfg["turn"]
    ns = types.SimpleNamespace(**vars(args))
    ns.solver, ns.device = cfg["solver"], cfg["devices"]
    ns.lock_command = turn["lock_command"] if turn["mode"] == "lock" else []
    ns.no_lock = turn["mode"] == "none"
    ns.turn_from_coordinator = turn["mode"] == "coordinator"
    ns.max_minutes, ns.max_steps = cfg["max_minutes"], cfg["max_steps"]
    ns.max_input_mb, ns.max_tier = cfg["max_input_mb"], cfg["max_tier"]
    ns.watchdog, ns.work = cfg["watchdog"], cfg["work"]
    return ns


def _farm_command(args, say):
    cmd = args.cmd
    if cmd in ("init", "invite", "serve", "members", "revoke"):
        _need("coordinator")
    elif cmd == "enroll":
        if not (config.enabled("worker") or config.enabled("client")):
            raise SystemExit("turn on the worker or client role first: emfdtd-farm enable worker|client")
    elif cmd in ("devices", "worker"):
        _need("worker")
    else:
        _need("client")

    if cmd == "init":
        if not pki.ca_exists():
            pki.init_ca()
        bundle = pki.issue("coordinator", "main", hosts=args.host)
        pki.save_identity("coordinator", bundle)
        say("farm certificate authority ready.")
        fp = pki.show_fingerprint(pki.fingerprint(bundle["cert"]))
        say("coordinator fingerprint (give it to every device you enrol): " + fp)
        return 0
    if cmd == "members":
        policy_devices = policy.load()["devices"]
        rows = [
            {
                "key": key,
                "fingerprint": info["fingerprint"],
                "may": policy_devices.get(key, {}).get("may", []),
                "revoked": info["fingerprint"] in pki.revoked(),
            }
            for key, info in sorted(pki.members().items())
        ]
        if args.json:
            say(json.dumps({"members": rows}))
            return 0
        for row in rows:
            granted = ",".join(row["may"]) or "(removed)"
            say(f"{row['key']:<28} {row['fingerprint'][:16]}...  may: {granted}")
        return 0
    if cmd == "revoke":
        key = f"{args.role}:{args.name}"
        member = pki.members().get(key)
        if not member:
            raise SystemExit(f"{key} is not an enrolled device")
        pki.revoke(member["fingerprint"], f"{key} revoked by the user")
        policy.remove_device(args.role, args.name)
        say(f"{key} revoked: its certificate no longer works and its grants are gone")
        return 0
    if cmd == "invite":
        bundle = pki.load_identity("coordinator")
        code = enroll.create_invite(args.name, args.role, args.minutes)
        say(f"invitation for {args.role} {args.name!r}, valid {args.minutes} minutes, one use.")
        say(f"  code:        {code}")
        say(f"  fingerprint: {pki.show_fingerprint(pki.fingerprint(bundle['cert']))}")

        async def go():
            server = await enroll.serve(args.listen, args.port, bundle)
            say(f"  listening on {args.listen}:{server.sockets[0].getsockname()[1]} until it is used")
            while enroll.invite_active():
                await asyncio.sleep(0.5)
            server.close()
            await server.wait_closed()

        asyncio.run(go())
        say("invitation closed" + (": the device joined" if args.name in "".join(pki.members()) else ""))
        return 0
    if cmd == "serve":
        from .coordinator import Coordinator

        bundle = pki.load_identity("coordinator")

        async def go():
            c = Coordinator(
                bundle, args.listen, args.port, gates=_load_gates(), gates_loader=read_gates,
                checkpoint_every_s=args.checkpoint_every, hold_s=args.hold_minutes * 60,
            )  # fmt: skip
            await c.start()
            say(f"farm coordinator listening on {args.listen}:{c.port} (Ctrl-C or SIGTERM to stop)")
            stop = asyncio.Event()
            loop = asyncio.get_running_loop()
            for sig in (signal.SIGINT, signal.SIGTERM):
                with contextlib.suppress(NotImplementedError, ValueError):  # not on Windows loops
                    loop.add_signal_handler(sig, stop.set)
            await stop.wait()
            say("stopping: giving back any GPU turn")
            await c.stop()  # releases held turns and closes the listeners

        with contextlib.suppress(KeyboardInterrupt):
            asyncio.run(go())
        return 0
    if cmd == "enroll":
        host, port = _hostport(args.enrolment)
        chost, cport = _hostport(args.coordinator)
        try:
            bundle = asyncio.run(enroll.enroll(host, port, args.fingerprint, args.code))
        except enroll.EnrollError as e:
            raise SystemExit(f"enrolment failed: {e}") from None
        role = bundle.get("role", "worker")
        pki.save_identity(role, bundle)
        (config.home() / "coordinator.json").write_text(json.dumps({"host": chost, "port": cport}))
        nxt = "worker --solver PATH --device NAME" if role == "worker" else "queue"
        say(f"enrolled as {role} {bundle.get('name', '')}. Next: emfdtd-farm {nxt}")
        return 0
    if cmd == "devices":
        from .worker import Worker, parse_devices

        async def go():
            w = Worker(
                {},
                "",
                0,
                args.solver,
                Path(args.work) if args.work else config.home() / "work",
                [],
                lock_command=args.lock_command,
                no_lock=args.no_lock,
            )
            code, text = await w._capture("--list-devices", gpu=True)
            return w, parse_devices(text)

        worker, found = asyncio.run(go())
        if args.save:
            from .worker import choose

            if not args.device:
                raise SystemExit("--save needs --device (the GPUs to record)")
            chosen = choose(found, args.device)
            worker.work_root.mkdir(parents=True, exist_ok=True)
            worker.save_devices(chosen)
            say(f"recorded {len(chosen)} device(s) in {worker._devices_file()}")
        for d in found:
            gib = d["memory_bytes"] / (1 << 30)
            vk = f"{d['vulkan'][0]}.{d['vulkan'][1]}"
            note = "" if d["usable"] else "  (not usable)"
            say(f"{d['uuid']}  {d['name']}  {gib:.1f} GiB  vulkan {vk}  {d['driver']}{note}")
        return 0
    if cmd == "worker":
        from .worker import Worker

        if args.from_agent:
            args = _args_from_agent(args)
        if not args.solver:
            raise SystemExit("give --solver (the emfdtd binary), or --from-agent")
        if not args.device:
            raise SystemExit(
                "choose the GPUs to offer with --device (see `emfdtd-farm devices --solver ...`)"
            )
        if sum([bool(args.lock_command), args.no_lock, args.turn_from_coordinator]) != 1:
            raise SystemExit(
                "give exactly one of: --lock-command (a command that waits for this GPU's turn and runs what "
                "follows it), --turn-from-coordinator (the coordinator holds the turn per job), or, only if "
                "nobody else uses this GPU, --no-lock"
            )
        caps = {}
        for key, value in (
            ("max_minutes", args.max_minutes),
            ("max_steps", args.max_steps),
            ("max_input_bytes", args.max_input_mb * policy.MIB if args.max_input_mb else None),
            ("max_tier", args.max_tier),
        ):
            if value is not None:
                caps[key] = value
        host, port = _connection()
        w = Worker(
            pki.load_identity("worker"), host, port, args.solver,
            Path(args.work) if args.work else config.home() / "work", args.device, args.lock_command,
            caps=caps, no_lock=args.no_lock, turn_from_coordinator=args.turn_from_coordinator,
            watchdog_s=args.watchdog, status_file=agent.status_file(),
        )  # fmt: skip
        try:
            w.recorded_devices()  # fail now, with the way to fix it, not as a traceback after connecting
        except RuntimeError as e:
            raise SystemExit(str(e)) from None
        say(f"worker connecting out to {host}:{port} (Ctrl-C or SIGTERM to stop)")

        async def run_worker():
            task = asyncio.ensure_future(w.run_forever())
            loop = asyncio.get_running_loop()

            stopping = []

            async def graceful():
                await w.graceful_stop()  # a solver that can checkpoint writes one and leaves: the job pauses
                task.cancel()

            def stop_now():
                if stopping:  # a second signal: no more waiting
                    w.stop()
                    task.cancel()
                    return
                stopping.append(True)
                asyncio.ensure_future(graceful())

            for sig in (signal.SIGINT, signal.SIGTERM):
                with contextlib.suppress(NotImplementedError, ValueError):  # not on Windows loops
                    loop.add_signal_handler(sig, stop_now)

            async def watch_stop_file():
                while not task.done():
                    if agent.stop_file().exists():
                        stop_now()
                        return
                    await asyncio.sleep(0.5)

            watcher = asyncio.ensure_future(watch_stop_file()) if args.from_agent else None
            with contextlib.suppress(asyncio.CancelledError):
                await task
            if watcher:
                watcher.cancel()

        asyncio.run(run_worker())
        w.stop()
        return 0

    # client commands
    host, port = _connection()
    bundle = pki.load_identity("client")
    if cmd == "queue":
        st = _run(client.status(bundle, host, port))
        if args.json:
            say(json.dumps(st))
            return 0
        say("devices:")
        for d in st["devices"]:
            say(f"  {d['worker']:<14} {d['name']:<28} {d['state']:<8} {d['mcells_per_s']:.0f} Mcells/s")
        say("your jobs:")
        for j in st["jobs"]:
            pct = f"{j['percent']:>3}%" if j.get("percent") is not None else "    "
            note = (
                j["reason"]
                if j["state"] in ("paused", "failed", "cancelled")
                else (j["progress"] or j["reason"])
            )
            say(f"  {j['job']}  {j['state']:<10} {pct} {j['label']}  {note}")
        return 0
    if cmd == "cancel":
        _run(client.cancel(bundle, host, port, args.job))
        say("cancel requested")
        return 0
    if cmd in ("resume", "restart"):
        try:
            if cmd == "resume":
                _run(client.resume(bundle, host, port, args.job, args.max_minutes))
            else:
                _run(client.restart(bundle, host, port, args.job))
        except client.ClientError as e:
            if args.json:
                say(json.dumps({"ok": False, "error": str(e)}))
            else:
                print(f"error: {e}", file=sys.stderr)
            return 1
        say(
            json.dumps({"ok": True, "job": args.job})
            if args.json
            else (
                "resume requested: it continues on the worker that holds its checkpoint"
                if cmd == "resume"
                else "restart requested: it starts again from step 0"
            )
        )
        return 0
    if cmd == "results":
        got = _run(client.fetch(bundle, host, port, args.job, args.dest))
        for _, path in sorted(got.items()):
            say(path)
        return 0
    # submit
    spec = _spec(args)
    specs, group = [spec], None
    if args.sweep:
        key, _, values = args.sweep.partition("=")
        specs = jobs.expand_sweep(spec, key, [_num(v) for v in values.split(",")])
        group = "sweep-" + Path(args.snapshot).stem[:20]
    if not args.no_estimate:
        try:
            est, why = estimate.estimate_job(estimate.default_solver(), args.snapshot, specs[0])
        except RuntimeError as e:
            est, why = None, str(e)
        if est:  # the first point stands for a sweep: its points differ little in size
            for sp in specs:
                sp["estimate"] = est
        else:
            print(f"no time estimate for the job ({why}); it is placed without one", file=sys.stderr)
    ids = []
    for sp in specs:
        ids.append(_run(client.submit(bundle, host, port, sp, args.snapshot, args.label, group)))
        if not args.json:
            say(ids[-1])
    if args.json:
        say(json.dumps({"jobs": ids}))
    if args.wait:
        import time

        pending = set(ids)
        while pending:
            st = _run(client.status(bundle, host, port))
            for j in st["jobs"]:
                if j["job"] in pending and j["state"] in ("done", "failed", "cancelled"):
                    pending.discard(j["job"])
                    if j["state"] == "done":
                        _run(client.fetch(bundle, host, port, j["job"], str(Path(args.wait) / j["job"])))
                    say(f"{j['job']} {j['state']} {j['reason']}")
            if pending:
                time.sleep(2)
    return 0
