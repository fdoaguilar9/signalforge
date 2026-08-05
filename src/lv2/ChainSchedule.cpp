#include "ChainSchedule.h"

#include <algorithm>
#include <cmath>

namespace sf
{

ChainSpec LinearSpec(const std::vector<std::string>& uris)
{
	ChainSpec spec;

	for (const std::string& uri : uris)
	{
		SpecNode node;
		node.uri = uri;
		spec.push_back(std::move(node));
	}

	return spec;
}

namespace
{

void FlattenInto(const SpecList& nodes, std::vector<std::string>& out)
{
	for (const SpecNode& node : nodes)
	{
		if (!node.IsSplit())
		{
			out.push_back(node.uri);
			continue;
		}

		for (const SpecBranch& branch : node.branches)
		{
			FlattenInto(branch.nodes, out);
		}
	}
}

// A signal on the way through: which buffers carry it, and how many samples
// of delay it has picked up getting here.
struct Bus
{
	std::vector<uint32_t> channels;
	uint32_t latency = 0;
};

// Everything the recursive walk needs to share. Kept in one place so the walk
// itself reads as the algorithm and not as parameter plumbing.
struct Planner
{
	const std::vector<StageIo>* stage_io = nullptr;
	ChainPlan* plan = nullptr;
	std::string error;

	size_t next_stage = 0;
	bool failed = false;

	uint32_t NewBus() { return plan->bus_count++; }

	std::vector<uint32_t> NewBuses(unsigned count)
	{
		std::vector<uint32_t> buses;
		buses.reserve(count);

		for (unsigned i = 0; i < count; ++i)
		{
			buses.push_back(NewBus());
		}

		return buses;
	}

	bool Fail(std::string reason)
	{
		if (!failed)
		{
			failed = true;
			error = std::move(reason);
		}

		return false;
	}

	// Mono into a stereo pedal, or a stereo pedal into a mono one. The two
	// adapters are the whole of SF-48: every other channel count is refused
	// rather than guessed at, because there is no one right answer for what
	// four channels into two should do and silently picking one is worse than
	// saying so.
	Bus Adapt(const Bus& in, unsigned want)
	{
		if (failed || in.channels.size() == want)
		{
			return in;
		}

		Bus out;
		out.latency = in.latency;

		if (in.channels.size() == 1 && want == 2)
		{
			out.channels = NewBuses(2);

			for (unsigned ch = 0; ch < 2; ++ch)
			{
				ChainOp op;
				op.kind = OpKind::Copy;
				op.dst = out.channels[ch];
				op.src0 = in.channels[0];
				plan->ops.push_back(op);
			}

			return out;
		}

		if (in.channels.size() == 2 && want == 1)
		{
			out.channels = NewBuses(1);

			ChainOp op;
			op.kind = OpKind::Downmix;
			op.dst = out.channels[0];
			op.src0 = in.channels[0];
			op.src1 = in.channels[1];
			plan->ops.push_back(op);

			return out;
		}

		Fail("cannot adapt " + std::to_string(in.channels.size()) + " channel(s) to " + std::to_string(want) +
			" -- this host handles mono and stereo only");

		return in;
	}

	// Aligns a lane that arrives early with one that arrives late. Without
	// this, summing two lanes of unequal latency comb-filters, which is the
	// single most likely way for a split to sound wrong (SF-51).
	Bus Align(const Bus& in, uint32_t target_latency)
	{
		if (failed || in.latency >= target_latency)
		{
			return in;
		}

		const uint32_t samples = target_latency - in.latency;

		Bus out;
		out.latency = target_latency;
		out.channels = NewBuses(static_cast<unsigned>(in.channels.size()));

		for (size_t ch = 0; ch < in.channels.size(); ++ch)
		{
			ChainOp op;
			op.kind = OpKind::Delay;
			op.dst = out.channels[ch];
			op.src0 = in.channels[ch];
			op.delay_line = static_cast<uint32_t>(plan->delay_lengths.size());
			plan->ops.push_back(op);

			plan->delay_lengths.push_back(samples);
		}

		return out;
	}

	Bus PlanList(const SpecList& nodes, const Bus& input)
	{
		Bus current = input;

		for (const SpecNode& node : nodes)
		{
			if (failed)
			{
				return current;
			}

			if (!node.IsSplit())
			{
				current = PlanPlugin(current);
				continue;
			}

			current = PlanSplit(node, current);
		}

		return current;
	}

