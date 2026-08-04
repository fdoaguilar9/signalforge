# Handoff: Signalforge pedalboard UI

## Overview
A clickable prototype of the Signalforge desktop window: a pedalboard-style host for LV2
plugins. Covers the board screen (banks, presets, MIDI learn, engine stats, levels, and a
sequential chain with an optional binary split into channels A/B), a live performance
screen, and four modals (plugin UI, add-stage browser, file picker, audio settings) plus
a MIDI mapping table, tuner and metronome.

Built against `fdoaguilar9/signalforge@main` — `src/ui/BoardView.cpp` and
`src/ui/AudioSettingsView.cpp` are the functional ground truth; `docs/PLAN.md` supplied
the vocabulary (stage, chain, bank, crossfade, learn) and the split/merge model from §4.2.

## About the design files
The files in this bundle are **design references created in HTML** — prototypes showing
intended look and behavior, not production code to copy. The real app is C++/Dear ImGui
over SDL2, so the task is to **recreate these layouts with ImGui calls** in
`src/ui/BoardView.cpp` and friends, following the patterns already in that file
(`ImGui::CollapsingHeader`, `ImGui::BeginChild`, `RecordWidget` probes for the UI self
test, `ImGui::BeginPopupModal` for every modal). Nothing here should be ported as HTML.

Two colour caveats when translating to ImGui:
- Meter and warning colours in the prototype come straight back from `DrawLevelMeter` —
  reuse the existing `ImVec4` literals rather than converting the hex below.
- The amber accent (`#e0a458`) is new; it does not exist in the current ImGui theme and
  needs adding to the style setup in `AppWindow.cpp`.

## Fidelity
**High fidelity.** Final colours, type, spacing and interaction states. Layout proportions
and control grouping are intentional; the exact pixel metrics below are the target.
Animation is deliberately near-zero (see Interactions).

## Screens / Views

### 1. Board (default)
Full-window, three bands: header bar, body (sidebar + chain), status bar.

**Header (52px tall, `--bg-surface`, 1px bottom hairline, 16px horizontal padding, 16px gap)**
- Wordmark "Signalforge" — 16px, weight 600, letter-spacing 0.04em.
- 1px × 20px vertical divider.
- A breadcrumb: bank name, a `›` separator in `--text-disabled`, then the preset name
  (13px; bank in `--text-secondary`, preset in `--text-primary`, 8px gaps). Preset name
  has the preset file path as its tooltip.
- Spacer, then a segmented Board / Live tab pair (6px 16px padding, 13px, 3px outer radii,
  shared border; active tab = `--bg-surface-raised` fill, `--accent-primary` text,
  `--border-strong`).
- Ghost buttons, in order: MIDI map, Tuner, Metronome, Audio settings.

**MIDI-learn strip** (only while armed) — `--bg-surface`, 10px 16px, amber
`--status-warning` text: `Press a switch or move a pedal to bind <target>.` plus a ghost
`cancel` button. In the real app this is `state.learning` / `state.learning_what`, already
present in `BoardViewState`.

**Warning banner** (only when the board disagrees with its preset) — the StatusBanner
warning tone, 8px top padding, copy taken verbatim from the source's missing-plugin case.

**Sidebar (330px fixed, own vertical scroll, 16px padding, 1px right hairline, 20px between groups)**
Every group is headed by an 11px mono label in `--text-disabled`, letter-spacing 0.08em.
1. **BANK** — header row carries the label, a right-aligned `next bank` caption and a
   learn button; the bank `<select>` sits full width on the line below (the select has a
   260px min-width, so it must not share a row with anything).
2. **PRESETS** — label plus an `N of N` count; a `Step:` row with learn buttons for next
   and prev; then the preset list in an inset well (`--bg-inset`, hairline, 5px radius,
   6px padding, 4px between rows). Each row: name + `<stem>.json` sublabel, selected row
   highlighted, learn button to its right. Selection follows what is *playing*, not what
   was clicked (see the `requested_preset` comment in `BoardView.h`).
3. **ENGINE** — two-column key/value table: Sample rate, Block size, Latency
   (`5.3 ms round trip`), MIDI bindings (count derived from the live mapping table),
   Controller. Below it the **DSP gauge** (see Components).
4. **LEVELS** — Input and Output meters, a shared dB scale row
   (`-60 -30 -18 -12 -6 0`), then global **Input gain** and **Output gain** sliders
   (−24…+24 dB, each with a learn button). Clipping adds the red line
   `Output is clipping -- lower the level in the chain or at the interface.`

