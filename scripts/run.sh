#!/usr/bin/env bash
# Starts signalforge and connects its audio to the default sound card.
#
# MIDI controllers connect themselves (see SF-20); audio doesn't yet, so
# this script does it. It picks the default PipeWire source and sink unless
# SF_INPUT / SF_OUTPUT name something else:
#
#   SF_INPUT=alsa_input.…:capture_MONO ./scripts/run.sh 02-rhythm
#
# The auto-picked input prefers a device that looks like an audio interface
# over one that looks like a plain microphone, and among an interface's own
# ports (e.g. a two-input Scarlett's Mic1/Mic2) prefers whichever one is
# already patched somewhere -- which device name simply cannot tell you.
# That is a guess informed by the last time *something* was connected, not a
# guarantee it's the jack your guitar is in this time: if you hear nothing
# and pw-link shows the wrong port, name it explicitly with SF_INPUT.
#
# Everything after the preset is passed through to signalforge, so e.g.
#   ./scripts/run.sh 01-clean --no-crossfade
# works. With no preset named it resumes whichever one was playing last
# (SF-32), so `./scripts/run.sh --gui` is a complete command line.

set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${SF_BINARY:-$here/build/src/signalforge}"

if [[ ! -x "$binary" ]]; then
	echo "No build at $binary -- run: cmake -B build && cmake --build build -j" >&2
	exit 1
fi

# No preset, or a flag where the preset would be: let the board resume what it
# was last playing rather than picking one here.
preset=()
if [[ $# -gt 0 && "$1" != --* ]]; then
	preset=("$1")
	shift
fi

map=()
if [[ -f "$here/presets/mapping.json" ]]; then
	map=(--midi-map "$here/presets/mapping.json")
fi

# Picking "the first ALSA capture port" is a coin flip on any machine with
# more than one input device plugged in -- a webcam/USB mic and a guitar
# interface both match, and whichever one pw-link happens to list first
# wins, silently, regardless of which one anything is actually plugged into.
# So this prefers a device that looks like an instrument interface (a name
# containing "guitar", "interface", "usb audio", or a well-known
# audio-interface brand) over anything else. SF_INPUT always wins over the
# guess.
find_input()
{
	local ports interface
	ports="$(pw-link -o 2>/dev/null | grep '^alsa_input\..*:capture')"

	interface="$(printf '%s\n' "$ports" |
		grep -iE 'guitar|interface|usb.?audio|scarlett|focusrite|behringer|presonus|steinberg|motu|apogee')"

	if [[ -n "$interface" ]]; then
		# An interface can expose more than one physical input (Mic1/Mic2,
		# AUX0/AUX1, ...), and which one has a guitar plugged into it is not
		# something a device name says -- so among interface ports, prefer
		# one already carrying signal (a live PipeWire connection) over one
		# that isn't, and only fall back to "first listed" when nothing is
		# currently patched anywhere.
		local connected
		connected="$(pw-link -l 2>/dev/null | grep -B1 '^\s*|->' | grep -oE '^\S+' | sort -u)"

		local candidate
		while IFS= read -r candidate; do
			[[ -z "$candidate" ]] && continue
			if grep -qxF "$candidate" <<<"$connected"; then
				printf '%s\n' "$candidate"
				return
			fi
		done <<<"$interface"

		head -n1 <<<"$interface"
		return
	fi

	# Nothing looked like an interface by name -- e.g. a generic USB audio
	# device with no identifying string. Fall back to whatever a plain
	# microphone doesn't call itself, which at least deprioritises the
	# obvious webcam/headset mic case; if there's a real ambiguity left,
	# SF_INPUT is how to resolve it.
	local non_mic
	non_mic="$(printf '%s\n' "$ports" | grep -vi 'mic')"
	printf '%s\n' "${non_mic:-$ports}" | head -n1
}

# Default to whatever PipeWire considers the current source/sink, so this
# keeps working when the interface is unplugged and plugged back in.
input="${SF_INPUT:-$(find_input)}"
output_left="${SF_OUTPUT:-$(pw-link -i 2>/dev/null | grep -m1 '^alsa_output\..*:playback_FL')}"
output_right="${output_left/playback_FL/playback_FR}"

if [[ -z "$input" ]]; then
	echo "Warning: found no ALSA capture port; connect the input by hand." >&2
fi

# The explicit `<&0` matters: bash points a background job's stdin at
# /dev/null unless told otherwise, which would silently kill the console
# commands whenever this script is run from a non-interactive shell.
pw-jack "$binary" --run-preset "${preset[@]}" --preset-dir "$here/presets" "${map[@]}" "$@" <&0 &
app=$!

# Ports don't exist until the client is registered and activated.
for _ in $(seq 1 50); do
	pw-link -i 2>/dev/null | grep -q '^signalforge:in_1$' && break
	sleep 0.1
done

# "File exists" just means it was already patched, which is the state we want.
link() { pw-link "$1" "$2" 2>&1 | grep -v 'File exists' >&2; }

[[ -n "$input" ]] && link "$input" signalforge:in_1
[[ -n "$output_left" ]] && link signalforge:out_1 "$output_left"
[[ -n "$output_right" ]] && link signalforge:out_2 "$output_right"

wait $app
