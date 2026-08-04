add_executable(signalforge main.cpp app/Settings.cpp audio/Crossfade.cpp audio/AudioConfig.cpp audio/PitchDetect.cpp audio/Metronome.cpp audio/NoiseGate.cpp lv2/PortInfo.cpp lv2/PathProperty.cpp lv2/UridMap.cpp lv2/Worker.cpp lv2/PluginStage.cpp lv2/Chain.cpp lv2/ChainSchedule.cpp
	jack/JackEngine.cpp jack/JackServer.cpp midi/MidiEvent.cpp midi/MidiMap.cpp midi/MidiLearn.cpp offline/OfflineEngine.cpp preset/Preset.cpp
	preset/ChainGraph.cpp
	preset/ChainPreset.cpp preset/SplitEdit.cpp
	preset/PresetLibrary.cpp)

find_package(Threads REQUIRED)

target_link_libraries(signalforge PRIVATE PkgConfig::LILV PkgConfig::JACK PkgConfig::LV2 Threads::Threads)
target_include_directories(signalforge PRIVATE third_party ${CMAKE_CURRENT_SOURCE_DIR})

# The UI is optional. Everything that matters -- the engine, presets, MIDI --
# works headless, and a build host without SDL2 development headers should get
# a working program rather than a configure error.
if(SDL2_FOUND)
	set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

	add_library(imgui STATIC
		${IMGUI_DIR}/imgui.cpp
		${IMGUI_DIR}/imgui_draw.cpp
		${IMGUI_DIR}/imgui_tables.cpp
		${IMGUI_DIR}/imgui_widgets.cpp
		${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
		${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp)

	target_include_directories(imgui PUBLIC ${IMGUI_DIR})
	target_link_libraries(imgui PUBLIC PkgConfig::SDL2)

	# Vendored third-party code, held to its own standards rather than ours.
	target_compile_options(imgui PRIVATE -w)

	target_sources(signalforge PRIVATE ui/AppWindow.cpp ui/Theme.cpp ui/Fonts.cpp ui/Widgets.cpp ui/Meter.cpp ui/BoardView.cpp ui/ChainView.cpp ui/StageCard.cpp ui/AudioSettingsView.cpp ui/UiSelfTest.cpp)
	target_link_libraries(signalforge PRIVATE imgui)
	target_compile_definitions(signalforge PRIVATE SIGNALFORGE_GUI)

	# The last place Fonts.cpp looks for resources/, so that a binary run
	# straight out of a build tree finds the fonts without anyone having to set
	# an environment variable. Deliberately last in that search: an installed
	# copy must never reach back into a source checkout that has since moved on.
	target_compile_definitions(signalforge PRIVATE
		SIGNALFORGE_SOURCE_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../resources")
endif()
