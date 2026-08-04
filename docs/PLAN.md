# Signalforge — Planning Document

A minimal, standalone, pedalboard-style LV2 host for guitarists. Load a chain of
LV2 plugins (including `neural_amp_modeler.lv2`), save it as a preset, and
switch presets live with a USB MIDI foot controller. No drag-and-drop patch
bay — a preset is a chain read top to bottom, which may divide once into two
parallel channels and merge back.

## 1. Goals

- Run on Linux (primary target), audio via JACK/PipeWire.
- Host arbitrary LV2 plugins in a **sequential chain** (mono or stereo).
- Define **presets**: an ordered list of plugin instances, each with its
  parameter values and any LV2 state (e.g. the `.nam` model path loaded via
  `atom:Path`).
- Switch between presets **glitch-free** via MIDI (Program Change or CC from
  any class-compliant USB MIDI controller), or from the UI.
- Per-preset and global MIDI learn: map CC/PC messages to preset switching,
  bypass toggles, and individual plugin parameters (for expression pedals,
  wah, etc).
- Dead simple UI: a board read top to bottom and a live screen of preset
  tiles, not a node editor.

## 2. Non-goals (v1)

Three lines here stopped being true during Milestones 9 and 10. They are
rewritten rather than deleted: what a project decided *not* to do is worth
keeping legible, and "we said no to this and then did it, for these reasons"
is more useful to a reader than a gap.

- No drag-and-drop patch bay. **Still true, and it is the drag-and-drop that
  is the point** — the board is a graph now (§4.2), but it is edited by
  buttons that name what they do, not by dragging cables. A gesture that can
  be made by accident on a stage floor is a gesture this does not have.
- ~~No parallel routing beyond a single binary split→merge.~~ **The model
  carries splits at any depth** (SF-45), the engine plans and plays them
  (SF-49, SF-50, SF-51), and each has a splitter — full range, a 4th-order
  Linkwitz-Riley crossover, or one side of a stereo input into each lane
  (SF-75). What is still limited is the *editor*: the board screen draws one
  split on the trunk, and a preset with more says so and goes read-only
  rather than drawing half of it (SF-71).
- No plugin GUI embedding (`suil`) in v1 — control ports are exposed as
  generic sliders/knobs in our own UI. **Still true.** The plugin-UI modal
  (SF-79) says so where a plugin has an editor of its own, and offers the
  generic controls in its place; the well is an honest placeholder, not a
  promise.
- ~~No tuner, metronome or preset banks.~~ All three shipped: banks in SF-67,
  a tuner with control-thread pitch detection in SF-81, and a metronome
  scheduled from a sample counter on the audio thread in SF-82.
- **One piece of DSP is the board's own** rather than a plugin: the noise gate
  (SF-85), at the input, before the chain. Everything else the board does to
  the sound is an LV2 plugin the player chose, and that stays the rule -- but a
  gate has to sit *before* the first pedal to work at all, which is the one
  place the plugin model cannot reach. It is per preset, and its settings live
  in the preset file beside the chain.
