"""The submitting side: connect with the client certificate, send a job, ask for status, fetch results."""

import asyncio
import hashlib
import json
import ssl
from pathlib import Path

from . import pki, protocol


class ClientError(Exception):
    pass


async def _connect(bundle, host, port):
    ctx = pki.client_context(bundle)
    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port, ssl=ctx), 15)
    except (OSError, ssl.SSLError, asyncio.TimeoutError) as e:
        raise ClientError(f"could not reach the coordinator: {e}") from None
    ident = pki.peer_identity(writer.get_extra_info("ssl_object"))
    if ident is None or ident[0] != "coordinator":
        writer.close()
        raise ClientError("the server is not a coordinator of this farm")
    return reader, writer


async def _ask(bundle, host, port, message):
    reader, writer = await _connect(bundle, host, port)
    try:
        writer.write(protocol.encode(message))
        await writer.drain()
        reply = await asyncio.wait_for(protocol.read_message(reader), 30)
    except (protocol.ProtocolError, ConnectionError, ssl.SSLError, asyncio.TimeoutError) as e:
        raise ClientError(f"the coordinator conversation failed: {e}") from None
    finally:
        writer.close()
    if reply is None:
        raise ClientError("the coordinator closed the connection")
    if reply["type"] == "refused":
        raise ClientError(reply["reason"])
    return reply


async def submit(bundle, host, port, spec, snapshot_path, label="", group=None):
    """Send the job and its snapshot. Returns the job id."""
    data = Path(snapshot_path).read_bytes()
    msg = {
        "type": "submit",
        "spec": spec,
        "input_sha256": hashlib.sha256(data).hexdigest(),
        "input_bytes": len(data),
        "label": label,
    }
    if group:
        msg["group"] = group
    job = (await _ask(bundle, host, port, msg))["job"]
    reader, writer = await _connect(bundle, host, port)
    try:
        writer.write(protocol.encode({"type": "transfer_open", "job": job, "what": "input_in"}))
        await protocol.send_blob(writer, "input.json", str(snapshot_path))
        ack = await asyncio.wait_for(protocol.read_message(reader), 60)
    except (protocol.ProtocolError, ConnectionError, ssl.SSLError, asyncio.TimeoutError) as e:
        raise ClientError(f"sending the input failed: {e}") from None
    finally:
        writer.close()
    if not ack or ack["type"] != "blob_ack" or not ack["ok"]:
        raise ClientError("the coordinator did not accept the input: " + (ack or {}).get("reason", ""))
    return job


async def status(bundle, host, port):
    reply = await _ask(bundle, host, port, {"type": "status"})
    return {"jobs": reply["jobs"], "devices": reply["devices"]}


async def cancel(bundle, host, port, job):
    await _ask(bundle, host, port, {"type": "cancel", "job": job})


async def fetch(bundle, host, port, job, dest):
    """Download a finished job's result files into dest. Returns {name: path}."""
    info = (await _ask(bundle, host, port, {"type": "results", "job": job}))["files"]
    reader, writer = await _connect(bundle, host, port)
    got = {}
    try:
        writer.write(protocol.encode({"type": "transfer_open", "job": job, "what": "result_out"}))
        await writer.drain()
        while True:
            m = await asyncio.wait_for(protocol.read_message(reader), 60)
            if m is None or m["type"] == "blob_end":
                break
            if m["type"] == "blob_ack":
                raise ClientError(m["reason"])
            name, _, sha = await protocol.recv_blob(reader, str(dest), 1 << 31, header=m)
            if info.get(name, {}).get("sha256") != sha:
                raise ClientError(f"{name} does not match what the coordinator announced")
            got[name] = str(Path(dest) / name)
    except (protocol.ProtocolError, ConnectionError, ssl.SSLError, asyncio.TimeoutError) as e:
        raise ClientError(f"fetching results failed: {e}") from None
    finally:
        writer.close()
    return got


def pretty(obj):
    return json.dumps(obj, indent=2)
