# Runs the headless chain test against ${PLUGIN_URI}, reporting a skip if
# that plugin isn't installed on this machine -- so a bare CI image without
# the LV2 plugin set doesn't report a spurious failure.
#
# The skip is signalled by printing "SKIPPED" and exiting 0; the test's
# SKIP_REGULAR_EXPRESSION property turns that into a real ctest skip.

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

execute_process(
	COMMAND ${SIGNALFORGE} --test-chain ${PLUGIN_URI} --expect-signal
	RESULT_VARIABLE test_result
)

if(NOT test_result EQUAL 0)
	message(FATAL_ERROR "Headless chain test failed (exit ${test_result})")
endif()
