# Changelog

All notable changes are listed here. The project follows [Semantic Versioning](https://semver.org): while the
major version is 0, anything may change in a minor release. Format: [Keep a Changelog](https://keepachangelog.com).

## [Unreleased]

### Added (Agent section)
- Farm tab: an **Agent** section, shown while the worker role is on: the agent's state (stopped, running, connected, its job and last error), Start, Stop and Restart (Stop and Restart ask first when a job is running), Start held back with the failing doctor check and its fix, and "Show checks" and "Show log". The window runs `agent status|doctor|start|stop|restart --json` and `agent logs`; "This computer's GPU" keeps the one-time setup.

### Added (machine-readable CLI)
- `emfdtd --json`: one JSON document on stdout (human text to stderr) for `--list-settings`, `--list-devices`, `--dry-run` and a headless run; new `emfdtd --run-info RUNDIR [--s11]` summarizes a saved run (grid, steps, best match, -10 dB band, S11 table) without solving. Schema 1, additive changes only; see `docs/cli-json.md`. Tests: `tests/cpu/cli_json_test.cpp`, `scripts/cli_json_check.sh` (`ctest -L cpu`).

### Changed (window font)
- The window uses the operating system's UI font (San Francisco on macOS, Segoe UI on Windows, the desktop's interface font on Linux; loaded at run time, never bundled) instead of the small built-in bitmap font, at the same text size and following the system text-size setting. Settings, View, "Font" (system or built-in) and `EMFDTD_UI_FONT` choose; the built-in font stays as the fallback for missing characters. Only macOS is exercised.

### Added (coordinator's own GPU)
- `emfdtd-farm local-worker status|enable|disable`: the coordinator machine offers its own GPU to the farm on request (never automatic): it turns the worker role on, enrolls this computer with its own certificate authority, records one entry per physical GPU, saves the agent settings (an existing `agent.json` is kept) and starts the agent. The Farm tab has the button (see below).

### Added (this computer's GPU in the Farm tab)
- Farm tab: a section "This computer's GPU" (coordinator role on) with one explicit, two-step control to offer the coordinator machine's own GPU to the farm, and a Stop button. Off until clicked. The window runs `emfdtd_farm.py local-worker status|enable|disable --json`; `scripts/ui_farm_check.sh` covers the states.

### Tests (farm placement)
- Scheduler scenarios pin down the placement order: fastest idle device first, an unmeasured device last but used when it is the only one idle, ties by worker name.

### Added (KiCad toolbar button)
- Plugin action **EM FDTD farm** (`plugin/action_farm.py`, its own icon): opens the solver window on the Farm tab, or switches an open window to it, without starting a run. The farm stays off until turned on there. A new window gets `--ui-tab Farm`; an open one reads `farm` from `request.txt`.

### Fixed
- `agent stop` on Windows ended every worker only after the 20 s timeout (a console-less process ignores `taskkill /T` without `/F`): it now writes an `agent.stop` file the worker watches. The detached start no longer sets `CREATE_NO_WINDOW`.

### Added (agent window)
- `emfdtd-farm gui`: a tkinter window (standard library only, no listening port, nothing in the background unless open) to manage the farm worker agent on a machine that has only the farm bundle: status with Start, Stop and Restart (asks first when a job would be cancelled), the doctor checks, settings (solver, GPU, GPU-turn mode, limits, start-at-login off by default), a step-by-step set-up wizard, and the log. Logic is in `farm/gui/controller.py` (headless, 43 tests); the window is walked by `tests/gui_smoke.py` (`EMFDTD_GUI_TESTS=1`).

### Added (Farm tab and a progress estimate)
- Window, Farm tab and Preview: a "Run on" choice (This GPU or the farm) when the client role is on, remembered in the farm folder (`window.json`); a farm run sends the snapshot, the settings deviations, refine boxes, part and body treatments and picked nets as a job and switches to the Farm tab; running jobs show how long they have run, the list shows the newest 20, and a done job says where it ran and has **Fetch and open** (S11 into the S11 tab, "Fetched from the farm"). `emfdtd_farm.py submit --json` and a `started` time in `queue --json` support it. `scripts/ui_farm_check.sh` covers the Run-on choice, elapsed time, the cap on the list, a click on Fetch and the fetched result.
- Window: a **Farm** tab (docs/ux.md "Farm screen"). Role switches with a two-step confirmation that says what turning a role on allows; the farm's devices; your jobs with a progress bar, the phase text and Cancel; Add a device (an invitation with its one-time code and fingerprint); Members with Revoke. The window never touches the network: a background thread runs the farm tool (`emfdtd_farm.py status|queue|members --json`) while the tab is showing, so a window that never opens the tab runs nothing. `EMFDTD_FARM_COMMAND` replaces the tool (tests), `EMFDTD_PYTHON` picks the Python; `enable` records the Python it ran under. "Where a run goes" on the run preview is not built yet. `scripts/ui_farm_check.sh` (in `check.sh`) renders the tab against a stand-in tool at 720x520 and checks that every control is named.
- Solver: `emfdtd` prints `progress: N steps, about P%, T s` every 5 s. The estimate is 60 % for the pulse being injected and 40 % for the ring-down on a log scale (`RunResult::progress`, never backwards, ends at 1); the farm turns it into the job's progress bar. `emfdtd_farm.py queue` shows it too; `members --json` is new.

### Added (GPU farm; docs/farm.md, docs/PLAN.md "GPU farm"; off by default)
- `emfdtd --device SELECTOR` (an index or UUID prefix) and `emfdtd --list-devices`; env `EMFDTD_DEVICE`. A named device that cannot be used is an error, never a silent fallback. The default device is now the newest Vulkan version (discrete GPU first, then newest API, then most memory): an Apple GPU offered through KosmicKrisp (1.4) and MoltenVK (1.2) now runs on KosmicKrisp.
- GPU speed on discrete GPUs: the simulation state (fields, tables, CPML, port and phasor buffers) is device-resident with a host staging copy (`vkcore::Residency::Device`), as it must be off unified memory; unified-memory GPUs keep the old path. A 0.3 M cell job on an RTX 4060 Ti over Thunderbolt went from 24 to 1185 Mcells/s (49 times faster) with a bit-identical S11. `EMFDTD_STAGED_BUFFERS=1` forces staging for tests.
- Windows builds: an MSVC branch in `CMakeLists.txt`, POSIX guards, `<complex>` include and a `setvbuf` crash fix (the solver builds with Visual Studio 2026 and passes the CPU tests).
- `plugin/src/emfdtd_plugin/farm/` and `plugin/tools/emfdtd_farm.py`: a job farm in the Python standard library. Mutually authenticated TLS 1.3 with a private CA, one-time-code pairing with a pinned fingerprint, per-request policy, validated jobs (the worker builds the command line), fair-share scheduling with leases and requeue, separate transfer connections, audit log, 7-day retention, cancel and timeout that kill the process tree, and a command line for coordinator, workers and clients. TLS 1.3 is required with no fallback. A worker must be given a GPU turn mechanism (a lock command that wraps every Vulkan call, a turn the coordinator holds per job from `gates.json`, or an explicit no-lock), makes no GPU call from its heartbeat, keeps its own limit ceilings that the coordinator can only lower, the farm folder is restricted to the current user (icacls on Windows), and the worker makes no dry run (it duplicated the model preparation, over two minutes on the Windows box), reports each phase of a job, and a coordinator-held GPU turn is never given back while the solver might still run: the coordinator pings, a silent worker kills its solver tree after its watchdog, and the turn is released only on confirmation or after the watchdog plus a margin. Off until the user enables a role.

### Added (mesh snapping of traced copper)
- Inside a `--refine` box, the long straight edges (at least two cells, 0.15 mm minimum) of copper shapes with more than 8 vertices (Gerber-traced or merged pours) now become grid lines, so a traced radiator's widths are on the grid.
- New setting `lateral_growth_ratio` (Mesh group, Advanced, default 1.3 as before): how fast cells grow along the board away from a refined box. 1.1 gave less scatter between cell sizes on the TI antenna board.
- `scripts/antenna/validate_lines.py` takes `--er`, `--board W H`, `--full-refine` and repeated `--set KEY=VALUE`.

### Added (copper thickness and the feed-line field solve; docs/PLAN.md, "Not-built items, status")
- New setting `copper_model` (PCB group, Advanced; `sheet` by default, so every earlier result is reproduced): `slab` makes a copper layer with a thickness (from the board, the fab profile or `copper_outer_um` / `copper_inner_um`) fill grid cells of that thickness, with the planes and the z edges inside the footprint conducting. A 35 um trace's effective permittivity then falls by about 1 percent and its resonance rises by about as much; the thin cells cost time step and memory, and a layer without a thickness stays a sheet. The run notes say which model is in use.
- The snapshot takes an optional `thickness` (m) on copper stackup entries; the plugin exporter reads it from the KiCAD stackup. The settings `copper_outer_um` and `copper_inner_um` override it (a value you type wins, then the board's, then the fab profile's) and are used by the feed-line estimate; the solver model still treats copper as a zero-thickness sheet.
- Tests: a hidden report scenario sizes what a solder mask does to a line's effective permittivity in the 2D solver (`feed_solve_test`: "Report: what a solder mask does").
- Antenna tab, Feed line: the field solve's grounded walls are 8 line-widths away (they were 4, which biased the effective permittivity about 2 percent low).
- Antenna tab, Feed line: below the closed-form estimate, a field solve of the same line with the copper's thickness (computed in the background; a coplanar line reads 6 to 9 percent lower than the closed form). On the IFA test board's feed: 58 ohm closed form, 54.4 ohm solved.
- Antenna tab, Feed line: the row says how uniform the line is. It lists stretches of different width with each one's impedance (the IFA test board's feed is 0.48 mm for 1.0 to 1.4 mm from the pad, about 91 ohm by the closed form, then 0.76 mm, 58 ohm), names other parts' pads on the feed net (their copper no longer counts as line width) and says where the copper turns or branches, with how much a short step or a stub matters.
- Antenna tab, Feed line: a line with ground copper along one side only is no longer read as plain microstrip; the report says so, with the gap, and the field solve includes that ground.

