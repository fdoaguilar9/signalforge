// AudioConfig tests (SF-34): the data model, its JSON round-trip and
// clamping, plus enumeration on whatever machine this runs on. No JACK
// server is started -- IsJackServerRunning() only probes.

#include "../src/audio/AudioConfig.h"

#include <cstdio>

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

void TestClamp()
{
	sf::AudioConfig config;
	config.driver = "nonsense";
	config.sample_rate = -5.0;
	config.buffer_size = 0;
	config.Clamp();

	Check(config.driver == "alsa", "unknown driver falls back to alsa");
	Check(config.sample_rate >= 8000.0, "sample rate clamped up from a negative value");
	Check(config.buffer_size >= 16, "buffer size clamped up from zero");

	sf::AudioConfig huge;
	huge.sample_rate = 5'000'000.0;
	huge.buffer_size = 1'000'000;
	huge.Clamp();

	Check(huge.sample_rate <= 192000.0, "sample rate clamped down from an absurd value");
	Check(huge.buffer_size <= 8192, "buffer size clamped down from an absurd value");

	sf::AudioConfig external;
	external.driver = "external";
	external.Clamp();
	Check(external.driver == "external", "'external' driver is preserved by Clamp");
}

void TestJsonRoundTrip()
{
	sf::AudioConfig config;
	config.driver = "alsa";
	config.device = "hw:1";
	config.sample_rate = 44100.0;
	config.buffer_size = 128;
	config.input_ports = "airstep";
	config.output_ports = "scarlett";
	config.configured = true;

	const nlohmann::json json = config.ToJson();
	const sf::AudioConfig round_tripped = sf::AudioConfig::FromJson(json);

	Check(round_tripped.driver == config.driver, "driver round-trips");
	Check(round_tripped.device == config.device, "device round-trips");
	Check(round_tripped.sample_rate == config.sample_rate, "sample rate round-trips");
	Check(round_tripped.buffer_size == config.buffer_size, "buffer size round-trips");
	Check(round_tripped.input_ports == config.input_ports, "input port filter round-trips");
	Check(round_tripped.output_ports == config.output_ports, "output port filter round-trips");
	Check(round_tripped.configured == config.configured, "configured flag round-trips");
}

void TestFromJsonTolerance()
{
	// Same posture as Settings::FromJson: wrong types and unknown keys cost
	// only the value they carried, never the whole parse.
	const nlohmann::json json = {
		{"driver", 5},              // wrong type -- default kept
		{"device", "hw:2"},         // right type -- kept
		{"sample_rate", "fast"},    // wrong type -- default kept
		{"unknown_key", "ignored"},
	};

	const sf::AudioConfig config = sf::AudioConfig::FromJson(json);

	Check(config.driver == "alsa", "wrong-typed driver falls back to the default");
	Check(config.device == "hw:2", "well-typed device is still read");
	Check(config.sample_rate == 48000.0, "wrong-typed sample rate falls back to the default");

	const sf::AudioConfig from_non_object = sf::AudioConfig::FromJson(nlohmann::json("not an object"));
	Check(from_non_object.driver == "alsa", "a non-object payload yields plain defaults, not a crash");
}

void TestEnumeration()
{
	// Whatever this machine has -- zero cards or several -- the call must not
	// crash, and every entry must at least have a device id shaped like
	// jackd/ALSA expects one ("hw:N").
	const std::vector<sf::AudioDevice> devices = sf::EnumerateAlsaDevices();

	std::printf("info  found %zu ALSA device(s)\n", devices.size());

	bool all_well_formed = true;

	for (const sf::AudioDevice& device : devices)
	{
		if (device.id.rfind("hw:", 0) != 0 || device.name.empty())
		{
			all_well_formed = false;
		}

		std::printf("info    %s (%s)\n", device.id.c_str(), device.name.c_str());
	}

	Check(all_well_formed, "every enumerated device has a well-formed id and a name");

	// A probe, not a launch: this must never start a server as a side
	// effect. Called twice to make sure the first call didn't leave anything
	// running that the second then finds.
	const bool first = sf::IsJackServerRunning();
	const bool second = sf::IsJackServerRunning();

	Check(first == second, "probing for a server twice gives a consistent answer");
}

void TestEnumeratePhysicalPorts()
{
	// Same posture as the two probes above: must never crash and must never
	// start a server, whether or not one happens to be reachable here.
	const std::vector<sf::PhysicalPort> ports = sf::EnumeratePhysicalPorts();

	std::printf("info  found %zu physical port(s)\n", ports.size());

	bool all_well_formed = true;

	for (const sf::PhysicalPort& port : ports)
	{
		// name must be exactly "client:port" split at the colon -- the whole
		// dialog relies on that decomposition to label rows and on `name` to
		// match a saved setting back to a real port.
		if (port.name.empty() || port.client.empty() || port.port.empty() ||
			port.name != port.client + ":" + port.port)
		{
			all_well_formed = false;
		}

		std::printf("info    %-9s %s\n", port.is_capture ? "[capture]" : "[playback]", port.name.c_str());
	}

	Check(all_well_formed, "every port's name decomposes into exactly its client and port parts");

	// Individual ports, not devices: two channels of one interface must come
	// back as two entries, or a mono board has no way to choose between them.
	bool names_unique = true;

	for (size_t i = 0; i < ports.size(); ++i)
	{
		for (size_t j = i + 1; j < ports.size(); ++j)
		{
			if (ports[i].name == ports[j].name)
			{
				names_unique = false;
			}
		}
	}

	Check(names_unique, "no port is listed twice");

	// Can't enumerate ports on a server that isn't there.
	if (!sf::IsJackServerRunning())
	{
		Check(ports.empty(), "no reachable server means no ports");
	}
}

}  // namespace

int main()
{
	TestClamp();
	TestJsonRoundTrip();
	TestFromJsonTolerance();
	TestEnumeration();
	TestEnumeratePhysicalPorts();

	std::printf("\n%s\n", g_failures == 0 ? "All audio config tests passed." : "Audio config tests FAILED.");

	return g_failures == 0 ? 0 : 1;
}
