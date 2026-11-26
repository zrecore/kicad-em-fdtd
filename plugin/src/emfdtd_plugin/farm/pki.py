"""A private certificate authority for one farm, built with the system `openssl` on the coordinator only.

Python's standard library cannot make certificates, and Windows has no openssl, so a worker never generates
keys: the coordinator issues the worker's identity once, over the enrolment channel (enroll.py). Every
certificate is EC P-256; a device's name and role are its subject common name, "role:name".
"""

import hashlib
import json
import os
import shutil
import ssl
import subprocess
import tempfile
from pathlib import Path

from . import config

CA_DAYS = 3650
DEVICE_DAYS = 90


class PkiError(Exception):
    pass


def _openssl():
    exe = shutil.which("openssl")
    if not exe:
        raise PkiError("the coordinator needs the `openssl` command-line tool to issue certificates")
    return exe


def _run(*args):
    try:
        subprocess.run([_openssl(), *args], check=True, capture_output=True)
    except subprocess.CalledProcessError as e:
        raise PkiError(
            "openssl {} failed: {}".format(args[0], e.stderr.decode(errors="replace").strip())
        ) from None


def _private(path):
    config.restrict(path)


def ca_dir():
    return config.home() / "ca"


def ca_exists():
    return (ca_dir() / "ca.pem").exists() and (ca_dir() / "ca.key").exists()


