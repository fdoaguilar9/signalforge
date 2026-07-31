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

		Entry entry;
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

		out.entries_.push_back(std::move(entry));
	}

	return true;
}

bool PresetLibrary::Resolve(const std::string& query, size_t& index_out) const
{
	if (query.empty())
	{
		return false;
	}

	// Numeric index.
	if (query.find_first_not_of("0123456789") == std::string::npos)
	{
		const unsigned long long index = std::stoull(query);

		if (index < entries_.size())
		{
			index_out = static_cast<size_t>(index);
			return true;
		}

		return false;
	}

	// Preset "name" field.
	for (size_t i = 0; i < entries_.size(); ++i)
	{
		if (entries_[i].preset.name == query)
		{
			index_out = i;
			return true;
		}
	}

	// Filename stem.
	for (size_t i = 0; i < entries_.size(); ++i)
	{
		if (entries_[i].stem == query)
		{
			index_out = i;
			return true;
		}
	}

	return false;
}

}  // namespace sf
