# UX/UI design (proposal, for discussion with the user)

Status: **partly implemented.** Proposal written 2026-11-19 after research and an advisor review; since then the standalone window (`emfdtd --ui`: setup, settings, run, results and field views), the theme and colour settings, the accessibility support below and the minimal plugin flow in `plugin/action_simulate.py` exist. Sections marked "decided" or dated are built; the rest are still proposals. Two points conflict with the principles below (marked **conflict**).

## Who and what

Hobbyists and electrical engineers both use the tool. Both mostly ask one question: *does my antenna (or line) work at frequency X, and if not, why?* Both occasionally need port type, mesh control or convergence checks. So the design splits by **how often a feature is used, not by who the user is**: one default flow that suits a first run, and one "Advanced" layer behind it. There is no hobbyist mode and no engineer mode. This follows progressive disclosure as Nielsen frames it (defer rarely used features, split tasks by frequency; its failure mode is hiding daily-use features, which taxes every visit). Source: NN/g progressive disclosure (summaries read; the 2026 original not read).

Principles used, with sources read through secondary summaries unless noted:
- **Visibility of system status** and **response-time limits** (0.1 s, 1 s, 10 s; for waits over 10 s show a percent-done indicator with elapsed and remaining time, not a spinner or a title string): NN/g "3 response time limits", "Progress indicators".
- **Error prevention, recognition over recall, match with the real world**: Nielsen's ten heuristics (listed in the NN/g articles; the error-prevention text itself was not retrieved).
- **Colour-vision deficiency**: about 8 % of men and 0.5 % of women (Nature Communications, s41467-020-19160-7, as reported in search results). Avoid rainbow maps; prefer perceptually uniform, CVD-aware maps (CMasher, Crameri; cividis is the most cited CVD-optimised option). WCAG 1.4.11 asks 3:1 contrast for graphical objects, but how it applies to a continuous colormap was not settled by anything I found, so we do not claim conformance; we follow the colormap literature instead.

Prior art (RFsim, an open-source KiCAD 10 plugin) was read for what it does, and each point is assessed against the principles rather than adopted because it exists.

## Decisions and open items

### 1. Port definition
- **Default: click one pad.** The port drives that pad against the copper on the adjacent layer (the vertical port already implemented). The tool checks that reference copper exists and refuses with a named reason if not.
- **Advanced:** two pads (in-plane gap port), several ports, port type.
- *Principle:* error prevention. One pad cannot be paired wrongly, and the check happens before the run. *RFsim:* "click a pad, it becomes port 1" matches this; adopted for that reason. A dialog that offers only the port types the geometry permits also matches (errors made impossible).
- **Conflict in current code:** `action_simulate.py` exits unless exactly two pads are selected. That is a dead end. Target behaviour: nothing selected opens a picker (recognition over recall), one selected pad just works, two selected pads is the advanced in-plane port.

### 2. Model and view selection
- **Default: everything inside a region of interest, no selection step.** Selecting parts is a progressive-disclosure option. The model selection and the view selection stay independent and are stored in the sidecar file next to the board, never in the KiCAD design.
- **Mandatory preview before the run:** domain box, ports, included copper, and the assumptions in effect (stackup source, materials, margin). A 30 s to 10 min solve with a wrong port is the costliest error here, so preventing it beats reporting it afterwards (visibility, error prevention).
- *RFsim:* a preview at the top of its dialog matches this.

### 3. Defaults and settings
- **Frequency band** cannot be inferred from the design: one prominent field with a labelled default (open item: the default value).
- **Mesh quality:** presets (coarse, medium, fine), each showing **estimated cells, memory and time** before the run (also an existing requirement in `docs/PLAN.md`). Boundary margin and fine-cell size sit behind Advanced. So do the air grading next to the board (`mesh_growth_ratio`, default 1.3; 1 gives the earlier even split) and the vertical cells through the board; the Before-you-run notes name any gap between two nets near a port that is only a few cells wide, so the user sees why a coplanar feed reads off and which setting fixes it.
- **Terminology:** keep the engineering terms and pair them with plain words ("S11, how much power reflects back"); do not replace them. Every default is shown with where it came from (see "Defaults and editable attributes" in `docs/PLAN.md`).
- *RFsim:* presets and a margin setting match; putting every setting in one dialog does not match progressive disclosure (nothing is prioritised).
- **Conflict in current code:** the hard-coded `--fine 0.15` and the 2 to 3 GHz band default in `action_simulate.py` and `emfdtd` are silent. They must become visible, labelled defaults.

