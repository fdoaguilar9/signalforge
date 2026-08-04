# SF-32: with no preset named, the board comes back up on the one it was last
# playing -- and when that preset has since been renamed or deleted, it starts
# anyway rather than refusing.
#
# This runs the real binary against real files. It needs no plugins and no
# audio server: preset resolution happens before anything is opened, so the run
# is pointed at a JACK server that cannot exist and is expected to fail
# immediately afterwards. What is asserted is what it said before then.
#
# Naming a dead server (rather than just letting it find whatever is running)
# is what keeps this from starting a real board on a developer's machine and
# sitting there.

set(preset_dir "${CMAKE_CURRENT_BINARY_DIR}/resume-test")
file(REMOVE_RECURSE ${preset_dir})
file(MAKE_DIRECTORY ${preset_dir})

# The URIs never get built, so they need not exist: this is about which file is
# chosen, not what is in it.
file(WRITE ${preset_dir}/01-first.json
"{\n  \"name\": \"First\",\n  \"chain\": [ { \"uri\": \"urn:signalforge:test-a\", \"controls\": {} } ]\n}\n")

file(WRITE ${preset_dir}/02-second.json
"{\n  \"name\": \"Second\",\n  \"chain\": [ { \"uri\": \"urn:signalforge:test-b\", \"controls\": {} } ]\n}\n")

set(settings "${preset_dir}/settings.json")

# Runs the board with a dead JACK server and returns everything it said.
function(run_board out_output)
	execute_process(
		COMMAND ${CMAKE_COMMAND} -E env
			JACK_NO_START_SERVER=1
			JACK_DEFAULT_SERVER=signalforge-no-such-server
			${SIGNALFORGE} --run-preset ${ARGN} --preset-dir ${preset_dir} --settings ${settings}
		OUTPUT_VARIABLE output
		ERROR_VARIABLE errors
		RESULT_VARIABLE result
		TIMEOUT 30
	)

	set(${out_output} "${output}${errors}" PARENT_SCOPE)
endfunction()

# 1. Nothing remembered yet: the first run has to pick something rather than
#    ask.
file(WRITE ${settings} "{\n  \"window\": { \"width\": 800, \"height\": 600 }\n}\n")
run_board(first_run)

if(first_run MATCHES "Resuming")
	message(FATAL_ERROR "With nothing remembered, the board claimed to resume:\n${first_run}")
endif()

if(NOT first_run MATCHES "Loading preset 'First'")
	message(FATAL_ERROR "With nothing remembered, the board did not start at the first preset:\n${first_run}")
endif()

# 2. A remembered preset is what plays.
file(WRITE ${settings} "{\n  \"last_preset\": \"02-second\"\n}\n")
run_board(resumed)

if(NOT resumed MATCHES "Resuming '02-second'")
	message(FATAL_ERROR "The board did not resume the preset it was told was last played:\n${resumed}")
endif()

# 3. Naming a preset overrides what was remembered -- otherwise the argument
#    would be quietly ignored on any machine that had run before.
run_board(explicit 01-first)

if(explicit MATCHES "Resuming")
	message(FATAL_ERROR "An explicitly named preset was overridden by the remembered one:\n${explicit}")
endif()

# 4. The remembered preset has been renamed or deleted since. A board that
#    refused to start over a stale settings file would be worse than useless.
file(WRITE ${settings} "{\n  \"last_preset\": \"99-deleted\"\n}\n")
run_board(stale)

if(NOT stale MATCHES "is gone; starting at the first one")
	message(FATAL_ERROR "A remembered preset that no longer exists did not fall back:\n${stale}")
endif()

if(NOT stale MATCHES "Loading preset 'First'")
	message(FATAL_ERROR "After falling back, the board did not start at the first preset:\n${stale}")
endif()

# 5. The crossfade setting is read from the same file and reported, so what is
#    configured and what is running can be told apart.
file(WRITE ${settings} "{\n  \"crossfade\": { \"milliseconds\": 25, \"curve\": \"linear\" }\n}\n")
run_board(configured)

if(NOT configured MATCHES "Crossfade: 25.0 ms, linear")
	message(FATAL_ERROR "The crossfade setting was not picked up from the settings file:\n${configured}")
endif()

