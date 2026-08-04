# Signalforge — Task Breakdown

Working backlog, broken into ~2h chunks. Grouped by milestone (see
[PLAN.md](PLAN.md) §11 for the bigger picture). IDs are stable references
for commit messages / conversation ("let's do SF-12 next").

Check items off as they land. Order within a milestone is a suggestion, not
a hard dependency chain, except where noted.

## Milestone 1 — Engine core (single plugin, proves the RT loop)

- [x] **SF-1**: CMake finds `lilv` and `jack` via `pkg-config` (fail with a
  clear message if missing); vendor `nlohmann/json` single header into
  `src/third_party/`. *Acceptance: `cmake --build` fails loudly and
  correctly if `liblilv-dev`/`libjack-dev` aren't installed, succeeds when
  they are.*
- [x] **SF-2**: `LilvWorld` setup + plugin discovery. A CLI flag
  (`--list-plugins`) prints every installed LV2 plugin's URI and name.
  *Acceptance: `neural_amp_modeler.lv2` shows up in the list.*
- [x] **SF-3**: Instantiate a single plugin by URI; classify its ports
  (audio/control/atom/cv × input/output) and allocate buffers per LV2
  spec. *Acceptance: unit-testable port classification for both
  `neural_amp_modeler.lv2` and one other installed plugin (e.g.
  `ir.lv2`).*
- [x] **SF-4**: JACK client: register in/out audio ports, process
  callback wires JACK buffers directly to the plugin's port buffers.
  *Acceptance: running the app with a simple gain-only LV2 plugin passes
  audio through audibly (test in `jack_simple_client`/qjackctl patchbay or
  via `pw-jack`).*
- [x] **SF-5**: Control port defaults from TTL + CLI override
  (`--control name=value`). *Acceptance: changing `--control gain=6.0`
  audibly changes output level.*
- [x] **SF-6**: `urid:map`/`unmap` feature (shared single map) +
  `state:makePath`/`mapPath`; load a `.nam` file into
  `neural_amp_modeler.lv2` via direct `lilv_state` set from a CLI path arg
  (no preset format yet — just prove atom:Path + state works
  end-to-end). *Acceptance: plugin produces amp-modeled audio, not just
  passthrough.*
- [x] **SF-7**: Replaced SF-6's synchronous worker stub with a real
  background thread per `PluginStage` (`src/lv2/Worker`): a mutex+condvar
  request queue (worker thread wakes, calls the plugin's `work()`, which
  may block/allocate freely since it's off the audio thread) and a
  lock-free SPSC ring buffer for responses, drained every `Run()` call
  before `lilv_instance_run()` (matching the worker extension's contract:
  deliver responses received since the last run() before the next one).
  `Worker::Stop()` joins the thread before the plugin instance is freed.
  *Acceptance: verified as part of SF-10 below (a live multi-stage,
  multi-worker model swap, not just single-plugin setup-time loading).*

## Milestone 2 — Chain (N plugins in sequence)

- [x] **SF-8**: `Stage`/`Chain` data structures; chain defined as a
  hardcoded array of plugin URIs (no JSON yet). Wire stage N's audio
  output buffer as stage N+1's input. *Acceptance: 2-stage chain (amp
  sim → IR loader) processes audio correctly.*
- [x] **SF-9**: Channel-count adaptation — mono-only chain end-to-end,
  with an implicit mono→stereo duplication stage only at the very final
  output if the audio backend wants stereo. *Acceptance: mono amp+IR
  chain outputs correctly to a stereo JACK output pair.*
- [x] **SF-10**: Multi-stage worker draining — confirm per-instance
  worker threads don't interfere with each other under a 3+ stage chain.
  *Acceptance: no dropped work responses / no xruns with 3 stages, one of
  which uses the worker extension.*
