# Setting up a farm worker on your machine

This guide is for a person who wants to lend their computer's GPU to someone else's emfdtd GPU farm, on Windows,
macOS or Linux, **with no assistant and without the source code**. Follow it from top to bottom.
For what the farm is, how it is protected and how it behaves, see `farm.md` in the same folder.

Two people are involved:

- **You**, the *worker owner*: your machine runs simulations for the farm. Your machine only connects **out**.
  It opens no port.
- **The coordinator owner**: runs the machine that keeps the queue. They give you a few details (section 3) and
  you give them the name you want your machine to have. Their part is described in section 9.

**Three ways to do the same thing:** this guide's plain commands; `emfdtd-farm setup` (a question-by-question
wizard in the terminal for sections 4 to 7, with a flag for every answer); and `emfdtd-farm gui` (a window with the
same steps, a status panel with Start and Stop, the checks, the settings and the log). The window needs Python with
Tk: it is included in the python.org and Windows installers; on Homebrew install `python-tk` for your Python
(`brew install python-tk@3.12`); on Debian and Ubuntu install `python3-tk`. If Tk is missing the window says so and
the commands still work.

## 1. Before you start

| You need | How to check it |
|---|---|
| **Python 3.9 or newer with TLS 1.3.** The farm refuses to run without TLS 1.3 and does not fall back to an older version. | `python3 -c "import ssl; print(ssl.OPENSSL_VERSION, ssl.HAS_TLSv1_3)"` must end with `True`. On Windows use `python -c ...`. The launcher in this folder looks for a suitable Python itself; if it finds none it tells you what to install. |
| **The `emfdtd` program for your machine** (a build for your operating system, from the coordinator owner or a release archive). | `emfdtd --version` prints `emfdtd 0.1.0` (or newer). On Windows run `emfdtd.exe --version`. |
| **A GPU with Vulkan 1.2 or newer and its driver.** | `emfdtd --list-devices` prints one line per GPU, for example `0  NVIDIA GeForce RTX 4060 Ti  uuid 99cbe8b6...  discrete  15.7 GiB  vulkan 1.4  driver NVIDIA 621.24`. A GPU marked `(below Vulkan 1.2: not usable)` cannot be offered. This command makes a Vulkan instance: run it when nobody is using the GPU. |
| **A network path out** to the coordinator's farm port (default `7420`). | Nothing to open on your side: no inbound firewall rule is needed. If your network blocks outgoing connections to that port, ask the coordinator owner for the address and port to use. |
| **A correct clock.** Certificates last 90 days and are checked against your clock. | Check the date and time, and the time zone. |

macOS note: the Python that comes with Xcode and the command-line tools does **not** have TLS 1.3. Install one with
`brew install python` or from python.org.

## 2. Install the bundle

1. Unpack the archive anywhere you like (for example `C:\emfdtd-farm-agent` or `~/emfdtd-farm-agent`). Everything
   it needs is inside the folder; there is nothing to install and no packages to download.
2. Check that the files arrived intact: the file `SHA256SUMS` lists the expected checksums.
   macOS and Linux: `cd` into the folder and run `shasum -a 256 -c SHA256SUMS` (or `sha256sum -c SHA256SUMS`).
   Windows (PowerShell): `Get-FileHash tools\emfdtd_farm.py -Algorithm SHA256` and compare it with the line in the file.
3. Smoke test. macOS and Linux: `./emfdtd-farm status`. Windows: `emfdtd-farm.cmd status`.
   You should see:

   ```
   farm state folder: /Users/you/Library/Application Support/emfdtd/farm
     coordinator  off
     worker       off
     client       off
   ```

   Everything is **off**. Nothing runs, listens or connects until you turn it on. In the rest of this guide
   `emfdtd-farm` means that launcher (`./emfdtd-farm` or `emfdtd-farm.cmd`); you can put the folder on your PATH.
   If it prints `emfdtd-farm: no suitable Python found.`, install Python as it says and try again.

