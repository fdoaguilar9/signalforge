// Compiling a chain's shape into block operations: where an adapter goes,
// what a split sums, which lane gets delayed, and what the operations
// themselves do to samples (SF-48, SF-49, SF-50, SF-51).
//
// The planner takes channel counts and latencies as plain numbers rather than
// asking plugins for them, so all of this runs with nothing installed and no
// audio server -- which is the only way the meaning of a split was ever going
// to be checked rather than just listened to.

#include "../src/lv2/ChainSchedule.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

void Check(bool condition, const char* description)
{
	if (condition)
	{
		std::printf("ok    %s\n", description);
		return;
	}

	std::printf("FAIL  %s\n", description);
	++g_failures;
}

void CheckClose(float actual, float expected, const char* description)
{
	// Loose enough for a gain in dB round-tripping through a pow, tight enough
	// that a wrong gain -- half, double, or a 3 dB slip -- can't pass.
	const bool ok = std::fabs(actual - expected) < 1e-5f;

	if (!ok)
	{
		std::printf("FAIL  %-52s expected %g, got %g\n", description, expected, actual);
		++g_failures;
		return;
	}

	std::printf("ok    %-52s %g\n", description, actual);
}

// The same, with the tolerance stated at the call site. A filter response is
// measured rather than computed, so what counts as "flat" is a judgement that
// belongs next to the claim it is about.
void CheckCloseWithin(float actual, float expected, float tolerance, const char* description)
{
	if (std::fabs(actual - expected) > tolerance)
	{
		std::printf("FAIL  %-52s expected %g +/- %g, got %g\n", description, expected, tolerance, actual);
		++g_failures;
		return;
	}

	std::printf("ok    %-52s %g\n", description, actual);
}

sf::SpecNode Plugin(const char* uri)
{
	sf::SpecNode node;
	node.uri = uri;

	return node;
}

sf::SpecNode Split(sf::SpecList a, sf::SpecList b)
{
	sf::SpecNode node;
	node.branches.resize(2);
	node.branches[0].nodes = std::move(a);
	node.branches[1].nodes = std::move(b);

	return node;
}

sf::StageIo Io(unsigned inputs, unsigned outputs, uint32_t latency = 0)
{
	sf::StageIo io;
	io.inputs = inputs;
	io.outputs = outputs;
	io.latency = latency;

	return io;
}

sf::ChainPlan PlanOrFail(
	const sf::ChainSpec& spec, const std::vector<sf::StageIo>& io, unsigned board_inputs, const char* description)
{
	sf::ChainPlan plan;
	std::string error;

	if (!sf::PlanChain(spec, io, board_inputs, plan, error))
	{
		std::printf("FAIL  %s: unexpected planning failure: %s\n", description, error.c_str());
		++g_failures;
	}

	return plan;
}

void ExpectRefused(
	const sf::ChainSpec& spec, const std::vector<sf::StageIo>& io, unsigned board_inputs, const char* description)
{
	sf::ChainPlan plan;
	std::string error;

	if (sf::PlanChain(spec, io, board_inputs, plan, error))
	{
		std::printf("FAIL  %-52s expected refusal, planned %zu op(s)\n", description, plan.ops.size());
		++g_failures;
		return;
	}

	std::printf("ok    %-52s refused: %s\n", description, error.c_str());
}

size_t CountOps(const sf::ChainPlan& plan, sf::OpKind kind)
{
	size_t count = 0;

	for (const sf::ChainOp& op : plan.ops)
	{
		if (op.kind == kind)
		{
			++count;
		}
	}

	return count;
}

// Runs a plan over real buffers, with a caller-supplied function standing in
// for each plugin. Mirrors Chain::Run's switch deliberately: what is being
// checked here is the *plan*, and the ops it names are the same free
// functions the engine calls.
class Executor
{
public:
	using StageFn = std::function<void(const std::vector<float*>& in, const std::vector<float*>& out, uint32_t frames)>;

	// The sample rate only matters to the crossover filters -- everything else
	// in a plan is counted in samples -- so it is defaulted to the one the
	// rest of these tests assume.
	Executor(const sf::ChainPlan& plan, uint32_t block, double sample_rate = 48000.0)
		: plan_(plan)
		, block_(block)
		, storage_(plan.bus_count, std::vector<float>(block, 0.0f))
		, delays_(plan.delay_lengths.size())
		, filters_(plan.filters.size())
	{
		for (size_t i = 0; i < plan.delay_lengths.size(); ++i)
		{
			delays_[i].Reset(plan.delay_lengths[i]);
		}

		for (size_t i = 0; i < plan.filters.size(); ++i)
		{
			filters_[i].Reset(sample_rate, plan.filters[i]);
		}
	}

	float* Bus(uint32_t id) { return storage_[id].data(); }