- [x] **SF-11**: Headless engine test harness: process a generated test
  signal (e.g. a sine sweep or noise buffer) through a chain with no real
  audio device, assert no NaNs/Infs and correct sample count out.
  *Acceptance: runs in CI without a JACK server or audio hardware.*
  Landed as `src/offline/OfflineEngine` + `--test-chain`, wired into
  `ctest` (`selftest`, always runnable; `headless_chain`, auto-skipped when
  the plugin isn't installed). Found and fixed a real bug in the process:
  worker-based model/IR loading is async, and offline processing outran it,
  so results depended on disk speed — a 2s run measured pure passthrough
  while a 120s run caught the model partway. Fixed with
  `PluginStage::SettleWorker()` / `Chain::Settle()`, which block until
  pending worker activity has landed (setup-time only, never on the audio
  thread). Worth remembering for SF-16/17: preset switching has the same
  hazard, and there the fix must *not* be a blocking wait.

## Milestone 3 — Presets

- [x] **SF-12**: Preset JSON schema + (de)serialization structs matching
  PLAN.md §5.1 (`nlohmann::json`). *Acceptance: round-trip a hand-written
  preset file through the parser without loss.* `src/preset/Preset`
  (`Preset`/`PresetStage`/`MidiTrigger`) + `--preset-roundtrip`, covered by
  the `preset_roundtrip` ctest over `tests/data/roundtrip-preset.json`.
  Note on "without loss": control values are `float` (that's what LV2
  control ports are), so a hand-written `0.1` becomes the nearest float and
  re-serializes as `0.10000000149011612`. The check therefore compares
  parsed *models*, not raw text — and serialization is idempotent, so a
  file only ever changes shape once, not on every save.
- [x] **SF-13**: Capture stage state: `lilv_state_new_from_instance` →
  serialize to string → embed in the preset JSON's `state` field.
  *Acceptance: saving a preset with a loaded `.nam` file captures the
  model path in the JSON.* `PluginStage::SaveState()/CaptureControls()`,
  `preset/ChainPreset` (keeps the lv2↔preset coupling in the preset layer
  so `lv2/` stays standalone), and `--save-preset`, covered by the
  `preset_save` ctest.
  Two decisions worth remembering:
  (1) state deliberately excludes control port values (`get_port_value`
  returns null) — controls live in the preset's `controls` map so they stay
  hand-editable and there's exactly one home per piece of data;
  (2) state is only captured from plugins that declare `state:interface`
  **in their TTL**, and dropped when it holds zero properties. Carla's
  plugins hand back a state interface they never declared, whose `save()`
  then fails — that was producing a spurious lilv error and a ~113-byte
  blob containing nothing.
- [x] **SF-14**: Restore stage state during chain build:
  `lilv_state_new_from_string` → `lilv_state_restore`. *Acceptance:
  loading a saved preset reproduces identical audio output to when it was
  saved (bit-exact or close enough by ear).* **Bit-exact**, not just by
  ear: `OfflineEngine::Result` gained an FNV-1a `output_hash` over every
  output sample, and the `preset_restore` ctest asserts a preset-built
  chain hashes identically to the chain it was captured from.
  `PluginStage::RestoreState()` + `BuildChainFromPreset()`, plus
  `--test-chain --preset <file>`.
  Restore order is state first, then the preset's `controls` — so the
  file's explicit controls win over any port values a hand-written or
  third-party state blob happens to carry.
  Failure behaviour is deliberately split: an empty chain, an unknown
  plugin URI, or an unparseable state blob fail hard (exit 1), but a
  *missing model file* only warns and leaves the rest of the chain running
  — losing one amp model shouldn't silence the whole pedalboard.
- [x] **SF-15**: Preset manifest — a directory of preset files, loaded by
  name or index via CLI (`--preset clean.json`). *Acceptance: switching
  between two preset files via CLI arg changes the running chain.*
  `src/preset/PresetLibrary`, plus `--list-presets`, `--run-preset`
  (live/JACK) and `--preset` on `--test-chain`, all taking `--preset-dir`.
  Resolves PLAN.md §12's open question — see the struck-through entry
  there: a sorted directory scan by default, overridden by an optional
  `manifest.json` that controls order *and* membership. A preset resolves
  by index, `name` field, filename stem, or path.
  A preset that fails to parse is warned about and skipped rather than
  taking down the whole library — one bad file shouldn't cost the player
  their pedalboard. Covered by the `preset_library` ctest, which asserts
  two presets hash differently, that all four resolution forms agree, and
  that a manifest overrides order/membership.

## Milestone 4 — Click-free preset switching

- [x] **SF-16**: `ActiveChain` atomic pointer; build the next chain fully
  on the control thread (instantiate + restore state) before publishing.
  *Acceptance: switching presets from a second CLI thread/command while
  audio is running doesn't crash or glitch the *old* chain while the new
  one is being built.*
  `JackEngine` now holds `std::atomic<AudioHost*>` (one acquire load per
  cycle, so a mid-cycle swap simply takes effect next callback), plus
  `SwapHost()`, `WaitForProcessCycles()` and `ProcessCycleCount()`.
  `--run-preset` reads switch commands from stdin on its own thread and
  builds each new chain on the control thread.
  Retirement uses the plan's §4.1 scheme: a counter bumped at the end of
  every callback, and the control thread waits 2 cycles after publishing
  before freeing the old chain (bounded, so a stalled audio thread can't
  hang the app).
  Because JACK ports are registered once, a swap to a chain with a
  different audio port layout is **rejected** with a clear message rather
  than attempted — verified with a mono→stereo attempt, which left the
  running chain untouched and still switchable afterwards.
  Note: verification here is live/manual, not ctest — the acceptance is
  inherently about a running JACK graph, which CI doesn't have.
- [x] **SF-17**: Hard mute → swap → unmute sequencing, plus safe teardown
  of the old chain after a grace period once the audio thread has moved
  on. *Acceptance: rapid repeated preset switching (script it) doesn't
  crash, leak, or produce audible clicks worse than the intentional
  mute gap.*
  `JackEngine::SwitchHost()` encapsulates the whole sequence (validate port
  layout → fade out → wait for actual silence → swap → fade back in) so
  callers can't get the ordering wrong. The mute is a short **ramp**
  (`kFadeMilliseconds = 5`), not an instant jump to zero — an instant jump
  is itself a click. Port layout is validated *before* muting, so a
  rejected switch costs nothing audibly.
  **Superseded by SF-18**, which replaced the mute with a crossfade; the
  mute machinery no longer exists. The A/B flag is now `--no-crossfade`.
  Measured, not assumed: recording the output through 3 switches gave
  **3 discontinuities >0.05 without the fade, and 0 with it** (max jump
  0.1323 → 0.0479, against 0.0258 for a no-switch control recording).
  50 rapid switches under ASan/UBSan: no use-after-free, no UB, and the
  reported leak stayed **constant** at 3709 bytes whether 6, 20 or 50
  switches ran — fixed one-time allocations in the plugin/library layer,
  not per-switch, so chain retirement is clean.
- [x] **SF-18** *(stretch)*: Replace hard mute with a short equal-power
  crossfade. *Acceptance: switching is inaudible as a "gap," just a
  smooth transition.*
  Supersedes SF-17's mute machinery, which is gone: `SwitchHost()` now
  publishes an `incoming_` host, and for `kCrossfadeMilliseconds` (12ms)
  **both chains run**, each into its own pre-allocated scratch buffer, mixed
  with cos/sin gains so summed power stays constant (a linear crossfade
  would dip mid-fade). The audio thread promotes the incoming host and
  clears `incoming_` when the fade completes, which is how the control
  thread knows it can retire the old chain.
  `--run-preset --no-crossfade` keeps the raw `SwapHost` path for A/B.
  Measured over 3 switches: crossfade gave **0 clicks and 0 gaps at any
  switch**, vs 8 clicks for the hard swap (max jump 0.0480 vs 0.1697). The
  one 2.7ms silent run in the crossfade recording sits at 1.451s, i.e.
  before the test tone even starts at ~1.95s — not at a switch.
  CPU roughly doubles during a fade, so that was checked explicitly:
  switching between two 8-stage NAM presets (16 model instances running at
  once) gave **0 xruns** over 12 crossfades. 40 switches under ASan/UBSan:
  no memory errors, leak constant at 3709 bytes regardless of switch count.

## Milestone 5 — MIDI

- [x] **SF-19**: MIDI input via JACK MIDI port; parse Program Change, CC,
  and Note On messages; log parsed events to console. *Acceptance:
  moving/pressing something on a connected MIDI controller prints the
  right message type/channel/number.*
  `src/midi/MidiEvent` (parsing) + a `signalforge:midi_in` JACK port. The
  audio thread parses and enqueues into a lock-free SPSC ring; the control
  thread drains and logs — **no printf or allocation on the audio thread**.
  A full queue drops and counts rather than blocking or overwriting.
  NoteOn with velocity 0 is reported as NoteOff (controllers commonly send
  releases that way; treating it as a press would double-trigger every
  footswitch). Non-musical messages (clock, pitch bend) and truncated
  messages are rejected.
  Verified end-to-end over real JACK MIDI: 7 synthetic messages played
  through `aplaymidi` came back with exact type/channel/number/value, and
  the AIRSTEP itself emitted `cc ch=0 number=1` on connect, parsed
  correctly. Parser covered by the `midi_event` ctest (13 cases, no
  hardware needed).
- [x] **SF-20**: Auto-connect to available hardware MIDI input ports at
  startup (skip virtual/software ports), with a config override to pick
  a specific device by name. *Acceptance: plugging in the AIRSTEP pedal
  and starting the app requires no manual patching.*
  `JackEngine::ConnectMidiInputs()`, called from `--run-preset`. By
  default it connects every MIDI source flagged `JackPortIsPhysical`,
  which cleanly excludes other applications' MIDI outputs (verified:
  Ardour's five software MIDI outs are all skipped). Two extra rules were
  needed on top of the flag:
  - **"Midi Through" is excluded by name.** The ALSA sequencer loopback is
    flagged physical exactly like a real controller, but it's an
    app-to-app pipe -- auto-connecting it would pick up whatever anything
    else routes there.
  - **A named device overrides both rules.** `--midi-device <substring>`
    is a case-insensitive match on the port name and bypasses the
    physical/loopback filtering entirely, so odd setups and
    hardware-free testing have an escape hatch.

  `--no-midi-connect` opts out. An already-present connection (`EEXIST`)
  counts as success rather than an error, so manual patching and
  auto-connect coexist.
  Verified against the live PipeWire graph in five modes, checking the
  actual link set rather than just our log: default connected exactly
  `Midi-Bridge:AIRSTEP MIDI 1 (capture)` and `bluez_midi.server:out` and
  nothing else; `--midi-device airstep` narrowed it to one; `--midi-device
  "midi through"` connected the loopback the default correctly refuses;
  an unmatched name and `--no-midi-connect` both left `midi_in` with zero
  links and warned appropriately. Acceptance met end-to-end: with no
  manual patching at all, the AIRSTEP's `cc ch=0 number=1` arrived in the
  running app.
- [x] **SF-21**: MIDI mapping table data structure + JSON persistence
  (PLAN.md §5.2). *Acceptance: a hand-written mapping file loads and is
  queryable by incoming MIDI event.*
  `src/midi/MidiMap` — `MidiBinding` (matcher + action + target),
  `MidiMap` (load/save/round-trip, `Resolve`). PLAN.md §5.2 was updated to
  the schema we actually settled on: flat target fields instead of a
  nested `"target"` object, `stage` instead of the ambiguous
  `preset_slot`, and an optional `"channel"` that means *any* when
  omitted (pedals ship on whatever channel they ship on).
  **`Resolve` is stateful, which is the whole trap here.** A momentary
  footswitch sends CC 127 on press and 0 on release, so firing on both
  would advance two presets per stomp; a held switch that repeats its
  message would advance continuously. So the map tracks which switch
  bindings are held and fires on the press edge only. `set_control` is
  exempt — for a knob, 0 is a position, not a release. Program changes
  have no release and always fire.
  Loading is strict: unknown action, missing target, `set_control` bound
  to a `program_change` (carries no value), an empty min/max range, and
  two bindings claiming the same message are all rejected with the
  offending index. A binding that fails silently at stomp time is much
  worse than one that refuses to load.
  Verified three ways. (1) `midi_map` ctest, 45 assertions covering
  parsing, round-trip, matching, switch edges, and value scaling —
  validated by injecting three regressions (removing held-switch
  suppression, removing the press/release edge, removing omni-overlap
  duplicate detection) and confirming each was caught. (2) `midi_map_file`
  ctest loads `tests/data/example-mapping.json` through the real binary
  via the new `--check-midi-map`, which also prints the table in plain
  language for anyone debugging their own file. (3) Live over real JACK
  MIDI with `--run-preset --midi-map`: press fired, release didn't,
  second press fired, held repeat didn't, an expression sweep resolved to
  -20 / 0.157 / 20, and unmapped messages passed through with no action.
  Environment note found while testing: PipeWire's Midi-Bridge silently
  drops **CC 99** (NRPN parameter MSB) between ALSA seq and JACK —
  `aseqdump` sees it, we never do. CC 20/70/80/102 and program changes
  all pass fine. Not our bug, but worth knowing before mapping CC 98/99.
- [x] **SF-22**: Wire mapping actions: `load_preset`, `next_preset`,
  `prev_preset`, `toggle_bypass`, `set_control`. *Acceptance: a Program
  Change message triggers `SF-16`/`SF-17`'s preset switch end-to-end.*
  `--run-preset --midi-map <file>` now performs what SF-21 resolves. The
  preset switch itself reuses SF-16/17's build-then-crossfade path
  unchanged, so a footswitch and a typed command take exactly the same
  route.
  **`toggle_bypass` required implementing bypass, which didn't exist** —
  `PresetStage::bypassed` was parsed and round-tripped but nothing ever
  acted on it. It lives in `PluginStage` (not `Chain`), so a single-plugin
  host gets it too: an atomic flag, and `Run()` copies audio inputs
  straight to outputs instead of calling the plugin. That makes it *true*
  bypass — the plugin stops running, so reverb/delay tails are cut rather
  than ringing out, which is how a stompbox behaves. Channel counts needn't
  match: extra outputs repeat the last input (so a mono->stereo plugin
  bypasses to both sides) and outputs are silenced if there are no audio
  inputs at all. Worker responses are still drained while bypassed, since
  a model may be mid-load and dropping its response would leave the plugin
  wrong when it's switched back in. Bypass is now also captured and
  restored by presets.
  Library order is read once at startup, not per stomp — a directory scan
  has no business sitting between a footswitch and the sound.
  `next`/`prev` wrap around, because a player stepping past the end of the
  board wants the first sound back, not silence. `set_control` applies
  silently by design: an expression pedal sends a message per degree of
  travel and a log line each would bury everything else.
  New CLI along the way: `status` (current preset, per-stage bypass state
  and control values — the readback for actions that are otherwise
  silent), `next`/`prev`/`bypass <stage>` typed commands, and
  `--test-chain --bypass <stage>` so the bypass path is measurable offline
  instead of only by ear.
  Verified four ways. (1) New `bypass` ctest compares *output hashes*, not
  levels — an almost-right bypass (an extra copy, a stale block, an
  off-by-one) still lands on the right peak. Bypassing both stages of a
  two-gain chain is bit-identical to a single bypassed stage; bypassing
  either half is bit-identical to a one-stage chain; and a sanity check
  fails if bypass changes nothing at all. Validated by injecting two
  regressions — making `SetBypassed` a no-op, and copying one sample short
  — both caught. (2) ASan/UBSan clean over the bypass path at block sizes
  64/256/1024, all three producing the same hash, which also proves the
  short final block is handled. (3) Live over real JACK MIDI, all five
  actions: PC loaded a preset, two `next` stomps walked crunch -> lead ->
  wrapped to clean, `prev` wrapped back to lead, releases correctly did
  nothing, bypass toggled on and off, and an expression CC scaled to 8.0.
  (4) `status` confirmed the values actually reached the plugin — stage 1
  reading `gain 2.01575` (= 32/127 x 8) and `BYPASSED`, stage 0 untouched.
  Failure paths checked too: a binding naming a missing preset, and one
  targeting stage 1 after a one-stage preset is loaded, both warn and
  leave the board playing.
- [x] **SF-23**: MIDI-learn (CLI first): a `learn <target>` command that
  binds the next incoming MIDI event to that target and appends it to the
  mapping file. *Acceptance: `learn next_preset` then pressing a pedal
  button creates a correct mapping entry.*
  `src/midi/MidiLearn` + a `learn <action> [args]` console command
  (`learn cancel` to abort). Binds to the channel the message actually
  arrived on — learning knows exactly what came in, and a specific
  channel is what keeps two controllers from colliding. Re-learning a
  message *moves* it rather than failing as a duplicate
  (`MidiMap::Bind`), because pressing a switch again is how a user says
  "make it do this instead". A `set_control` learned without an explicit
  range picks up the port's real bounds from its `PortInfo`.
  **The hard part isn't listening, it's deciding which message the user
  meant.** Two things get in the way, and both are handled:
  - A footswitch sends press *and* release. Learning the release would
    record a value of 0 and then fire on the wrong edge forever after.
  - Controllers chatter. Fernando's expression pedal streams CC 7
    continuously (values jittering 88-91) whether or not anyone touches
    it, and it wins every race against a human foot. So `MidiLearn`
    watches all traffic continuously and refuses to bind a source that
    was already streaming before arming.
  **A real bug was caught by live testing that the unit tests missed.**
  The noise check originally compared timestamps at `Offer()` time —
  but the live loop `Observe()`s an event *before* offering it, so the
  offending controller's own arrival moved `last_ms` past the arming
  instant, `armed_at_ms_ - last_ms` underflowed as unsigned, and the
  chattering pedal bound anyway. The tests passed only because they
  offered events without observing them first — they didn't reproduce
  the production call order. Fixed by snapshotting the chattering
  sources *at* `Arm()`, which removes the ordering hazard entirely
  rather than patching the comparison; the tests now route every offer
  through an `ObserveThenOffer` helper that mirrors the real loop.
  Verified by re-introducing the original design and confirming the
  corrected tests fail on it (6 assertions).
  Otherwise verified three ways. (1) `midi_learn` ctest, 44 assertions
  over parsing, binding, noise rejection and rebinding — validated by
  injecting three regressions (removing noise rejection, removing the
  press-edge filter, and lowering the chatter threshold so *any* prior
  traffic counts as noise), each caught, the last one confirming the
  rejection is calibrated rather than merely present. (2) Live against a
  simulated chattering pedal — CC 7 streaming ~10/second — where learn
  rejected ~15 consecutive chatter messages with a one-time explanation
  and then bound the deliberate program change. (3) Two sequential
  learns in one session, both written through to the mapping file and
  re-loading cleanly alongside the hand-written bindings.

## Milestone 6 — UI

- [x] **SF-24**: Dear ImGui + SDL2 window skeleton, empty, builds and
  runs alongside the audio engine (engine on its own thread/callback,
  UI on the main thread). *Acceptance: window opens, audio keeps running,
  closing the window shuts down cleanly.*

  Dear ImGui v1.92.9 vendored at `src/third_party/imgui` (core plus the
  SDL2 and SDL_Renderer2 backends; MIT licence file kept alongside). The
  SDL_Renderer backend rather than OpenGL, deliberately: it needs no GL
  loader and no GL development headers, which is one less thing to be
  missing on a Pi.

  `src/ui/AppWindow` owns the platform — SDL, the window, the ImGui
  context, and the frame loop — and nothing else. `src/ui/BoardView`
  draws one frame from a `BoardViewState` snapshot the caller fills in,
  so the drawing code never reaches into the engine or the chain. The
  UI is opt-in behind `--gui`; the board is expected to run on machines
  with no screen attached.

  The engine was already on its own thread — JACK's real-time callback —
  so the UI took over the *control* loop rather than the audio path:
  where `--run-preset` used to sleep 20ms between polls, it now draws a
  frame. MIDI, console commands and preset switching all still happen on
  that one control thread, which means a stalled UI delays a footswitch
  but can never interrupt the sound.

  Three things worth recording:

  - SDL installs its own SIGINT/SIGTERM handlers, which know nothing
    about stopping the engine and freeing the chain in order.
    `SDL_HINT_NO_SIGNAL_HANDLERS` keeps Ctrl-C reaching ours. SDL is
    also initialised with `SDL_INIT_VIDEO` alone — it has an audio
    subsystem of its own, and there is exactly one audio path here.
  - Frame pacing is measured, not inferred. `SDL_RENDERER_PRESENTVSYNC`
    is reported as set even under the dummy video driver, where
    presenting returns instantly — trusting the flag spun the UI thread
    at ~500fps (60 frames in 0.12s). Sleeping to a measured 16ms floor
    instead brought that to 1.03s for the same 60 frames.
  - `scripts/run.sh` backgrounds the binary, and bash points a
    background job's stdin at `/dev/null` unless told otherwise, so
    console commands were silently dead whenever the script ran from a
    non-interactive shell. Fixed with an explicit `<&0`.

  Verified: (1) `--test-ui`, a new headless mode that opens the window,
  draws N frames against a synthetic chain-less board and shuts down,
  run under SDL's dummy video driver — no display, no sound card, no
  plugins, but the real `AppWindow`/`BoardView` code. Added as the
  `ui_smoke` ctest (12 tests now), and validated by injecting a
  frame-loop regression: `FAIL: drew 10 of 60 frame(s)`. (2) Clean under
  ASan/UBSan/LSan; the leaks LSan reports against a real display are all
  inside libSDL2/libdbus/libffi one-time init, none in our code. A
  deliberately reversed teardown order was *not* caught, so that
  ordering rests on the API contract rather than on a test. (3) Live,
  with the JP-2C presets: the audio-cycle counter climbed 7243 → 29601
  over ~60s = 373/s, against the 48000/128 = 375 the engine should be
  doing — the UI thread is demonstrably not carrying the audio. (4)
  Console `status`, `next` and `bypass 0` all took effect with the
  window up, the view followed the preset switch from Clean to Rhythm,
  and both closing the window and Ctrl-C shut down through the normal
  "Stopping." path with no stray process.

  A false alarm worth remembering: console commands appeared to be
  broken under `--gui` across four runs. It was a stale `exec 9>fifo`
  descriptor in the test harness — the persistent shell held the write
  end of a fifo that had since been replaced, so the writes went
  nowhere. The same harness had also made a MIDI-triggered preset switch
  look like a typed `next` succeeding. Verify with a fresh process per
  run, not a long-lived shell.
- [x] **SF-25**: Preset list view — lists presets from the manifest,
  clicking one loads it (via SF-15/16/17). *Acceptance: click-to-switch
  works without audible glitches.*

  The list is flattened out of `PresetLibrary` once at startup into a
  plain `vector<BoardPreset>` of name and stem, not read live per frame.
  The library doesn't change while the board is running, and a redraw
  has no business copying preset data out from under a switch that may
  be replacing it. Presets with no `name` field fall back to their
  filename, so the list never shows a blank row you can click but can't
  identify.

  The view reports a click as `requested_preset` and the caller decides;
  the highlight always follows `current_preset`, which is what is
  actually playing. So a switch that fails leaves the selection correct
  on the next frame instead of lying, and console/MIDI switches move the
  highlight for free. Clicking the preset that's already loaded is a
  no-op rather than a pointless chain rebuild.

  The switch runs *after* `EndFrame`, not before. Building a chain can
  take longer than a frame, so doing it after presenting means the click
  is drawn as pressed before the window stops responding — the pause
  reads as "working" rather than as a hang. Audio is unaffected either
  way; it is on JACK's thread, not this one.

  Verified: (1) `ui_smoke` now drives a real synthetic click. It injects
  mouse events through Dear ImGui's own input queue, aimed at the
  coordinates the view reports for the first row rather than a
  hard-coded position that any layout change would silently invalidate,
  and asserts the click produces *exactly one* request for *exactly*
  index 0. Also draws the no-library case, which a normal session never
  shows. Validated by injecting two regressions: dropping the click
  handler (`0 preset request(s), expected 1`) and making the row fire
  every frame while the button is held (`2 preset request(s)`) — the
  second is the one that would rebuild the chain repeatedly for a single
  press. (2) Live, driving real X11 clicks into the running window with
  XTEST: clicking "Lead" then "Rhythm" switched both times, one switch
  per click, the highlight and header followed, the audio-cycle counter
  kept climbing across both switches, and re-clicking the current preset
  produced no rebuild. (3) Clean under ASan/UBSan. (4) 12/12 tests in
  both the UI and headless configurations.
- [x] **SF-26**: Stage list for the selected preset — plugin name, bypass
  toggle, move up/down, remove. *Acceptance: reordering/removing a stage
  in the UI takes effect on next preset save+reload.*

  Stages now carry a real name: `PluginStage::Name()` reads the plugin's
  declared name from lilv, falling back to the URI when a plugin
  declares none, so the list never shows a blank row. Looked up on
  demand, never on the audio thread.

  The two kinds of edit here are not alike, and the code says so. Bypass
  flips a flag the audio thread already reads — no rebuild, effective on
  the next block. Moving or removing changes the shape of the chain and
  costs the same crossfaded rebuild a preset switch uses, for the same
  reason: the old chain keeps making sound while the new one is built.
  The edit is applied to a snapshot of the *running* chain, not to the
  file it came from, so a knob moved by hand or by an expression pedal
  survives having a stage reordered underneath it.

  A rebuild that fails leaves the running chain untouched and says why
  in the window. This is not hypothetical: adjacent stages have to agree
  on channel counts, and not every order of a given set of plugins does.

  Two guards worth naming. The last stage can't be removed — an empty
  chain has no ports for the engine to connect, and silently keeping one
  would be worse than refusing. And saving copies the preset's non-chain
  metadata across from the file it was loaded from, because a chain
  snapshot only knows about stages: without it, every save would
  silently drop the preset's MIDI trigger. Saving also re-reads the
  directory, so next/prev and the preset list see what was just written
  rather than the copy loaded at startup.

  `BoardView` no longer touches `Chain` or `PluginStage` at all. It
  draws from a per-frame `vector<BoardStage>` snapshot, which decouples
  the drawing code from the audio objects and — the point — makes the
  entire view exercisable with no plugins installed and no engine
  running.

  Verified: (1) `ui_smoke` grew a generic widget probe: the view records
  where each interactive widget landed and whether it was clickable, so
  the test clicks *by name* ("bypass:1", "down:0", "up:2", "remove:2")
  rather than by coordinate. Six real click interactions asserted, each
  for the exact edit and index, plus the disabled states — nothing above
  the first stage, nothing below the last, and a one-stage chain that
  can't be emptied. Validated by injecting two regressions: removing the
  disabled guard on "move earlier" (which then fired `MoveUp 0`, also
  proving the click genuinely lands on that button rather than missing
  it) and making "move later" report `i + 1`. (2) Live with three real
  plugins — NAM, x42-eq, x42-dpl — driven by XTEST clicks: reordered
  stage 0 later, bypassed a stage, removed a stage, saved, and switched
  away and back. The reloaded chain came back in the edited order with
  the bypass intact and the removed stage gone, which is the acceptance
  criterion and also proves the post-save directory re-read works, since
  a stale library copy would have restored three stages. (3) Metadata
  preservation checked by hand-adding a MIDI trigger, editing, and
  saving — the trigger survived, and removing the one line that carries
  it across made it vanish. (4) Clean under ASan/UBSan; 12/12 tests in
  both configurations.
- [x] **SF-27**: "Add stage" — searchable list of installed plugins from
  `lilv_world_get_all_plugins()`. *Acceptance: adding
  `neural_amp_modeler.lv2` to an empty preset via the UI works.*

  The catalogue is scanned once at startup: every installed plugin, its
  name, and how many audio channels it takes and returns. Walking every
  plugin's ports is far too slow to do while drawing, and nothing gets
  installed while the board is running.

  Plugins with no audio path at all — MIDI filters, control utilities —
  can't sit in a sequential audio chain, so they're left out. Left out
  and *counted*, though: the browser's footer reads "86 of 86 (59 more
  installed have no audio path)", which adds up to the 145 that
  `--list-plugins` reports. A browser that quietly shows fewer plugins
  than are installed looks broken.

  Search matches name or URI, case-insensitively — the URI because
  that's how preset files and docs name a plugin, so it's what someone
  will paste. Each row shows its channel counts, so a mono pedal can be
  told from a stereo one *before* inserting it and finding out the chain
  won't build.

  Adding appends to the end. The stage list already has move up/down, so
  "add, then position it" is one story rather than two ways to say where
  a plugin goes. `PresetWithAddedStage` is shared by the button and by
  `--test-add-stage`, so the test drives the real function rather than a
  copy of it.

  **Deviation from the acceptance criterion, stated plainly:** a preset
  with an empty chain cannot be loaded at all — the preset layer rejects
  it with "preset 'Empty' has an empty chain". That is a pre-existing
  constraint, not something this ticket introduced, and it is why the
  stage list refuses to remove the last stage (SF-26). So the criterion
  was met in the nearest honest form: NAM added to a one-stage chain,
  both live through the UI and as an automated test.

  Verified: (1) `ui_smoke` grew a browser phase — open, all rows drawn,
  search by name, search by URI, search case-insensitively, Cancel
  closing without adding, then picking a plugin *while a search is
  active* and asserting the index refers to the catalogue rather than to
  the filtered list. Also that no browser rows exist before it is
  opened, and that it closes behind a choice. Validated by injecting two
  regressions: keying the choice by filtered position (`requested index
  0, expected 2` — the "adds the wrong plugin, but only when you
  searched" bug) and disabling the filter (`left 3 plugin(s)`). (2) New
  `add_stage` ctest with a real plugin (14 tests now), which caught a
  dropped append; strengthened after a second injection — a stage
  inheriting its neighbour's controls and LV2 state — slipped through
  the first version, since that would silently carry over another
  plugin's loaded model. (3) Live: NAM appended to an eq-only chain
  offline (24000 frames, no non-finite samples), and through the real
  window with the real 86-plugin catalogue, giving "Added Audio Gain
  (Mono)." and an unsaved-changes state. (4) The refusal path live too:
  a 2-in plugin after a 1-out stage is declined, the running chain
  untouched, audio uninterrupted.

  One thing this ticket fixed on the way. `Chain::Create` printed its
  real reason to stderr and handed callers a generic "could not build
  the chain", so the window said nothing useful while the terminal knew
  exactly what was wrong. The reason is now returned as text as well as
  printed, and the window shows "stage 0 has 1 output(s) but stage 1
  wants 2 input(s), and channel adaptation isn't implemented yet". A
  status line that promises to say why has to actually say why.

  Noted in passing, not addressed: `fil4.lv2` logs "LV2 comm-buffersize
  is insufficient 8/4320 bytes" when hosted here. It processes audio
  fine; our atom port buffers are smaller than it would like for its own
  GUI messaging. Pre-existing, unrelated to this ticket.
- [x] **SF-28**: Auto-generated control panel from port metadata (name,
  range, default, current value) — sliders/toggles per control port.
  *Acceptance: moving a slider audibly changes plugin behavior in
  real time.*

  `PortInfo` gained the `lv2:portProperty` hints it was missing:
  `toggled`, `integer`, `enumeration`, and `pprops:logarithmic`. Without
  them a control panel gets things wrong in ways that matter — an on/off
  switch drawn as a 0..1 float slider, a whole-number setting draggable
  to 2.7, or a 5Hz–1250Hz frequency control whose useful range is
  squeezed into the first millimetre of a linear slider. All three cases
  are real: x42-eq has four toggles and a logarithmic highpass
  frequency. `--inspect` prints the hints too, so what the UI will make
  of a plugin can be checked from the terminal.

  Each stage gets an expander arrow; controls are collapsed by default,
  because a board of four pedals with thirty controls each would open as
  a wall of sliders. Output control ports — meters, peak readouts — are
  shown but never editable: reading them is the point, writing them
  would be writing over the plugin.

  A moved control goes straight onto the running plugin. No rebuild, no
  crossfade, audible on the next block; the same path a MIDI expression
  pedal already takes.

  Verified: (1) `ui_smoke` gained a control-panel phase — controls
  absent while collapsed, expanding draws exactly that stage's controls
  and no other stage's, a toggle reports its *maximum* rather than some
  fraction along a range, a slider reports the right stage and symbol
  with a value inside its range, a meter is present-but-disabled and
  ignores clicks, and collapsing puts them all away. Validated by
  injecting three regressions: ignoring the `toggled` hint, making
  output ports editable, and drawing every stage's controls regardless
  of expansion. (2) Objectively, that a control change alters the sound:
  the same preset rendered at `input_level` -12/0/+12 gives peaks
  0.207/0.221/0.428 and three distinct output hashes.

  Worth recording: sliders and buttons differ, and the test had to learn
  it. Dear ImGui commits a slider's value on mouse-*down* while a button
  fires on release, so the first version of the slider check read the
  release frame and saw nothing.

  **A bug found while trying to verify this ticket.** `--test-chain
  --preset X --control 0:input_level=12` silently ignored the
  `--control` flag: the preset branch never called
  `ApplyChainControls`. Rendering at three different `input_level`
  values produced three *identical* hashes, which reads exactly like "a
  control has no effect on the sound" — the opposite of what this ticket
  is about. Command-line flags now apply on top of the preset for both
  branches, which is the only precedence that makes sense for something
  typed at a prompt. The `input_level=0` hash is byte-identical to
  before the fix, so default behaviour is unchanged.

  **Not covered, stated plainly.** The three lines in `main.cpp` joining
  the view's control request to `SetControl` on the running stage are
  not exercised end-to-end. Both halves are: the view's reporting by
  `ui_smoke`, and `SetControl`'s audible effect by the hash comparison
  above. The junction itself would need a real click in a real window,
  and live GUI testing is now gated behind a KDE "control input devices"
  permission prompt that is the user's to grant, not mine to click
  through. The "audibly" in the acceptance criterion is in any case
  theirs to confirm by ear.

  Also noted: ASan reports a 32-byte leak in 4 allocations inside the
  NAM plugin's own `NeuralModelLoader::CreateFromJson`. Third-party,
  pre-existing; a chain without NAM is clean, and so is the new
  port-property code.
- [x] **SF-29**: File picker widget for `atom:Path` ports (model/IR
  files). *Acceptance: picking a `.nam` file from the UI loads it into
  the running chain.*

  File parameters aren't ports, so nothing in the program knew they
  existed. `FindPathProperties` reads them from the plugin's TTL — the
  `patch:writable` parameters whose `rdfs:range` is `atom:Path` — along
  with their `rdfs:label` and their `mod:fileTypes`. That last one is a
  MOD extension rather than core LV2, but it's what plugins actually
  declare, and it's the difference between a picker showing four `.nam`
  files and showing a home directory. NAM declares
  `nam, nammodel, json, aidax, aidadspmodel`. `--inspect` prints all of
  this, so what the UI will make of a plugin can be checked from a
  terminal.

  Showing *which* model is loaded needed `PluginStage::GetPathProperty`:
  it runs the plugin's own `state:interface` save() with a store
  callback that keeps one property. Asking the plugin is the only
  reliable way — a path that arrived inside a preset's state blob was
  never seen by this program, so remembering what we set would show
  nothing for exactly the presets that matter.

  The picker itself touches no filesystem. It draws the listing it is
  handed and reports where the user wants to look next; the caller
  resolves that with `std::filesystem` and does the listing, filtered by
  the property's declared types, directories first. Which is what lets
  the whole picker be driven by a test against a directory that doesn't
  exist. It opens where the current file lives, so swapping one capture
  for its neighbour doesn't start from `$HOME`, and it lists only while
  open and only when the directory changed — a scan per frame would be
  pointless work and, on a network share, a visible stall.

  Verified: (1) `ui_smoke` gained a picker phase — rows absent while
  closed; opening asks for the loaded file's directory; every entry
  drawn plus an "up" row that is always offered even at the root;
  clicking a directory navigates rather than loading it as a model;
  choosing a file reports an absolute path against the directory shown,
  for the right stage and property; and the picker closes behind it.
  Validated by injecting three regressions: treating a directory click
  as a file choice, reporting the file relative instead of absolute, and
  opening at wherever the picker was last left. (2) New `path_property`
  ctest (14 tests now) doing the other half with a real plugin and a
  real file: read the property, load a different model, settle the
  worker, read it back, and process audio. The "before" value comes from
  a model restored via the preset's state blob, so it also proves
  `GetPathProperty` reads state this program never saw. Validated by
  stubbing the readback (`reads back as ''`). Peak moves 0.221 -> 0.352
  between models, which is the sound changing. (3) Our code is clean
  under ASan/UBSan; every leak in the run traces to
  `neural_amp_modeler.so` itself.

  **Two caveats, stated plainly.** The `path_property` test needs a
  model file, and there isn't one we can assume exists, so it skips
  unless named: `cmake -B build -DSIGNALFORGE_TEST_MODEL=<path>`. A test
  that skips by default is worth less than one that doesn't, and this is
  the honest trade rather than shipping a fixture we'd have to invent.
  And as with SF-28, the junction in `main.cpp` between the view's
  request and `LoadPathProperty` is not covered end-to-end — both sides
  are, the join isn't — because live GUI clicking is behind a KDE
  input-injection permission that is the user's to grant.

  Enumerations still render as whole-number sliders rather than a
  dropdown of `lv2:scalePoint` labels. Nothing installed here uses them;
  worth revisiting if something does.

  **Follow-up, found by running the board rather than by testing it.**
  The per-frame stage snapshot read each file parameter's current value
  through `GetPathProperty`, which asks the plugin to *serialise its
  state* — and re-parsed the plugin's TTL through `PathProperties()`
  besides. At 60Hz. A ten-second GUI run against the real presets
  produced 609 `Saving state` lines from neural-amp-modeler: RDF parsing
  and allocation on the UI thread, once a frame, to redraw a filename
  that cannot have changed. Those two reads (and the plugin's name) are
  now cached per chain and refreshed only when the chain is replaced or
  a file is loaded into it; the rest of the snapshot is still rebuilt
  every frame, because bypass and control values do change from MIDI and
  from the console and are float reads. The same ten-second run now logs
  1, and a run that stomps through three presets logs exactly 3 — one
  per chain, which is the floor.
- [x] **SF-30**: MIDI-learn UI affordance — a "learn" button next to any
  control/toggle/preset entry, wired to SF-21–23. *Acceptance: no CLI
  needed anymore to create a mapping.*

  One `DrawLearnButton` next to everything bindable: `next`/`prev` above
  the preset list, every preset row, and — inside an open stage — its
  bypass and each writable control. Next to the button sits the message
  currently bound to it, in words ("PC 3", "CC 80"), so the board can be
  read at a glance instead of by opening the mapping file. Output ports
  don't get one: a meter has nothing to bind.

  The view deliberately doesn't know what a `MidiBinding` is. It reports
  a `LearnRequest` naming the thing to bind, and `main.cpp` translates
  that into the same `MidiLearn::Arm` the `learn` console command uses,
  and the same `commit_learned` that writes the table back to disk.
  So a binding made with the mouse and one made by typing take exactly
  the same route, including SF-23's chatter rejection.

  **A control's request carries the port's real range.** A binding built
  without one defaults to 0..1, which would leave an expression pedal
  moving a -18..18 dB control across the first thirty-sixth of its
  travel — the sort of bug that gets diagnosed as a broken pedal.

  Every learn button is disabled while a learn is armed, because
  `MidiLearn` binds one thing at a time and a second press would
  silently replace the target you were about to press a switch for.
  While armed, the prompt and its cancel button sit at the top of the
  window rather than in the status line: it's the only thing on screen
  that matters until it resolves.

  The outcome now reaches the window, not just stdout — what was bound,
  whether it replaced an existing binding, whether saving the file
  failed (active either way, but a binding that won't survive a restart
  is discovered at rehearsal), and the "that controller is streaming on
  its own" advice that would otherwise leave the window saying "press
  something" while a chattering pedal ate every press. Without that, the
  CLI is still needed to find out what happened, which is the acceptance
  criterion.

  **Two real bugs, both found by the new test, both invisible in a
  screenshot.** (1) Dear ImGui identifies a widget by its label and every
  one of these is labelled "learn"; the per-preset and per-stage ones sit
  in ID scopes that keep them apart, but `next` and `prev` sit side by
  side at window level and were therefore the *same widget* — two
  buttons, one of which did nothing. Fixed by scoping each to its probe
  id. (2) A preset row is a full-width `Selectable` and the learn button
  is drawn on top of its right-hand end; Dear ImGui gives the hover to
  the first item that claims it, so pressing "learn" loaded the preset
  instead of arming a binding for it. Fixed with
  `ImGuiSelectableFlags_AllowOverlap`, which in turn requires the
  previous frame to have seen the button hovered — so `ui_smoke`'s click
  helper now hovers before it presses, which is what a real mouse does
  anyway.

  Verified: `ui_smoke` gained a learn phase — no cancel button while
  nothing is armed; `next`, `prev` and a preset row each arm exactly
  their own target and *nothing else* (the preset row asserting it
  didn't also switch preset, the bypass row that it didn't also toggle
  bypass, the control that it didn't also move the control); a collapsed
  stage draws no learn buttons at all; a control's request carries
  -18..18 rather than 0..1; and while armed, every learn button is
  recorded disabled, counted rather than merely checked so "all
  disabled" can't be satisfied by drawing none, clicking one does
  nothing, and cancel is the one thing still clickable. Validated by
  injecting two regressions — dropping the disabled-while-armed guard
  (`'learn-next:0' was still clickable`) and dropping the range carry
  (`range 0..1, expected the port's -18..18`) — on top of the two bugs
  above, which the test caught on its first run.

  Not covered end-to-end, for the same reason as SF-28 and SF-29: the
  junction between a learn button and an arriving MIDI message needs
  live GUI clicking, which is behind a KDE input-injection permission
  that is the user's to grant. Both sides are covered; the join isn't.
  Worth one live pass: press learn next to a preset, stomp a switch, and
  confirm the window names what it bound and the mapping file grew an
  entry.

  Environment note: `ui_smoke` is skipped unless `libsdl2-dev` is
  installed — without the headers CMake configures a headless build and
  none of the UI is compiled at all.

## Milestone 7 — Polish

- [x] **SF-31**: Missing-plugin handling — a preset referencing an
  uninstalled plugin URI skips that stage with a visible warning instead
  of crashing. *Acceptance: manually edit a preset's URI to garbage,
  confirm graceful degradation.*

  Missing plugins are the one build failure a player can't do anything
  about mid-song, so they now cost that stage rather than the whole
  preset. `BuildChainFromPreset` asks lilv about each URI first
  (`Chain::IsPluginInstalled`), leaves out what isn't there, and reports
  it. `Chain::Create` stays strict: a URI typed on a command line is
  meant, and a typo there should be refused, not quietly skipped.

  **Skipping shifts every stage after it, and that is where the bug
  would be.** Chain stage 1 may be the file's stage 2, so state and
  control restore is keyed to the file's index — get it wrong and one
  pedal's settings land on another, which is a wrong *sound* rather than
  an error. Two consequences that aren't obvious are stated in the
  warning rather than left to be discovered: saving would write the file
  without the missing stages, and a stage-numbered MIDI binding now
  counts a shorter chain.

  So **saving is refused while a board is degraded**, and the button is
  disabled with the reason on hover. A preset file is the only record
  that a pedal was ever in the chain; a "Save" that deletes it because
  the plugin happened to be uninstalled that day is the kind of loss you
  find out about a week later. The refusal is sticky for as long as that
  file is loaded — moving or adding a stage afterwards doesn't clear it,
  because the missing stage is still in the file.

  A preset where *every* plugin is missing still fails. There is no
  chain left, and degrading to silence would be a worse lie than
  refusing.

  Verified: (1) New `missing_plugin` ctest doing literally what the
  acceptance criterion says — the same two-stage chain built twice, once
  intact and once with the *first* stage's URI edited to garbage (first,
  so every survivor shifts). It compares output hashes: the degraded
  board must be bit-identical to the surviving stage on its own, with
  the surviving stage's own gain, which rules out silence, a doubled
  stage, and the wrong controls alike. Also asserts the skipped stage is
  named with its file position, that an intact preset reports none, and
  that an all-missing preset fails with a reason. Validated by injecting
  two regressions: restoring state by chain index instead of file index
  (`degraded chain is not the chain of what was left`, hashes differing)
  and disabling the skip entirely (the run fails outright).
  (2) `ui_smoke` grew a degraded-board phase: the warning banner, and a
  save button that is disabled, refuses the click, and comes back when
  the board is whole again — validated by removing the guard
  (`saving was still offered while stages were missing`).
- [x] **SF-32**: Persist window size/layout and "last loaded preset"
  across restarts.

  `~/.config/signalforge/settings.json` (`$XDG_CONFIG_HOME` honoured,
  `--settings <file>` overrides), holding the window size, the last
  preset, and SF-33's crossfade values. Deliberately *not* the preset
  format: a preset is a sound that gets copied between machines and
  checked into a repo, this is one machine's habits, and mixing them
  would mean copying a preset also moved somebody's window around.

  With no preset named, `--run-preset` resumes the last one played —
  `./scripts/run.sh --gui` is now a complete command line. A remembered
  preset that has since been renamed or deleted warns and starts at the
  first one instead; a board that refused to start because of a stale
  settings file would be worse than useless. Naming a preset explicitly
  still wins.

  **Written on every switch, not only at exit.** A pedalboard is
  switched off at the wall, not quit, and "the preset you were playing"
  is worth a few hundred bytes per stomp to survive that. Which preset
  is stored by *stem*, not path, because presets move between
  directories with `--preset-dir` and the name is what survives it.

  Window **size** only: under Wayland a client cannot place its own
  window, so a remembered position is a value that silently does nothing
  on half the machines this runs on. There is no other layout to keep —
  no dockable panels, no movable sub-windows — so there is still no
  `imgui.ini`, and the one piece of state worth having lives in our own
  file where it can be read and edited.

  Every read is optional and every value clamped: a settings file must
  never be the reason a board won't play. A missing file is a first run;
  a truncated one (a power cut mid-write) is reported and the defaults
  used; wrong types, unknown keys and an unknown curve name each cost
  only the value they carried.

  Verified: (1) New `settings` ctest — round-trip through a directory
  that doesn't exist yet, then every way a file can be wrong: missing,
  empty path, truncated, every value the wrong type, partial,
  commented, and out of range in both directions. Validated by injecting
  two regressions (dropping the clamp, and treating a parse error as
  success), each caught. (2) New `resume_preset` ctest running the real
  binary end to end against a JACK server that deliberately doesn't
  exist — preset resolution happens before anything is opened, so this
  needs no audio server and no plugins: first run picks the first
  preset, a remembered one is resumed, an explicit argument overrides
  it, a deleted one falls back with a warning, and the crossfade
  settings are read from the file and overridden by the flags.
  Validated by making the resume path ignore the stored preset (`did not
  resume the preset it was told was last played`).
  (3) Live over real JACK (PipeWire), which is the only place the write
  half happens — settings are written at shutdown, and the automated run
  above never gets that far: started with no preset named, stomped
  `next`, quit, and confirmed `last_preset` had followed the switch; then
  restarted with no preset and watched it resume that one.

  Not covered automatically: that the *window size* written at shutdown
  is the size the window had. It needs a real window manager to resize
  a window, which is the same live-GUI gap as SF-28 through SF-30. One
  live pass is worth it: resize, quit, restart, confirm.
- [x] **SF-33**: Crossfade duration/curve tuning pass (by ear), expose as
  a config value.

  The fade is now its own thing — `src/audio/Crossfade` — rather than
  arithmetic buried in the JACK callback. It knows nothing about JACK,
  plugins or chains, which is what lets the shape of a switch be
  *measured* rather than only heard. `JackEngine` owns one and drives it
  a block at a time.

  Duration and curve are settings (`crossfade.milliseconds` /
  `crossfade.curve`, or `--crossfade-ms` / `--crossfade-curve`).
  Defaults unchanged at 12ms equal-power, now for stated reasons rather
  than by inheritance.

  **Why equal-power is the default, and why linear is offered at all.**
  Two presets are two different sounds — a clean amp and a fuzz share
  almost nothing — so they add in power, not in amplitude, and gains
  that follow cos/sin hold the loudness steady. A linear fade drops 3dB
  in the middle of every switch: an audible hole. The mirror case is
  real too — two takes of nearly the same sound add coherently, where
  equal-power bulges 3dB instead — so linear is a setting, not a
  mistake. Neither curve is zero in both columns; that is the trade, and
  which side of it is right depends on the pedals, not on the code.

  `--tune-crossfade` prints the table the default was chosen from: for
  each candidate length and both curves, the largest gain change between
  adjacent samples (the click risk), and how far the fade strays from
  constant power and from constant sum. At 12ms/48kHz the worst
  per-sample step is 0.0027 — two orders of magnitude below anything
  audible as an edge — while the fade is still short enough that a stomp
  lands on the beat.

  Verified: new `crossfade` ctest — both curves at their endpoints and
  across a thousand positions (equal-power holds power to 1e-5 and
  bulges to 1.414 in sum; linear does the opposite, and the 0.5 power
  hole is asserted rather than assumed); clamping of positions outside
  0..1; that 12ms at 48kHz is 576 samples and the ramp *continues*
  across block boundaries rather than restarting per block (the bug that
  would hold the outgoing chain at full gain forever, and that a test
  looking only at the first and last sample would miss); that block size
  doesn't change the ramp; that a zero, negative or NaN length still
  produces real audio and completes; that every channel of a block walks
  the same ramp from the same place, and that mixing doesn't advance the
  fade (or a stereo switch would run twice as fast as a mono one). Plus
  a `crossfade_tuning` ctest that keeps the table honest. Validated by
  injecting two regressions — mixing advancing the shared position, and
  equal-power implemented as a linear ramp — each caught, the second by
  the power assertions specifically.

  Also run live over real JACK (PipeWire), since the ramp being right
  says nothing about `JackEngine` driving it correctly: switched back and
  forth between two presets at 20ms and confirmed each fade *completed*
  on the audio thread rather than falling back to the "timed out waiting
  for the crossfade" swap. And a flag you tried does not become
  permanent: `--crossfade-ms 5 --crossfade-curve linear` ran at 5ms
  linear and left the file's own 12ms equal-power untouched.

  **The by-ear half is not done, and can't be by me.** The numbers say
  12ms equal-power is where to start; whether a stomp between two of
  *your* presets feels immediate at 12ms, or wants 8, or 20, is a
  judgement about your board that needs your ears. That is exactly why
  it is a setting now: `--crossfade-ms 8` and stomp, then keep what you
  like in the settings file.

## Milestone 8 — Audio device configuration

Today `JackEngine::Open()` just connects as a plain JACK client to whatever
server is already running (started externally via `jackd`/`pipewire-jack`);
device, driver, sample rate and buffer size are entirely out of the app's
hands. This milestone changes that: on a machine with no server running,
SignalForge starts and owns its own `jackd`, with the driver/device/rate/
buffer chosen through a config UI. A native (no-JACK) ALSA backend is
explicitly deferred to a later milestone — everything here still goes
through JACK, just a JACK the app can configure and launch itself.

- [x] **SF-34**: Audio backend config data model + enumeration.
  `src/audio/AudioConfig.h/.cpp`: `AudioConfig` (driver, device, sample
  rate, buffer size, input/output port filters, `configured` flag) with
  JSON (de)serialization and clamping; `EnumerateAlsaDevices()` (parses
  `/proc/asound/cards`); `IsJackServerRunning()` (a side-effect-free
  `jack_client_open(..., JackNoStartServer, ...)` probe). `--list-audio-devices`
  prints both. Covered by the `audio_config` ctest (clamping, JSON
  round-trip and tolerance of wrong-typed/unknown fields, and enumeration
  sanity on whatever machine it runs on — this repo's dev box reports 4
  ALSA cards).
- [x] **SF-35**: JACK server lifecycle manager. `src/jack/JackServer.h/.cpp`:
  `EnsureRunning()` is a no-op (`OwnsServer() == false`) when a server is
  already reachable; otherwise forks/execs `jackd -R -d alsa [-d <device>]
  -r <rate> -p <buffer>` and polls until it accepts clients or a timeout
  elapses. `Stop()` only ever touches a server *we* started — SIGTERM, then
  SIGKILL after a grace period — and is a no-op otherwise, including
  implicitly via the destructor. Orphan prevention on a crash uses
  `PR_SET_PDEATHSIG` in the child, belt-and-braces alongside the clean-exit
  `Stop()` call.
  **`JACK_NO_START_SERVER` is honoured**, not just left to libjack's own
  auto-start: without this, setting it to stop libjack auto-starting a
  server would do nothing, since we'd start one ourselves right after.
  This is also what keeps the `resume_preset` ctest (which points at a
  JACK server that deliberately doesn't exist, with that variable set, to
  fail fast without touching real audio) from spawning a real `jackd`
  during `ctest`.
  Covered by the `jack_server` ctest: `Stop()` without a start is a no-op;
  `driver: "external"` refuses to spawn when no server is actually up;
  a missing/failing `jackd` fails cleanly rather than hanging (this dev
  box has no `jackd` binary at all — `pipewire-jack` provides the JACK API
  without it — so this is the path exercised here); and
  `JACK_NO_START_SERVER` is honoured. The happy path — a real `jackd`
  against a real card — needs a machine with `jackd` installed and is not
  covered by ctest here.
- [x] **SF-36**: First-run detection + blocking config step, in
  `ResolveAudioConfig()`. `settings.audio.configured == false` (first run,
  or after `--reconfigure-audio`) is what triggers it; a server already
  reachable short-circuits straight to `driver: "external"` with no
  question asked. Otherwise: `ResolveAudioConfigCli()` prompts on stdin,
  but only when one is attached (`isatty`) — a non-interactive run (a
  script, a systemd unit, `ctest`) gets the defaults and a one-line notice
  instead of hanging forever waiting for input that will never come.
  `--gui` instead opens a small standalone window (`ResolveAudioConfigGui`,
  reusing SF-38's dialog) *before* the real board window exists, since the
  board only opens once the engine is already up — confirmed live: with no
  server reachable and `--gui`, the process blocks on the dialog exactly as
  intended (verified by timeout, since nothing here can click a real
  button) rather than proceeding or crashing.
  Runs before `JackServer`/`JackEngine` touch anything, so a config mistake
  is caught before any audio device is opened.
- [x] **SF-37**: App menu bar, in `BoardView`'s single fullscreen window
  (`ImGuiWindowFlags_MenuBar`, a "Settings" menu with one item so far).
  `BoardViewState::audio_settings_requested` is how the view reports the
  click, the same borrow-a-flag pattern as every other request in that
  struct — the view still doesn't know what an audio device is.
- [x] **SF-38**: Audio Settings dialog. `src/ui/AudioSettingsView.h/.cpp`
  (`DrawAudioSettings`), reused three ways: SF-36's first-run window, the
  menu-triggered floating dialog over a running board, and the same struct
  either drives. Applying calls `restart_audio_engine()` in `main.cpp`.
  **Honest about the one thing it can't hide**: changing device/rate/buffer
  means restarting the JACK server itself when we manage it, which takes
  every client down, ours included — there is no crossfade-style trick
  available here, unlike a preset switch. What *is* guarded against is a
  *failed* change leaving the board silent afterward:
  `restart_audio_engine` tries the requested config first and, if it
  doesn't come up clean (bad device, `jackd` failure, chain rebuild
  failure), falls back to reopening the config that was working before, so
  "the new setting didn't work" doesn't compound into "and now nothing
  plays" — only a config that was *also* broken in the meantime (e.g. the
  previously-working device unplugged mid-restart) ends in a stopped-audio
  state, reported in the dialog rather than crashing.
  A same-sample-rate change (buffer size, device) reuses the running
  `Chain` as-is; a sample-rate change rebuilds it via
  `BuildChainFromPreset` at the new rate, since an LV2 plugin instance is
  bound to the rate it was instantiated with. Fixed a real bug found while
  writing this: the fallback attempt originally compared against the rate
  *at the start of the whole call*, not the rate the (possibly already
  rebuilt) `chain` variable actually held at that point — a forward attempt
  that rebuilt the chain and only then failed at `Start()` would have left
  the fallback reusing a chain built for the wrong rate. Fixed by tracking
  `chain_sample_rate` as its own variable, updated exactly when `chain` is.
  `JackEngine::SampleRate()`/`MaxBlockSize()` now return 0 rather than
  crash when called on a closed engine (`client_ == nullptr`), which the
  double-failure path above can leave outstanding until the user tries
  again — same "never the reason a board won't play" posture as `Settings`.
  Not covered end-to-end for the same reason as SF-28/29/30: real GUI
  clicking needs the KDE input-injection permission that's the user's to
  grant, not mine. What's verified is that everything compiles and runs
  clean, the full ctest suite (21 tests) stays green with the wiring in
  place, and the CLI/first-run halves of the same code paths behave live
  (above).
- [x] **SF-39**: Physical audio input/output port selection.
  `JackEngine::ConnectAudioPorts()` mirrors `ConnectMidiInputs` for audio:
  hardware capture ports (JACK outputs) feed our `in_N` ports, our `out_N`
  (mirror included) feed hardware playback ports (JACK inputs), matched by
  position after sorting by name and bounded by whichever side has fewer —
  a two-in board on a four-in interface gets the first two capture ports,
  not all four. An empty filter (`AudioConfig::input_ports`/`output_ports`)
  matches every physical port of the right direction, same convention as
  SF-20's MIDI filter; a named one is a case-insensitive substring match
  and reaches non-physical ports too. Called after `engine.Start()` (that's
  where `in_N`/`out_N` actually get registered) in both the initial run and
  every `restart_audio_engine` call.

  **Three real bugs found live, on real hardware (a Scarlett Solo and a
  Razer mic both present), none of them caught by ctest since all three
  need an actual multi-device JACK graph to manifest:**
  (1) `JackServer::EnsureRunning` short-circuits to "do nothing" whenever a
  server is already reachable — correct for not stomping on a server we
  don't own, but on a PipeWire machine (no standalone `jackd` process, so
  "already reachable" is always true) it meant `AudioConfig::device` was
  silently never used at all. Device selection only ever took effect on
  the machine where nobody needed it to.
  (2) With an empty port filter, `ConnectAudioPorts` grabs the first N
  physical ports sorted alphabetically — with two interfaces in the same
  graph, whichever sorted first won, independent of anything the dialog
  showed as selected. Between the two bugs, picking "Scarlett" in Audio
  Settings had no effect and the board connected to the Razer mic instead.
  Fixed by adding an explicit, always-visible, user-editable **port name
  filter** field to the dialog (`AudioSettingsViewState::port_filter`,
  same case-insensitive-substring convention as SF-20's MIDI device
  filter) rather than trying to guess PipeWire's exact port-naming scheme
  and trust it blindly — picking an ALSA device fills the field with a
  guess (`GuessPortFilter`, the text after the card's " - " separator),
  but the field itself is what's actually applied, so a wrong guess is
  correctable by hand.
  **Improved further**: rather than lean on that guess at all when it can
  be avoided, `AudioConfig::EnumeratePhysicalPortClients()` asks the
  reachable server directly for every distinct client name behind a
  physical port (grouped, with capture/playback counts) and the dialog
  offers those in a second, authoritative dropdown ("Detected hardware")
  that fills the same filter field with the server's own name for the
  hardware, not a guess. Only available once a server exists to ask —
  during SF-36's first-run step, before one does, the ALSA-based guess is
  still what's on offer, same as before. Covered by the
  `TestEnumeratePhysicalPortClients` case in the `audio_config` ctest
  (well-formed entries, and that a server known unreachable yields an
  empty list rather than stale/fabricated data).
  Sample rate/buffer fields are now
  hidden only when a server is already running (they're genuinely not
  ours to set there); the device/filter controls stay visible either way,
  since picking ports is a client-side operation independent of who owns
  the server.
  **And split in two, which was the remaining half of the same bug.** One
  shared filter drove both directions, so the input and the output could
  never be different hardware — but a guitar in a Scarlett with the sound
  going out to headphones on the motherboard codec is the ordinary desk,
  not an exotic one. `AudioSettingsViewState` now carries `input_filter`
  and `output_filter` separately, each with its own dropdown built by
  `DrawPortSelector` from the live enumeration and filtered to devices
  that actually have ports in that direction (a playback-only sink is
  never offered as an input, since choosing it could only fail), plus its
  own text field for the two cases a dropdown can't serve: hardware that
  isn't plugged in right now, and first-run setup where no server exists
  to enumerate. `AudioConfig` already stored the two independently, so
  persistence needed no change; the `audio_config` ctest round-trips them
  with deliberately *different* values, which is what keeps them from
  being quietly conflated again.
  Applying a device change no longer restarts anything: when only the
  filters differ (same card, rate and buffer), the apply path calls the
  new `JackEngine::DisconnectAudioPorts()` and re-runs
  `ConnectAudioPorts()`, since re-pointing the board at other hardware is
  a change of patching, not of the engine or the server. The full restart
  is kept for changes that genuinely touch the server's own settings.

  **Then made per-port rather than per-device, and the text fields
  dropped.** Enumeration returns individual ports (`PhysicalPort` /
  `EnumeratePhysicalPorts`) instead of devices with port counts: the chain
  is mono, so *which* input of an interface the guitar is in is precisely
  the choice being made, and a Scarlett Solo shown as one row reading "2
  inputs" hides the only thing worth picking. The free-text filter fields
  went with it -- a dropdown that can express everything worth choosing
  makes a second way to say the same thing just a second thing to get out
  of sync.
  Dropping to ports alone would have broken output, though, and this is
  worth stating because it was nearly shipped: the board feeds a stereo
  pair (`out_1`/`out_2`), so an exact single playback port would have put
  the sound in one ear. So each dropdown offers **two granularities** --
  a whole device ("all N ports", what an output wants) and each individual
  port (what a mono input wants) -- rather than guessing per direction. A
  device with one port in that direction gets one row, not two identical
  ones.
  A saved port whose hardware isn't plugged in today is preserved rather
  than silently reset: shown as the selection, offered back as a row
  marked "not connected", so unplugging an interface for an evening
  doesn't quietly rewrite the setting.
  Verified by `ui_smoke`, which gained widget probes for this dialog
  (`AudioSettingsViewState::record_widgets`, the same affordance
  `BoardViewState` already had) because the rows only exist while the
  dropdown is *open* -- reaching them needs a real click, not a redraw.
  **The first version of this test was worthless and the injection pass is
  what caught it**: it only drew the closed combo, so the dropdown body
  never ran, and a deliberately planted "silently reset the saved value"
  bug passed clean. Opening a popup also needs settle frames before its
  row coordinates mean anything -- aiming at the opening frame's positions
  lands outside the popup and dismisses it. With both fixed, three
  injected regressions are each caught: dropping an unplugged device's
  saved port, offering playback ports as inputs, and conflating the input
  choice into the output (the original bug this whole thread started
  from).
  (3) A live-restart applying via the menu recorded `settings.audio =
  config` verbatim, which always carried `driver: "alsa"` — even on the
  common short-circuit path where no server was actually started by us.
  The dialog's "already running" banner separately (and wrongly) checked
  `driver == "external"` to decide what to show. Together these produced
  an infinite loop: apply successfully attaches to the already-running
  server, mislabels it "alsa", and the *next* time Settings is opened the
  stale label makes the dialog claim nothing is running and prompt to
  start a server that's already up. Fixed by keying both the display and
  the record on `JackServer::OwnsServer()` — the actual fact of who
  started it — instead of a copied config value.
- [x] **SF-40**: Persist audio config in settings.json (extends SF-32).
  `Settings::audio` (an `AudioConfig`) round-trips through `ToJson`/
  `FromJson` and is clamped by `AudioConfig::Clamp()` on load, same
  wrong-type/missing-key tolerance as the rest of `Settings`. `--reconfigure-audio`
  clears `configured` for one run without hand-editing the file, the CLI
  equivalent of opening the menu's Audio Settings.
  Verified live: `--list-audio-devices` reports this dev box's 4 ALSA
  cards; `--run-preset` with no `jackd` installed here fails cleanly with
  "jackd exited immediately" rather than hanging, and writes nothing to
  the settings file (consistent with settings only being saved at a
  successful shutdown, same as before this milestone); the full ctest
  suite passes (21/21, one pre-existing skip needing
  `-DSIGNALFORGE_TEST_MODEL`) with the new `audio_config` and
  `jack_server` tests included, and `resume_preset` — which deliberately
  points at a JACK server that cannot exist — still passes unmodified,
  confirming `JACK_NO_START_SERVER` keeps `ctest` from ever spawning a
  real `jackd`.
  **Not verified live, stated plainly**: this dev machine has no `jackd`
  binary (PipeWire provides the JACK API directly), so the actual
  spawn-a-server-and-play-audio happy path of SF-35/36/38 is exercised
  only by code inspection and the failure-path tests above, not by an
  end-to-end run against real hardware. Worth one live pass on a machine
  with `jackd` installed: cold start with no preset, first-run dialog,
  device/rate/buffer choice, audio out; then Audio Settings mid-session to
  change buffer size and confirm playback survives it.

- [x] **SF-41**: Level meters and latency in the engine panel; buffer size
  adjustable live.

  **Metering.** `JackEngine` measures peak magnitude on the audio thread --
  inputs before the chain (so it says whether the *guitar* is too hot,
  independently of what the plugins do with it) and outputs last of all, on
  the buffers actually going to the interface, after any crossfade mix,
  since a switch is exactly when two chains summing can push into clipping.
  Peaks are published as "loudest since you last looked", reset by the
  read: the audio thread runs ~375 blocks/s against a 60Hz redraw, so a
  plain snapshot would show one arbitrary block in six and miss the
  transient that matters. Written with a relaxed CAS loop rather than a
  lock -- one writer, a reader that only ever swaps in zero, and nothing
  that can block the audio thread.
  Clipping is a monotonic count of full-scale blocks rather than a flag,
  so the reader can tell "clipping now" from "clipped a moment ago" by
  watching it change, and how long a warning stays up (1.5s) is decided in
  the UI where it belongs rather than in the engine. Full scale is `>= 1.0`,
  not `> 1.0`: a sample sitting exactly at 1.0 is already the top of the
  range.
  The bar itself has ballistics -- rises instantly, falls gradually --
  because a bar driven straight off a read-and-reset peak flickers, showing
  a transient for one frame and then nothing.

  **Latency replaces the audio-cycle counter**, as asked. The cycle count
  was SF-24's proof that audio isn't on the UI thread; it is still there in
  `ProcessCycleCount()` and the `status` command, it just isn't what a
  player wants in the panel. `RoundTripLatencyMs()` asks JACK
  (`jack_port_get_latency_range`, capture on our input plus playback on our
  output), so it accounts for the interface's own buffering and anything
  patched around us rather than being computed from the period. Unconnected
  ports report zero, which would read as "no latency at all", so that falls
  back to one period -- the floor no client can beat, stated as such.

  **Buffer size is now changeable from Audio Settings even on a server we
  don't own**, because JACK supports it: `jack_set_buffer_size` changes the
  period live. Sample rate deliberately stays behind the "we started this
  server" condition, since the driver is opened at a rate and keeps it --
  that asymmetry is real, not an oversight.
  **The ordering here is the whole risk and is worth stating.** The new
  period takes effect on the audio thread the moment the server agrees,
  while the chain currently running is still the one processing it -- and
  an LV2 plugin handed more frames than it was instantiated for writes past
  the end of its buffers. So a chain whose maximum block covers *both* the
  old and new size is built and crossfaded in first, and only then is the
  size actually changed. A server that refuses the size leaves that
  oversized chain in place, which is harmless: a maximum block is a
  maximum, not a promise to fill it. `JackEngine` also grew a
  buffer-size callback so the crossfade scratch is resized (legal to
  allocate there -- JACK stops processing around it) instead of being
  overflowed by the next fade.

  Verified by `ui_smoke`, and **the meter test is worth recording as a
  near-miss**. The first version only *drew* the meter at silence, past
  full scale and NaN, and asserted nothing -- which caught nothing, because
  Dear ImGui saturates a progress bar's fraction internally, so a mapping
  returning 12.0 or NaN still looked fine. Injecting the obvious bug
  (dropping the non-finite guard) passed clean. Fixed by extracting the
  scale mapping into a pure, inline, directly assertable `MeterFraction`
  and checking eight inputs including both infinities and NaN; two injected
  regressions -- dropping the non-finite guard and dropping the clamp --
  are each now caught. The drawing pass is kept alongside it, since the
  bar's overlay text and colour choice read the level too.

  Not covered end-to-end, same standing limitation as SF-28 through SF-30
  and SF-38: that a real signal moves the real meter, and that a live
  buffer-size change on a real server keeps audio running, both need
  hardware and a real window. Worth one live pass: play, watch the input
  meter track; push a gain stage until the output goes red; then change
  buffer size from the menu and confirm the latency figure moves and the
  sound survives it.

## Milestone 9 — Graph view, routing, and stereo

Supersedes the two-line sketch this milestone used to hold (SF-43/SF-44,
folded into SF-45 and SF-53 below). The ask grew: not just "one split
somewhere in the list", but a **graph view of the signal chain** — the
horizontal signal path from an AmpliTube-style board, where the chain is
drawn left to right as tiles on a wire, a split fans the wire into two
stacked lanes that rejoin at a mixer, and the two ends of the wire are the
board's input and output.

Two things this is explicitly **not**:

- **Not drag and drop.** Nodes are placed by structure, not by the mouse.
  Every edit is a button: `+` at an insertion point, `×` on a tile, "split
  here", "rejoin". Layout is computed, never stored — there are no
  coordinates in a preset file. This keeps the whole view a pure function
  of the chain, which is what makes it testable and what keeps a preset
  diffable.
- **Not a free patch bay.** No arbitrary node-to-node cables, no cycles.
  The topology is a tree: series of nodes, where a node may be a plugin or
  a split into two branches that merge back. Everything drawable is
  therefore also buildable and always has a defined signal order.

The other half of the ask — "route the inputs/outputs" and "plug 2in/2out
effects" — is really about **channels**, and it is the part with actual
audio risk in it. Today `Chain` is mono end-to-end and refuses any
channel-count mismatch between neighbours (`Chain.h`, SF-9). Stereo pedals,
splits, and merges all break that assumption at once, so the engine work
below lands *before* any of the drawing work.

**PLAN.md needs amending alongside this** (§2 non-goals, §4.2, §5.1, §7
channel adaptation) — that's SF-59, deliberately last, so it records what
was actually built rather than what was hoped for.

### Phase A — data model (no behavior change)

- [x] **SF-45**: Chain graph node tree. `src/preset/ChainGraph.{h,cpp}`:
  `Node` = one of `Plugin` (today's `PresetStage`), `Split` (exactly two
  branches, each an ordered list of nodes, plus a merge description), where
  a branch may itself contain a `Split` — recursion falls out of the shape
  for free, and the UI, not the model, is what limits depth. Merge carries
  per-branch gain in dB and a mute flag; the merge itself is a sum. Every
  node gets a **stable id** (monotonic `uint32`, allocated on insert, never
  reused within a preset) — indices shift on every edit and MIDI bindings,
  the control panel, and the selection all need to survive that.
  Plus `Flatten()` (graph → signal-ordered node list) and `FindById()`.
  *Acceptance: unit tests build a nested graph, flatten it, and round-trip
  ids through insert/remove.*

  Landed as written, with **one change of mind that is worth stating,
  because the plan said the opposite: an empty branch is legal.** It was to
  be rejected at load. But an empty branch is a straight wire, and a dry
  wire in parallel with a pedal is the main reason to reach for a split at
  all — parallel compression and any wet/dry blend are exactly that shape.
  Rejecting it would have forced a placeholder plugin into every dry path,
  which is a worse model rather than a stricter one. A split with *both*
  branches empty is also legal and is simply two wires; it's what you get
  the moment you press "split" and before you put anything in either lane,
  so refusing it would mean the editor couldn't build one.

  `PresetStage` moved to its own header (`preset/PresetStage.h`) so that
  `Preset` can hold a `ChainGraph` and `ChainGraph` can hold stages without
  the two headers including each other.

  The flat index — trunk in order, and at a split all of branch 0 then all
  of branch 1 before the trunk resumes — is now the *definition* of a
  "stage index", and `NodeAtFlatIndex`/`FlatIndexOf` are the only places
  the index world and the identity world meet.
- [x] **SF-46**: Preset JSON v2. Read: a top-level `"graph"` if present,
  else today's flat `"chain"` array (which loads as a linear graph — every
  existing preset in `presets/` keeps working untouched). Write: **flat
  `"chain"` when the graph is linear**, `"graph"` only when it isn't, so a
  linear preset's file doesn't churn just because the loader grew. Strict
  errors as everywhere else (SF-31 style): a split with other than two
  branches or a duplicate node id is rejected at load with the offending
  path (an *empty* branch is legal — see SF-45). *Acceptance: every file in
  `presets/` loads, saves, and is byte-identical; a hand-written split
  preset round-trips.*

  Byte-identity verified against all three files in `presets/` through
  `--preset-roundtrip`. It needed one rule the plan hadn't thought of:
  **the flat form writes an `id` per entry only once the ids stop being
  positional.** A preset that has never been edited has ids 1..N in order,
  which is exactly what the loader would assign anyway, so they are left
  out and the file doesn't churn. The moment an insert or a delete shuffles
  them they start being written — otherwise the node a selection or a
  binding names would silently become a different pedal on the next load.

  Both forms in one file is a load error rather than a precedence rule:
  which one is the board would be a coin toss, and the two would drift
  apart the first time either was hand-edited.
