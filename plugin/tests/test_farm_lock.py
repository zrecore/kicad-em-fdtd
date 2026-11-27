import asyncio
import json
import subprocess
import time

from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm import cli, client, config, gpulock
from emfdtd_plugin.farm.worker import Worker, parse_devices
from farm_stub.paths import STUB

scenarios("features/farm_lock.feature")


class Mgr:
    """A lock manager on its own event loop, with named holders."""

    def __init__(self):
        self.loop = asyncio.new_event_loop()
        self.locks = gpulock.GpuLocks()
        self.tasks, self.holds = {}, {}

    def run(self, coro):
        return self.loop.run_until_complete(coro)

    def ask(self, who, name, max_s=60):
        self.tasks[who] = self.loop.create_task(self.locks.acquire(name, who, max_s))
        self.run(asyncio.sleep(0.05))

    def holder(self, who):
        t = self.tasks[who]
        if t.done() and not t.cancelled() and who not in self.holds:
            self.holds[who] = t.result()
        return self.holds.get(who)


@given("a lock manager", target_fixture="mgr")
def _mgr():
    m = Mgr()
    yield m
    m.loop.close()


@when(parsers.parse('"{a}" asks for the lock "{name}" and then "{b}" does'))
def _two_ask(mgr, a, name, b):
    mgr.ask(a, name)
    mgr.ask(b, name)


@when(parsers.parse('"{a}" asks for the lock "{n1}" and then "{b}" asks for the lock "{n2}"'))
def _two_locks(mgr, a, n1, b, n2):
    mgr.ask(a, n1)
    mgr.ask(b, n2)


@when(parsers.parse('"{a}" asks for the lock "{name}" for {s:f} seconds and then "{b}" does'))
def _short(mgr, a, name, s, b):
    mgr.ask(a, name, s)
    mgr.ask(b, name)
    mgr.run(asyncio.sleep(s + 0.3))


@when(parsers.parse('"{a}" asks for the lock "{name}" and then "{b}" does and "{c}" gives up'))
def _giveup(mgr, a, name, b, c):
    mgr.ask(a, name)
    mgr.ask(b, name)
    mgr.tasks[c].cancel()
    mgr.run(asyncio.sleep(0.05))


@when(parsers.parse('"{c}" asks for the lock "{name}" and "{a}" gives the turn back'))
def _third(mgr, c, name, a):
    mgr.ask(c, name)
    mgr.holder(a).release()
    mgr.run(asyncio.sleep(0.05))


@when(parsers.parse('"{a}" gives the turn back'))
def _release(mgr, a):
    mgr.holder(a).release()
    mgr.run(asyncio.sleep(0.05))


@then(parsers.parse('"{a}" holds the turn and "{b}" waits'))
def _first_waits(mgr, a, b):
    assert mgr.holder(a) is not None and mgr.holder(b) is None
    assert mgr.locks.status()[0]["waiting"] == [b]


@then(parsers.parse('"{a}" holds the turn'))
def _holds(mgr, a):
    assert mgr.holder(a) is not None


@then("both hold their turn at once")
def _both(mgr):
    assert mgr.holder("first") is not None and mgr.holder("second") is not None


@then(parsers.parse('the turn of "{a}" is lost and "{b}" holds the turn'))
def _lost(mgr, a, b):
    assert mgr.holder(a).lost.done()
    assert mgr.holder(b) is not None


@given(parsers.parse('gates for "{w}" and for its GPU "{part}"'))
def _two_gates(farm, w, part):
    farm.coord.gates = {w: {"lock": "whole"}, f"{w}:{part}": {"lock": "one"}}


@then("the gate of that GPU is the specific one and any other device gets the worker's gate")
def _specific(farm):
    end = time.time() + 10
    while time.time() < end and "gpu-box" not in farm.coord.sched.workers:
        time.sleep(0.1)
    devs = farm.coord.sched.workers["gpu-box"]["devices"]
    uuid_ = next(iter(devs))
    assert farm.coord._gate_for("gpu-box", uuid_) == {"lock": "one"}
    farm.coord.gates = {"gpu-box": {"lock": "whole"}, "gpu-box:no such card": {"lock": "one"}}
    assert farm.coord._gate_for("gpu-box", uuid_) == {"lock": "whole"}
    assert farm.coord._gate_for("other", uuid_) is None


