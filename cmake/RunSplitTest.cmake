# SF-49: a split has to play, and it has to play the *right* thing.
#
# Everything else about the split is asserted in chain_schedule_test, which
# checks the plan and the arithmetic without a plugin in sight. What that
# cannot reach is the whole path: a preset in the nested "graph" form, loaded
# from disk, translated into a spec, planned, instantiated against real LV2
# plugins, and run through the offline engine. This is that pass.
#
# The claim it makes is exact rather than approximate: a split with one lane
# muted must be sample-for-sample identical to the other lane on its own.
# Hashes rather than levels, for the same reason the bypass test uses them --
# an almost-right merge (a stray copy, a lane summed twice, a gain of 0.999)
# still lands on the right peak.
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

set(work "${CMAKE_CURRENT_BINARY_DIR}/split-test")
file(MAKE_DIRECTORY "${work}")

# One gain stage, in the flat form every preset used before Milestone 9.
file(WRITE "${work}/single.json"
"{
  \"name\": \"Single\",
  \"chain\": [
    { \"uri\": \"${PLUGIN_URI}\", \"bypassed\": false, \"controls\": { \"gain\": 4.0 } }
  ]
}
")

# The same gain stage in lane 0, with lane 1 muted: audibly and numerically
# the same board.
file(WRITE "${work}/muted-lane.json"
"{
  \"name\": \"Muted lane\",
  \"graph\": [
    { \"id\": 1, \"branches\": [
      { \"gain_db\": 0.0, \"muted\": false, \"nodes\": [
        { \"id\": 2, \"uri\": \"${PLUGIN_URI}\", \"bypassed\": false, \"controls\": { \"gain\": 4.0 } }
      ] },
      { \"gain_db\": 0.0, \"muted\": true, \"nodes\": [
        { \"id\": 3, \"uri\": \"${PLUGIN_URI}\", \"bypassed\": false, \"controls\": { \"gain\": 4.0 } }
      ] }
    ] }
  ]
}
")

# Both lanes live. Not compared against anything exact -- two correlated
# copies summing is a level change, not a hash we can predict -- but it must
# not come out the same as one lane alone, or the merge is dropping a lane.
file(WRITE "${work}/both-lanes.json"
"{
  \"name\": \"Both lanes\",
  \"graph\": [
    { \"id\": 1, \"branches\": [
      { \"gain_db\": 0.0, \"muted\": false, \"nodes\": [
        { \"id\": 2, \"uri\": \"${PLUGIN_URI}\", \"bypassed\": false, \"controls\": { \"gain\": 4.0 } }
      ] },
      { \"gain_db\": 0.0, \"muted\": false, \"nodes\": [
        { \"id\": 3, \"uri\": \"${PLUGIN_URI}\", \"bypassed\": false, \"controls\": { \"gain\": 4.0 } }
      ] }
    ] }
  ]
}
")

# A lane that is nothing but a wire, which is how a dry parallel path is
# spelled. It has to build -- an empty lane is legal (SF-45) -- and it has to
# differ from the processed lane alone.
file(WRITE "${work}/dry-lane.json"
"{
  \"name\": \"Dry lane\",
  \"graph\": [
    { \"id\": 1, \"branches\": [
      { \"gain_db\": 0.0, \"muted\": false, \"nodes\": [
        { \"id\": 2, \"uri\": \"${PLUGIN_URI}\", \"bypassed\": false, \"controls\": { \"gain\": 4.0 } }
      ] },
      { \"gain_db\": 0.0, \"muted\": false, \"nodes\": [] }
    ] }
  ]
}
")

# A split whose two lanes come back at different levels, for the live-level
# checks below. Lane B at -12 dB in the file is what a run that changes
# nothing has to reproduce.
file(WRITE "${work}/uneven-lanes.json"
"{
  \"name\": \"Uneven lanes\",
  \"graph\": [
    { \"id\": 1, \"branches\": [
      { \"gain_db\": 0.0, \"muted\": false, \"nodes\": [
        { \"id\": 2, \"uri\": \"${PLUGIN_URI}\", \"bypassed\": false, \"controls\": { \"gain\": 4.0 } }
      ] },
      { \"gain_db\": -12.0, \"muted\": false, \"nodes\": [
        { \"id\": 3, \"uri\": \"${PLUGIN_URI}\", \"bypassed\": false, \"controls\": { \"gain\": 12.0 } }
      ] }
    ] }
  ]
}
")

