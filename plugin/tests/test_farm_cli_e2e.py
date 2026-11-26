import json
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest
from pytest_bdd import given, scenarios, then, when

scenarios("features/farm_cli_e2e.feature")

TOOL = str(Path(__file__).resolve().parent.parent / "tools" / "emfdtd_farm.py")
STUB = str(Path(__file__).parent / "farm_stub" / "stub_solver.py")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Machines:
    def __init__(self, tmp):
        self.tmp = tmp
        self.homes = {n: tmp / n for n in ("coord", "worker", "client")}
        self.procs = []
        self.out = {}

    def run(self, who, *args, timeout=60, input=None):
        env = {**os.environ, "EMFDTD_FARM_HOME": str(self.homes[who])}
        r = subprocess.run([sys.executable, TOOL, *args], env=env, capture_output=True, text=True,
                           timeout=timeout, input=input)  # fmt: skip
        assert r.returncode == 0, f"{who} {args}: {r.stdout}\n{r.stderr}"
        return r.stdout

    def spawn(self, who, *args):
        env = {**os.environ, "EMFDTD_FARM_HOME": str(self.homes[who])}
        p = subprocess.Popen([sys.executable, "-u", TOOL, *args], env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True)  # fmt: skip
        self.procs.append(p)
        return p

    def close(self):
        for p in self.procs:
            p.terminate()
        for p in self.procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()


@pytest.fixture
def m(tmp_path):
    machines = Machines(tmp_path)
    yield machines
    machines.close()


def _read_until(proc, pattern, timeout=15):
    end = time.time() + timeout
    seen = ""
    while time.time() < end:
        line = proc.stdout.readline()
        seen += line
        if re.search(pattern, line):
            return seen
    raise AssertionError(f"never saw {pattern!r}; got:\n{seen}")


def _invite(m, name, role):
    port = free_port()
    p = m.spawn("coord", "invite", name, "--role", role, "--listen", "127.0.0.1", "--port", str(port))
    text = _read_until(p, r"listening on")
    code = re.search(r"code:\s+(\S+)", text).group(1)
    fp = re.search(r"fingerprint:\s+(\S+)", text).group(1)
    return p, port, code, fp


@given("three machines with their own farm folders")
def _machines(m):
    assert len(m.homes) == 3


@when("the coordinator machine enables its role, initialises and opens an invitation for a worker")
def _setup_coord(m):
    m.run("coord", "enable", "coordinator", "--yes")
    m.run("coord", "init", "--host", "127.0.0.1")
    m.serve_port = free_port()
    m.invite = _invite(m, "gpu-box", "worker")


@when("the worker machine enrols with that code and fingerprint")
def _enrol_worker(m):
    p, port, code, fp = m.invite
    m.run("worker", "enable", "worker", "--yes")
    out = m.run("worker", "enroll", f"127.0.0.1:{port}", "--fingerprint", fp, "--code", code,
                "--coordinator", f"127.0.0.1:{m.serve_port}")  # fmt: skip
    assert "enrolled as worker" in out
    p.wait(15)


@when("the coordinator opens an invitation for a client and the client machine enrols")
def _enrol_client(m):
    p, port, code, fp = _invite(m, "alice", "client")
    m.run("client", "enable", "client", "--yes")
    out = m.run("client", "enroll", f"127.0.0.1:{port}", "--fingerprint", fp, "--code", code,
                "--coordinator", f"127.0.0.1:{m.serve_port}")  # fmt: skip
    assert "enrolled as client" in out
    p.wait(15)


@when("the coordinator serves and the worker connects out")
def _serve(m):
    p = m.spawn("coord", "serve", "--listen", "127.0.0.1", "--port", str(m.serve_port))
    _read_until(p, r"listening on")
    m.spawn("worker", "worker", "--solver", STUB, "--device", "stub gpu a", "--no-lock")
    time.sleep(1.5)


@when("the client submits a sweep of 2 values and waits for the results")
def _submit(m):
    snap = m.tmp / "board.json"
    snap.write_text(json.dumps({"stub": {}}))
    m.dest = m.tmp / "results"
    m.out["submit"] = m.run("client", "submit", str(snap), "--profile", "bluetooth", "--sweep",
                            "mesh_preset=coarse,medium", "--wait", str(m.dest), timeout=120)  # fmt: skip


@then("the client has the results of both jobs")
def _results(m):
    runs = sorted(p for p in m.dest.iterdir() if p.is_dir())
    assert len(runs) == 2
    for r in runs:
        assert (r / "s11.s1p").exists() and (r / "summary.txt").exists()
    texts = sorted((r / "settings.json").read_text() for r in runs)
    assert "mesh_preset=coarse" in texts[0] and "mesh_preset=medium" in texts[1]
    assert m.out["submit"].count(" done") == 2


@then("the coordinator lists the two enrolled devices")
def _members(m):
    out = m.run("coord", "members")
    assert "worker:gpu-box" in out and "client:alice" in out


@then("revoking the worker removes it from the farm")
def _revoke(m):
    out = m.run("coord", "revoke", "worker", "gpu-box")
    assert "revoked" in out
    assert "(removed)" in m.run("coord", "members")
