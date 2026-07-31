// Settings tests (SF-32/SF-33): what the program remembers between runs.
// Pure file and JSON handling, so this runs anywhere -- no window, no audio.
//
// The thing being defended here is that a settings file can never be the
// reason a board won't play. Every read is optional, every value is clamped,
// and a file that is missing, truncated, or hand-edited into nonsense costs at
// most the value it was carrying.

#include "../src/app/Settings.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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

std::filesystem::path TempDirectory()
{
	const std::filesystem::path directory =
		std::filesystem::temp_directory_path() / "signalforge-settings-test";

	std::filesystem::remove_all(directory);
	std::filesystem::create_directories(directory);

	return directory;
}

sf::Settings LoadText(const std::filesystem::path& path, const std::string& text, bool& ok)
{
	{
		std::ofstream file(path);
		file << text;
	}

	sf::Settings settings;
	std::string error;
	ok = settings.Load(path.string(), error);

	if (!ok)
	{
		std::printf("      (load said: %s)\n", error.c_str());
	}

	return settings;
}

void TestRoundTrip(const std::filesystem::path& directory)
{
	std::printf("\n-- round trip --\n");

	sf::Settings written;
	written.window_width = 1024;
	written.window_height = 768;
	written.last_preset = "03-lead";
	written.crossfade_ms = 20.0;
	written.crossfade_curve = sf::CrossfadeCurve::Linear;

	// Deliberately a path two directories deep that doesn't exist: the usual
	// case is a first run on a machine with no ~/.config/signalforge yet.
	const std::filesystem::path path = directory / "nested" / "deeper" / "settings.json";

	std::string error;
	Check(written.Save(path.string(), error), "saving creates the directories it needs");

	sf::Settings read;
	Check(read.Load(path.string(), error), "and the file loads back");

	Check(read.window_width == 1024 && read.window_height == 768, "the window size survives");
	Check(read.last_preset == "03-lead", "the last preset survives");
	Check(read.crossfade_ms == 20.0, "the crossfade length survives");
	Check(read.crossfade_curve == sf::CrossfadeCurve::Linear, "the crossfade curve survives");
}

void TestMissingAndBroken(const std::filesystem::path& directory)
{
	std::printf("\n-- a file that isn't there, or isn't right --\n");

	const sf::Settings defaults;

	sf::Settings first_run;
	std::string error;

	Check(first_run.Load((directory / "does-not-exist.json").string(), error),
		"a missing file is a first run, not a failure");
	Check(first_run.window_width == defaults.window_width && first_run.last_preset.empty(),
		"and leaves the defaults alone");

	Check(first_run.Load("", error), "no path at all is also fine -- a container with no HOME");
	Check(first_run.Save("", error), "and saving to nowhere quietly does nothing");

	bool ok = false;
	const std::filesystem::path path = directory / "settings.json";

	// Truncated mid-write: what a power cut during Save leaves behind.
	LoadText(path, "{ \"window\": { \"width\": 900,", ok);
	Check(!ok, "a truncated file is reported rather than silently ignored");

	// Every value the wrong type. Each one falls back on its own; none of them
	// takes the others down with it.
	const sf::Settings wrong = LoadText(path,
		"{ \"window\": \"big\", \"last_preset\": 7, \"crossfade\": { \"milliseconds\": \"soon\", \"curve\": 3 } }", ok);

	Check(ok, "a file full of wrong types still loads");
	Check(wrong.window_width == defaults.window_width, "a window that isn't an object keeps the default size");
	Check(wrong.last_preset.empty(), "a last_preset that isn't a string is ignored");
	Check(wrong.crossfade_ms == defaults.crossfade_ms, "a length that isn't a number keeps the default");

	// A partial file is the common hand-edited case: set one thing, leave the
	// rest out.
	const sf::Settings partial = LoadText(path, "{ \"crossfade\": { \"milliseconds\": 8 } }", ok);

	Check(ok && partial.crossfade_ms == 8.0, "a file that sets one value sets exactly that one");
	Check(partial.window_width == defaults.window_width, "and leaves the rest at their defaults");

	// Comments, like presets and mappings.
	const sf::Settings commented = LoadText(path, "{\n  // how long a switch takes\n  \"crossfade\": { \"milliseconds\": 5 }\n}", ok);
	Check(ok && commented.crossfade_ms == 5.0, "comments are allowed, as everywhere else in this program");

	// An unknown curve name keeps the default curve rather than refusing the
	// file: a typo shouldn't cost you your window size as well.
	const sf::Settings typo = LoadText(path, "{ \"window\": { \"width\": 900 }, \"crossfade\": { \"curve\": \"smoooth\" } }", ok);

	Check(ok && typo.crossfade_curve == defaults.crossfade_curve, "an unknown curve name keeps the default curve");
	Check(typo.window_width == 900, "and the rest of the file is still honoured");
}

void TestClamping(const std::filesystem::path& directory)
{
	std::printf("\n-- limits --\n");

	bool ok = false;
	const std::filesystem::path path = directory / "settings.json";

	// A window remembered from a monitor that has since been unplugged, and
	// one small enough to be invisible.
	const sf::Settings huge = LoadText(path, "{ \"window\": { \"width\": 99999, \"height\": 1 } }", ok);

	Check(ok, "an out-of-range window size loads");
	Check(huge.window_width < 99999 && huge.window_height > 1, "and is clamped to something you could actually use");

	const sf::Settings negative = LoadText(path, "{ \"crossfade\": { \"milliseconds\": -12 } }", ok);
	Check(ok && negative.crossfade_ms == 0.0, "a negative crossfade becomes an instant one");

	const sf::Settings enormous = LoadText(path, "{ \"crossfade\": { \"milliseconds\": 100000 } }", ok);
	Check(ok && enormous.crossfade_ms == sf::Crossfade::kMaxMilliseconds,
		"a crossfade longer than a song is capped");

	// Clamp() is what the command line goes through too, so a flag can't do
	// what a file can't.
	sf::Settings typed;
	typed.window_height = -400;
	typed.crossfade_ms = 1e9;
	typed.Clamp();

	Check(typed.window_height > 0 && typed.crossfade_ms == sf::Crossfade::kMaxMilliseconds,
		"the same limits apply to values set in code");
}

}  // namespace

int main()
{
	const std::filesystem::path directory = TempDirectory();

	TestRoundTrip(directory);
	TestMissingAndBroken(directory);
	TestClamping(directory);

	std::filesystem::remove_all(directory);

	std::printf("\n%s\n", g_failures == 0 ? "All settings tests passed." : "Settings tests FAILED.");

	return g_failures == 0 ? 0 : 1;
}
