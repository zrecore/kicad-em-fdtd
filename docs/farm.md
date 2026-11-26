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
     when the lock lives on another machine. The worker offers the devices recorded earlier with
     `devices --save` (run once while you hold a turn), so it makes no Vulkan call of its own at connect time.
   - `--no-lock`: nobody else uses this GPU.

   ```
   emfdtd-farm devices --solver C:\emfdtd\emfdtd.exe --no-lock                       # see the GPUs (one Vulkan call)
   emfdtd-farm worker --solver C:\emfdtd\emfdtd.exe --device "RTX 4060 Ti" --lock-command /path/to/with_gpu_lock.sh
   ```

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

## The Farm tab

The emfdtd window has a **Farm** tab for what the command line does: turn roles on and off (turning one on asks
you to confirm and says what it allows), see the farm's devices, watch your jobs (state, a progress bar, the phase,
Cancel), create an invitation for a new device (the one-time code and this machine's fingerprint are shown) and list
or revoke members. It runs the same `emfdtd_farm.py` while the tab is showing and nothing otherwise, and it needs the
same Python with TLS 1.3 (set `EMFDTD_PYTHON` to choose one; `enable` records the Python it ran under). The progress
bar is the solver's estimate ("about N %": 60 % for the pulse, 40 % for the ring-down).

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

## Fairness and sharing

Several people can share a coordinator. Jobs are served round-robin by how many each person has already had
started, so one person's long sweep does not starve everyone else. Each device runs one job at a time and the
fastest idle device is chosen first (speed is measured from completed jobs). On a worker that shares its GPU with
other tools, wrap the solver in your own lock with `--lock-command`, for example
`--lock-command /path/to/with_gpu_lock.sh nice -n 10`.

## Limits of this version

- The queue is kept in memory: restarting the coordinator loses waiting and running jobs (finished results on disk
  stay until their 7 days are up, but are no longer listed).
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
