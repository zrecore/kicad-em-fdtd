"""Length-prefixed JSON frames and the message schema shared by the coordinator and workers.

A frame is a 4-byte big-endian length followed by that many bytes of UTF-8 JSON (one object with a "type").
Control frames are small; bulk data never travels on the control connection (see PLAN.md, GPU farm).
"""

import asyncio
import contextlib
import hashlib
import json
import os
import re
import struct

MAX_CONTROL_FRAME = 1 << 20  # 1 MiB: job offers and events are small
PROTOCOL_VERSION = 1


class ProtocolError(Exception):
    """A peer sent something that is not a valid frame or message. The connection must be dropped."""


# type -> required fields and their JSON types. Unknown types and extra-typed fields are refused.
_STR, _INT, _NUM, _LIST, _DICT, _BOOL = str, int, (int, float), list, dict, bool
MESSAGES = {
    "hello": {"protocol": _INT, "name": _STR, "devices": _LIST, "solver_version": _STR, "data_hash": _STR},
    "heartbeat": {"devices": _LIST},
    "offer": {
        "job": _STR,
        "lease": _STR,
        "expires": _NUM,
        "spec": _DICT,
        "device": _STR,
        "input_sha256": _STR,
        "input_bytes": _INT,
    },
    "accept": {"job": _STR, "lease": _STR},
    "progress": {"job": _STR, "lease": _STR, "line": _STR},
    "result": {"job": _STR, "lease": _STR, "files": _DICT},
    "fail": {"job": _STR, "lease": _STR, "reason": _STR},
    "cancel": {"job": _STR},
    # checkpoints (a worker that said `features: ["checkpoint"]` in its hello; docs/farm.md)
    "paused": {
        "job": _STR,
        "lease": _STR,
        "step": _INT,
    },  # worker -> coordinator: the solver wrote a checkpoint and left
    "checkpointed": {
        "job": _STR,
        "lease": _STR,
        "step": _INT,
    },  # worker -> coordinator: a checkpoint now exists
    "discard": {"job": _STR},  # coordinator -> worker: the checkpoint of this job is not needed any more
    "drain": {},
    "ping": {},  # coordinator -> worker: proof the coordinator is still there (see the worker's watchdog)
    "stopped": {"job": _STR, "lease": _STR},  # worker -> coordinator: the solver for this job is dead
    # clients (people submitting work) talk to the coordinator on the same listener, by their certificate role
    "submit": {"spec": _DICT, "input_sha256": _STR, "input_bytes": _INT, "label": _STR},
    "submitted": {"job": _STR},
    "status": {},
    "resume": {"job": _STR},  # client: continue a paused job from its checkpoint (optional max_minutes)
    "restart": {"job": _STR},  # client: drop the checkpoint and start the job again from step 0
    "status_reply": {"jobs": _LIST, "devices": _LIST},
    "results": {"job": _STR},
    "result_info": {"job": _STR, "files": _DICT},
    "ok": {},
    "refused": {"reason": _STR},
    # bulk data uses its own connection per transfer, never the control stream (see blobs below)
    "transfer_open": {"job": _STR, "what": _STR},
    "blob_header": {"name": _STR, "bytes": _INT, "sha256": _STR},
    "blob_ack": {"ok": _BOOL, "reason": _STR},
    "blob_end": {},
    # enrolment (a new device has no certificate yet; see enroll.py)
    "enroll_hello": {"name": _STR, "nonce": _STR},
    "enroll_challenge": {"nonce": _STR},
    "enroll_proof": {"proof": _STR},
    "enroll_bundle": {"cert": _STR, "key": _STR, "ca": _STR},  # also carries role and name
    "enroll_denied": {"reason": _STR},
}


def encode(message):
    """Validate and frame one message."""
    validate(message)
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    if len(body) > MAX_CONTROL_FRAME:
        raise ProtocolError(f"message of {len(body)} bytes exceeds the control frame limit")
    return struct.pack(">I", len(body)) + body


