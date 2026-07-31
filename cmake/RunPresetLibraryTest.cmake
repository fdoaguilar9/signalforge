# SF-15 acceptance: switching between presets by CLI argument changes the
# running chain, and a preset resolves identically by index, name, stem and
# path.
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

set(preset_dir "${CMAKE_CURRENT_BINARY_DIR}/preset-library-test")
file(REMOVE_RECURSE ${preset_dir})
file(MAKE_DIRECTORY ${preset_dir})

# Two presets that differ only in a control value, so any difference in
# output can only come from the preset actually being applied.
foreach(spec "quiet;1.0" "loud;3.0")
	list(GET spec 0 preset_stem)
	list(GET spec 1 gain_value)

	execute_process(
		COMMAND ${SIGNALFORGE} --save-preset ${preset_dir}/${preset_stem}.json ${PLUGIN_URI}
			--control 0:gain=${gain_value} --name "Preset ${preset_stem}"
		RESULT_VARIABLE save_result
	)

	if(NOT save_result EQUAL 0)
		message(FATAL_ERROR "--save-preset ${preset_stem} failed (exit ${save_result})")
	endif()
endforeach()

function(preset_hash query out_var)
	execute_process(
		COMMAND ${SIGNALFORGE} --test-chain --preset ${query} --preset-dir ${preset_dir}
			--expect-signal --seconds 0.5
		OUTPUT_VARIABLE run_output
		RESULT_VARIABLE run_result
	)

	if(NOT run_result EQUAL 0)
		message(FATAL_ERROR "--test-chain --preset ${query} failed (exit ${run_result}):\n${run_output}")
	endif()

	if(NOT run_output MATCHES "output-hash:[ \t]*([0-9a-f]+)")
		message(FATAL_ERROR "No output hash for preset '${query}':\n${run_output}")
	endif()

	set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# Alphabetical scan order: [0]=loud, [1]=quiet.
preset_hash("loud" loud_by_stem)
preset_hash("quiet" quiet_by_stem)

if(loud_by_stem STREQUAL quiet_by_stem)
	message(FATAL_ERROR "Switching presets did not change the chain (both hashed ${loud_by_stem})")
endif()

# The same preset must resolve identically however it's named.
preset_hash("0" loud_by_index)
preset_hash("Preset loud" loud_by_name)
preset_hash("${preset_dir}/loud.json" loud_by_path)

foreach(pair "index;${loud_by_index}" "name;${loud_by_name}" "path;${loud_by_path}")
	list(GET pair 0 how)
	list(GET pair 1 got)

	if(NOT got STREQUAL loud_by_stem)
		message(FATAL_ERROR "Resolving by ${how} gave ${got}, expected ${loud_by_stem}")
	endif()
endforeach()

# A manifest must override both order and membership.
file(WRITE ${preset_dir}/manifest.json "{ \"presets\": [\"quiet.json\"] }")

execute_process(
	COMMAND ${SIGNALFORGE} --list-presets --preset-dir ${preset_dir}
	OUTPUT_VARIABLE manifest_listing
	RESULT_VARIABLE manifest_result
)

if(NOT manifest_result EQUAL 0)
	message(FATAL_ERROR "--list-presets with manifest failed (exit ${manifest_result})")
endif()

if(NOT manifest_listing MATCHES "1 preset")
	message(FATAL_ERROR "Manifest did not restrict membership:\n${manifest_listing}")
endif()

preset_hash("0" manifest_index_zero)

if(NOT manifest_index_zero STREQUAL quiet_by_stem)
	message(FATAL_ERROR "With a manifest, index 0 should be 'quiet' but hashed ${manifest_index_zero}")
endif()

message(STATUS "Preset switching verified: loud=${loud_by_stem} quiet=${quiet_by_stem}")

file(REMOVE_RECURSE ${preset_dir})