### 4. First thing the user sees
- **Open on a live S11 plot with the field slice beside it**, not on the 3D volume. The user's question is "does it resonate where I want"; S11 answers it, the field explains why. The volume is costly, hard to read cold and not built yet.
- **Progress:** a solve takes longer than 10 s, so show elapsed time, steps done, estimated remaining time and a stop button in the window (today only the window title changes). The S11 curve filling in as the run proceeds is itself status feedback.
- **Colour:** the blue/orange diverging map is a defensible CVD-aware choice (the confusion axis is red/green); keep it, show a colour legend with units and the scale, never rely on colour alone for sign (add contour or sign labelling in the advanced view), and offer a single-hue alternative.
- **Not yet decided:** which field component and which frequency the slice shows by default (now Ez at the S11 minimum).

### 5. Results
- **Conflict in current code:** results go to a `tempfile.mkdtemp` directory the user cannot find. Target: a named results folder next to the board (same pattern as the sidecar), the path shown in the window, "Open folder" and "Save Touchstone as..." actions. S11 is plotted in the window and the Touchstone file is on disk (both).
- Run summary: every assumption and where it came from, reproducible.
- *RFsim:* plot window plus output directory matches.

## First-run flow (target)
1. In KiCAD, click one pad (the port). 2. Click the toolbar button. 3. A preview shows the domain, port, included copper and assumptions, with the band, a mesh preset and the cells/memory/time estimate. 4. Run. 5. The solver window opens: S11 fills in live, the field slice animates, elapsed/remaining time and the results path are visible. 6. Done: Open folder, Save Touchstone, adjust and re-run.