### Added (board geometry and run checks; docs/PLAN.md, "Plugin lessons from the antenna study")
- The snapshot takes optional `cutouts` (holes in the board) and optional `from`/`to` layers on a via (blind and micro vias); the plugin exporter reads the true Edge.Cuts outline (arcs, circles, beziers, holes) instead of a bounding rectangle and writes via spans. A via narrower than the local grid cell is no longer dropped.
- Run notes now report: copper of different nets that touches on the grid, a via that lands on another net, a port whose terminals are joined by copper or that floats, metal bodies that touch in the grid, and dielectric bodies under two cells across. Component bodies can carry a loss tangent (`--body REF=dielectric:EPS:TAND`) and a label (`--body-label REF=TEXT`, shown in the notes; unlabelled bodies read "assumed, not fitted").
- Grid lines snap to the corners of modelled component bodies (shapes of 8 vertices or fewer); two snap targets closer than a cell both become grid lines.
- The plugin exporter finds a panel frame at its breakaway tabs (slot-end bridging, `outline.board_proper`) and writes the board proper next to the outline as drawn (snapshot `board_proper`). The new setting `board_outline` (PCB group; proper by default, or whole) picks which is modelled; with proper, the frame's copper, vias and parts are dropped and the run notes say so. Panels without slots, with a single slot, or whose region is under a fifth of the drawn outline are exported as drawn.
- The 3D volume view keeps animating when a sequential colour map is chosen (View, 3D volume colours): the colour follows the instantaneous field and pulses at twice the wave's frequency; paused (or in a still image) it shows the peak magnitude, as before.
- Antenna tab: the chosen part is saved in the project's `selection.json`; a project can add its own rules in `em-fdtd/antenna_rules.json` (they win over the shipped ones); new settings for the keepout placement (rule, edge or corner) and the vendor test board size; rules may carry `placement`.
- Antenna tab: static checks of a chip antenna's surroundings against rules in `data/antenna_rules.json` (copper and vias in the keepout on every layer, ground size against the vendor's test board, stackup under the part); settings `antenna_keepout_along_mm` and `antenna_keepout_depth_mm` for parts without a stored rule.
- Saved runs can be opened again without solving: each run keeps `run.json` and its field data (setting `save_fields`, on by default), the window has "Open run..." (list of the project's runs, and a button that drops the fields of older ones), `--open-run DIR` and `--prune-runs N` do the same from the command line. The setting `board_outline` picks the board proper or the whole panel.
- Saved phasor files (fp16, one scale per frequency; `src/app/run_archive.*`) as the first step of reloading a saved run.
- The window's controls and text wrap and flow instead of running off their pane, the tab strip scrolls, and the window has a 720 x 520 minimum size.
- Project skills for known-load control checks, reference-board antenna comparison and sourcing an antenna part's files.

### Changed (GPU rendering; docs/PLAN.md, "GPU rendering")
- The 3D volume ray march, the field slice and the window's UI are drawn by the GPU through Vulkan (compute shaders for the two field views, Dear ImGui's Vulkan backend for the UI). With the GPU backend the window frame drops from about 45 ms to under 10 ms; the CPU renderers stay as the reference, the fallback and the path for screenshots (`--cpu-ui` forces the CPU UI). New options `--ui-frames N` and `--cpu-ui`; `EMFDTD_VALIDATE=1` turns on the Vulkan validation layer in the window.
- MoltenVK is deprecated upstream; KosmicKrisp (in the LunarG Vulkan SDK) is the target Vulkan driver on Apple Silicon and runs the GPU tests and the benchmark as fast as MoltenVK. `vkcore::Timing` reports when a driver gives no device timestamps (KosmicKrisp), and the benchmark falls back to wall time.

