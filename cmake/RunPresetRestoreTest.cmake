# SF-14 acceptance: a chain rebuilt from a saved preset must produce
# bit-identical audio to the chain it was captured from.
#
# Runs the same sweep twice -- once through a chain configured on the
# command line, once through a chain rebuilt from a preset saved from that
# same configuration -- and compares the output hashes.
#
# Skips if ${PLUGIN_URI} isn't installed; see RunHeadlessChainTest.cmake.

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

function(extract_hash output_text out_var)
	if(NOT output_text MATCHES "output-hash:[ \t]*([0-9a-f]+)")
		message(FATAL_ERROR "No output hash in:\n${output_text}")
	endif()
	set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# Pass A: configure the chain directly.
execute_process(
	COMMAND ${SIGNALFORGE} --test-chain ${PLUGIN_URI} --control 0:gain=2.5 --expect-signal --seconds 0.5
	OUTPUT_VARIABLE direct_output
	RESULT_VARIABLE direct_result
)

if(NOT direct_result EQUAL 0)
	message(FATAL_ERROR "Direct chain test failed (exit ${direct_result}):\n${direct_output}")
endif()

extract_hash("${direct_output}" direct_hash)

# Save that same configuration to a preset.
set(preset_file "${CMAKE_CURRENT_BINARY_DIR}/restore-test.json")

execute_process(
	COMMAND ${SIGNALFORGE} --save-preset ${preset_file} ${PLUGIN_URI} --control 0:gain=2.5 --name "Restore Test"
	RESULT_VARIABLE save_result
)

if(NOT save_result EQUAL 0)
	message(FATAL_ERROR "--save-preset failed (exit ${save_result})")
endif()

# Pass B: rebuild from the preset.
execute_process(
	COMMAND ${SIGNALFORGE} --test-chain --preset ${preset_file} --expect-signal --seconds 0.5
	OUTPUT_VARIABLE restored_output
	RESULT_VARIABLE restored_result
)

if(NOT restored_result EQUAL 0)
	message(FATAL_ERROR "Preset-restored chain test failed (exit ${restored_result}):\n${restored_output}")
endif()

extract_hash("${restored_output}" restored_hash)

if(NOT direct_hash STREQUAL restored_hash)
	message(FATAL_ERROR
		"Preset did not reproduce its saved audio.\n"
		"  direct chain:    ${direct_hash}\n"
		"  from preset:     ${restored_hash}")
endif()

message(STATUS "Bit-identical output from preset: ${restored_hash}")

file(REMOVE ${preset_file})