## Decisions made by the user (2026-11-19)
- **Results** go to a folder at the base of the KiCAD project: `<project>/em-fdtd/` holds `settings.json` (this board's saved settings), `snapshot.json` (last export) and `results/run-<time>/` (one folder per run: `s11.s1p`, `settings.json`, `board.json`, `summary.txt`, `frames/`). Implemented (`src/app/paths.cpp`, `plugin/action_simulate.py`; the project folder comes from `board.get_project().path`).
- **Nothing hard-coded:** bands, channels and meshing rules live in `data/radio_profiles.json`; every setting is in one registry (`src/app/settings.cpp`). The old fixed fine cell and band are gone from the plugin and the command line.
- **Premade profiles:** Wi-Fi (2.4, 5 and 6 GHz variants), Bluetooth, Zigbee (2.4 GHz and sub-GHz), LWMesh (2.4 GHz and sub-GHz), UWB (channels 5 and 9, 5 only, 9 only) and Custom. Users can adjust values, save them (per board now, per user next) and reset one setting or everything to the defaults. Default frequencies were researched; status and sources are per variant in the data file (Wi-Fi 2.4/5/6, Bluetooth, Zigbee 2.4 and UWB verified from sources read; LWMesh and the sub-GHz bands are unverified, flagged in the file and in the run summary).
- **Three nested ranges per profile** (advisor): band of interest, reported sweep (band widened by 15 % of the frequency so a resonance that is off target is still visible) and excitation band (sweep widened by 25 % of its width, so the source spectrum has no weak edge where S11 = 2V/Vs - 1 would be noisy).
- **The preview is the first screen of the solver window.** Advanced settings are exposed incrementally: a minimum at first (profile, variant, band, mesh quality), groups that expand on demand, and a debounced search box (200 ms after the last keystroke) that finds any setting by label, help text or keyword whatever its tier. The registry and debouncer are built and tested; the screen itself is next.
- **The 3D volume view is required** (first release).

## How the field is drawn (researched 2026-11-19)
Compared with how established tools draw fields: Meep's tutorials plot Ez with a **white-at-zero diverging map** (RdBu) over the geometry in grey, **smoothly interpolated**, on a light background ([Meep tutorial](https://meep.readthedocs.io/en/latest/Python_Tutorials/Basics/)); openEMS dumps fields for ParaView and converts magnitudes to dB for patterns ([openEMS tutorial](https://openems.readthedocs.io/en/latest/octave/Tutorials/Simple_Patch_Antenna.html)); the volume-rendering literature calls the transfer function the hardest part and notes EM work using thresholded intensity projection. What we did with that:
- **2D slice:** white-centred map on a light panel (toggle "light panel"), bilinear smoothing between grid nodes, copper drawn as a dark outline, scale from the 90th percentile of the visible area (not the single port cell).
- **3D volume:** magnitude on a **dB scale** (28 dB range), opacity per voxel crossed, emissive glow with a soft tone-map (hue survives), camera that auto-fits to where the field is active.
- **The big one was a bug, not a style choice:** the images were drawn under the UI layer and the UI's own window background (about 94 % opaque) covered them, so every render looked nearly black. Panels are now transparent over the images. Lesson for later: verify a render against the pixels it should have, not only by eye.
- The old near-black diverging map stays in `viz::diverging` for dark panels.

## Theme and colours (decided 2026-11-19)
- **Appearance setting: Auto, Light or Dark.** Auto follows the operating system (macOS is implemented and exercised; the Windows registry and GNOME `gsettings` paths are written but untested). The 3D view follows the theme too (glow on a dark background, absorption on a light one); the field slice and the 3D view follow it unless the user picks their panel in Settings, View ("Field slice panel" and "3D view panel": follow theme, light or dark; a setting, saved with the others, not a per-tab checkbox).
- **Field colours are settings, with the user's defaults per theme** (`data/ui_defaults.json`, each with a source): dark theme a colour-blind-safe **sky blue** (negative) and **yellow** (positive) pair from the Okabe-Ito palette, which is specified in CMYK values and designed on the blue-yellow axis that stays distinguishable in red-green colour blindness ([Wong 2011, Nature Methods 8:441](https://doi.org/10.1038/nmeth.1618)); light theme traditional **blue** (negative) and **red** (positive). Four colour settings in View (negative and positive, per theme), each with a picker and a hex value, saved with the board's settings and resettable to the defaults. The 3D view uses the current theme's pair; every legend follows the configured colours and says only "negative" and "positive" (never a colour name, which could go stale).
- Yellow is clearly lighter than sky blue, so sign also shows in lightness; a user's own choice is not checked for colour-vision safety. The light pair (blue/red) is the traditional choice the user asked for and is less ideal for red-green colour blindness; it can be changed.

## Accessibility (system preferences respected, 2026-11-19)
- **Reduce Motion** (system, or Settings, View, "Reduce motion": follow / on / off): the animation starts paused so the window opens on a still, and the 3D auto-fit camera jumps instead of easing. The user can still press play.
- **Increase Contrast** (system, or "High contrast"): pure black or white text, secondary text and accents at 7:1 or better on their backgrounds, full-strength borders, opaque panels.
- **Reduce Transparency** (system): legends and panels drawn over images become opaque.
- Read from macOS (`com.apple.universalaccess`, checked about once a second, so a change applies while the window is open), Windows (client-area animations, high-contrast flag) and GNOME (`gsettings`); only macOS is exercised so far.
- **Text size** (system, or "Text size": follow / 100 % to 250 %): the font is rebuilt at the chosen size and every padding, control width and legend scales with it, within 75 % to 300 %. Read from macOS (the Body text style against 13 pt, `src/viz/a11y_mac.mm`), Windows (`HKCU\SOFTWARE\Microsoft\Accessibility\TextScaleFactor`) and GNOME (`text-scaling-factor`); only macOS is exercised. The font is ImGui's built-in bitmap-style font, which stays legible but coarse at 200 %. A scalable bundled font is the next step if that is not good enough.

## Screen readers (2026-11-19)
- **How:** the window is drawn by our own rasteriser, so there is nothing for the operating system to inspect. Every widget therefore records a node (role, name, value, help text, bounds, focus) in an accessibility tree (`src/viz/access.hpp`, independent of any platform) right after it is drawn. AccessKit (`src/viz/screen_reader.cpp`) hands that tree to VoiceOver; the same API serves Windows UI Automation (Narrator, NVDA) and AT-SPI (Orca), and only that one file needs a branch per platform.
- **Cost:** the tree is built only while an assistive technology is attached; otherwise nothing is recorded.
- **What is announced:** every control with its name, value and the setting's help text; tabs with the selected one; settings grouped under their headings; the run status ("Ready", progress, "Finished in 43 s. Results are in ...", failures) as a live region so changes are read out; the estimate and assumptions; a spoken summary of the S11 plot (best match and the band better than -10 dB) and a description of the field slice and the 3D view, in words that name no colour.
- **What it can do:** press (click) any control, set a setting's value (text field, choice) from the screen reader. Moving the screen reader's cursor does not move the keyboard focus (Focus requests are ignored); pressing does work.
- **Keyboard** (ImGui keyboard navigation is on; the keys are ImGui's standard ones and have not yet been tried by hand in the window, so treat this table as intended behaviour):

  | Key | Does |
  |---|---|
  | Arrow keys | move focus to the neighbouring control (Settings, tab strip, buttons); the main way to get around |
  | Space or Enter | press the focused button, toggle a checkbox, open a drop-down (arrows pick, Enter confirms) |
  | Esc | close a drop-down, leave a text field |
  | Tab, Shift+Tab | step through text fields (the search box, numeric fields); not a full "next control" key |
  | Ctrl+Tab | cycle windows or panels; with the tab strip focused it should switch tabs (least certain) |
  | Ctrl + arrows, or Ctrl+click | adjust a focused slider, or type its value |

  The slice and the volume cannot be panned or rotated from the keyboard yet. Whether every panel draws a visible focus highlight is not checked.
- **Check:** `emfdtd ... --ui-screenshot out.png --ui-a11y-dump tree.txt` writes the tree and exits 1 if a control has no name; `viz_test` covers the tree's rules. Verified on this Mac (2026-11-19) by reading the live window through the macOS accessibility API: roles, names, values and help text came back correctly, and a press request reached the UI and found its control. A first version of the press did not toggle the control because the pointer position was not held for the click; it now holds it for three frames, but that fix was not re-checked live (the display went to sleep). Not yet tried with VoiceOver itself, which needs a person to turn it on (Cmd+F5). The tree was slow to read from outside (each query waits for the next 55 ms frame at Retina size) because the CPU rasteriser runs the loop.
- **Not done:** Windows and Linux adapters (the code is a branch in `screen_reader.cpp`; the build option `FDTD_SCREEN_READER` is on only on macOS), sliders do not expose increment and decrement, the plots are described but not navigable point by point.

## PCB properties (2026-11-20)
- Settings, group **PCB**: "PCB stackup" chooses the open board's own stackup (default) or a fabricator's stackup for the same number of copper layers (OSH Park 2, 4 and 6 layers; PCBWay 2 and 4 layers; all ENIG FR4-class). Below it, per dielectric layer from the top: thickness, permittivity and loss tangent, then outer and inner copper thickness. Picking a profile fills them in; typing a value overrides it; 0 keeps the board's value; reset returns to the profile. The preview's assumptions list says which values came from the board, a profile or you, and which the fab does not publish.
- A profile for a different layer count is not applied; the notes say so. Copper thickness is recorded only (the model uses zero-thickness sheets).

## Lesson: 3D view broken on the bomu board (2026-11-20)
A board with 735 copper shapes put more than 65 535 vertices in one ImGui draw list. ImDrawIdx is 16-bit, and the backend did not set `ImGuiBackendFlags_RendererHasVtxOffset`, so indices wrapped and triangles joined unrelated points (huge grey polygons, lines to the window corner). The CPU rasteriser also scaled only the right and bottom clip edges, which let the garbage spill over the toolbar on Retina. Both are fixed and covered by `viz_test` (vertex-offset and clip scenarios, plus the triangulation); copper fills use `viz::triangulate`. The small sample boards never reached the limit, which is why it showed only on a real design: test with a real board after drawing changes.

## Panels and the 3D camera (2026-11-20)
- **Settings starts collapsed**; the button reads "Settings >>" when closed and "<< Hide settings" when open (`--ui-show-settings` opens it for screenshots and checks).
- **3D volume:** drag rotates, right-drag, middle-drag or Shift+drag pans (the picture follows the pointer), the wheel zooms, double-click refits to the field's activity. Pan, rotate and zoom from the keyboard are still missing.

## Layout rules: nothing runs off its pane (2026-11-21, after the user found controls and text overflowing with no way to reach them)
- **The window has a minimum size (720 x 520 points).** Below it the layout cannot be guaranteed, so the window refuses to shrink further; the picture is never panned to reach controls (panning broke the 2D and 3D renders).
- **Text wraps at the pane edge:** status and estimate lines, notes and assumptions (`coloured` and `wrapped`). A checkbox label wider than the pane wraps beside its box. The "Ready." or blocked-reason text drops to its own row when it does not fit beside the Run button.
- **Fixed-width inputs shrink to the room left (`fitW`)**; a footer button that does not fit beside the previous one starts a new row (`sameLineIfFits`).
- **The tab strip scrolls with arrows instead of truncating labels** (`FittingPolicyScroll`).
- Check with `emfdtd BOARD --cpu-ui --ui-size 720x520 [--ui-show-settings] [--ui-tab TAB] --ui-screenshot out.png`, and for tabs that fill with a run, add `--cpu --ui-run`. Still to check by eye: every control row of the Preview tab's collapsed sections (model selection, probes, sweeps) and the S11, slice, far-field and 3D tabs once a run exists, at 720 x 520 and 1024 x 640.

## Layers, parts, sweeps and figures (2026-11-20; items marked "user" were decided by the user, the rest are my proposals, open to change)
- **Layers (user):** the 3D tab has independent checkboxes for field, surface current, copper and isosurface; the slice tab for field, surface current (top or bottom copper), power flow and the board outline. Layers show alone or overlaid. The state is not saved between sessions.
- **Camera views (user chose the axis triad over a cube):** six labelled axis ends in the corner of the 3D view snap to the +X, -X, +Y, -Y, +Z, -Z views, an Iso button, keys 1 to 6 and 0; the move glides unless Reduce Motion is on. No clickable cube (an Autodesk patent covers that widget; see docs/PLAN.md, "3D view controls").
- **Animation control (proposal):** one phase for the slice, the volume and the surface current; a speed slider while playing and a phase slider while paused, so layers animate and freeze together.
- **Model selection (proposal, default unchanged: model everything):** a collapsed section in the Preview tab: pick nets (parts and the nets they reach follow), a treatment per two-terminal part (Auto, Open, Short, Values for a parallel R, L, C) and per component body (Default, Ignore, Dielectric, Metal); saved in the sidecar file `em-fdtd/selection.json`, never in the KiCAD files. "Show the model as the solver sees it" draws the grid and the forced copper edges.
- **S11 views (proposal):** dB plot (default), Smith chart, impedance and VSWR, pinned reference, sweep overlay; each has a spoken summary.
- **Far field (user: in scope for the first release):** a Far field tab with three polar cuts, directivity, realised gain and radiation efficiency; no 3D pattern yet.
- **Sweeps and probes (user asked for parametric-driven simulation):** collapsed sections in the Preview tab; a sweep runs the board once per value and overlays the S11 curves; probes record E at points and plot its spectrum.
- **Export:** "Export for ParaView" writes the volume (legacy VTK) and the isosurface (OBJ) into the results folder.

## Proposals awaiting the user's choice (2026-11-20, nothing built; options and sources in docs/PLAN.md, Next steps 7)
1. **Ground nets:** a role combo per net (Auto with its evidence, Signal, Ground, Left out) in the model-selection section; one click undoes a wrong guess.
2. **3D pattern:** a "pattern" layer in the 3D view (radius and colour follow the gain, translucent, same camera and triad), with a "View in 3D" button on the Far field tab; the cuts and numbers stay as the accessible form.
3. **Wide ports:** show |S11| from the strip-summed power, the per-strip reflection coefficients as a band and a Smith-chart fan, and a one-click "narrow this port"; no silent change.
4. **Refinement: BUILT 2026-11-22 (the user asked for it to be implemented).** Preview tab, collapsed header "Fine cells in narrow gaps (optional)", below "Parts and nets in the model": one row per narrow gap (at most 4) between two nets in the model within 4 mm of a port, found on the copper itself (0.01 mm raster, gaps up to 1 mm), each a checkbox with its reason (gap, nets, layer, place, the cell it would use). All off by default, so the default mesh and its estimate do not change. Switching one on shows a cell field (mm; default the gap over 6 cells; clamped between 0.01 mm and the gap) and a line with the cells across the gap and the box size; the estimate above updates after the usual 0.3 s. The choices (enabled, cell) are saved in the board's `em-fdtd/selection.json` (`refinement`, keyed by a stable id) and come back next time. Command line: `--refine-gaps CELLS` applies all of them to a headless run; a dry run lists the suggestions that are not applied.

## Still open
1. Whether user-saved profiles also travel with the board (today: per board in `settings.json`; per user saved profiles are next in the user configuration folder).
2. Which colour map is the default per theme (decided 2026-11-20: a sequential map from `data/colormaps.json`, chosen by a View setting; see docs/PLAN.md Open questions) and whether the volume shows |E|, Ez or power flow first.

## Saved runs (decided by the user 2026-11-21)
- "Open run..." is a button next to "Open results folder" on the results screen (and next to "Run simulation" on the Preview). It opens a list of the project's saved runs, newest first: time, board, best match, whether fields were kept, size on disk. Opening one shows its results without solving; "Adjust settings and run again" then starts from that run's settings.
- The list has "Remove the fields of older runs (keep the newest 3)": the S11 and the summary stay. The same is `--prune-runs N` on the command line.
- Fields are kept by default (setting "Keep field data of each run", Run group, Advanced).
- A run whose fields were removed is listed as "S11 only" and cannot be opened; the Open button is disabled for it.

## Board outline (decided by the user 2026-11-21)
- Setting "Board outline" (PCB group, Essential): proper (default) or whole. For a board drawn inside a panel, proper models only the board found between the slots and breakaway tabs; the frame, its copper and its parts are left out. The run notes say which was used. It lives in the window's settings, not in the plugin.

## Antenna tab (decided by the user 2026-11-21: a separate "Antenna" section)
A tab next to Preview, available before any run: the checks read the board as drawn. It says where the board departs from the vendor's layout, never how the antenna will perform.

### What it shows, top to bottom
1. The antenna part, the rule and its source (marked verified or unverified).
2. Findings, in the warning colour: copper of other parts or nets inside the keepout, per copper layer, with the area; vias inside the keepout; the ground against the vendor's test board when it is much smaller. A good-news line when nothing is found.
3. Ground by layer (area, reach from the board edge, width), and the stackup under the part (total dielectric thickness, and the dielectric between the part's layer and the next copper layer).
4. **Feed line (built 2026-11-22):** the copper that leaves the antenna part's signal pad: its width, the gap to ground beside it on the same layer, the ground plane under it and the dielectric between, and from those the characteristic impedance (closed form: microstrip, or grounded coplanar waveguide when ground runs close beside it) against the port's reference impedance, in the warning colour when more than 10 percent off. It does not depend on the keepout, so it is shown even when the keepout size is unknown. The text states the method's limits (zero-thickness copper; microstrip within about 2 percent of this project's solver, the coplanar form not confirmed to better than about 8 percent). **Field solve row (built 2026-11-23, requested by the user):** under the closed form, the same line solved as a 2D cross-section with the copper's thickness (the Copper thickness settings, PCB group, or the board's own value), in the warning colour when more than 10 percent off the port; "computing" shows while it runs on a background thread (well under a second for a typical line); it assumes ground beside the line on both sides at the measured gap, and says so. Nothing else on the screen moved. **Antenna part list (fixed 2026-11-23, reported by the user: on the nRF52840 IFA board the tab showed "choose the antenna part" and ANT1 was not to be seen):** the list used the export's own order, so ANT1 was the ninth of 28 entries, below the eight a drop-down shows before it scrolls, and only a part a stored rule recognises was preselected. Now antenna-looking parts (reference ANT or AE, a value saying antenna, or an IFA / PIFA / MIFA word) are listed first, the rest in natural order (C2 before C10); the preselected part is the rule's, else the only antenna-looking one, else none (two candidates: the user picks); the user's pick still wins and is still saved. No new control. **Uniformity sentences (built 2026-11-23, under the Feed line row, following the user's question about bends and stubs):** one sentence each for width changes along the line (each stretch with its closed-form impedance), other parts' pads on the feed net, and where the copper turns or branches; or "uniform over the stretch measured" when there is none. They add findings text only: no new control.
5. The keepout size settings, and a note on what is not checked yet (the matching network, nearby metal, metal outside the board such as a case or a host, and the simulated antenna).

