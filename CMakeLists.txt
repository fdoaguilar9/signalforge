cmake_minimum_required(VERSION 3.16)

project(Signalforge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)

pkg_check_modules(LILV IMPORTED_TARGET lilv-0)
if(NOT LILV_FOUND)
	message(FATAL_ERROR "lilv-0 not found via pkg-config. Install the development package, e.g.:\n  sudo apt-get install liblilv-dev")
endif()

pkg_check_modules(JACK IMPORTED_TARGET jack)
if(NOT JACK_FOUND)
	message(FATAL_ERROR "jack not found via pkg-config. Install the development package, e.g.:\n  sudo apt-get install libjack-jackd2-dev")
endif()

pkg_check_modules(LV2 IMPORTED_TARGET lv2)
if(NOT LV2_FOUND)
	message(FATAL_ERROR "lv2 core headers not found via pkg-config. Install the development package, e.g.:\n  sudo apt-get install lv2-dev")
endif()

# SDL2 carries the window and input for the Dear ImGui UI. Unlike the three
# above it is not required: a headless build (a build host, a CI image, a Pi
# running the board with no screen attached) is a supported way to use this.
pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
if(SDL2_FOUND)
	message(STATUS "SDL2 ${SDL2_VERSION} found -- building with the UI.")
else()
	message(STATUS "SDL2 not found -- building headless. --gui will be unavailable.\n"
		"  Install the development package to enable it, e.g.:\n"
		"    sudo apt-get install libsdl2-dev")
endif()

# A model or impulse response for the path_property test. There is no file we
# can assume exists, so the test skips without one.
set(SIGNALFORGE_TEST_MODEL "" CACHE FILEPATH "A .nam/.wav file for the path_property test")

add_subdirectory(src)

enable_testing()

# Dependency-free smoke test: proves the binary runs and lilv/jack/json are
# usable. Needs no audio server, no sound card, and no LV2 plugins installed.
add_test(NAME selftest COMMAND signalforge)

# MIDI message parsing: pure logic, no JACK/audio/plugins needed.
add_executable(midi_event_test tests/MidiEventTest.cpp src/midi/MidiEvent.cpp)
add_test(NAME midi_event COMMAND midi_event_test)

# MIDI mapping table: parsing, round-trip, and press/release edge handling.
# Also pure logic, so no controller needs to be plugged in.
add_executable(midi_map_test tests/MidiMapTest.cpp src/midi/MidiMap.cpp src/midi/MidiEvent.cpp)
target_include_directories(midi_map_test PRIVATE src/third_party)
add_test(NAME midi_map COMMAND midi_map_test)

# MIDI learn: which message gets bound, and the noise rejection that keeps a
# chattering controller from winning the race against a human foot.
add_executable(midi_learn_test tests/MidiLearnTest.cpp src/midi/MidiLearn.cpp src/midi/MidiMap.cpp
	src/midi/MidiEvent.cpp)
target_include_directories(midi_learn_test PRIVATE src/third_party)
add_test(NAME midi_learn COMMAND midi_learn_test)

# The crossfade ramp: shape, length, and what happens across block boundaries.
# Pure arithmetic, so the sound of a switch is measurable without an audio
# server -- only the choice between the two curves needs ears.
add_executable(crossfade_test tests/CrossfadeTest.cpp src/audio/Crossfade.cpp)
add_test(NAME crossfade COMMAND crossfade_test)

# Remembered window size, last preset, and crossfade settings: every way a
# settings file can be wrong, and the fact that none of them stops the board.
add_executable(settings_test tests/SettingsTest.cpp src/app/Settings.cpp src/audio/Crossfade.cpp src/audio/AudioConfig.cpp)
target_link_libraries(settings_test PkgConfig::JACK)
target_include_directories(settings_test PRIVATE src/third_party)
add_test(NAME settings COMMAND settings_test)

# The audio backend config model: JSON round-trip, clamping, and ALSA device
# enumeration on whatever machine this runs on. Never starts a JACK server --
# IsJackServerRunning only probes.
add_executable(audio_config_test tests/AudioConfigTest.cpp src/audio/AudioConfig.cpp)
target_link_libraries(audio_config_test PkgConfig::JACK)
add_test(NAME audio_config COMMAND audio_config_test)

# The jackd lifecycle manager. Exercises the failure paths (no server, no
# jackd, or a bogus device) that are the only ones a plain CI sandbox can
# reach -- the happy path (spawning a real jackd against a real card) is
# covered live, not here.
add_executable(jack_server_test tests/JackServerTest.cpp src/jack/JackServer.cpp src/audio/AudioConfig.cpp)
target_link_libraries(jack_server_test PkgConfig::JACK)
add_test(NAME jack_server COMMAND jack_server_test)

# The tuning table SF-33's by-ear pass was made against. Cheap to run, and it
# fails if the curves ever stop being what they claim.
add_test(NAME crossfade_tuning COMMAND signalforge --tune-crossfade)

