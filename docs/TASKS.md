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

## Milestone 8 — Binary split/merge (later)

- [ ] **SF-34**: Extend the chain data model from a flat list to the
  `SeriesNode`/`SplitNode`/`MergeNode` tree described in PLAN.md §4.2,
  without changing v1 behavior (still produces linear chains).
- [ ] **SF-35**: Add exactly one split/merge pair to the UI (PLAN.md
  explicitly scopes this to *binary* — no nested/multiple splits).

---

### Suggested first few sessions

1. SF-1, SF-2 — get the build talking to `lilv` at all.
2. SF-3, SF-4 — first sound through a real plugin.
3. SF-5, SF-6 — prove `atom:Path`/state works with our own NAM plugin.
4. SF-7 — prove the worker extension with the IR loader.

That gets to a real, audible "one plugin hosted correctly" milestone before
any chain/preset/MIDI complexity is added.
