#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <jack/jack.h>
#include <lilv/lilv.h>

#include <nlohmann/json.hpp>

#include "app/Settings.h"
#include "audio/Crossfade.h"
#include "jack/JackEngine.h"
#include "lv2/Chain.h"
#include "midi/MidiEvent.h"
#include "midi/MidiLearn.h"
#include "midi/MidiMap.h"
#include "offline/OfflineEngine.h"
#include "preset/ChainPreset.h"
#include "preset/Preset.h"
#include "preset/PresetLibrary.h"
#include "lv2/PluginStage.h"
#include "lv2/PathProperty.h"
#include "lv2/PortInfo.h"
#include "lv2/UridMap.h"

#ifdef SIGNALFORGE_GUI
#include "ui/AppWindow.h"
#include "ui/BoardView.h"
#include "ui/UiSelfTest.h"
#endif

namespace
{

void PrintUsage(const char* argv0)
{
	std::printf(
		"Usage: %s [--list-plugins | --inspect <plugin-uri> | --preset-roundtrip <preset.json> |\n"
		"           --list-presets [--preset-dir <dir>] |\n"
		"           --run-preset [name|index|path] [--preset-dir <dir>] [--no-crossfade]\n"
		"                        [--midi-device <name>] [--no-midi-connect] [--midi-map <file>] [--gui]\n"
		"                        [--settings <file>] [--crossfade-ms <N>] [--crossfade-curve equal-power|linear] |\n"
		"           --tune-crossfade [--sample-rate N] |\n"
		"           --check-midi-map <mapping.json> | --test-ui [frames] |\n"
		"           --test-add-stage <preset> <plugin-uri> [--preset-dir <dir>] |\n"
		"           --test-path-property <preset> <file> [--preset-dir <dir>] |\n"
		"           --run <plugin-uri> [--control name=value ...] [--set-path property-uri=file-path ...] |\n"
		"           --run-chain <plugin-uri> [...] [--set-path stage-index:property-uri=file-path ...] |\n"
		"           --test-chain {<plugin-uri> [...] | --preset <name|index|path> [--preset-dir <dir>]}\n"
		"                        [--set-path stage:property-uri=file-path ...] [--control stage:symbol=value ...]\n"
		"                        [--seconds N] [--block-size N] [--expect-signal] [--bypass <stage> ...] |\n"
		"           --save-preset <out.json> <plugin-uri> [...] [--set-path stage:property-uri=file-path ...]\n"
		"                         [--control stage:symbol=value ...] [--name \"...\"]]\n",
		argv0);
}

// Splits "key=value" on the first '='. Returns false if there's no '=' or
// an empty key.
bool SplitKeyValue(const char* arg, std::string& key, std::string& value)
{
	const char* equals = std::strchr(arg, '=');
	if (!equals || equals == arg)
	{
		return false;
	}

	key.assign(arg, equals - arg);
	value.assign(equals + 1);

	return true;
}

// Parses "name=value" into (name, value). Returns false on malformed input.
bool ParseControlArg(const char* arg, std::string& name, float& value)
{
	std::string value_string;
	if (!SplitKeyValue(arg, name, value_string))
	{
		return false;
	}

	char* end = nullptr;
	value = std::strtof(value_string.c_str(), &end);

	return end != value_string.c_str() && *end == '\0';
}

int ListPlugins()
{
	LilvWorld* world = lilv_world_new();
	lilv_world_load_all(world);

	const LilvPlugins* plugins = lilv_world_get_all_plugins(world);

	unsigned count = 0;

	LILV_FOREACH(plugins, it, plugins)
	{
		const LilvPlugin* plugin = lilv_plugins_get(plugins, it);

		const LilvNode* uri = lilv_plugin_get_uri(plugin);
		LilvNode* name = lilv_plugin_get_name(plugin);

		std::printf("%-70s %s\n", lilv_node_as_uri(uri), name ? lilv_node_as_string(name) : "(unnamed)");

		lilv_node_free(name);

		++count;
	}

	std::printf("\n%u plugin(s) found.\n", count);

	lilv_world_free(world);

	return 0;
}

int Inspect(const char* uri_string)
{
	LilvWorld* world = lilv_world_new();
	lilv_world_load_all(world);

	LilvNode* uri = lilv_new_uri(world, uri_string);
	if (!uri)
	{
		std::fprintf(stderr, "Invalid URI: %s\n", uri_string);
		lilv_world_free(world);

		return 1;
	}

	const LilvPlugins* plugins = lilv_world_get_all_plugins(world);
	const LilvPlugin* plugin = lilv_plugins_get_by_uri(plugins, uri);

	lilv_node_free(uri);

	if (!plugin)
	{
		std::fprintf(stderr, "Plugin not found: %s\n", uri_string);
		lilv_world_free(world);

		return 1;
	}

	const std::vector<sf::PortInfo> ports = sf::ClassifyPorts(world, plugin);

	std::printf("%-4s %-14s %-8s %-20s %-24s %s\n", "Idx", "Type", "Dir", "Symbol", "Name", "Range");

	for (const sf::PortInfo& port : ports)
	{
		std::printf("%-4u %-14s %-8s %-20s %-24s", port.index, sf::ToString(port.type), sf::ToString(port.direction),
			port.symbol.c_str(), port.name.c_str());

		if (port.type == sf::PortType::Control)
		{
			std::printf("[%g .. %g], default %g", port.min_value, port.max_value, port.default_value);

			// The hints that decide how a control gets drawn, so --inspect can
			// be used to check what the UI will make of a plugin.
			if (port.toggled)
			{
				std::printf(" toggled");
			}

			if (port.integer)
			{
				std::printf(" integer");
			}

			if (port.enumeration)
			{
				std::printf(" enumeration");
			}

			if (port.logarithmic)
			{
				std::printf(" logarithmic");
			}
		}

		std::printf("\n");
	}

	// File-valued parameters aren't ports, so they don't appear above -- but
	// they are the reason a plugin needs a file picker.
	const std::vector<sf::PathProperty> path_properties = sf::FindPathProperties(world, plugin);

	if (!path_properties.empty())
	{
		std::printf("\nFile parameters:\n");

		for (const sf::PathProperty& property : path_properties)
		{
			std::printf("  %-24s %s", property.label.c_str(), property.uri.c_str());

			if (!property.file_types.empty())
			{
				std::printf("  (");

				for (size_t i = 0; i < property.file_types.size(); ++i)
				{
					std::printf("%s%s", i > 0 ? ", " : "", property.file_types[i].c_str());
				}

				std::printf(")");
			}

			std::printf("\n");
		}
	}

	lilv_world_free(world);

	return 0;
}

std::atomic<bool> g_keep_running{true};

void HandleSigint(int)
{
	g_keep_running = false;
}

int Run(const char* uri_string, const std::vector<std::pair<std::string, float>>& controls,
	const std::vector<std::pair<std::string, std::string>>& paths)
{
	LilvWorld* world = lilv_world_new();
	lilv_world_load_all(world);

	LilvNode* uri = lilv_new_uri(world, uri_string);
	const LilvPlugins* plugins = lilv_world_get_all_plugins(world);
	const LilvPlugin* plugin = uri ? lilv_plugins_get_by_uri(plugins, uri) : nullptr;

	if (uri)
	{
		lilv_node_free(uri);
	}

	if (!plugin)
	{
		std::fprintf(stderr, "Plugin not found: %s\n", uri_string);
		lilv_world_free(world);

		return 1;
	}

	sf::JackEngine engine;

	if (!engine.Open("signalforge"))
	{
		lilv_world_free(world);

		return 1;
	}

	sf::UridMap urid_map;

	std::unique_ptr<sf::PluginStage> stage =
		sf::PluginStage::Create(world, plugin, engine.SampleRate(), engine.MaxBlockSize(), urid_map);

	if (!stage)
	{
		engine.Close();
		lilv_world_free(world);

		return 1;
	}

	for (const auto& [name, value] : controls)
	{
		if (stage->SetControl(name, value))
		{
			std::printf("Set control '%s' -> %g (requested %g)\n", name.c_str(), stage->GetControl(name), value);
			std::fflush(stdout);
		}
	}

	for (const auto& [property_uri, file_path] : paths)
	{
		if (stage->LoadPathProperty(property_uri, file_path, urid_map))
		{
			std::printf("Set path '%s' -> %s\n", property_uri.c_str(), file_path.c_str());
			std::fflush(stdout);
		}
	}

	if (!engine.Start(stage.get()))
	{
		engine.Close();
		lilv_world_free(world);

		return 1;
	}

	std::printf("Running %s -- connect its JACK ports and press Ctrl+C to stop.\n", uri_string);

	std::signal(SIGINT, HandleSigint);

	while (g_keep_running)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	std::printf("Stopping.\n");

	engine.Close();
	stage.reset();
	lilv_world_free(world);

	return 0;
}

// A per-stage "--set-path" target for --run-chain: "stage_index:property-uri=file-path".
struct ChainPathArg
{
	size_t stage_index;
	std::string property_uri;
	std::string file_path;
};

bool ParseChainPathArg(const char* arg, ChainPathArg& out)
{
	const char* colon = std::strchr(arg, ':');
	if (!colon || colon == arg)
	{
		return false;
	}

	char* end = nullptr;
	const long index = std::strtol(arg, &end, 10);
	if (end != colon || index < 0)
	{
		return false;
	}

	out.stage_index = static_cast<size_t>(index);

	return SplitKeyValue(colon + 1, out.property_uri, out.file_path);
}

// A per-stage "--control" target: "stage_index:symbol=value".
struct ChainControlArg
{
	size_t stage_index;
	std::string symbol;
	float value;
};

bool ParseChainControlArg(const char* arg, ChainControlArg& out)
{
	const char* colon = std::strchr(arg, ':');
	if (!colon || colon == arg)
	{
		return false;
	}

	char* end = nullptr;
	const long index = std::strtol(arg, &end, 10);
	if (end != colon || index < 0)
	{
		return false;
	}

	out.stage_index = static_cast<size_t>(index);

	return ParseControlArg(colon + 1, out.symbol, out.value);
}

void ApplyChainControls(sf::Chain& chain, const std::vector<ChainControlArg>& controls)
{
	for (const ChainControlArg& control : controls)
	{
		if (control.stage_index >= chain.StageCount())
		{
			std::fprintf(stderr, "--control: stage index %zu out of range (chain has %zu stages).\n",
				control.stage_index, chain.StageCount());
			continue;
		}

		sf::PluginStage* stage = chain.Stage(control.stage_index);

		if (stage->SetControl(control.symbol, control.value))
		{
			std::printf("Stage %zu: set control '%s' -> %g\n", control.stage_index, control.symbol.c_str(),
				stage->GetControl(control.symbol));
		}
	}
}

void ApplyChainPaths(sf::Chain& chain, const std::vector<ChainPathArg>& paths, sf::UridMap& urid_map)
{
	for (const ChainPathArg& path : paths)
	{
		if (path.stage_index >= chain.StageCount())
		{
			std::fprintf(stderr, "--set-path: stage index %zu out of range (chain has %zu stages).\n", path.stage_index,
				chain.StageCount());
			continue;
		}

		if (chain.Stage(path.stage_index)->LoadPathProperty(path.property_uri, path.file_path, urid_map))
		{
			std::printf(
				"Stage %zu: set path '%s' -> %s\n", path.stage_index, path.property_uri.c_str(), path.file_path.c_str());
			std::fflush(stdout);
		}
	}
}

int RunChain(const std::vector<std::string>& uri_strings, const std::vector<ChainPathArg>& paths)
{
	LilvWorld* world = lilv_world_new();
	lilv_world_load_all(world);

	sf::JackEngine engine;

	if (!engine.Open("signalforge"))
	{
		lilv_world_free(world);
		return 1;
	}

	sf::UridMap urid_map;

	std::unique_ptr<sf::Chain> chain =
		sf::Chain::Create(world, uri_strings, engine.SampleRate(), engine.MaxBlockSize(), urid_map);

	if (!chain)
	{
		engine.Close();
		lilv_world_free(world);
		return 1;
	}

	ApplyChainPaths(*chain, paths, urid_map);

	if (!engine.Start(chain.get()))
	{
		engine.Close();
		lilv_world_free(world);
		return 1;
	}

	std::printf("Running a %zu-stage chain -- connect its JACK ports and press Ctrl+C to stop.\n", uri_strings.size());
	std::fflush(stdout);

	std::signal(SIGINT, HandleSigint);

	while (g_keep_running)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	std::printf("Stopping.\n");

	engine.Close();
	chain.reset();
	lilv_world_free(world);

	return 0;
}

// Resolves what the user passed to --preset: an existing file path is used
// directly, otherwise it's looked up in the preset directory by index, name
// or filename stem.
bool ResolvePreset(const std::string& query, const std::string& preset_dir, sf::Preset& out, std::string& resolved_path)
{
	std::string error;

	if (std::filesystem::exists(query))
	{
		if (!sf::LoadPresetFile(query, out, error))
		{
			std::fprintf(stderr, "%s\n", error.c_str());
			return false;
		}

		resolved_path = query;

		return true;
	}

	sf::PresetLibrary library;
	std::vector<std::string> warnings;

	if (!sf::PresetLibrary::Open(preset_dir, library, error, warnings))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return false;
	}