	Bus PlanPlugin(const Bus& input)
	{
		if (next_stage >= stage_io->size())
		{
			Fail("more plugins in the chain than channel counts were given for");
			return input;
		}

		const StageIo& io = (*stage_io)[next_stage];

		// Checked here rather than only where an adapter happens to be needed:
		// a three-output plugin at the end of the chain would otherwise sail
		// through and hand the board three outputs it never claimed to have.
		if (io.inputs > 2 || io.outputs > 2 || io.outputs == 0)
		{
			Fail("stage " + std::to_string(next_stage) + " has " + std::to_string(io.inputs) + " input(s) and " +
				std::to_string(io.outputs) + " output(s) -- this host handles mono and stereo only");
			return input;
		}

		// A plugin with no audio inputs is a generator: it replaces the signal
		// rather than processing it, so there is nothing to adapt into it.
		Bus current = io.inputs == 0 ? input : Adapt(input, io.inputs);

		if (failed)
		{
			return current;
		}

		Bus output;
		output.channels = NewBuses(io.outputs);
		output.latency = current.latency + io.latency;

		plan->stage_input_buses[next_stage] = io.inputs == 0 ? std::vector<uint32_t>() : current.channels;
		plan->stage_output_buses[next_stage] = output.channels;

		ChainOp op;
		op.kind = OpKind::Run;
		op.stage = static_cast<uint32_t>(next_stage);
		plan->ops.push_back(op);

		++next_stage;

		return output;
	}

	// What one lane is fed, before its own send (SF-75). Full range hands back
	// the input untouched, which is what keeps a default splitter free: no op,
	// no bus, and a plan identical to the one this produced before splitters
	// existed.
	Bus Divide(const SpecNode& node, const Bus& input, unsigned lane)
	{
		if (failed)
		{
			return input;
		}

		switch (node.splitter.mode)
		{
			case SplitMode::Full:
				return input;

			case SplitMode::Crossover:
			{
				// Below into A, above into B. Every channel of the input gets
				// its own filter: the state is per channel, and one filter
				// shared between two would smear each into the other.
				Bus out;
				out.latency = input.latency;
				out.channels = NewBuses(static_cast<unsigned>(input.channels.size()));

				for (size_t ch = 0; ch < input.channels.size(); ++ch)
				{
					ChainOp op;
					op.kind = OpKind::Filter;
					op.dst = out.channels[ch];
					op.src0 = input.channels[ch];
					op.filter = static_cast<uint32_t>(plan->filters.size());
					plan->ops.push_back(op);

					CrossoverSpec spec;
					spec.frequency = node.splitter.frequency;
					spec.highpass = lane == 1;
					plan->filters.push_back(spec);
				}

				return out;
			}

			case SplitMode::Stereo:
			{
				// One side of a stereo input into each lane. A one-channel
				// input has no sides, so this is Full -- the mode still
				// selects and the board still plays, it just has nothing to
				// divide yet (SF-76). Refusing here would take the sound away
				// for a reason nobody could see.
				if (input.channels.size() != 2)
				{
					return input;
				}

				const SplitSource source = node.splitter.source[lane];

				if (source == SplitSource::Sum)
				{
					return Adapt(input, 1);
				}

				// Taking a side is not a copy: the lane reads the input
				// channel it wants directly, which costs no op and no buffer.
				Bus out;
				out.latency = input.latency;
				out.channels = {input.channels[source == SplitSource::Left ? 0 : 1]};

				return out;
			}
		}

		return input;
	}

	// The lane's send: how hard it drives what is in it, as opposed to the
	// `gain_db` return that decides how much comes back. Unity costs nothing,
	// which is what a preset written before sends existed asks for.
	Bus Send(const Bus& in, float send_db)
	{
		if (failed || send_db == 0.0f)
		{
			return in;
		}

		const float gain = GainFromDb(send_db);

		Bus out;
		out.latency = in.latency;
		out.channels = NewBuses(static_cast<unsigned>(in.channels.size()));

		for (size_t ch = 0; ch < in.channels.size(); ++ch)
		{
			ChainOp op;
			op.kind = OpKind::Gain;
			op.dst = out.channels[ch];
			op.src0 = in.channels[ch];
			op.gain0 = gain;
			plan->ops.push_back(op);
		}

		return out;
	}

