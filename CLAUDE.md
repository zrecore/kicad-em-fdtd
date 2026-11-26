# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

Working prototype on macOS. A board snapshot (JSON, `docs/snapshot.md`) goes through the voxelizer to the FDTD solver (CPU reference and Vulkan GPU, both with CPML and lumped ports) and out as S11 (Touchstone) plus an animated field view; the KiCAD plugin (`plugin/`) exports the open board and starts the solver. `docs/PLAN.md` is the source of truth for decisions, measurements and next steps; read it before starting anything and update it when a decision changes. The project is self-contained: scope is this repository only. The example board in `../bomu-hardware/bomu` is read-only input; never copy or commit anything from that repository here.

The GPU farm (`docs/farm.md`; jobs on other machines' GPUs, off by default; a Farm tab in the window; TLS 1.3 only) is built and in use between this Mac and the Windows eGPU box; read the status paragraph under "GPU farm" in `docs/PLAN.md` before touching it. The solver builds with MSVC (`CMakeLists.txt` has the branch); discrete GPUs use device-resident buffers (`vkcore::Residency::Device`).

Focus: make it work on this Mac (M1 Pro; Vulkan through KosmicKrisp, the driver Khronos now recommends, with MoltenVK as the deprecated fallback; see "GPU rendering" in `docs/PLAN.md`) first. Windows, Linux, CI and packaging come later; keep code portable but do not build for them yet (the Windows build and the farm worker there exist because the user asked for them; new Windows work still needs the user).

The goal is a real-time 3D FDTD Maxwell solver on the GPU (Vulkan 1.2 baseline; a Vulkan-on-Metal driver on macOS) with a live 3D field viewer, shipped as a cross-platform (macOS, Windows, Linux) KiCAD IPC plugin. The standalone window (`emfdtd --ui`) exists and its UX is in `docs/ux.md` (decisions the user made are marked there); the KiCAD plugin UI is still the minimal flow in `plugin/action_simulate.py`, so do not invent plugin UI beyond it and discuss any new UX with the user first.

## Commands

```
scripts/check.sh           # build (ci preset = debug + sanitizers + -Werror, as on GitHub), CPU tests, clang-format, ruff + pytest. Runs anywhere.
scripts/check.sh --gpu     # also GPU tests, under the shared GPU lock
scripts/check.sh --fast    # skip the [slow] solver suite (~6 min): only for work that does not touch the solver (UI, farm, plugin, docs); the full run stays the default and is what CI and releases use
cmake --preset release && cmake --build --preset release
/Volumes/CrucialX9/Projects/mongo/poc/gpu/tools/with_gpu_lock.sh nice -n 10 build/release/apps/bench/bench   # writes out/bench.json
build/release/apps/emfdtd/emfdtd SNAPSHOT.json [--ui] [--ui-screenshot out.png --ui-a11y-dump tree.txt] [--profile wifi|bluetooth|zigbee|lwmesh|uwb|custom[:VARIANT]] [--set KEY=VALUE] [--dry-run|--view|--frames N] [--cpu]   # results in <project>/em-fdtd/results/run-<time>/; --list-settings shows every setting; GPU only under the lock
# more flags: --nets A,B [--ground-nets G] (net-driven model selection), --part REF=open|short|fit|r600,l100n,c0.2p (a two-terminal part's treatment), --body REF=ignore|metal|dielectric:EPS[:TAND] and --body-label REF=TEXT (a component body's treatment), --refine X0 Y0 X1 Y1 CELL (mm, repeatable), --refine-gaps CELLS (fine cells across every narrow gap between two nets near a port; a dry run lists the suggestions), --open-run RUNDIR (show a saved run, no solve; use --cpu for screenshots), --prune-runs N (drop the field data of all but the newest N runs), --sweep KEY=a,b,c|start:stop:count (one run per value, results in sweep-<time>/), --geom NET.x=MM (stretch a net, solver side only), --probes x,y,z;x,y,z (mm), --ui-layers field,nofield,current,power,iso,voxels,smith,impedance,sweep,model (screenshots), --ui-export (3D volume as VTK); window: --ui-frames N (measure and exit), --cpu-ui; env EMFDTD_VALIDATE=1 (Vulkan validation in the window)
cd plugin && .venv/bin/python -I -c "import sys,runpy; sys.path.insert(0,'src'); sys.argv=['x','out.json']; runpy.run_path('tools/export_snapshot.py', run_name='__main__')"   # needs pcbnew open on a COPY of a board (read-only export)
python3 boards/w3008-testboard/generate.py   # regenerate the reference board; check with `kicad-cli pcb drc --refill-zones` on a copy (never --save-board on the committed file)
```
Checkpoint and resume (`docs/checkpoint.md`): `emfdtd BOARD.json ... --checkpoint-dir DIR [--checkpoint-every SEC]`, later `... --checkpoint-dir DIR --resume`; `DIR/stop` or SIGTERM writes a checkpoint and exits 75 (not an error); a resume is bit-identical to an uninterrupted run and only works on the same machine, device, driver, binary and inputs (refused with the reason otherwise); not with `--ui`, `--view`, `--frames`, `--sweep`, `--dry-run`. `scripts/checkpoint_check.sh` runs in `ctest -L cpu`; the GPU scenario is `tests_gpu "[checkpoint]"` (under the GPU lock).
Machine-readable output (`docs/cli-json.md`; schema 1, additive changes only): `emfdtd --list-settings|--list-devices --json`, `emfdtd SNAPSHOT [--dry-run] --json` (stdout is one document, text goes to stderr), `emfdtd --run-info RUNDIR [--s11] --json` (a saved run: board, grid, steps, best match, -10 dB band, no solve); `scripts/cli_json_check.sh` runs in `ctest -L cpu`.
GPU farm (jobs on other machines, off by default, `docs/farm.md`): `python3 plugin/tools/emfdtd_farm.py status|enable ROLE|init|invite|enroll|serve|worker|submit|queue|results ...`; TLS 1.3 is required with no exceptions and no fallback: `plugin/.venv` must be built on a Python with TLS 1.3 (here Homebrew `python@3.12`, OpenSSL 3.5), never the Xcode Python (LibreSSL, no TLS 1.3); the farm tests fail, not skip, if it cannot. Ruff still lints for Python 3.9 (the plugin must stay 3.9 compatible, KiCad's bundled interpreter) but the tests run on the venv's Python, so keep asyncio objects out of `__init__` and avoid post-3.9 APIs. `emfdtd --list-devices`, `--device SEL` choose the GPU; the window's Farm tab (`--ui-tab Farm`) is checked against a stand-in tool by `scripts/ui_farm_check.sh` (also run by `check.sh`; `EMFDTD_FARM_COMMAND` replaces the tool, `EMFDTD_PYTHON` picks the Python).
Vendor Gerbers into a snapshot: `python3 scripts/gerber/gerber_to_snapshot.py RECIPE.json OUT.json [--erase LAYER:x0,y0,x1,y1] [--fill LAYER:x0,y0,x1,y1]` (needs numpy; see `.claude/skills/gerber-to-model`; vendor files stay in git-ignored `out/`). Versioning and releases: semver in `VERSION` only (`scripts/version.sh show|check|bump|notes`; `check` also runs in `scripts/check.sh`); plugin/pyproject.toml follows it. CI is `.github/workflows/ci.yml`, releases are tag-driven (`release.yml`, tag `vX.Y.Z` must match `VERSION` and have a `## [X.Y.Z]` section in `CHANGELOG.md`). Never tag, push or bump without the user asking; the user commits and tags manually. Read-only files (data, shaders) are found through `core::resourceDir` (`src/core/resources.hpp`), never through a bare `FDTD_DATA_DIR`/`FDTD_SHADER_DIR` in the app, so a packaged build works from any folder. Package locally: `cmake --build --preset release && (cd build/release && cpack -G TGZ -B ../../out/dist)`.

Run one Catch2 scenario: `build/debug/tests/tests_cpu "Scenario: <name>"` (GPU tests only under the lock; filter with `[gpu]`). macOS: the `debug`/`ci` test presets set `VK_LAYER_PATH` to the SDK manifest dir because Homebrew's validation-layer manifest has a bare library name the loader cannot resolve; without it validation silently does not load. `build/`, `.venv/`, `plugin/.venv/` and `out/` are git-ignored.

## Planned architecture

```
KiCAD PCB editor -> thin Python IPC plugin (kipy) -> board snapshot -> native C++/Vulkan app
                                                     (voxelizer -> FDTD compute (SPIR-V) -> 3D viewer GLFW + ImGui)
                                                     backends: Vulkan GPU | CPU reference
```

- The plugin is a KiCAD IPC API plugin (`plugin.json`, separate process). Legacy SWIG `pcbnew` plugins are deprecated; do not use them. It must stay Python 3.9 compatible (KiCAD's bundled interpreter).
- The solver/viewer is a separate native process with its own window (a plugin cannot draw inside KiCAD) and must also run standalone. Never `fork()` after Vulkan is initialized (Metal's compiler cannot run in a forked process).
- One snapshot format (e.g. JSON plus binary blob) is shared by the plugin and the C++ app. The CPU backend sits behind the same interface as the GPU one (reference, fallback, differential tests).
- Toolchain: CMake C++20, GLSL to SPIR-V with `glslc` at build time (no CUDA), GLFW, Dear ImGui, VMA later via FetchContent. Dependencies and licences: see `docs/PLAN.md`.

## Constraints that shape the design

- **The plugin must work on any user's own design in KiCAD:** nothing hard-coded to the example boards (part, net, layer, size). See "Product requirement" in `docs/PLAN.md`.
- **The user selects which components** (footprints, nets, layers) are in the model and which are shown, as two independent selections stored in a sidecar file, never in the KiCAD design files.
- **Radio bands, channels and meshing rules** live only in `data/radio_profiles.json` (each with a source and verified/unverified status); the settings registry in `src/app/settings.cpp` is the one list of settings (tier, group, help, default) that drives the preview screen, search, saved files and run summary. Never hard-code a band or a cell size.
- **Accessibility is a requirement, not polish:** every new widget goes through the helpers in `apps/emfdtd/ui.cpp` (`button`, `checkbox`, `sliderInt`, `selectable`, `note`, ...) so it records a node with a name in the tree (`src/viz/access.hpp`); `emfdtd ... --ui-screenshot out.png --ui-a11y-dump tree.txt` exits 1 if a control has no name. Sizes and paddings go through `px()` so they follow the text-size setting; never hard-code pixels. Respect the system preferences (reduce motion, contrast, transparency, text size). Platform code stays in one file per concern (`src/viz/a11y_mac.mm`, `src/viz/screen_reader.cpp`); the tree and settings logic stay platform-neutral. Windows and Linux will add an AccessKit adapter in `screen_reader.cpp`, so do not call platform accessibility APIs anywhere else.
- **UI colours** (field colours per theme) live in `data/ui_defaults.json` and are user settings (View group); legends never name a colour. Sequential colour maps for the 3D magnitude view are data in `data/colormaps.json` (CC0, generated by `scripts/make_colormaps.py`).
- **Physical defaults** (stackup, materials, fab limits) live only in `data/fab_profiles.json`, each with a `source`; the user can override any of them (Settings, group PCB, saved in the sidecar file next to the board; `app::applyPcbSettings` applies them). A fab profile holds one stack per copper-layer count; mark every value the fab does not publish in the layer's `assumed` list. Never hard-code a material or fab number in code. See "Defaults and editable attributes" in `docs/PLAN.md`.
- **The 3D view is an animation of the waves**, decoupled from the solve (drawn on the GPU: compute shaders for the 3D volume and the slice, Dear ImGui's Vulkan backend for the UI; the CPU renderers are the reference, the fallback and the headless screenshot path): the solver accumulates a multi-frequency DFT at display resolution (one path for S11, steady-state and transient animation) and the viewer reconstructs each frame in a compute pass. Solver field buffers and the display image are separate; the snapshot cadence is a tunable.
- **Performance:** FDTD is bandwidth-bound. Fields stay in GPU memory, many steps per submit but each submit short (tens of ms, macOS watchdog), a graded grid (fine cells only where needed; the smallest cell sets the time step). Start with simple coalesced kernels and add tiling only if measured to help. Measure warm, against the measured copy bandwidth.
- **Portability:** kernels run on KosmicKrisp, MoltenVK, NVIDIA and Mesa without extension tricks (test GPU changes on both Apple drivers with `.claude/skills/gpu-driver-check`; some drivers give no device timestamps, see `vkcore::Timing::timestamps`); optional features detected at run time; never take the first enumerated device. Machine facts (M1 Pro Mac, Windows RTX 4060 Ti box, WSL limits) are in `docs/PLAN.md`.

## Coding standards
- BDD: write the scenario first (C++ Catch2 `SCENARIO/GIVEN/WHEN/THEN` in `tests/`, Python `pytest-bdd` Gherkin in `plugin/tests/features/`), with the tolerance in the `THEN`. Numerical scenarios are listed in `docs/PLAN.md`.
- Tests are labelled: `ctest -L "^cpu$"` runs anywhere (anchored: the label is a regex and would also match `cpu_slow`), `ctest -L gpu` needs the GPU lock. `scripts/check.sh [--gpu]` is the single health check (build, tests, clang-format, ruff).
- Style: C++20, `.clang-format` (2 spaces, 110 cols; format with `/opt/homebrew/opt/llvm/bin/clang-format`), `.clang-tidy`, `-Wall -Wextra -Wshadow`; presets `debug` (ASan+UBSan, validation layer), `release`, `ci` (`-Werror`). Python: ruff, target py39.
- DRY: shared flags live in `fdtd_options`, Vulkan plumbing in `src/vkcore` (RAII wrappers, one `VK_CHECK`), unit conversion in one header per language. No bare `vkDestroy*` outside `vkcore`. SI units (m, s, Hz) in the core; mm and nm only at the board boundary.
- Layout: `src/vkcore` (Vulkan), `src/core` (Vulkan-free solver pieces), `apps/`, `tests/`, `plugin/`, `poc/` (throwaway; promote or delete at milestone end). No volk for now (YAGNI): link the Vulkan loader.

## Licensing and sources

- The project is MIT. **Clean-room rule:** the mature FDTD/EM codes (openEMS, MEEP, gprMax) are GPL. Run them for validation and read their docs for behaviour, but never copy, translate or use their source as an implementation template. Write kernels from the papers in `docs/PLAN.md` References.
- When adding a dependency, check its LICENSE file, add it to the Licensing table in `docs/PLAN.md` and to `CREDITS.md`, and install its licence text in `cmake/Package.cmake`. Build-time-only tools are noted as such.
- When choosing a numerical method, cite a peer-reviewed source in References (with a verified DOI), and say if its contents have not been read.
- Do not implement a conformal/subcell PEC scheme before the patent check in `docs/PLAN.md`. `../bomu-hardware` is the user's own design, used read-only; by their instruction nothing from it is copied or committed here.

## Working rules

- Do not modify KiCAD design files from tools. Do not commit unless asked; the user commits manually.
- Experiment outputs go in git-ignored directories inside the repository (`out/`: samples in `out/samples/`, convergence logs in `out/convergence/`, demo boards in `out/kicad-demo/`). Never leave work in `/tmp` or another temporary folder: it is ephemeral. Tests keep scratch files in the build tree (`FDTD_TEST_TMP`).
- **Heavy CPU jobs take a slot of the CPU queue** (`scripts/with_cpu_lock.sh [-p NICE] [-l LABEL] COMMAND...`; one slot machine-wide by default, fair first-come queue, `--status` shows who holds it; `scripts/check.sh` does it for its builds and tests): sanitizer builds, the `[slow]` suite, long CPU solves and anything whose timing matters. It gates when a job starts and does not cap its threads. Other agents on this machine are expected to use it too (they are told where it is); the GPU lock below is separate. Self-test: `scripts/with_cpu_lock_selftest.sh`.
- **Every GPU run on this Mac, including debugging and benchmarks, goes through the shared lock** (`scripts/check.sh --gpu`, or `with_gpu_lock.sh nice -n 10 <cmd>`). It shares one fair queue with the pg-strom agent: keep holds short, never hold the lock while idle, never run a GPU binary directly.
- Windows GPU box (later work): other agents share the 4060 Ti. Take the lock with `win_gpu_lock.sh` (status, hold, release; it lives on the Mac, read it first) before any GPU run there. WSL there only exposes CPU `llvmpipe` Vulkan.
- Open decisions that need the user: KiCAD PCM vs direct builds (later). The license is MIT.