- No Windows/Mac support in v1 (architecture should not actively prevent it
  later, since JACK and LV2 exist cross-platform, but it's not a target).
- No plugin discovery/marketplace — assume plugins are already installed
  system-wide (`~/.lv2`, `/usr/lib/lv2`, etc), same as any LV2 host.

## 3. Prior art / reference implementation

**`jalv`** (https://github.com/lv2/jalv) is the reference minimal LV2 host and
is the right starting point to study/borrow from, not necessarily to build a
UI on top of. Its useful patterns:

- `LilvWorld` setup + plugin discovery via `lilv`.
- Port introspection: iterating `lilv_plugin_get_num_ports`, classifying each
  port (audio/control/atom/cv, input/output) and allocating buffers
  accordingly.
- **Feature negotiation**: `urid:map`/`urid:unmap`, `bufsize:boundedBlockLength`,
  `options:options`, and critically `worker:schedule` for plugins that do
  non-realtime work off the audio thread (convolution IR loaders often need
  this).
- **State extension** (`lv2/state.h`): `lilv_state_new_from_instance` /
  `lilv_state_restore` — this is how a plugin's full state (including
  `atom:Path` file references like our `.nam` model path) gets saved and
  reloaded. This is the mechanism our preset format should lean on rather
  than reinventing per-plugin serialization.
- **Worker thread**: a dedicated non-RT thread draining a ring buffer of work
  requests from the audio thread, calling back into the plugin's
  `LV2_Worker_Interface`.
- JACK process callback structure: connect audio ports directly to JACK
  ports; atom ports get a small pre-allocated `LV2_Atom_Sequence` buffer that
  is cleared each cycle.

Our host is simpler than `jalv` in one way (fixed sequential topology, not an
arbitrary single plugin with arbitrary ports) and more complex in another
(N plugins chained, preset persistence, MIDI-driven switching, our own
minimal UI instead of relying on the host app's window per instance).

## 4. High-level architecture

```
┌─────────────────────────────────────────────────────────────┐
│                         UI (thin)                            │
│   preset list / footswitch grid, param sliders, MIDI-learn    │
└───────────────┬───────────────────────────────────────────────┘
                │ (non-RT control messages, lock-free queues)
┌───────────────▼───────────────────────────────────────────────┐
│                      Engine (control thread)                  │
│  - Preset store (load/save)                                   │
│  - MIDI input handling + learn/mapping table                  │
│  - Orchestrates preset switches (builds "next chain",         │
│    swaps atomically, tears down old chain off the RT thread)  │
└───────────────┬───────────────────────────────────────────────┘
                │ lock-free swap of an immutable "ActiveChain"
┌───────────────▼───────────────────────────────────────────────┐
│                   Audio thread (JACK/PipeWire process cb)      │
│  - Reads pointer to ActiveChain (atomic)                       │
│  - For each plugin in sequence: run(), feeding output buffer   │
│    of stage N into input buffer of stage N+1                  │
│  - No allocation, no locks, no logging                         │
└─────────────────────────────────────────────────────────────┘
                │
        ┌───────┴────────┐
        │  Worker thread  │  (per plugin needing worker:schedule)
        └────────────────┘
```

Key architectural decision: **the audio thread never mutates the plugin
graph**. Building a new chain (instantiating plugins, restoring state,
connecting ports) happens entirely on the control thread. Switching presets
means: build the new chain fully off-thread → atomically publish a pointer →
audio thread picks it up on the next process cycle → old chain is deactivated
and freed on the control thread once we're sure the audio thread no longer
references it (e.g. via a two-cycle grace period or an RCU-style counter).

### 4.1 Click-free preset switching

Instantiating plugins is not RT-safe and can take milliseconds — this must
never happen inside the audio thread. Approach:

1. On a preset-switch request (from MIDI or UI), the control thread
   instantiates and fully activates the new chain in the background
   (including restoring LV2 state — model files, IRs, etc — which can
   involve disk I/O).
2. Once ready, apply a short equal-power crossfade (a few ms, driven by the
   audio thread, not the control thread) between old and new chain outputs,
   or at minimum a hard mute-unmute of a couple of ms to avoid a discontinuity
   click. Start with hard mute; crossfading is a nice-to-have.
3. Publish the new chain pointer with `std::atomic<ActiveChain*>` /
   acquire-release semantics.
4. Old chain: deactivate + delete on the control thread after confirming the
   audio thread has moved on (e.g. a sequence counter bumped once per audio
   callback, control thread waits for it to advance past the swap point).

### 4.2 The split, as built

This was written as a future milestone and shipped in Milestone 9. What it
predicted is roughly what happened, with two differences worth recording.

The preset is a tree (`ChainGraph`): an ordered trunk of nodes, where a node
is either a plugin or a **split** into exactly two branches that run in
parallel and sum back. Splits nest.

- There is no separate `MergeNode`. A split *is* its merge -- the two are one
  node -- because every split has exactly one merge and a second concept would
  only be a second thing to keep in step. Per-lane return level and mute live
  on the branch (`gain_db`, `muted`); per-lane send level and how the signal
  divides live on the split's `splitter`.
- **An empty branch is legal and is not a degenerate case.** It is a straight
  wire, which is how a dry parallel path is spelled -- and a wet/dry blend is
  most of why anyone reaches for a split. Requiring a placeholder plugin would
  have been a worse model, not a stricter one.

The engine does not walk the tree. It is compiled once, on the control thread,
into a flat list of block operations over single-channel buffers
(`ChainSchedule`): no recursion, no allocation, and no branching on the audio
thread beyond a switch on a small enum. Lanes of unequal latency are aligned
with a delay before they sum, because summing them as they arrive comb-filters
-- which is the single most likely way for a split to sound wrong.

## 5. Data model

### 5.1 Preset file format

Plain JSON (or TOML — pick one; JSON is fine, simple, human-diffable in git).
One file per preset, plus a manifest/index listing them in switch order.

```jsonc
{
  "name": "Crunch Rhythm",
  "chain": [
    {
      "uri": "http://github.com/mikeoliphant/neural-amp-modeler-lv2",
      "bypassed": false,
      "controls": { "input": 0.0, "output": -3.0, "quality": 1.0 },
      // Full LV2 state blob (base64 or nested turtle) as produced by
      // lilv_state_new_from_instance — this is what carries the atom:Path
      // to the loaded .nam file.
      "state": "…lv2 state serialization…"
    },
    {
      "uri": "http://gareus.org/oss/lv2/convoLV2#Mono",
      "bypassed": false,
      "controls": { "dry-wet": 1.0 },
      "state": "…ir file path + convolution params…"
    }
  ],
  "midi": {
    "trigger": { "type": "program_change", "channel": 0, "number": 3 }
  }
}
```

**A board with a split writes `graph` instead of `chain`**, and the two are
mutually exclusive -- a file with both is refused rather than one of them
guessed at. The flat `chain` form is kept for linear boards, and a preset that
was linear before splits existed still round-trips byte-identical: ids are
omitted while they are positional (SF-46), and a `splitter` equal to its
default is omitted entirely (SF-75).

```jsonc
{
  "name": "Two amps",
  "graph": [
    { "id": 1, "uri": "…comp…", "bypassed": false, "controls": {} },
    { "id": 2,
      // Absent, this reads as full range with unity sends -- which is what
      // every split written before SF-75 meant.
      "splitter": {
        "mode": "crossover",   // "full" | "crossover" | "stereo"
        "freq": 800.0,         // crossover only
        "send_a": -6.0,        // into each lane, before its stages
        "send_b": 0.0,
        "src_a": "left",       // stereo only: "left" | "right" | "sum"
        "src_b": "right"
      },
      "branches": [
        // gain_db and muted are the *return*: how much of each lane comes
        // back at the merge. An empty "nodes" is a dry wire, not an error.
        { "gain_db": 0.0, "muted": false, "nodes": [ { "id": 3, "uri": "…amp A…" } ] },
        { "gain_db": -3.0, "muted": false, "nodes": [ { "id": 4, "uri": "…amp B…" } ] }
      ] },
    { "id": 5, "uri": "…reverb…" }
  ]
}
```

Stage *indices* -- what a MIDI binding and the console commands speak -- are
the order `ChainGraph::Flatten` reports: the trunk in order, and at a split all
of branch 0 then all of branch 1 before the trunk resumes. That ordering is the
definition of a stage index now that the chain is not a list, and the engine,
the UI and the bindings all have to mean the same one by it.

Reuse LV2's own state serialization (`lilv_state_to_string` /
`lilv_state_new_from_string`) rather than hand-rolling how each plugin's
state is captured — this is exactly the problem the LV2 state extension
solves, and it's what makes `atom:Path` (model files, IRs) round-trip
correctly.

### 5.2 MIDI mapping table

Global (not per-preset) table, persisted alongside presets:

```jsonc
[
  { "type": "program_change", "channel": 0, "number": 0, "action": "load_preset", "preset": "clean" },
  { "type": "cc", "number": 80, "action": "next_preset" },
  { "type": "cc", "channel": 0, "number": 64, "action": "toggle_bypass", "stage": 1 },
  { "type": "cc", "channel": 0, "number": 20, "action": "set_control", "stage": 0, "port": "quality",
    "min": 0.0, "max": 1.0 }
]
```

Settled in SF-21, differing from the sketch above in three ways:

- **Targets are flat fields, not a nested `"target"` object.** `preset` for
  `load_preset`; `stage` (the chain stage index — clearer than the sketch's
  `preset_slot`, which read as if it meant a preset) plus `port`/`min`/`max`
  for the others. Only the fields an action actually uses are written back
  out, so a `next_preset` binding doesn't look like it cares about a stage.
- **`"channel"` is optional and means *any* when omitted.** Foot controllers
  ship on whatever channel they ship on and users routinely don't know which,
  so the forgiving reading is the default.
- **Loading is strict.** An unknown action, a missing target, a
  `set_control` bound to a `program_change` (which carries no value), or two
  bindings claiming the same message are all rejected at load time with the
  offending index — a binding that fails silently at stomp time is much worse
  than one that refuses to load.

Resolution is stateful, which the sketch didn't anticipate: a momentary
footswitch sends CC 127 on press and 0 on release, so firing on both would
advance two presets per stomp. `MidiMap::Resolve` tracks which switch
bindings are held and fires on the press edge only. Continuous
(`set_control`) bindings are exempt — for a knob, 0 is a position, not a
release.

"MIDI learn" mode in the UI: user selects a target (preset slot number, or
"next preset"/"previous preset", or a specific control on a specific slot),
then presses/moves something on the pedal; the next MIDI event received gets
bound to that target and appended to the table.

## 6. MIDI input

- Use **JACK MIDI** or **PipeWire MIDI** ports if running under
  JACK/Pipewire-as-JACK (consistent with the audio backend already chosen —
  see prior conversation about `pw-jack`).
- Fall back to raw ALSA MIDI (`snd_rawmidi_*` or `libasound`
  `snd_seq_*`) so the tool also works headless/without a JACK graph, since
  "any MIDI input should work" implies not requiring the user to patch things
  in a JACK patchbay by hand. Practical approach: auto-connect to all
  available *hardware* MIDI input ports at startup (skip virtual/software
  ports), with a config override to pick a specific device by name.
- Parse only `Program Change`, `Control Change`, and (optionally) `Note On`
  messages for triggers — a MIDI foot controller commonly sends one of
  these three.

## 7. Plugin hosting layer (the `lilv`-based engine)

Responsibilities, modeled on `jalv`:

1. **World setup**: one `LilvWorld`, `lilv_world_load_all()` at startup.
2. **Instantiation per chain entry**:
   - Resolve `LilvPlugin*` by URI.
   - Build the LV2 feature array: `urid:map`/`unmap` (shared, single URID
     map for the whole process — important so that state blobs are portable
     across plugin instances), `worker:schedule` (per-instance worker
     ring buffer), `bufsize:boundedBlockLength`/`options:options` for a
     fixed max block size, `state:makePath`/`state:mapPath` if the plugin
     needs to resolve relative file paths.
   - `lilv_plugin_instantiate()`.
   - Classify and connect ports: audio in/out buffers wired stage-to-stage
     directly (no intermediate copy needed, connect stage N's output buffer
     pointer as stage N+1's input buffer pointer where channel counts
     match); control ports get a `float` slot per port, initialized from
     defaults or the preset's `controls` map; atom ports get a small
     pre-allocated sequence buffer.
   - `lilv_state_new_from_string()` + `lilv_state_restore()` to apply the
     preset's saved state (this is what reloads the `.nam` file path into
     our own plugin, or an IR path into the convolution plugin).
   - `lilv_instance_activate()`.
