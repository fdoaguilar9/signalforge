#include "Settings.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace sf
{

namespace
{

// A window smaller than this can't show the board, and one larger than any
// real display is almost always a remembered value from a monitor that has
// since been unplugged.
constexpr int kMinWindowSize = 240;
constexpr int kMaxWindowSize = 16384;

}  // namespace

std::string Settings::DefaultPath()
{
	namespace fs = std::filesystem;

	if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
	{
		return (fs::path(xdg) / "signalforge" / "settings.json").string();
	}

	if (const char* home = std::getenv("HOME"); home && *home)
	{
		return (fs::path(home) / ".config" / "signalforge" / "settings.json").string();
	}

	return {};
}

void Settings::Clamp()
{
	window_width = std::min(kMaxWindowSize, std::max(kMinWindowSize, window_width));
	window_height = std::min(kMaxWindowSize, std::max(kMinWindowSize, window_height));

	crossfade_ms = Crossfade::ClampMilliseconds(crossfade_ms);

	audio.Clamp();
}

nlohmann::json Settings::ToJson() const
{
	return nlohmann::json{
		{"window", {{"width", window_width}, {"height", window_height}}},
		{"last_preset", last_preset},
		{"crossfade", {{"milliseconds", crossfade_ms}, {"curve", ToString(crossfade_curve)}}},
		{"audio", audio.ToJson()},
	};
}

Settings Settings::FromJson(const nlohmann::json& json)
{
	Settings out;

	if (!json.is_object())
	{
		return out;
	}

	// Every read is "if it's there and it's the right type". A settings file is
	// hand-editable and half-written files happen; the failure mode has to be
	// "that one value went back to its default", never "the board won't start".
	if (json.contains("window") && json["window"].is_object())
	{
		const nlohmann::json& window = json["window"];

		if (window.contains("width") && window["width"].is_number_integer())
		{
			out.window_width = window["width"].get<int>();
		}

		if (window.contains("height") && window["height"].is_number_integer())
		{
			out.window_height = window["height"].get<int>();
		}
	}

	if (json.contains("last_preset") && json["last_preset"].is_string())
	{
		out.last_preset = json["last_preset"].get<std::string>();
	}

	if (json.contains("crossfade") && json["crossfade"].is_object())
	{
		const nlohmann::json& crossfade = json["crossfade"];

		if (crossfade.contains("milliseconds") && crossfade["milliseconds"].is_number())
		{
			out.crossfade_ms = crossfade["milliseconds"].get<double>();
		}

		if (crossfade.contains("curve") && crossfade["curve"].is_string())
		{
			// An unknown curve name keeps the default rather than refusing the
			// file: a typo shouldn't cost you your window size as well.
			ParseCrossfadeCurve(crossfade["curve"].get<std::string>(), out.crossfade_curve);
		}
	}

	if (json.contains("audio") && json["audio"].is_object())
	{
		out.audio = AudioConfig::FromJson(json["audio"]);
	}

	out.Clamp();

	return out;
}

bool Settings::Load(const std::string& path, std::string& error)
{
	if (path.empty())
	{
		return true;
	}

	std::error_code ec;

	if (!std::filesystem::exists(path, ec))
	{
		// The first run. Not a failure, and not worth a warning either.
		return true;
	}

	std::ifstream file(path);

	if (!file)
	{
		error = "cannot open '" + path + "'";
		return false;
	}

	nlohmann::json json;

	try
	{
		// Same parse flags as presets and mappings: throw on error, allow
		// comments, so a hand-annotated settings file works.
		json = nlohmann::json::parse(file, nullptr, true, true);
	}
	catch (const nlohmann::json::parse_error& e)
	{
		error = "invalid JSON in '" + path + "': " + e.what();
		return false;
	}

	*this = FromJson(json);

	return true;
}

bool Settings::Save(const std::string& path, std::string& error) const
{
	if (path.empty())
	{
		// Nowhere to write, which is a supported way to run rather than a
		// failure. See DefaultPath.
		return true;
	}

	std::error_code ec;
	const std::filesystem::path directory = std::filesystem::path(path).parent_path();

	if (!directory.empty())
	{
		std::filesystem::create_directories(directory, ec);

		if (ec)
		{
			error = "cannot create '" + directory.string() + "': " + ec.message();
			return false;
		}
	}

	std::ofstream file(path);

	if (!file)
	{
		error = "cannot write '" + path + "'";
		return false;
	}

	file << ToJson().dump(2) << "\n";

	if (!file)
	{
		error = "error while writing '" + path + "'";
		return false;
	}

	return true;
}

}  // namespace sf