	void Run(const std::vector<StageFn>& stages, uint32_t frames)
	{
		for (const sf::ChainOp& op : plan_.ops)
		{
			switch (op.kind)
			{
				case sf::OpKind::Run:
				{
					std::vector<float*> in;
					std::vector<float*> out;

					for (uint32_t bus : plan_.stage_input_buses[op.stage])
					{
						in.push_back(Bus(bus));
					}

					for (uint32_t bus : plan_.stage_output_buses[op.stage])
					{
						out.push_back(Bus(bus));
					}

					stages[op.stage](in, out, frames);
					break;
				}

				case sf::OpKind::Copy:
					sf::OpCopy(Bus(op.dst), Bus(op.src0), frames);
					break;

				case sf::OpKind::Downmix:
					sf::OpDownmix(Bus(op.dst), Bus(op.src0), Bus(op.src1), frames);
					break;

				case sf::OpKind::Delay:
					delays_[op.delay_line].Process(Bus(op.dst), Bus(op.src0), frames);
					break;

				case sf::OpKind::Gain:
					sf::OpGain(Bus(op.dst), Bus(op.src0), op.gain0, frames);
					break;

				case sf::OpKind::Filter:
					filters_[op.filter].Process(Bus(op.dst), Bus(op.src0), frames);
					break;

				case sf::OpKind::Mix:
					sf::OpMix(Bus(op.dst), Bus(op.src0), Bus(op.src1), op.gain0, op.gain1, frames);
					break;
			}
		}
	}

private:
	const sf::ChainPlan& plan_;
	uint32_t block_;
	std::vector<std::vector<float>> storage_;
	std::vector<sf::DelayLine> delays_;
	std::vector<sf::CrossoverFilter> filters_;
};

// A stage that multiplies every input channel into the matching output. The
// stand-in for "a pedal that does something measurable".
Executor::StageFn Gain(float gain)
{
	return [gain](const std::vector<float*>& in, const std::vector<float*>& out, uint32_t frames)
	{
		for (size_t ch = 0; ch < out.size(); ++ch)
		{
			const float* src = in[std::min(ch, in.size() - 1)];

			for (uint32_t i = 0; i < frames; ++i)
			{
				out[ch][i] = gain * src[i];
			}
		}
	};
}

void TestLinear()
{
	const sf::ChainSpec spec = {Plugin("a"), Plugin("b")};
	const sf::ChainPlan plan = PlanOrFail(spec, {Io(1, 1), Io(1, 1)}, 1, "mono series");

	Check(CountOps(plan, sf::OpKind::Run) == 2, "a mono series is two Run ops");
	Check(plan.ops.size() == 4, "...and nothing else but the two copies that widen the board's output");
	Check(plan.input_buses.size() == 1, "a mono board takes one input");
	Check(plan.output_buses.size() == 2, "...and hands back two, whatever the last pedal produced");
	Check(plan.stage_input_buses[1][0] == plan.stage_output_buses[0][0],
		"one stage's output bus is the next one's input, with no copy between");
	Check(plan.latency == 0, "plugins that report no latency add none");
}

void TestAdapters()
{
	// Mono guitar into a stereo pedal: the signal is duplicated, not refused.
	const sf::ChainPlan widen = PlanOrFail({Plugin("stereo")}, {Io(2, 2)}, 1, "mono into a stereo pedal");
	Check(CountOps(widen, sf::OpKind::Copy) == 2, "mono into stereo duplicates into both channels");
	Check(widen.stage_input_buses[0].size() == 2, "...and the pedal sees two inputs");

	const sf::ChainPlan narrow = PlanOrFail({Plugin("stereo"), Plugin("mono")}, {Io(2, 2), Io(1, 1)}, 2,
		"stereo pedal into a mono one");
	Check(CountOps(narrow, sf::OpKind::Downmix) == 1, "stereo into mono folds down");
	Check(narrow.stage_input_buses[1].size() == 1, "...and the mono pedal is handed one channel");

	// The level question, which is the one that matters: a signal that goes
	// mono -> stereo -> mono must come back at the level it went in. Summing
	// two correlated copies without halving would put it out 6 dB hot.
	const uint32_t frames = 8;
	std::vector<float> src(frames, 0.5f);
	std::vector<float> wide_a(frames, 0.0f);
	std::vector<float> wide_b(frames, 0.0f);
	std::vector<float> folded(frames, 0.0f);

	sf::OpCopy(wide_a.data(), src.data(), frames);
	sf::OpCopy(wide_b.data(), src.data(), frames);
	sf::OpDownmix(folded.data(), wide_a.data(), wide_b.data(), frames);

	CheckClose(folded[0], 0.5f, "mono -> stereo -> mono is unity, not +6 dB");

	// Two genuinely different channels average rather than sum, which is the
	// same rule seen from the other side.
	std::vector<float> left(frames, 1.0f);
	std::vector<float> right(frames, 0.0f);
	sf::OpDownmix(folded.data(), left.data(), right.data(), frames);
	CheckClose(folded[0], 0.5f, "a hard-panned signal folds to half");
}