@given(parsers.parse('the coordinator keeps the turn of "{name}" in its own lock "{lock}"'))
def _own(farm, name, lock):
    farm.coord.gates[name] = {"lock": lock}
    farm.seen_held = []
    real = farm.coord.locks.acquire

    async def spy(n, label, max_s):
        h = await real(n, label, max_s)
        farm.seen_held.append(n)
        return h

    farm.coord.locks.acquire = spy


@given(parsers.parse('a worker "{name}" offering "{allow}" that relies on the coordinator\'s turn'))
def _turn_worker(farm, name, allow):
    out = subprocess.run([STUB, "--list-devices"], capture_output=True, text=True, check=True).stdout
    work = farm.tmp / ("work-" + name)
    work.mkdir(parents=True, exist_ok=True)
    Worker({}, "", 0, STUB, work, [], no_lock=True).save_devices(
        [{**d, "vulkan": list(d["vulkan"])} for d in parse_devices(out)]
    )
    farm.add_worker(name, allow, turn_from_coordinator=True)


@then(parsers.parse('the own lock "{lock}" was held during the job and is free again'))
def _was_held(farm, lock):
    assert farm.seen_held == [lock]
    end = time.time() + 5
    while time.time() < end and (farm.coord.turns or farm.coord.locks.status()):
        time.sleep(0.1)
    assert not farm.coord.turns and farm.coord.locks.status() == []


@then("a gate with a lock name is accepted and a gate with both or neither is refused")
def _validate(monkeypatch):
    def read(obj):
        (config.home()).mkdir(parents=True, exist_ok=True)
        (config.home() / "gates.json").write_text(json.dumps(obj))
        return cli.read_gates()

    assert read({"w:gpu": {"lock": "x"}}) == {"w:gpu": {"lock": "x"}}
    for bad in ({"w": {}}, {"w": {"lock": "x", "hold": ["a"], "release": ["b"]}}, {"w": {"lock": ""}}):
        try:
            read(bad)
        except cli.GatesError:
            continue
        raise AssertionError(f"accepted {bad}")


# --- the lock queue over the network ----------------------------------------------------------------------


async def _open(farm, who, name, minutes=5):
    from emfdtd_plugin.farm import client, protocol

    reader, writer = await client._connect(farm.clients[who], "127.0.0.1", farm.coord.port)
    writer.write(protocol.encode({"type": "lock_hold", "lock": name, "label": who, "max_minutes": minutes}))
    await writer.drain()
    return reader, writer


async def _read(reader):
    from emfdtd_plugin.farm import protocol

    return await asyncio.wait_for(protocol.read_message(reader), 10)


@when(parsers.parse('"{a}" takes the lock "{name}" and "{b}" asks for it'))
def _two_net(farm, a, name, b):
    async def go():
        ra, wa = await _open(farm, a, name)
        farm.net = {a: (ra, wa)}
        assert (await _read(ra))["type"] == "lock_queued"
        assert (await _read(ra))["type"] == "lock_granted"
        rb, wb = await _open(farm, b, name)
        farm.net[b] = (rb, wb)
        farm.queued = await _read(rb)

    farm.run(go())


@then(parsers.parse('"{b}" is told one turn is ahead and waits'))
def _told(farm, b):
    assert farm.queued == {"type": "lock_queued", "ahead": 1}


@when(parsers.parse('"{a}" hangs up'))
def _hang(farm, a):
    farm.net[a][1].close()


@then(parsers.parse('"{b}" is granted the turn'))
def _granted(farm, b):
    assert farm.run(_read(farm.net[b][0])) == {"type": "lock_granted"}


@then(parsers.parse('the farm status lists the lock "{name}" held by "{who}"'))
def _listed(farm, name, who):
    from emfdtd_plugin.farm import client

    st = farm.run(client.status(farm.clients[who], "127.0.0.1", farm.coord.port))
    held = [lk for lk in st["locks"] if lk["lock"] == name]
    assert held and held[0]["holder"] == who and held[0]["waiting"] == []
    farm.net[who][1].close()


@then(parsers.parse('asking for the lock "{name}" is refused'))
def _refused(farm, name):
    async def go():
        r, w = await _open(farm, "alice", name)
        msg = await _read(r)
        w.close()
        return msg

    msg = farm.run(go())
    assert msg["type"] == "refused" and "cpu:MACHINE" in msg["reason"]


