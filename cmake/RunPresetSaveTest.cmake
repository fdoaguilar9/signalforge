# Saves a preset from a live (headless) chain, then round-trips the result
# back through the parser. Skips if ${PLUGIN_URI} isn't installed -- see
# RunHeadlessChainTest.cmake for the skip convention.

execute_process(
	COMMAND ${SIGNALFORGE} --list-plugins
	OUTPUT_VARIABLE plugin_list
	RESULT_VARIABLE list_result
)

if(NOT list_result EQUAL 0)
	message(FATAL_ERROR "signalforge --list-plugins failed (exit ${list_result})")
endif()

string(FIND "${plugin_list}" "${PLUGIN_URI}" uri_position)

if(uri_position EQUAL -1)
	message("SKIPPED: plugin not installed: ${PLUGIN_URI}")
	return()
endif()

set(preset_file "${CMAKE_CURRENT_BINARY_DIR}/saved-preset-test.json")

execute_process(
	COMMAND ${SIGNALFORGE} --save-preset ${preset_file} ${PLUGIN_URI}
		--control 0:gain=2.5 --name "Save Test"
	RESULT_VARIABLE save_result
)

if(NOT save_result EQUAL 0)
	message(FATAL_ERROR "--save-preset failed (exit ${save_result})")
endif()

# The captured control value must actually be in the file, not just the
# TTL default -- otherwise "capture" would be indistinguishable from
# "emit defaults".
file(READ ${preset_file} preset_contents)

string(FIND "${preset_contents}" "\"gain\": 2.5" gain_position)
if(gain_position EQUAL -1)
	message(FATAL_ERROR "Saved preset did not capture the control value:\n${preset_contents}")
endif()

execute_process(
	COMMAND ${SIGNALFORGE} --preset-roundtrip ${preset_file}
	RESULT_VARIABLE roundtrip_result
)

if(NOT roundtrip_result EQUAL 0)
	message(FATAL_ERROR "Saved preset failed to round-trip (exit ${roundtrip_result})")
endif()

file(REMOVE ${preset_file})
