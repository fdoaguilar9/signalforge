# The board's edge does not move when the pedals on it change.
#
# chain_schedule_test asserts this over the plan, with no plugin in sight. What
# it cannot reach is the part that actually broke: a real chain, instantiated
# against real LV2 plugins, whose *boundary port count* is what the JACK engine
# registers once at Start() and refuses to see change afterwards. A board that
# came out two channels wide only when its last pedal happened to be stereo
# would drop every edit that added or removed one -- which is the bug this is
# here to keep fixed.
#
# So the claim is about the port counts a built chain presents, on both board
# widths, with a mono pedal and with a stereo one: always two out.
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

# Runs one board and asserts the channel line reads exactly `expected`.
function(expect_channels description board_inputs expected)
	execute_process(
		COMMAND ${SIGNALFORGE} --test-chain ${ARGN}
			--board-inputs ${board_inputs} --seconds 0.2 --expect-signal
		OUTPUT_VARIABLE output
		RESULT_VARIABLE result
	)

	if(NOT result EQUAL 0)
		message(FATAL_ERROR "${description}: signalforge failed (exit ${result})\n${output}")
	endif()

	if(NOT output MATCHES "channels:[ \t]*${expected}")
		message(FATAL_ERROR
			"${description}: expected '${expected}'.\n${output}")
	endif()

	message("ok    ${description}: ${expected}")
endfunction()

# A mono board still hands over a normal stereo pair -- the thing the JACK
# engine used to fake with a mirrored port.
expect_channels("a mono pedal on a mono board" 1 "1 in / 2 out" ${PLUGIN_URI})

# ...and a stereo board is stereo at both ends, with the mono pedal in the
# middle fed a fold of the two inputs and its one output widened again.
expect_channels("a mono pedal on a stereo board" 2 "2 in / 2 out" ${PLUGIN_URI})

# The sidechain half of the same idea (optional -- named with
# -DSIDECHAIN_URI=<uri>, skipped quietly when it isn't installed).
#
# A stereo pedal with a key input has three audio input ports and used to be
# refused outright as something this host could not adapt. A port count is not
# a channel count: two of those three are the channels, and the board's width
# is unmoved by which pedal is on it.
if(SIDECHAIN_URI)
	string(FIND "${plugin_list}" "${SIDECHAIN_URI}" sidechain_position)

	if(sidechain_position EQUAL -1)
		message("(no ${SIDECHAIN_URI} installed; skipping the sidechain checks)")
	else()
		expect_channels("a keyed stereo pedal on a mono board" 1 "1 in / 2 out" ${SIDECHAIN_URI})
		expect_channels("a keyed stereo pedal on a stereo board" 2 "2 in / 2 out" ${SIDECHAIN_URI})
		expect_channels("a keyed pedal after a plain one" 1 "1 in / 2 out" ${PLUGIN_URI} ${SIDECHAIN_URI})
	endif()
endif()