	for (const std::string& warning : warnings)
	{
		std::fprintf(stderr, "Warning: %s\n", warning.c_str());
	}

	size_t index = 0;

	if (!library.Resolve(query, index))
	{
		std::fprintf(stderr, "No preset matching '%s' in '%s' (%zu preset(s) available).\n", query.c_str(),
			preset_dir.c_str(), library.Count());
		return false;
	}

	out = library.At(index).preset;
	resolved_path = library.At(index).path;

	return true;
}

int ListPresets(const std::string& preset_dir)
{
	sf::PresetLibrary library;
	std::string error;
	std::vector<std::string> warnings;

	if (!sf::PresetLibrary::Open(preset_dir, library, error, warnings))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	for (const std::string& warning : warnings)
	{
		std::fprintf(stderr, "Warning: %s\n", warning.c_str());
	}

	std::printf("%zu preset(s) in '%s':\n", library.Count(), preset_dir.c_str());

	for (size_t i = 0; i < library.Count(); ++i)
	{
		const sf::PresetLibrary::Entry& entry = library.At(i);
		std::printf("  [%zu] %-24s %-24s %zu stage(s)\n", i, entry.stem.c_str(), entry.preset.name.c_str(),
			entry.preset.chain.size());
	}

	return 0;
}

// Describes one binding the way a pedal manual would, so a user can check a
// hand-written mapping file against what their controller actually sends.
// The message side of a binding, in words: "PC 3" rather than the action.
// This is what the UI shows next to a control -- what to press, not what it
// does, because what it does is the row it's sitting on.
std::string DescribeMatcher(const sf::MidiBinding& binding)
{
	std::string text;

	switch (binding.type)
	{
		case sf::MidiEventType::ProgramChange: text = "PC "; break;
		case sf::MidiEventType::ControlChange: text = "CC "; break;
		case sf::MidiEventType::NoteOn: text = "Note "; break;
		default: text = "? "; break;
	}

	text += std::to_string(binding.number);

	if (binding.channel != sf::kAnyChannel)
	{
		text += " ch" + std::to_string(binding.channel);
	}

	return text;
}

std::string DescribeBinding(const sf::MidiBinding& binding)
{
	std::string text = sf::ToString(binding.action);

	switch (binding.action)
	{
		case sf::MidiAction::LoadPreset:
			text += " '" + binding.preset + "'";
			break;

		case sf::MidiAction::ToggleBypass:
			text += " on stage " + std::to_string(binding.stage);
			break;

		case sf::MidiAction::SetControl:
		{
			// %g rather than std::to_string, which pads every range out to six
			// decimal places and makes the listing unreadable.
			char range[64];
			std::snprintf(range, sizeof(range), "%g..%g", binding.minimum, binding.maximum);

			text += " stage " + std::to_string(binding.stage) + " '" + binding.port + "' over " + range;
			break;
		}

		case sf::MidiAction::NextPreset:
		case sf::MidiAction::PrevPreset:
			break;
	}

	return text;
}

// Validates a mapping file and prints what it means. Loading is strict (see
// MidiMap), so this is how a user finds out *why* their file was rejected
// without having to start the audio engine.
// What a degraded board has to say for itself: which pedals aren't there, and
// the two consequences that aren't obvious from the plugin names alone --
// saving would write the file without them, and a MIDI binding that names a
// stage by number now counts a shorter chain.
std::string DescribeMissingPlugins(const std::vector<std::string>& missing)
{
	if (missing.empty())
	{
		return {};
	}

	std::string text = std::to_string(missing.size()) + " stage(s) skipped -- plugin not installed: ";

	for (size_t i = 0; i < missing.size(); ++i)
	{
		text += (i == 0 ? "" : ", ") + missing[i];
	}

	text += ". The board is playing without them; saving is blocked so the preset keeps them, and any "
			"stage-numbered MIDI binding now counts a shorter chain.";

	return text;
}

int CheckMidiMap(const char* path)
{
	sf::MidiMap map;
	std::string error;

	if (!map.Load(path, error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	std::printf("%zu binding(s) in '%s':\n", map.Size(), path);

	for (const sf::MidiBinding& binding : map.Bindings())
	{
		char matcher[64];

		if (binding.channel == sf::kAnyChannel)
		{
			std::snprintf(matcher, sizeof(matcher), "%s %d (any channel)", sf::ToString(binding.type),
				binding.number);
		}
		else
		{
			std::snprintf(matcher, sizeof(matcher), "%s %d ch %d", sf::ToString(binding.type), binding.number,
				binding.channel);
		}

		std::printf("  %-30s -> %s\n", matcher, DescribeBinding(binding).c_str());
	}

	return 0;
}

// Live playback from a preset: the actual pedalboard path.
// Snapshots the running chain and appends one plugin to the end of it.
//
// Shared by the UI's "Add stage" and by --test-add-stage so the test drives
// the real thing rather than a copy of it. The snapshot is of what's running,
// not of the file on disk, so control values a player has already moved
// survive gaining a stage.
#ifdef SIGNALFORGE_GUI

// Which extensions the open picker should show, from the property it was
// opened for. Empty means "everything", which is what a plugin that declares
// no file types deserves.
const std::vector<std::string>* PathFilterFor(const std::vector<sf::BoardStage>& stages, const sf::BoardViewUi& ui)
{
	if (ui.picker_stage >= stages.size())
	{
		return nullptr;
	}

	for (const sf::BoardPathProperty& path : stages[ui.picker_stage].paths)
	{
		if (path.uri == ui.picker_property)
		{
			return &path.file_types;
		}
	}

	return nullptr;
}

// Directories first, then matching files, each alphabetical. Unreadable
// directories come back empty rather than throwing: a picker that crashes on
// /root is worse than one that shows nothing there.
std::vector<sf::BoardFileEntry> ListDirectory(const std::string& directory, const std::vector<std::string>* extensions)
{
	std::vector<sf::BoardFileEntry> directories;
	std::vector<sf::BoardFileEntry> files;

	std::error_code ec;

	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, ec))
	{
		const std::string name = entry.path().filename().string();

		// Dotfiles are noise in a picker for guitar models.
		if (name.empty() || name[0] == '.')
		{
			continue;
		}

		std::error_code kind_ec;

		if (entry.is_directory(kind_ec))
		{
			directories.push_back({name, true});
			continue;
		}

		if (extensions && !extensions->empty())
		{
			sf::PathProperty filter;
			filter.file_types = *extensions;

			if (!sf::MatchesFileTypes(filter, name))
			{
				continue;
			}
		}

		files.push_back({name, false});
	}

	auto by_name = [](const sf::BoardFileEntry& a, const sf::BoardFileEntry& b) { return a.name < b.name; };

	std::sort(directories.begin(), directories.end(), by_name);
	std::sort(files.begin(), files.end(), by_name);

	directories.insert(directories.end(), files.begin(), files.end());

	return directories;
}

#endif

// "/a/b/Capture 16.nam" -> "Capture 16.nam", for report lines that would
// otherwise be mostly directory.
std::string FileName(const std::string& path)
{
	const size_t cut = path.find_last_of('/');

	return cut == std::string::npos ? path : path.substr(cut + 1);
}

sf::Preset PresetWithAddedStage(
	const sf::Chain& chain, sf::UridMap& urid_map, const std::string& name, const std::string& plugin_uri)
{
	sf::Preset edited = sf::CaptureChainPreset(chain, urid_map, name);

	// No controls and no state: a stage added by hand should come up as the
	// plugin's author intended, not inherit anything from its neighbours.
	sf::PresetStage added;
	added.uri = plugin_uri;
	edited.chain.push_back(added);

	return edited;
}

struct RunPresetOptions
{
	bool use_crossfade = true;

	// Empty means "auto": connect every hardware controller found. A name is
	// a case-insensitive substring match on the JACK port.
	std::string midi_device;

	bool connect_midi = true;

	// Optional mapping table: what incoming MIDI messages do.
	std::string midi_map_path;

	// Open the desktop window. Off by default: the board is expected to run
	// on machines with no screen attached.
	bool show_gui = false;

	// Where the remembered window size and last preset live. Empty means the
	// usual place; see Settings::DefaultPath.
	std::string settings_path;

	// Command-line overrides for what the settings file otherwise decides.
	// Unset means "whatever the file says", which is what makes a flag a way
	// to try a value rather than a way to lose one.
	bool crossfade_ms_set = false;
	double crossfade_ms = 0.0;

	bool crossfade_curve_set = false;
	sf::CrossfadeCurve crossfade_curve = sf::CrossfadeCurve::EqualPower;
};

