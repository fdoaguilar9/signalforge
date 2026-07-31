#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <lilv/lilv.h>
#include <lv2/options/options.h>

#include "AudioHost.h"
#include "PathProperty.h"
#include "PortInfo.h"
#include "UridMap.h"
#include "Worker.h"

namespace sf
{

// Hosts a single LV2 plugin instance: connects every port (control/atom
// ports to persistent buffers, audio ports to a scratch buffer initially),
// activates it, and runs it one block at a time.
//
// Audio port buffers are meant to be repointed every block by the caller
// (see ConnectAudioPort) directly at the real I/O buffer (e.g. a JACK port
// buffer) for that cycle -- no copying.
class PluginStage : public AudioHost
{
public:
	// Returns nullptr on failure (missing required host features, bad URI,
	// etc), with a diagnostic already printed to stderr.
	static std::unique_ptr<PluginStage> Create(
		LilvWorld* world, const LilvPlugin* plugin, double sample_rate, uint32_t max_block_size, UridMap& urid_map);

	~PluginStage() override;

	PluginStage(const PluginStage&) = delete;
	PluginStage& operator=(const PluginStage&) = delete;

	const std::vector<PortInfo>& Ports() const override { return ports_; }

	// The plugin's URI, as a preset needs to record it.
	std::string Uri() const;

	// The plugin's declared name, for showing a person what's in the chain.
	// Falls back to the URI when the plugin declares no usable name.
	std::string Name() const;

	// Sets a control port's value by symbol (e.g. "gain"), clamped to the
	// port's declared [min, max] range. Returns false (with a diagnostic on
	// stderr) if no control port with that symbol exists.
	bool SetControl(const std::string& symbol, float value);

	// Returns the control port's current value, or NaN if no control port
	// with that symbol exists.
	float GetControl(const std::string& symbol) const;

	// Repoints an audio port's buffer for the upcoming Run() call. `index`
	// is the port's index as reported in Ports().
	void ConnectAudioPort(uint32_t index, float* buffer) override;

	// Clears atom sequence buffers (input and output) ready for the next
	// Run() call. No-op if the plugin has no atom ports.
	void PrepareAtomPorts() override;

	void Run(uint32_t sample_count) override;

	// Invokes the plugin's state:interface restore() to set a single
	// atom:Path-typed property (e.g. neural-amp-modeler-lv2's "model"
	// parameter) directly, without going through a full preset/state file --
	// that's SF-12/13's job. Requires the plugin to implement state:interface
	// and (per LV2 convention) do any actual file loading via the worker
	// feature, which must already be bound (see Create()).
	//
	// Returns false if the plugin has no state:interface, or restore()
	// itself fails.
	bool LoadPathProperty(const std::string& property_uri, const std::string& file_path, UridMap& urid_map);

	// Reads one atom:Path property back out of the plugin -- which model or
	// impulse response is loaded right now. Empty when nothing is set, when
	// the plugin has no state:interface, or when the property isn't one of
	// its own. Asks the plugin rather than remembering what we set, because a
	// path restored from a preset's state blob was never seen by us.
	std::string GetPathProperty(const std::string& property_uri, UridMap& urid_map) const;

	// The plugin's file-valued parameters, from its TTL. Looked up on demand
	// like Name(): this is for showing a person what a stage can load, so it
	// happens at UI speed, never on the audio thread.
	std::vector<PathProperty> PathProperties() const;

	// Blocks until all scheduled worker activity has completed and its
	// responses have been delivered to the plugin.
	//
	// Needed because worker-based loading (LoadPathProperty and friends) is
	// asynchronous: the plugin doesn't actually switch to the new model/IR
	// until a response is delivered on a subsequent Run(). Live playback
	// just picks it up a few blocks later, but offline processing can run
	// faster than the load completes, so the result would depend on disk
	// speed. Setup-time only -- never call this from the audio thread.
	void SettleWorker();