Where your farm identity lives (keep it private): `~/Library/Application Support/emfdtd/farm` on macOS,
`%APPDATA%\emfdtd\farm` on Windows, `~/.config/emfdtd/farm` on Linux. It holds your private key and is made readable by
your user only. It is not inside the bundle folder, so updating the bundle does not touch it.

## 3. What to get from the coordinator owner

Ask for these four things and for the name they will give your machine (letters, digits, `-`, `_` and `.` only):

1. the address and port where the **invitation** is open (for example `192.168.1.20:7421`),
2. the **one-time code** (looks like `ABCD-EFGH-JKLM`; it works once and expires in minutes),
3. the coordinator's **fingerprint** (sixteen groups of four characters). Compare it as read to you or sent on another
   channel; do not take it from the same place as the code if you can avoid it,
4. the address and port of the running **coordinator** (for example `192.168.1.20:7420`).

The coordinator owner opens the invitation only when you are ready, so do sections 4 and 5 first.

## 4. Turn the worker role on

```
emfdtd-farm enable worker
```

It describes what the role allows and asks you to type `enable worker` to confirm:

```
This machine may act as a farm worker: it will be able to connect OUT to a coordinator you enrol it with (it never listens) and run the solver on this machine's GPU for jobs that coordinator sends.
Type 'enable worker' to confirm:
```

After you confirm: `farm worker is on`. Add `--yes` to skip the question. (`emfdtd-farm setup` walks through
sections 4 to 7 with these questions in order, or use the window.)

## 5. Join the farm (enroll)

When the coordinator owner says the invitation is open, run:

```
emfdtd-farm enroll INVITE_HOST:PORT --fingerprint FINGERPRINT --code CODE --coordinator COORDINATOR_HOST:PORT
```

with the four things from section 3. The tool checks that the machine it reached has exactly that fingerprint
**before** it sends anything, then proves it knows the code without sending the code. On success:

```
enrolled as worker NAME. Next: emfdtd-farm worker --solver PATH --device NAME
```

Your private key and certificate are now in the farm folder. If it fails, see section 8.

## 6. Tell the farm which GPUs to offer, and how your GPU takes turns

First record which GPU(s) you offer. This runs the Vulkan device listing once, so do it when the GPU is free (the lock
mode below matters here). The `--solver` path is your `emfdtd` program; `--device` is a part of the GPU's name or the
start of its UUID from `emfdtd --list-devices`.

Now choose **how the GPU takes turns**. A worker never uses the GPU outside a turn, so you must pick exactly one:

| Your situation | Choose | What it does |
|---|---|---|
| **Nobody else uses this GPU.** It is a dedicated machine. | `--no-lock` | Runs jobs whenever they arrive. |
| **You, or other programs, also use this GPU** and there is a way to wait for your turn (a lock script, a queue). | `--lock-command COMMAND` | Every GPU call is run as `COMMAND the-solver arguments...`: your command waits for the turn, then runs what follows it, and returns when it is done. If the command needs options of its own (for example `nice -n 10`), put it in a small script and give the script's path, because option-like words are read as options of `emfdtd-farm`. |
| **The coordinator owner holds this GPU's turn** (the lock lives on their machine, for example a shared eGPU). | `--turn-from-coordinator` | The coordinator takes the turn before it offers a job and says so in the offer; the worker refuses any offer that does not say so. |

Record the devices once, in the same mode:

```
emfdtd-farm devices --solver /path/to/emfdtd --save --device "RTX 4060 Ti" --no-lock
```

(use `--lock-command ...` instead of `--no-lock` when you chose that mode; with `--turn-from-coordinator` run it
while you hold a turn and use `--no-lock`). It prints the GPUs it found and then `recorded 1 device(s) in ...`. The
worker announces these saved devices when it connects, so it never needs the GPU just to join; the live check that the
GPU is still there happens before every job, inside the turn.

## 7. Start the worker