int RunPreset(const std::string& query, const std::string& preset_dir, const RunPresetOptions& options)
{
	const bool use_crossfade = options.use_crossfade;

	// Read before anything is opened: it decides how big the window is and,
	// when no preset was named, which one plays.
	const std::string settings_path =
		options.settings_path.empty() ? sf::Settings::DefaultPath() : options.settings_path;

	sf::Settings settings;

	{
		std::string settings_error;

		if (!settings.Load(settings_path, settings_error))
		{
			// A settings file is a convenience; it never keeps the board from
			// playing. The defaults are perfectly good.
			std::fprintf(stderr, "Warning: %s -- using defaults.\n", settings_error.c_str());
		}
	}

	// What the file said, kept so that a value tried on the command line is
	// used but not remembered. A flag that quietly became permanent would mean
	// there was no way to try a crossfade length without adopting it.
	const double stored_crossfade_ms = settings.crossfade_ms;
	const sf::CrossfadeCurve stored_crossfade_curve = settings.crossfade_curve;

	if (options.crossfade_ms_set)
	{
		settings.crossfade_ms = options.crossfade_ms;
	}

	if (options.crossfade_curve_set)
	{
		settings.crossfade_curve = options.crossfade_curve;
	}

	settings.Clamp();

	sf::Preset preset;
	std::string resolved_path;

	// No preset named: pick up where the last run left off. A board that was
	// switched off after the last song should come back playing that song's
	// sound, not whichever preset sorts first.
	std::string effective_query = query;

	if (effective_query.empty())
	{
		if (!settings.last_preset.empty())
		{
			std::printf("Resuming '%s' (last played).\n", settings.last_preset.c_str());
			effective_query = settings.last_preset;
		}
		else
		{
			effective_query = "0";
		}
	}

	if (!ResolvePreset(effective_query, preset_dir, preset, resolved_path))
	{
		// A remembered preset that has since been renamed or deleted is a
		// normal thing to happen, and is not a reason to refuse to start.
		if (effective_query == query || effective_query == "0")
		{
			return 1;
		}

		std::fprintf(stderr, "Warning: the last preset played ('%s') is gone; starting at the first one.\n",
			settings.last_preset.c_str());

		if (!ResolvePreset("0", preset_dir, preset, resolved_path))
		{
			return 1;
		}
	}

	// Said before anything is opened, so a run that fails at the audio device
	// still shows which preset it had chosen -- which is the whole question
	// when the choice was made by the settings file rather than by the user.
	std::printf("Loading preset '%s' from %s.\n", preset.name.c_str(), resolved_path.c_str());

	// Also before the engine: this is configuration, and what a run was
	// configured with is worth knowing whether or not it got as far as making
	// a sound.
	std::printf("Crossfade: %.1f ms, %s.\n", settings.crossfade_ms, sf::ToString(settings.crossfade_curve));

	// Load the mapping before opening any audio device: a typo in the file
	// should fail immediately, not after the engine is up.
	sf::MidiMap midi_map;

	// Without --midi-map, bindings live next to the presets they refer to.
	// Defaulted rather than left empty so that learning something -- from the
	// window or the console -- always has somewhere to go: needing a flag
	// before a footswitch can be bound is a CLI step this program shouldn't
	// require.
	const std::string midi_map_path = options.midi_map_path.empty()
		? (std::filesystem::path(preset_dir) / "mapping.json").string()
		: options.midi_map_path;

	if (!options.midi_map_path.empty() || std::filesystem::exists(midi_map_path))
	{
		std::string map_error;

		if (!midi_map.Load(midi_map_path, map_error))
		{
			std::fprintf(stderr, "%s\n", map_error.c_str());

			// An explicitly named file that won't parse is fatal; a defaulted
			// one that happens to be broken shouldn't stop the board playing.
			if (!options.midi_map_path.empty())
			{
				return 1;
			}
		}
		else
		{
			std::printf("Loaded %zu MIDI binding(s) from %s.\n", midi_map.Size(), midi_map_path.c_str());
		}
	}

	LilvWorld* world = lilv_world_new();
	lilv_world_load_all(world);

	sf::JackEngine engine;

	if (!engine.Open("signalforge"))
	{
		lilv_world_free(world);
		return 1;
	}

	// Before Start(), which is where the length becomes a per-sample step.
	engine.SetCrossfade(settings.crossfade_ms, settings.crossfade_curve);

	sf::UridMap urid_map;
	std::string error;

	// Plugins the *current preset file* names but that aren't installed, so
	// their stages aren't in the running chain (SF-31). Sticky for as long as
	// that file is loaded: an edit made afterwards doesn't put them back, and
	// forgetting them is how they get saved away.
	std::vector<std::string> missing_plugins;

	sf::ChainBuildReport build_report;

	std::unique_ptr<sf::Chain> chain = sf::BuildChainFromPreset(
		world, preset, engine.SampleRate(), engine.MaxBlockSize(), urid_map, error, &build_report);

	missing_plugins = build_report.skipped_uris;

	if (!chain)
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		engine.Close();
		lilv_world_free(world);
		return 1;
	}

	if (!engine.Start(chain.get()))
	{
		engine.Close();
		lilv_world_free(world);
		return 1;
	}

	if (options.connect_midi)
	{
		engine.ConnectMidiInputs(options.midi_device);
	}

	std::printf("Running preset '%s' from %s (%zu stage(s)).\n", preset.name.c_str(), resolved_path.c_str(),
		chain->StageCount());

	if (!missing_plugins.empty())
	{
		// On stdout as well as the per-stage warnings on stderr: this is the
		// line that says what it means for the board, and a headless run has
		// no window to put it in.
		std::printf("Warning: %s\n", DescribeMissingPlugins(missing_plugins).c_str());
	}

	std::printf("Commands: a preset name/index, \"next\", \"prev\", \"bypass <stage>\", \"status\",\n");
	std::printf("          \"learn <action> [args]\", \"learn cancel\", \"quit\".\n");
	std::fflush(stdout);

	// The library is opened once, not per stomp: next_preset/prev_preset need
	// a defined order to step through, and a directory scan has no business
	// sitting between a footswitch and the sound.
	sf::PresetLibrary library;

	{
		std::string library_error;
		std::vector<std::string> library_warnings;

		if (!sf::PresetLibrary::Open(preset_dir, library, library_error, library_warnings))
		{
			std::fprintf(stderr, "Warning: %s -- next/prev preset will be unavailable.\n", library_error.c_str());
		}

		for (const std::string& warning : library_warnings)
		{
			std::fprintf(stderr, "Warning: %s\n", warning.c_str());
		}
	}

	// Where we are in the library. Count() means "the running preset didn't
	// come from the library" (it was given as a path), in which case stepping
	// starts from one end rather than pretending to know a position.
	size_t current_index = library.Count();

	// What's loaded right now, for anything that has to display it. Kept
	// alongside current_index because a preset given as a path has no index.
	std::string current_name = preset.name;
	std::string current_path = resolved_path;

	// The preset as it came off disk. Kept because a chain snapshot only knows
	// about stages: saving from one alone would quietly drop everything else
	// the file carries, starting with its MIDI trigger.
	sf::Preset current_source = preset;

	for (size_t i = 0; i < library.Count(); ++i)
	{
		if (library.At(i).path == resolved_path)
		{
			current_index = i;
			break;
		}
	}

#ifdef SIGNALFORGE_GUI
	// Flattened once, not per frame: the library doesn't change while the
	// board is running, and a redraw has no business copying preset data out
	// from under a switch that may be replacing it.
	std::vector<sf::BoardPreset> board_presets;
	board_presets.reserve(library.Count());

	for (size_t i = 0; i < library.Count(); ++i)
	{
		const sf::PresetLibrary::Entry& entry = library.At(i);

		// Fall back to the filename when a preset has no name of its own, so
		// the list never shows a blank row you can click but can't identify.
		board_presets.push_back({entry.preset.name.empty() ? entry.stem : entry.preset.name, entry.stem});
	}

	// Rebuilt from scratch every frame. Each entry costs a lilv name lookup,
	// which is microseconds against drawing a frame, and doing it live means
	// bypass state can never be stale -- it also changes from MIDI and from
	// the console, neither of which this code hears about.
	std::vector<sf::BoardStage> board_stages;

	// The message bound to each preset, and to stepping the library. Recomputed
	// after a learn rather than every frame: the map only changes when someone
	// binds something.
	std::string next_binding;
	std::string prev_binding;

	auto refresh_bindings = [&]
	{
		next_binding.clear();
		prev_binding.clear();

		for (sf::BoardPreset& entry : board_presets)
		{
			entry.binding.clear();
		}

		for (const sf::MidiBinding& binding : midi_map.Bindings())
		{
			switch (binding.action)
			{
				case sf::MidiAction::NextPreset:
					next_binding = DescribeMatcher(binding);
					break;

				case sf::MidiAction::PrevPreset:
					prev_binding = DescribeMatcher(binding);
					break;

				case sf::MidiAction::LoadPreset:
					for (sf::BoardPreset& entry : board_presets)
					{
						if (entry.stem == binding.preset || entry.name == binding.preset)
						{
							entry.binding = DescribeMatcher(binding);
						}
					}
					break;

				default:
					break;
			}
		}
	};

	refresh_bindings();

	// The part of a stage that costs a lilv query or a plugin state save to
	// read, and that only changes when the chain does.
	//
	// The rest of the snapshot is rebuilt every frame on purpose (bypass and
	// control values also change from MIDI and from the console, which this
	// code never hears about), and those are float reads. These are not:
	// PathProperties() re-parses the plugin's TTL, and reading a file
	// parameter's current value means asking the plugin to serialise its
	// state -- neural-amp-modeler logs a line each time it is asked. Doing
	// both 60 times a second is the UI thread paying, in RDF parsing and
	// allocation, to redraw a filename that cannot have changed.
	struct StageStatics
	{
		std::string name;
		std::string uri;
		std::vector<sf::BoardPathProperty> paths;
	};

	std::vector<StageStatics> stage_statics;

	// Cleared wherever the chain is replaced or a file is loaded into it --
	// the only two things that can make the above wrong.
	bool stage_statics_valid = false;

	auto snapshot_stages = [&]
	{
		if (!stage_statics_valid)
		{
			stage_statics.clear();

			for (size_t i = 0; i < chain->StageCount(); ++i)
			{
				sf::PluginStage* stage = chain->Stage(i);

				StageStatics statics;
				statics.name = stage->Name();
				statics.uri = stage->Uri();

				for (const sf::PathProperty& property : stage->PathProperties())
				{
					sf::BoardPathProperty path;
					path.uri = property.uri;
					path.label = property.label;
					path.file_types = property.file_types;
					path.value = stage->GetPathProperty(property.uri, urid_map);

					statics.paths.push_back(std::move(path));
				}

				stage_statics.push_back(std::move(statics));
			}

			stage_statics_valid = true;
		}

		board_stages.clear();

		for (size_t i = 0; i < chain->StageCount(); ++i)
		{
			sf::PluginStage* stage = chain->Stage(i);

			sf::BoardStage entry;
			entry.name = stage_statics[i].name;
			entry.uri = stage_statics[i].uri;
			entry.paths = stage_statics[i].paths;
			entry.bypassed = stage->IsBypassed();

			for (const sf::PortInfo& port : stage->Ports())
			{
				if (port.type != sf::PortType::Control)
				{
					continue;
				}

				sf::BoardControl control;
				control.symbol = port.symbol;
				control.name = port.name;
				control.value = stage->GetControl(port.symbol);
				control.minimum = port.min_value;
				control.maximum = port.max_value;
				control.toggled = port.toggled;

				// An enumeration is a set of numbered choices; until scale
				// point labels are read, a whole-number slider is at least
				// honest about what it accepts.
				control.integer = port.integer || port.enumeration;
				control.logarithmic = port.logarithmic;

				// Output control ports are meters. Reading them is the point;
				// writing them would be writing over the plugin.
				control.writable = port.direction == sf::PortDirection::Input;

				entry.controls.push_back(std::move(control));
			}

			// What message drives this stage's bypass, if any.
			for (const sf::MidiBinding& binding : midi_map.Bindings())
			{
				if (binding.action == sf::MidiAction::ToggleBypass && static_cast<size_t>(binding.stage) == i)
				{
					entry.bypass_binding = DescribeMatcher(binding);
					break;
				}
			}

			for (sf::BoardControl& control : entry.controls)
			{
				for (const sf::MidiBinding& binding : midi_map.Bindings())
				{
					if (binding.action == sf::MidiAction::SetControl && static_cast<size_t>(binding.stage) == i &&
						binding.port == control.symbol)
					{
						control.binding = DescribeMatcher(binding);
						break;
					}
				}
			}

			board_stages.push_back(std::move(entry));
		}
	};

	// The plugin catalogue, scanned once. Walking every installed plugin's
	// ports is far too slow to do while drawing, and nothing gets installed
	// while the board is running.
	std::vector<sf::BoardPlugin> board_plugins;
	size_t hidden_plugins = 0;

	{
		const LilvPlugins* all = lilv_world_get_all_plugins(world);

		LILV_FOREACH (plugins, it, all)
		{
			const LilvPlugin* candidate = lilv_plugins_get(all, it);

			unsigned audio_in = 0;
			unsigned audio_out = 0;

			for (const sf::PortInfo& port : sf::ClassifyPorts(world, candidate))
			{
				if (port.type != sf::PortType::Audio)
				{
					continue;
				}

				if (port.direction == sf::PortDirection::Input)
				{
					++audio_in;
				}
				else
				{
					++audio_out;
				}
			}

			// A sequential audio chain has nothing to do with a plugin that
			// has no audio path -- MIDI filters, control utilities. Counted
			// rather than dropped silently, so the browser can say so.
			if (audio_in == 0 || audio_out == 0)
			{
				++hidden_plugins;
				continue;
			}

			LilvNode* name_node = lilv_plugin_get_name(candidate);
			const std::string uri = lilv_node_as_uri(lilv_plugin_get_uri(candidate));
			std::string name = name_node ? lilv_node_as_string(name_node) : uri;

			if (name_node)
			{
				lilv_node_free(name_node);
			}

			board_plugins.push_back({name.empty() ? uri : name, uri, audio_in, audio_out});
		}
	}

	std::sort(board_plugins.begin(), board_plugins.end(),
		[](const sf::BoardPlugin& a, const sf::BoardPlugin& b) { return a.name < b.name; });

	// Whether the running chain has drifted from the file it came from.
	bool unsaved_changes = false;
	std::string ui_status;

	sf::BoardViewUi view_ui;

	// What the picker is showing, and where that listing came from.
	std::vector<sf::BoardFileEntry> file_entries;
	std::string listed_directory;

	// Somewhere sensible to start when nothing is loaded yet.
	if (const char* home = std::getenv("HOME"))
	{
		view_ui.browse_directory = home;
	}