// The board's edge does not move, whatever is on it.
//
// **This is the invariant the whole thing turns on, not a nice property.** A
// host registers its ports once and swaps chains underneath them, so a board
// that ended in two channels only when its last pedal happened to be stereo
// would refuse the swap the moment a stereo pedal was added or removed -- which
// is exactly the bug this exists to keep fixed. Asserted over a spread of
// shapes rather than one, because "it works for the chain I tried" is what let
// it through the first time.
void TestBoardWidth()
{
	struct Case
	{
		const char* description;
		sf::ChainSpec spec;
		std::vector<sf::StageIo> io;
	};

	const std::vector<Case> cases = {
		{"an empty board", {}, {}},
		{"a mono pedal", {Plugin("a")}, {Io(1, 1)}},
		{"a stereo pedal", {Plugin("a")}, {Io(2, 2)}},
		{"a mono pedal after a stereo one", {Plugin("a"), Plugin("b")}, {Io(2, 2), Io(1, 1)}},
		{"a stereo pedal after a mono one", {Plugin("a"), Plugin("b")}, {Io(1, 1), Io(2, 2)}},
		{"a pedal that widens on its own", {Plugin("a")}, {Io(1, 2)}},
		{"a pedal that folds on its own", {Plugin("a")}, {Io(2, 1)}},
		{"a generator", {Plugin("a")}, {Io(0, 1)}},
		{"a split of two mono lanes", {Split({Plugin("a")}, {Plugin("b")})}, {Io(1, 1), Io(1, 1)}},
		{"a split with one stereo lane", {Split({Plugin("a")}, {Plugin("b")})}, {Io(1, 1), Io(2, 2)}},
		{"a split with a dry lane", {Split({Plugin("a")}, {})}, {Io(2, 2)}},
	};

	for (unsigned board_inputs = 1; board_inputs <= 2; ++board_inputs)
	{
		for (const Case& item : cases)
		{
			char description[128];
			std::snprintf(description, sizeof(description), "%s, %u in", item.description, board_inputs);

			const sf::ChainPlan plan = PlanOrFail(item.spec, item.io, board_inputs, description);

			Check(plan.input_buses.size() == board_inputs && plan.output_buses.size() == sf::kBoardOutputs,
				description);
		}
	}

	// And the board's two outputs are always two distinct buffers, even where
	// nothing in the chain forced them apart. Two output ports that turned out
	// to be the same buffer would have the engine hand the same pointer to
	// JACK twice and lose a channel.
	const sf::ChainPlan mono = PlanOrFail({Plugin("a")}, {Io(1, 1)}, 1, "a mono pedal on a mono board");
	Check(mono.output_buses[0] != mono.output_buses[1], "the board's two outputs are two separate buffers");

	const sf::ChainPlan empty = PlanOrFail({}, {}, 2, "an empty stereo board");
	Check(empty.output_buses[0] != empty.output_buses[1], "...on an empty stereo board too");
	Check(empty.output_buses[0] != empty.input_buses[0], "...and are not the input buffers either");
}