# With no preset named, the board resumes the one it was last playing -- and
# survives that preset having been deleted since. Needs no audio server: preset
# resolution happens before anything is opened.
add_test(NAME resume_preset
	COMMAND ${CMAKE_COMMAND}
		-DSIGNALFORGE=$<TARGET_FILE:signalforge>
		-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunResumePresetTest.cmake
)

# The UI skeleton: open a window, draw, shut down. SDL's dummy video driver
# means this needs no display, no sound card and no plugins -- and it still
# runs the real AppWindow/BoardView code, not a stand-in. Skipped, not failed,
# on a build configured without SDL2.
add_test(NAME ui_smoke COMMAND signalforge --test-ui 60)
set_tests_properties(ui_smoke PROPERTIES
	ENVIRONMENT "SDL_VIDEODRIVER=dummy"
	SKIP_REGULAR_EXPRESSION "SKIPPED")

# The hand-written mapping fixture must load through the real binary, comments
# and all -- that's the SF-21 acceptance criterion. No hardware needed.
add_test(NAME midi_map_file
	COMMAND signalforge --check-midi-map ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/example-mapping.json)

# Preset schema round-trip over a hand-written fixture. Pure parsing, so it
# needs no plugins, no audio server, and no sound card.
add_test(NAME preset_roundtrip
	COMMAND signalforge --preset-roundtrip ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/roundtrip-preset.json)

# Headless DSP test over a real plugin chain. Skipped automatically when the
# plugin isn't installed, so this stays green on a bare CI image while still
# doing the real work (sweep through the chain, NaN/Inf + frame-count checks)
# wherever the plugin is present.
add_test(NAME headless_chain
	COMMAND ${CMAKE_COMMAND}
		-DSIGNALFORGE=$<TARGET_FILE:signalforge>
		-DPLUGIN_URI=http://kxstudio.sf.net/carla/plugins/audiogain
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunHeadlessChainTest.cmake
)
set_tests_properties(headless_chain PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED")

# Capture a preset from a live chain, then round-trip it. Also skipped when
# the plugin isn't installed.
add_test(NAME preset_save
	COMMAND ${CMAKE_COMMAND}
		-DSIGNALFORGE=$<TARGET_FILE:signalforge>
		-DPLUGIN_URI=http://kxstudio.sf.net/carla/plugins/audiogain
		-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunPresetSaveTest.cmake
)
set_tests_properties(preset_save PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED")

# A chain rebuilt from a preset must produce bit-identical audio to the one
# the preset was captured from.
add_test(NAME preset_restore
	COMMAND ${CMAKE_COMMAND}
		-DSIGNALFORGE=$<TARGET_FILE:signalforge>
		-DPLUGIN_URI=http://kxstudio.sf.net/carla/plugins/audiogain
		-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunPresetRestoreTest.cmake
)
set_tests_properties(preset_restore PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED")

# Preset library: switching presets by CLI arg changes the chain, every
# resolution form agrees, and a manifest overrides order and membership.
add_test(NAME preset_library
	COMMAND ${CMAKE_COMMAND}
		-DSIGNALFORGE=$<TARGET_FILE:signalforge>
		-DPLUGIN_URI=http://kxstudio.sf.net/carla/plugins/audiogain
		-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunPresetLibraryTest.cmake
)
set_tests_properties(preset_library PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED")

# Adding a stage through the UI's code path yields a chain that builds and
# processes audio, with a real plugin.
add_test(NAME add_stage
	COMMAND ${CMAKE_COMMAND}
		-DSIGNALFORGE=$<TARGET_FILE:signalforge>
		-DPLUGIN_URI=http://kxstudio.sf.net/carla/plugins/audiogain
		-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunAddStageTest.cmake
)
set_tests_properties(add_stage PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED")

# A file chosen in the picker actually loads into the running chain. Needs a
# real model/IR file as well as a plugin, so it skips unless one is named with
# -DSIGNALFORGE_TEST_MODEL=<path>.
add_test(NAME path_property
	COMMAND ${CMAKE_COMMAND}
		-DSIGNALFORGE=$<TARGET_FILE:signalforge>
		-DPLUGIN_URI=http://github.com/mikeoliphant/neural-amp-modeler-lv2
		-DTEST_MODEL=${SIGNALFORGE_TEST_MODEL}
		-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunPathPropertyTest.cmake
)
set_tests_properties(path_property PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED")

# A preset naming an uninstalled plugin loses that stage and keeps playing --
# and the audio that comes out is exactly the chain of what was left.
add_test(NAME missing_plugin
	COMMAND ${CMAKE_COMMAND}
		-DSIGNALFORGE=$<TARGET_FILE:signalforge>
		-DPLUGIN_URI=http://kxstudio.sf.net/carla/plugins/audiogain
		-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunMissingPluginTest.cmake
)
set_tests_properties(missing_plugin PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED")

# Bypass removes a stage from the signal path exactly, not approximately.
add_test(NAME bypass
	COMMAND ${CMAKE_COMMAND}
		-DSIGNALFORGE=$<TARGET_FILE:signalforge>
		-DPLUGIN_URI=http://kxstudio.sf.net/carla/plugins/audiogain
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunBypassTest.cmake
)
set_tests_properties(bypass PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED")