- [x] **SF-47**: ~~MIDI bindings target node ids.~~ **Deliberately not
  done — the plan was wrong to ask for it.** A binding lives in a *global*
  mapping file that outlives any one preset: "CC 64 bypasses stage 1" means
  stage 1 of whatever preset is loaded. Node ids are per-preset, so a
  binding carrying one would mean nothing in every other preset in the
  library. That's a regression dressed as a fix, and the acceptance
  criterion it was written against ("a binding survives inserting a stage
  ahead of its target") describes the wrong behaviour for a global map —
  the player bound "bypass the second pedal", and it should stay that.

  So the persisted meaning stays positional, `MidiMap` is untouched, and
  the flat index keeps meaning what it always did. What the graph actually
  needs — a stable handle for the UI's selection and for an edit in flight
  — is served by node ids *in session only*, with `NodeAtFlatIndex` /
  `FlatIndexOf` translating at the boundary. `apply_stage_edit` in
  `main.cpp` is the first user: it takes the view's flat index, resolves it
  to an id once, and edits by identity from there.

  One pre-existing wrinkle this leaves standing, for SF-57 to deal with:
  the flat index the UI and MIDI speak is a position in the *built chain*,
  while `ChainBuildReport::skipped_indices` are positions in the *preset*.
  They differ whenever a plugin is missing. That is already how it is; it
  just gets easier to confuse once there are lanes.

**Also done in Phase A, as fallout rather than as its own task:**
`Preset::chain` no longer exists — `Preset::graph` replaced it, and every
caller (`main.cpp`, `ChainPreset.cpp`, the console and CLI paths) now goes
through `PluginCount()`/`PluginAt()`/`Flatten()`. For the duration of Phase A
`BuildChainFromPreset` *refused* a preset containing a split rather than
flattening it — `Chain` ran a series and nothing else, and a split played as
a series is not a near miss but every parallel path stacked end to end. Phase
B replaced that refusal with an engine that can actually run one; the
translation from graph to `ChainSpec` now lives in the same function. Covered by
`tests/ChainGraphTest.cpp` (`ctest -R chain_graph`, 69 checks); the
signal-order assertions were checked against an injected regression
(recursing into the two lanes in the wrong order), which they catch.

### Phase B — engine (the part with audio risk)

- [x] **SF-48**: Channel-aware links and implicit adapters. Replace
  `Chain`'s equal-count requirement with a per-link channel count resolved
  at build time: a link carries 1 or 2 channels, a stage declares what it
  wants, and the builder inserts an adapter when they differ — mono→stereo
  duplicates, stereo→mono sums at −6 dB (two correlated copies of the same
  signal sum to +6, and duplicating then re-summing must be unity, not
  double). Adapters are engine-internal; they never appear in a preset and
  never appear as a tile. *Acceptance: a mono amp into a stereo chorus into
  a mono cab builds and passes signal; a unit test asserts mono→stereo→mono
  is unity gain, not +6 dB.*

  Done, with the adapters living in a new **pure planner**
  (`lv2/ChainSchedule.{h,cpp}`) rather than inside `Chain`. That split is the
  main structural decision of Phase B and it is what made everything after it
  testable: the planner takes a shape plus each stage's channel counts and
  latency *as numbers*, and returns ops and buffers. It never touches lilv,
  so what a split means is assertable with no plugins installed and no audio
  server running.

  Two refusals were added that the plan didn't ask for, both because the
  alternative is a board that silently isn't what it claims. A plugin wider
  than stereo on either side is refused *at the plugin*, not only where an
  adapter happens to be needed — a three-output plugin last in the chain
  would otherwise sail through and hand the board three outputs. A plugin
  with no audio output at all is refused rather than leaving the signal with
  nowhere to go. A plugin with no audio *inputs* is fine, and is treated as a
  generator: it replaces the signal rather than processing it.
- [x] **SF-49**: Split/merge execution. A split copies its input to both
  branches; each branch runs into its own buffers; the merge sums them with
  the per-branch gains. Buffers come from a **pool sized at build time**
  with lifetime analysis (a branch's buffer is free again after the merge),
  because one `vector` per link at depth is a lot of memory to touch per
  block for no reason. *Acceptance: a split with one branch muted is
  sample-identical to the other branch alone; both branches at 0 dB sum as
  expected.*

  **The buffer pool is simpler than planned, deliberately.** The plan called
  for lifetime analysis so a lane's buffer could be reused after the merge.
  What landed allocates one buffer per link, which is what `link_buffers_`
  already did — for a board of a dozen pedals that is a few dozen blocks of
  float, and the analysis would have bought memory nobody is short of at the
  cost of the one part of this that has to be obviously correct. Worth
  revisiting only if a board gets big enough to notice.

  Both lanes are planned to read the *same* input buses, with no copy. That
  is safe because an LV2 plugin never writes to its input buffers — it writes
  to the fresh output buses allocated for it — so nothing lane 0 does can
  disturb what lane 1 reads. If that assumption ever breaks, this is the line
  it breaks at.

  One consequence worth knowing before it surprises someone: **two dry wires
  in parallel sum to twice the signal.** That is arithmetic rather than a bug
  — two unity paths carrying the same samples add — but it means a split with
  nothing in either lane is a +6 dB jump. Per-lane gain is where to fix it on
  a given board; whether the *editor* should default a new split's lanes to
  −6 dB is a Phase C question (SF-55).
- [x] **SF-50**: Flattened RT schedule. The tree is compiled on the control
  thread into a linear op list (`RunStage`, `Copy`, `MixInto`, `Adapt`)
  that `Chain::Run` walks with no recursion, no allocation, no branching on
  node type beyond a switch. This is what keeps a graph as RT-safe as
  today's list. *Acceptance: existing engine tests still pass; a schedule
  for a nested graph is asserted op-by-op in a unit test.*

  `Chain::Run` is now a loop over a `std::vector<ChainOp>` and a switch on a
  five-value enum. The tree is gone by the time the audio thread sees it.

  The one piece of per-cycle work that isn't an op: a bus at the board's edge
  is repointed when JACK hands out its buffer for that cycle, and any plugin
  port sitting on that bus has to follow it. That list is precomputed at
  build time, and nothing on the path allocates.
- [x] **SF-51**: Latency compensation across a split. Two branches of
  unequal latency comb-filter when summed, and it is the single most likely
  "why does the split sound wrong" bug. Sum each branch's reported LV2
  `latency` port, delay the shorter branch by the difference. Report the
  per-branch figures so the UI can show them. *Acceptance: a split with a
  deliberately latent plugin on one side sums without cancellation; the
  compensation delay is asserted directly.*

  `PortInfo` learned to spot a latency port — both spellings, since plugins
  in the wild use both: `lv2:designation lv2:latency` and the older
  `lv2:reportsLatency` property. `PluginStage::LatencySamples()` reads it, and
  a negative, non-finite, or absent figure reads as zero rather than sizing a
  delay line from garbage.

  **What this does not do is follow a plugin that changes its latency while
  running.** The alignment delay is fixed when the chain is compiled, because
  re-planning mid-block is not something the audio thread can do; rebuilding
  the chain picks up the new figure. Worth stating because a lookahead
  limiter switching modes is a real case, not a hypothetical one.
- [x] **SF-52**: Stereo board boundary and input/output routing. The board
  itself becomes 1-or-2-in / 1-or-2-out; `JackEngine`'s existing
  mono→stereo mirror at the output (SF-9) becomes the degenerate case
  rather than the rule. Which hardware ports feed the input and which take
  the output becomes explicit, persisted in settings, and settable at
  runtime through the existing `ConnectAudioPorts`/`DisconnectAudioPorts`
  path. *Acceptance: a stereo-out board reaches both speakers with no
  mirror; changing the output device from the UI re-patches without a
  restart.*

  **Half of this turned out to be already built.** Choosing which physical
  ports feed the board, persisting that choice, and re-patching at runtime
  all landed in SF-39/40 — `AudioConfig::input_ports`/`output_ports`, the
  Audio Settings view, and `JackEngine::ConnectAudioPorts`. Rather than
  rebuild it, what was actually missing got added: **the board's width is now
  a choice rather than a consequence.** `AudioConfig::board_inputs` is 0
  (auto), 1, or 2; auto means "as many as the first pedal wants", which is
  what this always did and is right for a guitar. Without it, the width of
  the whole board is decided by whichever pedal happens to be first, which is
  a strange way for a stereo rig to be configured.

  The mono→stereo mirror at the output (SF-9) is now the degenerate case
  rather than the rule: a board that is genuinely stereo out reaches both
  speakers on its own. Nothing about the mirror needed changing — it was
  already written as a condition rather than an assumption.

**Phase B verification.** `chain_schedule_test` (43 checks) covers the
planner and the arithmetic with nothing installed: both adapters and their
levels, split ordering, lane gain and mute, alignment, the passthrough
aliasing case, and every refusal. `split_routing` covers the whole path
against a real LV2 plugin — a preset in the nested `graph` form, loaded from
disk, translated, planned, instantiated, and run through the offline engine —
and asserts that **a split with one lane muted is sample-for-sample identical
to that lane on its own** (equal output hashes), that both lanes live is not,
and that a dry lane in parallel is its own thing again.

Both were checked against injected regressions — summing a downmix instead of
halving it, and disabling lane alignment — and each is caught. The alignment
injection also caught a flaw in the test itself: it indexed a vector it had
only checked the size of on the line before, so a failure segfaulted instead
of naming the broken assertion. Fixed.

**Not covered, and it needs ears:** that a split *sounds* right through real
speakers, and that the alignment delay removes the comb filtering rather than
merely existing. The hashes prove the arithmetic is what was intended; only
playing through it proves the intent was right. Same standing limitation as
SF-28 through SF-30, SF-38 and SF-41.

### Phase C — the view

- [x] **SF-53**: Pure layout pass. `src/ui/GraphLayout.{h,cpp}`: graph →
  `{node id, column, lane, rect}` plus the wire segments between them, with
  no ImGui in the file at all. Column is signal position; a split occupies
  the columns its longest branch needs and its two branches take the lanes
  above and below the centre line. *Acceptance: layout of a nested graph is
  asserted as data — this is the piece that would otherwise only ever be
  checked by looking at it.*
- [x] **SF-54**: Draw it, read-only. `GraphView` renders the layout with
  `ImDrawList`: rounded tiles carrying the plugin name, wires as
  colour-coded polylines (branch A / branch B / trunk, matching the
  reference board's orange-and-blue reading), bypassed tiles dimmed with
  the wire passing straight through them, the selected tile outlined.
  Input and output endpoints drawn as the two ends of the wire.
  *Acceptance: `ui_smoke` renders a linear and a split graph without
  assertions firing.*
- [x] **SF-55**: Edit affordances. `+` at every insertion point (opens the
  existing plugin browser, inserts *there* rather than appending), `×` on
  every tile, "split here" on any insertion point on the trunk, "rejoin" on
  a split (which concatenates branch A then branch B into the trunk and
  says so, rather than silently discarding B), and ‹ › to move a tile
  within its lane or across into the sibling lane. One edit per frame, as
  `StageEdit` already enforces, since each costs a rebuild. *Acceptance:
  every edit is driven by name through the widget probes, not by
  coordinates.*

  One departure from the plan, for a reason worth keeping: **"split here"
  lives inside the browser that a "+" opens, not as a second button on the
  board.** A split goes exactly where a pedal goes, so it is offered where a
  pedal is offered; two buttons in every gap would double the clutter of the
  picture that is meant to be readable at a glance.

  Moving (‹ ›) went to the selected pedal's panel rather than onto the tile,
  for the same reason. Removing stayed on the tile — it is the edit most
  often made, and a panel is a step too many for it. The tile's "x" and the
  panel's are disabled together when only one pedal is left: two ways to do
  the same thing that disagree about whether it is allowed would be worse
  than either alone.

  "Rejoin" concatenates lane 0 then lane 1 into the trunk, flattening any
  nested split with them. No pedal is lost — a rejoin that kept one lane and
  dropped the other would silently delete pedals, and the lane it dropped
  would be a choice the user never made.

  Moving *across* lanes is still not done, and is the one thing on the board
  that still needs the file: a pedal in the wrong lane has to be removed and
  re-added on the other one. It wants a gesture that isn't ‹ ›.
- [x] **SF-56**: Routing UI on the endpoints. Clicking the input or output
  endpoint opens a small popup listing the available JACK ports (from
  SF-52) with mono/stereo choice. Per-branch gain/mute live on the merge
  node's tile. *Acceptance: routing changes survive a restart; a muted
  branch reads as muted on the wire, not just in a number.*

  The plugs open the audio settings the program already had rather than a
  second panel saying the same thing — there is one routing configuration,
  and the board's edge is a way *into* it. Their tooltips say what is
  currently patched, so the common case needs no click at all.

  Per-lane gain sits on the wire next to its mute, because two unity lanes in
  parallel are +6 dB and that is the control which makes a parallel blend
  usable. **It reports on mouse release, not while dragging**: a lane gain is
  compiled into the chain (SF-49), so every change costs a rebuild and a
  crossfade, and firing per frame would rebuild the board sixty times a
  second while the mouse moved. The number follows the cursor; the sound
  catches up when it is let go. Making merge gains live — atomics the Mix op
  reads, no rebuild at all — is the obvious follow-up if that lag grates.
- [x] **SF-57**: Selection drives the existing panel. Clicking a tile
  selects it; the control sliders, file picker, and MIDI-learn buttons that
  `BoardView` already draws per stage now draw for the selected node,
  keyed by node id. The old vertical stage list is **replaced**, not kept
  alongside — two ways to edit the same chain is how they drift apart.
  Console commands (`bypass <n>`, `status`) keep taking flattened indices.
  *Acceptance: `ui_smoke` covers select → move a control → learn a binding
  on a node inside a split branch.*

  The vertical stage list is gone, and its per-stage expander went with it:
  the panel shows the *selected* pedal, so "expand" and "collapse" stopped
  being things. Clicking a tile twice deliberately does not deselect — a
  selection that toggled would make the panel vanish under the second click
  of a double one.

  This needed a shape-preserving capture, and it is the part most likely to
  be got wrong by someone changing this later. `CaptureChainPreset` can only
  ever produce a *series*: a running `Chain` is a flat list of stages, and the
  lanes it was built from are not recoverable from it. Editing a board means
  keeping what the player changed by hand — knobs, bypasses, a loaded model —
  without flattening their splits, so `CaptureChainPresetInto` takes the
  source preset for the shape and the chain for the values. Saving goes
  through it too, and did not before: **a board with a split, saved, would
  have come back straightened out.**

**Phase C verification.** `graph_layout_test` (34 checks) covers the geometry
with no window: signal order with real gaps between tiles, lanes above and
below the trunk, an empty lane that still offers somewhere to click,
determinism, and no overlap at any nesting depth. Checked against an injected
regression — placing both lanes on the trunk line — which it catches four
ways.

`ui_smoke` now drives the board itself: selecting a tile, the "x" on a tile,
the "+" in a gap, the browser it opens and the split that can be inserted
there instead, the plugs at both ends, and — on a board with an actual split
— a lane mute, a rejoin, a "+" *inside* a lane carrying which lane it was,
and selecting a pedal that lives in a lane and reading its controls there.

Two things that test taught, both fixed rather than worked around: two
widgets had ended up answering to the same probe name (`remove:` from a tile
and from the panel), which makes a test click whichever it finds first; and a
step that opens a popup reports positions from the frame it opened in, while
Dear ImGui may still move that popup on its second frame, so aiming at where
it *was* missed. There is a settling frame between steps now.

**Not covered, and it needs eyes:** whether the picture is legible. Whether
the lane colours read as lanes, whether a nested split is comprehensible or
merely correct, whether the tiles are the right size on a real screen. Every
claim above is about structure and behaviour; none of them is about whether
it looks like a signal chain.

### Phase D — closing out

- [x] **SF-58**: Rebuild-on-edit through the existing crossfade path, and
  the degraded-build story (SF-31) extended to graphs: a missing plugin
  inside a branch drops that node, an entirely empty branch collapses the
  split back to a series, and the warning line says which. *Acceptance: a
  preset naming an uninstalled plugin inside a split still loads and
  plays.*

  The rebuild half was already done: every shape edit goes through
  `switch_to_preset`, which is the crossfaded path a preset switch uses, for
  the same reason -- the old chain keeps making sound while the new one is
  built. And a missing plugin inside a branch already dropped that node, since
  the translation walk is recursive and never cared which list it was in.

  What was missing is the case that only splits have. **A lane that lost
  everything it had is not the same as a lane that was authored empty**, and
  the difference matters more than it looks: an authored empty lane is a dry
  wire and the whole point of a wet/dry blend, while an emptied one would sum
  a dry copy against the surviving lane at +6 dB -- a tone nobody saved, from
  a board that looks like it loaded fine. So a split with an emptied lane
  collapses to a series, which is the board minus the pedal that is gone; a
  split with an authored empty lane is untouched.

  `missing_plugin` covers both, and the exact claim is the strong one: a
  collapsed split is sample-for-sample the surviving lane in series. An intact
  split and a dry-lane split are each asserted to sound like neither, or that
  check would pass in a build that collapsed every split it met. Collapsing
  authored dry lanes too was the injected regression.
- [x] **SF-59**: Update PLAN.md (§2 non-goals — the "no node editor / no
  parallel routing / mono-only" lines are no longer true and should say
  what replaced them, not just be deleted; §4.2; §5.1 format; §7 channel
  adaptation) and README's "no drag-and-drop graph editor" framing, which
  is still accurate about *drag and drop* and now wrong about *graph*.

### Decisions worth making before Phase A

These change the shape of the work, so they're better answered than
discovered:

1. **Nested splits: allowed or one level?** The model gets recursion for
   free; the *layout* is where it costs, since lanes have to nest. Cheapest
   honest answer: allow it in the model and cap the UI at one level of
   nesting for now, with the cap in one named constant so lifting it later
   is a one-line change plus a layout test.
2. **Merge = sum only, or sum + parallel-blend?** Sum is what a real
   rig does and is one line. A dry/wet blend across the split is the same
   thing expressed as gains, so per-branch gain (SF-45) covers it without a
   second concept.
3. **Does a split branch get its own input, or only the split's?** Only the
   split's, i.e. no side-chain inputs. Anything else stops being a tree.

## Milestone 10 — Look and feel

A high-fidelity design for the whole window now exists — `design/`, a
clickable HTML prototype plus a token set. It is a *reference*, not code to
port: the app is C++/Dear ImGui over SDL2, so the work is recreating those
layouts with ImGui calls. `design/.../README.md` carries the exact metrics
and copy and is the authority for every value; the token CSS under `_ds/` is
the authority for every colour.

What it covers: a board screen (banks, presets, MIDI learn, engine stats,
levels, and a chain with an optional split into channels A and B), a Live
performance screen, and seven modals — plugin UI, add stage, file picker,
audio settings, MIDI mapping table, tuner, metronome.

### The design was drawn against a board that no longer exists

**This is the thing to know before reading any task below.** The prototype
was built against `main` as pushed, which did not yet contain Milestone 9's
Phase C — `GraphView`, `GraphLayout` and the whole wired canvas were
uncommitted at the time. So the design models the chain area as the
*vertical stage list SF-57 deliberately deleted*, and knows nothing about
tiles on a wire, `+` slots, or nested splits.

The decision is to **adopt the design**: the wired canvas is retired
(SF-74) and the chain becomes a vertical list of stage cards with the split
drawn as two lanes side by side. That costs real, working, tested code, and
it is worth writing down what it costs so nobody re-derives it later:

- **Nested splits become undrawable.** `ChainGraph` keeps them — the model
  is not narrowed, only the editor is — but the board draws exactly one
  split, at one place on the trunk. What happens to a preset it can't draw
  is specified in SF-71, and it is *not* "draw part of it".
- **Insertion loses its precision.** A `+` in every gap becomes an `Add
  stage` at the end of each section, plus `^ v` to walk a card into place.
  More clicks for the same result.
- **`graph_layout_test` goes**, and with it the only assertions about where
  anything lands. The vertical layout is ImGui's flow layout, which is not
  ours to assert.

Four smaller places where the design and the code contradict each other.
All four are settled in favour of the design, but each is a thing someone
will otherwise get backwards:

1. **The bypass checkbox is inverted.** In the design **checked means
   active**; today `ImGui::Checkbox("##bypass", &bypassed)` is bound
   straight to `bypassed`, so checked means *off*. Getting this wrong is
   silent and audible.
2. **The meter's colours mean something else.** Today `kMeterWarnDb` /
   `kMeterHotDb` colour the *whole bar* by the current level, at −6 and −1
   dBFS. The design colours each *segment* by that segment's own position:
   below −12 comfortable, −12…−6 warn, above −6 hot, and everything above
   −3 clip-red while clipping. A meter that changes colour all at once is
   the thing the segmented one exists to replace, so the constants change
   meaning rather than surviving unchanged (SF-64).
3. **Lane mute has no home.** The mixer card is two level sliders;
   `GraphBranch::muted` is not drawn anywhere. The flag stays — presets
   carry it and a muted lane must not silently come back on load — and gets
   a place in SF-72 rather than being dropped.
4. **`Quit` is gone from the board.** The design's status bar is Save,
   dirty caption, status line. The button exists today so the shutdown path
   has a second deliberate trigger to test against, so it moves rather than
   disappears (SF-73).

The audio-risk order from Milestone 9 applies again: the splitter's new
modes and the stereo input boundary (Phase D) are engine work, and they
land before the card that edits them is finished.

### Phase A — foundation (nothing moves yet)

- [x] **SF-60**: Theme. `src/ui/Theme.{h,cpp}`: the token set as named
  `ImVec4` constants, and one `ApplyTheme(ImGuiStyle&)` that sets every
  `ImGuiCol_`, the radii (3px controls / 5px cards / 8px modals via
  `FrameRounding`, `ChildRounding`, `WindowRounding`/`PopupRounding`), 1px
  borders, and item spacing off the 4/8/12/16/20/24 scale. Hover steps one
  background level lighter, press one more, focus is a 2px amber ring with
  a 2px gap. Called once from `AppWindow::Open`, replacing
  `ImGui::StyleColorsDark()`.

  The amber accent (`#e0a458`) is genuinely new and is the whole reason the
  window will stop looking like a debug tool. The **semantic meter colours
  are not new** and must not be re-derived from the token hex: the tokens
  were read *out of* `DrawLevelMeter`, so `Theme` takes the existing
  `ImVec4` literals and the CSS is the copy. Round-tripping them through
  hex would move them by a hair for no reason and make the design file and
  the code disagree about which is the source.

  *Acceptance: `--test-ui` renders with no assertions; `grep` finds no
  `PushStyleColor` outside `Theme.cpp` and the meter/warning cases that are
  semantic rather than chrome.*

  Landed as written. One colour was quietly two: the clipping status line was
  `ImVec4(0.95, 0.35, 0.35, 1)` while the meter's clip segment was
  `(0.90, 0.25, 0.25, 1)`, and the token set has only one `--meter-clip`.
  They are one colour now. Nobody would have called the difference
  deliberate, but it is the kind of thing that survives forever once two
  call sites have their own literal.
- [x] **SF-61**: Fonts. Vendor Space Grotesk (Regular, SemiBold) and IBM
  Plex Mono (Regular) — both SIL OFL, licence text alongside — into
  `resources/fonts/`, and load them into the ImGui atlas at the sizes the
  design actually uses (11/12/13/14/16 for UI, 11/12/13 mono, plus the
  display sizes 20/26/46/72/84 the Live screen and the tuner need). A
  scoped `Fonts::Mono` push/pop, because *every* numeric or technical
  readout is mono in this design and threading a font handle through each
  call site by hand will be got wrong.

  ImGui's bundled `stb_truetype` loads these directly — no `imgui_freetype`,
  no new dependency, and the SDL renderer backend uploads the atlas as it
  already does.

  Two things that are not optional. **Resource lookup has to work from a
  build tree, an install prefix and a `SIGNALFORGE_RESOURCE_DIR`
  override**, or the fonts load on the developer's machine and nowhere
  else. And **a missing font file is not fatal**: keep the bundled bitmap
  font, say so once on stderr, and open the window. A Pi image with no
  `resources/` must still boot the board.

  DPI belongs here too: the design's metrics are 1× pixels, so the atlas is
  built at the display scale rather than scaled after the fact, which is
  what keeps 11px mono legible instead of blurred.

  *Acceptance: the window renders with the design's type at 1× and at 2×;
  deleting `resources/fonts/` degrades to the bundled font with a warning
  rather than failing to start.*

  **Three things this task was wrong about, all found by doing it.**

  *One atlas entry per size is no longer a thing.* Dear ImGui 1.92 rasterizes
  a face at whatever size `PushFont(font, size)` asks for. So this is three
  faces, not eleven bakes, and `sf::text::` is a list of numbers rather than
  a list of font handles. The whole "which sizes do we preload" question the
  task spends a paragraph on does not exist.

  *Space Grotesk has no SemiBold.* Upstream ships Light, Regular, Medium and
  Bold statics plus a variable font that stb_truetype cannot instance, and
  Google Fonts has no static 600 either. The design asks for weight 600
  throughout. **Weight 600 is Medium (500) here.** Bold was the other
  candidate and is too heavy at 84px, which is the size that settles it.

  *The design's disclosure arrows do not exist in either typeface.* Neither
  face carries U+25B8 `▸` or U+25BE `▾` — checked, not assumed — so a stage
  card cannot set them as text, and the glyph range is Latin-1 plus U+2039
  and U+203A for the header's breadcrumb and nothing more. **SF-70 has to
  draw those arrows as triangles on the draw list**; `ImGui::ArrowButton`
  already does exactly that, so this costs nothing as long as somebody knows
  before they write `"▾"` and get a box.

  The resource search is environment variable, then alongside the binary,
  then the install prefix, then two build-tree shapes, then the source
  directory compiled in by CMake — and the failure message lists every
  directory it looked in, deduplicated, because "it works on the machine it
  was built on" is the way this goes wrong.
- [x] **SF-62**: The window frame. Three bands: header (52px, surface, 1px
  bottom hairline, 16px padding), body, status bar. Header carries the
  wordmark, a 1px×20px divider, the `bank › preset` breadcrumb (preset path
  as its tooltip), a spacer, the segmented Board / Live pair, then ghost
  buttons — MIDI map, Tuner, Metronome, Audio settings. Body splits into
  the 330px sidebar and the content area, **each with its own vertical
  scroll**, which is the structural change: today everything shares one
  scrolling column, so expanding a stage pushes the meters off the bottom.

  The menu bar goes. Its one item, Audio Settings, becomes the header's
  ghost button, which is also where the retired canvas endpoints (SF-56)
  sent people.

  The MIDI-learn strip and the missing-plugin warning banner move between
  the header and the body, both already driven by state that exists
  (`learning`/`learning_what`, `warning`).

  *Acceptance: `ui_smoke` reaches audio settings through the header button
  rather than the menu; the sidebar scrolls with the chain area held still
  and vice versa.*

  **The window had to get much bigger, and that is a real consequence rather
  than a preference.** A fixed 330px sidebar at the old 720×560 default left
  the chain 390px — less room than the meters get. The default is 1280×800
  now, and the self-test window went from 700×1100 to 1280×1100. That test
  was the first thing to notice: at 700 wide, the `x` on a tile sits past the
  right edge of the chain pane, so `ui_smoke` failed on a step that had
  nothing to do with what changed. Worth knowing that this milestone has
  raised the floor on how small the window can usefully be; whether 1280 is
  the right floor for a 7" Pi panel is SF-84's problem.

  **Hairlines are drawn as snapped rects, not as lines.** `AddLine` with
  anti-aliasing on spreads a horizontal rule across two rows when its
  coordinate isn't on a pixel centre, and because the spread depends on the
  coordinate, the *same* rule came out 1px thick at one end and 2px at the
  other. On a design that is almost entirely hairlines this reads as a
  rendering fault. `AddRectFilled` on whole pixels cannot do it.

  Three of the design's four header buttons are not here. MIDI map, Tuner and
  Metronome arrive with the screens they open (SF-78, SF-81, SF-82) rather
  than sitting in the header disabled — a header of dead controls is a worse
  first impression than a header that is still filling up. Audio settings is
  there, and is now the only way in, since the menu bar it used to live on is
  gone.

  **Also landed, and not in the plan: a frame-capture hook.**
  `SIGNALFORGE_UI_CAPTURE=<path> SIGNALFORGE_UI_CAPTURE_FRAME=<n>` writes one
  rendered frame to a BMP from `AppWindow::EndFrame`. It exists because this
  entire milestone is judged by eye and `ui_smoke` cannot see: every
  assertion it makes is about what was drawn, not what it looks like, and the
  dummy video driver the tests run under has no window to photograph. It is
  an environment variable rather than a CLI flag because it is a development
  aid, not a feature. It found both problems above.

  One thing it also showed, which is not solved: the self-test cannot run on
  a real display. Its injected mouse positions fight the actual pointer, so
  it fails at the first click under X11 and passes under the dummy driver.
  Fine for now — capture works headless — but a `--test-ui` that only works
  headless is worth knowing about before someone tries to demo it.

### Phase B — the parts the screens are made of

- [x] **SF-63**: Shared widgets. `src/ui/Widgets.{h,cpp}`: section label
  (11px mono, `--text-disabled`, 0.08em tracking), rail (centred label
  between two hairlines), segmented control (the Board|Live and 1ch|2ch
  pair are the same thing twice), the three button weights (ghost,
  secondary, primary), status banner in its two tones, list row
  (label + sublabel + selected), and the modal frame — plain
  `rgba(0,0,0,0.6)` scrim with **no blur**, 8px radius, Escape and Cancel
  both close.

  `DrawLearnButton` moves here out of `BoardView.cpp` unchanged: it is
  already what the design asks for, down to being disabled while any learn
  is armed, and it is now wanted by three screens.

  Every one of these takes the probe recorder. The self-test drives widgets
  by name, and a component that forgets to record is a component no test
  can reach.

  *Acceptance: each widget is exercised once by `ui_smoke` by name; the
  modal frame's Escape path is covered for a modal that has no Cancel
  button.*

  `WidgetProbe` moved here out of `BoardView.h` and grew a `ProbeRecorder`
  around it -- a nullable sink, so a widget records itself without asking
  whether it is being tested and costs nothing when nobody is listening.
  `AudioSettingsView.h` was already including `BoardView.h` for that one
  struct; it no longer has to.

  Letter-spacing is drawn a character at a time, which makes `TextTracked`
  **ASCII only**. Every label that wants it is an uppercase word like
  `SIGNAL PATH`, and a multi-byte code point would be split into meaningless
  bytes -- so that limit is stated at the declaration rather than discovered
  later.

  The segmented control is drawn rather than assembled from buttons: the
  design gives the group one rounded outline with square divisions inside it,
  and ImGui rounds a button's four corners or none of them.
- [x] **SF-64**: Meters, DAW style. Replaces `DrawLevelMeter`'s
  `ImGui::ProgressBar`. An inset well (`--bg-inset`, hairline, 3px radius,
  inset shadow, 2–3px padding), 28 equal segments with 2px gaps, each
  coloured by **its own** dB position (see the contradiction list above), a
  2px `--gray-200` peak-hold marker falling 1.2 dB per 450 ms, and the
  reading right-aligned in mono (`-9.2 dB`, `--` for silence, red while
  clipping). 14px tall on the board, 20px in Live, with the shared
  `-60 -30 -18 -12 -6 0` scale row underneath.

  Peak hold is view state and decays on the frame clock, not the audio
  clock — the engine already reports a per-frame peak and has no business
  knowing how long a marker should hang.

  `MeterFraction` keeps its non-finite guards exactly as they are; they are
  the reason silence and NaN don't produce a segment count outside the
  array.

  *Acceptance: a pure unit test (no window) asserts segment colouring at
  each boundary, the clip case, and that peak-hold decays to the live level
  after the expected number of frames.*

  `meter_test`, 75 checks, and it compiles with only `src` on the include
  path -- which is the real assertion that the pure half of `Meter.h` stayed
  pure. `MeterFraction` moved there out of `BoardView.h`, guards intact.

  **The peak fall is stored as a rate, not as the design's per-frame step.**
  "1.2 dB per 450 ms frame" is a figure from a prototype that ticked at 450
  ms; this program draws at 60 Hz, so kept as a per-frame step the marker
  would fall fifteen times too fast. It is dB per *second*, and the test
  asserts that sixty small steps land where one big one does.

  A segment is coloured by the dB at **its own centre**. Some rule had to be
  chosen and written down: -12, -6 and -3 fall on no segment boundary at any
  segment count worth using.

  One bug worth recording because it is the shape of mistake this layout
  invites: the readings were positioned from `GetContentRegionAvail()` read
  *after* the cursor had already moved along the row, so both meters drew
  their dB figures off the end of the sidebar. Every position in a row like
  this is now measured from one width captured before anything is drawn.
- [x] **SF-65**: DSP load, measured and drawn. `JackEngine` times its own
  process callback and publishes an exponentially-smoothed fraction of the
  block's wall-clock budget through a relaxed atomic; the view reads it.
  Nothing on the audio side allocates or locks — it is two clock reads and
  a store.

  Drawn as the same segmented well as the meter, labelled `DSP`, with a
  percent readout: green below 60%, warn 60–80%, hot above. Tooltip
  verbatim: `Share of each audio block's time budget spent running the
  chain`.

  This is the number that tells a player why they are getting xruns, and it
  is the one piece of engine state in the design that has no source today.

  *Acceptance: the figure is near zero with an empty chain and rises with a
  real one; the callback's allocation-free property is checked the way the
  existing headless path checks it.*

  `jack_get_time` rather than `std::chrono`: it is documented as callable
  from the process callback, it does not allocate, and it is what jackd
  measures itself with -- so this figure and the server's agree about what a
  microsecond is.

  Smoothed **on the audio thread**, not in the view. The UI draws at 60 Hz
  and the engine may run ten blocks between frames, so sampling the figure
  per frame would miss exactly the spikes worth seeing. The coefficient is
  per block rather than per second, which makes the smoothing longer in
  wall-clock terms at a small buffer size -- correct, since that is also
  where block times scatter worst.

  **Not clamped to 100%.** Above 1.0 means the callback took longer than the
  block it was given, which is an xrun about to happen; hiding that behind a
  full bar would lose the one reading that explains the noise.
- [x] **SF-66**: Global input and output gain. Two trims **outside** the
  chain: one on the input buses before the first stage, one on the output
  buses after the last. Deliberately not plugins — a plugin would be a
  stage, would take a slot on the board, and would be saved into every
  preset. Range −24…+24 dB, applied per block with a short ramp so a slider
  drag doesn't zipper (the crossfade code's ramp is the right shape and is
  already tested).

  **They live in settings, not in presets.** The design puts them in the
  sidebar's LEVELS group alongside the meters and lists them as app-level
  state, which is right: they trim the interface, not the sound of a
  patch. A per-preset output trim is a different feature and should not be
  smuggled in through this one.

  The output trim is also where SF-81's tuner mute lands, so there is one
  place that can silence the board rather than two.

  *Acceptance: ±24 dB is exactly ±24 dB through the offline engine; a
  swept gain produces no discontinuity at block boundaries.*

  **The input trim needs a scratch copy and the output trim does not.** A
  JACK input port buffer belongs to the server and is ours to read, not to
  write, so the input gain cannot be applied in place; the output buffers
  are. That asymmetry is the only interesting thing in the implementation
  and it is easy to get wrong in the quiet direction.

  The ramp is linear across the block, not equal-power: this is one signal
  being scaled, not two being crossfaded. Without it a trim steps the
  waveform at every block boundary the value changes on, which at 128 frames
  is a click 375 times a second while the mouse is moving.

  **The input meter now reads *after* the trim**, reversing what the old
  comment in `Process` said it was for. Pre-trim is defensible -- it says
  whether the guitar is too hot regardless of what we do next -- but the
  design puts the meter and the trim slider in one group, and a meter that
  doesn't move as you drag the slider beside it is useless for the one job
  it has there.

  The output trim is applied before the mirrors copy and before the meter
  reads, so a mirrored channel carries the same level as the one it mirrors
  and the clip light describes what actually leaves the machine.

  Moving a trim deliberately does **not** set `unsaved_changes`: these are
  settings, and dirtying the preset because someone matched the output to
  their interface would be wrong.

  The arithmetic lives in `src/audio/Gain.h`, header-only and free of JACK,
  so `gain_test` calls exactly what the engine calls rather than a copy that
  can drift. 28 checks: the dB conversion at both ends of the slider and its
  reciprocal symmetry, that unity *copies* rather than no-ops (the input trim
  writes into a scratch buffer the chain then reads, so a skipped copy would
  feed it last block's audio), and the continuity claim in full -- equal steps
  inside a block, **the same step across the seam into the next one**, and a
  sweep of the whole -24..+24 dB range block by block that never jumps by
  more than one increment anywhere.

  That seam is why a block ends at `to - step` rather than at `to`: ending
  exactly on the target duplicates that value and leaves a flat spot at every
  boundary, which is a buzz at the block rate while dragging.
- [x] **SF-67**: Banks. A bank is a **subdirectory of the preset directory**
  holding its own presets and, optionally, its own `manifest.json` for
  order. The loose `*.json` directly in the preset directory are the
  `Default` bank — so today's layout keeps working, unmoved and unedited,
  and a second bank is a folder someone makes.

  `PresetLibrary` grows a bank axis (or a `BankLibrary` over N of them).
  Next/prev step within the current bank; `next bank` is its own action and
  loads that bank's first preset, as the design specifies. CLI preset
  resolution accepts `bank/preset` alongside every form it already takes,
  and `Settings` remembers bank and preset together so resume still lands
  where the player left off.

  *Acceptance: `presets/` loads as one bank named Default in the same order
  as today; a subdirectory appears as a second bank; the `resume_preset`
  test is extended to a preset inside a non-default bank, including the
  case where that bank has since been deleted.*

  **`Count()` and `At()` still mean "the library", they just now mean the
  current bank.** That is what kept this from being a rewrite of twenty call
  sites in `main.cpp`: everything that predates banks already meant the bank
  it was standing in. What changed shape is `Resolve`, which now returns a
  `{bank, index}` and accepts `bank/preset`.

  Verified by hand against a three-bank directory: Default first, then
  subdirectories alphabetically, `mapping.json` still excluded. `Worship
  set/w2-drive` resolves; a bare `w2-drive` resolves by falling through to
  another bank; **`Covers/w2-drive` is refused rather than falling back** --
  an explicitly named bank means that bank, or nothing.

  Two rules that fell out of building it. An **empty folder is not a bank**:
  somewhere to stomp into and find nothing is worse than not being able to
  stomp there. And a **broken manifest costs its own bank, not the library**
  -- the player loses a folder of sounds and is told which, where refusing to
  start would lose them the gig.

  `settings.last_preset` is now qualified (`bank/stem`) except in the Default
  bank, which keeps it a bare stem -- so a settings file written before banks
  still resolves to the same preset.

  `resume_preset` now covers banks: that adding one doesn't change where the
  board starts, that a qualified `bank/stem` resumes into it, that a bare stem
  still finds a preset in another bank, that a **named bank which lacks the
  preset is refused rather than falling back**, and that a remembered preset
  whose whole *folder* has been deleted falls back like a missing file does --
  which is what happens when someone tidies their presets between gigs.

  Checked against an injected regression (the qualified form falling back to
  any bank holding that name), which it catches. The first regression tried
  did *not* fail the test, and that was worth finding out: the fallback search
  uses the whole query including the slash, so it can never match a bare name
  by accident. The refusal is doing real work, just not the work the first
  guess assumed.
- [x] **SF-68**: The MIDI vocabulary the design assumes. `MidiAction` gains
  `NextBank`, `SetInputGain`, `SetOutputGain`, `SetSplitterControl` (send A,
  send B, or crossover, depending on mode), `SetMixerLevel` (A or B),
  `SetTempo` and `ToggleMetronome`; `LearnTarget` gains the matching
  entries. `PrevBank` comes along for symmetry even though the design only
  draws a `next bank` learn button — a next with no prev is a footswitch
  layout nobody wants.

  Persisted meaning stays positional, for exactly the reason SF-47 was
  refused: the mapping file is global and outlives any one preset, so a
  binding says "the mixer's channel A", not a node id.

  Unknown actions in a hand-edited file keep failing loudly with the
  offending entry named, SF-31 style.

  *Acceptance: `midi_map_test` round-trips and rejects every new action;
  `tests/data/example-mapping.json` grows one binding of each kind and
  still loads through `--check-midi-map`.*

  `SetSplitterControl` and `SetMixerLevel` name their target through the
  existing `port` field ("send_a"/"send_b"/"crossover", and "a"/"b") rather
  than a new lane field. That is exactly what `port` already means -- the
  name of a control on a thing -- and the alternative was a field used by two
  actions out of thirteen.

  Two things generalised while passing through. The list of action names is
  now built from the enum, so the "expected one of..." in a load error can no
  longer go stale -- the hand-written list would have been wrong the moment
  this grew. And the refusal of `program_change` for a continuous action now
  covers **all six** value-carrying actions rather than only `set_control`,
  which was the only one that existed when that check was written.

  `NextBank` and `PrevBank` both exist although the design only draws a
  `next bank` learn button: a next with no prev is a footswitch layout nobody
  wants.

  **Covering this found a real bug, which is the whole argument for having
  done it.** `IsSwitchAction` was written as `action != SetControl`, so all
  five of the new value-carrying actions were treated as switches: an
  expression pedal on the output trim would fire only above the halfway point
  and read every value below it as a release. The symptom -- a control that
  simply stops responding over half its travel -- is one nobody would have
  traced back to that line. It is now a switch over the whole enum, so adding
  an action and forgetting to classify it is visible at the point of the
  change.

  `midi_map_test` gained a `TestBoardWideActions` covering every new name, the
  round-trip (each action writes a different subset of the target fields, and
  a subset that doesn't match what the loader requires is a map that saves and
  then won't load), the refusals, and the scaling. `example-mapping.json` now
  carries one binding of each kind and still loads through `--check-midi-map`
  -- 15 bindings, all described correctly.

### Phase C — the board screen

- [x] **SF-69**: The sidebar. 330px fixed, own scroll, 16px padding, 1px
  right hairline, 20px between groups, each group under an 11px mono label.

  **BANK** — label, right-aligned `next bank` caption, learn button; the
  select full width on the line below (it has a 260px minimum and must not
  share a row). **PRESETS** — label plus `N of N`, a `Step:` row with next
  and prev learn buttons, then the list in an inset well, one row per
  preset carrying its name over a `<stem>.json` sublabel. Selection follows
  **what is playing**, not what was clicked — the `requested_preset`
  contract in `BoardView.h` is already written this way and stays.
  **ENGINE** — the two-column table (sample rate, block size, latency as
  `5.3 ms round trip`, MIDI bindings, controller) with the DSP gauge under
  it. **LEVELS** — both meters, the shared scale row, the two gain sliders
  with learn buttons, and the clipping line.

  `Controller` is a new row: the name of the connected MIDI input, which
  `JackEngine` knows and nothing currently shows.

  *Acceptance: every group renders with an empty library, no MIDI device,
  and no engine running — the sidebar is the first thing seen on a
  half-configured machine and must not be a wall of blanks or crashes.*

  `Controller` shows the JACK *client* name of the first connected MIDI
  input, plus `+N` when there are several — an interface commonly presents
  more than one port, and the row is 330px wide answering "is anything
  plugged in", not "list my ports". Nothing connected reads `none` rather
  than blank: a footswitch that does nothing is the most common thing to be
  confused by, and an empty cell looks like a fault in the row rather than
  an answer. It is read where the connection is made — the only place that
  knows — and read again after an audio restart, since the ports are
  registered afresh on the far side of one.
- [x] **SF-70**: `StageCard`. `src/ui/StageCard.{h,cpp}` — surface fill,
  hairline, 5px radius, panel shadow, 12px/16px padding. Header row: expand
  arrow, bypass checkbox (**checked means active** — see the contradiction
  list), plugin name (disabled colour when bypassed, URI as tooltip), the
  **stage code right-aligned** in 12px mono (`0`, `1`, `A0`, `B1`), then
  three 24px square `^ v x` buttons. `^` disabled on the first card of its
  lane, `v` on the last, `x` when the chain would be left empty, tooltip
  `A chain needs at least one stage`.

  The code goes on the right next to the buttons, not as a `0. Name`
  prefix. It is the number MIDI bindings and console commands speak, so it
  needs to be readable, but it is not the card's title.

  Expanded body, 24px indent: `Bypass` + learn, with a right-aligned
  secondary `Open plugin UI` when the plugin declares one (SF-79); a row
  per `atom:Path` property showing the file's basename or `(none) --
  choose...` with the full path as tooltip; a row per control port, slider
  or toggle by the port's own hints, non-writable ports as a disabled label
  and value, and `This plugin has no controls.` when there are none. Most
  of this is `DrawStagePaths`/`DrawStageControls` moved rather than
  written.

  **Expansion is keyed by node id, not by index.** The existing
  `stage_expanded` vector-of-bool follows the wrong row after a move, and
  its own comment admits it. With every card expandable at once instead of
  one selected stage, that stops being cosmetic.

  *Acceptance: `ui_smoke` expands a card, moves a control, and reads the
  value back; bypass polarity is asserted explicitly, because it is
  inverted from what the code does today.*

  Asserting the polarity needed something the probes could not say. A
  `WidgetProbe` carried a position and whether the widget was enabled, and
  nothing about what it was *showing*; Dear ImGui does not keep a checkbox's
  state anywhere a test can read it back afterwards. So a probe now also
  carries `checked`, recorded by the card from the same `active` it draws
  with. Without it the only assertable claim was "clicking it reports
  ToggleBypass", which passes just as happily with the tick on the wrong
  side — and the wrong side is where it was.
- [x] **SF-71**: The chain area. Sections read off the graph — pre, then
  CHANNEL A and CHANNEL B side by side in a 2-column grid of inset panels,
  then post — separated by the `INPUT` / `SPLIT` / `MERGE` / `OUTPUT`
  rails, with an `Add stage` ghost button ending each section. 24px
  padding, 14px between blocks, own vertical scroll.

  **A shape the board cannot draw says so instead of drawing it wrong.** A
  preset with two splits on the trunk, or a split inside a branch, is
  legal in the model and unreachable through this editor. It gets the
  warning banner naming the shape and the chain area goes read-only. Half a
  board drawn silently is worse than a board that admits it needs the file
  — and this case is reachable today by hand-editing or by a preset written
  before this milestone.

  Insertion is section-append plus `^ v`. The `+` slots go with the canvas.

  *Acceptance: a linear preset, a split preset, and an undrawable one each
  render as described; every edit is driven by probe name, not
  coordinates.*

  Both undrawable shapes — two splits on the trunk, and a split inside a
  channel — are self-test fixtures now. Each is asserted twice: once as
  data, since `ReadShape` is pure and can be called with no window at all,
  and once as a drawn frame. The data assertion checks the *reason* and not
  just the refusal, because two different refusals that both said "cannot
  draw this" would send someone to the wrong file.

  The drawn assertion is the one worth keeping honest. It checks that the
  banner is there, that **no** edit affordance is — `add-stage:*`,
  `channels:*`, the mixer and splitter toggles, the mutes, and any stage card
  at all — and that the sidebar's preset list survives, since switching away
  is how someone gets *off* an undrawable board. The banner had to start
  recording a probe to make the positive half of that assertable; without it
  the only claim available was "the board is absent", which a crash also
  satisfies.
- [x] **SF-72**: The signal path control, and the splitter and mixer cards.

  The `SIGNAL PATH` card sits above the `INPUT` rail: the mono label, a
  segmented `1 channel` / `2 channels` pair styled exactly like Board|Live,
  and the caption that changes with it. **This is the only control that
  splits and un-splits.** `2 channels` inserts a split at the end of the
  trunk with both lanes empty; `1 channel` collapses A → B → post into a
  single series and reports `Collapsed to one channel -- every stage now
  runs in series.` That is `RejoinSplit` in `main.cpp`, which already
  exists and already refuses to discard a lane — a rename and a new caller,
  not new behaviour.

  Named "signal path" rather than "amp section", deliberately: the split is
  not amp-specific.

  **Splitter card** — 560px, centred, collapsible, its summary showing the
  current setting when closed (`crossover at 800 Hz` / `full range into
  both channels` / `Left input / Right input`). Mode select with all three
  options; the two that need SF-75 are disabled with a tooltip until it
  lands. Per-mode contents and captions verbatim from the design README.
  Every select in this card stacks full width — a 260px minimum select
  paired side by side overflows a 560px card.

  **Mixer card** — same shape and same collapse behaviour, summary
  `A 0.0 dB / B 0.0 dB`. `Level A` and `Level B`, −60…+6 dB, with learn
  buttons, under the caption `channel levels into the merge`. These map
  onto `GraphBranch::gain_db`, which already persists.

  Two things to fix while here rather than reproduce. **Merge gain becomes
  live** — an atomic the `Mix` op reads, no rebuild — which SF-56 already
  named as the obvious follow-up; with the mixer now a front-and-centre
  card rather than a number on a wire, a slider that only takes effect on
  mouse-up is the wrong feel. And **`GraphBranch::muted` gets a home**: a
  muted lane reads `muted` in the summary with its slider disabled, and the
  toggle sits on the card. The flag has to stay reachable — presets carry
  it, and a lane that silently un-muted on load would be a bug nobody could
  see.

  Collapse state is UI-only and does not dirty the preset.

  *Acceptance: splitting and collapsing round-trip a preset's stages with
  none lost in either direction; a mixer slider drag is audible while the
  mouse is still down.*

  **Live merge gain, and where the number now travels.** A `Mix` op used to
  carry the two lane gains the planner baked into it, so changing one meant
  replanning — which is to say a rebuild and a crossfade, sixty a second
  while a mouse moved. The op now also carries a *merge index*, and the
  chain keeps one atomic gain per lane per merge. `Chain::SetMergeGain`
  writes it from the control thread; `Run` reads every merge once at the top
  of the block, before any op looks at one, and each `Mix` sweeps between the
  value it had and the value it has. Reading per block rather than per op is
  what keeps the two channels of a stereo merge in step — advancing one
  shared "current" as the ops went by would give the second channel a
  different ramp from the first.

  It sweeps rather than steps because stepping a summing gain at a block
  boundary is a click, and a fader that clicks while it moves is not a fader
  anyone can set by ear. `OpMixRamp` with `from == to` is `OpMix` exactly,
  bit for bit — asserted, not assumed — so a board nobody is touching sounds
  the way it did and `split_routing`'s hashes are unchanged.

  **Merge numbering is a contract between two files.** The planner numbers
  splits pre-order, a split before either of its branches;
  `ChainGraph::MergeIndexOf` counts them the same way. They have to stay the
  same walk, and if they ever drift a fader moves the wrong lane of the wrong
  split — which looks like a UI bug for a week. Both sides assert the order
  independently, including the case that separates them: post-order
  numbering, which the *outer* split of a nested pair would get wrong.

  **Mute deliberately did not become live.** It is one edit rather than a
  stream of them, and going through the rebuild path is what gives it a
  crossfade instead of stepping a lane to silence at a block boundary.

  The learn buttons bind `SetMixerLevel`, which the MIDI layer already had a
  name and a `port` field for and nothing drove. The binding names the
  *lane* — "a" or "b" — not the split it was made on: a mapping file outlives
  the preset it was written against, and a node id would mean a different
  node in the next one. A pedal therefore moves the first split on the trunk,
  which is the one the board draws.

  **Not covered, and it needs ears** — the same standing limitation as the
  split itself: that a lane fader *sounds* smooth under a fast drag. The
  ramp's arithmetic is asserted, and the rebuild it replaced is gone from the
  path, but only playing through it proves the sweep is inaudible.
- [x] **SF-73**: The status bar. Surface fill, 1px top hairline, 8px/16px:
  the primary `Save preset` (disabled until dirty, and the
  `save_blocked_reason` tooltip on hover when blocked), the `unsaved
  changes` caption, and the right-aligned status line.

  `Quit` leaves the board. It exists so the shutdown path has a deliberate
  trigger the self-test can pull, so it moves to a key binding rather than
  being deleted — closing the window is the player's way out and the design
  has no button for it.

  *Acceptance: `ui_smoke` still ends through the deliberate quit path
  rather than only through the frame-count timeout.*

  The self-test now ends by pressing Ctrl-Q and asserting the board asked to
  quit — and by drawing one more frame with nothing held and asserting it
  did not, so the first check can't pass on a flag that was simply always
  set. It also checks the chord asked for *nothing else*: a shortcut that
  saved or edited the chain on its way out would be worse than no shortcut.

  **What that reaches, and what it does not.** `--test-ui` drives the view
  directly and never builds an engine, so there is no loop here to leave;
  what is asserted is the *request*, the flag the main loop watches. That is
  the half that would rot unnoticed — the other half is one `if` in
  `main.cpp` that every ordinary run executes and every quit proves.
- [x] **SF-74**: Retire the canvas, rework the self-test. Delete
  `ui/GraphView.{h,cpp}`, `ui/GraphLayout.{h,cpp}`, `tests/GraphLayoutTest.cpp`
  and the `graph_layout` test; `BoardGraphNode`, `BoardGraphBranch` and
  `BoardSlot` move into `BoardView.h`, since they are the view's snapshot
  of the shape and outlive the thing that drew it.

  `UiSelfTest.cpp` is the real work: ~22 probe references point at canvas
  widgets (`node:`, `slot:`, `mute:`, `rejoin:`, `gain:`, `endpoint:`) that
  will not exist. Each becomes a card, section or card-button probe. The
  endpoints were also the way into audio settings, which is now the
  header's ghost button.

  Two lessons from Phase C's own self-test work are worth not relearning:
  no two widgets may answer to the same probe name, and a step that opens a
  popup needs a settling frame before it aims at anything inside it.

  *Acceptance: `ui_smoke` covers select → expand → move a control → learn a
  binding on a stage inside CHANNEL B; `ctest` is green with `graph_layout`
  gone rather than skipped.*

  The split fixture's pedal moved from CHANNEL A to **CHANNEL B**, which is
  the lane the acceptance names and the one that can actually go wrong. A
  stage in lane A is the case that works by accident: everything in lane A
  comes first in signal order, so its flat index is the one it would have had
  on a linear board, and a card that ignored lanes entirely would still
  address it correctly. Lane B has to be counted.

  It keeps the index it had — lane A is empty, and flattening takes all of
  lane A before lane B — so nothing downstream in the self-test had to be
  renumbered. It is also the *muted* lane, so the card is drawn inside a lane
  contributing nothing: a muted lane still runs, and its pedals keep their
  controls and their bindings.

  The chain there is now expand → move a control → learn a binding, all on
  that card. One thing learned putting it there: **collapsing the card
  afterwards is not free.** An expanded card is tall, the mixer sits below
  the lanes, and putting one away moves everything under it through three
  levels of auto-resizing child — more than the two settling frames every
  other step needs. It stays expanded, as it did before there was anything to
  learn on it.

**Phase C verification.** `ctest` is green: 26 tests, 0 failures, one skip
(`path_property`, which wants `-DSIGNALFORGE_TEST_MODEL=<path>` to a real
model file and has always skipped without one). `graph_layout` is gone rather
than skipped, which is the half of SF-74 that a green run proves.

`chain_schedule_test` grew a merge-gain section: that a stereo split is one
merge and not two, that the outer split of a nested pair is merge 0 though it
sums last, that the plan's gain table and the ops it planted agree including a
muted lane's zero, and that a flat `OpMixRamp` is `OpMix` bit for bit while a
moving one starts where it was and ends one step short of the target.
`chain_graph_test` asserts the same numbering from the other side, on a graph
with a nested split and a later trunk split.

`ui_smoke` now also covers bypass polarity as a fact about what is drawn
rather than what a click reports; expanding a card in CHANNEL B, moving one of
its controls and learning a binding on it; the channel level reporting
`SetLaneGain` mid-press with a value in dB; a muted lane's level
drawn-but-disabled; both lanes' learn buttons carrying the lane and the
−60…+6 dB range; two boards this editor refuses to draw, each asserted as data
and as a frame with every edit affordance absent; and Ctrl-Q asking to quit
while an unpressed frame does not.

`split_routing` reaches the rest. `--test-chain` grew `--lane-gain <a|b>=<dB>`,
which sets a level on the built chain the way the mixer card does — after
`Settle`, with no replan. Without a way in from the command line, the merge
index, the dB conversion and the atomic the audio thread reads would only ever
have been checked by reading them: `SetMergeGain` is called from a slider, and
nothing headless can move a slider.

Its claims are about which runs differ from which rather than about matching a
planned reference, because the first block *sweeps* to a new level instead of
stepping to it — a level set live and the same level planned into the file
agree everywhere except that one block, by design. So: setting a lane to the
level it already has must be hash-identical to leaving it alone (which pins
the lane and the conversion — writing −12 dB into the 0 dB lane would be
plainly audible); setting it to a different one must not be (which rules out a
`SetMergeGain` that never reaches the audio thread); and pulling lane A down
must not sound like pulling lane B down.

Six injected regressions, each caught: numbering merges post-order (caught by
the nested-split check, and only by that one — a single split is merge 0
either way); a ramp that jumps to its target instead of sweeping (caught three
ways); a learn button that reports lane A for both lanes; `SetMergeGain`
ignoring its lane argument; a second split on the trunk quietly drawn instead
of refused; and Ctrl-Q doing nothing.

**Still only read, not run:** the five lines in `main.cpp` between the
slider's `SetLaneGain` and `Chain::SetMergeGain`. Both ends are asserted — the
view's report in `ui_smoke`, the engine's behaviour through real plugins in
`split_routing` — and the numbering they agree on is asserted from both sides.
Reaching the glue itself needs a GUI frame and a running engine in the same
test, which nothing here has.

**Not covered, and it needs ears:** whether the picture is legible, and
whether a lane fader sounds smooth under a fast drag. Every claim above is
about structure and arithmetic.

### Phase D — splitter modes and stereo (the part with audio risk)

- [x] **SF-75**: Splitter modes. A split node gains a `splitter` object:
  `mode` (`full` | `crossover` | `stereo`), `send_a`/`send_b` in dB, `freq`
  in Hz, and `src_a`/`src_b` (`left` | `right` | `sum`). **An absent
  `splitter` reads as `full` with unity sends**, so every split preset
  written before this round-trips byte-identical — the same rule SF-46
  established for ids.

  In the planner (`ChainSchedule`, which takes numbers and touches no
  lilv): `full` is today's copy plus two gains. `crossover` is a
  Linkwitz–Riley pair at the crossover frequency — 4th-order LR
  specifically, because its two halves sum flat and a Butterworth pair does
  not, and a crossover that colours the sum is a crossover that will get
  blamed on the amps. Below feeds A, above feeds B. Filter state lives in
  the op, not in a plugin. `stereo` takes one side of a two-channel input
  bus into each lane.

  *Acceptance: `chain_schedule_test` asserts the LR pair sums flat across
  the band within tolerance, that lane gains are applied where claimed, and
  that stereo mode against a one-channel input is identical to full range;
  a `split_routing`-style live test covers a crossover preset end to end.*

  **Sends live on the splitter, returns on the branch.** The file groups
  `send_a`/`send_b` with the mode and the frequency, as the plan above says,
  which reads oddly next to the per-branch `gain_db` until you say what each
  one is: a send is how hard the lane is *driven*, a return is how much of the
  result comes back. With a distorting pedal between them those are not the
  same knob, and the whole point of having both is to be able to hit an amp
  hard and then bring it down. The planner wants the send where the lane it
  feeds is, so `ChainPreset` moves it onto the branch on the way through --
  one more thing that translation exists to do.

  **A default splitter costs nothing, in every layer.** Full range with unity
  sends plans no op and allocates no bus, so a preset written before this
  compiles to exactly the plan it used to; the JSON writer omits a splitter
  equal to the default, so such a preset also *saves* byte-identical; and
  every field is written only if it differs, so a board that picked a
  crossover frequency doesn't acquire two stereo sources nobody chose. All
  three are asserted, the last two on the file and the first on the plan.

  **Nothing here is live.** The mode, the frequency, the sources and the sends
  are all planned into the chain, so each costs a rebuild -- unlike a merge
  gain (SF-72) there is no atomic that could avoid it, because the filter
  coefficients are built when the chain is. So the selects report on click and
  the sliders report on release, the rule SF-56 established. That turned out
  to need more than `IsItemDeactivatedAfterEdit`: **Dear ImGui writes a
  dragged value into the caller's variable while the item is active and writes
  nothing on the frame it is released**, so a control that read its value back
  on deactivation reported the value the drag *started* from. The mid-drag
  value is kept in `BoardViewUi` now, which also makes the number follow the
  cursor while the sound waits -- the behaviour that was wanted anyway.

  **Filter state is per channel as well as per half.** A stereo crossover is
  four filters, not two: the state belongs to one channel, and one filter
  shared between two would fold each into the other. The planner emits one per
  channel and the test asserts the count, because this is invisible until it
  is audible and by then it sounds like a bad plugin.

  The crossover frequency is clamped at the filter rather than refused --
  it comes from a slider and a silent board is a worse answer than one that
  runs out of band -- but refused at the *parser*, where a `freq` of 0 is a
  mistake in a file that should be reported once rather than silently
  rewritten on every load.
- [x] **SF-76**: Stereo in at the board boundary. `AudioConfig::board_inputs`
  (0 auto / 1 / 2) landed with SF-52; what is missing is the Audio settings
  row that sets it (`Mono (1 in)` / `Stereo (2 in)`) and the engine
  actually opening two capture ports and carrying both into the board's
  input bus.

  This is what makes the splitter's `stereo` mode mean anything. When the
  interface is providing one input channel, the mode still selects and the
  splitter card shows the caveat the design specifies — both channels get
  the same signal until a stereo input is chosen in Audio settings. Saying
  that is better than disabling a control for a reason the player has to
  guess.

  *Acceptance: a stereo-input board with the splitter in stereo mode puts
  left into A and right into B, asserted offline; a one-input board shows
  the caveat and stays audible.*

  The engine needed less than expected and the offline harness needed more.
  `JackEngine::Start` already registers one JACK port per chain input port and
  `ConnectAudioPorts` already matches them against capture ports by position,
  so a chain planned for two inputs has always opened two. What was missing
  was a way to *ask* for one: the dialog's `Board input` row, and a rebuild
  when it changes. `restart_audio_engine` only replanned on a sample-rate
  change; a width change has the same consequence for a different reason -- it
  is planned into the chain, and the engine registers ports from the chain --
  so a board that went mono to stereo would have come back up with the port
  count it had before.

  **The offline engine could not have caught any of this.** Its generator was
  `f(frame)` and fed every input channel the same sample, so left into A and
  right into B was indistinguishable from the reverse, from both, and from
  neither. It takes `f(frame, channel)` now, and `MakeSineSweep` puts channel
  0 at full level and everything right of it 12 dB down. Quieter rather than
  silent, so a lane fed from the right is still a lane making sound: "it went
  quiet" and "it was routed wrong" should not look the same. Channel 0 is
  untouched, so every mono measurement ever taken with it still holds -- which
  is why no existing hash moved.

  `--test-chain` gained `--board-inputs 1|2` to reach it, alongside the
  `--lane-gain` added for SF-72. Both exist for the same reason: these are
  paths whose only other entrance is a control nothing headless can operate.

**Phase D verification.** `ctest` is green: 26 tests, 0 failures, one skip
(`path_property`, which still wants `-DSIGNALFORGE_TEST_MODEL=<path>`).

The crossover is checked as a filter before it is checked as a feature.
`chain_schedule_test` measures its response at six frequencies from 40 Hz to
15 kHz and asserts the two halves sum to unity within 2% — they come out
within 1e-5 — and that each half is **−6 dB** at the crossover rather than
−3. That second one is what tells a Linkwitz-Riley from the Butterworth pair
it is built out of, and it is the check that would catch someone "simplifying"
the cascade back to one section. RMS rather than peak, deliberately: at 8 kHz
on a 48 kHz board a sine is six samples per cycle and no sample need land near
the crest, so peak under-reads by up to 13% for reasons that have nothing to
do with the filter.

Then the plan: that a default splitter emits no ops at all, that a send is one
gain op on one lane, that send-down-and-return-up is unity end to end while
what sat between them saw half, that a stereo crossover is four filters and a
mono one is two, that low feeds A and high feeds B, that a stereo split takes
its sides without copying them, and that stereo mode with one input channel
plans *exactly* full range — op for op, not "sounds about the same".

`chain_graph_test` covers the file: an absent splitter reads as full range and
is written back absent, a splitter that spells out the defaults equals an
absent one and saves as nothing, every field round-trips, only what differs is
written, and six malformed splitters are refused with the JSON path named. A
misspelt mode is refused rather than read as full range — the difference
between two amps and one is not something to guess at.

`ui_smoke` drives the card: per-mode contents (full range draws no frequency
and no sources; crossover draws a frequency and no sources; stereo the
reverse), the mode select opened and a mode chosen from it, the frequency and
send sliders reporting on release, **and that moving one control leaves the
rest of the splitter alone** — the check that catches a control which rebuilds
the splitter instead of amending it. Plus the Audio settings `Board input`
row, which must change the width and not the port filter beside it.

`split_routing` covers the whole path with real plugins: an absent splitter and
a spelled-out default one are the same sound sample for sample; stereo mode on
a one-input board is full range, sample for sample; a crossover is not a copy;
800 Hz and 250 Hz are different boards; a send changes what comes out; and on a
two-input board, left-into-A, right-into-A and sum-into-both are three
different sounds.

Four injected regressions, each caught: one Butterworth section instead of the
LR4 cascade (caught by both the flatness and the −6 dB checks, and by nothing
else — the pass/stop checks are happy either way); a JSON writer that always
emits the splitter (caught as a round-trip failure *and* as a compatibility
one); a stereo split that ignores its sources and always takes the left; and
an Audio settings row that always answers mono.

Writing that third one turned up something worth keeping: with the stereo
fixture's second lane at a plugin gain of `0.0` — a *linear* multiplier, so
silence — every routing through it sounded identical and three of the
assertions were passing on a silent lane. Both lanes carry real gain now.

**Not covered, and it needs ears:** whether a crossover at 800 Hz is a
musically useful place to split a guitar into two amps, and whether the LR4's
phase behaviour is audible on a real cabinet. The arithmetic says the two
halves sum flat; only playing through it says the split is worth having.

### Phase E — the other screens

- [x] **SF-77**: Live. Same header and status bar, body centred with
  48px/32px padding and 28px gaps: bank name uppercase in mono, preset name
  at 84px, `Next: <preset>` in mono, a 3-column grid of 104px preset tiles
  (active tile raised, 2px amber border, amber name, panel shadow; each
  tile shows its binding or `unbound`), full-width 64px `Prev` and `Next`,
  then the 20px meters with their scale row.

  This is the screen that gets looked at from two metres away with a guitar
  on, which is the whole argument for the type sizes.

  *Acceptance: `ui_smoke` switches to Live, loads a preset from a tile, and
  returns to the board with the same preset playing.*

  **Live replaces the body, not the window.** The header, the learn strip and
  the status bar are drawn on both screens, because what they say is true on
  both. What that buys is asserted rather than assumed: with Live up, none of
  the board's widgets exist at all, so there is nothing underneath for a click
  to reach.

  Every way of changing preset here goes through `requested_preset` -- the
  tiles, and Prev/Next -- which is the same field the sidebar's list uses.
  Stepping *is* a preset switch, and a second channel to the caller would be a
  second thing that could disagree about what is playing.

  One ImGui trap worth writing down: **a child window is not an item.** The
  tile was a bordered child with `IsItemClicked` after it, which is silently
  never true -- a child captures the mouse on its own behalf. The tile's target
  is an invisible button covering it, drawn first and written over, which is
  the ordinary way to make an area of ImGui clickable and gives the probe a
  real rect to aim at.
- [x] **SF-78**: MIDI mappings modal. 680px, a 4-column table
  (110/130/1fr/90) with a sticky `MESSAGE ACTION TARGET RANGE` header, one
  row per binding — message in amber mono, action in secondary mono, target
  in body text, range in disabled mono with `--` where the action takes no
  value — and `N bindings` in the footer. Read-only, and **derived from the
  live `MidiMap` rather than stored**: presets, next/prev, bank, every
  stage bypass and control port, mixer, splitter, global gains and
  metronome all render from the same table the engine resolves against.
  Empty reads `No bindings yet -- press learn next to anything bindable.`

  *Acceptance: a binding made through learn appears in the table without a
  reload; the table's row count agrees with the sidebar's `MIDI bindings`
  figure.*

  Both halves of that acceptance are answered by *construction* rather than by
  a check that could pass while being wrong. The rows are built in
  `refresh_bindings`, which already ran whenever the map changed and is the
  only place they are built -- there is no stored copy to fall behind, so a
  binding just learned is in the table by the time the next frame draws. And
  the sidebar's figure is now taken from the rendered rows rather than from
  `MidiMap::Size()`: a row the table declined to render is a binding the
  sidebar must not claim, and two counts of one list that could disagree
  eventually will.

  The row count is still asserted in `ui_smoke` -- construction settles what
  the *caller* hands over, not whether the table draws all of it.
- [x] **SF-79**: Plugin UI modal. 640px: a 200px inset well reading `The
  plugin's own editor (suil embed) renders here.` over the plugin URI in
  mono, then a `GENERIC CONTROLS` label over the same path and control rows
  the stage card draws. Opened by the card's `Open plugin UI` button, which
  only appears when the plugin actually declares a UI — lilv can say.

  **The well is honest placeholder, not a promise.** Embedding a plugin's
  own editor through `suil` is a v1 non-goal (PLAN.md §2) and stays one;
  the generic controls are the fallback and have to keep working whether or
  not that ever changes.

  *Acceptance: opening the modal from a card shows that stage's controls,
  and moving one is audible on the running plugin.*

  The modal shows the card's rows, not a second set: `DrawStagePaths` and
  `DrawStageControls` moved out of `StageCard`'s anonymous namespace and both
  callers use them. So a knob moved in the modal reports through the fields
  the card already reports through, and reaches the running plugin by the path
  that already works -- two implementations would be two chances for one of
  them to stop.

  That immediately hit the rule Phase C wrote down: **no two widgets may
  answer to the same probe name.** With the card expanded behind the modal,
  both sets of rows were called `control:1:gain`, a test clicked whichever it
  found first, and that was the one under a modal that nobody can reach. The
  row drawers take a probe prefix now; the card passes none and the modal
  passes its own.

  Removing the duplication turned up more of it: `BoardView.cpp` still carried
  its own `DrawStagePaths`/`DrawStageControls` from before cards existed,
  called from nowhere since SF-70. Deleted.

  `PluginStage::HasCustomUi` asks lilv rather than guessing, and the button is
  drawn only for plugins that have an editor to talk about -- most have none,
  and a button on every card that opened a modal saying "this plugin has no
  editor" would be clutter serving the few that do.
- [x] **SF-80**: Audio settings as a modal. Today it is its own floating
  window with its own shape; it becomes a 520px modal in the shared frame
  like every other one. Device select with the `unavailable` status banner
  when the saved device is absent, then Input channels (SF-76), Sample rate
  and Buffer size — each stacked full width, one per row, for the same
  260px-minimum reason as the splitter card — a mono latency readout, then
  Cancel / Apply.

  The first-run path keeps its behaviour: no Cancel, because there is
  nothing yet to cancel back to.

  *Acceptance: both entry points (first run and the header button) render
  correctly; applying still re-patches without a restart.*

  The two entry points were already one function called from two places, so
  what changed is the shape rather than the paths: `ImGui::Begin` became
  `widgets::BeginModal`, and the popup is opened by the drawing code itself --
  the caller has nothing else to say about it, and `DrawAudioSettings` being
  called *is* the request. Applying is untouched, so the SF-38 re-patch still
  happens without a restart.

  Every select stacks full width for the same 260px-minimum reason the
  splitter card has, and the buffer size gained the round-trip figure the
  design asks for: two periods, one in and one out, which is what the
  sidebar's Latency row means by the same words.
- [x] **SF-81**: Tuner. 440px modal: `OUTPUT IS MUTED WHILE THE TUNER IS
  OPEN`, the note at 72px, the frequency in mono, a 40px needle track with
  centre and ±25-cent ticks, the `-50 -25 0 +25 +50` scale, and the cents
  readout — green within ±5 cents, amber outside.

  Pitch detection runs on the **control thread** over a copy of the input
  buffer, not on the audio thread: autocorrelation or YIN over a window
  long enough for a low E, which is far too much work per block to do
  inline. The mute is the output trim from SF-66, so there is one thing
  that can silence the board rather than a second path that can disagree
  with it.

  *Acceptance: fed a synthetic 440 Hz sine offline it reads A4 at 0 cents,
  and a ±20-cent detune reads back within a cent or two — the claim is
  testable, so it should be tested rather than eyeballed against a guitar.*

  It reads 440.0 Hz and 0.0002 cents, and the detunes come back within 0.003
  of where they were put. Two decisions bought that, and both are load-bearing:

  **YIN's cumulative-mean normalisation, not plain autocorrelation.** A
  periodic signal is just as unlike itself at twice its period, so the deepest
  minimum is very often the octave *below* the note — and on a plucked string
  where the second harmonic is louder than the fundamental, that is the common
  case rather than the corner one. The normalisation makes the first deep
  minimum win instead of the deepest. All six open guitar strings and a bass
  low E are asserted as plucked tones with the fundamental deliberately quieter
  than its own second harmonic, which is the shape that breaks the naive
  version.

  **Sub-sample interpolation of the minimum.** At 48 kHz, A4 is 109.09 samples
  per period and the whole-sample lags either side are +1.4 and −14.4 cents. A
  detector that rounded to a sample could not express "20 cents flat" at all —
  it would put a perfectly tuned string 14 cents out and the display would be
  reporting its own arithmetic. Removing the interpolation was one of the
  injected regressions; it fails with exactly those two numbers.

  **The window crosses threads as a whole buffer, not a ring.** Two buffers
  with a handover flag: the audio thread fills one while the control thread
  reads the other, and filling stops until the reader takes it. A ring the
  reader chased could be overwritten mid-window, and half of one window
  followed by half of the next is not noise — it is *periodic-looking*, and
  would read as a confident wrong note.

  The mute is the SF-66 output trim, at −90 dB rather than −INFINITY, because
  that trim ramps across a block and there is no ramping toward negative
  infinity. What it was before is remembered and restored: a tuner that left
  the board at 0 dB would have quietly changed a setting the player chose.
- [x] **SF-82**: Metronome. 460px modal: beat indicator (one 14px dot per
  beat, active dot amber) beside the BPM readout; Tempo 40–240 with learn;
  Time signature select (4/4, 3/4, 6/8, 2/4); Level 0–100%; Start/Stop
  primary, Tap tempo secondary, and a learn button for start/stop.

  The click is a source mixed into the output **after** the chain and after
  the output trim, so the board's gain staging doesn't change how loud it
  is and it can't feed the tuner. Beats are scheduled from a sample counter
  on the audio thread — no timers, no allocation, no wall clock. Tap tempo
  averages the last few taps on the control thread and writes one number
  across.

  *Acceptance: at 120 BPM and 48 kHz the click lands every 24000 frames,
  asserted offline; the process callback still allocates nothing.*

  Every gap is exactly 24000 frames -- not on average, not within a
  millisecond. **The beat boundary is checked per sample, not per block**,
  which is the difference between a metronome and a suggestion: quantising to
  a 256-sample buffer is 5 ms of jitter, audible as sloppiness against anything
  played to it. That was an injected regression too, and it reads 24064.

  The block size is asserted not to move the beat at 13, 64, 512 and 1024
  frames -- 13 especially, because a period that is not a multiple of the
  block is the case a boundary check gets away with at 256.

  The click mixes in **after the chain and after the output trim**, so the
  board's gain staging cannot change how loud it is and it can never reach the
  tuner, which listens to the input. `Metronome` is a control-thread object the
  engine is handed a pointer to; jack/ has no business owning one, and its job
  is only to give the click somewhere to land.

  `SetTempo` and `ToggleMetronome` were bound and persisted since SF-21 with
  nothing behind them. They drive something now -- both are plain setters on
  atomics the audio thread reads, so a pedal can send as fast as it likes.

**Phase E verification.** `ctest` is green: 28 tests, 0 failures, one skip
(`path_property`).

**One collision found after the fact, by a check that did not exist yet.**
Every `Add stage` button is labelled "Add stage", and Dear ImGui identifies a
widget by its label and the id stack above it -- so on a *split* board, the two
on the trunk shared one id. A linear board draws one and never notices, which
is why it survived every phase up to here.

**How bad it actually was: not very, and it is worth being accurate about.**
Hover and press resolve per *rectangle*, not per id, and ImGui's "is another
item already hovered" guard passes precisely because the two share an id. Both
buttons worked. Keyboard nav is not enabled here and neither button has a
tooltip, so the entire visible symptom was ImGui's own debug tooltip in front
of a developer hovering one of the pair. What a duplicate id genuinely breaks
is id-*keyed* state -- popups, a slider mid-drag, tree nodes, focus -- and none
of that was involved.

It was still worth fixing and the check still worth keeping, for the case that
is coming rather than the one that happened: the day one of those buttons grows
a tooltip or opens a popup, the same collision stops being harmless and starts
being silent. `ui_smoke` now records the id ImGui gave each widget alongside its
probe and compares them directly -- one pass over one frame per screen,
covering every widget rather than only the ones a mouse can reach. Six states
are checked: board, live, split board, and each modal drawn over a board that
is still there.

The first version of that check hovered every widget in turn and took 28
seconds. It also left the modals it opened *stale*: clearing an open flag stops
a modal being drawn but leaves the popup on ImGui's stack, and a stale one
blocks every click behind it for the rest of the run. Both are fixed; the check
costs about two seconds. Two of those tests are new, and both exist because their
subject has a right answer that can be written down.

`pitch_detect_test` (30 checks): 440 Hz reads 440.0 Hz and A4 at 0.0002 cents;
detunes of ±7 and ±20 cents come back within 0.003; all six open guitar
strings and a bass low E read correctly as *plucked* tones with the
fundamental quieter than its own second harmonic; middle C is C4 and the
guitar's low E is E2, which is the octave numbering nothing else would catch;
and silence, a chord, a short window, no samples and no sample rate each
report nothing rather than a note.

`metronome_test` (18 checks): every beat exactly 24000 frames after the last
at 120 BPM; the same beat placements at block sizes of 13, 64, 512 and 1024;
40, 60, 90 and 240 BPM each landing on their exact period; the downbeat
audibly unlike the beats after it and repeating on the next bar; silence when
stopped and at zero level; and the click summing into the output rather than
overwriting it.

`ui_smoke` grew the four screens: the Board|Live switch and a live screen with
no board drawn behind it, the mappings table's row count against the sidebar's
figure, the plugin-UI modal's controls answering to their own probe names, the
audio modal's two entry points differing only in Cancel, the tuner's three
display states, and each metronome control asking for its own thing and only
its own thing.

Six injected regressions, each caught: a card offering an editor for every
plugin; the mappings table dropping a row; live Next always jumping to the
first preset; a first run offering Cancel; pitch detection without sub-sample
interpolation (which fails at exactly the +1.4 and −14.4 cents its comment
predicts); and a metronome checking beats per block instead of per sample
(24064 frames instead of 24000).

Three of this round's own tests were caught asserting nothing before they
asserted anything: a negative check on a *collapsed* card, which draws no body
at all; a slider clicked at its midpoint when the fixture already sat there;
and a beat detector that found dozens of starts per click because a decaying
sine crosses zero every half cycle. All three now start from somewhere the
click can move.

**Not covered, and it needs ears:** whether the tuner is quick enough to be
usable between songs, whether the click sits right against a band, and whether
the live screen is actually legible at two metres -- which is the entire
argument for its type sizes and the one thing no assertion here touches.

### Phase F — closing out

- [x] **SF-83**: Documentation, and the design bundle itself. Unzip
  `design/LV2 Pedalboard Host.zip` into the repo so the tokens and the
  handoff README are readable without a zip tool, and point README at it as
  the authority for values. Then PLAN.md: §8 (UI) rewritten around the two
  screens and seven modals it now has; §2 non-goals, where tuner,
  metronome and banks have stopped being non-goals and should say what they
  became rather than just vanishing; §5.1 for the `splitter` object. Reads
  alongside SF-59, which is the same job for Milestone 9 and is still open.

  Done together with SF-59, since they are one edit to one document. The
  non-goals are **rewritten rather than deleted**: what a project decided not
  to do is worth keeping legible, and "we said no to this and then did it, for
  these reasons" tells a reader more than a gap does. Two of the three are
  still true and now say what is actually limited -- no *drag-and-drop* (the
  board is a graph, edited by buttons that name what they do), and no suil
  embedding (the modal says so where a plugin has an editor).

  §4.2 was written as a future milestone and now records what shipped,
  including the two places it guessed wrong: there is no separate merge node,
  and an empty branch is legal rather than degenerate. §5.1 gains the `graph`
  form and the `splitter` object, with the rules that keep old files
  byte-identical. §7 recommended mono-only end to end; what shipped adapts
  between neighbours, because a board with one stereo pedal in it turned out
  to be the ordinary case rather than the exotic one.

  The design bundle was already unzipped in the tree; README and PLAN.md now
  name it as the authority for values, with `src/ui/Theme.h` as its
  transcription.
- [ ] **SF-84**: The pass that needs eyes. Everything above is asserted as
  structure and behaviour; **none of it is a claim that the window looks
  right.** On a real screen at the real size: whether 330px is the right
  sidebar, whether 28 segments read as a meter or as a smear, whether 84px
  is too big for the Live preset name, whether the amber and the meter
  green fight each other, whether 11px mono is legible. Then the same on
  the Pi at whatever resolution the board actually runs at, where the
  design's 1× metrics may not be what a 7" panel wants.

  Same standing limitation as SF-28 through SF-30, SF-38, SF-41 and Phase
  C's own: the tests prove the intent was implemented, not that the intent
  was right.

  **Still open, and it is the one task here that cannot be finished without a
  person and a screen.** Every question it asks -- whether 84px is too big,
  whether the amber and the meter green fight, whether 11px mono is legible at
  arm's length on a 7" panel -- is a judgement about what an eye does with a
  rendered pixel, and nothing in this repository can answer one.

  What *was* checkable has been checked, and it is worth separating from what
  has not: **the transcription is faithful.** `src/ui/Theme.h` and `Fonts.h`
  were compared field by field against the bundle's tokens --
  21 colours, the whole 4/8/12/16/20/24px spacing scale, all three radii, and
  every step of the type scale from 11px to 46px. Nothing differs. So if the
  window looks wrong, it is the design's figures that are wrong for this
  screen, not a value that was mistyped on the way in -- which is the more
  useful of the two things to have ruled out before anyone sits down with a
  Pi.

---

## Milestone 11 — the built-in noise gate

- [x] **SF-85**: A noise gate built into the board rather than reached for as a
  plugin. Every high-gain preset needs one, and requiring the player to install
  and insert one makes the board's most universally-needed pedal the one thing
  it cannot supply itself.

  **Per preset**, not board-wide: a high-gain lead gates hard and a clean tone
  should not gate at all, and one threshold cannot serve both. It is a
  `NoiseGateSettings` on `Preset`, not a node on the graph -- a gate is what the
  board does to its *input*, and making it a stage would put it somewhere it
  could be moved to where it does not belong.

  **Threshold and release are the only controls.** Attack, hold, hysteresis, the
  detector's decay and its high-pass are fixed; the numbers and the reasoning
  are in `NoiseGate.cpp`, which is the place to argue with them.

  **Where it runs is the whole design.** In `JackEngine::Process`, in place on
  `input_scratch_`, *after* the tuner tap and *before* the chain. Sitting there
  needs no restructuring only because `ConnectAudioPort` hands over a pointer
  rather than a copy, so mutating the buffer before `Run` is seen by the chain
  -- and by both hosts during a crossfade, which is what gives a preset switch
  one continuous gate rather than two fighting inside the fade.

  Two orderings there are load-bearing and invisible once they work. The input
  meter is measured *before* the gate, so it keeps showing the ungated signal --
  which matters because the meter is what the threshold is set against, and one
  reading silence exactly when you need to see what you are cutting is no use.
  And the tuner taps before the gate, so tuning is never gated: a quietly
  plucked string is precisely what a gate removes.

  **The preset is the value and the engine is a sink.** Written that way round
  deliberately: if an edit went only to the engine, `CaptureChainPresetInto`
  would snapshot a stale `current_source.gate` and a save would silently throw
  the change away. The only thing read back *from* the engine is the live gain
  for the indicator, which is a meter rather than a setting.

  The absent-object rule from SF-46 and SF-75 applies unchanged: a gate equal to
  its default is omitted on save, an absent one parses as that default, and
  `enabled` defaults false -- so every preset written before this round-trips
  byte-identical *and* sounds identical. Out-of-range numbers clamp rather than
  refuse; a threshold has silly values, not wrong ones. Wrong *types* are still
  refused, with the field named.

  *Acceptance: the arithmetic every slider promises is asserted rather than
  listened for; a preset with no gate is unchanged in file and in sound.*

**Verification.** `ctest` is green: 29 tests, one new.

`noise_gate_test` (34 checks) covers the timing the sliders promise -- the
release reaches −60 dB at exactly 2880, 9600 and 24000 samples for 60, 200 and
500 ms at 48 kHz, and means the same in milliseconds at 96 kHz -- plus: a gate
never enabled is bit-identical *and does no per-sample work at all* (the frame
counter is what tells "returned early" from "multiplied by 1.0f"); a closed gate
reaches exactly `0.0f` rather than trickling denormals into every plugin
downstream; both channels take one gain so a stereo image cannot collapse; and
the same samples at block sizes 13, 64, 256 and 1024.

Then the failure modes a gate actually has in the field: a NaN must not wedge it
shut, a DC offset must not wedge it open, enabling it mid-note must not cut the
note, and disabling it while shut must ramp rather than step -- that last one
lands exactly on a preset change, where the crossfade has just gone to the
trouble of avoiding a click.

Five injected regressions. Three were caught at once: no denormal flush, a
short hold, and a disable that steps instead of ramping. **The other two are the
interesting ones.**

The NaN guard was *incomplete when written* and the test found it: clearing the
envelope is not enough, because the detector's own high-pass state stays NaN,
every later sample filters to NaN, and the gate sits shut forever. The symptom
would have been a board that goes permanently silent after one bad sample.

And the chatter test was **asserting nothing**. It used a note 10 dB clear of
the threshold, which cannot ripple below the close threshold however short the
hold -- an injected 5 ms hold sailed straight through it. Moved to just above
the threshold, where a real gate chatters. That also turned up something worth
recording: hold and detector decay are *redundant* here, and shortening either
alone still passes. It takes losing both. Two cheap mechanisms covering one
another is the right trade for a failure that sounds like a broken amp, but it
means neither constant is justified by a test on its own, and the comments now
say so rather than claiming credit for the hold.

**The gate, splitter and mixer cards all start collapsed.** Three open cards
push the pedals -- which are what the board is actually about -- a long way down
the page, and on a split board the chain area would open showing more settings
than signal path. Each header carries its current setting when shut (`off`,
`crossover at 800 Hz`, `A 0.0 dB / B 0.0 dB`), so closing costs the ability to
*change* them at a glance, not to read them.

That did cost something, and it is fixed rather than accepted: the gate's live
indicator lives in the card body, so collapsing by default would have hidden the
one reading a player needs while setting a threshold. The shut header says what
the gate is *doing* as well as what it is set to -- `-50 dB, 200 ms release --
cutting`. `ui_smoke` asserts all three come up collapsed and opens each by its
header, so the default and the expand affordance are both covered.

**Not covered, and it needs ears:** whether −50 dB and 200 ms are good defaults
for a real guitar into a real interface, and whether the gate is tight enough in
front of a high-gain amp model. It also cannot do what a four-cable-method gate
does and sit *after* a preamp; that would be a stage, not this.

- [x] **SF-86**: Collapsing to one channel and back puts the pedals where they
  were. Going from two channels to one flattens both lanes into the trunk, which
  throws away the one thing the player cared about -- *which* channel each pedal
  was on -- so pressing `2 channels` again gave an empty split beside a series
  of pedals rather than the board they had a moment ago.

  **It could not have worked as the code stood.** `RejoinSplit` rescued each
  pedal's `PresetStage` *values* and re-inserted them with `InsertPlugin`, which
  mints a fresh id -- so a collapse silently renumbered every pedal in the
  split, and nothing afterwards could say which one had been where. Ids are
  documented as stable identity; this was quietly breaking that. `TakeNode` and
  `InsertNode` detach and attach a node *keeping* its id, and the collapse uses
  them, which is a fix worth having on its own.

  The layout is remembered **in the preset**, so collapse, save, reload, expand
  still restores it and switching preset and back does not lose it. It names
  pedals by id rather than by position, which is what makes editing while
  flattened degrade sensibly: a pedal removed since is skipped, one added stays
  on the trunk, and if nothing remembered survives the restore declines and the
  caller makes a plain empty split. The memory is spent on use -- leaving it
  would mean a second round trip restoring a layout two edits stale.

  Everything about the split comes back, not only the pedals: the splitter's
  mode, frequency and sends, and both channels' levels and mutes. Losing a
  crossover on the way through would be the same surprise as losing the layout.

  Absent when there is nothing to remember and omitted from the file then, the
  same rule as ids (SF-46), the splitter (SF-75) and the gate (SF-85). A
  *malformed* one is ignored rather than refused, which is the one place this
  codebase does that: it is a convenience the board left itself, not something
  the player wrote, and taking a preset away over it would trade a file for a
  layout that can be rebuilt by hand in seconds.

  *Acceptance: collapse and expand is a round trip -- same trunk, same pedals on
  the same channels in the same order, same splitter and levels.*

**Verification.** `ctest` is green: 30 tests, one new.

`RejoinSplit` and `RestoreSplit` moved out of `main.cpp` into
`src/preset/SplitEdit.{h,cpp}` to make any of this checkable -- they are graph
surgery and touch no engine, no plugins and no files, so `split_edit_test` (32
checks) asserts the round trip directly: the trunk comes back identical, each
channel has its own pedals in order, ids survive the collapse, and the splitter
and both levels return with them. Then the cases a real board hits: a pedal
removed while flattened, one added, and every remembered pedal gone -- which
declines rather than leaving a half-built split behind.

Two things the injected regression turned up. Putting the old id-minting
collapse back fails the round trip, as intended -- but it did so by
**segfaulting**, because the test dereferenced the split a failed restore never
created. That is the same flaw Phase B recorded ("it indexed a vector it had
only checked the size of, so a failure segfaulted instead of naming the broken
assertion"), and it is now guarded: the same injection reports six named
failures instead of a crash.

### Suggested first few sessions

1. SF-1, SF-2 — get the build talking to `lilv` at all.
2. SF-3, SF-4 — first sound through a real plugin.
3. SF-5, SF-6 — prove `atom:Path`/state works with our own NAM plugin.
4. SF-7 — prove the worker extension with the IR loader.

That gets to a real, audible "one plugin hosted correctly" milestone before
any chain/preset/MIDI complexity is added.
