#!/usr/bin/env python3
# ruff: noqa
"""Builds the agent window against a fake agent and walks every tab. Standalone (standard library + the farm
package on the path): run it with a Python that has tkinter and a display. tests/test_farm_gui_view.py runs it
when EMFDTD_GUI_TESTS=1. EMFDTD_GUI_SHOTS=<dir> also saves a screenshot of each tab (macOS screencapture)."""

import os
import subprocess
import sys
import time
from pathlib import Path

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here.parent / "src"))
sys.path.insert(0, str(here))

import tkinter as tk

from emfdtd_plugin.farm.gui import controller as ctl
from emfdtd_plugin.farm.gui import view
from farm_gui_fake import DOCTOR_BAD, FakeAgent

checks = 0


def expect(cond, message):
    global checks
    checks += 1
    if not cond:
        raise SystemExit(f"FAILED: {message}")


def text(w):
    return w.cget("text")


def shot(app, name):
    out = os.environ.get("EMFDTD_GUI_SHOTS")
    if not out:
        return
    os.makedirs(out, exist_ok=True)
    app.root.update_idletasks()
    app.root.update()
    app.root.attributes("-topmost", True)  # above the terminal this runs from, or the shot shows it instead
    app.root.lift()
    app.root.focus_force()
    app.root.update()
    time.sleep(0.8)
    r = app.root
    region = f"{r.winfo_rootx()},{r.winfo_rooty()},{r.winfo_width()},{r.winfo_height()}"
    subprocess.run(["screencapture", "-x", f"-R{region}", str(Path(out) / f"{name}.png")], check=False)
    app.root.attributes("-topmost", False)


fake = FakeAgent()
answers = {"yes": True}
asked = []
root = tk.Tk()
app = view.App(
    root,
    ctl.Controller(fake, recorded=lambda: ["NVIDIA GeForce RTX 4060 Ti"]),
    ask=lambda title, msg: (asked.append((title, msg)), answers["yes"])[1],
    browse=lambda: "/picked/emfdtd",
    sync=True,
    system="Linux",
)
root.update()
w = app.w

# 1. status panel: stopped, then running with a job
app.refresh_status()
expect(text(w["state"]) == "Agent: Stopped", "stopped state in words")
expect(
    "disabled" in w["stop"].state() and "disabled" not in w["start"].state(),
    "only Start is enabled when stopped",
)
shot(app, "1-status-stopped")
w["start"].invoke()
expect(fake.running, "Start ran `agent start`")
expect(text(w["state"]) == "Agent: Running", "running state in words")
expect(text(w["uptime"]) == "Running for 1 h 2 min", "uptime shown")
expect("Connected to the coordinator" in text(w["connection"]), "connection shown in words")
expect(text(w["status_device"]).startswith("GPU: "), "GPU shown")
fake.job = {"id": "abc12345", "label": "fd_dk42", "phase": "solving", "percent": 37}
app.refresh_status()
expect("fd_dk42" in text(w["job"]) and "37%" in text(w["job"]), "job with phase and percent")
expect(float(w["progress"].cget("value")) == 37.0 and text(w["progress_text"]) == "about 37%", "progress bar")
shot(app, "2-status-running-job")

# 2. Stop asks first because a job runs, and Escape-equivalent (answer no) keeps it running
answers["yes"] = False
w["stop"].invoke()
expect(
    asked and "cancels that job" in asked[-1][1] and fake.running,
    "stop with a job asks and 'no' keeps it running",
)
answers["yes"] = True
w["stop"].invoke()
expect(not fake.running and text(w["state"]) == "Agent: Stopped", "confirmed stop stops it")
fake.job = None
w["start"].invoke()
asked.clear()
w["stop"].invoke()
expect(not asked, "stop with no job does not ask")

# 3. checks tab
app.select_tab("checks")
root.update()
expect("Everything is in order" in text(w["checks_summary"]), "checks summary ok")
fake.doctor_checks = DOCTOR_BAD
w["check_again"].invoke()
expect("1 thing(s) need attention" in text(w["checks_summary"]), "checks summary with a problem")
labels = [c.cget("text") for c in app.checks_box.winfo_children()[-1].winfo_children()]
expect(
    any(l.startswith("Problem: The coordinator is reachable") for l in labels), "problem named with a word"
)
expect(any("What to do: Start the coordinator" in l for l in labels), "the fix is shown")
shot(app, "3-checks")