**Chain area (fills remaining width, own vertical scroll, 24px padding, 14px between blocks)**
Rail separators mark the signal path: a centred 11px mono label between two 1px hairlines.
Order top to bottom:
- **SIGNAL PATH bar** — a card (`--bg-surface`, hairline, 5px radius, 10px 12px padding,
  12px gap) sitting above the INPUT rail: the mono label `SIGNAL PATH`, a segmented
  `1 channel` / `2 channels` control styled exactly like the header's Board|Live pair
  (same `tabBase` + active treatment, `nowrap` and `flex: none` so both stay one line at
  31px), then a caption taking the remaining width —
  `One series chain from input to output.` or
  `Two parallel channels, each with its own amp, merged back through the mixer.`
  This is the only control for splitting and un-splitting; `2 channels` splits,
  `1 channel` collapses A → B → post-merge stages back into a single series chain and
  reports `Collapsed to one channel -- every stage now runs in series.`
  Named "signal path", not "amp section" — the split is not amp-specific.
- `INPUT` rail (label in `--text-disabled`).
- Pre-split stages (StageCard each), then an `Add stage` ghost button.
- `SPLIT` rail (label in `--status-warning`).
- **Splitter card** — 560px max, centred. Collapsible: header row is a click target with a
  ▾/▸ arrow, the mono label `SPLITTER`, and a summary that reads
  `how the signal divides into A and B` when open, or the current setting when collapsed
  (`crossover at 800 Hz` / `full range into both channels` / `Left input / Right input`).
  Open contents: a Mode select with three options —
  `Full range (copy)` | `Frequency crossover` | `Stereo input (L / R)`:
  - **Full range** — `Send A` and `Send B` sliders (−60…+6 dB) under the caption
    `Level of the signal sent into each channel.`
  - **Frequency crossover** — a logarithmic `Crossover` slider (20 Hz…20 kHz) captioned
    `Below the crossover feeds channel A, above it feeds channel B.`
  - **Stereo input** — `Channel A source` and `Channel B source` selects, each
    `Left input` | `Right input` | `Sum (L+R)`, defaulting A=left, B=right. Caption reads
    `Each channel takes one side of the stereo input.`, or — when the engine is running one
    input port — `The interface is providing one input channel -- both channels will receive
    the same signal until a stereo input is selected in Audio settings.`
  Every slider has a learn button. All selects in this card stack **full width**; the design
  system's Select has a 260px min-width and overflows the 560px card if paired side by side.
- **Channel lanes** — a 2-column grid, 16px gap, each lane an inset panel (`--bg-inset`,
  hairline, 5px radius, 12px padding, 12px gap) headed `CHANNEL A` / `CHANNEL B`, holding
  its own StageCards and `Add stage`.
- `MERGE` rail (amber label).
- **Mixer card** — same 560px centred card and same collapse behaviour as the splitter;
  header summary collapses to `A 0.0 dB / B 0.0 dB`. Open contents: `Level A` and
  `Level B` sliders (−60…+6 dB) with learn buttons, under the caption
  `channel levels into the merge`.
- Post-merge stages and their `Add stage`.
- `OUTPUT` rail.

**Status bar (`--bg-surface`, 1px top hairline, 8px 16px)** — primary `Save preset` button
(disabled until there are unsaved changes), the caption `unsaved changes` when dirty, and
a right-aligned status line (`Loaded preset 'Rhythm' from presets/02-rhythm.json`).

### 2. Live
Same header and status bar; body is centred with 48px 32px padding and 28px gaps.
- Bank name, uppercase, 13px mono, `--text-secondary`.
- Preset name at 84px / weight 600 / line-height 1.
- `Next: <preset>` in 13px mono `--text-disabled`.
- A 3-column grid (max 880px, 16px gap) of preset tiles, 104px tall: inactive tiles are
  `--bg-surface` with a hairline and secondary text; the active tile is
  `--bg-surface-raised` with a 2px `--accent-primary` border, amber name and panel shadow.
  Each tile shows the preset name (20px) over its binding (11px mono) or `unbound`.
- `Prev` / `Next` buttons, 64px tall, full width, split evenly.
- Input/Output meters (20px tall variant) with the dB scale row.

### 3. Modals
All share the design system's Modal: plain `rgba(0,0,0,0.6)` scrim, **no blur**, 8px radius.