### Added (credits)
- `CREDITS.md`; release archives ship it with the licence texts of ImGui, GLFW, nlohmann/json and AccessKit (`share/doc/emfdtd/licenses/`).

### Added (feed checks, 2026-11-22; docs/PLAN.md, "Feed impedance row and refinement suggestions")
- Antenna tab: a Feed line section reads the copper leaving the antenna part's signal pad (width, gap to ground beside it, ground plane below, dielectric between) and gives the line's characteristic impedance (`src/board/line_impedance.*`: Hammerstad and Jensen microstrip, grounded coplanar by conformal mapping) against the port impedance. It does not need the keepout size.
- Preview tab: "Fine cells in narrow gaps (optional)" lists narrow gaps between two nets near a port with a suggested refinement box and an editable cell; each is off until switched on, the choice is saved in `em-fdtd/selection.json`. Command line: `--refine-gaps CELLS`; a dry run lists the suggestions. `board::Selection::refine` carries the boxes to every run and sweep (the window now also honours `--refine` boxes).
- Project skills `antenna-matching`, `antenna-placement`, `antenna-debug` and `vendor-reference-board`, with standard-library helpers in `scripts/antenna/` (`match.py`, `lines.py`, `validate_lines.py`, a self-test run by `scripts/check.sh`).