#endif

	// Builds the new chain and crossfades to it. Everything expensive --
	// instantiating plugins, restoring state, loading models -- happens here
	// on the control thread while the old chain keeps making sound.
	// Writes what this run should be remembered by. Called on every switch, not
	// only at exit: a pedalboard is switched off at the wall rather than quit,
	// and "the preset you were playing" is worth a few hundred bytes per stomp
	// to survive that. The window's size is filled in by the shutdown path,
	// which is the only place there is still a window to ask.
	auto save_settings = [&]
	{
		// The window size and last preset are this run's to record; the
		// crossfade is whatever the file already said, unless it was changed
		// there rather than on the command line.
		sf::Settings to_write = settings;
		to_write.crossfade_ms = stored_crossfade_ms;
		to_write.crossfade_curve = stored_crossfade_curve;
		to_write.Clamp();

		std::string settings_error;

		if (!to_write.Save(settings_path, settings_error))
		{
			// Worth saying once, but never worth stopping for: the board is
			// playing, and this only affects what happens next time.
			std::fprintf(stderr, "Warning: %s\n", settings_error.c_str());
		}
	};

	// `from_file` is false for a chain the program itself assembled -- a stage
	// moved, removed, or added. Those can't be missing a plugin (they were
	// captured from something that is running), and more to the point they
	// must not clear a warning that belongs to the file on disk: the skipped
	// stage is still in the file, and forgetting that is how it gets saved
	// away.
	auto switch_to_preset = [&](const sf::Preset& next_preset, const std::string& next_path, size_t next_index,
							   bool from_file = true)
	{
		const auto build_start = std::chrono::steady_clock::now();
		const uint64_t cycles_before = engine.ProcessCycleCount();

		sf::ChainBuildReport report;

		std::unique_ptr<sf::Chain> next_chain = sf::BuildChainFromPreset(
			world, next_preset, engine.SampleRate(), engine.MaxBlockSize(), urid_map, error, &report);

		const auto build_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - build_start)
				.count();

		// Audio cycles completed while the new chain was being built.
		// Non-zero proves the old chain kept processing throughout, i.e. the
		// build really did stay off the audio thread.
		const uint64_t cycles_during_build = engine.ProcessCycleCount() - cycles_before;

		if (!next_chain)
		{
			std::fprintf(stderr, "%s\n", error.c_str());
			return false;
		}

		// Equal-power crossfade: both chains run for a few ms and are mixed,
		// so the transition is smooth rather than a click or a gap.
		// --no-crossfade takes the raw path, for comparing the two.
		const bool switched =
			use_crossfade ? engine.SwitchHost(next_chain.get()) : engine.SwapHost(next_chain.get());

		if (!switched)
		{
			return false;
		}

		// The audio thread may still be mid-cycle with the old chain; wait for
		// it to move on before destroying it.
		engine.WaitForProcessCycles(2);

		chain = std::move(next_chain);

#ifdef SIGNALFORGE_GUI
		// Different plugins, different file parameters: everything the view
		// caches about a stage belongs to the chain that just went.
		stage_statics_valid = false;
#endif

		current_index = next_index;
		current_name = next_preset.name;
		current_path = next_path;
		current_source = next_preset;

		if (from_file)
		{
			missing_plugins = report.skipped_uris;

			// The stem, not the path: presets get moved between directories
			// with --preset-dir, and the name is the part that survives it.
			settings.last_preset = std::filesystem::path(current_path).stem().string();
			save_settings();
		}

		std::printf("Switched to '%s' (%s), built in %lldms (%llu audio cycle(s) ran during the build).\n",
			next_preset.name.c_str(), next_path.c_str(), static_cast<long long>(build_ms),
			static_cast<unsigned long long>(cycles_during_build));
		std::fflush(stdout);

		return true;
	};

	// Switches by name/index/path, the way a typed command or a load_preset
	// binding asks for it.
	auto switch_to_query = [&](const std::string& query)
	{
		sf::Preset next_preset;
		std::string next_path;

		if (!ResolvePreset(query, preset_dir, next_preset, next_path))
		{
			return false;
		}

		// Match on the resolved path rather than the query, so stepping still
		// works after a switch requested by name, index, or file path alike.
		size_t next_index = library.Count();

		for (size_t i = 0; i < library.Count(); ++i)
		{
			if (library.At(i).path == next_path)
			{
				next_index = i;
				break;
			}
		}

		return switch_to_preset(next_preset, next_path, next_index);
	};

	// next_preset / prev_preset. Wraps around, because a player stepping past
	// the end of the board wants the first sound back, not silence.
	auto step_preset = [&](int delta)
	{
		if (library.Count() == 0)
		{
			std::fprintf(stderr, "No preset library in '%s' to step through.\n", preset_dir.c_str());
			return false;
		}

		const size_t count = library.Count();
		const size_t next_index =
			current_index >= count ? (delta > 0 ? 0 : count - 1) : (current_index + count + delta) % count;

		return switch_to_preset(library.At(next_index).preset, library.At(next_index).path, next_index);
	};

	sf::MidiLearn midi_learn;
	const auto start_time = std::chrono::steady_clock::now();

	// Whether the "that controller is chattering" advice has been given for
	// the current learn.
	bool reported_noisy_source = false;

	// Writes a learned binding into the table and back to disk. Re-learning a
	// switch moves it rather than failing as a duplicate, which is the whole
	// point of pressing it again.
	auto commit_learned = [&](const sf::MidiBinding& learned)
	{
		const size_t replaced = midi_map.Bind(learned);

		std::printf("Bound %s %d (channel %d) -> %s%s\n", sf::ToString(learned.type), learned.number, learned.channel,
			DescribeBinding(learned).c_str(), replaced > 0 ? "  (replaced an existing binding)" : "");

		std::string save_error;
		const bool saved = midi_map.Save(midi_map_path, save_error);

		if (!saved)
		{
			std::fprintf(stderr, "%s -- the binding is active but was not saved.\n", save_error.c_str());
		}
		else
		{
			std::printf("Saved to %s\n", midi_map_path.c_str());
		}

		std::fflush(stdout);

#ifdef SIGNALFORGE_GUI
		// So the window shows what was just bound rather than what was bound
		// when it started.
		refresh_bindings();

		// The learn buttons are meant to make the console unnecessary, so the
		// outcome has to appear in the window too -- otherwise a learn that
		// succeeded and one that quietly failed to save look identical.
		ui_status = "Bound " + DescribeMatcher(learned) + " -> " + DescribeBinding(learned);

		if (replaced > 0)
		{
			ui_status += " (replaced)";
		}

		if (!saved)
		{
			// Active either way, but it won't survive a restart, and that is
			// exactly the sort of thing a player discovers at a rehearsal.
			ui_status += " -- but not saved: " + save_error;
		}
#endif
	};

	// toggle_bypass / set_control act on the *running* chain in place -- no
	// rebuild, so they take effect on the next audio block.
	auto stage_for = [&](const sf::MidiBinding& binding) -> sf::PluginStage*
	{
		if (static_cast<size_t>(binding.stage) >= chain->StageCount())
		{
			std::fprintf(stderr, "Binding targets stage %d, but the running chain has %zu stage(s).\n", binding.stage,
				chain->StageCount());
			return nullptr;
		}

		return chain->Stage(static_cast<size_t>(binding.stage));
	};

	auto perform = [&](const sf::MidiActionInvocation& invocation)
	{
		const sf::MidiBinding& binding = *invocation.binding;

		switch (invocation.action)
		{
			case sf::MidiAction::LoadPreset:
				switch_to_query(binding.preset);
				break;

			case sf::MidiAction::NextPreset:
				step_preset(1);
				break;

			case sf::MidiAction::PrevPreset:
				step_preset(-1);
				break;

			case sf::MidiAction::ToggleBypass:
				if (sf::PluginStage* stage = stage_for(binding))
				{
					const bool bypassed = !stage->IsBypassed();
					stage->SetBypassed(bypassed);

					std::printf("Stage %d (%s) is now %s.\n", binding.stage, stage->Uri().c_str(),
						bypassed ? "BYPASSED" : "active");
					std::fflush(stdout);
				}
				break;

			case sf::MidiAction::SetControl:
				if (sf::PluginStage* stage = stage_for(binding))
				{
					// Deliberately quiet: an expression pedal sends a message
					// per degree of travel, and a line each would bury
					// everything else.
					stage->SetControl(binding.port, invocation.control_value);
				}
				break;
		}
	};

	std::signal(SIGINT, HandleSigint);

#ifdef SIGNALFORGE_GUI
	// Moving or removing a stage changes the shape of the chain, so it costs a
	// rebuild -- the same crossfaded rebuild a preset switch uses, for the same
	// reason: the old chain keeps making sound while the new one is built.
	//
	// The edit is applied to a snapshot of the *running* chain rather than to
	// the file it came from, so a knob moved by hand or by an expression pedal
	// survives having a stage reordered underneath it.
	auto apply_stage_edit = [&](sf::StageEdit edit, size_t index)
	{
		sf::Preset edited = sf::CaptureChainPreset(*chain, urid_map, current_name);

		if (index >= edited.chain.size())
		{
			return;
		}

		switch (edit)
		{
			case sf::StageEdit::MoveUp:
				std::swap(edited.chain[index - 1], edited.chain[index]);
				break;

			case sf::StageEdit::MoveDown:
				std::swap(edited.chain[index], edited.chain[index + 1]);
				break;

			case sf::StageEdit::Remove:
				edited.chain.erase(edited.chain.begin() + static_cast<long>(index));
				break;

			default:
				return;
		}

		// A refused rebuild leaves the running chain untouched, which is the
		// behaviour that matters: adjacent stages have to agree on channel
		// counts, and not every order of a given set of plugins does.
		if (!switch_to_preset(edited, current_path, current_index, false))
		{
			ui_status = "Could not apply that change: " + error;
			return;
		}

		unsaved_changes = true;
		ui_status.clear();
	};

	// Re-reads the directory after a save, so next/prev and the preset list
	// see what was just written rather than the copy loaded at startup.
	auto reload_library = [&]
	{
		sf::PresetLibrary fresh;
		std::string library_error;
		std::vector<std::string> library_warnings;

		if (!sf::PresetLibrary::Open(preset_dir, fresh, library_error, library_warnings))
		{
			return;
		}

		library = std::move(fresh);
		board_presets.clear();
		current_index = library.Count();

		for (size_t i = 0; i < library.Count(); ++i)
		{
			const sf::PresetLibrary::Entry& entry = library.At(i);
			board_presets.push_back({entry.preset.name.empty() ? entry.stem : entry.preset.name, entry.stem});

			if (entry.path == current_path)
			{
				current_index = i;
			}
		}
	};

	auto save_current = [&]
	{
		// A capture only knows about stages that are running, so saving a
		// degraded board would quietly delete the pedals whose plugins are
		// missing -- the file being the only record that they were ever in the
		// chain. Refused rather than silently lossy; the button is disabled
		// too, so this is the second line of defence.
		if (!missing_plugins.empty())
		{
			ui_status = "Not saving: " + DescribeMissingPlugins(missing_plugins);
			std::fprintf(stderr, "%s\n", ui_status.c_str());
			return;
		}

		sf::Preset snapshot = sf::CaptureChainPreset(*chain, urid_map, current_name);

		// The chain comes from what's running; everything else comes from the
		// file, because a capture has no way to know about it.
		snapshot.midi_trigger = current_source.midi_trigger;

		std::string save_error;

		if (!sf::SavePresetFile(current_path, snapshot, save_error))
		{
			ui_status = "Could not save: " + save_error;
			return;
		}

		current_source = snapshot;
		unsaved_changes = false;
		ui_status = "Saved " + current_path;

		std::printf("Saved %s\n", current_path.c_str());
		std::fflush(stdout);

		reload_library();
	};

	// Opened after the engine is up, so the window only ever appears over a
	// board that is already making sound.
	sf::AppWindow window;

	if (options.show_gui)
	{
		std::string ui_error;

		if (!window.Open("Signalforge", settings.window_width, settings.window_height, ui_error))
		{
			std::fprintf(stderr, "%s\n", ui_error.c_str());
			engine.Close();
			chain.reset();
			lilv_world_free(world);
			return 1;
		}
	}
