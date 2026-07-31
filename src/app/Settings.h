#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "../audio/Crossfade.h"

namespace sf
{

// What the program remembers between runs, and what it lets you tune without
// rebuilding it (SF-32, SF-33).
//
// Deliberately *not* the preset format. A preset is a sound, gets copied
// between machines and checked into a repo; this is one machine's habits --
// how big the window was and which preset was playing when the power went
// off. Mixing the two would mean copying a preset also moved somebody's window
// around.
struct Settings
{
	// The window's last size. Position is not kept on purpose: under Wayland a
	// client cannot place its own window, so a remembered position is a value
	// that silently does nothing on half the machines this runs on.
	int window_width = 720;
	int window_height = 560;

	// The filename stem of the preset that was playing, so a board comes back
	// up where it left off. A stem rather than a path because presets get
	// moved between directories with --preset-dir, and the name is the part
	// that survives that.
	std::string last_preset;

	// SF-33. Exposed here because the right length is a matter of taste and of
	// the pedals in the chain -- a fuzz and a clean amp want a different
	// handover from two takes of the same amp.
	double crossfade_ms = Crossfade::kDefaultMilliseconds;
	CrossfadeCurve crossfade_curve = CrossfadeCurve::EqualPower;

	// $XDG_CONFIG_HOME/signalforge/settings.json, or ~/.config/... when that
	// isn't set. Empty if neither XDG_CONFIG_HOME nor HOME is set, which is
	// treated as "don't persist anything" rather than as an error -- that's a
	// service account or a container, not a user who lost their settings.
	static std::string DefaultPath();

	// A file that isn't there is not a failure: it's the first run. Returns
	// false only when a file exists and can't be used, with the reason in
	// `error`; unknown keys are ignored and out-of-range values are clamped,
	// because a settings file should never be the reason a board won't play.
	bool Load(const std::string& path, std::string& error);

	// Creates the directory if needed. Returns false with a reason; callers
	// warn rather than fail, for the same reason.
	bool Save(const std::string& path, std::string& error) const;

	nlohmann::json ToJson() const;
	static Settings FromJson(const nlohmann::json& json);

	// Applies the same limits Load does, so a value set from the command line
	// can't do what a value in the file can't.
	void Clamp();
};

}  // namespace sf
