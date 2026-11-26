"""What each enrolled device may do. The coordinator is the decision point: every request is checked against
this file at the time it is made, not once per connection. Anything not granted is refused."""

import json
import os

from . import config

ACTIONS = ("submit", "cancel", "status", "results", "run")
DEFAULT_GRANTS = {
    "worker": ("run",),
    "client": ("submit", "cancel", "status", "results"),
}
MIB = 1 << 20
DEFAULT_LIMITS = {
    "max_input_bytes": 64 * MIB,
    "max_result_bytes": 256 * MIB,
    "max_minutes": 120,
    "max_steps": 1_000_000,
    "max_tier": "advanced",  # settings above this tier (expert) cannot be set by a submitter
}


def _file():
    return config.home() / "policy.json"


def load():
    try:
        data = json.loads(_file().read_text())
        return data if isinstance(data.get("devices"), dict) else {"devices": {}}
    except (OSError, ValueError, AttributeError):
        return {"devices": {}}


def _save(data):
    config.ensure_home()
    tmp = _file().with_suffix(".tmp")
    tmp.write_text(json.dumps(data, indent=2))
    os.replace(str(tmp), str(_file()))


def key(role, name):
    return f"{role}:{name}"


def add_device(role, name, extra_grants=(), limits=None):
    """Called when a device is enrolled: the default grants of its role, plus anything the user adds."""
    grants = sorted(set(DEFAULT_GRANTS.get(role, ())) | set(extra_grants))
    bad = [g for g in grants if g not in ACTIONS]
    if bad:
        raise ValueError("unknown action {}".format(", ".join(bad)))
    data = load()
    data["devices"][key(role, name)] = {"may": grants, "limits": dict(limits or {})}
    _save(data)


def remove_device(role, name):
    data = load()
    data["devices"].pop(key(role, name), None)
    _save(data)


def authorize(identity, action):
    """(allowed, reason). `identity` is (role, name) from a verified certificate, or None."""
    if identity is None:
        return False, "no verified identity"
    if action not in ACTIONS:
        return False, f"unknown action {action!r}"
    entry = load()["devices"].get(key(*identity))
    if entry is None:
        return False, f"{key(*identity)} is not enrolled"
    if action not in entry.get("may", ()):
        return False, f"{key(*identity)} may not {action}"
    return True, "ok"


def limits_for(identity):
    """The device's limits: the defaults with its own overrides, never loosened beyond what is written."""
    entry = load()["devices"].get(key(*identity), {})
    out = dict(DEFAULT_LIMITS)
    out.update(entry.get("limits", {}))
    return out