- **Plugin UI** (`<Plugin> -- plugin UI`, 640px) — a 200px inset well standing in for the
  plugin's own editor (`The plugin's own editor (suil embed) renders here.` + the plugin
  URI in mono), then a `GENERIC CONTROLS` label over the same path/control rows the stage
  card shows. In the app this is where `suil` would host the plugin's native UI; PLAN.md
  lists embedding as a v1 non-goal, so the generic controls must stay as the fallback.
- **Add stage** (560px) — autofocused search input, a 280px scrolling list of
  `name` + `uri -- N in / N out`, a `M of N` count with
  `(3 more installed have no audio path)`, and Cancel. Empty result reads
  `Nothing matches that.` Search matches name *and* URI, case-insensitively.
- **Choose a file** (560px) — current directory path in mono, a 260px list starting with
  `../`, directories suffixed `/`, filtered by the property's accepted extension, a
  `Showing: .nam` caption, and Cancel. Empty reads
  `Nothing here this plugin can load.`
- **Audio Settings** (520px) — Device select (with an `unavailable` StatusBanner when the
  saved device is absent), then `Input channels` (`Mono (1 in)` | `Stereo (2 in)`),
  Sample rate and Buffer size — all stacked full width, one per row, for the same
  min-width reason — a mono latency readout, then Cancel / Apply.
  Input channels drives the splitter's stereo mode: with one input port the stereo mode
  still selects, but the splitter shows the mono caveat above.
- **MIDI mappings** (680px) — a 4-column table (110px / 130px / 1fr / 90px) with a sticky
  header `MESSAGE ACTION TARGET RANGE`, one row per binding, message in amber mono, action
  in secondary mono, range in disabled mono (`--` when the action takes no value). Footer
  shows `N bindings`. Rows are derived, not stored separately: presets, next/prev, bank,
  every stage bypass and control port, mixer, splitter, global gains and metronome.
- **Tuner** (440px) — `OUTPUT IS MUTED WHILE THE TUNER IS OPEN` caption, note at 72px, the
  frequency in mono, a 40px needle track with centre and ±25-cent tick marks, a
  `-50 -25 0 +25 +50` scale, and a cents readout. Green (`--meter-comfortable`) within
  ±5 cents, otherwise `--status-warning`.
- **Metronome** (460px) — beat indicator (one 14px dot per beat, active dot amber) beside
  the BPM readout; Tempo slider (40–240 BPM) with learn; Time signature select
  (4/4, 3/4, 6/8, 2/4); Level slider (0–100%); Start/Stop primary button, Tap tempo
  secondary button, and a learn button for start/stop.

## Components

### StageCard
`--bg-surface`, 1px `--border-hairline`, 5px radius, `--shadow-panel`, 12px 16px padding.
Header row (8px gap): expand arrow (▸/▾, 14px hit area, `--text-secondary`) · bypass
checkbox (accent amber; **checked means active**, unchecked means bypassed) · plugin name
(flex, `--text-primary`, or `--text-disabled` when bypassed, plugin URI as tooltip) ·
**stage code** right-aligned in 12px mono `--text-secondary` (`0`, `1`, `A0`, `B1`…) ·
three 24px square buttons `^` `v` `x` (`--bg-surface-raised`, hairline, 3px radius, mono).

Note: the code sits at the **right**, next to the buttons — not as a `0. Name` prefix.
`^` is disabled on the first stage, `v` on the last, `x` when the chain would be left
empty (tooltip: `A chain needs at least one stage`).

Expanded body: 12px top margin, 24px left indent, 12px gaps —
1. `Bypass` label + learn button, and (when the plugin has its own editor) a right-aligned
   secondary `Open plugin UI` button.
2. One row per `atom:Path` property: label (52px min) + a secondary button showing the
   file's basename or `(none) -- choose...`, full path in the tooltip.
3. One row per control port: a slider (or a toggle for `toggled` ports) plus a learn
   button. Non-writable ports render as a disabled label + value. A port with no controls
   reads `This plugin has no controls.`

### Meters (DAW style)
14px tall on the board, 20px in Live. Inset well (`--bg-inset`, hairline, 3px radius,
`--shadow-inset`, 2–3px padding), 28 equal segments with a 2px gap, 1px segment radius.
Segment colour by its own dB position: below −12 `--meter-comfortable`, −12…−6
`--meter-warn`, above −6 `--meter-hot`; when clipping, everything above −3 is
`--meter-clip`. A 2px `--gray-200` peak-hold marker sits at the peak position and falls
1.2 dB per 450 ms frame. Readout right-aligned in mono (`-9.2 dB`, or `--` for silence),
red while clipping. Scale row underneath: `-60 -30 -18 -12 -6 0`.

### DSP gauge
Same segmented well, labelled `DSP`, percent readout. Green below 60%, `--meter-warn`
60–80%, `--meter-hot` above. Tooltip:
`Share of each audio block's time budget spent running the chain`.