Optional limits of your own (the coordinator can only make them stricter, never looser): `--max-minutes N` (default 120),
`--max-steps N`, `--max-input-mb N`, `--max-tier essential|common|advanced|expert` and `--watchdog SECONDS` (default 20: if the
coordinator is silent that long, the solver is stopped).

```
emfdtd-farm worker --solver /path/to/emfdtd --device "RTX 4060 Ti" --no-lock --max-minutes 45
```

It prints `worker connecting out to HOST:PORT (Ctrl-C to stop)` and then stays quiet while it waits for jobs.
If the coordinator cannot be reached it keeps retrying with growing pauses (1 s up to 30 s) and prints nothing more.

- **Leave it running** in a terminal window, or run it in the background:
  macOS and Linux: `nohup emfdtd-farm worker ... > worker.log 2>&1 &` (stop it with `kill PID`);
  Windows: `start "" /B emfdtd-farm.cmd worker ... > worker.log 2>&1` (stop it in Task Manager), or simply leave a
  Command Prompt window open.
- **Stop it** with Ctrl-C in its window. Any job it is running is stopped and given back to the queue.
- It does **not** start by itself when the machine restarts unless you set that up.

The same, managed for you, with a saved configuration (`emfdtd-farm agent configure` saves the solver path, the
devices, the turn mode and the limits once; `agent start` then needs no options):

```
emfdtd-farm agent doctor       checks Python, TLS 1.3, the solver, the devices, the enrolment and the coordinator
emfdtd-farm agent start        starts the worker in the background and returns
emfdtd-farm agent status       running or not, the last job, the last error
emfdtd-farm agent logs         the end of the worker's log
emfdtd-farm agent stop         stops it and any running job (the solver and its child processes too)
emfdtd-farm agent restart      stop, then start
emfdtd-farm agent configure --show     what is saved (add --json for a program to read)
emfdtd-farm agent autostart on|off    start it when you log in (off by default; your choice; uses launchd on macOS, a systemd
                                      user service on Linux, Task Scheduler on Windows, and runs the same `agent start`)
```

## 8. If something goes wrong

The exact messages the tool prints, and what to do:

