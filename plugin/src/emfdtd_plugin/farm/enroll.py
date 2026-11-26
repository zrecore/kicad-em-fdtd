"""Pairing a new device with the coordinator, with no certificate on the device yet.

The user creates an invitation on the coordinator (a short one-time code, valid for minutes, a few tries) and
types the code, plus the coordinator's fingerprint read from its screen, on the new device. The device pins
the fingerprint (so nothing but the real coordinator can answer), then proves it knows the code with an HMAC
over both nonces and that fingerprint; the code itself never crosses the network. On success the coordinator
issues the device's certificate once. The endpoint exists only while an invitation is active.
"""

import asyncio
import contextlib
import hashlib
import hmac
import json
import os
import secrets
import ssl
import time

from . import config, pki, policy, protocol

ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"  # no 0/O/1/I
CODE_GROUPS, GROUP_LEN = 3, 4  # 12 characters, 60 bits
READ_TIMEOUT = 10.0
FAIL_DELAY = 1.0  # seconds a wrong code costs, to slow guessing
PROOF_CONTEXT = b"emfdtd-enroll-v1"


class EnrollError(Exception):
    pass


def _file():
    return config.home() / "invite.json"


def _norm(code):
    return "".join(c for c in code.upper() if c.isalnum())


def create_invite(name, role="worker", minutes=10, attempts=5):
    """Make the one active invitation (replacing any earlier one). Returns the code to type on the device."""
    if role not in ("worker", "client"):
        raise EnrollError("an invitation is for a worker or a client")
    if not name or not all(c.isalnum() or c in "-_." for c in name):
        raise EnrollError("a device name may contain only letters, digits, - _ and .")
    code = "".join(secrets.choice(ALPHABET) for _ in range(CODE_GROUPS * GROUP_LEN))
    config.ensure_home()
    _write(
        {
            "name": name,
            "role": role,
            "code": code,
            "expires": time.time() + 60 * minutes,
            "attempts_left": int(attempts),
        }
    )
    return "-".join(code[i : i + GROUP_LEN] for i in range(0, len(code), GROUP_LEN))