3. **Per-block processing**: for each stage in order, clear atom output
   sequences, `lilv_instance_run(block_size)`, hand its audio output buffer
   to the next stage's input.
4. **Worker draining**: after `run()`, call
   `worker_interface->end_run()` if present; the worker thread itself
   drains requests asynchronously and calls back
   `worker_interface->work()`; responses are queued back to the audio
   thread via a ring buffer and applied via `work_response()` at the start
   of the next `run()` — same pattern as `jalv`.
5. **Teardown**: `lilv_instance_deactivate()` + free, only ever from the
   control thread once the audio thread has stopped referencing the
   instance (see §4.1).

Channel-count mismatches between chained plugins (e.g. a mono-in/mono-out amp
sim into a stereo IR loader) needed explicit handling. This recommended
starting mono-only end-to-end; **what shipped adapts between neighbours
instead** (SF-48), because a board with one stereo pedal in it turned out to be
the ordinary case rather than the exotic one.

Two adapters, and only two:

- Mono into a stereo pedal duplicates.
- Stereo into a mono pedal folds down at −6 dB per side, so a signal that goes
  mono → stereo → mono comes back at the level it went in.

Anything wider than stereo is **refused rather than guessed at**: there is no
one right answer for what four channels into two should do, and silently
picking one is worse than saying so. The board's own input width is a setting
(`board_inputs`: auto, 1, or 2 — SF-52, SF-76), not a consequence of whichever
pedal happens to be first.

