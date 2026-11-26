"""Run the solver as a child process the farm can stop for good: its own process group on POSIX (the whole
tree is signalled), `taskkill /T` on Windows."""

import asyncio
import contextlib
import os
import signal
import subprocess
import sys

ENV_KEEP_EXACT = {
    "PATH", "HOME", "USERPROFILE", "SYSTEMROOT", "SYSTEMDRIVE", "WINDIR", "TEMP", "TMP", "TMPDIR",
    "LANG", "LC_ALL", "APPDATA", "LOCALAPPDATA", "PROGRAMDATA", "COMSPEC", "PATHEXT", "VULKAN_SDK",
    "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "LD_LIBRARY_PATH",
}  # fmt: skip
ENV_KEEP_PREFIX = ("VK_", "XDG_", "EMFDTD_DEVICE_", "MESA_", "__GLX", "NVIDIA_")


def scrubbed_env(environ=None):
    """The environment the solver gets: what it needs to find the Vulkan loader and a temp folder, nothing
    else (no tokens or credentials the worker's user happens to have in the shell)."""
    environ = os.environ if environ is None else environ
    return {k: v for k, v in environ.items() if k in ENV_KEEP_EXACT or k.startswith(ENV_KEEP_PREFIX)}


class Running:
    def __init__(self, proc):
        self.proc = proc
        self.killed = False

    def kill_tree(self):
        self.killed = True
        if self.proc.returncode is not None:
            return
        if sys.platform == "win32":
            subprocess.run(
                ["taskkill", "/PID", str(self.proc.pid), "/T", "/F"], capture_output=True, check=False
            )
        else:
            with contextlib.suppress(ProcessLookupError, PermissionError):
                os.killpg(self.proc.pid, signal.SIGKILL)


def _kill_late(fut):
    """The start was cancelled before its caller got the handle: kill the child as soon as it exists."""
    if not fut.cancelled() and fut.exception() is None:
        fut.result().kill_tree()


async def start(argv, cwd, env=None):
    """Start argv (a list, never a shell string) with stdout and stderr merged. If the caller is cancelled
    while the child is being created, the child is killed rather than left running unowned."""
    fut = asyncio.ensure_future(_spawn(argv, cwd, env))
    try:
        return await asyncio.shield(fut)
    except asyncio.CancelledError:
        fut.add_done_callback(_kill_late)
        raise


async def _spawn(argv, cwd, env):
    kwargs = {}
    if sys.platform == "win32":
        kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        kwargs["start_new_session"] = True
    proc = await asyncio.create_subprocess_exec(
        *argv,
        cwd=cwd,
        env=scrubbed_env() if env is None else env,
        stdin=asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
        **kwargs,
    )
    return Running(proc)


async def pump(running, log_path, on_line, max_bytes, timeout):
    """Copy the child's output to log_path (capped), call on_line(text) for each line, and wait for it to end.
    Kills the tree on timeout. Returns (returncode, timed_out, last_lines)."""
    last = []
    written = 0

    async def read():
        nonlocal written
        with open(log_path, "ab") as log:
            async for raw in running.proc.stdout:
                line = raw.decode("utf-8", "replace").rstrip()
                last.append(line)
                del last[:-20]
                if written < max_bytes:
                    log.write(raw[: max_bytes - written])
                    written += len(raw)
                on_line(line)

    reader = asyncio.ensure_future(read())
    timed_out = False
    try:
        await asyncio.wait_for(running.proc.wait(), timeout)
    except asyncio.TimeoutError:
        timed_out = True
        running.kill_tree()
        await running.proc.wait()
    except asyncio.CancelledError:
        running.kill_tree()
        raise
    finally:
        with contextlib.suppress(Exception):
            await asyncio.wait_for(reader, 5)
    return running.proc.returncode, timed_out, last