function(preset_hash description preset out_var)
	execute_process(
		COMMAND ${SIGNALFORGE} --test-chain --preset "${work}/${preset}" --expect-signal --seconds 0.5 ${ARGN}
		OUTPUT_VARIABLE test_output
		RESULT_VARIABLE test_result
	)

	if(NOT test_result EQUAL 0)
		message(FATAL_ERROR "${description} failed (exit ${test_result}):\n${test_output}")
	endif()

	if(NOT test_output MATCHES "skipped-stages:[ \t]*0")
		message(FATAL_ERROR "${description} did not build every stage:\n${test_output}")
	endif()

	if(NOT test_output MATCHES "output-hash:[ \t]*([0-9a-f]+)")
		message(FATAL_ERROR "No output hash for ${description}:\n${test_output}")
	endif()

	set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

preset_hash("one gain stage" single.json hash_single)
preset_hash("a split with one lane muted" muted-lane.json hash_muted)
preset_hash("a split with both lanes live" both-lanes.json hash_both)
preset_hash("a split with a dry lane" dry-lane.json hash_dry)

if(NOT hash_muted STREQUAL hash_single)
	message(FATAL_ERROR
		"A split with one lane muted is not the other lane on its own.\n"
		"  muted lane:  ${hash_muted}\n"
		"  lane alone:  ${hash_single}")
endif()

if(hash_both STREQUAL hash_single)
	message(FATAL_ERROR
		"Both lanes live sounds identical to one lane -- the merge is dropping a lane: ${hash_both}")
endif()

if(hash_dry STREQUAL hash_single OR hash_dry STREQUAL hash_both)
	message(FATAL_ERROR
		"A dry lane in parallel is indistinguishable from something else.\n"
		"  dry + processed: ${hash_dry}\n"
		"  processed alone: ${hash_single}\n"
		"  two processed:   ${hash_both}")
endif()

message(STATUS "Split routing is exact: muted=${hash_muted} single=${hash_single} both=${hash_both} dry=${hash_dry}")

# SF-72: a lane's level set on the *running* chain, with no replan.
#
# This is the only place the whole of that path is exercised. `SetMergeGain` is
# called from a mixer slider, which nothing headless can move, so without a
# way in from the command line the merge index, the dB conversion and the
# atomic the audio thread reads would only ever be checked by reading them.
#
# The claims are about which runs differ from which, not about matching a
# planned reference: the first block sweeps to the new level rather than
# stepping to it, so a level that was set live and the same level planned into
# the file agree everywhere except that one block.
preset_hash("uneven lanes, untouched" uneven-lanes.json hash_uneven)

# Setting a lane to the level it already has must change nothing at all. This
# is the exact one, and it is the one that pins the *lane*: writing -12 dB into
# lane A -- which is at 0 dB -- would be plainly audible, and writing it
# through a bad dB conversion would be too.
preset_hash("lane B set live to the level it already had" uneven-lanes.json hash_noop --lane-gain b=-12)

if(NOT hash_noop STREQUAL hash_uneven)
	message(FATAL_ERROR
		"Setting a lane to the level it already had changed the sound -- the wrong lane, or the wrong "
		"conversion, is being written.\n"
		"  untouched: ${hash_uneven}\n"
		"  b=-12:     ${hash_noop}")
endif()

# ...and setting one to a different level must change it, or the check above
# passes just as happily on a SetMergeGain that never reaches the audio thread.
preset_hash("lane B pulled down live" uneven-lanes.json hash_quiet_b --lane-gain b=-48)
preset_hash("lane A pulled down live" uneven-lanes.json hash_quiet_a --lane-gain a=-48)