### Added (internal)
- `docs/methods/`: in-house write-ups for work whose papers cannot be read: `paper-claims.md` (each paywalled work, its abstract paraphrased, what could be tested), `bode-fano.md` (a self-contained proof of the Bode-Fano limit for an RC load with a numerical check), `microstrip-closed-forms.md` (the Z0 = 1/(c sqrt(C C0)) derivation and the closed forms against an independent 2D solve: microstrip within 0.28 percent in eeff over 21 cases, conductor-backed coplanar within 0.2 percent for gap <= h/2), `line-extraction-and-bends.md` (two-length line extraction, the FDTD solver's own effective permittivity, and a right-angle bend measured as a capacitance and a length shift, with the capacitance-compensated mitre at about 70 percent for a 2.0 mm line on 1.6 mm FR-4). Tools: `scripts/antenna/{bodefano,line_extract,bend_extract,openend}.py` (`openend.py`: open-end extension formulas from the Qucs documentation, `validate_lines.py --behind`) (numpy; self-tested, skipped without it); `validate_lines.py` gained `--line-len`, `--width`, `--thickness-um`, `--copper-model`, `--miter`, `--leg1`, `--leg2`, a bend case, and one snapshot file per run.
- `scripts/gerber/`: a Gerber (RS-274X) and Excellon reader and rasteriser, a raster-to-polygon tracer and a recipe-driven builder that turns a vendor's manufacturing files into a snapshot (outline, stackup, copper traced from Gerber layers, ground vias only, a controlled port); self-tested on synthetic files (`scripts/gerber/selftest.py`, run by `scripts/check.sh`, skipped without numpy).
- Project skills `rf-board-engineering` (trust order, sizing, which lever moves which number, where each skill and script lives) and `gerber-to-model` (a vendor's Gerbers into a model, with the controls and the limits to state).
- `scripts/with_cpu_lock.sh`: a CPU queue for heavy jobs (fair first-come slots, stale-owner takeover, nested calls pass through), self-tested, used by `scripts/check.sh`.
- `src/fdtd/line2d.*`: a 2D quasi-static field solve of a line's cross-section (CPU reference, Jacobi and red-black over-relaxation, Richardson-extrapolated impedance). The grid has a line on every conductor edge and layer interface and can grow away from them (`GridOptions`), so widths, gaps and copper thickness are exact whatever the cell; on the graded grid a feed line solves in about 0.07 s. It reproduces the coplanar and microstrip closed forms to 2 percent and shows what they cannot (one-sided grounds, copper thickness).
- `src/fdtd_vk/gpu_line2d.*` and `shaders/line2d_rb.comp`: the same solve on the GPU (float32 red-black over-relaxation, capacitance summed in double on the CPU); agrees with the CPU to 0.5 percent. It wins only on large uniform grids (2.2 s against 0.9 s measured), so the window uses the CPU.
- `src/board/feed_solve.*`: the measured feed line solved as a cross-section with the copper's thickness.
- `scripts/antenna/cascade.py`: sizes a short neck, step or open stub on a feed by cascading ideal line sections.

### Changed
- Antenna tab: the antenna part list puts antenna-looking parts first (reference ANT or AE, a value saying antenna or IFA) and the rest in natural order, and preselects the only such part when no stored rule recognises one; before, a board whose antenna part had no rule showed "choose the antenna part" with the part buried in export order.
- The run note for a via whose column meets another net's copper now measures the real gap: copper under the via is still "short-circuits them", but copper beside it that is apart on the board (a legal 0.15 mm clearance on a coarse grid) now says "on the board they are G mm apart; only the grid joins them" and advises refining there.
- The air above and below the board is now graded: its cells grow from the board's own cell size by the new setting `mesh_growth_ratio` (default 1.3) up to the coarse cell, so neighbouring z cells never differ by more than that ratio. Before, the first air cell could be 12 times the cell below it, which read a 50 ohm microstrip as 44 ohm (now 50.8 ohm, formula 50.0); the cost is about 18 percent more cells. Set the ratio to 1 for the earlier even split. Results from earlier versions of a planar feed or antenna differ for this reason (docs/PLAN.md, "Air meshing next to the board").
- New run note: a gap of 4 cells or fewer between two nets near a port (a coplanar feed's gap to its ground, a feed to a pour) is reported with its width in cells and millimetres, because the field in it is under-resolved and the line's impedance reads low (15 percent at 3 cells, 8 percent at 5 in the validation runs); the note says to refine there (`--refine`, or the Fine cell near ports setting).
- The voxelizer's net-touch note also fires when two nets' copper meets at one grid node, which is how a coplanar gap narrower than a cell joined a feed to its pour without any note (docs/PLAN.md, "IFA control failure: cause found").
- Known limit, not a fix: a port as wide as the copper it feeds still reads an apparent loss in |S11| (the strips see different voltages; a 3 mm port on a lossless pad gave 0.886 at 3 GHz). The run notes now flag such a port, and the far-field efficiency uses the power measured strip by strip. A narrow port is exact (a lossless pad gives 1.0000; a shorted microstrip line gives an effective permittivity within 1.5 % of the quasi-static formula).
- The Settings panel starts collapsed (the button reads "Settings >>"; open it to read "<< Hide settings"). For screenshots, `--ui-hide-settings` became `--ui-show-settings`.
- 3D volume: right-drag, middle-drag or Shift+drag pans; double-click refits the view. The camera's look-at point is now honoured by the renderer (auto-fit follows the field's activity).

### Fixed
- The swapchain reused one render-finished semaphore for all images (found by the validation layer).
- The collapsible legends over the 3D volume and the field slice (the "[-]" on Board, Positive field, Negative field) and the axis triad did not react to clicks: the invisible button that catches drags over the view took every click. It now lets the controls drawn over it have theirs (checked with a new `--ui-click X,Y` screenshot option).
- The 3D view of a board with hundreds of copper shapes drew huge grey triangles and stray lines over the window. Two causes: ImGui draw lists use 16-bit indices and our backend never enabled vertex offsets (indices wrapped past 65 535 vertices), and the CPU rasteriser's clip rectangle ignored the display scale on its left and top edges (Retina). Copper fills now use our own ear-clipping triangulation instead of ImGui's concave fill.

### Added (visual features, antenna figures, parts and sweeps; docs/PLAN.md has the measurements)
- 3D view: axis triad and keys 1 to 6 and 0 for the +X, -X, +Y, -Y, +Z, -Z and isometric views; layer toggles (field, surface current, copper, isosurface); magnitude colour maps (viridis, magma, inferno, plasma; CC0) as the setting "3D volume colours"; threshold isosurface; export of the volume for ParaView (legacy VTK) and of the isosurface (OBJ).
- Field slice: power-flow arrows, surface current on the copper, a phase slider while paused, an animation speed slider.
- S11 tab: Smith chart, impedance and VSWR, a pinned reference S11, and a sweep overlay. Far field tab: gain pattern cuts, directivity, radiation efficiency, realised gain (near-to-far-field transform, checked against a short dipole and a half-wave dipole).
- Two-terminal parts: resistors, 0 ohm links, capacitors, inductors and a parallel R, L, C (ferrite bead) are modelled across their pads (value field decides, not the reference letter; DNP parts stay open); component bodies as dielectric or metal blocks; net-driven selection (`--nets`) that follows the matching network; per-part treatment in the window and in the sidecar file.
- Parameter sweeps (`--sweep KEY=a,b,c`, a window panel) with one results folder per point, geometry overrides (`--geom NET.x=MM`, never edits the board), field probes with a spectrum tab and `probes.csv`.
- GPU phasor accumulator (Kahan-compensated) for every animated quantity; float controls of the device in the run summary.
- The model view of the preview ("Show the model as the solver sees it").
- PCB properties are settings (group PCB): choose the board's own stackup or a fab profile (OSH Park 2, 4, 6 layers; PCBWay 2, 4 layers; ENIG FR4-class) and edit each dielectric layer's thickness, permittivity and loss tangent. Values the fab does not publish are marked assumed in `data/fab_profiles.json` and in the run notes.

## [0.1.0]

First tagged version: a working prototype on macOS (Apple silicon).

### Added
- FDTD solver (CPU reference and Vulkan GPU) with CPML absorbing boundary and lumped ports; S11 (Touchstone) output.
- Board snapshot format, voxelizer with a graded grid, component selection, radio and fab profiles.
- Standalone window (`emfdtd --ui`): setup, settings, run, results, field slice and 3D volume views.
- Theme (Auto, Light, Dark) with configurable field colours; system Reduce Motion, Increase Contrast, Reduce
  Transparency and text size respected.
- Screen reader support on macOS (AccessKit) and keyboard navigation.
- KiCAD IPC plugin entry points (untested inside KiCAD).
- `emfdtd --version`; release archives built by GitHub Actions for macOS arm64.