	Bus PlanSplit(const SpecNode& node, const Bus& input)
	{
		if (node.branches.size() != 2)
		{
			Fail("a split has exactly two branches, not " + std::to_string(node.branches.size()));
			return input;
		}

		// Numbered before the branches are walked, so a split's merge index is
		// its position in a plain pre-order walk of the shape. A caller that
		// wants to address this merge later can count splits the same way
		// without knowing anything about how the plan came out.
		const uint32_t merge_index = plan->merge_count++;
		plan->merge_gain.resize(2 * plan->merge_count, 1.0f);

		// What each lane is actually handed. With the default splitter this is
		// `input` itself for both -- no ops, nothing allocated, the plan a
		// split produced before there was a choice of splitter.
		//
		// Sharing one bus between two lanes is safe because a plugin never
		// writes to its input buffers -- it writes to the fresh output buses
		// allocated for it -- so nothing branch 0 does can disturb what
		// branch 1 reads.
		const Bus feed_a = Send(Divide(node, input, 0), node.branches[0].send_db);
		const Bus feed_b = Send(Divide(node, input, 1), node.branches[1].send_db);

		if (failed)
		{
			return input;
		}

		Bus a = PlanList(node.branches[0].nodes, feed_a);
		Bus b = PlanList(node.branches[1].nodes, feed_b);

		if (failed)
		{
			return input;
		}

		const unsigned width =
			static_cast<unsigned>(std::max(a.channels.size(), b.channels.size()));

		a = Adapt(a, width);
		b = Adapt(b, width);

		const uint32_t latency = std::max(a.latency, b.latency);
		a = Align(a, latency);
		b = Align(b, latency);

		if (failed)
		{
			return input;
		}

		Bus merged;
		merged.channels = NewBuses(width);
		merged.latency = latency;

		const float gain_a = node.branches[0].muted ? 0.0f : GainFromDb(node.branches[0].gain_db);
		const float gain_b = node.branches[1].muted ? 0.0f : GainFromDb(node.branches[1].gain_db);

		plan->merge_gain[2 * merge_index] = gain_a;
		plan->merge_gain[2 * merge_index + 1] = gain_b;

		for (unsigned ch = 0; ch < width; ++ch)
		{
			ChainOp op;
			op.kind = OpKind::Mix;
			op.dst = merged.channels[ch];
			op.src0 = a.channels[ch];
			op.src1 = b.channels[ch];
			op.gain0 = gain_a;
			op.gain1 = gain_b;
			op.merge = merge_index;
			plan->ops.push_back(op);
		}

		return merged;
	}
};

}  // namespace

std::vector<std::string> FlattenSpec(const ChainSpec& spec)
{
	std::vector<std::string> uris;
	FlattenInto(spec, uris);

	return uris;
}

bool PlanChain(
	const ChainSpec& spec, const std::vector<StageIo>& stage_io, unsigned board_inputs, ChainPlan& out, std::string& error)
{
	out = ChainPlan();

	const size_t plugin_count = FlattenSpec(spec).size();

	if (plugin_count != stage_io.size())
	{
		error = "the chain has " + std::to_string(plugin_count) + " plugin(s) but " + std::to_string(stage_io.size()) +
			" channel count(s) were given";
		return false;
	}

	if (board_inputs != 1 && board_inputs != 2)
	{
		error = "a board takes 1 or 2 inputs, not " + std::to_string(board_inputs);
		return false;
	}

	out.stage_input_buses.resize(plugin_count);
	out.stage_output_buses.resize(plugin_count);

	Planner planner;
	planner.stage_io = &stage_io;
	planner.plan = &out;

	Bus input;
	input.channels = planner.NewBuses(board_inputs);

	out.input_buses = input.channels;

	Bus output = planner.PlanList(spec, input);

	if (planner.failed)
	{
		error = planner.error;
		out = ChainPlan();
		return false;
	}

	// The far end of the board, pinned. A chain that already ends in stereo is
	// handed straight back and costs nothing; a mono one gains two copies. Both
	// widths are 1 or 2 here, so this cannot fail.
	output = planner.Adapt(output, kBoardOutputs);

	out.output_buses = output.channels;
	out.latency = output.latency;

	// A board whose output buses *are* its input buses -- an empty chain, or
	// one that is nothing but dry wires -- would otherwise pass nothing: the
	// two ends get repointed at different JACK buffers, and no op ever moves
	// a sample from one to the other.
	bool aliased = false;

	for (uint32_t bus : out.output_buses)
	{
		if (std::find(out.input_buses.begin(), out.input_buses.end(), bus) != out.input_buses.end())
		{
			aliased = true;
			break;
		}
	}

	if (aliased)
	{
		std::vector<uint32_t> copied;

		for (uint32_t bus : out.output_buses)
		{
			const uint32_t dst = out.bus_count++;

			ChainOp op;
			op.kind = OpKind::Copy;
			op.dst = dst;
			op.src0 = bus;
			out.ops.push_back(op);

			copied.push_back(dst);
		}

		out.output_buses = copied;
	}

	return true;
}

void OpCopy(float* dst, const float* src, uint32_t frames)
{
	for (uint32_t i = 0; i < frames; ++i)
	{
		dst[i] = src[i];
	}
}

void OpDownmix(float* dst, const float* src0, const float* src1, uint32_t frames)
{
	for (uint32_t i = 0; i < frames; ++i)
	{
		dst[i] = 0.5f * (src0[i] + src1[i]);
	}
}

void OpGain(float* dst, const float* src, float gain, uint32_t frames)
{
	for (uint32_t i = 0; i < frames; ++i)
	{
		dst[i] = gain * src[i];
	}
}

void OpMix(float* dst, const float* src0, const float* src1, float gain0, float gain1, uint32_t frames)
{
	for (uint32_t i = 0; i < frames; ++i)
	{
		dst[i] = gain0 * src0[i] + gain1 * src1[i];
	}
}

void OpMixRamp(float* dst, const float* src0, const float* src1, float gain0_from, float gain0_to, float gain1_from,
	float gain1_to, uint32_t frames)
{
	if (frames == 0)
	{
		return;
	}

	// The step is per sample, and the last sample of the block lands one step
	// short of the target rather than on it. That is deliberate: the next
	// block starts *at* the target, so the sweep is continuous across the
	// boundary instead of holding the final value for one extra sample.
	const float step = 1.0f / static_cast<float>(frames);
	const float delta0 = (gain0_to - gain0_from) * step;
	const float delta1 = (gain1_to - gain1_from) * step;

	float gain0 = gain0_from;
	float gain1 = gain1_from;

	for (uint32_t i = 0; i < frames; ++i)
	{
		dst[i] = gain0 * src0[i] + gain1 * src1[i];

		gain0 += delta0;
		gain1 += delta1;
	}
}

float GainFromDb(float db)
{
	return std::pow(10.0f, db / 20.0f);
}

void CrossoverFilter::Reset(double sample_rate, const CrossoverSpec& spec)
{
	sections_[0] = Section();
	sections_[1] = Section();

	if (sample_rate <= 0.0)
	{
		return;
	}

	// Clamped rather than refused. The frequency comes from a number a player
	// typed into a box, and a board that goes silent because someone asked for
	// 30 kHz is a worse answer than one that runs out of band.
	const double nyquist = sample_rate * 0.5;
	const double frequency = std::min(std::max(static_cast<double>(spec.frequency), 1.0), nyquist * 0.99);

	// One 2nd-order Butterworth section, designed twice. Two of these in
	// series is the 4th-order Linkwitz-Riley half: LR(2n) is defined as
	// Butterworth(n) squared, which is exactly what makes the two halves sum
	// to unity magnitude instead of the +3 dB a Butterworth pair gives.
	const double omega = 2.0 * 3.14159265358979323846 * frequency / sample_rate;
	const double cosine = std::cos(omega);
	const double sine = std::sin(omega);

	// Q = 1/sqrt(2): Butterworth, the maximally flat one.
	const double alpha = sine / (2.0 * 0.70710678118654752440);
	const double a0 = 1.0 + alpha;

	Section section;
	section.a1 = static_cast<float>(-2.0 * cosine / a0);
	section.a2 = static_cast<float>((1.0 - alpha) / a0);

	if (spec.highpass)
	{
		section.b0 = static_cast<float>(((1.0 + cosine) * 0.5) / a0);
		section.b1 = static_cast<float>((-(1.0 + cosine)) / a0);
		section.b2 = section.b0;
	}
	else
	{
		section.b0 = static_cast<float>(((1.0 - cosine) * 0.5) / a0);
		section.b1 = static_cast<float>((1.0 - cosine) / a0);
		section.b2 = section.b0;
	}

	sections_[0] = section;
	sections_[1] = section;
}

void CrossoverFilter::Process(float* dst, const float* src, uint32_t frames)
{
	for (uint32_t i = 0; i < frames; ++i)
	{
		float sample = src[i];

		for (Section& section : sections_)
		{
			const float out = section.b0 * sample + section.z1;

			section.z1 = section.b1 * sample - section.a1 * out + section.z2;
			section.z2 = section.b2 * sample - section.a2 * out;

			sample = out;
		}

		dst[i] = sample;
	}
}

void DelayLine::Reset(uint32_t length)
{
	length_ = length;
	write_ = 0;

	buffer_.assign(length + 1, 0.0f);
}

void DelayLine::Process(float* dst, const float* src, uint32_t frames)
{
	if (length_ == 0)
	{
		OpCopy(dst, src, frames);
		return;
	}

	const uint32_t size = static_cast<uint32_t>(buffer_.size());

	for (uint32_t i = 0; i < frames; ++i)
	{
		// Read before write, so a delay of exactly one sample works rather
		// than reading back what was just stored.
		const uint32_t read = (write_ + size - length_) % size;

		buffer_[write_] = src[i];
		dst[i] = buffer_[read];

		write_ = (write_ + 1) % size;
	}
}

}  // namespace sf