if(hash_quiet_b STREQUAL hash_uneven)
	message(FATAL_ERROR
		"Pulling lane B down 36 dB on the running chain changed nothing: ${hash_quiet_b}")
endif()

if(hash_quiet_a STREQUAL hash_uneven)
	message(FATAL_ERROR
		"Pulling lane A down 48 dB on the running chain changed nothing: ${hash_quiet_a}")
endif()

# And the two lanes are not the same lane. A `SetMergeGain` that ignored its
# lane argument would silence whichever one it was hard-coded to, twice.
if(hash_quiet_a STREQUAL hash_quiet_b)
	message(FATAL_ERROR
		"Pulling lane A down sounds identical to pulling lane B down -- the lane argument is ignored: "
		"${hash_quiet_a}")
endif()

message(STATUS "Live lane levels reach the running chain: noop=${hash_noop} quiet-a=${hash_quiet_a} "
	"quiet-b=${hash_quiet_b}")

# SF-75: splitter modes, through the whole path -- a file on disk, parsed into
# a graph, translated into a spec, planned into filter ops, and run against
# real plugins. `chain_schedule_test` proves the filter is a Linkwitz-Riley and
# that the plan puts it where it claims; none of that would catch a splitter
# lost in translation between the three layers that spell it.

# The same two lanes four ways: no splitter at all, a splitter that spells out
# the defaults, a crossover, and a crossover an octave and a half lower.
function(write_split_preset name splitter)
	file(WRITE "${work}/${name}.json"
"{
  \"name\": \"${name}\",
  \"graph\": [
    { \"id\": 1, ${splitter}\"branches\": [
      { \"gain_db\": 0.0, \"muted\": false, \"nodes\": [
        { \"id\": 2, \"uri\": \"${PLUGIN_URI}\", \"bypassed\": false, \"controls\": { \"gain\": 4.0 } }
      ] },
      { \"gain_db\": 0.0, \"muted\": false, \"nodes\": [
        { \"id\": 3, \"uri\": \"${PLUGIN_URI}\", \"bypassed\": false, \"controls\": { \"gain\": 4.0 } }
      ] }
    ] }
  ]
}
")
endfunction()

write_split_preset(no-splitter "")
write_split_preset(default-splitter "\"splitter\": {\"mode\": \"full\", \"send_a\": 0.0, \"send_b\": 0.0}, ")
write_split_preset(crossover-800 "\"splitter\": {\"mode\": \"crossover\", \"freq\": 800.0}, ")
write_split_preset(crossover-250 "\"splitter\": {\"mode\": \"crossover\", \"freq\": 250.0}, ")
write_split_preset(sent-a "\"splitter\": {\"send_a\": -12.0}, ")
write_split_preset(stereo-mode "\"splitter\": {\"mode\": \"stereo\"}, ")

preset_hash("a split with no splitter" no-splitter.json hash_no_splitter)
preset_hash("a split whose splitter says the defaults" default-splitter.json hash_default_splitter)
preset_hash("a crossover at 800 Hz" crossover-800.json hash_cross_800)
preset_hash("a crossover at 250 Hz" crossover-250.json hash_cross_250)
preset_hash("a split with channel A sent -12 dB" sent-a.json hash_sent_a)
preset_hash("stereo mode on a one-input board" stereo-mode.json hash_stereo_mono)

# The compatibility promise, end to end: an absent splitter and a splitter that
# spells out its defaults are the same board, sample for sample. Every split
# preset written before SF-75 depends on this.
if(NOT hash_default_splitter STREQUAL hash_no_splitter)
	message(FATAL_ERROR
		"A splitter set to its defaults does not sound like no splitter at all.\n"
		"  no splitter:      ${hash_no_splitter}\n"
		"  default splitter: ${hash_default_splitter}")
endif()

# ...and stereo mode with one input channel is full range, which is what lets
# the mode be selectable on a mono board instead of greyed out (SF-76).
if(NOT hash_stereo_mono STREQUAL hash_no_splitter)
	message(FATAL_ERROR
		"Stereo mode on a one-input board is not full range.\n"
		"  full range: ${hash_no_splitter}\n"
		"  stereo:     ${hash_stereo_mono}")
