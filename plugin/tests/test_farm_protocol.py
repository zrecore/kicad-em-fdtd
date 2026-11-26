import asyncio
import json
import struct

import pytest
from pytest_bdd import parsers, scenarios, then, when

from emfdtd_plugin.farm import protocol

scenarios("features/farm_protocol.feature")


@pytest.fixture
def ctx():
    return {}


def _frame(obj):
    body = json.dumps(obj).encode()
    return struct.pack(">I", len(body)) + body


def _read(raw):
    async def go():
        reader = asyncio.StreamReader()
        reader.feed_data(raw)
        reader.feed_eof()
        return await protocol.read_message(reader)

    return asyncio.run(go())


BAD = {
    "a frame longer than the cap": struct.pack(">I", protocol.MAX_CONTROL_FRAME + 1) + b"x",
    "a zero-length frame": struct.pack(">I", 0),
    "a frame that is not JSON": struct.pack(">I", 3) + b"abc",
    "a JSON array": _frame([1, 2]),
    "an unknown message type": _frame({"type": "run_shell", "cmd": "rm"}),
    "a message missing a field": _frame({"type": "accept", "job": "j"}),
    "a number given as true": _frame(
        {
            "type": "offer",
            "job": "j",
            "lease": "l",
            "expires": True,
            "spec": {},
            "device": "d",
            "input_sha256": "0",
            "input_bytes": 1,
        }
    ),
    "a truncated frame body": struct.pack(">I", 50) + b"{}",
}


@when("a heartbeat message is framed and read back")
def _round(ctx):
    msg = {"type": "heartbeat", "devices": [{"uuid": "aa", "ok": True}]}
    ctx["sent"] = msg
    ctx["got"] = _read(protocol.encode(msg))


@then("the same message comes out")
def _same(ctx):
    assert ctx["got"] == ctx["sent"]


@when(parsers.parse("a peer sends {bad}"))
def _send_bad(ctx, bad):
    try:
        _read(BAD[bad])
    except protocol.ProtocolError as e:
        ctx["error"] = e


@then("the reader raises a protocol error")
def _raised(ctx):
    assert "error" in ctx


@when("a peer closes without sending anything")
def _empty(ctx):
    ctx["got"] = _read(b"")


@then("the reader returns nothing")
def _none(ctx):
    assert ctx["got"] is None