void TestSplit()
{
	sf::ChainSpec spec = {Split({Plugin("a")}, {Plugin("b")})};

	const sf::ChainPlan plan = PlanOrFail(spec, {Io(1, 1), Io(1, 1)}, 1, "a split");
	Check(CountOps(plan, sf::OpKind::Mix) == 1, "a mono split merges with one Mix");

	// The merge is still the last thing that happens *to the signal*; what
	// follows it is only the board widening its own output to two channels.
	{
		size_t mix_at = plan.ops.size();

		for (size_t i = 0; i < plan.ops.size(); ++i)
		{
			if (plan.ops[i].kind == sf::OpKind::Mix)
			{
				mix_at = i;
			}
		}

		bool only_widening_after = mix_at < plan.ops.size();

		for (size_t i = mix_at + 1; i < plan.ops.size(); ++i)
		{
			only_widening_after = only_widening_after && plan.ops[i].kind == sf::OpKind::Copy;
		}

		Check(only_widening_after, "the merge is the last thing that happens to the signal");
	}

	Check(plan.stage_input_buses[0][0] == plan.stage_input_buses[1][0], "both lanes are fed the same signal");
	Check(plan.stage_input_buses[0][0] == plan.input_buses[0], "...which is the board's input");

	// Numerically: a split of a 2x lane and a 3x lane sums to 5x.
	{
		Executor executor(plan, 4);
		float* in = executor.Bus(plan.input_buses[0]);
		in[0] = 1.0f;

		executor.Run({Gain(2.0f), Gain(3.0f)}, 4);
		CheckClose(executor.Bus(plan.output_buses[0])[0], 5.0f, "two lanes at unity sum");
	}

	// A muted lane is sample-identical to not having it at all.
	sf::ChainSpec muted_spec = {Split({Plugin("a")}, {Plugin("b")})};
	muted_spec[0].branches[1].muted = true;

	const sf::ChainPlan muted = PlanOrFail(muted_spec, {Io(1, 1), Io(1, 1)}, 1, "a split with one lane muted");
	const sf::ChainPlan alone = PlanOrFail({Plugin("a")}, {Io(1, 1)}, 1, "the other lane on its own");

	Executor muted_run(muted, 4);
	muted_run.Bus(muted.input_buses[0])[0] = 1.0f;
	muted_run.Run({Gain(2.0f), Gain(3.0f)}, 4);

	Executor alone_run(alone, 4);
	alone_run.Bus(alone.input_buses[0])[0] = 1.0f;
	alone_run.Run({Gain(2.0f)}, 4);

	CheckClose(muted_run.Bus(muted.output_buses[0])[0], alone_run.Bus(alone.output_buses[0])[0],
		"a muted lane contributes exactly nothing");

	// Lane gain, in dB, applied where the plan says it is.
	sf::ChainSpec quiet_spec = {Split({Plugin("a")}, {Plugin("b")})};
	quiet_spec[0].branches[0].gain_db = -6.0f;
	quiet_spec[0].branches[1].muted = true;

	const sf::ChainPlan quiet = PlanOrFail(quiet_spec, {Io(1, 1), Io(1, 1)}, 1, "a lane pulled down 6 dB");
	Executor quiet_run(quiet, 4);
	quiet_run.Bus(quiet.input_buses[0])[0] = 1.0f;
	quiet_run.Run({Gain(1.0f), Gain(1.0f)}, 4);
	CheckClose(quiet_run.Bus(quiet.output_buses[0])[0], 0.5011872f, "-6 dB on a lane is half the amplitude");

	// Lanes of different widths meet at the wider one.
	const sf::ChainPlan uneven = PlanOrFail({Split({Plugin("mono")}, {Plugin("stereo")})},
		{Io(1, 1), Io(1, 2)}, 1, "a mono lane against a stereo one");
	Check(uneven.output_buses.size() == 2, "a split with a stereo lane comes out stereo");
	Check(CountOps(uneven, sf::OpKind::Mix) == 2, "...and merges both channels");
}

void TestLatencyAlignment()
{
	// One lane has a plugin that reports 64 samples of latency; the other has
	// none. Summed as-is they comb-filter, so the short lane is delayed.
	const sf::ChainPlan plan = PlanOrFail({Split({Plugin("latent")}, {Plugin("instant")})},
		{Io(1, 1, 64), Io(1, 1)}, 1, "lanes of unequal latency");

	Check(plan.delay_lengths.size() == 1, "exactly one lane is delayed");

	// Guarded rather than indexed straight: when the check above fails there
	// is no element 0, and a test that segfaults instead of saying which
	// assertion broke is a worse test than one that reports.
	Check(plan.delay_lengths.size() == 1 && plan.delay_lengths[0] == 64,
		"...by the difference, not by either lane's own latency");
	Check(plan.latency == 64, "the chain reports the latency it actually has");

	// Equal latency needs no delay at all.
	const sf::ChainPlan even = PlanOrFail({Split({Plugin("a")}, {Plugin("b")})}, {Io(1, 1, 32), Io(1, 1, 32)}, 1,
		"lanes of equal latency");
	Check(even.delay_lengths.empty(), "lanes that already line up are not delayed");
	Check(even.latency == 32, "...and the chain's latency is the lane's, not the sum");

	// A series accumulates.
	const sf::ChainPlan series = PlanOrFail({Plugin("a"), Plugin("b")}, {Io(1, 1, 10), Io(1, 1, 5)}, 1, "series latency");
	Check(series.latency == 15, "latency adds along a series");

	// And numerically: a lane delayed by 3 samples lines an impulse back up
	// with the lane that was already 3 samples late.
	sf::DelayLine line;
	line.Reset(3);

	std::vector<float> src(8, 0.0f);
	std::vector<float> dst(8, 0.0f);
	src[0] = 1.0f;

	line.Process(dst.data(), src.data(), 8);
	CheckClose(dst[3], 1.0f, "a 3-sample delay puts the impulse at sample 3");
	CheckClose(dst[0], 0.0f, "...and nothing before it");

	// Across a block boundary, which is where a ring buffer written carelessly
	// goes wrong.
	sf::DelayLine across;
	across.Reset(6);

	std::vector<float> block(4, 0.0f);
	block[0] = 1.0f;
	std::vector<float> out(4, 0.0f);

	across.Process(out.data(), block.data(), 4);
	CheckClose(out[0], 0.0f, "a delay longer than the block emits nothing yet");

	std::vector<float> silence(4, 0.0f);
	across.Process(out.data(), silence.data(), 4);
	CheckClose(out[2], 1.0f, "...and the impulse arrives in the next block, at the right sample");
}