## 8. UI

Dear ImGui on SDL2 + OpenGL, as suggested here originally and as built. What
this section described as one screen is now **two screens and six modals**,
laid out against a design system whose tokens live in
`design/design_handoff_pedalboard_ui/` -- that bundle is the authority for
every colour, size and spacing figure, and `src/ui/Theme.h`, `Fonts.h` and
`Widgets.h` are its transcription. Where this document and the bundle
disagree, the bundle wins.

The window is three horizontal bands -- a header, a body, a status bar -- with
a learn strip and a missing-plugin banner appearing between the first two when
they have something to say. The header carries the Board | Live switch and the
buttons that open the modals.

**Board** (SF-69 through SF-73) is a 330px sidebar beside the chain area:

- *Sidebar*: bank, the preset list, an engine readout (rate, block size,
  latency, MIDI bindings, controller) with a DSP gauge, and both meters with
  the two board-wide trims.
- *Chain area*: one card per stage, each expandable in place -- controls, file
  parameters and learn buttons -- separated by `INPUT` / `SPLIT` / `MERGE` /
  `OUTPUT` rails. A split draws as CHANNEL A and CHANNEL B side by side with a
  splitter card above and a mixer card below.
- A shape this editor cannot draw -- two splits on the trunk, or a split
  inside a lane -- **says so and goes read-only** rather than drawing the part
  it understands. Half a board shown silently would be worse than a board that
  admits it needs the file.

