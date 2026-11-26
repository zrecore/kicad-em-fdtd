# Checkpoint and resume

A headless solve can write its state to disk now and then, and be continued later from the newest checkpoint instead of starting over. The use is the job farm (a job that hits a time ceiling, a worker that restarts), but the flags work on any machine. **A checkpoint resumes only on the machine, device, driver and binary that wrote it, with the same inputs.** Anything else is refused with the reason. Moving a checkpoint to another machine is not supported yet.

## Use

```
emfdtd BOARD.json [options] --checkpoint-dir DIR [--checkpoint-every SECONDS]
emfdtd BOARD.json [the same options] --checkpoint-dir DIR --resume
```

- `--checkpoint-dir DIR`: write checkpoints into `DIR` (created if needed). A run without `--resume` first clears `DIR` of the files of an earlier run.
- `--checkpoint-every SECONDS`: wall-clock time between checkpoints (default 300). A checkpoint of the real board (about 20 M cells) is roughly 0.55 GB; see "Sizes" for the measured write time.
- `--resume`: continue the run saved in `DIR`. It writes into the results folder the run started in, and `summary.txt` says `resumed at step N`.
- **Pausing a running solve:** create the file `DIR/stop`, or send `SIGTERM` (POSIX). The run writes a checkpoint at its next quiet point (every 16 steps), prints `paused: checkpoint at step N` and exits with status **75**. That is not an error: resume it with `--resume`. The stop file is removed when the run is resumed, and a fresh run clears an old one.
- `--json`: a paused run prints `{"paused": true, "checkpoint_step": N, "checkpoint_dir": ..., "results_dir": ..., "exit_status": 75}`; a resumed run's document has `"resumed_from_step": N` (otherwise `null`).

Not allowed together with `--checkpoint-dir` (exit 2, with the reason): the window (`--ui`), `--view`, `--frames`, `--sweep`, `--dry-run`. The animation views keep phasor lists, which are not part of a checkpoint (they are 60 MB to 1 GB more state and the farm does not use them); a checkpoint is never silently skipped.

## What is saved

- The six field arrays, the CPML auxiliary arrays, the lumped-port state (inductor current and previous E), the step counter. GPU and CPU backends have their own layouts of the CPML state.
- The driver's own state: steps, progress, the decay window, and the whole port voltage, source and strip histories (S11 is computed from them at the end; there are no partial sums to save).
- Not saved, rebuilt from the same inputs: materials, update coefficients, CPML tables, PEC edges, port constants, the source waveform (a function of the step).

A resumed run gives **bit-identical** results to the same run left alone: the port voltage, source, every strip voltage, all six field arrays and S11 (tests: CPU in `tests/cpu/checkpoint_test.cpp`, GPU in `tests/gpu/checkpoint_gpu_test.cpp`, command line in `scripts/checkpoint_check.sh`).

## File format

`DIR/ckpt.a` and `DIR/ckpt.b`, written alternately (temporary file, flushed to disk, renamed over the older one), so a crash while writing leaves the previous checkpoint. On resume the newest intact generation that matches is used; a damaged or cut-short one is ignored and named in the message if nothing can be used.

```
magic "EMFDCKPT" | u32 format version (1) | META | HOST | ENGN
section: u32 tag | u64 length | u32 CRC-32C of the bytes | bytes
META: run hash, backend, device uuid and name, driver, emfdtd version, grid nodes, dt, steps, results folder
HOST: the driver state    ENGN: the backend state (fields, CPML, ports, histories)
```

Values are in the machine's own byte order. The **run hash** (128 bits, a change detector, not a security hash) covers the post-voxelizer model (grid edges, cell materials, material table, PEC edges, every port edge) and the resolved run options (frequency range, CPML cells, Courant factor, step limit, decay threshold, sample interval).

## Refusals

A resume fails with exit status 1 and one of these, per file: `is damaged: ...` (not a checkpoint, a checksum does not match, cut short), `was written by emfdtd X, this is Y`, `was written by the cpu backend, this run uses the gpu backend`, `was written on DEVICE (uuid), this run uses DEVICE (uuid): a checkpoint resumes only on the device that wrote it`, `was written with the driver ...`, `was written for other inputs (the board, the settings or the options differ)`, `is missing`.

## Not covered (yet)

Another machine or device (needs a backend-neutral layout: the CPML layouts differ between CPU and GPU, and float results differ slightly between GPUs), animation views and the window's own runs, sweeps (each point would need its own folder), and the farm side (a paused job state, `resume` and automatic resume after a worker restart: `docs/PLAN.md`, "Checkpoint and resume").
