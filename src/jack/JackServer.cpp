#include "JackServer.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sf
{

namespace
{

// How long to wait for a freshly spawned jackd to start accepting clients,
// and how often to poll while waiting.
constexpr int kStartupTimeoutMs = 5000;
constexpr int kPollIntervalMs = 100;

// How long to give a server we asked to stop before escalating to SIGKILL.
constexpr int kStopTimeoutMs = 3000;

}  // namespace

JackServer::~JackServer()
{
	Stop();
}

bool JackServer::EnsureRunning(const AudioConfig& config, std::string& error)
{
	if (IsJackServerRunning())
	{
		// Already up, whether that's what `config.driver` promised or not --
		// there's nothing for us to start either way.
		return true;
	}

	if (config.driver == "external")
	{
		error = "no JACK server is running, but the audio config says to use an existing one";
		return false;
	}

	// The same escape hatch libjack itself honours for its own auto-start:
	// if the environment says not to start a server, that has to bind us
	// too, not just the JACK client library's own auto-start path.
	if (std::getenv("JACK_NO_START_SERVER"))
	{
		error = "JACK_NO_START_SERVER is set; refusing to start our own jackd";
		return false;
	}

	std::vector<std::string> args = {"jackd", "-R", "-d", "alsa"};

	if (!config.device.empty())
	{
		args.push_back("-d");
		args.push_back(config.device);
	}

	args.push_back("-r");
	args.push_back(std::to_string(static_cast<long>(config.sample_rate)));
	args.push_back("-p");
	args.push_back(std::to_string(config.buffer_size));

	std::vector<char*> argv;
	argv.reserve(args.size() + 1);

	for (std::string& arg : args)
	{
		argv.push_back(arg.data());
	}

	argv.push_back(nullptr);

	const pid_t pid = fork();

	if (pid < 0)
	{
		error = std::string("fork() failed: ") + std::strerror(errno);
		return false;
	}

	if (pid == 0)
	{
		// Child. If this process dies (crash, kill -9), the kernel sends us
		// SIGTERM instead of leaving jackd running with nothing managing it --
		// belt-and-braces alongside Stop(), which handles the clean-exit case.
		prctl(PR_SET_PDEATHSIG, SIGTERM);

		execvp("jackd", argv.data());

		// Only reached if exec failed.
		std::fprintf(stderr, "Failed to exec jackd: %s\n", std::strerror(errno));
		_exit(127);
	}

	// Parent.
	for (int waited = 0; waited < kStartupTimeoutMs; waited += kPollIntervalMs)
	{
		int wstatus = 0;
		const pid_t reaped = waitpid(pid, &wstatus, WNOHANG);

		if (reaped == pid)
		{
			error = "jackd exited immediately (is it installed? is the device available?)";
			return false;
		}

		if (IsJackServerRunning())
		{
			pid_ = pid;
			owns_ = true;
			return true;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
	}

	error = "jackd did not come up within " + std::to_string(kStartupTimeoutMs) + "ms";

	// Don't leave a half-started process behind on a timeout.
	kill(pid, SIGTERM);
	waitpid(pid, nullptr, 0);

	return false;
}

void JackServer::Stop()
{
	if (!owns_ || pid_ <= 0)
	{
		return;
	}

	kill(pid_, SIGTERM);

	for (int waited = 0; waited < kStopTimeoutMs; waited += kPollIntervalMs)
	{
		int wstatus = 0;
		const pid_t reaped = waitpid(pid_, &wstatus, WNOHANG);

		if (reaped == pid_)
		{
			pid_ = -1;
			owns_ = false;
			return;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
	}

	// It didn't go quietly.
	kill(pid_, SIGKILL);
	waitpid(pid_, nullptr, 0);

	pid_ = -1;
	owns_ = false;
}

}  // namespace sf