void TestPassthroughAliasing()
{
	// A split of two empty lanes is two wires. The board's output must not
	// simply *be* its input: the two ends are pointed at different buffers
	// every cycle, so without a copy nothing would ever reach the speakers.
	const sf::ChainPlan plan = PlanOrFail({Split({}, {})}, {}, 1, "two dry wires");

	Check(!plan.output_buses.empty(), "a board of wires still has an output");
	Check(!plan.output_buses.empty() && plan.output_buses[0] != plan.input_buses[0],
		"the output bus is not the input bus");
	Check(!plan.ops.empty(), "...because something copies between them");

	Executor executor(plan, 4);
	executor.Bus(plan.input_buses[0])[0] = 1.0f;
	executor.Run({}, 4);

	// Two wires in parallel, both at unity, are two copies of the same signal
	// summing -- so this is 2, and that is arithmetic rather than a bug.
	CheckClose(executor.Bus(plan.output_buses[0])[0], 2.0f, "two unity wires in parallel sum");
}

void TestGenerator()
{
	// A plugin with no audio inputs replaces the signal rather than
	// processing it; there is nothing to adapt into it.
	const sf::ChainPlan plan = PlanOrFail({Plugin("tone"), Plugin("amp")}, {Io(0, 1), Io(1, 1)}, 1, "a generator");

	Check(plan.stage_input_buses.size() == 2 && plan.stage_input_buses[0].empty(),
		"a generator is connected to no input bus");
	Check(plan.stage_input_buses.size() == 2 && !plan.stage_input_buses[1].empty() &&
			plan.stage_input_buses[1][0] == plan.stage_output_buses[0][0],
		"...and what follows takes its output");
}

// Merge numbering and the ramping mix behind a live lane level (SF-72).
//
// The numbering is a contract between two walks that live in different files:
// the planner's, here, and ChainGraph::MergeIndexOf's. Both are "pre-order --
// a split before either of its branches", and if they ever stop agreeing a
// fader moves the wrong lane of the wrong split, which is exactly the sort of
// bug that looks like a UI problem for a week.
void TestMergeGain()
{
	// A stereo merge is two Mix ops, and they are one merge, not two: a fader
	// that moved only the left channel of a lane would be worse than one that
	// did nothing.
	{
		const sf::ChainPlan stereo = PlanOrFail({Split({Plugin("a")}, {Plugin("b")})}, {Io(2, 2), Io(2, 2)}, 2,
			"a stereo split");

		Check(stereo.merge_count == 1, "a split is one merge however many channels it sums");
		Check(CountOps(stereo, sf::OpKind::Mix) == 2, "...drawn as one Mix op per channel");

		bool shared = true;

		for (const sf::ChainOp& op : stereo.ops)
		{
			if (op.kind == sf::OpKind::Mix && op.merge != 0)
			{
				shared = false;
			}
		}

		Check(shared, "...all naming the same merge");
		Check(stereo.merge_gain.size() == 2, "one gain per lane of one merge");
	}

	// Nested: the outer split is met first, so it is merge 0, even though its
	// Mix op is emitted last.
	{
		const sf::ChainPlan nested = PlanOrFail({Split({Split({Plugin("a")}, {Plugin("b")})}, {Plugin("c")})},
			{Io(1, 1), Io(1, 1), Io(1, 1)}, 1, "a split inside a split");

		Check(nested.merge_count == 2, "two splits are two merges");

		uint32_t last_mix_merge = 99;

		for (const sf::ChainOp& op : nested.ops)
		{
			if (op.kind == sf::OpKind::Mix)
			{
				last_mix_merge = op.merge;
			}
		}

		Check(last_mix_merge == 0, "the outer split is merge 0 though it sums last");
	}

	// The gain table and the ops agree, including a muted lane's zero -- a
	// host that takes the table as its starting point must start where a host
	// that never touches it stays.
	{
		sf::ChainSpec spec = {Split({Plugin("a")}, {Plugin("b")})};
		spec[0].branches[0].gain_db = -6.0f;
		spec[0].branches[1].muted = true;

		const sf::ChainPlan plan = PlanOrFail(spec, {Io(1, 1), Io(1, 1)}, 1, "a quiet lane and a muted one");

		Check(plan.merge_gain.size() == 2, "the gain table has both lanes");
		CheckClose(plan.merge_gain[0], 0.5011872f, "the table carries -6 dB as an amplitude");
		CheckClose(plan.merge_gain[1], 0.0f, "...and a muted lane as zero");

		for (const sf::ChainOp& op : plan.ops)
		{
			if (op.kind == sf::OpKind::Mix)
			{
				CheckClose(op.gain0, plan.merge_gain[2 * op.merge], "the op and the table agree on lane 0");
				CheckClose(op.gain1, plan.merge_gain[2 * op.merge + 1], "...and on lane 1");
			}
		}
	}

	// A flat ramp is the plain mix, sample for sample. This is what lets the
	// engine ramp unconditionally: a board nobody is touching has to be
	// unchanged, not merely close.
	{
		const uint32_t frames = 8;
		float a[frames];
		float b[frames];
		float plain[frames];
		float ramped[frames];

		for (uint32_t i = 0; i < frames; ++i)
		{
			a[i] = 0.1f * static_cast<float>(i + 1);
			b[i] = -0.3f * static_cast<float>(i);
		}

		sf::OpMix(plain, a, b, 0.75f, 0.25f, frames);
		sf::OpMixRamp(ramped, a, b, 0.75f, 0.75f, 0.25f, 0.25f, frames);

		bool identical = true;

		for (uint32_t i = 0; i < frames; ++i)
		{
			if (plain[i] != ramped[i])
			{
				identical = false;
			}
		}

		Check(identical, "a ramp that goes nowhere is the plain mix, bit for bit");
	}

	// A moving ramp starts at `from` and walks toward `to` without reaching
	// it, so the next block starts on the target rather than repeating the
	// last value.
	{
		const uint32_t frames = 4;
		float ones[frames] = {1.0f, 1.0f, 1.0f, 1.0f};
		float zeros[frames] = {0.0f, 0.0f, 0.0f, 0.0f};
		float out[frames];

		sf::OpMixRamp(out, ones, zeros, 0.0f, 1.0f, 0.0f, 0.0f, frames);

		CheckClose(out[0], 0.0f, "a ramp starts where it was");
		CheckClose(out[1], 0.25f, "...and steps by one frame's share");
		CheckClose(out[3], 0.75f, "...ending one step short of the target");
	}
}