def validate(message):
    if not isinstance(message, dict):
        raise ProtocolError("a message must be a JSON object")
    kind = message.get("type")
    spec = MESSAGES.get(kind) if isinstance(kind, str) else None
    if spec is None:
        raise ProtocolError(f"unknown message type {kind!r}")
    for field, want in spec.items():
        value = message.get(field)
        # bool is an int in Python; never accept it where a number or integer is wanted
        wrong = (isinstance(value, bool) and want is not _BOOL) or not isinstance(value, want)
        if field not in message or wrong:
            raise ProtocolError(f"{kind} message: field {field!r} missing or of the wrong type")
    return message


def decode(body):
    try:
        message = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as e:
        raise ProtocolError(f"frame is not valid UTF-8 JSON: {e}") from None
    return validate(message)


async def read_message(reader):
    """Read one frame from an asyncio StreamReader. Returns None at a clean end of stream."""
    try:
        head = await reader.readexactly(4)
    except asyncio.IncompleteReadError as e:
        if not e.partial:
            return None
        raise ProtocolError("stream ended inside a frame header") from None
    (length,) = struct.unpack(">I", head)
    if length == 0 or length > MAX_CONTROL_FRAME:  # refuse before allocating anything
        raise ProtocolError(f"frame length {length} is outside 1..{MAX_CONTROL_FRAME}")
    try:
        body = await reader.readexactly(length)
    except asyncio.IncompleteReadError:
        raise ProtocolError("stream ended inside a frame body") from None
    return decode(body)


# --- bulk data ----------------------------------------------------------------------------------
# A blob is a blob_header frame, then exactly header.bytes raw bytes. The receiver caps the size before
# reading, hashes while writing to disk, and answers with blob_ack. Names are plain file names.


CHUNK = 1 << 20
_SAFE_NAME = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")


def safe_name(name):
    if not isinstance(name, str) or not _SAFE_NAME.match(name) or name.startswith(".."):
        raise ProtocolError(f"file name {name!r} is not allowed")
    return name


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


async def send_blob(writer, name, path):
    """Send one file. Returns when the bytes are written; the caller reads blob_ack."""
    size = os.path.getsize(path)
    writer.write(
        encode({"type": "blob_header", "name": safe_name(name), "bytes": size, "sha256": file_sha256(path)})
    )
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(CHUNK), b""):
            writer.write(chunk)
            await writer.drain()
    await writer.drain()


async def recv_blob(reader, dest_dir, max_bytes, expect_name=None, header=None):
    """Receive one blob into dest_dir. Returns (name, bytes, sha256). Raises ProtocolError if the header is
    wrong, the size is over the cap, the name is not allowed, or the hash does not match."""
    import os

    if header is None:
        header = await read_message(reader)
    if header is None or header["type"] != "blob_header":
        raise ProtocolError("expected a blob header")
    name = safe_name(header["name"])
    if expect_name is not None and name != expect_name:
        raise ProtocolError(f"expected {expect_name!r}, got {name!r}")
    size = header["bytes"]
    if size < 0 or size > max_bytes:
        raise ProtocolError(f"blob of {size} bytes is over the limit of {max_bytes}")
    os.makedirs(dest_dir, exist_ok=True)
    final = os.path.join(dest_dir, name)
    part = final + ".part"
    h = hashlib.sha256()
    got = 0

    try:
        with open(part, "wb") as f:
            while got < size:
                chunk = await asyncio.wait_for(reader.read(min(CHUNK, size - got)), 60)
                if not chunk:
                    raise ProtocolError("stream ended inside a blob")
                f.write(chunk)
                h.update(chunk)
                got += len(chunk)
        if h.hexdigest() != header["sha256"]:
            raise ProtocolError("blob hash does not match its header")
        os.replace(part, final)
    except BaseException:
        with contextlib.suppress(OSError):
            os.unlink(part)
        raise
    return name, size, header["sha256"]