**Live** (SF-77) replaces the body: the preset name at 84px, what the next
stomp lands on, a grid of preset tiles carrying their bindings, and two
64px stomps. It is the screen read from two metres away with a guitar on,
which is the whole argument for the type sizes.

**Modals**: the plugin browser and file picker, MIDI mappings (SF-78), plugin
UI (SF-79), audio settings (SF-80), tuner (SF-81) and metronome (SF-82).

Everything on both screens is driven by a snapshot in and requests out --
`BoardViewState`. The drawing code touches no engine object, which is what
lets the whole UI be exercised by a self-test with no plugins installed and no
audio server running (`--test-ui`).

## 9. Threading & real-time safety summary

| Thread | Responsibilities | Constraints |
|---|---|---|
| Audio (JACK/PipeWire callback) | Run plugin chain, read `ActiveChain*` atomically | No allocation, no locks, no syscalls, no logging |
| Worker | Drain per-plugin work queues, call `work()` | Can block/allocate, one thread (or one per plugin if needed) |
| Control/UI | MIDI parsing, preset load/instantiate/state-restore, UI rendering, old-chain teardown | Not RT, can do I/O |

Communication control→audio: an atomic pointer swap for the whole chain
(published only after the new chain is fully built and activated).
Communication audio→control: lock-free SPSC ring buffers for "worker
response ready" and any UI meters (level, gain-reduction, etc — v1 doesn't
need this, but leave the seam).