# 6. And the command line overrides the file for this run.
#
# That the override is *not* written back is deliberate -- a flag you tried
# should not become permanent -- but it can't be asserted here: settings are
# written at shutdown, and this run never gets that far. It is checked live
# instead; see TASKS.md SF-33.
run_board(overridden --crossfade-ms 3 --crossfade-curve equal-power)

if(NOT overridden MATCHES "Crossfade: 3.0 ms, equal-power")
	message(FATAL_ERROR "--crossfade-ms/--crossfade-curve did not override the settings file:\n${overridden}")
endif()

# --- banks (SF-67) ---
#
# A bank is a subdirectory of the preset directory. Everything above ran
# without one and has to keep working exactly as it did, which is why the bank
# cases come last rather than being folded into the fixtures above.

set(bank_dir "${preset_dir}/Worship set")
file(MAKE_DIRECTORY "${bank_dir}")

file(WRITE "${bank_dir}/w1-ambient.json"
"{\n  \"name\": \"Ambient\",\n  \"chain\": [ { \"uri\": \"urn:signalforge:test-c\", \"controls\": {} } ]\n}\n")

file(WRITE "${bank_dir}/w2-swell.json"
"{\n  \"name\": \"Swell\",\n  \"chain\": [ { \"uri\": \"urn:signalforge:test-d\", \"controls\": {} } ]\n}\n")

# 7. The Default bank still comes first, so a library that has grown a bank
#    still starts where it always did when nothing is remembered.
file(WRITE ${settings} "{\n  \"window\": { \"width\": 800, \"height\": 600 }\n}\n")
run_board(with_bank)

if(NOT with_bank MATCHES "Loading preset 'First'")
	message(FATAL_ERROR "Adding a bank changed where the board starts:\n${with_bank}")
endif()

# 8. A preset inside a bank, named the way settings now writes it.
file(WRITE ${settings} "{\n  \"last_preset\": \"Worship set/w2-swell\"\n}\n")
run_board(in_bank)

if(NOT in_bank MATCHES "Resuming 'Worship set/w2-swell'")
	message(FATAL_ERROR "The board did not resume a preset inside a bank:\n${in_bank}")
endif()

if(NOT in_bank MATCHES "Loading preset 'Swell'")
	message(FATAL_ERROR "Resuming into a bank loaded the wrong preset:\n${in_bank}")
endif()

# 9. An unqualified name still finds a preset in another bank, so a stem that
#    is unique does not have to be spelled out.
file(WRITE ${settings} "{\n  \"last_preset\": \"w1-ambient\"\n}\n")
run_board(unqualified)

if(NOT unqualified MATCHES "Loading preset 'Ambient'")
	message(FATAL_ERROR "An unqualified stem did not resolve into a bank:\n${unqualified}")
endif()

# 10. Naming a bank that exists and a preset that doesn't is refused rather
#     than falling back to a same-named preset somewhere else -- an explicitly
#     named bank means that bank, or nothing.
file(WRITE ${settings} "{\n  \"last_preset\": \"Worship set/01-first\"\n}\n")
run_board(wrong_bank)

if(NOT wrong_bank MATCHES "is gone; starting at the first one")
	message(FATAL_ERROR "A preset named in the wrong bank was found anyway:\n${wrong_bank}")
endif()

if(NOT wrong_bank MATCHES "Loading preset 'First'")
	message(FATAL_ERROR "After refusing a wrong-bank name, the board did not fall back:\n${wrong_bank}")
endif()

# 11. **The bank itself has been deleted since.** The stale-preset case above
#     covers a missing file; this covers a missing folder, which is what
#     happens when someone tidies up their presets between gigs.
file(REMOVE_RECURSE "${bank_dir}")
file(WRITE ${settings} "{\n  \"last_preset\": \"Worship set/w2-swell\"\n}\n")
run_board(bank_gone)

if(NOT bank_gone MATCHES "is gone; starting at the first one")
	message(FATAL_ERROR "A remembered preset whose whole bank was deleted did not fall back:\n${bank_gone}")
endif()

if(NOT bank_gone MATCHES "Loading preset 'First'")
	message(FATAL_ERROR "After a deleted bank, the board did not start at the first preset:\n${bank_gone}")
endif()

message(STATUS "Resume, fallback, banks and crossfade configuration all behave.")