def _write(data):
    tmp = _file().with_suffix(".tmp")
    fd = os.open(str(tmp), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(data, f)
    os.replace(str(tmp), str(_file()))


def _read():
    try:
        return json.loads(_file().read_text())
    except (OSError, ValueError):
        return None


def clear_invite():
    with contextlib.suppress(OSError):
        _file().unlink()


def invite_active():
    inv = _read()
    return bool(inv) and inv.get("attempts_left", 0) > 0 and inv.get("expires", 0) > time.time()


def _proof(code, nonce_c, nonce_s, server_fp):
    msg = PROOF_CONTEXT + bytes.fromhex(nonce_c) + bytes.fromhex(nonce_s) + bytes.fromhex(server_fp)
    return hmac.new(_norm(code).encode(), msg, hashlib.sha256).hexdigest()


async def _send(writer, message):
    writer.write(protocol.encode(message))
    await writer.drain()


async def _recv(reader):
    return await asyncio.wait_for(protocol.read_message(reader), READ_TIMEOUT)


def make_handler(server_bundle):
    server_fp = pki.fingerprint(server_bundle["cert"])
    lock = asyncio.Lock()

    async def handle(reader, writer):
        try:
            hello = await _recv(reader)
            if not hello or hello["type"] != "enroll_hello":
                return
            try:
                bytes.fromhex(hello["nonce"])
            except ValueError:
                return
            if not invite_active():
                await _send(writer, {"type": "enroll_denied", "reason": "no invitation is active"})
                return
            nonce_s = secrets.token_hex(16)
            await _send(writer, {"type": "enroll_challenge", "nonce": nonce_s})
            msg = await _recv(reader)
            if not msg or msg["type"] != "enroll_proof":
                return
            async with lock:  # one guess at a time, so parallel connections cannot beat the attempt limit
                inv = _read()
                if not invite_active():
                    await _send(writer, {"type": "enroll_denied", "reason": "no invitation is active"})
                    return
                want = _proof(inv["code"], hello["nonce"], nonce_s, server_fp)
                if not hmac.compare_digest(want, msg["proof"]):
                    inv["attempts_left"] -= 1
                    if inv["attempts_left"] <= 0:
                        clear_invite()
                    else:
                        _write(inv)
                    await asyncio.sleep(FAIL_DELAY)
                    await _send(writer, {"type": "enroll_denied", "reason": "the code was not accepted"})
                    return
                clear_invite()  # single use, even if issuing below fails
                bundle = pki.issue(inv["role"], inv["name"])
                policy.add_device(inv["role"], inv["name"])
                pki.record_member(inv["role"], inv["name"], pki.fingerprint(bundle["cert"]))
                bundle = {**bundle, "role": inv["role"], "name": inv["name"]}
            await _send(writer, {"type": "enroll_bundle", **bundle})
        except (protocol.ProtocolError, asyncio.TimeoutError, ConnectionError, ssl.SSLError):
            pass
        finally:
            writer.close()

    return handle


async def serve(host, port, server_bundle):
    """Start the enrolment endpoint (TLS, no client certificate). The caller closes it when the invitation
    is used up; see run_until_done."""
    ctx = pki.server_context(server_bundle, require_client_cert=False)
    return await asyncio.start_server(make_handler(server_bundle), host, port, ssl=ctx)


async def run_until_done(host, port, server_bundle, poll=0.2):
    """Serve only while an invitation is active, then close the port. Returns the bound port."""
    server = await serve(host, port, server_bundle)
    bound = server.sockets[0].getsockname()[1]
    try:
        while invite_active():
            await asyncio.sleep(poll)
    finally:
        server.close()
        await server.wait_closed()
    return bound


async def enroll(host, port, pinned_fingerprint, code):
    """The device side. Returns the certificate bundle, or raises EnrollError with the reason."""
    pinned = pki.parse_fingerprint(pinned_fingerprint)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = pki.min_tls()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE  # authenticity comes from the pinned fingerprint, checked below
    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port, ssl=ctx), READ_TIMEOUT)
    except (OSError, asyncio.TimeoutError, ssl.SSLError) as e:
        raise EnrollError(f"could not reach the coordinator: {e}") from None
    try:
        der = writer.get_extra_info("ssl_object").getpeercert(binary_form=True)
        seen = pki.fingerprint(der)
        if not hmac.compare_digest(seen, pinned):
            raise EnrollError(
                f"the coordinator's fingerprint is {pki.show_fingerprint(seen)}, not the one you gave: "
                "stop, this is not your coordinator"
            )
        nonce_c = secrets.token_hex(16)
        await _send(writer, {"type": "enroll_hello", "name": "new-device", "nonce": nonce_c})
        challenge = await _recv(reader)
        if not challenge or challenge["type"] != "enroll_challenge":
            raise EnrollError(
                challenge["reason"]
                if challenge and challenge["type"] == "enroll_denied"
                else "unexpected reply from the coordinator"
            )
        try:
            bytes.fromhex(challenge["nonce"])
        except ValueError:
            raise EnrollError("bad challenge from the coordinator") from None
        await _send(
            writer, {"type": "enroll_proof", "proof": _proof(code, nonce_c, challenge["nonce"], seen)}
        )
        reply = await _recv(reader)
        if not reply:
            raise EnrollError("the coordinator closed the connection")
        if reply["type"] == "enroll_denied":
            raise EnrollError(reply["reason"])
        if reply["type"] != "enroll_bundle":
            raise EnrollError("unexpected reply from the coordinator")
        return {k: reply[k] for k in ("cert", "key", "ca", "role", "name") if k in reply}
    except (protocol.ProtocolError, asyncio.TimeoutError, ConnectionError, ssl.SSLError) as e:
        raise EnrollError(f"the enrolment conversation failed: {e}") from None
    finally:
        writer.close()