### How the antenna part is chosen
- **Automatically:** the first footprint whose value or reference contains (ignoring case) a part number listed in a rule's `parts`. The project's own rules are checked before the shipped ones.
- **By you:** the "Antenna part" list in the tab holds every footprint of the export. Your pick is saved in the project's `em-fdtd/selection.json` and comes back next time (if that footprint is no longer on the board, the automatic choice is used).

### How to configure it
- **Settings, group Antenna (also in the tab, under "Keepout and test board"):**
  - "Antenna keepout, width" (along the board edge) and "Antenna keepout, depth" (into the board), mm, from the part's datasheet. 0 uses the rule's value.
  - "Antenna keepout placement": `rule` (default: as the rule says, else edge), `edge` or `corner` (see below).
  - "Vendor test board, length" and "width", mm: the datasheet's evaluation board, for the ground-size comparison. 0 uses the rule's value.
  These are saved with the other settings for the board.
- **Add your own parts, per project:** create `em-fdtd/antenna_rules.json` next to the board (the same folder as `settings.json`), with the same layout as the shipped `data/antenna_rules.json`. Its rules are read alongside the shipped ones: a project rule wins when a part number matches both, and a project rule with the same `id` as a shipped one replaces it. The tab marks rules read from the project. A file that cannot be read is reported in the tab and ignored.
- **Change the shipped rules:** edit `data/antenna_rules.json` (in an installed package: `share/emfdtd/data/antenna_rules.json`), for rules you want everywhere. Each entry in `rules`:

  | field | meaning |
  |---|---|
  | `id` | name shown in the tab (required) |
  | `parts` | list of part numbers matched in a footprint's value or reference; empty for a generic entry that no footprint matches |
  | `keepout_along_mm`, `keepout_depth_mm` | the copper-free keepout from the datasheet; omit when unknown (then the two settings above are needed) |
  | `placement` | `"edge"` (default) or `"corner"`, for parts the datasheet says to mount in a corner |
  | `test_board_mm` | `[length, width]` of the vendor's test board; enables the ground-size comparison |
  | `layers` | the layers the rule applies to (text, informational) |
  | `source` | where the values were read: datasheet name, revision and date, or the vendor note. Never enter a value from memory |
  | `status` | `"verified"` (read from the datasheet) or `"unverified"` |

  Part variants often differ in their clearances (a letter after the part number): give each variant its own entry.