#endif

	// Commands arrive on their own thread so audio keeps running while a
	// line is being waited for.
	std::mutex command_mutex;
	std::vector<std::string> pending_commands;

	std::thread command_reader(
		[&command_mutex, &pending_commands]
		{
			std::string line;

			while (std::getline(std::cin, line))
			{
				std::lock_guard<std::mutex> lock(command_mutex);
				pending_commands.push_back(line);
			}
		});

	command_reader.detach();

	uint64_t reported_midi_drops = 0;

	while (g_keep_running)
	{
		// Drain whatever the audio thread parsed since last time. Logging
		// happens here, never on the audio thread.
		sf::MidiEvent midi_event;

		while (engine.PopMidiEvent(midi_event))
		{
			const uint64_t now_ms = static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time)
					.count());

			// Fed unconditionally: the traffic history is what lets learn
			// tell a deliberate press from a controller that chatters, and it
			// has to predate the moment we arm.
			midi_learn.Observe(midi_event, now_ms);

			std::printf("MIDI  %-14s ch=%-2d number=%-3d value=%d", sf::ToString(midi_event.type),
				midi_event.channel, midi_event.number, midi_event.value);

			if (midi_learn.Armed())
			{
				sf::MidiBinding learned;

				switch (midi_learn.Offer(midi_event, now_ms, learned))
				{
					case sf::MidiLearn::Result::Bound:
						std::printf("  ->  learned\n");
						std::fflush(stdout);
						commit_learned(learned);
						continue;

					case sf::MidiLearn::Result::Noisy:
						// Once per learn: a chattering controller produces
						// dozens of these a second, and repeating the advice
						// would bury the message telling you to press
						// something else.
						if (!reported_noisy_source)
						{
							std::printf("  ->  ignored (this controller is streaming on its own; "
										"press something else, or unplug it)\n");
							reported_noisy_source = true;

#ifdef SIGNALFORGE_GUI
							// The window would otherwise show "press
							// something" forever while a chattering pedal
							// silently ate every press.
							ui_status = std::string("Ignoring ") + sf::ToString(midi_event.type) + " " +
								std::to_string(midi_event.number) +
								" -- that controller is streaming on its own. Press something else.";
#endif
						}
						else
						{
							std::printf("  ->  ignored (still streaming)\n");
						}

						std::fflush(stdout);
						continue;

					case sf::MidiLearn::Result::Ignored:
						break;
				}

				// Not a message this target can use (a release, a wrong
				// type). Fall through and log it normally; still listening.
				std::printf("  ->  waiting for a %s\n",
					midi_learn.Target().action == sf::MidiAction::SetControl ? "control to move" : "switch press");
				std::fflush(stdout);
				continue;
			}

			// Resolving on this thread is deliberate: MidiMap tracks which
			// switches are held, so it is not audio-thread safe.
			sf::MidiActionInvocation invocation;

			const bool resolved = midi_map.Size() > 0 && midi_map.Resolve(midi_event, invocation);

			if (resolved)
			{
				std::printf("  ->  %s", DescribeBinding(*invocation.binding).c_str());

				if (invocation.action == sf::MidiAction::SetControl)
				{
					std::printf(" = %.3f", invocation.control_value);
				}
			}

			std::printf("\n");
			std::fflush(stdout);

			// After the log line, so the action's own output reads as a
			// consequence of the message rather than interleaving with it.
			if (resolved)
			{
				perform(invocation);
			}
		}

		const uint64_t drops = engine.DroppedMidiEvents();

		if (drops != reported_midi_drops)
		{
			std::fprintf(stderr, "Warning: dropped %llu MIDI event(s) (queue full).\n",
				static_cast<unsigned long long>(drops - reported_midi_drops));
			reported_midi_drops = drops;
		}

		std::vector<std::string> commands;

		{
			std::lock_guard<std::mutex> lock(command_mutex);
			commands.swap(pending_commands);
		}

		for (const std::string& command : commands)
		{
			if (command.empty())
			{
				continue;
			}

			if (command == "quit" || command == "exit")
			{
				g_keep_running = false;
				break;
			}

			// What the board is actually doing right now: which preset, and
			// each stage's bypass state and control values. This is the
			// readback for MIDI actions that are otherwise silent -- an
			// expression pedal moving a control prints nothing, by design.
			if (command == "status")
			{
				std::printf("Preset: %s\n",
					current_index < library.Count() ? library.At(current_index).stem.c_str() : "(not from library)");

				for (size_t i = 0; i < chain->StageCount(); ++i)
				{
					sf::PluginStage* stage = chain->Stage(i);
					std::printf("  [%zu] %-60s %s\n", i, stage->Uri().c_str(),
						stage->IsBypassed() ? "BYPASSED" : "active");

					for (const auto& [symbol, value] : stage->CaptureControls())
					{
						std::printf("        %-24s %g\n", symbol.c_str(), value);
					}
				}

				std::fflush(stdout);
				continue;
			}

			if (command.rfind("learn", 0) == 0 && (command.size() == 5 || command[5] == ' '))
			{
				if (command == "learn cancel")
				{
					midi_learn.Cancel();
					std::printf("Learning cancelled.\n");
					std::fflush(stdout);
					continue;
				}

				sf::MidiBinding target;
				std::string learn_error;

				if (!sf::ParseLearnCommand(command.substr(5), target, learn_error))
				{
					std::fprintf(stderr, "%s\n", learn_error.c_str());
					continue;
				}

				// A set_control learned without an explicit range should span
				// the port's real one, not a meaningless 0..1.
				if (target.action == sf::MidiAction::SetControl && target.minimum == 0.0f && target.maximum == 1.0f)
				{
					if (sf::PluginStage* stage = stage_for(target))
					{
						for (const sf::PortInfo& port : stage->Ports())
						{
							if (port.type == sf::PortType::Control && port.symbol == target.port)
							{
								target.minimum = port.min_value;
								target.maximum = port.max_value;
								break;
							}
						}
					}
				}

				const uint64_t now_ms = static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time)
						.count());

				midi_learn.Arm(target, now_ms);
				reported_noisy_source = false;

				std::printf("Learning %s -- %s now ('learn cancel' to stop).\n", DescribeBinding(target).c_str(),
					target.action == sf::MidiAction::SetControl ? "move the pedal or knob" : "press the switch");
				std::fflush(stdout);

				continue;
			}

			// Typed equivalents of the pedal actions, so the mapping can be
			// exercised (and the board driven) without hardware.
			if (command == "next")
			{
				step_preset(1);
				continue;
			}

			if (command == "prev")
			{
				step_preset(-1);
				continue;
			}

			if (command.rfind("bypass ", 0) == 0)
			{
				sf::MidiBinding binding;
				binding.stage = std::atoi(command.c_str() + 7);

				if (sf::PluginStage* stage = stage_for(binding))
				{
					const bool bypassed = !stage->IsBypassed();
					stage->SetBypassed(bypassed);

					std::printf("Stage %d (%s) is now %s.\n", binding.stage, stage->Uri().c_str(),
						bypassed ? "BYPASSED" : "active");
					std::fflush(stdout);
				}

				continue;
			}

			switch_to_query(command);
		}

#ifdef SIGNALFORGE_GUI
		if (window.IsOpen())
		{
			if (!window.BeginFrame())
			{
				g_keep_running = false;
				continue;
			}

			sf::BoardViewState view;
			view.preset_name = current_name;
			view.preset_path = current_path;
			view.sample_rate = engine.SampleRate();
			view.block_size = engine.MaxBlockSize();
			view.audio_cycles = engine.ProcessCycleCount();
			view.midi_drops = reported_midi_drops;
			view.midi_bindings = midi_map.Size();
			view.presets = &board_presets;
			view.current_preset = current_index < library.Count() ? current_index : sf::kNoPreset;
			view.unsaved_changes = unsaved_changes;
			view.status = ui_status;
			view.warning = DescribeMissingPlugins(missing_plugins);
			view.save_blocked_reason = missing_plugins.empty()
				? std::string()
				: "Saving would write this preset without its missing stage(s). Install the plugin(s), or edit the "
				  "file by hand.";

			snapshot_stages();
			view.stages = &board_stages;
			view.plugins = &board_plugins;
			view.plugins_hidden = hidden_plugins;
			view.next_binding = next_binding;
			view.prev_binding = prev_binding;
			view.learning = midi_learn.Armed();
			view.learning_what = midi_learn.Armed() ? DescribeBinding(midi_learn.Target()) : std::string();

			// Listed only while the picker is open, and only when it moved --
			// a directory scan every frame would be pointless work, and on a
			// network share a visible stall.
			if (view_ui.picker_open && view_ui.browse_directory != listed_directory)
			{
				listed_directory = view_ui.browse_directory;
				file_entries = ListDirectory(listed_directory, PathFilterFor(board_stages, view_ui));
			}

			view.file_entries = &file_entries;

			sf::DrawBoard(view, view_ui);

			// Presenting the frame waits for the display, which is what paces
			// this loop -- so no sleep on the UI path.
			window.EndFrame();

			// After presenting, not before: the switch builds a chain and can
			// take longer than a frame, and doing it here means the click is
			// drawn as pressed before the window stops responding, rather than
			// the window simply appearing to hang.
			if (view.requested_preset != sf::kNoPreset && view.requested_preset != current_index &&
				view.requested_preset < library.Count())
			{
				const sf::PresetLibrary::Entry& entry = library.At(view.requested_preset);

				// Loading a preset is how you discard edits you didn't save.
				if (switch_to_preset(entry.preset, entry.path, view.requested_preset))
				{
					unsaved_changes = false;
					ui_status.clear();
				}
			}

			if (view.stage_edit == sf::StageEdit::ToggleBypass && view.stage_edit_index < chain->StageCount())
			{
				// No rebuild: this flips a flag the audio thread reads, and
				// takes effect on the next block.
				sf::PluginStage* stage = chain->Stage(view.stage_edit_index);
				stage->SetBypassed(!stage->IsBypassed());
				unsaved_changes = true;
			}
			else if (view.stage_edit != sf::StageEdit::None && view.stage_edit_index != sf::kNoPreset)
			{
				apply_stage_edit(view.stage_edit, view.stage_edit_index);
			}

			// Where the picker is looking, resolved here because this is the
			// side with a filesystem. A relative name, "..", and an absolute
			// path all work: operator/ with an absolute right-hand side simply
			// replaces the left.
			if (!view.browse_request.empty())
			{
				std::error_code ec;
				const std::filesystem::path base =
					view_ui.browse_directory.empty() ? std::filesystem::path("/") : std::filesystem::path(view_ui.browse_directory);

				const std::filesystem::path target = std::filesystem::weakly_canonical(base / view.browse_request, ec);

				if (!ec)
				{
					view_ui.browse_directory = target.string();
				}
			}

			if (view.learn_request.target != sf::LearnTarget::None)
			{
				const sf::LearnRequest& request = view.learn_request;

				if (request.target == sf::LearnTarget::Cancel)
				{
					midi_learn.Cancel();
					ui_status = "Learning cancelled.";
				}
				else
				{
					sf::MidiBinding target;

					switch (request.target)
					{
						case sf::LearnTarget::LoadPreset:
							target.action = sf::MidiAction::LoadPreset;
							target.preset = request.preset;
							break;

						case sf::LearnTarget::NextPreset:
							target.action = sf::MidiAction::NextPreset;
							break;

						case sf::LearnTarget::PrevPreset:
							target.action = sf::MidiAction::PrevPreset;
							break;

						case sf::LearnTarget::ToggleBypass:
							target.action = sf::MidiAction::ToggleBypass;
							target.stage = static_cast<int>(request.stage);
							break;

						case sf::LearnTarget::SetControl:
							target.action = sf::MidiAction::SetControl;
							target.stage = static_cast<int>(request.stage);
							target.port = request.symbol;

							// The view carries the port's real range, so a
							// pedal sweeps the whole control.
							target.minimum = request.minimum;
							target.maximum = request.maximum;
							break;

						default:
							break;
					}

					const uint64_t now_ms = static_cast<uint64_t>(
						std::chrono::duration_cast<std::chrono::milliseconds>(
							std::chrono::steady_clock::now() - start_time)
							.count());

					midi_learn.Arm(target, now_ms);
					reported_noisy_source = false;
					ui_status.clear();
				}
			}

			if (view.path_stage < chain->StageCount())
			{
				sf::PluginStage* stage = chain->Stage(view.path_stage);

				if (stage->LoadPathProperty(view.path_property, view.path_file, urid_map))
				{
					// Loading happens on the worker thread. Settling here, on
					// the control thread, means the next status readback and
					// the next frame both see the model that was actually
					// asked for rather than the one still being replaced.
					stage->SettleWorker();

					// The one thing besides a chain rebuild that changes what a
					// stage's file parameters read back as, so the next frame
					// has to ask the plugin again rather than redraw the model
					// that was there before.
					stage_statics_valid = false;

					unsaved_changes = true;
					ui_status = "Loaded " + view.path_file;
				}
				else
				{
					ui_status = "Could not load " + view.path_file;
				}
			}

			if (view.control_stage < chain->StageCount())
			{
				// Straight onto the running plugin: no rebuild, no crossfade,
				// audible on the next block. This is the same path a MIDI
				// expression pedal takes.
				chain->Stage(view.control_stage)->SetControl(view.control_symbol, view.control_value);
				unsaved_changes = true;
			}

			if (view.requested_plugin < board_plugins.size())
			{
				const sf::BoardPlugin& chosen = board_plugins[view.requested_plugin];

				const sf::Preset edited = PresetWithAddedStage(*chain, urid_map, current_name, chosen.uri);

				if (!switch_to_preset(edited, current_path, current_index, false))
				{
					// Most often a channel-count mismatch with the stage
					// before it. The running chain is untouched either way.
					ui_status = "Could not add " + chosen.name + ": " + error;
				}
				else
				{
					unsaved_changes = true;
					ui_status = "Added " + chosen.name + ".";
				}
			}

			if (view.save_requested)
			{
				save_current();
			}

			if (view.quit_requested)
			{
				g_keep_running = false;
			}

			continue;
		}
