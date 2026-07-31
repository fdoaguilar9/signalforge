# SF-31: a preset naming a plugin that isn't installed must lose that stage,
# say so, and keep playing -- not refuse to load, and not crash.
#
# The acceptance criterion is "edit a preset's URI to garbage and confirm
# graceful degradation", so that is literally what this does: the same
# two-stage chain is built twice, once intact and once with the first stage's
# URI replaced by a URI that cannot exist.
#
# Comparing output hashes is what makes it meaningful. A degraded board that
# played silence, or that ran the surviving stage twice, or that restored the
# missing stage's control values into the surviving one, would all still
# "load" -- and all three would sound wrong. The hash says the chain that came
# out is exactly the chain of everything that was left.
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

set(preset_dir "${CMAKE_CURRENT_BINARY_DIR}/missing-plugin-test")
file(REMOVE_RECURSE ${preset_dir})
file(MAKE_DIRECTORY ${preset_dir})

set(missing_uri "urn:signalforge:no-such-plugin")

# Distinct gains, so restoring the wrong stage's controls into the survivor
# would change the sound rather than cancelling out.
file(WRITE ${preset_dir}/intact.json
"{\n  \"name\": \"Intact\",\n  \"chain\": [\n"
"    { \"uri\": \"${PLUGIN_URI}\", \"controls\": { \"gain\": 3 } },\n"
"    { \"uri\": \"${PLUGIN_URI}\", \"controls\": { \"gain\": 7 } }\n  ]\n}\n")

# The same file with the *first* stage's URI edited to garbage. First, not
# last, so every surviving stage shifts position -- the case where restoring
# state by chain index instead of file index would put stage 0's settings on
# the wrong plugin.
file(WRITE ${preset_dir}/broken.json
"{\n  \"name\": \"Broken\",\n  \"chain\": [\n"
"    { \"uri\": \"${missing_uri}\", \"controls\": { \"gain\": 3 } },\n"
"    { \"uri\": \"${PLUGIN_URI}\", \"controls\": { \"gain\": 7 } }\n  ]\n}\n")

# What the degraded board should sound like: the surviving stage, alone, with
# its own settings.
file(WRITE ${preset_dir}/survivor.json
"{\n  \"name\": \"Survivor\",\n  \"chain\": [\n"
"    { \"uri\": \"${PLUGIN_URI}\", \"controls\": { \"gain\": 7 } }\n  ]\n}\n")

# Every stage missing: there is no chain left, so this one *must* fail.
file(WRITE ${preset_dir}/hopeless.json
"{\n  \"name\": \"Hopeless\",\n  \"chain\": [\n"
"    { \"uri\": \"${missing_uri}\", \"controls\": {} }\n  ]\n}\n")

function(run_preset name out_hash out_output out_result)
	execute_process(
		COMMAND ${SIGNALFORGE} --test-chain --preset ${name} --preset-dir ${preset_dir} --seconds 0.5
		OUTPUT_VARIABLE output
		ERROR_VARIABLE errors
		RESULT_VARIABLE result
	)

	set(combined "${output}${errors}")
	set(hash "")

	if(combined MATCHES "output-hash:[ \t]*([0-9a-f]+)")
		set(hash "${CMAKE_MATCH_1}")
	endif()

	set(${out_hash} "${hash}" PARENT_SCOPE)
	set(${out_output} "${combined}" PARENT_SCOPE)
	set(${out_result} "${result}" PARENT_SCOPE)
endfunction()

run_preset(intact hash_intact output_intact result_intact)
run_preset(broken hash_broken output_broken result_broken)
run_preset(survivor hash_survivor output_survivor result_survivor)
run_preset(hopeless hash_hopeless output_hopeless result_hopeless)

message("${output_broken}")

# 1. It loads at all, and processes audio.
if(NOT result_broken EQUAL 0)
	message(FATAL_ERROR
		"A preset with one uninstalled plugin should still play, but the run failed (exit ${result_broken}):\n"
		"${output_broken}")
endif()

# 2. It says which stage went, and where it was in the file.
if(NOT output_broken MATCHES "skipped-stages: 1")
	message(FATAL_ERROR "The degraded run did not report exactly one skipped stage:\n${output_broken}")
endif()

if(NOT output_broken MATCHES "skipped stage 0: ${missing_uri}")
	message(FATAL_ERROR "The degraded run did not name the missing stage and its position:\n${output_broken}")
endif()

if(NOT output_broken MATCHES "plugin not installed")
	message(FATAL_ERROR "The degraded run produced no warning about the missing plugin:\n${output_broken}")
endif()

# 3. What came out is exactly the surviving stage, with the surviving stage's
#    own control values.
if(NOT hash_broken STREQUAL hash_survivor)
	message(FATAL_ERROR
		"The degraded chain is not the chain of what was left.\n"
		"  preset with a missing first stage: ${hash_broken}\n"
		"  the surviving stage on its own:    ${hash_survivor}\n"
		"${output_broken}")
endif()

# Sanity: the intact chain must sound different, or every check above would
# also pass in a build where nothing was skipped at all -- or where everything
# was.
if(hash_intact STREQUAL hash_survivor)
	message(FATAL_ERROR "The two-stage chain is indistinguishable from one stage: ${hash_intact}")
endif()

if(NOT output_intact MATCHES "skipped-stages: 0")
	message(FATAL_ERROR "An intact preset reported skipped stages:\n${output_intact}")
endif()

# 4. Nothing left to play is still an error: degrading to silence would be a
#    worse lie than refusing.
if(result_hopeless EQUAL 0)
	message(FATAL_ERROR "A preset whose every plugin is missing should fail, but it succeeded:\n${output_hopeless}")
endif()

if(NOT output_hopeless MATCHES "none of them are installed")
	message(FATAL_ERROR "A preset with no installable stage did not say why it failed:\n${output_hopeless}")
endif()

message(STATUS "Missing plugins degrade gracefully: intact=${hash_intact} degraded=${hash_broken} survivor=${hash_survivor}")