endif()

# A crossover is audibly not a copy, and its frequency is a number that is
# actually used rather than parsed and dropped.
if(hash_cross_800 STREQUAL hash_no_splitter)
	message(FATAL_ERROR "A crossover split sounds exactly like full range: ${hash_cross_800}")
endif()

if(hash_cross_800 STREQUAL hash_cross_250)
	message(FATAL_ERROR
		"Crossovers at 800 Hz and 250 Hz sound identical -- the frequency is being ignored: ${hash_cross_800}")
endif()

if(hash_sent_a STREQUAL hash_no_splitter)
	message(FATAL_ERROR "Sending channel A -12 dB changed nothing: ${hash_sent_a}")
endif()

message(STATUS "Splitter modes reach the engine: crossover-800=${hash_cross_800} crossover-250=${hash_cross_250} "
	"sent-a=${hash_sent_a}")

# SF-76: a board taking two inputs, with the splitter dividing them between the
# lanes. The offline generator feeds channel 0 the sweep and channel 1 the same
# sweep 12 dB down, so the two sides are telling apart -- with both channels
# carrying the same thing, every routing would sound identical and none of this
# would mean anything.
#
# The two lanes are given different gains for the same reason: they have to be
# distinguishable from each other as well as the inputs from each other. Both
# gains are non-zero and stay well clear of it -- this plugin's "gain" is a
# linear multiplier, so a lane at 0.0 is a silent lane, and a silent lane makes
# every routing through it sound the same.
function(write_stereo_preset name src_a src_b)
	file(WRITE "${work}/${name}.json"
"{
  \"name\": \"${name}\",
  \"graph\": [
    { \"id\": 1, \"splitter\": {\"mode\": \"stereo\", \"src_a\": \"${src_a}\", \"src_b\": \"${src_b}\"},
      \"branches\": [
      { \"gain_db\": 0.0, \"muted\": false, \"nodes\": [
        { \"id\": 2, \"uri\": \"${PLUGIN_URI}\", \"bypassed\": false, \"controls\": { \"gain\": 4.0 } }
      ] },
      { \"gain_db\": 0.0, \"muted\": false, \"nodes\": [
        { \"id\": 3, \"uri\": \"${PLUGIN_URI}\", \"bypassed\": false, \"controls\": { \"gain\": 2.0 } }
      ] }
    ] }
  ]
}
")
endfunction()

write_stereo_preset(stereo-lr left right)
write_stereo_preset(stereo-rl right left)
write_stereo_preset(stereo-sum sum sum)

preset_hash("a stereo board, left into A" stereo-lr.json hash_lr --board-inputs 2)
preset_hash("a stereo board, right into A" stereo-rl.json hash_rl --board-inputs 2)
preset_hash("a stereo board summing into both" stereo-sum.json hash_sum --board-inputs 2)

# Left into A and right into A are different boards. If the sources were parsed
# and dropped -- or if both lanes were quietly fed the whole input -- these
# would be the same sound.
if(hash_lr STREQUAL hash_rl)
	message(FATAL_ERROR
		"Swapping which input feeds which channel changed nothing -- the stereo sources are not reaching the "
		"engine: ${hash_lr}")
endif()

if(hash_sum STREQUAL hash_lr OR hash_sum STREQUAL hash_rl)
	message(FATAL_ERROR
		"Summing both inputs into both channels sounds like taking one side.\n"
		"  sum:   ${hash_sum}\n"
		"  L/R:   ${hash_lr}\n"
		"  R/L:   ${hash_rl}")
endif()

# And the same preset on a one-input board still plays. This is the other half
# of the mode being selectable on a mono board: it has to be *audible*, not
# merely legal -- `--expect-signal` inside preset_hash is what says so.
preset_hash("a stereo preset on a one-input board" stereo-lr.json hash_stereo_on_mono --board-inputs 1)

message(STATUS "Stereo input divides between the channels: L/R=${hash_lr} R/L=${hash_rl} sum=${hash_sum} "
	"mono=${hash_stereo_on_mono}")