#endif

		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	std::printf("Stopping.\n");

#ifdef SIGNALFORGE_GUI
	// While there is still a window to ask. Resizing it is not an event this
	// program acts on, so its size is read once, here, rather than watched.
	if (window.IsOpen())
	{
		window.Size(settings.window_width, settings.window_height);
	}
#endif

	settings.last_preset = std::filesystem::path(current_path).stem().string();
	save_settings();

#ifdef SIGNALFORGE_GUI
	// Before the engine: the window is the thing the user just closed, and it
	// should go away immediately rather than after the audio teardown.
	window.Close();
#endif

	engine.Close();
	chain.reset();
	lilv_world_free(world);

	return 0;
}

// The measurable half of SF-33's tuning pass. Prints, for each candidate fade
// length and both curves, the three numbers that decide how a switch sounds:
//
//   step   the largest gain change between adjacent samples. This is the click
//          risk -- a fade short enough that the gain jumps is a fade you hear
//          as an edge rather than as a handover.
//   power  how far from^2 + to^2 strays from 1 across the fade. Zero means two
//          uncorrelated sounds (a clean amp and a fuzz) hand over at constant
//          loudness; a negative dip is the hole in the middle of a switch.
//   sum    how far from + to strays from 1. Zero means two nearly identical
//          sounds hand over without a bulge.
//
// No curve is zero in both columns -- that is the trade, and which side of it
// is right depends on the pedals. The remaining half is the ear: run the board
// with --crossfade-ms and stomp between two presets that sound nothing alike.
int TuneCrossfade(double sample_rate)
{
	const double lengths[] = {0.0, 2.0, 5.0, 8.0, 12.0, 20.0, 35.0, 60.0};
	const sf::CrossfadeCurve curves[] = {sf::CrossfadeCurve::EqualPower, sf::CrossfadeCurve::Linear};

	std::printf("Crossfade tuning at %.0f Hz (default %.1f ms, maximum %.0f ms):\n\n", sample_rate,
		sf::Crossfade::kDefaultMilliseconds, sf::Crossfade::kMaxMilliseconds);
	std::printf("  %-8s %-13s %8s %10s %10s %10s\n", "ms", "curve", "samples", "step", "power", "sum");

	for (double milliseconds : lengths)
	{
		for (sf::CrossfadeCurve curve : curves)
		{
			sf::Crossfade fade;
			fade.Configure(sample_rate, milliseconds, curve);

			const uint64_t samples = fade.LengthInSamples();

			// Walked at the same resolution the audio thread would: the step
			// between adjacent samples is only meaningful at the real rate.
			const uint64_t steps = samples > 1 ? samples : 1;

			float worst_step = 0.0f;
			float worst_power = 0.0f;
			float worst_sum = 0.0f;

			float previous_from = 1.0f;
			float previous_to = 0.0f;

			for (uint64_t i = 0; i <= steps; ++i)
			{
				const float position = static_cast<float>(static_cast<double>(i) / static_cast<double>(steps));

				float from_gain = 0.0f;
				float to_gain = 0.0f;
				sf::Crossfade::Gains(position, curve, from_gain, to_gain);

				if (i > 0)
				{
					worst_step = std::max(worst_step,
						std::max(std::fabs(from_gain - previous_from), std::fabs(to_gain - previous_to)));
				}

				const float power_error = from_gain * from_gain + to_gain * to_gain - 1.0f;
				const float sum_error = from_gain + to_gain - 1.0f;

				if (std::fabs(power_error) > std::fabs(worst_power))
				{
					worst_power = power_error;
				}

				if (std::fabs(sum_error) > std::fabs(worst_sum))
				{
					worst_sum = sum_error;
				}

				previous_from = from_gain;
				previous_to = to_gain;
			}

			std::printf("  %-8.1f %-13s %8llu %10.4f %+10.3f %+10.3f\n", milliseconds, sf::ToString(curve),
				static_cast<unsigned long long>(samples), worst_step, worst_power, worst_sum);
		}
	}

	std::printf("\nA zero-length fade is a swap: one block boundary, one sample of gain step.\n");
	std::printf("Set what you land on with --crossfade-ms / --crossfade-curve, or in the settings file.\n");

	return 0;
}

struct TestChainOptions
{
	double sample_rate = 48000.0;
	uint32_t block_size = 256;
	double seconds = 2.0;
	bool expect_signal = false;

	// When set, the chain comes from this preset (path, name or index)
	// instead of from URIs/--set-path/--control on the command line.
	std::string preset_path;
	std::string preset_dir = "presets";

	// Stage indices to bypass, so the bypass path can be measured offline
	// rather than only by ear.
	std::vector<size_t> bypass_stages;
};

// Headless: builds the chain and pushes a sine sweep through it with no
// audio backend involved, then reports/validates what came out. Exits
// non-zero on failure so CI can gate on it.
int TestChain(const std::vector<std::string>& uri_strings, const std::vector<ChainPathArg>& paths,
	const std::vector<ChainControlArg>& controls, const TestChainOptions& options)
{
	LilvWorld* world = lilv_world_new();
	lilv_world_load_all(world);

	sf::UridMap urid_map;

	std::unique_ptr<sf::Chain> chain;
	size_t stage_count = 0;

	if (!options.preset_path.empty())
	{
		sf::Preset preset;
		std::string error;
		std::string resolved_path;

		if (!ResolvePreset(options.preset_path, options.preset_dir, preset, resolved_path))
		{
			lilv_world_free(world);
			return 1;
		}

		std::printf("Preset: %s (%s)\n", preset.name.c_str(), resolved_path.c_str());

		sf::ChainBuildReport report;

		chain = sf::BuildChainFromPreset(
			world, preset, options.sample_rate, options.block_size, urid_map, error, &report);

		if (!chain)
		{
			std::fprintf(stderr, "FAIL: %s\n", error.c_str());
			lilv_world_free(world);
			return 1;
		}

		// Named on stdout in a fixed form, so the degradation is measurable
		// rather than only visible: the test asserts on this line, and on the
		// audio that came out of the chain that was built anyway.
		std::printf("skipped-stages: %zu\n", report.skipped_indices.size());

		for (size_t i = 0; i < report.skipped_indices.size(); ++i)
		{
			std::printf("  skipped stage %zu: %s\n", report.skipped_indices[i], report.skipped_uris[i].c_str());
		}

		// BuildChainFromPreset already restored state, applied the preset's own
		// controls and settled the workers. Anything given on the command line
		// is applied below, on top.
		stage_count = chain->StageCount();
	}
	else
	{
		chain = sf::Chain::Create(world, uri_strings, options.sample_rate, options.block_size, urid_map);

		if (!chain)
		{
			lilv_world_free(world);
			return 1;
		}

		stage_count = chain->StageCount();
	}

	// Applied to both paths, not just the plugin-list one. When a preset was
	// given these override what the file said, which is the only precedence
	// that makes sense for a flag typed on the command line -- and until this
	// ran for presets too, "--preset X --control 0:gain=12" silently did
	// nothing and looked like the control had no effect on the sound.
	ApplyChainControls(*chain, controls);
	ApplyChainPaths(*chain, paths, urid_map);

	// Model/IR loading is asynchronous (worker thread). Offline processing can
	// outrun it, so wait for loads to land -- otherwise the measured output
	// would depend on disk speed vs. how fast we chew through blocks.
	chain->Settle();

	for (size_t stage_index : options.bypass_stages)
	{
		if (stage_index >= chain->StageCount())
		{
			std::fprintf(stderr, "--bypass %zu: the chain has %zu stage(s).\n", stage_index, chain->StageCount());
			lilv_world_free(world);
			return 1;
		}

		chain->Stage(stage_index)->SetBypassed(true);
	}

	// Deliberately not a whole number of blocks: the final short block
	// exercises the "fewer frames than max block length" path.
	const uint64_t total_frames = static_cast<uint64_t>(options.sample_rate * options.seconds) + 37;

	sf::OfflineEngine engine(options.block_size);
	const sf::OfflineEngine::Result result = engine.Run(
		*chain, total_frames, sf::MakeSineSweep(options.sample_rate, options.seconds, 50.0, 8000.0, 0.5f));

	std::printf("\nHeadless chain test (%zu stage(s), %.0f Hz, block %u):\n", stage_count, options.sample_rate,
		options.block_size);
	std::printf("  channels:      %u in / %u out\n", result.input_channels, result.output_channels);
	std::printf("  frames:        %llu processed / %llu expected\n",
		static_cast<unsigned long long>(result.frames_processed), static_cast<unsigned long long>(total_frames));
	std::printf("  non-finite:    %llu sample(s)\n", static_cast<unsigned long long>(result.non_finite_samples));
	std::printf("  peak:          %.6f\n", result.peak);
	std::printf("  rms:           %.6f\n", result.rms);
	std::printf("  output-hash:   %016llx\n", static_cast<unsigned long long>(result.output_hash));

	int failures = 0;

	if (result.frames_processed != total_frames)
	{
		std::fprintf(stderr, "FAIL: processed %llu frames, expected %llu.\n",
			static_cast<unsigned long long>(result.frames_processed), static_cast<unsigned long long>(total_frames));
		++failures;
	}

	if (result.non_finite_samples > 0)
	{
		std::fprintf(stderr, "FAIL: %llu non-finite (NaN/Inf) output sample(s).\n",
			static_cast<unsigned long long>(result.non_finite_samples));
		++failures;
	}

	if (result.output_channels == 0)
	{
		std::fprintf(stderr, "FAIL: chain has no audio output ports.\n");
		++failures;
	}

	if (options.expect_signal && result.peak == 0.0f)
	{
		std::fprintf(stderr, "FAIL: --expect-signal given, but the output was completely silent.\n");
		++failures;
	}

	std::printf("  result:        %s\n\n", failures == 0 ? "PASS" : "FAIL");

	chain.reset();
	lilv_world_free(world);

	return failures == 0 ? 0 : 1;
}

