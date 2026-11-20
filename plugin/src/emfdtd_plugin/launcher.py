"""Shared by the plugin's actions: export the open board, then open the solver window or reuse the one that is
already open for this project. Everything that does not need KiCAD is injectable, so it tests without it."""

import json
import os
import subprocess
from pathlib import Path


def solver_running(pid_file):
    """True if pid_file names a live process (the solver writes it while its window is open)."""
    try:
        pid = int(Path(pid_file).read_text().strip())
        os.kill(pid, 0)
    except (OSError, ValueError):
        return False
    return True


def previous_ports(project_dir):
    """Ports of the last export for this project (so reopening the window does not lose the port)."""
    try:
        return json.loads((Path(project_dir) / "em-fdtd" / "snapshot.json").read_text()).get("ports", [])
    except (OSError, ValueError):
        return []


def find_solver(plugin_dir, env=None):
    """EMFDTD_BIN, else solver_path.txt (from tools/install.sh), else bin/emfdtd, else the repo build."""
    env = os.environ if env is None else env
    plugin_dir = Path(plugin_dir)
    candidates = [env.get("EMFDTD_BIN")]
    path_file = plugin_dir / "solver_path.txt"
    if path_file.is_file():
        candidates.append(path_file.read_text().strip())
    candidates += [
        plugin_dir / "bin" / "emfdtd",
        plugin_dir.parent / "build" / "release" / "apps" / "emfdtd" / "emfdtd",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(candidate)
    raise RuntimeError("cannot find the emfdtd solver binary; set EMFDTD_BIN to its path")


def publish(snapshot, project_dir, plugin_dir, spawn=subprocess.Popen, request="preview"):
    """Write the snapshot to <project>/em-fdtd/snapshot.json. An open solver window for the project notices
    the new file, reloads and comes to the front; otherwise start one. Returns "reused" or "started"."""
    work = Path(project_dir) / "em-fdtd"
    work.mkdir(exist_ok=True)
    snapshot_file = work / "snapshot.json"
    # request: "preview" shows the new board on the settings screen, "run" also starts the solve. It is
    # written before the snapshot, which is what an open window watches for.
    (work / "request.txt").write_text(request + "\n")
    snapshot_file.write_text(json.dumps(snapshot))
    if solver_running(work / "solver.pid"):
        return "reused"
    # Band, mesh and every other value come from the profile and the settings in em-fdtd/settings.json.
    command = [find_solver(plugin_dir), str(snapshot_file), "--project-dir", str(project_dir), "--ui"]
    if request == "run":
        command.append("--autorun")
    (work / "request.txt").unlink(missing_ok=True)  # a new window gets the request on its command line
    spawn(command, cwd=work)
    return "started"