- **Where the keepout box sits:**
  - `edge`: centred on the part's pads, starting at the nearest edge of the board outline and reaching inward by the depth. This is how most chip-antenna datasheets draw it.
  - `corner`: starting at the outline's corner nearest the part, `width` mm along the nearer edge towards the part and `depth` mm inward. On a rounded corner the "corner" is the nearest point of the outline's polygon, so check the result against the datasheet drawing.
  The tab states which placement it used.
- **Panel boards:** the checks use the board proper when the export found a panel (setting "Board outline"), so the frame's edge is not mistaken for the board's.
- **Ground nets** are guessed (nets named GND, VSS and similar, or one net covering a quarter of the board's rectangle). They cannot be overridden in the tab yet.

### Limits to keep in mind
Metal outside the PCB (a case, a connector shell, the host the board plugs into) is not in the board file, so it is not in these numbers, and it can matter more than anything on the board. The tab's ground figures describe the PCB only.

## Farm screen (decided by the user 2026-11-25: "I approve the UX"; built the same day: items 1 to 5; the details below are mine, from the GPU farm plan)

The farm (`docs/farm.md`, `docs/PLAN.md` "GPU farm") is off by default and is configured and watched in a **Farm** tab of the
emfdtd window, not in KiCAD. State is machine-wide (the user's config folder), never in the board's sidecar file, so a copied project cannot switch networking on.

Top to bottom:
1. **Roles.** One switch per role (coordinator, worker, client), each saying in a sentence what it allows. Switching on is a two-step action: the switch asks for a confirmation that repeats the consequence ("This machine will keep a private certificate authority and listen on one LAN port..."); only the confirmation calls `emfdtd_farm.py enable ROLE --yes`. Off is one step. A machine whose Python has no TLS 1.3 gets the tool's own explanation shown under the switches ("cannot enable: this Python has no TLS 1.3...") and the role stays off.
2. **Devices.** A table of the farm's GPUs: worker, GPU name, state (idle, busy, offline), measured speed.
3. **My jobs.** A table: label, state, worker, attempt, a progress bar with the phase text under it (fetching the input, checking the GPU, starting the solver, then the solver's own estimate "about N %"), and a Cancel button for running or queued jobs. A failed job shows its reason in words. Progress is an estimate and says so ("about").
4. **Add a device** (coordinator role only). Creates an invitation and shows its one-time code, this machine's fingerprint and a countdown; the code and fingerprint can be read out or copied. One invitation at a time.
5. **Members** (coordinator role only). The enrolled devices with what each may do and a Revoke button (with a confirmation).
6. **Where a run goes** (on the run preview, last; built, see "Farm screen, second step" below; items 1 to 6 are checked by `scripts/ui_farm_check.sh`). "This GPU" is the default and unchanged; a chosen farm device or "any farm device" is offered only when the client role is on.

How it talks to the farm: the window never touches the network. A background thread runs `emfdtd_farm.py status --json` and `queue --json` about every 1.5 s and the UI reads the latest result; actions (enable, cancel, invite, revoke) run the same tool once. The tool's TLS 1.3 requirement is the farm's, not the window's: the window shows what the tool says. A local control socket is deferred.

Rules that apply as everywhere: every widget through the helpers in `apps/emfdtd/ui.cpp` so it has a name in the accessibility tree; sizes through `px()`; nothing runs off its pane at 720x520; the progress bar has a text value for screen readers; no colour carries meaning alone (states are words).

### Farm screen, second step (built 2026-11-25)
- **Run on.** When the client role is on, the Preview screen's top bar gains a "Run on" choice beside the Run button: "This GPU" (the default, unchanged) or "The farm (any available device)". The choice is kept in the farm folder (`window.json`), never in the board's sidecar file, and the Run button then says "Run on the farm". The window reads the client role from `farm.json` (no tool run). A farm run sends the snapshot the window already holds plus the settings that differ from the defaults, the refine boxes, the part and body treatments and the picked nets (`emfdtd_farm.py submit ... --json`), says "Sent to the farm as job abcd1234" and switches to the Farm tab. A hand-picked component list cannot go (the farm job has no field for it): the window says so and keeps the run local. Not built: choosing one particular farm device (the coordinator has no way to pin a job to a device yet).
- **Jobs.** A running job shows how long it has run ("running on win-egpu for 6 min"). The list shows the newest 20; older finished jobs collapse into one line. A done job says where it was solved ("Solved on win-egpu (NVIDIA GeForce RTX 4060 Ti)") and has **Fetch and open**: the tool fetches into `<project>/em-fdtd/results/run-farm-<first 8 characters of the id>`, and the window shows the S11 in the S11 tab with "Fetched from the farm." (a farm result holds the S11, the summary, the settings and the log; the field data stays on the worker, so only the S11 views have data). Afterwards the job row offers **Show S11** and **Open folder**.

### This computer's GPU (decided by the user 2026-11-25: "the coordinator machine can offer its own GPU with one explicit click; off until clicked")
- Where: Farm tab, section "This computer's GPU", between Roles and Devices. Shown only while the coordinator role is on (it is the machine that keeps the farm).
- Off (the default): a note says what it does (this computer's GPU runs farm jobs like any other device, it shares the GPU's turn lock with other programs, and it can be turned off here) and one button, "Also use this computer's GPU for the farm". If this machine cannot offer a GPU, the reason is shown instead of the button.
- One click asks first, as the role switches do: a warning sentence says it enrolls this computer as its own worker and starts a background program that runs the solver on its GPU, that every job waits for the GPU's turn and stops at the time ceiling, and that nothing opens a port. "Use this GPU" confirms, "Keep it off" cancels. Setting up can take minutes (it may wait for the GPU's turn), so the window shows "Setting up..." and stays usable.
- On: one line with the device and its state (running and connected, connecting, or not running) and "Stop using this computer's GPU" (no second step: stopping is always safe).
- Not automatic on purpose: the farm and each role stay off until the user turns them on, and a worker spends the GPU's time and heat on other people's jobs.
- The window runs `emfdtd_farm.py local-worker status|enable|disable --json` and parses the answer (`farm::parseLocalStatus`); `scripts/ui_farm_check.sh` covers the off, confirm, on and unavailable states against the stand-in tool.
