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
- **Mesh quality:** presets (coarse, medium, fine), each showing **estimated cells, memory and time** before the run (also an existing requirement in `docs/PLAN.md`). Boundary margin and fine-cell size sit behind Advanced.
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

## Still open
1. Whether user-saved profiles also travel with the board (today: per board in `settings.json`; per user saved profiles are next in the user configuration folder).
2. The default colour map for the volume (opacity transfer function) and whether the volume shows |E|, Ez or power flow first.
