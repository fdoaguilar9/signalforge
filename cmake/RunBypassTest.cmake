# SF-22: a bypassed stage must be removed from the signal path *exactly* --
# its input copied to its output, sample for sample -- not merely quietened
# or approximated.
#
# Runs the same sweep through a two-stage gain chain in four bypass
# combinations and compares output hashes against chains that are genuinely
# equivalent:
#
#   both bypassed    == a single bypassed stage (pure passthrough)
#   first bypassed   == second bypassed == a single active gain stage
#
# Comparing hashes rather than levels is what makes this meaningful: an
# almost-right bypass (an extra copy, a stale block, an off-by-one) still
# lands on the right peak.
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

# Runs --test-chain with the given extra arguments and returns its hash.
function(chain_hash description out_var)
	execute_process(
		COMMAND ${SIGNALFORGE} --test-chain ${ARGN} --expect-signal --seconds 0.5
		OUTPUT_VARIABLE test_output
		RESULT_VARIABLE test_result
	)

	if(NOT test_result EQUAL 0)
		message(FATAL_ERROR "${description} failed (exit ${test_result}):\n${test_output}")
	endif()

	if(NOT test_output MATCHES "output-hash:[ \t]*([0-9a-f]+)")
		message(FATAL_ERROR "No output hash for ${description}:\n${test_output}")
	endif()

	set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

set(two ${PLUGIN_URI} ${PLUGIN_URI} --control 0:gain=4 --control 1:gain=4)
set(one ${PLUGIN_URI} --control 0:gain=4)

chain_hash("two stages, none bypassed" hash_none ${two})
chain_hash("two stages, both bypassed" hash_both ${two} --bypass 0 --bypass 1)
chain_hash("two stages, first bypassed" hash_first ${two} --bypass 0)
chain_hash("two stages, second bypassed" hash_second ${two} --bypass 1)

chain_hash("one stage, active" hash_one_gain ${one})
chain_hash("one stage, bypassed" hash_passthrough ${one} --bypass 0)

if(NOT hash_both STREQUAL hash_passthrough)
	message(FATAL_ERROR
		"Bypassing every stage did not produce a clean passthrough.\n"
		"  both bypassed:      ${hash_both}\n"
		"  single passthrough: ${hash_passthrough}")
endif()

if(NOT hash_first STREQUAL hash_one_gain)
	message(FATAL_ERROR
		"Bypassing the first stage did not leave the second stage's audio intact.\n"
		"  first bypassed: ${hash_first}\n"
		"  one gain stage: ${hash_one_gain}")
endif()

if(NOT hash_second STREQUAL hash_one_gain)
	message(FATAL_ERROR
		"Bypassing the second stage did not leave the first stage's audio intact.\n"
		"  second bypassed: ${hash_second}\n"
		"  one gain stage:  ${hash_one_gain}")
endif()

# Sanity: bypass has to actually change the audio, or every check above
# would also pass in a build where the flag does nothing at all.
if(hash_none STREQUAL hash_one_gain OR hash_none STREQUAL hash_passthrough)
	message(FATAL_ERROR "The un-bypassed chain is indistinguishable from a bypassed one: ${hash_none}")
endif()

message(STATUS "Bypass is exact: passthrough=${hash_passthrough} one-stage=${hash_one_gain} both-stages=${hash_none}")