@when(parsers.parse('"{who}" runs a command inside the lock "{name}"'))
def _run_cmd(farm, who, name):
    import sys

    from emfdtd_plugin.farm import client

    marker = farm.tmp / "ran.txt"
    code = f"import pathlib;pathlib.Path({str(marker)!r}).write_text('x');raise SystemExit(7)"
    farm.exit = farm.run(
        client.lock(
            farm.clients[who],
            "127.0.0.1",
            farm.coord.port,
            name,
            "t",
            5,
            [sys.executable, "-c", code],
            lambda *_: None,
        )
    )
    farm.marker = marker


@then("the command ran, its exit code came back and the lock is free")
def _ran(farm):
    assert farm.marker.read_text() == "x" and farm.exit == 7
    end = time.time() + 5
    while time.time() < end and farm.coord.locks.status():
        time.sleep(0.1)
    assert farm.coord.locks.status() == []


@given("a clean command line parser")
def _parser_given():
    pass


@then(
    parsers.parse('"{line}" parses as lock {name}, {mins:d} minutes, label {label} and the command {command}')
)
def _parse_lock(monkeypatch, line, name, mins, label, command):
    seen = {}

    async def fake(bundle, host, port, n, lab, m, cmd, say=print):
        seen.update(name=n, label=lab, mins=m, cmd=cmd)
        return 0

    monkeypatch.setattr(cli.pki, "min_tls", lambda: None)
    monkeypatch.setattr(cli.pki, "load_identity", lambda role: {})
    monkeypatch.setattr(cli.client, "lock", fake)
    monkeypatch.setattr(cli, "_connection", lambda: ("127.0.0.1", 1))
    monkeypatch.setattr(cli.config, "enabled", lambda role: True)
    assert cli.main(line.split()) == 0
    assert seen == {"name": name, "label": label, "mins": mins, "cmd": command.split()}


# --- two GPUs of one machine ------------------------------------------------------------------------------


@given(parsers.parse('the coordinator keeps each GPU of "{name}" in its own lock'))
def _own_locks(farm, name):
    farm.coord.gates = {f"{name}:stub gpu a": {"lock": "gpu:a"}, f"{name}:stub gpu b": {"lock": "gpu:b"}}


@given(parsers.parse('the coordinator keeps both GPUs of "{name}" in one shared lock'))
def _shared_lock(farm, name):
    farm.coord.gates = {name: {"lock": "gpu:shared"}}


def _running(farm):
    return [j for j in farm.call(client.status, farm.who)["jobs"] if j["state"] == "running"]


@then(parsers.parse('both jobs run at once on "{name}" on two different GPUs holding two different locks'))
def _two_at_once(farm, name):
    end = time.time() + 30
    while time.time() < end:
        running = _running(farm)
        if len(running) == 2:
            assert {j["worker"] for j in running} == {name}
            assert len({j["device"] for j in running}) == 2
            held = {lk["lock"] for lk in farm.coord.locks.status() if lk["holder"]}
            assert held == {"gpu:a", "gpu:b"}, held
            return
        time.sleep(0.05)
    raise AssertionError("two jobs were never running together on one worker")


@then(parsers.parse('at most one job ever holds a turn at a time on "{name}"'))
def _one_at_a_time(farm, name):
    # a job shows as running while it waits for its lock, so count the turns that are actually held
    most, end = 0, time.time() + 60
    while time.time() < end:
        most = max(most, len(farm.coord.turns))
        jobs_ = farm.call(client.status, farm.who)["jobs"]
        if all(j["state"] in ("done", "failed") for j in jobs_):
            break
        time.sleep(0.02)
    assert most == 1, most


@then("both jobs finish and no lock stays held")
def _both_done(farm):
    end = time.time() + 60
    while time.time() < end:
        jobs_ = farm.call(client.status, farm.who)["jobs"]
        if all(j["state"] in ("done", "failed") for j in jobs_):
            break
        time.sleep(0.1)
    assert [j["state"] for j in jobs_] == ["done", "done"], jobs_
    end = time.time() + 10
    while time.time() < end and (farm.coord.turns or any(lk["holder"] for lk in farm.coord.locks.status())):
        time.sleep(0.1)
    assert not farm.coord.turns and not any(lk["holder"] for lk in farm.coord.locks.status())