// Splitter modes (SF-75): how a split divides the signal before either lane
// runs, as opposed to how it sums them afterwards.
//
// All of it is arithmetic on plain numbers, so all of it is checkable here --
// which matters more than usual, because "the crossover sounds wrong" is not a
// bug report anyone can act on and "the two halves do not sum flat" is.

// RMS of a settled sine through a filter, relative to the sine's own RMS. The
// measure is deliberately RMS and not peak: at 8 kHz on a 48 kHz board a sine
// is six samples per cycle, and no sample need land near the crest, so peak
// under-reads by up to 13% for reasons that have nothing to do with the
// filter.
float ResponseAt(const sf::CrossoverSpec& spec, double sample_rate, double frequency, bool sum_with_sibling)
{
	sf::CrossoverFilter filter;
	filter.Reset(sample_rate, spec);

	sf::CrossoverSpec sibling_spec = spec;
	sibling_spec.highpass = !spec.highpass;

	sf::CrossoverFilter sibling;
	sibling.Reset(sample_rate, sibling_spec);

	// Long enough that the first cycles, which carry the filter's start-up
	// transient, are a small share of what is measured -- and discarded on top
	// of that.
	const uint32_t frames = 24000;
	const uint32_t settle = 4800;

	std::vector<float> input(frames);

	for (uint32_t i = 0; i < frames; ++i)
	{
		input[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * frequency * i / sample_rate));
	}

	std::vector<float> filtered(frames);
	std::vector<float> other(frames);

	filter.Process(filtered.data(), input.data(), frames);
	sibling.Process(other.data(), input.data(), frames);

	double signal = 0.0;
	double reference = 0.0;

	for (uint32_t i = settle; i < frames; ++i)
	{
		const double value = sum_with_sibling ? filtered[i] + other[i] : filtered[i];

		signal += value * value;
		reference += static_cast<double>(input[i]) * input[i];
	}

	return static_cast<float>(std::sqrt(signal / reference));
}

