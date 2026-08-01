#include "AudioConfig.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <jack/jack.h>

namespace sf
{

namespace
{

std::string Trim(const std::string& s)
{
	const size_t begin = s.find_first_not_of(" \t");

	if (begin == std::string::npos)
	{
		return {};
	}

	const size_t end = s.find_last_not_of(" \t");

	return s.substr(begin, end - begin + 1);
}

constexpr uint32_t kMinBufferSize = 16;
constexpr uint32_t kMaxBufferSize = 8192;

constexpr double kMinSampleRate = 8000.0;
constexpr double kMaxSampleRate = 192000.0;

}  // namespace

std::vector<AudioDevice> EnumerateAlsaDevices()
{
	std::vector<AudioDevice> devices;

	std::ifstream file("/proc/asound/cards");

	if (!file)
	{
		// No ALSA, or no cards -- a normal state on a build host or CI image,
		// not a failure.
		return devices;
	}

	// Each card is two lines:
	//  " 1 [AIRSTEP        ]: USB-Audio - AIRSTEP"
	//  "                      XSONIC AIRSTEP at usb-...-2, full speed"
	// Only the first line of each pair is parsed; the second is a detail
	// line we don't need.
	std::string line;

	while (std::getline(file, line))
	{
		const size_t bracket_open = line.find('[');
		const size_t bracket_close = line.find(']');
		const size_t colon = line.find(':');

		if (bracket_open == std::string::npos || bracket_close == std::string::npos ||
			bracket_close < bracket_open || colon == std::string::npos || colon < bracket_close)
		{
			continue;
		}

		AudioDevice device;

		try
		{
			device.card_index = static_cast<unsigned>(std::stoul(line.substr(0, bracket_open)));
		}
		catch (const std::exception&)
		{
			continue;
		}

		const std::string id = Trim(line.substr(bracket_open + 1, bracket_close - bracket_open - 1));
		const std::string description = Trim(line.substr(colon + 1));

		device.id = "hw:" + std::to_string(device.card_index);
		device.name = description.empty() ? id : description;

		devices.push_back(device);
	}

	return devices;
}

bool IsJackServerRunning()
{
	jack_status_t status;
	// JackNoStartServer: this must never have the side effect of starting
	// one -- that's JackServer's job (SF-35), done deliberately, not as a
	// side effect of asking whether it's needed.
	jack_client_t* probe = jack_client_open("signalforge-probe", JackNoStartServer, &status);

	if (!probe)
	{
		return false;
	}

	jack_client_close(probe);

	return true;
}

std::vector<PhysicalPort> EnumeratePhysicalPorts()
{
	std::vector<PhysicalPort> found;

	jack_status_t status;
	jack_client_t* probe = jack_client_open("signalforge-port-probe", JackNoStartServer, &status);

	if (!probe)
	{
		return found;
	}

	// No Input/Output bit in the request: a port matches if it has *all*
	// requested flags, so asking for just JackPortIsPhysical matches a
	// physical port regardless of which direction it is -- both hardware
	// sources and hardware sinks come back in one list.
	const char** ports = jack_get_ports(probe, nullptr, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical);

	if (ports)
	{
		for (size_t i = 0; ports[i]; ++i)
		{
			const std::string full_name = ports[i];
			const size_t colon = full_name.find(':');

			if (colon == std::string::npos)
			{
				continue;
			}

			jack_port_t* port = jack_port_by_name(probe, full_name.c_str());

			if (!port)
			{
				continue;
			}

			PhysicalPort entry;
			entry.name = full_name;
			entry.client = full_name.substr(0, colon);
			entry.port = full_name.substr(colon + 1);

			// A hardware *source* -- the interface's mic/instrument input --
			// appears to JACK as an output port, since data flows out of it
			// into the graph.
			entry.is_capture = (jack_port_flags(port) & JackPortIsOutput) != 0;

			found.push_back(entry);
		}

		jack_free(ports);
	}

	jack_client_close(probe);

	std::sort(found.begin(), found.end(), [](const PhysicalPort& a, const PhysicalPort& b)
		{ return a.client != b.client ? a.client < b.client : a.port < b.port; });

	return found;
}

void AudioConfig::Clamp()
{
	if (driver != "alsa" && driver != "external")
	{
		driver = "alsa";
	}

	sample_rate = std::min(kMaxSampleRate, std::max(kMinSampleRate, sample_rate));
	buffer_size = std::min(kMaxBufferSize, std::max(kMinBufferSize, buffer_size));
}

nlohmann::json AudioConfig::ToJson() const
{
	return nlohmann::json{
		{"driver", driver},
		{"device", device},
		{"sample_rate", sample_rate},
		{"buffer_size", buffer_size},
		{"input_ports", input_ports},
		{"output_ports", output_ports},
		{"configured", configured},
	};
}

AudioConfig AudioConfig::FromJson(const nlohmann::json& json)
{
	AudioConfig out;

	if (!json.is_object())
	{
		return out;
	}

	if (json.contains("driver") && json["driver"].is_string())
	{
		out.driver = json["driver"].get<std::string>();
	}

	if (json.contains("device") && json["device"].is_string())
	{
		out.device = json["device"].get<std::string>();
	}

	if (json.contains("sample_rate") && json["sample_rate"].is_number())
	{
		out.sample_rate = json["sample_rate"].get<double>();
	}

	if (json.contains("buffer_size") && json["buffer_size"].is_number_integer())
	{
		out.buffer_size = json["buffer_size"].get<uint32_t>();
	}

	if (json.contains("input_ports") && json["input_ports"].is_string())
	{
		out.input_ports = json["input_ports"].get<std::string>();
	}

	if (json.contains("output_ports") && json["output_ports"].is_string())
	{
		out.output_ports = json["output_ports"].get<std::string>();
	}

	if (json.contains("configured") && json["configured"].is_boolean())
	{
		out.configured = json["configured"].get<bool>();
	}

	out.Clamp();

	return out;
}

}  // namespace sf
