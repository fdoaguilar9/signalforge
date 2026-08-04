#pragma once

#include <string>
#include <vector>

namespace sf
{

// The shape of a chain, in terms lv2/ can speak: plugin URIs and splits.
//
// Deliberately not `preset::ChainGraph`, which carries control values, state
// blobs, and node ids that mean nothing to a plugin host. This keeps lv2/ a
// standalone hosting library that knows nothing about our file format --
// `ChainPreset` is the one place the two are translated (SF-48).

struct SpecNode;

using SpecList = std::vector<SpecNode>;

struct SpecBranch
{
	SpecList nodes;

	// How this lane comes back in at the merge. A muted lane still runs --
	// its plugins keep their tails and their loaded models warm -- it just
	// contributes nothing.
	float gain_db = 0.0f;
	bool muted = false;

	// How this lane is fed *into* -- the send, before its plugins, as opposed
	// to `gain_db` which is the return after them. Two separate controls
	// because they do different things: a send drives the amp in the lane
	// harder, a return decides how much of the result comes back.
	float send_db = 0.0f;
};

// How a split divides the signal (SF-75).
enum class SplitMode
{
	// Both lanes get the whole signal. What every split did before there was
	// a choice, and what an absent choice still means.
	Full,

	// A Linkwitz-Riley pair at `frequency`: below into lane A, above into
	// lane B. The two halves of an LR4 sum flat, which is the whole reason
	// for choosing it over a Butterworth pair that does not.
	Crossover,

	// One side of a stereo input into each lane. With a one-channel input
	// there are no sides, and this means exactly what Full means.
	Stereo
};

// Which part of a stereo input a lane takes in Stereo mode.
enum class SplitSource
{
	Left,
	Right,
	Sum
};

struct SpecSplitter
{
	SplitMode mode = SplitMode::Full;

	// Crossover only. 800 Hz is the design's default and is only read when
	// the mode is Crossover, so it costs nothing to carry otherwise.
	float frequency = 800.0f;

	// Stereo only, per lane.
	SplitSource source[2] = {SplitSource::Left, SplitSource::Right};
};

struct SpecNode
{
	// A plugin node names a URI. A split node has two branches and no URI.
	std::string uri;
	std::vector<SpecBranch> branches;

	// Split nodes only. Defaulted so that a split built without thinking
	// about it behaves exactly as splits did before this existed.
	SpecSplitter splitter;

	bool IsSplit() const { return !branches.empty(); }
};

using ChainSpec = SpecList;

// The plain series every chain was before Milestone 9.
ChainSpec LinearSpec(const std::vector<std::string>& uris);

// Every plugin URI in signal order: the trunk in order, and at a split all of
// branch 0 then all of branch 1 before the trunk resumes. The same rule
// ChainGraph::Flatten uses, and it has to stay the same rule -- it is what
// makes a chain stage index mean the same thing on both sides.
std::vector<std::string> FlattenSpec(const ChainSpec& spec);

}  // namespace sf
