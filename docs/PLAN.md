# Signalforge — Planning Document

A minimal, standalone, pedalboard-style LV2 host for guitarists. Load a chain of
LV2 plugins (including `neural_amp_modeler.lv2`), save it as a preset, and
switch presets live with a USB MIDI foot controller. No drag-and-drop graph
editor — presets are simple ordered lists of effects, with at most a single
binary split/join added later.

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
- Dead simple UI: a footswitch-like list of presets/slots, not a node editor.

## 2. Non-goals (v1)

- No drag-and-drop patch bay / free-form graph editing.
- No parallel routing beyond a single **binary** split→merge (deferred to a
  later milestone; the data model should not preclude it, but v1 ships
  sequential-only).
- No plugin GUI embedding (`suil`) in v1 — control ports are exposed as
  generic sliders/knobs in our own UI. (Revisit if a plugin is unusable
  without its custom UI.)
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

### 4.2 Binary split (future milestone, not v1)

Data model should represent a preset as a **tree of nodes**, even though v1
only ever produces linear chains, so this doesn't require a breaking format
change later:

- `SeriesNode`: ordered list of children (plugin instances or nested nodes).
- `SplitNode` (future): exactly two children, each fed the same input; a
  `MergeNode` (future) sums/crossfades them back together. v1's UI simply
  never exposes creating a `SplitNode`.

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

Channel-count mismatches between chained plugins (e.g. a mono-in/mono-out
amp sim into a stereo IR loader) need explicit handling: either enforce
mono-only chains in v1 (simplest — most amp-sim + cab-IR chains are mono
until a stereo effect at the end), or insert an implicit mono→stereo
duplication stage when a plugin only exposes stereo ports. Recommend
**starting mono-only end-to-end**, adding a single implicit "widen to
stereo" at the very end of the chain if the audio backend wants stereo out.

## 8. UI

Keep it deliberately small:

- A vertical/horizontal list of presets (big, foot-friendly targets if a
  touchscreen is ever used, but primarily this is MIDI-driven — the UI is
  for *setup*, not live footswitching).
- Selecting a preset shows its chain as a simple ordered list of plugin
  names with a bypass toggle per stage and a "remove/reorder" affordance
  (move up/down — no free-form dragging needed, just up/down buttons).
- "Add stage": a searchable list of installed LV2 plugins (from
  `lilv_world_get_all_plugins()`), filtered to audio-effect classes.
- Per-stage control panel: auto-generated sliders/toggles from the plugin's
  control ports (name, range, default from its TTL) — no custom per-plugin
  UI in v1. For a port that's `atom:Path` (our model file / IR file), show
  a file picker.
- MIDI-learn mode: click "learn" next to any control/toggle/preset button,
  then move something on the pedal.

Suggested toolkit: **Dear ImGui** (immediate-mode, trivial to iterate on,
no XML/Designer files, easy to keep "simple" per the brief) rendered via
whatever backend is least friction on Linux (SDL2 + OpenGL, or GLFW). This
avoids the complexity of Qt/GTK signal/slot boilerplate for what is
fundamentally a small number of screens.

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
