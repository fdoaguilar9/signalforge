#pragma once

#include <string>
#include <vector>

#include "Preset.h"

namespace sf
{

// The preset library: one or more **banks**, each an ordered list of presets.
//
// A bank is a subdirectory of the preset directory. The loose `*.json` files
// in the directory itself are a bank too -- named "Default" -- so a library
// that predates banks keeps working, unmoved and unedited, and a second bank
// is a folder somebody makes.
//
// Order matters for a pedalboard (next/prev under a footswitch), so within a
// bank it can be stated explicitly in a "manifest.json":
//
//   { "presets": ["clean.json", "crunch.json", "lead.json"] }
//
// Without one, every *.json in that directory is included, sorted by filename
// -- so dropping files in a folder works, and users who care about order can
// either add a manifest or name files "01-clean.json" style. Banks themselves
// are ordered with Default first and the rest alphabetically; there is no
// manifest for bank order, because a bank is a place rather than a list.
//
// **Count() and At() report the *current* bank**, which is what every caller
// that predates banks already meant by "the library". Stepping to the next
// preset steps within a bank; changing bank is its own action.
class PresetLibrary
{
public:
	struct Entry
	{
		std::string path;
		std::string stem;  // filename without ".json"
		Preset preset;
	};

	struct Bank
	{
		std::string name;
		std::string directory;
		std::vector<Entry> entries;
	};

	// Where a preset is, across the whole library. A bare index means nothing
	// once there is more than one bank.
	struct Location
	{
		size_t bank = 0;
		size_t index = 0;
	};

	// Loads every bank under `directory`. Individual presets that fail to
	// parse are reported in `warnings` and skipped rather than failing the
	// whole library -- one bad file shouldn't cost the player their whole
	// pedalboard. A bank with no readable presets is left out entirely, so an
	// empty folder never becomes a bank you can stomp into and find nothing.
	//
	// Returns false only if the directory itself is unusable.
	static bool Open(
		const std::string& directory, PresetLibrary& out, std::string& error, std::vector<std::string>& warnings);

	size_t BankCount() const { return banks_.size(); }
	const Bank& BankAt(size_t index) const { return banks_[index]; }

	size_t CurrentBank() const { return current_bank_; }
	const std::string& CurrentBankName() const;

	// Returns false for an index that isn't a bank, leaving the selection
	// alone -- a refused bank change should keep playing what it was playing.
	bool SelectBank(size_t index);

	// The current bank.
	size_t Count() const;
	const Entry& At(size_t index) const;

	// Resolves a query to somewhere in the library. Accepts, in order:
	//   - "bank/preset", naming a bank explicitly
	//   - a decimal index ("2") within the current bank
	//   - a preset's "name" field ("Crunch Rhythm")
	//   - a filename stem ("crunch")
	//
	// The last three are looked for in the current bank first and then in
	// every other, so a name that is unique still works without qualifying it
	// and a name that isn't resolves to the one nearest to hand.
	bool Resolve(const std::string& query, Location& out) const;

	// Where a loaded file sits, if it came from the library at all.
	bool FindByPath(const std::string& path, Location& out) const;

	// "bank/stem", or just "stem" when it is in the Default bank -- the form
	// Resolve takes and the form worth writing into settings, since a bare
	// stem stops being unique the moment a second bank has one like it.
	std::string QualifiedName(const Location& at) const;

private:
	std::vector<Bank> banks_;
	size_t current_bank_ = 0;
};

}  // namespace sf
