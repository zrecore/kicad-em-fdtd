import asyncio
import json
import socket
import ssl
import time

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm import enroll, pki

scenarios("features/farm_pki.feature", "features/farm_enroll.feature")


@pytest.fixture
def ctx():
    return {}


@pytest.fixture(autouse=True)
def _no_fail_delay(monkeypatch):
    monkeypatch.setattr(enroll, "FAIL_DELAY", 0.0)


def _home(monkeypatch, tmp_path, name="farm"):
    monkeypatch.setenv("EMFDTD_FARM_HOME", str(tmp_path / name))


async def _connect(server_bundle, client_bundle):
    """Handshake over loopback. Returns (identity seen by the server or 'none', 'ok' or 'failed')."""
    seen = {}

    async def handle(reader, writer):
        seen["identity"] = pki.peer_identity(writer.get_extra_info("ssl_object"))
        seen["version"] = writer.get_extra_info("ssl_object").version()
        writer.write(b"ok")
        await writer.drain()
        writer.close()

    srv = await asyncio.start_server(handle, "127.0.0.1", 0, ssl=pki.server_context(server_bundle))
    port = srv.sockets[0].getsockname()[1]
    try:
        if client_bundle is None:  # trusts the CA but presents no certificate
            cctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            cctx.minimum_version = pki.min_tls()
            cctx.check_hostname = False
            cctx.load_verify_locations(cadata=server_bundle["ca"])
        else:
            cctx = pki.client_context(client_bundle)
            cctx.load_verify_locations(cadata=server_bundle["ca"])  # trust this coordinator's CA
        try:
            reader, writer = await asyncio.open_connection("127.0.0.1", port, ssl=cctx)
            data = await asyncio.wait_for(reader.read(2), 5)
            writer.close()
        except (ssl.SSLError, ConnectionError, OSError):
            return seen.get("identity", "none"), "failed"
        _connect.version = seen.get("version")
        return seen.get("identity", "none"), "ok" if data == b"ok" else "failed"
    finally:
        srv.close()
        await srv.wait_closed()


# --- farm set-up ---------------------------------------------------------------------------------


@given("a farm with a coordinator")
def _coordinator(ctx, monkeypatch, tmp_path):
    _home(monkeypatch, tmp_path)
    pki.init_ca()
    ctx["coord"] = pki.issue("coordinator", "main", hosts=["127.0.0.1"])


@given(parsers.parse('a farm with a coordinator and a worker named "{name}"'))
def _farm(ctx, monkeypatch, tmp_path, name):
    _coordinator(ctx, monkeypatch, tmp_path)
    ctx["worker"] = pki.issue("worker", name)


@given("a worker certificate issued by a different farm")
def _other_farm(ctx, monkeypatch, tmp_path):
    _home(monkeypatch, tmp_path, "other")
    pki.init_ca()
    ctx["worker"] = pki.issue("worker", "win4060")
    _home(monkeypatch, tmp_path)  # back to the real farm


@given("the worker's certificate is revoked")
def _revoke(ctx):
    pki.revoke(pki.fingerprint(ctx["worker"]["cert"]))


# --- certificates --------------------------------------------------------------------------------


@when("the worker connects to the coordinator")
def _worker_connects(ctx):
    ctx["identity"], ctx["result"] = asyncio.run(_connect(ctx["coord"], ctx["worker"]))


@when("a client with no certificate connects")
def _anon(ctx):
    ctx["identity"], ctx["result"] = asyncio.run(_connect(ctx["coord"], None))


@when("a second CA is initialised")
def _second_ca(ctx):
    try:
        pki.init_ca()
    except pki.PkiError as e:
        ctx["error"] = str(e)


@when(parsers.parse('a certificate is requested for the name "{name}"'))
def _bad_name(ctx, name):
    try:
        pki.issue("worker", name)
    except pki.PkiError as e:
        ctx["error"] = str(e)


@then(parsers.parse('the coordinator sees the identity "{role}" "{name}"'))
def _sees(ctx, role, name):
    assert ctx["result"] == "ok"
    assert ctx["identity"] == (role, name)


@then("the connection used TLS 1.3")
def _tls13():
    assert _connect.version == "TLSv1.3"


@when("the Python has no TLS 1.3")
def _no_tls13(ctx, monkeypatch):
    monkeypatch.setattr(pki.ssl, "HAS_TLSv1_3", False)
    try:
        pki.server_context({"cert": "", "key": "", "ca": ""})
    except pki.PkiError as e:
        ctx["error"] = str(e)


