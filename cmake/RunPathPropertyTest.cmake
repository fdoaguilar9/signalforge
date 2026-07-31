# SF-29: a file chosen in the picker is loaded into the running chain, and the
# plugin agrees that it was.
#
# Unlike the other plugin tests this needs a *file* as well as a plugin, and
# there is no model or impulse response we can assume exists -- so it skips
# unless one is named:
#
#   cmake -B build -DSIGNALFORGE_TEST_MODEL="$HOME/models/some capture.nam"
#
# The UI half of picking a file (which row, which property, what path it
# reports) is covered by the ui_smoke test, which needs no files at all.

if(NOT TEST_MODEL OR NOT EXISTS "${TEST_MODEL}")
	message("SKIPPED: no test model file; set -DSIGNALFORGE_TEST_MODEL=<path> to enable")
	return()
endif()

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

set(preset_dir "${CMAKE_CURRENT_BINARY_DIR}/path-property-test")
file(REMOVE_RECURSE ${preset_dir})
file(MAKE_DIRECTORY ${preset_dir})

# No state and no model: the stage starts with nothing loaded, so the test's
# "it changed" check can't pass by accident.
file(WRITE ${preset_dir}/bare.json
"{\n  \"name\": \"Bare\",\n  \"chain\": [\n    { \"uri\": \"${PLUGIN_URI}\", \"bypassed\": false, \"controls\": {} }\n  ]\n}\n")

execute_process(
	COMMAND ${SIGNALFORGE} --test-path-property bare "${TEST_MODEL}" --preset-dir ${preset_dir}
	OUTPUT_VARIABLE output
	ERROR_VARIABLE errors
	RESULT_VARIABLE result
)

message("${output}${errors}")

if(NOT result EQUAL 0)
	message(FATAL_ERROR "loading a file into the running chain failed (exit ${result})")
endif()

if(NOT output MATCHES "Path property: PASS")
	message(FATAL_ERROR "expected the property to read back as the file that was loaded")
endif()