void TestCrossoverFilter()
{
	const double rate = 48000.0;

	sf::CrossoverSpec low;
	low.frequency = 800.0f;
	low.highpass = false;

	// **The two halves sum flat.** This is the whole reason for choosing
	// Linkwitz-Riley: an LR4 pair sums to unity magnitude across the band,
	// where a Butterworth pair peaks +3 dB at the crossover. A crossover that
	// colours the sound at 800 Hz gets blamed on the amps behind it.
	const double band[] = {40.0, 200.0, 800.0, 2000.0, 8000.0, 15000.0};

	for (double frequency : band)
	{
		char what[64];
		std::snprintf(what, sizeof(what), "the halves sum flat at %.0f Hz", frequency);

		CheckCloseWithin(ResponseAt(low, rate, frequency, true), 1.0f, 0.02f, what);
	}

	// **Each half is -6 dB at the crossover, not -3.** This is the check that
	// tells an LR4 from the Butterworth pair it is built out of: one
	// Butterworth section is -3 dB at its corner, and LR4 is that section
	// squared. A pair that measured -3 dB here would still look like a
	// crossover and would sum 3 dB hot right where the ear is most sensitive
	// to it.
	CheckCloseWithin(ResponseAt(low, rate, 800.0, false), 0.5f, 0.02f, "the low half is -6 dB at the crossover");

	sf::CrossoverSpec high = low;
	high.highpass = true;

	CheckCloseWithin(ResponseAt(high, rate, 800.0, false), 0.5f, 0.02f, "the high half is -6 dB at the crossover");

	// And they pass what they are named for.
	Check(ResponseAt(low, rate, 50.0, false) > 0.99f, "the low half passes 50 Hz");
	Check(ResponseAt(low, rate, 8000.0, false) < 0.01f, "...and stops 8 kHz");
	Check(ResponseAt(high, rate, 8000.0, false) > 0.99f, "the high half passes 8 kHz");
	Check(ResponseAt(high, rate, 50.0, false) < 0.01f, "...and stops 50 Hz");

	// A frequency nobody sane would ask for still has to leave a working
	// board: the number comes from a box a player types in.
	sf::CrossoverSpec absurd;
	absurd.frequency = 400000.0f;

	sf::CrossoverFilter clamped;
	clamped.Reset(rate, absurd);

	float samples[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	clamped.Process(samples, samples, 4);

	Check(std::isfinite(samples[3]), "a crossover above Nyquist is clamped, not left to blow up");
}

void TestSplitterModes()
{
	// A default splitter costs nothing. Every preset written before splitters
	// existed reads as one, so this is the check that says those boards still
	// compile to exactly the plan they used to.
	{
		const sf::ChainPlan plain = PlanOrFail({Split({Plugin("a")}, {Plugin("b")})}, {Io(1, 1), Io(1, 1)}, 1,
			"a split with no splitter set");

		Check(CountOps(plain, sf::OpKind::Gain) == 0, "a default splitter plans no gain op");
		Check(CountOps(plain, sf::OpKind::Filter) == 0, "...and no filter");
		Check(plain.filters.empty(), "...and asks for no filter state");
	}

	// Sends: what the lane is driven with, before its own plugins, which is a
	// different control from the gain_db that decides how much comes back.
	{
		sf::ChainSpec spec = {Split({Plugin("a")}, {Plugin("b")})};
		spec[0].branches[0].send_db = -6.0f;

		const sf::ChainPlan plan = PlanOrFail(spec, {Io(1, 1), Io(1, 1)}, 1, "a split with lane A sent -6 dB");

		Check(CountOps(plan, sf::OpKind::Gain) == 1, "one sent lane is one gain op");
		Check(plan.stage_input_buses[0][0] != plan.stage_input_buses[1][0],
			"...and the two lanes no longer read the same bus");

		// The lane's plugin sees the attenuated signal; the other one does
		// not. Both lanes are unity so the sum is 0.5 + 1.0.
		Executor executor(plan, 4);
		executor.Bus(plan.input_buses[0])[0] = 1.0f;
		executor.Run({Gain(1.0f), Gain(1.0f)}, 4);

		CheckClose(executor.Bus(plan.output_buses[0])[0], 1.5011873f, "a -6 dB send only quietens its own lane");
	}

	// A send and a return are not the same control, and this is the case that
	// proves it: -6 dB in and +6 dB back out is unity overall, but what the
	// plugin in between saw was halved. With a distorting pedal there that is
	// the entire difference between the two knobs.
	{
		sf::ChainSpec spec = {Split({Plugin("a")}, {})};
		spec[0].branches[0].send_db = -6.0f;
		spec[0].branches[0].gain_db = 6.0f;
		spec[0].branches[1].muted = true;

		const sf::ChainPlan plan = PlanOrFail(spec, {Io(1, 1)}, 1, "a lane sent down and returned up");

		Executor executor(plan, 4);
		executor.Bus(plan.input_buses[0])[0] = 1.0f;
		executor.Run({Gain(1.0f)}, 4);

		CheckClose(executor.Bus(plan.output_buses[0])[0], 1.0f, "send down and return up is unity end to end");
	}

	// Crossover: one filter per channel per lane, and low goes to A.
	{
		sf::ChainSpec spec = {Split({Plugin("a")}, {Plugin("b")})};
		spec[0].splitter.mode = sf::SplitMode::Crossover;
		spec[0].splitter.frequency = 800.0f;

		const sf::ChainPlan plan = PlanOrFail(spec, {Io(1, 1), Io(1, 1)}, 1, "a mono crossover split");

		Check(CountOps(plan, sf::OpKind::Filter) == 2, "a mono crossover is two filters, one per lane");
		Check(plan.filters.size() == 2, "...and two filter states");
		Check(!plan.filters[0].highpass && plan.filters[1].highpass, "low feeds lane A and high feeds lane B");

		// A stereo crossover needs a filter per channel as well as per lane:
		// the filter state belongs to one channel, and sharing one between
		// two would fold each into the other.
		const sf::ChainPlan stereo = PlanOrFail(spec, {Io(2, 2), Io(2, 2)}, 2, "a stereo crossover split");

		Check(stereo.filters.size() == 4, "a stereo crossover is four filters, one per channel per lane");
	}

	// Stereo mode: a side of the input into each lane, taken by reading the
	// channel rather than copying it.
	{
		sf::ChainSpec spec = {Split({Plugin("a")}, {Plugin("b")})};
		spec[0].splitter.mode = sf::SplitMode::Stereo;

		const sf::ChainPlan plan = PlanOrFail(spec, {Io(1, 1), Io(1, 1)}, 2, "a stereo split into two mono lanes");

		Check(plan.stage_input_buses[0][0] == plan.input_buses[0], "lane A takes the left input");
		Check(plan.stage_input_buses[1][0] == plan.input_buses[1], "lane B takes the right input");
		// Against the board's inputs specifically: the copies that widen the
		// board's *output* are always there and are not what this is about.
		size_t copies_from_input = 0;

		for (const sf::ChainOp& op : plan.ops)
		{
			if (op.kind == sf::OpKind::Copy &&
				(op.src0 == plan.input_buses[0] || op.src0 == plan.input_buses[1]))
			{
				++copies_from_input;
			}
		}

		Check(copies_from_input == 0, "...and neither is copied to get there");

		// Numerically: left 1.0 and right 2.0 arrive in the lanes that asked
		// for them and nowhere else.
		Executor executor(plan, 4);
		executor.Bus(plan.input_buses[0])[0] = 1.0f;
		executor.Bus(plan.input_buses[1])[0] = 2.0f;
		executor.Run({Gain(1.0f), Gain(10.0f)}, 4);

		CheckClose(executor.Bus(plan.output_buses[0])[0], 21.0f, "left goes to A and right goes to B");

		// Both lanes summing is what `sum` means, and it is the mono fold, so
		// it comes back at the level it went in rather than doubled.
		sf::ChainSpec summed = spec;
		summed[0].splitter.source[0] = sf::SplitSource::Sum;
		summed[0].splitter.source[1] = sf::SplitSource::Sum;

		const sf::ChainPlan sum_plan = PlanOrFail(summed, {Io(1, 1), Io(1, 1)}, 2, "both lanes taking the sum");

		Executor sum_run(sum_plan, 4);
		sum_run.Bus(sum_plan.input_buses[0])[0] = 1.0f;
		sum_run.Bus(sum_plan.input_buses[1])[0] = 2.0f;
		sum_run.Run({Gain(1.0f), Gain(0.0f)}, 4);

		CheckClose(sum_run.Bus(sum_plan.output_buses[0])[0], 1.5f, "a summed source is the mono fold, halved");
	}

	// **Stereo mode against a one-channel input is full range**, not silence
	// and not a refusal. The mode still selects and the board still plays; it
	// simply has nothing to divide until a stereo input is chosen in Audio
	// settings (SF-76). Asserted as plan equality, op for op, because "sounds
	// about the same" is not the claim.
	{
		sf::ChainSpec spec = {Split({Plugin("a")}, {Plugin("b")})};
		spec[0].splitter.mode = sf::SplitMode::Stereo;

		const sf::ChainPlan mono = PlanOrFail(spec, {Io(1, 1), Io(1, 1)}, 1, "stereo mode on a one-input board");
		const sf::ChainPlan full = PlanOrFail({Split({Plugin("a")}, {Plugin("b")})}, {Io(1, 1), Io(1, 1)}, 1,
			"the same split at full range");

		bool same = mono.ops.size() == full.ops.size() && mono.bus_count == full.bus_count;

		for (size_t i = 0; same && i < mono.ops.size(); ++i)
		{
			same = mono.ops[i].kind == full.ops[i].kind && mono.ops[i].dst == full.ops[i].dst &&
				mono.ops[i].src0 == full.ops[i].src0 && mono.ops[i].src1 == full.ops[i].src1;
		}

		Check(same, "stereo mode with one input channel plans exactly full range");
	}
}

void TestRefusals()
{
	ExpectRefused({Plugin("a")}, {Io(1, 3)}, 1, "a plugin with three outputs");
	ExpectRefused({Plugin("a")}, {Io(3, 1)}, 1, "a plugin with three inputs");
	ExpectRefused({Plugin("a")}, {Io(1, 0)}, 1, "a plugin with no audio output");
	ExpectRefused({Plugin("a"), Plugin("b")}, {Io(1, 1)}, 1, "fewer channel counts than plugins");
	ExpectRefused({Plugin("a")}, {Io(1, 1), Io(1, 1)}, 1, "more channel counts than plugins");
	ExpectRefused({Plugin("a")}, {Io(1, 1)}, 3, "a board with three inputs");

	sf::SpecNode lopsided;
	lopsided.branches.resize(1);
	ExpectRefused({lopsided}, {}, 1, "a split with one branch");
}

}  // namespace

int main()
{
	TestLinear();
	TestAdapters();
	TestBoardWidth();
	TestSplit();
	TestLatencyAlignment();
	TestPassthroughAliasing();
	TestGenerator();
	TestMergeGain();
	TestCrossoverFilter();
	TestSplitterModes();
	TestRefusals();

	if (g_failures > 0)
	{
		std::printf("\n%d check(s) failed\n", g_failures);
		return 1;
	}

	std::printf("\nall checks passed\n");

	return 0;
}
