#include "OfflineEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sf
{

OfflineEngine::OfflineEngine(uint32_t block_size)
	: block_size_(block_size)
{
}

OfflineEngine::Result OfflineEngine::Run(
	AudioHost& host, uint64_t total_frames, const std::function<float(uint64_t)>& generator)
{
	std::vector<uint32_t> input_indices;
	std::vector<uint32_t> output_indices;

	for (const PortInfo& port : host.Ports())
	{
		if (port.type != PortType::Audio)
		{
			continue;
		}

		if (port.direction == PortDirection::Input)
		{
			input_indices.push_back(port.index);
		}
		else
		{
			output_indices.push_back(port.index);
		}
	}

	std::vector<std::vector<float>> input_buffers(input_indices.size(), std::vector<float>(block_size_, 0.0f));
	std::vector<std::vector<float>> output_buffers(output_indices.size(), std::vector<float>(block_size_, 0.0f));

	Result result;
	result.input_channels = static_cast<uint32_t>(input_indices.size());
	result.output_channels = static_cast<uint32_t>(output_indices.size());

	double sum_squares = 0.0;
	uint64_t finite_sample_count = 0;
	uint64_t frame = 0;

	uint64_t hash = 0xcbf29ce484222325ull;  // FNV-1a offset basis

	auto hash_sample = [&hash](float sample)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &sample, sizeof(bits));

		for (int byte = 0; byte < 4; ++byte)
		{
			hash ^= (bits >> (byte * 8)) & 0xff;
			hash *= 0x100000001b3ull;  // FNV-1a prime
		}
	};

	while (frame < total_frames)
	{
		const uint32_t frames_this_block =
			static_cast<uint32_t>(std::min<uint64_t>(block_size_, total_frames - frame));

		for (size_t ch = 0; ch < input_buffers.size(); ++ch)
		{
			for (uint32_t i = 0; i < frames_this_block; ++i)
			{
				input_buffers[ch][i] = generator(frame + i);
			}

			host.ConnectAudioPort(input_indices[ch], input_buffers[ch].data());
		}

		for (size_t ch = 0; ch < output_buffers.size(); ++ch)
		{
			// Zero first, so "the plugin wrote nothing" reads as silence
			// rather than as whatever the previous block left behind.
			std::fill(output_buffers[ch].begin(), output_buffers[ch].end(), 0.0f);
			host.ConnectAudioPort(output_indices[ch], output_buffers[ch].data());
		}

		host.PrepareAtomPorts();
		host.Run(frames_this_block);

		for (size_t ch = 0; ch < output_buffers.size(); ++ch)
		{
			for (uint32_t i = 0; i < frames_this_block; ++i)
			{
				const float sample = output_buffers[ch][i];

				hash_sample(sample);

				if (!std::isfinite(sample))
				{
					++result.non_finite_samples;
					continue;
				}

				result.peak = std::max(result.peak, std::fabs(sample));
				sum_squares += static_cast<double>(sample) * static_cast<double>(sample);
				++finite_sample_count;
			}
		}

		frame += frames_this_block;
		result.frames_processed += frames_this_block;
	}

	if (finite_sample_count > 0)
	{
		result.rms = std::sqrt(sum_squares / static_cast<double>(finite_sample_count));
	}

	result.output_hash = hash;

	return result;
}

std::function<float(uint64_t)> MakeSineSweep(
	double sample_rate, double duration_seconds, double start_hz, double end_hz, float amplitude)
{
	// Exponential sweep: instantaneous frequency goes start_hz -> end_hz over
	// the duration, with phase integrated in closed form so this stays a pure
	// function of the frame index (no accumulator state to get out of sync).
	const double ratio = std::log(end_hz / start_hz);

	return [sample_rate, duration_seconds, start_hz, ratio, amplitude](uint64_t frame) -> float
	{
		const double t = static_cast<double>(frame) / sample_rate;
		const double phase =
			2.0 * M_PI * start_hz * duration_seconds / ratio * (std::exp(t / duration_seconds * ratio) - 1.0);

		return amplitude * static_cast<float>(std::sin(phase));
	};
}

}  // namespace sf
