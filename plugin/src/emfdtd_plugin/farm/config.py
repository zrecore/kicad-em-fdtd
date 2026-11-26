"""Machine-wide farm state. Never stored next to a board: a copied project must not turn networking on."""

import json
import os
import subprocess
import sys
from pathlib import Path

ROLES = ("coordinator", "worker", "client")


class FarmDisabled(Exception):
    """A role was asked to run while the user has not enabled it."""


def home():
    """The folder holding the farm identity, policy and logs (created only when a role is enabled)."""
    override = os.environ.get("EMFDTD_FARM_HOME")
    if override:
        return Path(override)
    if sys.platform == "win32":
        return Path(os.environ.get("APPDATA", str(Path.home()))) / "emfdtd" / "farm"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "emfdtd" / "farm"
    return Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config"))) / "emfdtd" / "farm"


_restricted = set()


def restrict(path, directory=False):
    """Make `path` readable by the current user only. POSIX: mode 0700 or 0600. Windows: the mode bits do not
    restrict ACLs, so inheritance is removed and only the current user is granted access (icacls)."""
    path = str(path)
    if sys.platform == "win32":
        user = os.environ.get("USERNAME")
        if not user:
            raise OSError("cannot tell which Windows user to grant access to (USERNAME is not set)")
        domain = os.environ.get("USERDOMAIN")
        who = f"{domain}\\{user}" if domain else user
        grant = f"{who}:(OI)(CI)F" if directory else f"{who}:F"
        subprocess.run(["icacls", path, "/inheritance:r", "/grant:r", grant], check=True, capture_output=True)
    else:
        os.chmod(path, 0o700 if directory else 0o600)


def ensure_home():
    """Create the farm folder (private to this user) and return it."""
    h = home()
    h.mkdir(parents=True, exist_ok=True)
    if str(h) not in _restricted:
        restrict(h, directory=True)
        _restricted.add(str(h))
    return h


def _file():
    return home() / "farm.json"


def enabled(role):
    """True only if the user enabled `role`. A missing or unreadable file means off."""
    _check_role(role)
    try:
        data = json.loads(_file().read_text())
        return data.get("enabled", {}).get(role) is True
    except (OSError, ValueError, AttributeError):
        return False


def require_enabled(role):
    if not enabled(role):
        raise FarmDisabled(
            f"the farm {role} is off. Turn it on in the Farm screen or with: emfdtd-farm enable {role}"
        )


def set_enabled(role, value):
    """Write the switch. Called only by the user's enable/disable command (or the window's switch)."""
    _check_role(role)
    ensure_home()
    path = _file()
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError):
        data = {}
    flags = data.get("enabled", {})
    flags[role] = bool(value)
    data["enabled"] = {r: flags.get(r, False) is True for r in ROLES}
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, indent=2))
    os.replace(str(tmp), str(path))


def remember_python():
    """Record which Python the farm was enabled with, so the window runs the same tool the same way."""
    ensure_home()
    path = _file()
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError):
        data = {}
    data["python"] = sys.executable
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, indent=2))
    os.replace(str(tmp), str(path))


def _check_role(role):
    if role not in ROLES:
        raise ValueError(f"unknown farm role {role!r} (expected one of {', '.join(ROLES)})")
