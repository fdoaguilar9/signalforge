#pragma once

#include <string>
#include <vector>

#include "Preset.h"

namespace sf
{

// A directory of preset files, in switch order.
//
// Order matters for a pedalboard (next/prev under a footswitch), so it can
// be stated explicitly in a "manifest.json":
//
//   { "presets": ["clean.json", "crunch.json", "lead.json"] }
//
// When there's no manifest, every *.json in the directory is included,
// sorted by filename -- so just dropping files in a folder works, and
// users who care about order can either add a manifest or name files
// "01-clean.json" style.
class PresetLibrary
{
public:
	struct Entry
	{
		std::string path;
		std::string stem;  // filename without ".json"
		Preset preset;
	};

	// Loads every preset in `directory`. Individual presets that fail to
	// parse are reported in `warnings` and skipped rather than failing the
	// whole library -- one bad file shouldn't cost the player their whole
	// pedalboard. Returns false only if the directory itself is unusable.
	static bool Open(
		const std::string& directory, PresetLibrary& out, std::string& error, std::vector<std::string>& warnings);

	size_t Count() const { return entries_.size(); }
	const Entry& At(size_t index) const { return entries_[index]; }

	// Resolves a CLI argument to an index. Accepts, in order:
	//   - a decimal index ("2")
	//   - a preset's "name" field ("Crunch Rhythm")
	//   - a filename stem ("crunch")
	// Returns false if nothing matches.
	bool Resolve(const std::string& query, size_t& index_out) const;

private:
	std::vector<Entry> entries_;
};

}  // namespace sf