@then("the connection fails")
def _fails(ctx):
    assert ctx["result"] == "failed"
    assert ctx["identity"] == "none"


@then("the coordinator sees no identity")
def _none(ctx):
    assert ctx["identity"] is None


@then("it is refused")
def _refused(ctx):
    assert "error" in ctx


@then("the coordinator fingerprint survives printing and parsing")
def _fp(ctx):
    fp = pki.fingerprint(ctx["coord"]["cert"])
    assert len(fp) == 64
    assert pki.parse_fingerprint(pki.show_fingerprint(fp).upper()) == fp


@then("a short or non-hex fingerprint is refused")
def _fp_bad(ctx):
    for bad in ("abcd", "zz" * 32, ""):
        with pytest.raises(pki.PkiError):
            pki.parse_fingerprint(bad)


# --- enrolment -----------------------------------------------------------------------------------


@given(parsers.parse('an invitation for the worker "{name}"'))
def _invite(ctx, name):
    ctx["code"] = enroll.create_invite(name, "worker", attempts=2)


@given(parsers.parse('an invitation for the worker "{name}" that has expired'))
def _invite_expired(ctx, name):
    ctx["code"] = enroll.create_invite(name, "worker", attempts=2)
    inv = enroll._read()
    inv["expires"] = time.time() - 1
    enroll._write(inv)


async def _enrol_once(ctx, code, fp):
    server = await enroll.serve("127.0.0.1", 0, ctx["coord"])
    port = server.sockets[0].getsockname()[1]
    try:
        return await enroll.enroll("127.0.0.1", port, fp, code)
    finally:
        server.close()
        await server.wait_closed()


def _try(ctx, code, fp=None, times=1):
    fp = fp or pki.fingerprint(ctx["coord"]["cert"])
    ctx.pop("error", None)
    for _ in range(times):
        try:
            ctx["bundle"] = asyncio.run(_enrol_once(ctx, code, fp))
        except enroll.EnrollError as e:
            ctx["error"] = str(e)


@when("the device enrols with the right code and the right fingerprint")
def _right(ctx):
    _try(ctx, ctx["code"])


@when("the device enrols with a wrong code")
def _wrong(ctx):
    _try(ctx, "AAAA-AAAA-AAAA")


@when("the device enrols with a wrong code twice")
def _wrong2(ctx):
    _try(ctx, "AAAA-AAAA-AAAA", times=2)


@when("the device enrols with the right code and a wrong fingerprint")
def _wrong_fp(ctx):
    _try(ctx, ctx["code"], fp="00" * 32)


@when("another device tries the same code")
def _again(ctx):
    ctx["first"] = ctx.get("bundle")
    _try(ctx, ctx["code"])


@when("the enrolment endpoint runs and the device enrols")
def _endpoint(ctx):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    fp = pki.fingerprint(ctx["coord"]["cert"])

    async def go():
        task = asyncio.create_task(enroll.run_until_done("127.0.0.1", port, ctx["coord"], poll=0.05))
        await asyncio.sleep(0.3)
        ctx["bundle"] = await enroll.enroll("127.0.0.1", port, fp, ctx["code"])
        await asyncio.wait_for(task, 5)
        try:
            await asyncio.open_connection("127.0.0.1", port)
            ctx["listening"] = True
        except OSError:
            ctx["listening"] = False

    asyncio.run(go())


@then(parsers.parse('the device holds a certificate that the coordinator accepts as "{role}" "{name}"'))
def _bundle_works(ctx, role, name):
    identity, result = asyncio.run(_connect(ctx["coord"], ctx["bundle"]))
    assert result == "ok" and identity == (role, name)


@then("the invitation is used up")
def _used(ctx):
    assert not enroll.invite_active()


@then(parsers.parse('enrolment fails with "{text}"'))
def _enroll_fails(ctx, text):
    assert text in ctx.get("error", ""), ctx.get("error")
    assert "bundle" not in ctx or ctx.get("first") is not None or ctx["bundle"] is None


@then(parsers.parse("the invitation has {n:d} attempt left"))
@then(parsers.parse("the invitation has {n:d} attempts left"))
def _attempts(n):
    assert json.loads(enroll._file().read_text())["attempts_left"] == n


@then("the invitation is no longer active")
def _inactive():
    assert not enroll.invite_active()


@then("enrolling with the right code now fails")
def _now_fails(ctx):
    _try(ctx, ctx["code"])
    assert "no invitation" in ctx["error"]


@then("the endpoint has stopped listening")
def _stopped(ctx):
    assert ctx["bundle"] is not None
    assert ctx["listening"] is False