## 10. Suggested tech stack

- **Language**: C++17 (matches the LV2/NeuralAudio codebase conventions
  already in this repo).
- **Build**: CMake (consistent with `neural-amp-modeler-lv2`).
- **LV2 hosting**: `lilv` (+ `lv2` headers) — same dependency family already
  vendored in `deps/lv2` for the plugin side; the host needs `lilv` itself
  (typically system package `liblilv-dev` / `lilv-0`).
- **Audio/MIDI I/O**: JACK client API (works whether the real `jackd` or
  PipeWire's JACK shim is the server — consistent with how you're already
  running Ardour).
- **JSON**: `nlohmann/json` (single header, easy vendoring) for presets and
  MIDI mapping files.
- **UI**: Dear ImGui + SDL2 backend.
- **Testing**: a headless "engine" test mode that loads a preset against a
  null/file audio backend, useful for CI (no real audio device needed) —
  process N blocks of a test signal through the chain and assert no NaNs,
  no dropped samples, correct output port count.

## 11. Milestones

1. **Engine core**: `lilv`-based single-plugin loader, JACK client, process
   a mono signal through one plugin (prove the RT loop, port connection,
   and our own NAM plugin's `atom:Path` state works end-to-end).
2. **Chain**: generalize to N plugins in sequence; implicit port-count
   adaptation; worker-thread support for plugins that need it (test with
   the IR convolution plugin).
3. **Presets**: JSON schema + save/load using `lilv_state_*`; a "current
   preset" can be saved/reloaded from disk, restoring exact plugin state
   including model/IR file paths.
4. **Click-free switching**: build-next-chain-on-control-thread + atomic
   swap + mute/crossfade; validate with rapid preset switching under load.
5. **MIDI**: hardware MIDI auto-connect, PC/CC parsing, mapping table,
   MIDI-learn UX; wire "next/prev preset" and "load preset N" actions.
6. **UI**: preset list, per-stage control panel, add/remove/reorder stages,
   file picker for `atom:Path` ports, MIDI-learn affordance.
7. **Polish**: crossfade quality, persistence of window/layout, error
   handling for missing plugins referenced by a preset (graceful skip +
   warning rather than crash).
8. **(Later) Binary split/merge**: extend the tree data model, add exactly
   one `SplitNode`/`MergeNode` pair to the UI, keep everything else
   unchanged.

## 12. Open questions to resolve before/while building

- Mono-only v1, or does the target rig need true stereo (e.g. stereo
  chorus/delay at the end of the chain)? Affects §7's channel-adaptation
  decision.
- ~~Where do preset files live — a single directory scanned at startup
  (simplest, git-friendly if you want to version your own presets)?~~
  **Resolved (SF-15): both.** A directory is scanned for `*.json` and
  sorted by filename, so dropping files in a folder just works. If a
  `manifest.json` (`{ "presets": [...] }`) is present it takes over,
  defining switch order *and* membership — order matters once a footswitch
  is stepping through presets, and alphabetical ordering would otherwise
  force `01-`/`02-` filename prefixes. A preset resolves by index, by its
  `name` field, by filename stem, or by path.
- Should MIDI mappings be global-only, or can a preset override the
  "next/prev" behavior locally? (Recommend global-only for v1 — simpler
  mental model for the player.)
- Crossfade duration and curve — start with a fixed ~5–10ms linear fade,
  tune later by ear.
  *Settled in SF-33:* 12ms equal-power by default, and both are settings
  (`crossfade.milliseconds` / `crossfade.curve` in the settings file, or
  `--crossfade-ms` / `--crossfade-curve`). Equal-power rather than linear
  because two presets are two different sounds, which add in power, and a
  linear fade drops 3dB in the middle of every switch. `--tune-crossfade`
  prints the measurements the default was chosen from.
