# Lessons from mistakes (read before comparing solvers, changing the farm, CI or a command line)

Each entry: what went wrong, the rule it produced, and where the rule is enforced. Add one whenever a mistake costs
more than an hour; a rule with no enforcement is a wish, so say how it is checked.

## Comparing against a reference solver

1. **A control that tests nothing looked like a pass (2026-11-26).** The hand-made coupon snapshot had no `pads`, so
   `--part` silently did nothing and open, short and load agreed to 1.3e-6. *Rule:* a control must be shown to
   differ before its agreement means anything; a flag that can be ignored must say so. *Enforced:* the solver warns
   ("part R1 ignored: ... lists 0 pads", tested); `.claude/skills/fdtd-control-check` step 0.
2. **A reference number outlived the code that made it (2026-11-25 to 27).** The 25 November patch reference
   (-6.043 dB at 2.2195 GHz) was a coin toss on snapped edges, fixed by `872d69b`; for two days Windows and the Mac
   were "failing" to reproduce it. *Rule:* every reference number carries the commit, build and flags that made it,
   and is rerun on the current build before a comparison. *Enforced:* `.claude/skills/reference-comparison`.
3. **The reference was refined on one axis only (2026-11-27).** All openEMS runs had 83 mesh lines in z while x and y
   changed, so its vertical convergence was never tested; the Windows session saw the repeated 83 by reading the mesh
   line before a long run. *Rule:* vary every axis separately for each code and print per-axis counts. *Enforced:*
   `.claude/skills/reference-comparison`; `openems_case.py` (repository `/Volumes/CrucialX9/Projects/openEMS-check`) prints them.
4. **A trend claimed from one refined code (2026-11-27).** "The gap shrinks 3.0, 2.3, 2.0 %" compared our refined
   runs with an unrefined openEMS run; refining openEMS moved it up 0.96 % and the gap stayed near 3 %. *Rule:* no
   convergence statement until both codes are refined; label a one-sided trend as such. *Enforced:* `docs/trust.md`
   (statuses), review.
5. **An estimate was used without ever being checked against reality (2026-11-27).** The farm's memory check uses
   the solver's estimate (fields, materials, CPML only) with a guessed 1.5 margin. *Rule:* a number that gates
   behavior is a ledger item until measured. *Enforced:* `docs/trust.md` Open row; the allocation counter is the fix.

## Farm, CLI and CI

6. **A command was only tested through its parser stub (2026-11-27).** `emfdtd-farm lock NAME --max-minutes 1 -- cmd`
   passed its tests, but `argparse` REMAINDER swallowed the options when it ran for real. *Rule:* run every new
   command once against the live system before calling it done. *Enforced:* `plugin/tests/test_farm_lock.py`
   parses a full command line; habit.
7. **Relaxing a one-at-a-time assumption exposed a second one (2026-11-27).** Two GPUs per worker were allowed
   (the scheduler already supported it) but the worker's status file held one job and showed none when either ended.
   *Rule:* when you lift a limit, search for state that assumed it. *Enforced:* a scenario with two jobs at once.
8. **A flaky test was a missing folder (2026-11-27).** The farm-client tests appended to a log in a scratch folder
   that a fresh CI runner had not created; the stand-in tool printed a shell error before its JSON. It looked random
   because test order decided whether another test had created the folder. *Rule:* tests create what they use; an
   error message shows the raw output it could not parse; a flake is a hypothesis to reproduce, not noise.
   *Enforced:* `tmpPath()` in `tests/cpu/farm_client_test.cpp`; the parse errors now quote the output.
9. **A guard was set from a wrong unit (2026-11-27).** The ctest `--timeout 300` killed the whole CPU suite, because
   one ctest test is the entire Catch2 binary. Also, jobs ran with GitHub's 360 minute default timeout, so a stuck
   build held a runner for 45 minutes. *Rule:* set a timeout on every CI job and step from the measured duration
   (about 3 times); know what a "test" is in each tool. *Enforced:* `.github/workflows/ci.yml`.
10. **A background watcher's exit was read as CI finishing (2026-11-27).** The command that started the watcher
    exited at once. *Rule:* wait on the thing itself (`gh run watch ID --exit-status` in the foreground, or check
    the run's state), never on the launcher. 
11. **A long-lived service ran old code.** The coordinator rejected a field the new client sent until it was
    restarted. *Rule:* after a protocol or schema change, restart the long-lived processes (when idle) and test
    one real submit. 
12. **A blanket `git add -A` committed another agent's unfinished work (2026-11-27).** A subagent had written, but not
    compiled, a GPU memory counter in the working tree; my docs commit carried it into main and CI failed at the format
    check. *Rule:* while any agent works in the tree, stage by path (`git add docs/trust.md`), and read `git status`
    before every commit; code that was never built does not ride along with a docs change. *Enforced:* habit; CI
    caught it in 22 s because the format check runs first.
13. **A plausible mechanism was written into the ledger before it was tested (2026-11-27).** The strip-width
    explanation for the coarse-cell scatter was recorded as the "leading suspect" an hour before a single run could
    test it, and the test (a 30 um shift of the strip) refuted it. *Rule:* a suspect goes in the ledger labeled as
    untested with the test that would refute it, and is not given a name or a headline until that test has run.
    *Enforced:* `docs/trust.md` wording; review.
14. **A run that could not work was found by reading its log after 2 h 40 (2026-11-27).** `thin()` in
    `openems_case.py` (repository `/Volumes/CrucialX9/Projects/openEMS-check`) dropped the port's own mesh line when a snapped copper line lay within 0.07 mm,
    so openEMS ran 200 000 steps with the port never excited (energy exactly 0 on every line, a warning "unused primitive
    ... port_excite_1"). *Rule:* a script that builds a model checks that the model can do its job before a long run
    (the port is on the mesh), and an operator reads the first minutes of a long run's log for warnings and for a
    nonzero energy. *Enforced:* the guard `openems_guard.py` in the separate repository `/Volumes/CrucialX9/Projects/openEMS-check` (smoke run, then live checks on the console; usage in its README) and `openems_case.py` now exits before the solve if a port line is missing; `powershell`
    found it by reading the log and reported it with the evidence.
15. **Thresholds fitted to one log killed a healthy run (2026-11-27).** The run guard's instability rule was tuned
    on one healthy log (`c05`, whose excitation had ended long before) and stopped a healthy Windows run at step 18 270
    because the source was still on and the energy swings freely then. The unguarded run with identical inputs
    finished normally. *Rule:* a rule that kills a run is tested on every healthy log we have, from different cases,
    and may only act in a regime the log tells it has begun (here the end of the excitation signal); when in doubt it
    warns and does not kill. *Enforced:* a replay test of all healthy logs in `../openEMS-check`; the comparison of a guarded
    with an unguarded run on the same inputs, which is how it was found.