// Builds a chain headlessly (no audio backend), applies any --set-path
// loads, waits for them to land, then snapshots the whole thing to a
// preset file.
int SavePreset(const char* output_path, const std::vector<std::string>& uri_strings,
	const std::vector<ChainPathArg>& paths, const std::vector<ChainControlArg>& controls, const std::string& name)
{
	LilvWorld* world = lilv_world_new();
	lilv_world_load_all(world);

	sf::UridMap urid_map;

	// Sample rate / block size only matter for instantiation here; nothing
	// is processed.
	std::unique_ptr<sf::Chain> chain = sf::Chain::Create(world, uri_strings, 48000.0, 256, urid_map);

	if (!chain)
	{
		lilv_world_free(world);
		return 1;
	}

	ApplyChainControls(*chain, controls);
	ApplyChainPaths(*chain, paths, urid_map);

	// Loading is asynchronous; capture would otherwise race it and record
	// the state from *before* the model landed.
	chain->Settle();

	const sf::Preset preset = sf::CaptureChainPreset(*chain, urid_map, name);

	std::string error;

	if (!sf::SavePresetFile(output_path, preset, error))
	{
		std::fprintf(stderr, "FAIL: %s\n", error.c_str());
		chain.reset();
		lilv_world_free(world);
		return 1;
	}

	std::printf("Saved preset '%s' (%zu stage(s)) to %s\n", preset.name.c_str(), preset.chain.size(), output_path);

	for (size_t i = 0; i < preset.chain.size(); ++i)
	{
		const sf::PresetStage& stage = preset.chain[i];
		std::printf("  [%zu] %s: %zu control(s), %zu byte(s) of state\n", i, stage.uri.c_str(), stage.controls.size(),
			stage.state.size());
	}

	chain.reset();
	lilv_world_free(world);

	return 0;
}

// Loads a preset, serializes it back out, re-parses that, and checks the
// two parsed presets are identical.
//
// The comparison is on the parsed model rather than the raw JSON text on
// purpose: control values are floats (that's what LV2 control ports are),
// so a hand-written 0.1 necessarily becomes the nearest float and would
// re-serialize as 0.10000000149011612. Comparing text would flag that as
// "loss" when no information the host cares about was actually lost;
// comparing models asks the question that matters -- does everything
// survive a save/load cycle unchanged?
int PresetRoundTrip(const char* path)
{
	sf::Preset original;
	std::string error;

	if (!sf::LoadPresetFile(path, original, error))
	{
		std::fprintf(stderr, "FAIL: %s\n", error.c_str());
		return 1;
	}

	const nlohmann::json serialized = original.ToJson();

	sf::Preset reparsed;

	if (!sf::Preset::FromJson(serialized, reparsed, error))
	{
		std::fprintf(stderr, "FAIL: re-parsing our own output failed: %s\n", error.c_str());
		return 1;
	}

	std::printf("Loaded '%s':\n", path);
	std::printf("  name:    %s\n", original.name.c_str());
	std::printf("  stages:  %zu\n", original.chain.size());

	for (size_t i = 0; i < original.chain.size(); ++i)
	{
		const sf::PresetStage& stage = original.chain[i];
		std::printf("    [%zu] %s%s, %zu control(s), %s\n", i, stage.uri.c_str(), stage.bypassed ? " (bypassed)" : "",
			stage.controls.size(), stage.state.empty() ? "no state" : "has state");
	}

	if (original.midi_trigger)
	{
		std::printf("  trigger: %s ch%d #%d\n", original.midi_trigger->type.c_str(), original.midi_trigger->channel,
			original.midi_trigger->number);
	}
	else
	{
		std::printf("  trigger: (none)\n");
	}

	std::printf("\nRe-serialized:\n%s\n\n", serialized.dump(2).c_str());

	if (!(original == reparsed))
	{
		std::fprintf(stderr, "FAIL: preset changed across a save/load round-trip.\n");
		return 1;
	}

	std::printf("Round-trip: PASS (parsed preset is identical after serialize + re-parse)\n");

	return 0;
}

int TestUi(int frames)
{
#ifdef SIGNALFORGE_GUI
	return sf::RunUiSelfTest(frames);
#else
	// Skipped rather than failed: a build with no UI is a supported build,
	// and ctest is told to read this word as such.
	(void)frames;
	std::printf("SKIPPED: built without SDL2, so there is no UI to test.\n");
	return 0;
#endif
}

// Appends a plugin to a preset's chain exactly the way the "Add stage" button
// does -- capture the running chain, push a bare stage, rebuild -- and proves
// the result actually processes audio.
//
// The UI half of adding a stage (click, search, which plugin got chosen) is
// covered by --test-ui with no plugins installed. This is the other half: that
// a real plugin URI appended to a real captured preset produces a chain that
// builds and runs. Neither test needs an audio server.
int TestAddStage(const std::string& query, const std::string& preset_dir, const char* plugin_uri)
{
	sf::Preset preset;
	std::string resolved_path;

	if (!ResolvePreset(query, preset_dir, preset, resolved_path))
	{
		return 1;
	}

	constexpr double kSampleRate = 48000.0;
	constexpr uint32_t kBlockSize = 256;

	LilvWorld* world = lilv_world_new();
	lilv_world_load_all(world);

	sf::UridMap urid_map;
	std::string error;

	std::unique_ptr<sf::Chain> before = sf::BuildChainFromPreset(world, preset, kSampleRate, kBlockSize, urid_map, error);

	if (!before)
	{
		std::fprintf(stderr, "SKIPPED: could not build '%s': %s\n", preset.name.c_str(), error.c_str());
		lilv_world_free(world);
		return 0;
	}

	const size_t stages_before = before->StageCount();

	// The same function the button calls.
	const sf::Preset edited = PresetWithAddedStage(*before, urid_map, preset.name, plugin_uri);

	// A new stage must arrive empty. Inheriting a neighbour's controls would
	// be bad enough; inheriting its LV2 state would silently carry over
	// another plugin's loaded model or impulse response.
	{
		const sf::PresetStage& appended = edited.chain.back();

		if (!appended.controls.empty() || !appended.state.empty() || appended.bypassed)
		{
			std::fprintf(stderr, "FAIL: the appended stage arrived with %zu control(s), %zu byte(s) of state%s.\n",
				appended.controls.size(), appended.state.size(), appended.bypassed ? ", bypassed" : "");
			lilv_world_free(world);
			return 1;
		}
	}

	std::unique_ptr<sf::Chain> after = sf::BuildChainFromPreset(world, edited, kSampleRate, kBlockSize, urid_map, error);

	if (!after)
	{
		std::fprintf(stderr, "SKIPPED: could not add '%s': %s\n", plugin_uri, error.c_str());
		lilv_world_free(world);
		return 0;
	}

	if (after->StageCount() != stages_before + 1)
	{
		std::fprintf(stderr, "FAIL: chain went from %zu to %zu stage(s), expected %zu.\n", stages_before,
			after->StageCount(), stages_before + 1);
		lilv_world_free(world);
		return 1;
	}

	if (after->Stage(after->StageCount() - 1)->Uri() != plugin_uri)
	{
		std::fprintf(stderr, "FAIL: the appended stage is '%s', expected '%s'.\n",
			after->Stage(after->StageCount() - 1)->Uri().c_str(), plugin_uri);
		lilv_world_free(world);
		return 1;
	}

	// Building isn't enough -- a chain that instantiates but produces
	// NaN is worse than one that refuses to build.
	after->Settle();

	sf::OfflineEngine engine(kBlockSize);
	const sf::OfflineEngine::Result result =
		engine.Run(*after, static_cast<uint64_t>(kSampleRate * 0.5), sf::MakeSineSweep(kSampleRate, 0.5, 80.0, 5000.0, 0.5f));

	if (result.non_finite_samples > 0)
	{
		std::fprintf(stderr, "FAIL: %llu non-finite sample(s) after adding '%s'.\n",
			static_cast<unsigned long long>(result.non_finite_samples), plugin_uri);
		lilv_world_free(world);
		return 1;
	}

	std::printf("Add stage: PASS (%zu -> %zu stage(s), appended %s, %llu frames, peak %.4f, no non-finite samples)\n",
		stages_before, after->StageCount(), plugin_uri, static_cast<unsigned long long>(result.frames_processed),
		result.peak);

	after.reset();
	before.reset();
	lilv_world_free(world);

	return 0;
}

// Loads a file into a running chain's stage exactly the way the file picker
// does -- LoadPathProperty, then settle -- and proves it took by reading the
// property back out of the plugin.
//
// The UI half (which row was clicked, which property and path it reports) is
// covered by --test-ui with no files on disk. This is the half that needs a
// real plugin and a real file.
int TestPathProperty(const std::string& query, const std::string& preset_dir, const char* file_path)
{
	sf::Preset preset;
	std::string resolved_path;

	if (!ResolvePreset(query, preset_dir, preset, resolved_path))
	{
		return 1;
	}

	constexpr double kSampleRate = 48000.0;
	constexpr uint32_t kBlockSize = 256;

	LilvWorld* world = lilv_world_new();
	lilv_world_load_all(world);

	sf::UridMap urid_map;
	std::string error;

	std::unique_ptr<sf::Chain> chain = sf::BuildChainFromPreset(world, preset, kSampleRate, kBlockSize, urid_map, error);

	if (!chain)
	{
		std::fprintf(stderr, "SKIPPED: could not build '%s': %s\n", preset.name.c_str(), error.c_str());
		lilv_world_free(world);
		return 0;
	}

	// The first stage that has a file-valued parameter at all.
	sf::PluginStage* stage = nullptr;
	sf::PathProperty property;

	for (size_t i = 0; i < chain->StageCount() && !stage; ++i)
	{
		for (const sf::PathProperty& candidate : chain->Stage(i)->PathProperties())
		{
			stage = chain->Stage(i);
			property = candidate;
			break;
		}
	}

	if (!stage)
	{
		std::fprintf(stderr, "SKIPPED: no stage of '%s' has a file parameter.\n", preset.name.c_str());
		lilv_world_free(world);
		return 0;
	}

	const std::string before = stage->GetPathProperty(property.uri, urid_map);

	if (!stage->LoadPathProperty(property.uri, file_path, urid_map))
	{
		std::fprintf(stderr, "FAIL: loading '%s' into '%s' was refused.\n", file_path, property.label.c_str());
		lilv_world_free(world);
		return 1;
	}

	stage->SettleWorker();

	const std::string after = stage->GetPathProperty(property.uri, urid_map);

	if (after != file_path)
	{
		std::fprintf(stderr, "FAIL: '%s' reads back as '%s' after loading '%s'.\n", property.label.c_str(),
			after.c_str(), file_path);
		lilv_world_free(world);
		return 1;
	}

	if (before == after)
	{
		std::fprintf(stderr, "FAIL: '%s' already held '%s', so this proved nothing.\n", property.label.c_str(),
			file_path);
		lilv_world_free(world);
		return 1;
	}

	// Loading a model that doesn't process audio would pass the readback and
	// still be useless.
	chain->Settle();

	sf::OfflineEngine engine(kBlockSize);
	const sf::OfflineEngine::Result result = engine.Run(
		*chain, static_cast<uint64_t>(kSampleRate * 0.5), sf::MakeSineSweep(kSampleRate, 0.5, 80.0, 5000.0, 0.5f));

	if (result.non_finite_samples > 0)
	{
		std::fprintf(stderr, "FAIL: %llu non-finite sample(s) after loading '%s'.\n",
			static_cast<unsigned long long>(result.non_finite_samples), file_path);
		lilv_world_free(world);
		return 1;
	}

	std::printf("Path property: PASS ('%s' went from '%s' to '%s', %llu frames, peak %.4f)\n", property.label.c_str(),
		before.empty() ? "(none)" : FileName(before).c_str(), FileName(after).c_str(),
		static_cast<unsigned long long>(result.frames_processed), result.peak);

	chain.reset();
	lilv_world_free(world);

	return 0;
}

