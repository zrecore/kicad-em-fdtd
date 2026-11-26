# GPU farm: run jobs on other machines' GPUs

The farm lets one person (or a small group on a trusted LAN) send simulation jobs to GPUs on other
machines: another Mac, a Windows PC with an eGPU, a Linux box with an NVIDIA card. It is **off by default**:
nothing runs, listens or connects until you turn a role on. Design, threat model and measurements:
`docs/PLAN.md`, "GPU farm".

## Roles

| Role | What it does | Opens a port? |
|---|---|---|
| **coordinator** | keeps the queue, the private certificate authority and the audit log; hands jobs to workers | yes, one LAN address you choose |
| **worker** | runs the solver on the GPUs you choose, for jobs the coordinator sends | no: it connects out |
| **client** | a person submitting jobs and fetching results | no: it connects out |

One machine can have several roles (the usual set-up: this machine is coordinator and client, the other
machines are workers).

## Requirements

- Python 3.9 or newer **built with OpenSSL 1.1.1 or newer (TLS 1.3)**. The farm refuses to run without it;
  there is no fallback to older TLS and no exception. KiCad's own bundled Python (3.9, OpenSSL 1.1.1n) qualifies
  and is the interpreter the plugin runs on; the macOS system (Xcode) Python 3.9 does not (LibreSSL). Check with
  `python3 -c "import ssl; print(ssl.HAS_TLSv1_3)"`: it must print `True`.
- The coordinator needs the `openssl` command-line tool (macOS and Linux have it; it only issues
  certificates, workers do not need it).
- Workers need the `emfdtd` solver built for their platform (macOS arm64 and Windows x64 work today) and a
  Vulkan 1.2 GPU.
- No other packages: the farm uses only the Python standard library.

Run the tool as `python3 plugin/tools/emfdtd_farm.py ...` (in a packaged build:
`share/emfdtd/plugin/tools/emfdtd_farm.py`). Below it is written `emfdtd-farm`.

## Quick start

1. **Coordinator machine** (pick the LAN address other machines will reach it on, for example `192.168.1.20`):

   ```
   emfdtd-farm enable coordinator          # asks you to type a confirmation
   emfdtd-farm init --host 192.168.1.20    # creates the certificate authority; prints the coordinator fingerprint
   ```

2. **Add a worker.** On the coordinator, open an invitation (it lists a one-time code and the fingerprint, and
   listens for the new device only until the invitation is used or expires after 10 minutes):

   ```
   emfdtd-farm invite win-egpu --role worker --listen 192.168.1.20 --port 7421
   ```

   On the worker machine, type the code and fingerprint you read off the coordinator's screen:

   ```
   emfdtd-farm enable worker
   emfdtd-farm enroll 192.168.1.20:7421 --fingerprint AAAA-BBBB-... --code XXXX-XXXX-XXXX --coordinator 192.168.1.20:7420
   ```

3. **Add yourself (and anyone else who submits jobs)** the same way, with `--role client` and
   `emfdtd-farm enable client`. Each person gets their own identity; jobs and results are private to the
   person who submitted them.

