# Changelog

All notable changes are listed here. The project follows [Semantic Versioning](https://semver.org): while the
major version is 0, anything may change in a minor release. Format: [Keep a Changelog](https://keepachangelog.com).

## [Unreleased]

### Added (board geometry and run checks; docs/PLAN.md, "Plugin lessons from the antenna study")
- The snapshot takes optional `cutouts` (holes in the board) and optional `from`/`to` layers on a via (blind and micro vias); the plugin exporter reads the true Edge.Cuts outline (arcs, circles, beziers, holes) instead of a bounding rectangle and writes via spans. A via narrower than the local grid cell is no longer dropped.
- Run notes now report: copper of different nets that touches on the grid, a via that lands on another net, a port whose terminals are joined by copper or that floats, metal bodies that touch in the grid, and dielectric bodies under two cells across. Component bodies can carry a loss tangent (`--body REF=dielectric:EPS:TAND`) and a label (`--body-label REF=TEXT`, shown in the notes; unlabelled bodies read "assumed, not fitted").
- Grid lines snap to the corners of modelled component bodies (shapes of 8 vertices or fewer); two snap targets closer than a cell both become grid lines.
- The plugin exporter finds a panel frame at its breakaway tabs (slot-end bridging, `outline.board_proper`) and writes the board proper next to the outline as drawn (snapshot `board_proper`). The new setting `board_outline` (PCB group; proper by default, or whole) picks which is modelled; with proper, the frame's copper, vias and parts are dropped and the run notes say so. Panels without slots, with a single slot, or whose region is under a fifth of the drawn outline are exported as drawn.
- Saved runs can be opened again without solving: each run keeps `run.json` and its field data (setting `save_fields`, on by default), the window has "Open run..." (list of the project's runs, and a button that drops the fields of older ones), `--open-run DIR` and `--prune-runs N` do the same from the command line. The setting `board_outline` picks the board proper or the whole panel.
- Saved phasor files (fp16, one scale per frequency; `src/app/run_archive.*`) as the first step of reloading a saved run.
- The window's controls and text wrap and flow instead of running off their pane, the tab strip scrolls, and the window has a 720 x 520 minimum size.
- Project skills for known-load control checks, reference-board antenna comparison and sourcing an antenna part's files.

### Changed (GPU rendering; docs/PLAN.md, "GPU rendering")
- The 3D volume ray march, the field slice and the window's UI are drawn by the GPU through Vulkan (compute shaders for the two field views, Dear ImGui's Vulkan backend for the UI). With the GPU backend the window frame drops from about 45 ms to under 10 ms; the CPU renderers stay as the reference, the fallback and the path for screenshots (`--cpu-ui` forces the CPU UI). New options `--ui-frames N` and `--cpu-ui`; `EMFDTD_VALIDATE=1` turns on the Vulkan validation layer in the window.
- MoltenVK is deprecated upstream; KosmicKrisp (in the LunarG Vulkan SDK) is the target Vulkan driver on Apple Silicon and runs the GPU tests and the benchmark as fast as MoltenVK. `vkcore::Timing` reports when a driver gives no device timestamps (KosmicKrisp), and the benchmark falls back to wall time.

### Added (credits)
- `CREDITS.md`; release archives ship it with the licence texts of ImGui, GLFW, nlohmann/json and AccessKit (`share/doc/emfdtd/licenses/`).

### Changed
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
