# SF-27: a plugin appended to a captured preset -- exactly what the UI's "Add
# stage" button does -- produces a chain that builds and processes audio.
#
# The UI half (click, search, which plugin was chosen) is covered by the
# ui_smoke test with no plugins installed at all. This is the half that needs a
# real plugin, so it skips when one isn't there; see RunHeadlessChainTest.cmake.

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

set(preset_dir "${CMAKE_CURRENT_BINARY_DIR}/add-stage-test")
file(REMOVE_RECURSE ${preset_dir})
file(MAKE_DIRECTORY ${preset_dir})

# A one-stage starting point, the shallowest chain the engine will accept.
file(WRITE ${preset_dir}/one.json
"{\n  \"name\": \"One\",\n  \"chain\": [\n    { \"uri\": \"${PLUGIN_URI}\", \"bypassed\": false, \"controls\": {} }\n  ]\n}\n")

execute_process(
	COMMAND ${SIGNALFORGE} --test-add-stage one ${PLUGIN_URI} --preset-dir ${preset_dir}
	OUTPUT_VARIABLE output
	ERROR_VARIABLE errors
	RESULT_VARIABLE result
)

message("${output}${errors}")

if(NOT result EQUAL 0)
	message(FATAL_ERROR "adding a stage failed (exit ${result})")
endif()

if(NOT output MATCHES "1 -> 2 stage")
	message(FATAL_ERROR "expected the chain to grow from 1 to 2 stages")
endif()