4. **Run the coordinator** (`7420` is the default port; the address must be one of your machine's own):

   ```
   emfdtd-farm serve --listen 192.168.1.20 --port 7420
   ```

5. **Start the worker.** A worker must be told how it takes its GPU turn; it refuses to start otherwise. Every
   call that creates a Vulkan device (listing devices, the solve) happens inside that turn, and the
   heartbeat makes no GPU call. Choose exactly one:

   - `--lock-command CMD...`: a command put in front of every GPU call that waits for your turn and then runs
     the rest (for example a script like the project's `with_gpu_lock.sh`). Use this when the GPU is shared with
     other tools on the same machine.
   - `--turn-from-coordinator`: the coordinator takes the turn for each job (see *GPU turns held by the
     coordinator* below) and says so in the offer; the worker refuses an offer that does not carry it. Use this
     when the lock lives on another machine.
   - `--no-lock`: nobody else uses this GPU.

   **In every mode the worker makes no Vulkan call to connect.** It offers the devices you recorded once with
   `devices --save` (under a free lock, a turn you hold, or with `--no-lock`), so a GPU that someone else is
   using for the next hour (a long run holding the shared lock) never stops the worker from joining the farm; the
   live check that the GPU is still there happens before each job, inside the turn. Without a recorded list the
   worker refuses to start and says which command to run.

   ```
   emfdtd-farm devices --solver C:\emfdtd\emfdtd.exe --save --device "RTX 4060 Ti" --lock-command /path/to/with_gpu_lock.sh
   emfdtd-farm worker --solver C:\emfdtd\emfdtd.exe --device "RTX 4060 Ti" --lock-command /path/to/with_gpu_lock.sh
   ```

   Without `--save`, `devices` only lists the GPUs (one Vulkan call, so it also takes the turn through
   `--lock-command` or needs `--no-lock`).

   A worker also has its own **ceilings** (`--max-minutes`, `--max-steps`, `--max-input-mb`, `--max-tier`,
   defaults 120 minutes, 1,000,000 steps, 64 MB, `advanced`): the coordinator's limits can only lower them,
   never raise them.

   `--device` takes a UUID prefix or part of the GPU's name and can be repeated. Only devices with
   Vulkan 1.2 or newer are offered, and an Apple GPU that is listed once per Vulkan driver is offered once
   (the newest driver).

6. **Submit.**

   ```
   emfdtd-farm submit board.json --profile bluetooth --set mesh_preset=medium --wait results/
   emfdtd-farm submit board.json --profile wifi --sweep mesh_preset=coarse,medium,fine --wait results/
   emfdtd-farm queue          # your jobs (state, progress estimate, phase) and the farm's devices
   emfdtd-farm results JOB --dest results/   # fetch a finished job later
   emfdtd-farm cancel JOB
   ```

   A sweep becomes one job per value; all points of one sweep run on the same kind of device so the
   results are comparable.

Turn a role off with `emfdtd-farm disable ROLE`; `emfdtd-farm status` shows what is on.
`emfdtd-farm members` lists enrolled devices on the coordinator and `emfdtd-farm revoke worker NAME` removes one
for good (its certificate stops working at once, even for an open connection).

## Setting up a worker on another machine

A worker needs only the farm's Python package (standard library, no installs), a launcher that finds a Python with TLS 1.3,
the `emfdtd` program for that machine and a Vulkan 1.2 GPU. `scripts/package_farm_agent.sh` builds a self-contained bundle
(`out/dist/emfdtd-farm-agent-<version>-<date>.tar.gz` and `.zip`, with `SHA256SUMS`) and checks that it runs on its own;
`farm-setup.md` in it is a step-by-step guide for a person with no assistant: prerequisites, enrol, record devices, the three
GPU-turn modes, start and stop, update, uninstall, a table of the tool's messages, and the coordinator owner's side.

## The Farm tab

The emfdtd window has a **Farm** tab for what the command line does: turn roles on and off (turning one on asks
you to confirm and says what it allows), see the farm's devices, watch your jobs (state, a progress bar, the phase,
Cancel), create an invitation for a new device (the one-time code and this machine's fingerprint are shown) and list
or revoke members. It runs the same `emfdtd_farm.py` while the tab is showing and nothing otherwise, and it needs the
same Python with TLS 1.3 (set `EMFDTD_PYTHON` to choose one; `enable` records the Python it ran under). The progress
bar is the solver's estimate ("about N %": 60 % for the pulse, 40 % for the ring-down).

## The agent window

On a worker machine that has only the farm bundle (no emfdtd window), `emfdtd-farm gui` opens a small window to manage
the worker agent with the mouse and keyboard. It is plain Python `tkinter` (no installs, no listening port) and it does
nothing in the background unless it is open: while open it asks the agent for its status every 2 seconds, and for the
log only while the Log tab is showing. Everything it does goes through the command line (`emfdtd-farm agent ...`, `setup`,
`enroll`, `devices --save`), so the window and the terminal always agree.

- **Status panel (always visible):** Running or Stopped in words, how long, whether it is connected to the coordinator, the
  current job with its phase and a progress bar, the GPU in use, the last problem; Start, Stop and Restart. Stopping while a
  job runs asks first and says the job is cancelled and requeued.
- **Checks:** every check of `agent doctor` with OK or Problem in words, the detail and what to do; Check again.
- **Settings:** the solver (Browse...), the GPU (from the ones recorded on this machine), how the worker takes its GPU's turn
  (three choices with a sentence each), the limits and the watchdog; Save goes through `agent configure`, which validates.
  **Start when I log in** is off by default; switching it on asks first and names what it installs (a launchd agent, a systemd
  user unit or a Windows scheduled task).
- **Set up this machine:** the same steps as `emfdtd-farm setup`, one at a time, each opening only after the one before
  succeeded: check Python, turn the worker role on, join the farm (invitation address, fingerprint, one-time code,
  coordinator address; the code is cleared from the screen after use and is never written to any file or history), record the
  GPUs (one GPU call: do it while you hold the GPU's turn if the GPU is shared), save the settings, run the checks, start.
- **Log:** the tail of the agent's log with Copy.

Needs a Python with `tkinter` (the python.org and Windows installers and the macOS system Python have it; Homebrew's
needs `python-tk`) and a display; on a machine without it the command says so and points to `agent status`. The window
follows the system font (larger system text makes it larger), wraps its text, scrolls its content, works from the keyboard
(Tab, Space or Return on a focused button, the underlined letters) and never uses colour alone. Tests: the controller is
tested headless in `plugin/tests/test_farm_gui_controller.py`; the window itself is built and walked against a fake agent by
`tests/gui_smoke.py`, run by `EMFDTD_GUI_TESTS=1 pytest tests/test_farm_gui_view.py` (opt-in: it needs tkinter and a display,
and with the variable set any failure to build the window fails the test; `EMFDTD_GUI_PYTHON` picks the Python,
`EMFDTD_GUI_SHOTS=<folder>` saves a screenshot of each tab on macOS).

## Using the coordinator's own GPU

The coordinator machine does not offer its own GPU until you ask. One command (or the button in the Farm tab) does what you would do by hand on another machine, without the network step because the certificate authority is on the same computer:

```
emfdtd-farm local-worker status [--json]
emfdtd-farm local-worker enable [--solver PATH] [--lock-command CMD] [--max-minutes N] [--yes] [--no-start]
emfdtd-farm local-worker disable
```

`enable` turns the worker role on, enrolls this computer as a worker of its own farm (named after the host), asks the solver which GPUs it has (one Vulkan call, through the lock command if you gave one; the call can wait in that queue), keeps one entry per physical GPU (an Apple GPU is listed once per Vulkan driver: the newest is kept), saves the agent's settings (an existing `agent.json` is kept: solver, lock and limits) and starts the agent. The default longest job is 45 minutes. Run it again and nothing is duplicated. `disable` stops the agent and turns the worker role off; the identity stays, so enabling again is quick. It needs the coordinator role on and the coordinator's address in `coordinator.json`.

## Running the worker as a managed agent

The worker can be configured once and then started and stopped like any program, with no assistant: `emfdtd-farm agent ...`
(`python3 plugin/tools/emfdtd_farm.py agent ...` from a source tree). Everything below also has a `--json` form with a fixed
shape, which the agent window uses.

| Command | What it does |
|---|---|
| `agent configure --solver PATH --device NAME [--device ...] (--no-lock \| --lock-command "CMD" \| --turn-from-coordinator) [--max-minutes N --max-steps N --max-input-mb N --max-tier T --watchdog S --work DIR --log FILE]` | Validate and save the worker's settings in `agent.json` in the farm folder (private to you; no keys or tokens in it). Exactly one turn mode; the solver must exist. `--lock-command` takes one quoted string. |
| `agent configure --show [--json]` / `--json-in [--json]` | Print the saved settings / replace them from a JSON object on stdin (same checks). |
| `agent doctor [--json]` | Run every check (below), each with a one-line fix. Exit 0 only if all pass. |
| `agent start` | Run the doctor, then start `worker --from-agent` detached from the terminal, logging to `agent.log` (rotated at 10 MB at each start). Refuses to start a second copy and says why it cannot start. |
| `agent stop [--timeout S]` | Polite stop (SIGTERM; `taskkill /T` on Windows), wait, then kill the whole process tree; a running solver is ended too. |
| `agent restart` | Stop, then start. |
| `agent status [--json]` | Running or not, pid, uptime, connected to the coordinator, the current job and its phase. |
| `agent logs [-n N] [--follow]` | The last N lines of the log (default 50), optionally following. |
| `agent autostart on\|off\|status [--json]` | Start the agent when you log in: a launchd user agent (macOS), a systemd user unit (Linux) or a Task Scheduler task at logon (Windows). **Off unless you turn it on**, `on` needs a saved configuration, `off` removes only what `on` installed. |
| `setup [--yes] [flags]` | Walk a new machine through: check Python, turn the worker role on, enrol (invitation address, fingerprint, one-time code, coordinator address), record the GPUs (one GPU call, explained first), save the settings, run the doctor, offer to start. With `--yes` and the flags it never asks and says which flag is missing. |

The agent knows it is ours by a small `agent.status.json` the worker rewrites every 10 seconds (pid, connected, job, phase,
solver pids), so a pid that was recycled by an unrelated program is never mistaken for it and never killed.

**Doctor checks** (stable ids, in this order): `tls13` (this Python has TLS 1.3), `role` (the worker role is on),
`identity` (enrolled; the certificate is not expired when `openssl` can tell), `coordinator` (an address is stored and the TCP
port answers), `certificate` (the TLS handshake completes and the coordinator takes our certificate: "rejected" otherwise),
`solver` (`--version` runs; not a GPU call), `settings` (`--list-settings` lists settings), `devices` (the GPUs to offer were
recorded with `devices --save` and the configured names match them), `workdir` (writable, at least 2 GiB free), `config`
(saved and valid).

**JSON shapes.**
`agent status --json`: `{"running": bool, "pid": int|null, "started": epoch|null, "uptime_s": float|null,
"connected": bool, "coordinator": "host:port"|null, "job": {"id", "label", "phase", "percent"}|null, "device": str|null,
"last_error": str|null, "log": path}`.
`agent doctor --json`: `{"ok": bool, "checks": [{"id", "title", "ok", "detail", "fix"}]}`.
`agent configure --show --json`: `{"solver", "devices": [..], "turn": {"mode": "lock"|"coordinator"|"none", "lock_command": [..]},
"max_minutes", "max_steps", "max_input_mb", "max_tier", "watchdog", "work", "log"}` (unset limits are null).
`agent stop --json`: `{"was_running": bool, "stopped": bool, "forced": bool}`.
`agent autostart status --json`: `{"enabled": bool, "mechanism": "launchd"|"systemd"|"schtasks", "detail": str}`.
**Exit codes:** 0 success; 1 an expected failure (the message goes to stderr, or with `--json` to stdout as `{"error": "..."}`;
`doctor` prints its report and exits 1 when a check fails); 2 flags the command line could not parse.

Not tested on Windows from here: the detached start (`DETACHED_PROCESS`), `taskkill`/`tasklist`/PowerShell process lookups,
`schtasks` and the file ACLs are written to the documented Windows commands and unit-tested by their exact text, but have not
been run on a Windows machine.

## What protects it

- **Every connection is mutually authenticated TLS 1.3** with certificates from the farm's own authority. Being
  on the LAN gives no access. Certificates last 90 days.
- **Pairing** uses a one-time code (12 characters, valid for 10 minutes, a few tries, single use) *and* the
  coordinator's fingerprint, which you read from its screen: the new device accepts only the coordinator with
  that fingerprint, then proves it knows the code without sending it. The enrolment port exists only while an
  invitation is open.
- **Policy is checked on every request.** A worker may only run jobs, a client may only submit, list, cancel
  and fetch its own work. Nothing is allowed that is not granted.
- **A job is data, not a command.** The worker builds the solver's command line itself from a validated record:
  only settings that exist in the worker's own solver and are not `expert`-tier, never `backend`, a step limit,
  no values that look like options, no paths, no shell. `--gpu`, `--device` and the job folder are set by the
  worker. The job runs in its own folder, with a time limit, a scrubbed environment and a process group the
  worker can kill (cancel and timeout stop the whole tree).
- **Limits**: input 64 MB, results 256 MB, 120 minutes, 1,000,000 steps, 100 waiting jobs per person by default;
  `policy.json` in the farm folder can tighten them per device.
- **Audit log**: every decision (who, what, allowed or not) is appended to `audit.log` in the farm folder.
- Workers never listen. The coordinator listens only on the address you give it. Use a firewall rule that
  allows that one port from your LAN only.
- Finished jobs, their inputs and results are deleted from the coordinator after 7 days.

The farm folder (`~/Library/Application Support/emfdtd/farm` on macOS, `%APPDATA%\emfdtd\farm` on Windows,
`~/.config/emfdtd/farm` elsewhere; override with `EMFDTD_FARM_HOME`) holds private keys. It is created readable by
you only: mode 0700/0600 on macOS and Linux, and on Windows (where mode bits do not restrict access) inheritance is
removed and only your account is granted access (`icacls`).
It is never stored next to a board file, so a copied project cannot turn the farm on.

## GPU turns held by the coordinator

When the GPU's lock lives on the coordinator's machine (for example a Windows eGPU whose turn queue is a script on the
Mac), list the worker in `gates.json` in the coordinator's farm folder. The coordinator then takes the turn before
it offers a job to that worker and gives it back when the job ends, fails, is cancelled or the worker is lost; if
the turn never comes the job goes back to the queue without counting an attempt:

```json
{
  "win-egpu": {
    "hold": ["sh", "/path/to/win_gpu_lock.sh", "hold", "farm-{job}-{lease}", "{minutes}"],
    "release": ["sh", "/path/to/win_gpu_lock.sh", "release", "farm-{job}-{lease}"]
  }
}
```

`hold` must be a command that prints a line starting with `HOLDING` once it has the turn and keeps the turn until
the `release` command makes it exit (the coordinator runs `release`, then waits up to `release_wait_s` seconds,
default 20, for `hold` to exit on its own so it can clean up, and kills it only after that: a lock script killed
first leaves a stale lock); `{worker}`, `{job}`, `{lease}` (so a retry of a job takes a new turn label) and `{minutes}` are filled in. Start such a worker with
`--turn-from-coordinator`. An optional `"max_minutes"` caps the job's time limit for that worker (the turn is held
for that plus 10 minutes), so a shared GPU is never held for the default two hours.

**The turn is never given back while the solver might still be running.** A worker that is cut off from the
coordinator (a network partition, the coordinator's machine asleep) would otherwise keep using the GPU after the
lock said "free". So: the coordinator pings every worker every 5 s (or a quarter of the worker's watchdog if
shorter); if a worker hears nothing authenticated for its *watchdog* time (`--watchdog`, default 20 s) it kills the
solver's whole process tree, refuses to start a solve, and drops the connection; and the coordinator
gives a turn back only when the worker has said the solver is dead (a result, a failure, or a `stopped` message
after a cancel; a cancel stops the job wherever it is: fetching input, checking the device or in the solve, and nothing of it is left alive when `stopped` is sent) or, failing that, when the worker's watchdog plus a 10 s margin has certainly passed since the job
ended on the coordinator's side. If the hold command itself ends while the job runs, the coordinator cancels the
job and requeues it.

## Pausing and resuming

A worker that says `features: ["checkpoint"]` in its hello (this version and later) runs every job with the solver's checkpoints on (`docs/checkpoint.md`), so a job that is stopped can continue instead of starting over. **A checkpoint lives only on the worker that wrote it and resumes only there, on the same GPU, driver and binary.** Nothing moves between machines. An older worker that says nothing keeps the old behavior: no checkpoint flags are sent to it, and a job at its ceiling fails as before.

**What is saved where.** Each job gets a folder under the worker's work folder (`jobs/JOBID/`) with `in/` (the snapshot), `run/` (the results folder) and `ckpt/` (the solver's two checkpoint generations, `ckpt.a` and `ckpt.b`, written every 5 minutes by default; `serve --checkpoint-every SECONDS` changes it, clamped to 10 s to 1 h on the worker). `checkpoint.json` records the newest step; it is what the worker lists in its hello. A checkpoint is about 28 bytes per cell per generation (the 20 M cell board: about 0.55 GB each). The folder of a finished, failed or cancelled job is removed. The folder of a paused job is kept for 7 days (the worker's retention time, checked at every connect), then removed; the job is then `failed` with the reason `paused at step N, but WORKER no longer has its checkpoint (kept for 7 days); submit the job again`.

**The ceiling is now per run segment, not per job.** At the worker's time ceiling the worker no longer kills the solver: it writes `ckpt/stop`, waits up to 60 s for the solver to write a checkpoint and exit (status 75), and reports the job **paused** at that step (a solver that does not leave in time is killed and the job fails as before). A 65 minute job on a worker with a 45 minute ceiling therefore pauses once and finishes after a resume. The coordinator never raises a worker's ceiling.

```
paused at step 91200 on mac-m1 (ceiling reached); resume with: emfdtd-farm resume JOBID [--max-minutes N]
```

- `emfdtd-farm resume JOB [--max-minutes N]` (the submitter, with the same grant as cancel) queues the job again, **pinned to the worker and GPU that hold the checkpoint**, and the solver continues with `--resume`. `N` shortens this run's ceiling; more than the worker allows is refused (`mac-m1 allows at most 120 minutes per run: resume with --max-minutes 120 or less`). Resuming does not count as a failed attempt. A resume after the ceiling is never automatic: the owner decides.
- `emfdtd-farm restart JOB` drops the checkpoint (the worker deletes the folder) and starts the job again from step 0 on any worker. `cancel` also removes it.
- **After a lost connection, a restart of the coordinator or of the agent** a job that has a checkpoint is not sent to another worker. It waits for the worker that holds it (`waiting for mac-m1 to come back (it holds the checkpoint at step 91200); starts again from step 0 on any worker after 60 min`) and continues automatically when that worker is back, because the worker lists its checkpoints at every connect. If the worker does not return within the waiting time (`serve --hold-minutes N`, default 60; 0 = never wait) or comes back without the checkpoint, the job starts again from step 0 on any worker (`gave up waiting for mac-m1's checkpoint after 60 min; starting from step 0`). A job that never wrote a checkpoint is requeued at once, as before.
- **`agent stop` and a worker's SIGTERM** are polite: the worker first tells the coordinator its GPUs are out (so the paused job is not offered back to it), asks the solver for a checkpoint and waits for it (up to 75 s; a second signal kills at once). The job shows `paused at step N (the worker stopped); it resumes when WORKER is back` and continues when the agent starts again. This does not count as a failed attempt. `agent stop` now waits up to 90 s by default for that.
- The GPU turn of a paused job is given back exactly like the turn of a finished job (the coordinator-held turn after the worker confirms the solver is gone; the lock command ends with the solver).
- **Disk.** `agent doctor` now wants 2 GiB free for work plus 2 GiB for a job's checkpoint (2 x 1 GiB, because the size of the biggest job is not known). `queue --json` shows `checkpoint_step` for a job that has one; a paused job's reason says how to resume it.

Not covered: moving a checkpoint to another machine or device, jobs with animation views (those are not farmed), sweeps (each point is its own job and pauses on its own).

## Jobs that would not fit

Every worker has a longest-job ceiling (its agent's `--max-minutes`, lowered further by the coordinator's policy and gates). A job that outruns it is killed and fails, and the time already spent is lost: there is no checkpoint to resume from. To avoid that, `submit` sizes the job first and the scheduler places it only where it fits.

- **The estimate.** `submit` runs the local solver's dry run on the CPU (`emfdtd --cpu --dry-run --json`, in a scratch folder of its own, no GPU turn) for the cell count and the solver's upper bound on steps, scales the bound to what runs really take (0.5 of it at the default stop threshold, 0.7 at 3e-4, all of it below that; measured on the antenna model, see `farm/estimate.py`) and sends `estimate: {cells, steps}` with the job. A sweep uses its first point. `--no-estimate` skips the dry run; if the dry run cannot run, the job goes without an estimate (a note on stderr) and is placed as before.
- **The ceilings.** A worker tells the coordinator its own ceiling when it connects; the coordinator takes the smallest of that, the policy's `max_minutes` for the worker and a gate's cap. An older worker that does not say is judged by the last two and, after its first time-limit failure, by the limit it hit.
- **Placement.** Expected seconds on a device are cells x steps / measured speed x 1.25. A device is skipped when that exceeds its worker's ceiling; a device with no measured speed is judged at the slowest speed measured anywhere (if none is measured, nothing is skipped). Among devices that fit, the fastest idle one is chosen as before.
- **Waiting.** A job that fits nowhere right now stays queued and says why in `queue`: `waiting for mac-m1 (busy)`, or `needs about 49 min; the online workers allow 45 (win-egpu) and 45 (mac-m1)` when every online ceiling is too low (raise one with `emfdtd-farm agent configure --max-minutes N` on that machine, or connect a worker that allows more; the reason updates by itself).
- **Pinning.** `submit --device mac-m1` (or `mac-m1:Apple`, a GPU whose name contains the text) sends the job only there; it waits for that worker if it is busy or away (`waiting for mac-m1 (not connected)`) and the submission is refused if no enrolled worker has that name.
- **When a job still hits a ceiling** its reason reads `over the time limit of 45.0 minutes on win-egpu (its ceiling). Raise the ceiling on that machine (emfdtd-farm agent configure --max-minutes N) or send the job to a faster/longer-allowed worker with --device.`

The estimate and the pin are hints for the scheduler: the coordinator removes them from the job it hands to the worker, and the worker enforces its own ceilings whatever a submitter says.

## Restarting the coordinator

Stop it with Ctrl-C or SIGTERM (`kill PID`): it gives back any GPU turn it is holding and closes its ports before it
exits, so a stop never leaves a lock held. Do not kill it with SIGKILL while a turn is held: the lock's hold command
would be left running until its time limit. `gates.json` is read again before every dispatch, so adding or changing a
gate applies to the next job without a restart; a `gates.json` that is broken at that moment is ignored and the last
good one stays in force (a broken one at start-up stops the coordinator with a message).

## Fairness and sharing

Several people can share a coordinator. Jobs are served round-robin by how many each person has already had
started, so one person's long sweep does not starve everyone else. Each device runs one job at a time and the
fastest idle device is chosen first (speed is measured from completed jobs; a device with no measurement yet ranks after measured ones but still gets a job when it is the only one idle, which is how a new worker records its first speed; equal speeds go by worker name). A job is never held back for a faster device that is busy. On a worker that shares its GPU with
other tools, wrap the solver in your own lock with `--lock-command`, for example
`--lock-command /path/to/with_gpu_lock.sh nice -n 10`.

## Limits of this version

- The queue is saved (`jobs.json` in the farm folder, on every state change) and comes back when the coordinator
  starts again: waiting jobs stay queued, finished jobs and their results are still listed, and a job that was
  running is queued again with that attempt counted (its worker's watchdog killed the solver), or fails with "gave
  up" if it had used its attempts. Fair-share counters start again from zero, and progress text is not kept.
- Machine-wide Windows service or launchd installation is not provided; run the coordinator and workers in a
  terminal or under your own service manager. A Windows worker needs Python installed by you.
- The window's Farm tab runs on macOS and Linux; the Windows build of the window has no Farm tab yet (use the command line).
- The model is prepared on the worker while it holds the GPU turn (for a large board that is about a third of the time); preparing it first is designed but not built (docs/PLAN.md, "prepare before the turn").
- The coordinator does not check that a job's memory estimate fits the chosen GPU; a job that does not fit fails
  on the worker with the solver's own message.
- Discovery is manual (host and port); there is no mDNS.
- Different GPUs and drivers give slightly different floating-point results. Compare runs from the same kind of
  device, or check a known case on each new device first. Measured so far: a small patch antenna agrees with the
  solver's CPU reference to 4e-8 on an RTX 4060 Ti, and the TI reference board (118,000 steps) agrees between that
  RTX and an Apple M1 Pro GPU to 4e-7 in S11 (the same resonance to four digits); MoltenVK and KosmicKrisp agree to
  3e-8.

## Troubleshooting

- *cannot enable: this Python has no TLS 1.3*: use a newer Python (see Requirements).
- *the coordinator's fingerprint is ..., not the one you gave*: stop; the address you reached is not your
  coordinator, or the fingerprint was mistyped.
- A worker that cannot connect keeps retrying with growing pauses (1 s up to 30 s).
- *does not exist / not allowed* on submit: the setting is unknown to the worker's solver, `expert`-tier or
  denied; list settings with `emfdtd --list-settings`.
- Windows prompts for a firewall rule the first time the coordinator listens: allow it for the private network
  only.