| Message | What it means and what to do |
|---|---|
| `emfdtd-farm: no suitable Python found.` | No Python 3.9+ with TLS 1.3 on this machine. Install one (python.org, `brew install python`, your distribution's python3) or set `EMFDTD_PYTHON` to the full path of one. |
| `cannot enable: this Python has no TLS 1.3 (... ); the farm requires it. Use a Python built with OpenSSL 1.1.1 or newer.` | The Python in use is too old or built with LibreSSL. Same fix; run `emfdtd-farm --which-python` to see which one the launcher picked. |
| `the farm worker is off. Turn it on in the Farm screen or with: emfdtd-farm enable worker` | You skipped section 4. Run `emfdtd-farm enable worker`. |
| `turn on the worker or client role first: emfdtd-farm enable worker\|client` | Same: enable the role before `enroll`. |
| `expected HOST:PORT, got '...'` | An address needs a port, for example `192.168.1.20:7421`. |
| `enrolment failed: could not reach the coordinator: ...` | The invitation address is wrong, the invitation is not open yet, or a firewall in between blocks outgoing connections to it. Check with the coordinator owner. |
| `enrolment failed: the coordinator's fingerprint is XXXX-..., not the one you gave: stop, this is not your coordinator` | **Stop.** The machine you reached is not the coordinator whose fingerprint you were given, or you mistyped it. Re-check the fingerprint with the coordinator owner before trying again. |
| `enrolment failed: the code was not accepted` | Wrong code. Each invitation allows only a few tries; the coordinator owner can create a new one. |
| `enrolment failed: no invitation is active` | The invitation expired, was used up, or has not been opened. Ask for a new one. |
| `this machine has no coordinator address: enrol it first (emfdtd-farm enroll ...)` | Section 5 has not been done (or the farm folder was deleted). |
| a Python error whose last line is `emfdtd_plugin.farm.pki.PkiError: no worker identity on this machine: enrol it first` | Same: your key and certificate are not in the farm folder. Do section 5 (the message is only shown as an error trace for now). |
| a Python error whose last line is `emfdtd_plugin.farm.pki.PkiError: this Python's TLS library (...) has no TLS 1.3, which the farm requires; use a Python built with OpenSSL 1.1.1 or newer` | The Python running the tool has no TLS 1.3 (any command but `status`, `enable` and `disable` needs it). Use the launcher (`emfdtd-farm`), which picks a suitable Python, or set `EMFDTD_PYTHON`. |
| `choose the GPUs to offer with --device (see `emfdtd-farm devices --solver ...`)` | `worker` needs `--device NAME`. |
| `give exactly one of: --lock-command ..., --turn-from-coordinator ..., or, only if nobody else uses this GPU, --no-lock` | Pick one turn mode (section 6), not none and not two. |
| `no recorded devices in ...: run `emfdtd-farm devices --solver ... --save --device NAME` first, with --lock-command ... or --no-lock ...` | Do the recording step in section 6. |
| `the solver could not list devices: ...` | `emfdtd --list-devices` failed. Run it by hand to see why (Vulkan driver missing or out of date, wrong `emfdtd` build). |
| The worker runs but never gets jobs; `emfdtd-farm worker` shows nothing more | Normal while idle. If the coordinator owner cannot see you in their device list, check your clock, your network path to the farm port, and that the owner has started the coordinator. The worker retries on its own. |

## 9. The coordinator owner's part

On the machine that keeps the queue (needs the `openssl` command-line tool, which macOS and Linux have):

```
emfdtd-farm enable coordinator
emfdtd-farm init --host 192.168.1.20            # once: creates the farm's certificate authority; prints the fingerprint
emfdtd-farm invite NAME --role worker --listen 192.168.1.20 --port 7421
```

`invite` prints the code and the fingerprint and listens on that one address until the device joins or ten minutes pass.
Send the worker owner the four things in section 3. Then run the coordinator:

```
emfdtd-farm serve --listen 192.168.1.20 --port 7420
```

Allow **one inbound port** (the `serve` port) from your LAN in the firewall, and the invitation port while an invitation
is open. `emfdtd-farm members` lists who has joined; `emfdtd-farm revoke worker NAME` removes a device for good, and its
certificate stops working at once.

If a worker's GPU is shared and its turn lives on the coordinator's machine, list it in `gates.json` in the coordinator's
farm folder, so the coordinator takes the turn for each job and releases it afterwards:

```json
{
  "NAME": {
    "hold":    ["sh", "/path/to/lock.sh", "hold",    "farm-{job}-{lease}", "{minutes}"],
    "release": ["sh", "/path/to/lock.sh", "release", "farm-{job}-{lease}"],
    "max_minutes": 45
  }
}
```

`hold` must print a line starting with `HOLDING` once it has the turn and keep it until `release` makes it exit. The
worker is then started with `--turn-from-coordinator`. Details, limits and the restart behavior: `farm.md`.

## 10. Updating and removing

- **Update the bundle:** stop the worker; unpack the new bundle into a **new** folder (do not overwrite the old one);
  run `emfdtd-farm status` from the new folder (your identity in the farm folder is picked up unchanged); start the worker
  again from the new folder (`emfdtd-farm agent doctor` first checks everything and `agent start` runs it itself).
  Update `emfdtd` itself the same way: stop, replace the program, run `emfdtd --version`, start. Update the bundle and the program together:
  a worker offers checkpoints (pause and resume at the time ceiling) only if its `emfdtd` lists `--checkpoint-dir` in the usage text
  (`emfdtd` with no arguments prints it), and a solver older than the voxelizer fix of 2026-11-26 gives different results for sheet copper.
- **Pause:** stop the worker. Nothing else needs to change.
- **Leave the farm:** stop the worker; run `emfdtd-farm disable worker`; ask the coordinator owner to run
  `emfdtd-farm revoke worker NAME`; delete the bundle folder; if you want your key gone too, delete the farm folder
  (section 2 says where it is).
