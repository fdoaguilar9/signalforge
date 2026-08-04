# Signalforge

A minimal, standalone, pedalboard-style host for LV2 plugins, built for
guitarists. Chain LV2 effects sequentially (including
[neural-amp-modeler-lv2](https://github.com/mikeoliphant/neural-amp-modeler-lv2)
for amp modeling), save the chain as a preset, and switch presets live from
any class-compliant USB MIDI foot controller.

No drag-and-drop, no node-based patch bay — a chain you read top to bottom,
which can divide once into two parallel channels and merge back. You build it
with buttons that say what they do, save it, and stomp through it.

The interface is built against the design bundle in
`design/design_handoff_pedalboard_ui/` — its tokens are the authority for
colours, sizes and spacing, and `src/ui/Theme.h` is their transcription.

## Status

In progress. The engine, presets, and MIDI control all work, and the UI
now covers the board: switch presets, edit and reorder the chain, move
controls, pick model files, and bind a footswitch to any of it without
touching the console. See [docs/PLAN.md](docs/PLAN.md) for the architecture and
[docs/TASKS.md](docs/TASKS.md) for what is and isn't done.

## Building

```sh
sudo apt-get install liblilv-dev libjack-jackd2-dev lv2-dev libsdl2-dev
cmake -B build && cmake --build build -j
ctest --test-dir build
```

One test needs a model or impulse response file to run, and there isn't
one it can assume exists. Point it at yours to enable it:

```sh
cmake -B build -DSIGNALFORGE_TEST_MODEL="$HOME/models/some capture.nam"
```

`libsdl2-dev` is optional and carries the window: without it everything
still builds and runs, `--gui` is unavailable, and the UI test reports
as skipped rather than failing. The other three are required.

## Running

```sh
./scripts/run.sh 01-clean --gui
```

The script starts the board under `pw-jack` and patches its audio to the
default sound card; MIDI controllers connect themselves. Drop `--gui` to
run headless. Console commands (`next`, `prev`, `bypass <stage>`,
`status`, `learn <action>`, `quit`) work either way; with `--gui` the
same actions have buttons, including a "learn" button next to anything a
MIDI message can drive.

Naming a preset is optional — without one the board resumes whichever it
was playing last, so `./scripts/run.sh --gui` is a complete command line.

A preset that names a plugin you don't have installed still plays: that
stage is skipped, the window says so, and saving is blocked until the
plugin is back so the preset file keeps it.

## Settings

`~/.config/signalforge/settings.json` (or `$XDG_CONFIG_HOME`), written on
exit and on every preset switch — a pedalboard gets switched off at the
wall, not quit:

```json
{
  "window": { "width": 720, "height": 560 },
  "last_preset": "01-clean",
  "crossfade": { "milliseconds": 12, "curve": "equal-power" }
}
```

The crossfade is how long a preset switch takes and the shape of its
ramp; `equal-power` holds the loudness steady between two different
sounds, `linear` suits two takes of nearly the same one. Try a value
before keeping it with `--crossfade-ms 8` / `--crossfade-curve linear`,
and see `--tune-crossfade` for what each length measures. Anything in
this file can be missing or wrong without stopping the board.

## License

GPLv3 — see [LICENSE](LICENSE). Dear ImGui is vendored under
`src/third_party/imgui` and is MIT-licensed; its licence travels with it.
