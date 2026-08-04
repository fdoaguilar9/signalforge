#include "PresetLibrary.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace sf
{

namespace
{

constexpr const char* kManifestName = "manifest.json";

// The MIDI mapping table lives alongside the presets (PLAN.md §5.2) but
// isn't one. Without this it gets scanned as a preset and warns on every
// startup.
constexpr const char* kMappingName = "mapping.json";

// Reads the ordered filename list out of manifest.json. Returns false if
// there's no manifest (not an error -- we fall back to a directory scan).
bool ReadManifest(const std::filesystem::path& directory, std::vector<std::string>& filenames, std::string& error)
{
	const std::filesystem::path manifest_path = directory / kManifestName;

	if (!std::filesystem::exists(manifest_path))
	{
		return false;
	}

	std::ifstream file(manifest_path);

	if (!file)
	{
		error = "cannot open '" + manifest_path.string() + "'";
		return false;
	}

	nlohmann::json json;

	try
	{
		json = nlohmann::json::parse(file, nullptr, true, true);
	}
	catch (const nlohmann::json::parse_error& e)
	{
		error = "invalid JSON in '" + manifest_path.string() + "': " + e.what();
		return false;
	}

	if (!json.contains("presets") || !json["presets"].is_array())
	{
		error = "'" + manifest_path.string() + "' needs a \"presets\" array";
		return false;
	}

	for (const auto& entry : json["presets"])
	{
		if (!entry.is_string())
		{
			error = "'" + manifest_path.string() + "': every \"presets\" entry must be a filename string";
			return false;
		}

		filenames.push_back(entry.get<std::string>());
	}

	return true;
}

// The bank the loose files in the preset directory itself make up. Named
// rather than blank so that the header's breadcrumb and the bank selector
// have something to say on a library that has never heard of banks.
constexpr const char* kDefaultBankName = "Default";

// Loads one directory's worth of presets, in manifest order or filename order.
// Returns false only when a manifest exists and is broken -- a directory with
// nothing loadable in it is simply an empty bank.
bool LoadBank(const std::filesystem::path& dir, std::vector<PresetLibrary::Entry>& out, std::string& error,
	std::vector<std::string>& warnings)
{
	std::error_code ec;
	std::vector<std::string> filenames;
	std::string manifest_error;

	if (!ReadManifest(dir, filenames, manifest_error))
	{
		if (!manifest_error.empty())
		{
			error = manifest_error;
			return false;
		}

		// No manifest: take every *.json except the manifest itself, sorted
		// by filename for a stable, predictable order.
		for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
		{
			if (!entry.is_regular_file() || entry.path().extension() != ".json")
			{
				continue;
			}

			if (entry.path().filename() == kManifestName || entry.path().filename() == kMappingName)
			{
				continue;
			}

			filenames.push_back(entry.path().filename().string());
		}

		std::sort(filenames.begin(), filenames.end());
	}

	for (const std::string& filename : filenames)
	{
		const std::filesystem::path path = dir / filename;

		PresetLibrary::Entry entry;
		entry.path = path.string();
		entry.stem = path.stem().string();

		std::string load_error;

		if (!LoadPresetFile(entry.path, entry.preset, load_error))
		{
			// Skip, don't abort: one unreadable preset shouldn't take the
			// rest of the pedalboard down with it.
			warnings.push_back("skipping preset: " + load_error);
			continue;
		}

		out.push_back(std::move(entry));
	}

	return true;
}

}  // namespace

bool PresetLibrary::Open(
	const std::string& directory, PresetLibrary& out, std::string& error, std::vector<std::string>& warnings)
{
	out = PresetLibrary();

	const std::filesystem::path dir(directory);

	std::error_code ec;

	if (!std::filesystem::is_directory(dir, ec))
	{
		error = "'" + directory + "' is not a directory";
		return false;
	}

	// The loose files first, so Default is bank 0 whenever it exists. That
	// ordering is what keeps a library written before banks behaving exactly
	// as it did: the presets it always had, in the order it always had them,
	// in the bank it starts on.
	{
		Bank bank;
		bank.name = kDefaultBankName;
		bank.directory = dir.string();

		if (!LoadBank(dir, bank.entries, error, warnings))
		{
			return false;
		}

		if (!bank.entries.empty())
		{
			out.banks_.push_back(std::move(bank));
		}
	}

	std::vector<std::filesystem::path> subdirectories;

	for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
	{
		if (entry.is_directory())
		{
			subdirectories.push_back(entry.path());
		}
	}

	std::sort(subdirectories.begin(), subdirectories.end());

	for (const std::filesystem::path& subdirectory : subdirectories)
	{
		Bank bank;
		bank.name = subdirectory.filename().string();
		bank.directory = subdirectory.string();

		std::string bank_error;

		if (!LoadBank(subdirectory, bank.entries, bank_error, warnings))
		{
			// A broken manifest in one bank costs that bank, not the library.
			// The player loses a folder of sounds and is told which; refusing
			// to start at all would lose them the gig.
			warnings.push_back("skipping bank '" + bank.name + "': " + bank_error);
			continue;
		}

		// An empty folder is not a bank. Somewhere to stomp into and find
		// nothing is worse than not being able to stomp there.
		if (!bank.entries.empty())
		{
			out.banks_.push_back(std::move(bank));
		}
	}

	return true;
}

const std::string& PresetLibrary::CurrentBankName() const
{
	static const std::string kNone;

	return banks_.empty() ? kNone : banks_[current_bank_].name;
}

bool PresetLibrary::SelectBank(size_t index)
{
	if (index >= banks_.size())
	{
		return false;
	}

	current_bank_ = index;

	return true;
}

size_t PresetLibrary::Count() const
{
	return banks_.empty() ? 0 : banks_[current_bank_].entries.size();
}

const PresetLibrary::Entry& PresetLibrary::At(size_t index) const
{
	return banks_[current_bank_].entries[index];
}

bool PresetLibrary::Resolve(const std::string& query, Location& out) const
{
	if (query.empty() || banks_.empty())
	{
		return false;
	}

	// "bank/preset": the unambiguous form, and the only one that can reach a
	// preset whose name is shared with one in the bank you are standing in.
	const size_t slash = query.find('/');

	if (slash != std::string::npos)
	{
		const std::string bank_name = query.substr(0, slash);
		const std::string rest = query.substr(slash + 1);

		for (size_t b = 0; b < banks_.size(); ++b)
		{
			if (banks_[b].name != bank_name)
			{
				continue;
			}

			for (size_t i = 0; i < banks_[b].entries.size(); ++i)
			{
				const Entry& entry = banks_[b].entries[i];

				if (entry.preset.name == rest || entry.stem == rest)
				{
					out = {b, i};
					return true;
				}
			}

			return false;
		}

		return false;
	}

	// A bare index means a position in the bank you are on. It cannot mean
	// anything else -- an index that wandered between banks as the selection
	// changed would be a footgun in a console command.
	if (query.find_first_not_of("0123456789") == std::string::npos)
	{
		const unsigned long long index = std::stoull(query);

		if (index < Count())
		{
			out = {current_bank_, static_cast<size_t>(index)};
			return true;
		}

		return false;
	}

	// By name, then by stem -- the current bank first in both cases, so an
	// unqualified name resolves to the one nearest to hand rather than to
	// whichever bank happens to sort first.
	for (int pass = 0; pass < 2; ++pass)
	{
		for (size_t step = 0; step < banks_.size(); ++step)
		{
			const size_t b = (current_bank_ + step) % banks_.size();

			for (size_t i = 0; i < banks_[b].entries.size(); ++i)
			{
				const Entry& entry = banks_[b].entries[i];
				const std::string& against = pass == 0 ? entry.preset.name : entry.stem;

				if (against == query)
				{
					out = {b, i};
					return true;
				}
			}
		}
	}

	return false;
}

bool PresetLibrary::FindByPath(const std::string& path, Location& out) const
{
	for (size_t b = 0; b < banks_.size(); ++b)
	{
		for (size_t i = 0; i < banks_[b].entries.size(); ++i)
		{
			if (banks_[b].entries[i].path == path)
			{
				out = {b, i};
				return true;
			}
		}
	}

	return false;
}

std::string PresetLibrary::QualifiedName(const Location& at) const
{
	if (at.bank >= banks_.size() || at.index >= banks_[at.bank].entries.size())
	{
		return {};
	}

	const std::string& stem = banks_[at.bank].entries[at.index].stem;

	// The Default bank's presets stay unqualified, so a settings file written
	// before banks existed still names the same thing and a library that never
	// grows a second bank never grows a prefix either.
	return at.bank == 0 && banks_[at.bank].name == kDefaultBankName ? stem : banks_[at.bank].name + "/" + stem;
}

}  // namespace sf
