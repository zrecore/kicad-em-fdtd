"""The submitting side: connect with the client certificate, send a job, ask for status, fetch results."""

import asyncio
import contextlib
import hashlib
import json
import ssl
import sys
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
    return {"jobs": reply["jobs"], "devices": reply["devices"], "locks": reply.get("locks", [])}


async def lock(bundle, host, port, name, label, max_minutes, cmd, say=print):
    """Wait for the farm's lock `name` (cpu:MACHINE or gpu:MACHINE[:GPU]), run `cmd` inside the turn (None:
    just keep the turn until stdin closes, after printing HOLDING) and give the turn back. The turn is the
    open connection: if this process dies the coordinator takes it back at once. Returns the exit code
    (75 if the turn ended before the command did)."""
    reader, writer = await _connect(bundle, host, port)
    try:
        writer.write(
            protocol.encode({"type": "lock_hold", "lock": name, "label": label, "max_minutes": max_minutes})
        )
        await writer.drain()
        while True:
            msg = await protocol.read_message(reader)
            if msg is None:
                raise ClientError("the coordinator closed the connection")
            if msg["type"] == "refused":
                raise ClientError(msg["reason"])
            if msg["type"] == "lock_queued" and msg["ahead"]:
                say(f"waiting for {name}: {msg['ahead']} turn(s) ahead")
            if msg["type"] == "lock_granted":
                break
        lost = asyncio.ensure_future(protocol.read_message(reader))  # lock_lost, or the hang-up
        if cmd:
            proc = await asyncio.create_subprocess_exec(*cmd)
            work = asyncio.ensure_future(proc.wait())
        else:
            say("HOLDING")
            loop = asyncio.get_running_loop()
            work = asyncio.ensure_future(loop.run_in_executor(None, sys.stdin.buffer.read))
        await asyncio.wait({work, lost}, return_when=asyncio.FIRST_COMPLETED)
        if work.done():
            lost.cancel()
            with contextlib.suppress(Exception):
                writer.write(protocol.encode({"type": "lock_release"}))
                await writer.drain()
            return proc.returncode if cmd else 0
        if cmd:
            with contextlib.suppress(ProcessLookupError):
                proc.terminate()
            await proc.wait()
        say(f"the turn of {name} ended before the command did (its time ran out)")
        return 75
    except (protocol.ProtocolError, ConnectionError, ssl.SSLError) as e:
        raise ClientError(f"the coordinator conversation failed: {e}") from None
    finally:
        writer.close()


async def cancel(bundle, host, port, job):
    await _ask(bundle, host, port, {"type": "cancel", "job": job})


async def resume(bundle, host, port, job, max_minutes=None):
    """Continue a paused job from its checkpoint (on the worker that holds it)."""
    msg = {"type": "resume", "job": job}
    if max_minutes is not None:
        msg["max_minutes"] = max_minutes
    await _ask(bundle, host, port, msg)


async def restart(bundle, host, port, job):
    """Drop a paused job's checkpoint and start the job again from step 0 on any worker."""
    await _ask(bundle, host, port, {"type": "restart", "job": job})


def _place(dest, name):
    """Where a received file lives: a field file arrives flattened as fields.NAME and goes into fields/, so
    the folder opens with `emfdtd --open-run`."""
    path = dest / name
    if name.startswith("fields."):
        (dest / "fields").mkdir(exist_ok=True)
        target = dest / "fields" / name[len("fields.") :]
        path.replace(target)
        return target
    return path


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
            got[name] = str(_place(Path(dest), name))
    except (protocol.ProtocolError, ConnectionError, ssl.SSLError, asyncio.TimeoutError) as e:
        raise ClientError(f"fetching results failed: {e}") from None
    finally:
        writer.close()
    return got


def pretty(obj):
    return json.dumps(obj, indent=2)
