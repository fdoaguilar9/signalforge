#pragma once

#include <string>

#include <sys/types.h>

#include "../audio/AudioConfig.h"

namespace sf
{

// Starts and owns a `jackd` process, or does nothing at all when a server is
// already reachable (PipeWire's pipewire-jack shim counts as one -- it opens
// JACK clients just the same). SignalForge only ever stops a server it
// itself started; one found already running at startup is left completely
// alone, on the theory that whatever put it there put it there on purpose.
class JackServer
{
public:
	~JackServer();

	JackServer() = default;
	JackServer(const JackServer&) = delete;
	JackServer& operator=(const JackServer&) = delete;

	// If a server is already reachable, does nothing and returns true
	// (OwnsServer() stays false). Otherwise spawns `jackd -R -d alsa
	// [-d <device>] -r <rate> -p <buffer>` and waits for it to accept
	// clients. `config.driver == "external"` is a promise that a server is
	// already up -- that path is checked, not trusted blindly, and still
	// fails clearly if it turns out not to be true.
	//
	// Returns false with a reason in `error` if jackd isn't installed, fails
	// to start, or doesn't come up within the timeout. Failure leaves no
	// process behind.
	bool EnsureRunning(const AudioConfig& config, std::string& error);

	// Stops a server this instance started. No-op (not an error) if we
	// didn't start one, so it's always safe to call during teardown.
	void Stop();

	bool OwnsServer() const { return owns_; }

private:
	pid_t pid_ = -1;
	bool owns_ = false;
};

}  // namespace sf
