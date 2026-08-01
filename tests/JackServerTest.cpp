// JackServer tests (SF-35): the lifecycle manager that starts (and only
// ever stops) a jackd *we* spawned. Run wherever this happens to build --
// which usually has no jackd binary and no running server (PipeWire/JACK
// sockets aren't reachable from a bare CI sandbox), so the interesting case
// here is exactly that one: a clean, quick failure that leaves nothing
// behind, not a hang.

#include "../src/jack/JackServer.h"

#include <cstdio>
#include <cstdlib>

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

void TestStopWithoutStartIsANoOp()
{
	sf::JackServer server;
	server.Stop();  // must not crash, hang, or touch anything

	Check(!server.OwnsServer(), "a server that was never started reports OwnsServer() == false");
}

void TestExternalDriverWithNoServerFails()
{
	// "external" is a promise that a server is already up. If that promise
	// turns out false, EnsureRunning must say so rather than silently trying
	// to start one anyway -- that's what the driver field is for.
	sf::AudioConfig config;
	config.driver = "external";

	if (sf::IsJackServerRunning())
	{
		std::printf("skip  a JACK server is actually reachable here; nothing to test\n");
		return;
	}

	sf::JackServer server;
	std::string error;
	const bool result = server.EnsureRunning(config, error);

	Check(!result, "EnsureRunning refuses 'external' when no server is reachable");
	Check(!error.empty(), "the refusal comes with a reason");
	Check(!server.OwnsServer(), "no server was started");
}

void TestMissingOrFailingJackdIsReportedNotHung()
{
	if (sf::IsJackServerRunning())
	{
		std::printf("skip  a JACK server is actually reachable here; nothing to test\n");
		return;
	}

	sf::AudioConfig config;
	config.driver = "alsa";
	config.device = "hw:9999";  // near-certainly nonexistent either way

	sf::JackServer server;
	std::string error;
	const bool result = server.EnsureRunning(config, error);

	// Whether jackd is missing (exec fails, child exits 127 immediately) or
	// present but refuses a nonexistent device, this must fail with a
	// reason rather than hang for the full startup timeout or crash.
	Check(!result, "EnsureRunning fails cleanly rather than hanging");
	Check(!error.empty(), "the failure comes with a reason");
	Check(!server.OwnsServer(), "a failed start never claims ownership of a server");
}

void TestJackNoStartServerIsHonoured()
{
	if (sf::IsJackServerRunning())
	{
		std::printf("skip  a JACK server is actually reachable here; nothing to test\n");
		return;
	}

	// The environment variable libjack itself honours for its own
	// auto-start has to bind our own spawn path too, or setting it would
	// stop libjack starting a server just for us to start one anyway.
	setenv("JACK_NO_START_SERVER", "1", 1);

	sf::AudioConfig config;
	config.driver = "alsa";

	sf::JackServer server;
	std::string error;
	const bool result = server.EnsureRunning(config, error);

	unsetenv("JACK_NO_START_SERVER");

	Check(!result, "EnsureRunning refuses to spawn jackd when JACK_NO_START_SERVER is set");
	Check(error.find("JACK_NO_START_SERVER") != std::string::npos, "the reason names the environment variable");
}

}  // namespace

int main()
{
	TestStopWithoutStartIsANoOp();
	TestExternalDriverWithNoServerFails();
	TestMissingOrFailingJackdIsReportedNotHung();
	TestJackNoStartServerIsHonoured();

	std::printf("\n%s\n", g_failures == 0 ? "All JACK server tests passed." : "JACK server tests FAILED.");

	return g_failures == 0 ? 0 : 1;
}
