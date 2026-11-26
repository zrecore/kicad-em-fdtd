"""The agent window's widgets (tkinter/ttk). No decisions are made here: controller.py owns them.

Rules this file keeps (docs/ux.md style, applied to a tkinter window): every control has a visible text label;
everything can be done from the keyboard; nothing is said by colour alone; sizes come from the system font's
metrics, never from pixel constants, so a larger system text size scales the window; text wraps to the
window's width and the content scrolls, so it is usable at 720x520. The window opens no socket and does
nothing in the background unless it is open.
"""

from __future__ import annotations

import contextlib
import queue
import sys
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
from tkinter import filedialog, messagebox, ttk

from . import controller as ctl


class ScrollFrame(ttk.Frame):
    """A frame whose content scrolls vertically and always fits the width (so text can wrap)."""

    def __init__(self, parent):
        super().__init__(parent)
        self.canvas = tk.Canvas(self, highlightthickness=0, borderwidth=0)
        bar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=bar.set)
        self.inner = ttk.Frame(self.canvas)
        self._window = self.canvas.create_window((0, 0), window=self.inner, anchor="nw")
        self.canvas.grid(row=0, column=0, sticky="nsew")
        bar.grid(row=0, column=1, sticky="ns")
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        self.inner.bind("<Configure>", lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all")))
        self.canvas.bind("<Configure>", lambda e: self.canvas.itemconfigure(self._window, width=e.width))
        for seq in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
            self.canvas.bind_all(seq, self._wheel, add="+")

    def _wheel(self, event):
        if not self.winfo_ismapped():
            return
        delta = -1 if (getattr(event, "num", 0) == 4 or getattr(event, "delta", 0) > 0) else 1
        self.canvas.yview_scroll(delta, "units")


class Wrapped(ttk.Label):
    """A label that wraps to its parent's width."""

    def __init__(self, parent, text="", indent=0, **kw):
        super().__init__(parent, text=text, justify="left", **kw)
        self.indent = indent  # horizontal room the grid placement takes besides the parent's own padding
        parent.bind("<Configure>", self._fit, add="+")

    def _fit(self, event):
        self.configure(wraplength=max(120, event.width - 2 * 12 - self.indent))


class App:
    def __init__(
        self, root, controller=None, ask=None, browse=None, sync=False, clock=time.monotonic, system=None
    ):
        self.root = root
        self.c = controller or ctl.Controller()
        self.ask = ask or (lambda title, message: messagebox.askyesno(title, message, parent=root))
        self.browse = browse or (lambda: filedialog.askopenfilename(parent=root))
        self.sync = sync
        self.system = system
        self.status_ref = ctl.Refresher(clock=clock)
        self.log_ref = ctl.Refresher(clock=clock)
        self.wizard = ctl.Wizard(self.c)
        self.w = {}
        self.status = ctl.Status()
        self._after = []
        self._queue = queue.Queue()
        self._closed = False
        self._status_busy = False
        self._action_busy = False
        self._doctor_loaded = False
        self.font = tkfont.nametofont("TkDefaultFont")
        self.line = max(12, self.font.metrics("linespace"))
        self.pad = max(4, self.line // 3)
        root.title("Farm agent")
        root.geometry(f"{max(720, self.font.measure('0') * 62)}x{max(520, self.line * 32)}")
        root.minsize(self.font.measure("0") * 40, self.line * 22)
        root.columnconfigure(0, weight=1)
        root.rowconfigure(1, weight=1)
        self._styles()
        self._vars()
        self._build_status()
        self._build_tabs()
        self._key_bindings()
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.status_ref.start()
        self._schedule()
        self._load_settings()

    # ---- plumbing --------------------------------------------------------------------------------

    def _styles(self):
        st = ttk.Style(self.root)
        st.configure("Big.TButton", padding=(self.line, self.line // 2))
        self.big = self.font.copy()
        self.big.configure(size=int(self.font.cget("size") * 1.4) or self.font.cget("size"), weight="bold")
        self.bold = self.font.copy()
        self.bold.configure(weight="bold")

    def _vars(self):
        v = tk.StringVar
        self.v = {
            k: v(self.root)
            for k in (
                "solver", "device", "turn", "lock_command", "max_minutes", "max_steps", "max_input_mb",
                "max_tier", "watchdog", "work",
                "inv", "fingerprint", "code", "coordinator",
            )
        }  # fmt: skip
        self.v["turn"].set("lock")
        self.confirm_role = tk.BooleanVar(self.root, False)
        self.holding_turn = tk.BooleanVar(self.root, False)
        self.autostart_var = tk.BooleanVar(self.root, False)

    def _bg(self, fn, done):
        """Run fn off the interface thread and hand its result to done on it (inline when sync)."""

        def work():
            try:
                res = fn()
            except Exception as e:  # a failing runner must never kill the window
                res = ctl.Result(1, "", f"unexpected error: {e}")
            if self.sync:
                done(res)
            else:
                self._queue.put((done, res))

        if self.sync:
            work()
        else:
            threading.Thread(target=work, daemon=True).start()

    def _drain(self):
        if self._closed:
            return
        try:
            while True:
                done, res = self._queue.get_nowait()
                done(res)
        except queue.Empty:
            pass
        self._after.append(self.root.after(100, self._drain))

    def _schedule(self):
        self._drain()
        self._tick()

    def _tick(self):
        if self._closed:
            return
        if self.status_ref.due():
            self.refresh_status()
        if self.log_ref.due():
            self.refresh_log()
        self._after.append(self.root.after(500, self._tick))

    def close(self):
        self._closed = True
        self.status_ref.stop()
        self.log_ref.stop()
        for a in self._after:
            with contextlib.suppress(tk.TclError):
                self.root.after_cancel(a)
        self.root.destroy()

    def _key_bindings(self):
        self.root.bind_class("TButton", "<Return>", lambda e: e.widget.invoke())
        self.root.bind_class("TCheckbutton", "<Return>", lambda e: e.widget.invoke())
        self.root.bind("<F5>", lambda e: self.refresh_status())
        self.root.bind("<Control-w>", lambda e: self.close())

    def _label_row(self, parent, text, row, widget, underline=None):
        """A visible label to the left of its control, in reading order (the label names the control)."""
        lab = ttk.Label(parent, text=text, underline=-1 if underline is None else underline)
        lab.grid(row=row, column=0, sticky="w", padx=self.pad, pady=self.pad // 2)
        widget.grid(row=row, column=1, sticky="ew", padx=self.pad, pady=self.pad // 2)
        return lab

    # ---- status panel ----------------------------------------------------------------------------

    def _build_status(self):
        f = ttk.LabelFrame(self.root, text="Agent status")
        f.grid(row=0, column=0, sticky="ew", padx=self.pad * 2, pady=(self.pad * 2, self.pad))
        f.columnconfigure(0, weight=1)
        self.w["state"] = ttk.Label(f, text="Checking...", font=self.big)
        self.w["state"].grid(row=0, column=0, sticky="w", padx=self.pad, pady=self.pad)
        rows = [("uptime", 1), ("connection", 2), ("job", 3), ("status_device", 5), ("problem", 6)]
        for name, r in rows:
            self.w[name] = Wrapped(f, "")
            self.w[name].grid(row=r, column=0, columnspan=2, sticky="ew", padx=self.pad)
        pb = ttk.Frame(f)
        self.w["progress_frame"] = pb
        pb.grid(row=4, column=0, columnspan=2, sticky="ew", padx=self.pad)
        pb.columnconfigure(0, weight=1)
        self.w["progress"] = ttk.Progressbar(pb, maximum=100, mode="determinate")
        self.w["progress"].grid(row=0, column=0, sticky="ew")
        self.w["progress_text"] = ttk.Label(pb, text="")
        self.w["progress_text"].grid(row=0, column=1, padx=self.pad)
        bt = ttk.Frame(f)
        bt.grid(row=0, column=1, sticky="ne", padx=self.pad, pady=self.pad)
        self.w["start"] = ttk.Button(
            bt, text="Start", style="Big.TButton", underline=0, command=self.on_start
        )
        self.w["stop"] = ttk.Button(bt, text="Stop", style="Big.TButton", underline=1, command=self.on_stop)
        self.w["restart"] = ttk.Button(
            bt, text="Restart", style="Big.TButton", underline=0, command=self.on_restart
        )
        for b in ("start", "stop", "restart"):
            self.w[b].pack(side="left", padx=self.pad // 2)  # side by side: the status panel stays short
        self.w["busy"] = ttk.Label(f, text="")
        self.w["busy"].grid(row=0, column=0, sticky="e", padx=self.pad * 2)
        self.w["action_msg"] = Wrapped(f, "")
        self.w["action_msg"].grid(row=7, column=0, columnspan=2, sticky="ew", padx=self.pad)

    def refresh_status(self):
        if self._status_busy or self._closed:
            return
        self._status_busy = True

        def done(res):
            self._status_busy = False
            if self._closed:
                return
            self.status = res
            self.apply_status(res)

        self._bg(self.c.status, done)

    def apply_status(self, st):
        d = ctl.describe(st)
        self.w["state"].configure(text=f"Agent: {d['state']}")
        for k, key in (("uptime", "uptime"), ("connection", "connection"), ("job", "job"),
                       ("status_device", "device"), ("problem", "problem")):  # fmt: skip
            label = self.w[k]
            label.configure(text=d[key])
            if d[key]:
                label.grid()
            else:
                label.grid_remove()  # an empty row would only take room from the tabs
        pct = d["percent"]
        has_job = st.running and st.job is not None
        if has_job:
            self.w["progress_frame"].grid()
        else:
            self.w["progress_frame"].grid_remove()
        if pct is None:
            self.w["progress"].configure(value=0)
            self.w["progress_text"].configure(text="no progress reported" if has_job else "")
        else:
            self.w["progress"].configure(value=pct)
            self.w["progress_text"].configure(text=f"about {pct}%")
        self._buttons()

    def _buttons(self):
        busy = self._action_busy
        running = self.status.running
        known = not self.status.error
        self.w["start"].state(["!disabled"] if (not busy and not running and known) else ["disabled"])
        self.w["stop"].state(["!disabled"] if (not busy and running) else ["disabled"])
        self.w["restart"].state(["!disabled"] if (not busy and running) else ["disabled"])
        self.w["busy"].configure(text="Working..." if busy else "")

    def _act(self, label, fn):
        if self._action_busy:
            return
        self._action_busy = True
        self._buttons()

        def done(res):
            self._action_busy = False
            self.w["action_msg"].configure(
                text=f"{label}: {res.text()}" if res.ok else f"{label} did not work: {res.text()}"
            )
            self._status_busy = False
            self.refresh_status()

        self._bg(fn, done)

    def on_start(self):
        self._act("Start", self.c.start)

    def on_stop(self):
        msg = self.c.stop_needs_confirmation(self.status)
        if msg and not self.ask("Stop the agent?", msg):
            return
        self._act("Stop", self.c.stop)

    def on_restart(self):
        msg = self.c.stop_needs_confirmation(self.status)
        if msg and not self.ask(
            "Restart the agent?", msg.replace("Stop the agent anyway?", "Restart it anyway?")
        ):
            return
        self._act("Restart", self.c.restart)

    # ---- tabs ------------------------------------------------------------------------------------

    def _build_tabs(self):
        nb = ttk.Notebook(self.root)
        nb.grid(row=1, column=0, sticky="nsew", padx=self.pad * 2, pady=(self.pad, self.pad * 2))
        self.w["notebook"] = nb
        self.tabs = {}
        for key, title in (
            ("checks", "Checks"),
            ("settings", "Settings"),
            ("setup", "Set up this machine"),
            ("log", "Log"),
        ):
            frame = ScrollFrame(nb) if key != "log" else ttk.Frame(nb)
            nb.add(frame, text=title)
            self.tabs[key] = frame
        self._build_checks(self.tabs["checks"].inner)
        self._build_settings(self.tabs["settings"].inner)
        self._build_setup(self.tabs["setup"].inner)
        self._build_log(self.tabs["log"])
        nb.bind("<<NotebookTabChanged>>", self._tab_changed)

    def select_tab(self, key):
        self.w["notebook"].select(self.tabs[key])
        self.root.update_idletasks()
        self._tab_changed(None)

    def current_tab(self):
        sel = self.w["notebook"].select()
        for k, f in self.tabs.items():
            if str(f) == str(sel):
                return k
        return ""

    def _tab_changed(self, _event):
        tab = self.current_tab()
        if tab == "log":
            self.log_ref.start()
        else:
            self.log_ref.stop()
        if tab == "checks" and not self._doctor_loaded:
            self.run_doctor()

    # ---- checks ----------------------------------------------------------------------------------

    def _build_checks(self, p):
        p.columnconfigure(0, weight=1)
        top = ttk.Frame(p)
        top.grid(row=0, column=0, sticky="ew", padx=self.pad, pady=self.pad)
        self.w["check_again"] = ttk.Button(top, text="Check again", underline=0, command=self.run_doctor)
        self.w["check_again"].pack(side="left")
        self.w["checks_summary"] = ttk.Label(top, text="Not checked yet")
        self.w["checks_summary"].pack(side="left", padx=self.pad * 2)
        self.checks_box = ttk.Frame(p)
        self.checks_box.grid(row=1, column=0, sticky="ew")
        self.checks_box.columnconfigure(0, weight=1)

    def run_doctor(self):
        self._doctor_loaded = True
        self.w["checks_summary"].configure(text="Checking...")

        def fn():
            rep = self.c.doctor()
            return rep

        def done(rep):
            if isinstance(rep, ctl.Result):  # the runner itself failed
                rep = ctl.DoctorReport(False, [], rep.text())
            self.show_doctor(rep)

        self._bg(fn, done)

    def show_doctor(self, rep):
        for child in self.checks_box.winfo_children():
            child.destroy()
        if rep.error:
            self.w["checks_summary"].configure(text=f"The checks could not run: {rep.error}")
            return
        bad = [c for c in rep.checks if not c.ok]
        self.w["checks_summary"].configure(
            text="Everything is in order." if rep.ok else f"{len(bad)} thing(s) need attention."
        )
        for i, c in enumerate(rep.checks):
            row = ttk.Frame(self.checks_box)
            row.grid(row=i, column=0, sticky="ew", padx=self.pad, pady=self.pad // 2)
            row.columnconfigure(0, weight=1)
            head = ttk.Label(row, text=f"{ctl.check_word(c)}: {c.title}", font=self.bold)
            head.grid(row=0, column=0, sticky="w")
            if c.detail:
                Wrapped(row, c.detail).grid(row=1, column=0, sticky="ew")
            if not c.ok and c.fix:
                Wrapped(row, f"What to do: {c.fix}").grid(row=2, column=0, sticky="ew")
        self.root.update_idletasks()

    # ---- settings --------------------------------------------------------------------------------

    def _build_settings(self, p):
        p.columnconfigure(0, weight=1)
        r = 0
        prog = ttk.LabelFrame(p, text="Program and GPU")
        prog.grid(row=r, column=0, sticky="ew", padx=self.pad, pady=self.pad)
        prog.columnconfigure(1, weight=1)
        r += 1
        sol = ttk.Frame(prog)
        sol.columnconfigure(0, weight=1)
        self.w["solver"] = ttk.Entry(sol, textvariable=self.v["solver"])
        self.w["solver"].grid(row=0, column=0, sticky="ew")
        self.w["browse"] = ttk.Button(sol, text="Browse...", underline=0, command=self.on_browse)
        self.w["browse"].grid(row=0, column=1, padx=(self.pad, 0))
        self._label_row(prog, "The emfdtd program (solver):", 0, sol)
        self.w["device_choice"] = ttk.Combobox(prog, textvariable=self.v["device"], values=[])
        self._label_row(prog, "GPU to offer (name or UUID start):", 1, self.w["device_choice"])
        self.w["devices_note"] = Wrapped(prog, "")
        self.w["devices_note"].grid(row=2, column=0, columnspan=2, sticky="ew", padx=self.pad)

        turn = ttk.LabelFrame(p, text="How this worker takes its GPU's turn")
        turn.grid(row=r, column=0, sticky="ew", padx=self.pad, pady=self.pad)
        turn.columnconfigure(0, weight=1)
        r += 1
        for i, (ident, title, why) in enumerate(ctl.TURN_MODES):
            rb = ttk.Radiobutton(turn, text=title, value=ident, variable=self.v["turn"])
            rb.grid(row=2 * i, column=0, sticky="w", padx=self.pad)
            self.w["turn_" + ident] = rb
            Wrapped(turn, why, indent=self.pad * 8).grid(
                row=2 * i + 1, column=0, sticky="ew", padx=self.pad * 4
            )
        lock = ttk.Frame(turn)
        lock.columnconfigure(1, weight=1)
        ttk.Label(lock, text="Lock command (for the first choice):").grid(row=0, column=0, sticky="w")
        self.w["lock_command"] = ttk.Entry(lock, textvariable=self.v["lock_command"])
        self.w["lock_command"].grid(row=0, column=1, sticky="ew", padx=self.pad)
        lock.grid(row=6, column=0, sticky="ew", padx=self.pad, pady=self.pad)

        lim = ttk.LabelFrame(p, text="Limits")
        lim.grid(row=r, column=0, sticky="ew", padx=self.pad, pady=self.pad)
        lim.columnconfigure(1, weight=1)
        r += 1
        fields = (
            ("max_minutes", "Longest job (minutes):"),
            ("max_steps", "Most steps in a job:"),
            ("max_input_mb", "Largest input (MB):"),
            ("watchdog", "Silence before the solver is stopped (seconds):"),
            ("work", "Folder for job files (optional):"),
        )
        for i, (key, label) in enumerate(fields):
            self.w[key] = ttk.Entry(lim, textvariable=self.v[key])
            self._label_row(lim, label, i, self.w[key])
        self.w["max_tier"] = ttk.Combobox(
            lim, textvariable=self.v["max_tier"], values=list(ctl.TIERS), state="readonly"
        )
        self._label_row(lim, "Settings level jobs may use:", len(fields), self.w["max_tier"])

        bar = ttk.Frame(p)
        bar.grid(row=r, column=0, sticky="ew", padx=self.pad, pady=self.pad)
        r += 1
        self.w["save"] = ttk.Button(bar, text="Save settings", underline=0, command=self.on_save)
        self.w["save"].pack(side="left")
        self.w["reload"] = ttk.Button(bar, text="Reload", underline=0, command=self._load_settings)
        self.w["reload"].pack(side="left", padx=self.pad)
        self.w["save_msg"] = Wrapped(p, "")
        self.w["save_msg"].grid(row=r, column=0, sticky="ew", padx=self.pad)
        r += 1

        auto = ttk.LabelFrame(p, text="Start when I log in")
        auto.grid(row=r, column=0, sticky="ew", padx=self.pad, pady=self.pad)
        auto.columnconfigure(0, weight=1)
        self.w["autostart"] = ttk.Checkbutton(
            auto, text="Start the agent when I log in", variable=self.autostart_var, command=self.on_autostart
        )
        self.w["autostart"].grid(row=0, column=0, sticky="w", padx=self.pad)
        self.w["autostart_msg"] = Wrapped(auto, "")
        self.w["autostart_msg"].grid(row=1, column=0, sticky="ew", padx=self.pad)
        self._refresh_autostart()

    def settings(self):
        g = {k: v.get() for k, v in self.v.items()}
        return ctl.Settings(
            solver=g["solver"], device=g["device"], turn=g["turn"], lock_command=g["lock_command"],
            max_minutes=g["max_minutes"], max_steps=g["max_steps"], max_input_mb=g["max_input_mb"],
            max_tier=g["max_tier"], watchdog=g["watchdog"], work=g["work"],
        )  # fmt: skip

    def _load_settings(self):
        def done(res):
            s, err = res
            for k in ("solver", "device", "turn", "lock_command", "max_minutes", "max_steps", "max_input_mb",
                      "max_tier", "watchdog", "work"):  # fmt: skip
                self.v[k].set(getattr(s, k))
            self.w["device_choice"].configure(values=s.recorded_devices)
            self.w["devices_note"].configure(
                text=("GPUs recorded on this machine: " + ", ".join(s.recorded_devices)) if s.recorded_devices
                else "No GPUs recorded yet: use 'Set up this machine' (step 4) to record them."
            )  # fmt: skip
            if err:
                self.w["save_msg"].configure(text=f"The saved settings could not be read: {err}")

        self._bg(self.c.load_settings, done)

    def on_browse(self):
        path = self.browse()
        if path:
            self.v["solver"].set(path)

    def on_save(self):
        s = self.settings()

        def done(res):
            self.w["save_msg"].configure(text=("Saved. " if res.ok else "Not saved: ") + res.text())

        self._bg(lambda: self.c.save_settings(s), done)

    def _refresh_autostart(self):
        def done(a):
            self.autostart_var.set(a.enabled)
            self.w["autostart_msg"].configure(
                text=(a.error or a.detail or ("It is on." if a.enabled else "It is off."))
                + " Switching it on installs "
                + ctl.autostart_description(self.system)
            )

        self._bg(self.c.autostart, done)

    def on_autostart(self):
        want = self.autostart_var.get()
        if want:
            msg = "Start the farm agent when you log in? This installs " + ctl.autostart_description(
                self.system
            )
            if not self.ask("Start when I log in", msg):
                self.autostart_var.set(False)
                return

        def done(res):
            self.w["autostart_msg"].configure(text=res.text())
            self._refresh_autostart()

        self._bg(lambda: self.c.set_autostart(want), done)

    # ---- the set-up wizard

    def _build_setup(self, p):
        p.columnconfigure(0, weight=1)
        Wrapped(
            p,
            "Do these in order. Each step opens when the one before it worked. Nothing here is saved until "
            "its step succeeds, and the one-time code is never written anywhere.",
        ).grid(row=0, column=0, sticky="ew", padx=self.pad, pady=self.pad)
        self.step_frames = {}
        for n, step in enumerate(ctl.STEPS, start=1):
            f = ttk.LabelFrame(p, text=f"Step {n}: {ctl.STEP_TITLES[step]}")
            f.grid(row=n, column=0, sticky="ew", padx=self.pad, pady=self.pad)
            f.columnconfigure(1, weight=1)
            Wrapped(f, ctl.STEP_HELP[step]).grid(row=0, column=0, columnspan=2, sticky="ew", padx=self.pad)
            row = 1
            if step == "role":
                cb = ttk.Checkbutton(f, text="Yes, turn the worker role on", variable=self.confirm_role)
                cb.grid(row=row, column=0, columnspan=2, sticky="w", padx=self.pad)
                self.w["confirm_role"] = cb
                row += 1
            if step == "enroll":
                enroll_fields = (
                    ("inv", "Invitation address (host:port):"),
                    ("fingerprint", "Coordinator fingerprint:"),
                    ("code", "One-time code:"),
                    ("coordinator", "Coordinator address (host:port):"),
                )
                for key, label in enroll_fields:
                    e = ttk.Entry(f, textvariable=self.v[key])
                    self._label_row(f, label, row, e)
                    self.w["enroll_" + key] = e
                    row += 1
            if step == "devices":
                Wrapped(f, "Uses the program, GPU and turn choice from the Settings tab.").grid(
                    row=row, column=0, columnspan=2, sticky="ew", padx=self.pad
                )
                row += 1  # fmt: skip
                hold_text = "I hold the GPU's turn right now (needed for the coordinator-held choice)"
                cb = ttk.Checkbutton(f, text=hold_text, variable=self.holding_turn)
                cb.grid(row=row, column=0, columnspan=2, sticky="w", padx=self.pad)
                self.w["holding_turn"] = cb
                row += 1
            run = ttk.Button(f, text="Run this step", command=lambda s=step: self.on_step(s))
            run.grid(row=row, column=0, sticky="w", padx=self.pad, pady=self.pad)
            self.w["run_" + step] = run
            res = Wrapped(f, "")
            res.grid(row=row, column=1, sticky="ew", padx=self.pad)
            self.w["result_" + step] = res
            self.step_frames[step] = f
        self._refresh_wizard()

    def on_step(self, step):
        s = self.settings()
        inputs = {
            "python": {},
            "role": {"confirmed": self.confirm_role.get()},
            "enroll": {"invitation": self.v["inv"].get(), "fingerprint": self.v["fingerprint"].get(),
                       "code": self.v["code"].get(), "coordinator": self.v["coordinator"].get()},
            "devices": {"solver": s.solver, "device": s.device, "turn": s.turn,
                        "lock_command": s.lock_command, "holding_turn": self.holding_turn.get()},
            "configure": {"settings": s},
            "doctor": {},
            "start": {},
        }[step]  # fmt: skip
        self.w["result_" + step].configure(text="Working...")

        def done(res):
            self._refresh_wizard()
            if step == "enroll" and res.ok:
                self.v["code"].set("")  # the code works once: do not keep it on screen
            if step == "start":
                self.refresh_status()

        self._bg(lambda: self.wizard.run(step, **inputs), lambda r: done(r))

    def _refresh_wizard(self):
        for step in ctl.STEPS:
            res = self.wizard.results.get(step)
            if res is not None:
                text = ("Done: " if res.ok else "Problem: ") + res.message
            elif self.wizard.can_run(step):
                text = "Ready."
            else:
                text = "Waiting for the earlier steps."
            self.w["result_" + step].configure(text=text)
            self.w["run_" + step].state(["!disabled"] if self.wizard.can_run(step) else ["disabled"])

    # ---- log -------------------------------------------------------------------------------------

    def _build_log(self, p):
        p.columnconfigure(0, weight=1)
        p.rowconfigure(1, weight=1)
        top = ttk.Frame(p)
        top.grid(row=0, column=0, columnspan=2, sticky="ew", padx=self.pad, pady=self.pad)
        self.w["copy"] = ttk.Button(top, text="Copy", underline=0, command=self.on_copy)
        self.w["copy"].pack(side="left")
        self.w["refresh_log"] = ttk.Button(top, text="Refresh now", underline=0, command=self.refresh_log)
        self.w["refresh_log"].pack(side="left", padx=self.pad)
        ttk.Label(top, text="Updates every 2 seconds while this tab is showing.").pack(
            side="left", padx=self.pad
        )
        text = tk.Text(p, wrap="word", height=10, state="disabled", font="TkFixedFont")
        bar = ttk.Scrollbar(p, orient="vertical", command=text.yview)
        text.configure(yscrollcommand=bar.set)
        text.grid(row=1, column=0, sticky="nsew")
        bar.grid(row=1, column=1, sticky="ns")
        self.w["log_text"] = text

    def refresh_log(self):
        def done(text):
            t = self.w["log_text"]
            t.configure(state="normal")
            t.delete("1.0", "end")
            t.insert("end", text.text() if isinstance(text, ctl.Result) else text)
            t.see("end")
            t.configure(state="disabled")

        self._bg(lambda: self.c.log(200), done)

    def log_contents(self):
        return self.w["log_text"].get("1.0", "end").rstrip("\n")

    def on_copy(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.log_contents())


def main(argv=None):
    try:
        root = tk.Tk()
    except tk.TclError as e:
        message = f"emfdtd-farm gui: cannot open a window here ({e}). Use: emfdtd-farm agent status"
        print(message, file=sys.stderr)
        return 1
    App(root)
    root.mainloop()
    return 0