### Learn button
Small bordered button labelled `learn`; when bound it shows the binding text beside it in
disabled mono. **Disabled while any learn is armed** — `MidiLearn` binds one thing at a
time. Tooltip: `Bind a MIDI message to this` or `Rebind -- currently <binding>`.

## Interactions & behavior
- **Animation: essentially none.** Instant state swaps; if anything crossfades use 12ms
  equal-power, the product's own `crossfade.milliseconds` default. No easing, no transforms
  on press — a footswitch changes colour, it does not shrink.
- **Hover**: background steps one level lighter (`--bg-surface` → `--bg-surface-raised`);
  text colour unchanged except links. **Press**: one step lighter again, or
  `--accent-primary-active` for the primary button. **Focus**: 2px amber ring with a 2px
  chassis-colour gap — keyboard reachability matters here.
- Preset click loads it and clears the dirty flag; bank change loads that bank's first
  preset.
- Any stage edit (bypass, reorder, remove, control change, file choice, add) sets
  `unsaved` and writes a plain status line.
- Learn: press → strip appears → next MIDI message binds → status confirms. Cancel unbinds
  nothing. In the app, momentary switches must fire on the press edge only (`MidiMap::Resolve`).
- Splitter/mixer collapse state is UI-only and does not dirty the preset.
- Both scroll areas scroll independently; the split grid may scroll horizontally at narrow
  widths — acceptable for a power tool with long chains.
- Modals close on Escape and on Cancel; the file picker opens in the directory of the
  currently loaded file.

## State
Per chain: `split`, `pre[]`, `a[]`, `b[]`, `post[]`,
`splitCfg {mode, freq, sendA, sendB, srcA, srcB}`,
`mix {a, b}` — plus a binding string per bindable target. Per stage: `uri`, `name`,
`bypassed`, `expanded`, `paths[]`, `controls[]`. App-level: current bank and preset,
`learning`, active modal, `unsaved`, status line, meter/DSP readings, global input/output
gain, `inputPorts` (1 or 2), tuner cents, metronome (bpm, signature, level, running, beat).

The prototype's tree shape mirrors PLAN.md §4.2 — `SeriesNode` with an optional
`SplitNode`/`MergeNode` pair — so the preset JSON should gain nested nodes rather than a
new top-level format. The splitter and mixer both need persisting; neither exists in the
current schema. Stereo mode also needs the engine to open two input ports and route each
side into its channel — today `AudioEngine` assumes a single mono input.

## Design tokens
Chassis: `--gray-950 #0e0f11` (inset wells), `--gray-900 #16171a` (app), `--gray-850 #1c1d20`
(surface), `--gray-800 #222327` (raised), `--gray-700 #2c2d32` (hairline),
`--gray-600 #38393f` (strong border), `--gray-500 #525259` (disabled text),
`--gray-300 #9a9aa1` (secondary text), `--gray-200 #c2c2c7` (peak marker),
`--gray-100 #e8e6e0` (primary text).
Accent: `--amber-400 #e0a458` (primary), `#eabd7f` hover, `#d68a34` active;
links `--steel-400 #8ea9c0`.
Semantic (already in `BoardView.cpp`): comfortable `#3fb359`, warn `#e6cc33`,
hot `#f28c26`, clip `#e64040`, board warning `#ffb840`, unavailable `#ff9966`.
Type: Space Grotesk for UI, IBM Plex Mono for every numeric/technical readout;
11 / 12 / 13 / 14 / 16 / 20 / 26 / 34 / 46 px. (Both are substitutions — the repo ships no
fonts and ImGui uses its bundled bitmap font.)
Spacing: 4 / 8 / 12 / 16 / 20 / 24 / 32 / 40 / 48 / 64.
Radii: 3px controls, 5px cards, 8px modals. Shadows: `--shadow-panel` outer,
`--shadow-inset` for wells. No gradients, no blur, no texture.

## Copy rules
Terse and technical, as in the source: no emoji, no exclamation points, plain nouns
("stage", "bypass", "chain"). A warning states the condition *and* the fix. Tooltips are
one clause and explain why, not what.

## Assets
None. No logo exists in the repo or the design system — the wordmark is set in type. The
prototype draws every affordance as plain glyphs (`^ v x ▸ ▾`), matching the source's ASCII
buttons; keep it that way rather than introducing an icon set.

## Files
- `Signalforge.dc.html` — the whole prototype (board, live, all modals, all logic).
- `Stage.dc.html` — one chain stage; imported by the above.
- `support.js` — runtime for the two files; open `Signalforge.dc.html` in a browser to
  click through the design.
- `_ds/signalforge-design-system-.../` — design tokens and component bundle. The token CSS
  files are the authoritative source for every value listed above.
- `github.md` — repo association and the screen → source-file map.
