---
name: reference-comparison
description: Compare this project's solver with another solver (openEMS) or with an earlier result, without fooling yourself. Use before stating a gap or a convergence trend, when a reference number is older than the code, when adding a run to the openEMS ladder, or when two machines disagree.
---

# Comparing against a reference

Read `docs/lessons.md` entries 1 to 4 first. The rules:

1. **Same inputs.** Same snapshot (sha256), same band, same dielectric constants; write the flags and cell counts next to every number. A different quantity is not a comparison: a reactance zero crossing is not an S11 minimum.
2. **Vary every axis separately, for both codes.** x, y and z each at two or more resolutions; the case script prints the per-axis mesh line counts (`mesh lines X x Y x Z`): if one of them does not change when you change a flag, that axis was not refined. openEMS: `scripts/antenna/openems_case.py` flags `--cell`, `--fine`, `--coarse`, `--margin` refine x and y only unless it has a z option.
3. **Rerun the reference on the current build** before comparing (`docs/lessons.md` 2). Record commit, driver or version (openEMS v0.37.0 on the Windows PC; Mac master `ccb4472`, one commit behind upstream, CSXCAD identical, checked 2026-11-27).
4. **Do not claim a trend until both codes are refined**, to the same kind of step; say "one-sided" otherwise.
5. **Look at the sign of the gap on more than one structure.** The same sign on all of them suggests one cause (a port or feed model); different signs suggest mesh or edge treatment.
6. **Use a second pair of eyes.** An independent session (the `powershell` agent running the case cold) found the unrefined z axis by reading the mesh line; ask for the cell counts before a long run.
7. **Where to run:** CPU openEMS runs go through `emfdtd-farm lock cpu:MACHINE --max-minutes N -- COMMAND` (Mac: `scripts/with_cpu_lock.sh`); our solver on the farm GPUs (`docs/farm.md`), and use both machines at once.
8. **Record the result** in `docs/trust.md` with its status (established, provisional, open) the same day.
