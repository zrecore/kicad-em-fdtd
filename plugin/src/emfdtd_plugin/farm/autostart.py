"""Starting the farm agent when its user logs in. OFF unless the user turns it on, one command each way.

The generators return plain text or an argv and touch nothing, so they are tested for their exact content.
The installers write one file and run external commands through ONE helper, `_run`, which tests replace: no
test (and no code path except `agent autostart on|off`) ever installs or loads anything.
"""

import os
import subprocess
import sys
from pathlib import Path

LABEL = "org.kicad-em-fdtd.farm-agent"  # launchd
UNIT = "emfdtd-farm-agent.service"  # systemd --user
TASK = "EmfdtdFarmAgent"  # Windows Task Scheduler

MECHANISM = {"darwin": "launchd", "win32": "schtasks"}  # everything else: systemd


def mechanism(platform=None):
    return MECHANISM.get(platform or sys.platform, "systemd")


def start_argv(python, tool):
    """What every mechanism runs: the same `agent start` a person would type."""
    return [str(python), str(tool), "agent", "start"]


def stop_argv(python, tool):
    return [str(python), str(tool), "agent", "stop"]


def _xml(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def launchd_plist(python, tool, log):
    """A launchd user agent: runs `agent start` at login, once. KeepAlive is false so that stopping the agent
    is a stop: launchd does not bring it back."""
    args = "\n".join(f"    <string>{_xml(a)}</string>" for a in start_argv(python, tool))
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>{LABEL}</string>
  <key>ProgramArguments</key>
  <array>
{args}
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <false/>
  <key>StandardOutPath</key>
  <string>{_xml(str(log))}</string>
  <key>StandardErrorPath</key>
  <string>{_xml(str(log))}</string>
</dict>
</plist>
"""


def systemd_unit(python, tool):
    """A systemd user unit: `agent start` at login (it detaches), `agent stop` at logout or disable."""
    start = " ".join(_quote(a) for a in start_argv(python, tool))
    stop = " ".join(_quote(a) for a in stop_argv(python, tool))
    return f"""[Unit]
Description=kicad-em-fdtd farm agent (a worker for the GPU job farm)
After=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart={start}
ExecStop={stop}

[Install]
WantedBy=default.target
"""


def _quote(arg):
    return '"' + arg.replace('"', '\\"') + '"' if (" " in arg or not arg) else arg


def schtasks_create_argv(python, tool):
    """A per-user task at logon, limited rights (no elevation)."""
    run = " ".join('"' + a + '"' for a in start_argv(python, tool))
    return ["schtasks", "/Create", "/SC", "ONLOGON", "/TN", TASK, "/TR", run, "/RL", "LIMITED", "/F"]


def plist_path(home=None):
    return Path(home or Path.home()) / "Library" / "LaunchAgents" / f"{LABEL}.plist"


def unit_path(home=None):
    return Path(home or Path.home()) / ".config" / "systemd" / "user" / UNIT


def _run(argv):
    """The one place an external command is run. Returns (exit code, output)."""
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=60, check=False)
    except (OSError, subprocess.SubprocessError) as e:
        return 127, str(e)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def _uid():
    return os.getuid() if hasattr(os, "getuid") else 0


def on(python, tool, log, platform=None, home=None):
    """Install the autostart entry for this user. Returns a one-line description."""
    kind = mechanism(platform)
    if kind == "launchd":
        path = plist_path(home)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(launchd_plist(python, tool, log))
        code, out = _run(["launchctl", "bootstrap", f"gui/{_uid()}", str(path)])
        if code != 0 and "already" not in out.lower():
            raise RuntimeError(f"launchctl bootstrap failed: {out.strip()}")
        return f"launchd agent {LABEL} installed ({path})"
    if kind == "systemd":
        path = unit_path(home)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(systemd_unit(python, tool))
        for argv in (["systemctl", "--user", "daemon-reload"], ["systemctl", "--user", "enable", UNIT]):
            code, out = _run(argv)
            if code != 0:
                raise RuntimeError(f"{' '.join(argv)} failed: {out.strip()}")
        return f"systemd user unit {UNIT} enabled ({path})"
    code, out = _run(schtasks_create_argv(python, tool))
    if code != 0:
        raise RuntimeError(f"schtasks /Create failed: {out.strip()}")
    return f"scheduled task {TASK} created (at logon, current user)"


def off(platform=None, home=None):
    kind = mechanism(platform)
    if kind == "launchd":
        _run(["launchctl", "bootout", f"gui/{_uid()}/{LABEL}"])
        path = plist_path(home)
        if path.exists():
            path.unlink()
        return f"launchd agent {LABEL} removed"
    if kind == "systemd":
        _run(["systemctl", "--user", "disable", UNIT])
        path = unit_path(home)
        if path.exists():
            path.unlink()
        _run(["systemctl", "--user", "daemon-reload"])
        return f"systemd user unit {UNIT} removed"
    _run(["schtasks", "/Delete", "/TN", TASK, "/F"])
    return f"scheduled task {TASK} removed"


def status(platform=None, home=None):
    """{"enabled": bool, "mechanism": str, "detail": str}"""
    kind = mechanism(platform)
    if kind == "launchd":
        path = plist_path(home)
        enabled = path.exists()
        return {"enabled": enabled, "mechanism": kind, "detail": str(path) if enabled else "not installed"}
    if kind == "systemd":
        path = unit_path(home)
        code, _ = _run(["systemctl", "--user", "is-enabled", UNIT]) if path.exists() else (1, "")
        return {
            "enabled": path.exists() and code == 0,
            "mechanism": kind,
            "detail": str(path) if path.exists() else "not installed",
        }
    code, out = _run(["schtasks", "/Query", "/TN", TASK])
    return {
        "enabled": code == 0,
        "mechanism": kind,
        "detail": f"scheduled task {TASK}" if code == 0 else "not installed",
    }