	// Snapshots the plugin's own state (the state:interface save() output --
	// e.g. neural-amp-modeler-lv2's atom:Path to the loaded .nam) and
	// serializes it to a string suitable for the preset "state" field.
	// Returns an empty string if the plugin has no state to save.
	//
	// Deliberately does *not* include control port values: those live in the
	// preset's own "controls" map so they stay human-editable, and keeping
	// exactly one home per piece of data avoids two sources of truth
	// disagreeing on restore.
	std::string SaveState(UridMap& urid_map) const;

	// Applies a state string produced by SaveState(). Like LoadPathProperty,
	// the actual file loading happens on the worker thread, so call
	// SettleWorker() before relying on the result.
	//
	// If the state happens to carry control port values (ours don't, but a
	// hand-written or third-party one might), they're applied here -- and
	// any explicit preset controls applied afterwards deliberately win.
	bool RestoreState(const std::string& serialized_state, UridMap& urid_map);

	// Current values of every control port, keyed by symbol -- the other
	// half of what a preset needs to capture.
	std::map<std::string, float> CaptureControls() const;

	// Takes the plugin out of the signal path: Run() copies audio inputs
	// straight to outputs instead of calling the plugin.
	//
	// This is *true* bypass -- the plugin stops running, so reverb/delay
	// tails are cut rather than ringing out, which is how a stompbox behaves
	// and what a player stomping mid-song expects.
	//
	// Safe to call from the control thread while audio is running: the flag
	// is atomic and takes effect on the next block. The switch is not
	// crossfaded, so bypassing something with a large DC offset can click;
	// in practice guitar-signal plugins don't.
	//
	// Channel counts don't have to match. Each output takes the
	// same-numbered input, the last input repeats if there are more outputs
	// than inputs (so a mono->stereo plugin bypasses to both sides), and
	// outputs are silenced if the plugin has no audio inputs at all.
	void SetBypassed(bool bypassed);
	bool IsBypassed() const;

private:
	PluginStage() = default;

	static void SetPortValueCallback(
		const char* port_symbol, void* user_data, const void* value, uint32_t size, uint32_t type);

	// atom:Float, cached so the restore callback can check value types
	// without touching the (not thread-safe) URID map.
	uint32_t float_urid_ = 0;

	LilvInstance* instance_ = nullptr;

	// Kept for state capture (lilv_state_new_from_instance needs the plugin,
	// lilv_state_to_string needs the world). Neither is owned.
	LilvWorld* world_ = nullptr;
	const LilvPlugin* plugin_ = nullptr;

	std::vector<PortInfo> ports_;

	// Backing storage for non-audio ports, and scratch buffers for audio
	// ports until the caller repoints them via ConnectAudioPort. Indexed by
	// port index.
	std::vector<float> control_storage_;
	std::vector<std::vector<uint8_t>> atom_storage_;
	std::vector<std::vector<float>> audio_scratch_;

	// Where each audio port currently points, mirrored from
	// ConnectAudioPort -- lilv hands the pointer to the plugin and doesn't
	// hand it back, but bypass needs to copy between them. Indexed by port
	// index; null for non-audio ports.
	std::vector<float*> audio_buffers_;

	// Audio port indices by direction, in port order. Cached at Create()
	// so the bypass path does no searching per block.
	std::vector<uint32_t> audio_inputs_;
	std::vector<uint32_t> audio_outputs_;

	// Read by the audio thread every block, written by the control thread
	// when a footswitch is stomped.
	std::atomic<bool> bypassed_{false};

	// The LV2 spec requires the feature array (and anything it points to)
	// to stay valid for the plugin instance's whole lifetime, not just the
	// instantiate() call -- so these live here, not as locals in Create().
	int32_t max_block_length_value_ = 0;
	LV2_Options_Option options_[2] = {};
	LV2_Feature options_feature_ = {};
	LV2_Feature bounded_block_length_feature_ = {};

	Worker worker_;
};

}  // namespace sf