int SelfTest()
{
	std::puts("Signalforge - work in progress. See docs/PLAN.md.");

	LilvWorld* world = lilv_world_new();
	std::printf("lilv:        linked OK (world %p)\n", static_cast<void*>(world));
	lilv_world_free(world);

	std::printf("jack client: linked OK (jack_client_name_size=%d)\n", jack_client_name_size());

	nlohmann::json probe = {{"ok", true}};
	std::printf("nlohmann/json: %s\n", probe.dump().c_str());

	return 0;
}

}  // namespace

int main(int argc, char** argv)
{
	if (argc == 1)
	{
		return SelfTest();
	}

	if (argc == 2 && std::strcmp(argv[1], "--list-plugins") == 0)
	{
		return ListPlugins();
	}

	if (argc == 3 && std::strcmp(argv[1], "--inspect") == 0)
	{
		return Inspect(argv[2]);
	}

	if (argc >= 2 && std::strcmp(argv[1], "--test-ui") == 0)
	{
		return TestUi(argc >= 3 ? std::atoi(argv[2]) : 60);
	}

	if (argc >= 4 && std::strcmp(argv[1], "--test-path-property") == 0)
	{
		std::string preset_dir = "presets";

		for (int i = 4; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--preset-dir") == 0 && i + 1 < argc)
			{
				preset_dir = argv[i + 1];
				++i;
			}
			else
			{
				PrintUsage(argv[0]);
				return 1;
			}
		}

		return TestPathProperty(argv[2], preset_dir, argv[3]);
	}

	if (argc >= 4 && std::strcmp(argv[1], "--test-add-stage") == 0)
	{
		std::string preset_dir = "presets";

		for (int i = 4; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--preset-dir") == 0 && i + 1 < argc)
			{
				preset_dir = argv[i + 1];
				++i;
			}
			else
			{
				PrintUsage(argv[0]);
				return 1;
			}
		}

		return TestAddStage(argv[2], preset_dir, argv[3]);
	}

	if (argc == 3 && std::strcmp(argv[1], "--check-midi-map") == 0)
	{
		return CheckMidiMap(argv[2]);
	}

	if (argc == 3 && std::strcmp(argv[1], "--preset-roundtrip") == 0)
	{
		return PresetRoundTrip(argv[2]);
	}

	if (argc >= 2 && std::strcmp(argv[1], "--list-presets") == 0)
	{
		std::string preset_dir = "presets";

		if (argc == 4 && std::strcmp(argv[2], "--preset-dir") == 0)
		{
			preset_dir = argv[3];
		}
		else if (argc != 2)
		{
			PrintUsage(argv[0]);
			return 1;
		}

		return ListPresets(preset_dir);
	}

	if (argc >= 2 && std::strcmp(argv[1], "--tune-crossfade") == 0)
	{
		double sample_rate = 48000.0;

		for (int i = 2; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--sample-rate") == 0 && i + 1 < argc)
			{
				sample_rate = std::atof(argv[i + 1]);
				++i;
			}
			else
			{
				PrintUsage(argv[0]);
				return 1;
			}
		}

		if (!(sample_rate > 0.0))
		{
			std::fprintf(stderr, "--sample-rate must be > 0.\n");
			return 1;
		}

		return TuneCrossfade(sample_rate);
	}

	if (argc >= 2 && std::strcmp(argv[1], "--run-preset") == 0)
	{
		std::string preset_dir = "presets";
		RunPresetOptions options;

		// The preset is optional: without one the board resumes whatever it
		// was last playing (SF-32). A leading "--" is a flag, not a preset
		// name, so "--run-preset --gui" means "resume, with a window".
		const bool named_preset = argc >= 3 && std::strncmp(argv[2], "--", 2) != 0;
		const std::string query = named_preset ? argv[2] : std::string();

		for (int i = named_preset ? 3 : 2; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--preset-dir") == 0 && i + 1 < argc)
			{
				preset_dir = argv[i + 1];
				++i;
			}
			else if (std::strcmp(argv[i], "--no-crossfade") == 0)
			{
				options.use_crossfade = false;
			}
			else if (std::strcmp(argv[i], "--midi-device") == 0 && i + 1 < argc)
			{
				options.midi_device = argv[i + 1];
				++i;
			}
			else if (std::strcmp(argv[i], "--no-midi-connect") == 0)
			{
				options.connect_midi = false;
			}
			else if (std::strcmp(argv[i], "--midi-map") == 0 && i + 1 < argc)
			{
				options.midi_map_path = argv[i + 1];
				++i;
			}
			else if (std::strcmp(argv[i], "--settings") == 0 && i + 1 < argc)
			{
				options.settings_path = argv[i + 1];
				++i;
			}
			else if (std::strcmp(argv[i], "--crossfade-ms") == 0 && i + 1 < argc)
			{
				options.crossfade_ms_set = true;
				options.crossfade_ms = std::atof(argv[i + 1]);
				++i;
			}
			else if (std::strcmp(argv[i], "--crossfade-curve") == 0 && i + 1 < argc)
			{
				if (!sf::ParseCrossfadeCurve(argv[i + 1], options.crossfade_curve))
				{
					std::fprintf(stderr, "--crossfade-curve: expected 'equal-power' or 'linear', got '%s'.\n",
						argv[i + 1]);
					return 1;
				}

				options.crossfade_curve_set = true;
				++i;
			}
			else if (std::strcmp(argv[i], "--gui") == 0)
			{
#ifndef SIGNALFORGE_GUI
				// Said here rather than after the engine is up: a build with
				// no UI should be obvious before anything touches the audio
				// device, not after JACK has had its say.
				std::fprintf(stderr, "This build has no UI: SDL2 was missing when it was configured.\n"
									 "  sudo apt-get install libsdl2-dev, then re-run cmake.\n");
				return 1;
#endif
				options.show_gui = true;
			}
			else
			{
				PrintUsage(argv[0]);
				return 1;
			}
		}

		return RunPreset(query, preset_dir, options);
	}

	if (argc >= 3 && std::strcmp(argv[1], "--run") == 0)
	{
		std::vector<std::pair<std::string, float>> controls;
		std::vector<std::pair<std::string, std::string>> paths;

		for (int i = 3; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--control") == 0 && i + 1 < argc)
			{
				std::string name;
				float value = 0.0f;

				if (!ParseControlArg(argv[i + 1], name, value))
				{
					std::fprintf(stderr, "Malformed --control value (expected name=value): %s\n", argv[i + 1]);
					return 1;
				}

				controls.emplace_back(std::move(name), value);
				++i;
			}
			else if (std::strcmp(argv[i], "--set-path") == 0 && i + 1 < argc)
			{
				std::string property_uri;
				std::string file_path;

				if (!SplitKeyValue(argv[i + 1], property_uri, file_path))
				{
					std::fprintf(
						stderr, "Malformed --set-path value (expected property-uri=file-path): %s\n", argv[i + 1]);
					return 1;
				}

				paths.emplace_back(std::move(property_uri), std::move(file_path));
				++i;
			}
			else
			{
				PrintUsage(argv[0]);
				return 1;
			}
		}

		return Run(argv[2], controls, paths);
	}

	if (argc >= 3 && std::strcmp(argv[1], "--run-chain") == 0)
	{
		std::vector<std::string> uris;
		std::vector<ChainPathArg> paths;

		for (int i = 2; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--set-path") == 0 && i + 1 < argc)
			{
				ChainPathArg path;

				if (!ParseChainPathArg(argv[i + 1], path))
				{
					std::fprintf(stderr,
						"Malformed --set-path value (expected stage-index:property-uri=file-path): %s\n",
						argv[i + 1]);
					return 1;
				}

				paths.push_back(std::move(path));
				++i;
			}
			else
			{
				uris.emplace_back(argv[i]);
			}
		}

		return RunChain(uris, paths);
	}

	if (argc >= 4 && std::strcmp(argv[1], "--save-preset") == 0)
	{
		const char* output_path = argv[2];
		std::vector<std::string> uris;
		std::vector<ChainPathArg> paths;
		std::vector<ChainControlArg> controls;
		std::string name = "Untitled";

		for (int i = 3; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--set-path") == 0 && i + 1 < argc)
			{
				ChainPathArg path;

				if (!ParseChainPathArg(argv[i + 1], path))
				{
					std::fprintf(stderr,
						"Malformed --set-path value (expected stage-index:property-uri=file-path): %s\n", argv[i + 1]);
					return 1;
				}

				paths.push_back(std::move(path));
				++i;
			}
			else if (std::strcmp(argv[i], "--control") == 0 && i + 1 < argc)
			{
				ChainControlArg control;

				if (!ParseChainControlArg(argv[i + 1], control))
				{
					std::fprintf(stderr, "Malformed --control value (expected stage-index:symbol=value): %s\n",
						argv[i + 1]);
					return 1;
				}

				controls.push_back(std::move(control));
				++i;
			}
			else if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc)
			{
				name = argv[i + 1];
				++i;
			}
			else
			{
				uris.emplace_back(argv[i]);
			}
		}

		if (uris.empty())
		{
			std::fprintf(stderr, "--save-preset needs at least one plugin URI.\n");
			return 1;
		}

		return SavePreset(output_path, uris, paths, controls, name);
	}

	if (argc >= 3 && std::strcmp(argv[1], "--test-chain") == 0)
	{
		std::vector<std::string> uris;
		std::vector<ChainPathArg> paths;
		std::vector<ChainControlArg> controls;
		TestChainOptions options;

		for (int i = 2; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--set-path") == 0 && i + 1 < argc)
			{
				ChainPathArg path;

				if (!ParseChainPathArg(argv[i + 1], path))
				{
					std::fprintf(stderr,
						"Malformed --set-path value (expected stage-index:property-uri=file-path): %s\n", argv[i + 1]);
					return 1;
				}

				paths.push_back(std::move(path));
				++i;
			}
			else if (std::strcmp(argv[i], "--control") == 0 && i + 1 < argc)
			{
				ChainControlArg control;

				if (!ParseChainControlArg(argv[i + 1], control))
				{
					std::fprintf(
						stderr, "Malformed --control value (expected stage-index:symbol=value): %s\n", argv[i + 1]);
					return 1;
				}

				controls.push_back(std::move(control));
				++i;
			}
			else if (std::strcmp(argv[i], "--preset") == 0 && i + 1 < argc)
			{
				options.preset_path = argv[i + 1];
				++i;
			}
			else if (std::strcmp(argv[i], "--preset-dir") == 0 && i + 1 < argc)
			{
				options.preset_dir = argv[i + 1];
				++i;
			}
			else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
			{
				options.seconds = std::atof(argv[i + 1]);
				++i;
			}
			else if (std::strcmp(argv[i], "--block-size") == 0 && i + 1 < argc)
			{
				options.block_size = static_cast<uint32_t>(std::atoi(argv[i + 1]));
				++i;
			}
			else if (std::strcmp(argv[i], "--expect-signal") == 0)
			{
				options.expect_signal = true;
			}
			else if (std::strcmp(argv[i], "--bypass") == 0 && i + 1 < argc)
			{
				options.bypass_stages.push_back(static_cast<size_t>(std::atoi(argv[i + 1])));
				++i;
			}
			else
			{
				uris.emplace_back(argv[i]);
			}
		}

		if (options.seconds <= 0.0 || options.block_size == 0)
		{
			std::fprintf(stderr, "--seconds must be > 0 and --block-size must be >= 1.\n");
			return 1;
		}

		if (options.preset_path.empty() && uris.empty())
		{
			std::fprintf(stderr, "--test-chain needs either plugin URIs or --preset <file>.\n");
			return 1;
		}

		if (!options.preset_path.empty() && !uris.empty())
		{
			std::fprintf(stderr, "--test-chain takes either plugin URIs or --preset <file>, not both.\n");
			return 1;
		}

		return TestChain(uris, paths, controls, options);
	}

	PrintUsage(argv[0]);

	return 1;
}