def init_ca():
    """Create the farm CA once. Refuses to overwrite an existing one (that would orphan every device)."""
    d = ca_dir()
    if ca_exists():
        raise PkiError("a farm CA already exists; reset the farm to start over")
    config.ensure_home()
    d.mkdir(parents=True, exist_ok=True)
    config.restrict(d, directory=True)
    key, cert = d / "ca.key", d / "ca.pem"
    _run("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(key))
    _private(key)
    with tempfile.TemporaryDirectory() as t:
        cnf = Path(t) / "ca.cnf"
        cnf.write_text(
            "[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n"
            "[dn]\nCN=emfdtd-farm-ca\n"
            "[ext]\nbasicConstraints=critical,CA:true,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\n"
            "subjectKeyIdentifier=hash\n"
        )
        _run(
            "req",
            "-x509",
            "-new",
            "-key",
            str(key),
            "-days",
            str(CA_DAYS),
            "-config",
            str(cnf),
            "-out",
            str(cert),
        )
    return cert


def issue(role, name, days=DEVICE_DAYS, hosts=()):
    """Issue a certificate for "role:name" signed by the CA. Returns {"cert", "key", "ca"} as PEM text.
    `hosts` (names or IPs) become subject alternative names (for the coordinator's server certificate)."""
    if not ca_exists():
        raise PkiError("no farm CA: run init first")
    if role not in ("coordinator", "worker", "client"):
        raise PkiError(f"unknown role {role!r}")
    if not name or not all(c.isalnum() or c in "-_." for c in name):
        raise PkiError("a device name may contain only letters, digits, - _ and .")
    cn = f"{role}:{name}"
    d = ca_dir()
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        key, csr, crt, ext = t / "d.key", t / "d.csr", t / "d.pem", t / "ext.cnf"
        _run("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(key))
        _run("req", "-new", "-key", str(key), "-subj", "/CN=" + cn, "-out", str(csr))
        sans = []
        for h in hosts:
            is_ip = h.replace(".", "").isdigit() or ":" in h
            sans.append(("IP:" if is_ip else "DNS:") + h)
        usage = "serverAuth,clientAuth" if role == "coordinator" else "clientAuth"
        lines = [
            "[ext]",
            "basicConstraints=critical,CA:false",
            "keyUsage=critical,digitalSignature",
            "extendedKeyUsage=" + usage,
            "subjectKeyIdentifier=hash",
        ]
        if sans:
            lines.append("subjectAltName=" + ",".join(sans))
        ext.write_text("\n".join(lines) + "\n")
        _run(
            "x509",
            "-req",
            "-in",
            str(csr),
            "-CA",
            str(d / "ca.pem"),
            "-CAkey",
            str(d / "ca.key"),
            "-CAcreateserial",
            "-days",
            str(days),
            "-extfile",
            str(ext),
            "-extensions",
            "ext",
            "-out",
            str(crt),
        )
        return {"cert": crt.read_text(), "key": key.read_text(), "ca": (d / "ca.pem").read_text()}


def fingerprint(pem_or_der):
    """SHA-256 of the DER certificate, as 64 hex digits."""
    der = pem_or_der if isinstance(pem_or_der, bytes) else ssl.PEM_cert_to_DER_cert(pem_or_der)
    return hashlib.sha256(der).hexdigest()


def show_fingerprint(fp):
    """Groups of four for reading aloud or comparing on two screens."""
    return "-".join(fp[i : i + 4] for i in range(0, len(fp), 4))


def parse_fingerprint(text):
    fp = text.replace("-", "").replace(":", "").replace(" ", "").lower()
    if len(fp) != 64 or any(c not in "0123456789abcdef" for c in fp):
        raise PkiError("a fingerprint is 64 hex digits")
    return fp


# --- revocation -----------------------------------------------------------------------------------


def _revoked_file():
    return config.home() / "revoked.json"


def revoked():
    try:
        return json.loads(_revoked_file().read_text())
    except (OSError, ValueError):
        return {}


def revoke(fp, reason="revoked by the user"):
    data = revoked()
    data[parse_fingerprint(fp)] = reason
    config.ensure_home()
    tmp = _revoked_file().with_suffix(".tmp")
    tmp.write_text(json.dumps(data, indent=2))
    os.replace(str(tmp), str(_revoked_file()))


def is_revoked(der):
    return fingerprint(der) in revoked()


# --- TLS contexts ---------------------------------------------------------------------------------


def tls13_available():
    return bool(ssl.HAS_TLSv1_3)


def min_tls():
    """TLS 1.3, always. A Python whose OpenSSL lacks it (the macOS system Python 3.9 is linked to an old
    LibreSSL) cannot run the farm: there is no fallback to an older protocol."""
    if not tls13_available():
        raise PkiError(
            "this Python's TLS library ("
            + ssl.OPENSSL_VERSION
            + ") has no TLS 1.3, which the farm requires; use a Python built with OpenSSL 1.1.1 or newer"
        )
    return ssl.TLSVersion.TLSv1_3


def _write_identity(bundle, t):
    cert, key = Path(t) / "i.pem", Path(t) / "i.key"
    cert.write_text(bundle["cert"])
    key.write_text(bundle["key"])
    return str(cert), str(key)


def server_context(bundle, require_client_cert=True):
    """TLS 1.3 server context. With require_client_cert only devices holding a certificate signed by this CA
    get past the handshake; the enrolment endpoint turns it off (a new device has no certificate yet)."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = min_tls()
    with tempfile.TemporaryDirectory() as t:
        ctx.load_cert_chain(*_write_identity(bundle, t))
    if require_client_cert:
        ctx.verify_mode = ssl.CERT_REQUIRED
        ctx.load_verify_locations(cadata=bundle["ca"])
    return ctx


def client_context(bundle):
    """TLS 1.3 client context that presents the device certificate and trusts only this farm's CA. The server
    name is not matched (devices are reached by LAN address); the CA is private to this farm, so a certificate
    it signed for the coordinator role is what is checked."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = min_tls()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.load_verify_locations(cadata=bundle["ca"])
    with tempfile.TemporaryDirectory() as t:
        ctx.load_cert_chain(*_write_identity(bundle, t))
    return ctx


def peer_identity(ssl_object):
    """("worker", "win4060") from a verified peer certificate; None if unverified or revoked."""
    der = ssl_object.getpeercert(binary_form=True)
    cert = ssl_object.getpeercert()
    if not der or not cert or is_revoked(der):
        return None
    for rdn in cert.get("subject", ()):
        for key, value in rdn:
            if key == "commonName" and ":" in value:
                role, name = value.split(":", 1)
                return role, name
    return None


# --- stored identities and enrolled members ----------------------------------------------------------


def _identity_file(role):
    return config.home() / "identity" / f"{role}.json"


def save_identity(role, bundle):
    config.ensure_home()
    path = _identity_file(role)
    path.parent.mkdir(parents=True, exist_ok=True)
    config.restrict(path.parent, directory=True)
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)  # holds a private key
    with os.fdopen(fd, "w") as f:
        json.dump({"cert": bundle["cert"], "key": bundle["key"], "ca": bundle["ca"]}, f)
    config.restrict(path)  # on Windows the mode above does not restrict the ACL


def load_identity(role):
    try:
        return json.loads(_identity_file(role).read_text())
    except (OSError, ValueError):
        raise PkiError(f"no {role} identity on this machine: enrol it first") from None


def _members_file():
    return config.home() / "members.json"


def members():
    try:
        return json.loads(_members_file().read_text())
    except (OSError, ValueError):
        return {}


def record_member(role, name, fp):
    data = members()
    data[f"{role}:{name}"] = {"fingerprint": fp}
    config.ensure_home()
    tmp = _members_file().with_suffix(".tmp")
    tmp.write_text(json.dumps(data, indent=2))
    os.replace(str(tmp), str(_members_file()))