# 4. settings tab
app.select_tab("settings")
root.update()
expect(app.v["solver"].get() == "/opt/emfdtd/emfdtd", "saved settings loaded")
expect(app.v["turn"].get() == "lock", "turn mode loaded")
expect("NVIDIA GeForce RTX 4060 Ti" in text(w["devices_note"]), "recorded GPUs listed")
w["browse"].invoke()
expect(app.v["solver"].get() == "/picked/emfdtd", "Browse fills the solver path")
w["turn_none"].invoke()
expect(app.v["turn"].get() == "none", "turn radio")
app.v["max_minutes"].set("-5")
w["save"].invoke()
expect(
    "Not saved" in text(w["save_msg"]) and "above zero" in text(w["save_msg"]),
    "bad number refused with words",
)
app.v["max_minutes"].set("45")
w["save"].invoke()
expect(
    text(w["save_msg"]).startswith("Saved.") and fake.settings["max_minutes"] == 45,
    "save goes through the CLI",
)
expect(fake.settings["turn"]["mode"] == "none", "turn saved in the agent's own shape")
expect(fake.saved_documents[-1]["devices"] == ["RTX 4060 Ti"], "GPU sent as the agent's devices list")
shot(app, "4-settings")
# autostart: off by default, asks before switching on
expect(not app.autostart_var.get(), "autostart is off by default")
answers["yes"] = False
w["autostart"].invoke()
expect(
    not app.autostart_var.get()
    and not fake.autostart
    and "launchd" not in asked[-1][1]
    and "systemd" in asked[-1][1],
    "autostart asks, says what it installs, and 'no' leaves it off",
)
answers["yes"] = True
w["autostart"].invoke()
expect(fake.autostart and app.autostart_var.get(), "autostart on after confirming")

# 5. wizard
app.select_tab("setup")
root.update()
expect(
    "disabled" in w["run_role"].state() and "disabled" not in w["run_python"].state(), "only step 1 is open"
)
w["run_python"].invoke()
expect(
    text(w["result_python"]).startswith("Done:") or text(w["result_python"]).startswith("Problem:"),
    "step 1 result",
)
app.wizard.results["python"] = ctl.StepResult(True, "forced for the test")  # this Python may lack TLS 1.3
app._refresh_wizard()
w["run_role"].invoke()
expect(text(w["result_role"]).startswith("Problem: Confirm"), "role step needs the confirmation")
w["confirm_role"].invoke()
w["run_role"].invoke()
expect(text(w["result_role"]).startswith("Done:"), "role step done after confirming")
app.v["inv"].set("192.168.1.20:7421")
app.v["fingerprint"].set("1111-2222")
app.v["code"].set("ABCD-EFGH-JKLM")
app.v["coordinator"].set("192.168.1.20:7420")
w["run_enroll"].invoke()
expect(text(w["result_enroll"]).startswith("Done:") and fake.enrolled, "enrol ran")
expect(app.v["code"].get() == "", "the one-time code is cleared from the screen after use")
expect(
    all("ABCD-EFGH-JKLM" not in " ".join(c) for c in fake.calls),
    "the code is redacted in the recorded commands",
)
expect(
    all("ABCD-EFGH-JKLM" not in " ".join(h[0]) for h in app.c.history), "the code is not kept in the history"
)
shot(app, "5-setup")
w["holding_turn"].invoke()
app.v["turn"].set("coordinator")
w["run_devices"].invoke()
expect(text(w["result_devices"]).startswith("Done:"), "devices step done")
w["run_configure"].invoke()
expect(text(w["result_configure"]).startswith("Done:"), "configure step done")
fake.doctor_checks = [{"id": "x", "title": "All good", "ok": True, "detail": "", "fix": ""}]
w["run_doctor"].invoke()
expect(text(w["result_doctor"]).startswith("Done:"), "doctor step done")
fake.running = False
w["run_start"].invoke()
expect(fake.running and text(w["result_start"]).startswith("Done:"), "start step started the agent")

# 6. log tab: refreshes while visible, not otherwise
app.select_tab("log")
root.update()
expect(app.log_ref.active, "log refresh is on while the log tab shows")
app.refresh_log()
expect("worker connected" in app.log_contents(), "log text shown")
fake.log_text += "2026-11-26 10:01:00 job started\n"
app.refresh_log()
expect("job started" in app.log_contents(), "log updates")
root.clipboard_clear()
w["copy"].invoke()
expect("job started" in root.clipboard_get(), "Copy puts the log on the clipboard")
shot(app, "6-log")
app.select_tab("checks")
expect(not app.log_ref.active, "log refresh stops when the tab is left")

# 7. nothing runs after the window closes
n = len(fake.calls)
app.close()
time.sleep(0.8)
expect(len(fake.calls) == n, "no commands after the window is closed")
print(f"gui smoke: ok ({checks} checks)")
