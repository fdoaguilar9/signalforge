#include "Fonts.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include <limits.h>
#include <unistd.h>

namespace sf
{
namespace fonts
{

namespace
{

// The size the atlas is built at. Dear ImGui 1.92 rasterizes a face at
// whatever size PushFont asks for, so this is only the default -- one entry
// per size in the design's scale is no longer something anyone has to
// maintain.
constexpr float kBaseSize = text::kSm;

struct Loaded
{
	ImFont* ui = nullptr;
	ImFont* ui_medium = nullptr;
	ImFont* mono = nullptr;
};

Loaded g_loaded;
std::string g_directory;

const char* kFiles[] = {"SpaceGrotesk-Regular.ttf", "SpaceGrotesk-Medium.ttf", "IBMPlexMono-Regular.ttf"};

// Latin-1, plus the two angle quotes the header's breadcrumb is built from.
// ImGui's default range stops at U+00FF, and a `›` outside it draws as a box.
//
// **The design's disclosure arrows are not here, and cannot be**: neither
// Space Grotesk nor IBM Plex Mono carries U+25B8 `▸` or U+25BE `▾`, so the
// stage card (SF-70) has to draw those as triangles on the draw list rather
// than set them as text. ImGui::ArrowButton already does exactly that.
//
// Static, because ImGui keeps the pointer rather than copying the ranges.
const ImWchar kGlyphRanges[] = {
	0x0020, 0x00ff,  // Basic Latin + Latin-1 Supplement
	0x2039, 0x203a,  // ‹ ›
	0,
};

// Where this binary lives. The starting point for every relative guess below,
// because it is the one thing a running program knows about itself that
// doesn't depend on which directory it was started from.
std::string ExecutableDirectory()
{
	std::array<char, PATH_MAX> buffer{};
	const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);

	if (length <= 0)
	{
		return {};
	}

	buffer[static_cast<size_t>(length)] = '\0';

	return std::filesystem::path(buffer.data()).parent_path().string();
}

bool HasFonts(const std::filesystem::path& directory)
{
	std::error_code ignored;

	for (const char* file : kFiles)
	{
		if (!std::filesystem::exists(directory / "fonts" / file, ignored))
		{
			return false;
		}
	}

	return true;
}

// Every layout this program is expected to run from, in the order that a
// deliberate answer beats a guess: an explicit environment variable, then the
// install prefix, then a build tree, then the source tree the build was
// configured from.
//
// Spelled out rather than reduced to one path, because "the fonts loaded on
// the machine they were built on and nowhere else" is the ordinary way this
// goes wrong.
std::string FindResourceDirectory(std::vector<std::string>& looked_in)
{
	std::vector<std::filesystem::path> candidates;

	if (const char* override_dir = std::getenv("SIGNALFORGE_RESOURCE_DIR"))
	{
		candidates.emplace_back(override_dir);
	}

	const std::string executable = ExecutableDirectory();

	if (!executable.empty())
	{
		const std::filesystem::path base(executable);

		candidates.push_back(base / "resources");                    // alongside the binary
		candidates.push_back(base / ".." / "share" / "signalforge");  // <prefix>/bin -> <prefix>/share
		candidates.push_back(base / ".." / "resources");              // one deep in a build tree
		candidates.push_back(base / ".." / ".." / "resources");       // build/src/signalforge
	}

#ifdef SIGNALFORGE_SOURCE_RESOURCE_DIR
	// The build tree this binary was configured from. Last, so an installed
	// copy never reaches back into a source checkout that may have moved on.
	candidates.emplace_back(SIGNALFORGE_SOURCE_RESOURCE_DIR);
#endif

	for (const std::filesystem::path& candidate : candidates)
	{
		std::error_code ignored;
		const std::filesystem::path resolved = std::filesystem::weakly_canonical(candidate, ignored);
		const std::filesystem::path& directory = resolved.empty() ? candidate : resolved;

		if (HasFonts(directory))
		{
			return directory.string();
		}

		// Two candidates can resolve to the same place -- a build tree next to
		// its source is the ordinary case -- and a failure message that lists
		// the same directory twice reads like a bug in the search.
		if (std::find(looked_in.begin(), looked_in.end(), directory.string()) == looked_in.end())
		{
			looked_in.push_back(directory.string());
		}
	}

	return {};
}

}  // namespace

bool Load(std::string& warning)
{
	warning.clear();
	g_loaded = Loaded();

	std::vector<std::string> looked_in;
	g_directory = FindResourceDirectory(looked_in);

	if (g_directory.empty())
	{
		warning = "Could not find resources/fonts -- falling back to the bundled font. Looked in:";

		for (const std::string& path : looked_in)
		{
			warning += "\n  " + path;
		}

		warning += "\nSet SIGNALFORGE_RESOURCE_DIR to say where they are.";

		return false;
	}

	ImFontAtlas* atlas = ImGui::GetIO().Fonts;
	const std::filesystem::path directory = std::filesystem::path(g_directory) / "fonts";

	auto load = [&](const char* file) -> ImFont*
	{
		return atlas->AddFontFromFileTTF((directory / file).string().c_str(), kBaseSize, nullptr, kGlyphRanges);
	};

	// Loaded first, so it is the font anything that never calls PushFont gets.
	g_loaded.ui = load(kFiles[0]);
	g_loaded.ui_medium = load(kFiles[1]);
	g_loaded.mono = load(kFiles[2]);

	if (!g_loaded.ui || !g_loaded.ui_medium || !g_loaded.mono)
	{
		// A file that exists but won't parse. Rarer than a missing one and
		// worth telling apart from it, since the fix is different.
		warning = "A font in " + directory.string() + " could not be read -- falling back to the bundled font.";

		atlas->Clear();
		g_loaded = Loaded();

		return false;
	}

	ImGui::GetStyle().FontSizeBase = kBaseSize;

	return true;
}

ImFont* Get(Face face)
{
	switch (face)
	{
		case Face::Ui:
			return g_loaded.ui;

		case Face::UiMedium:
			return g_loaded.ui_medium;

		case Face::Mono:
			return g_loaded.mono;
	}

	return nullptr;
}

void Push(Face face, float size_px)
{
	// A null font means "keep the current one", which is exactly the right
	// behaviour on a fallback build: the size still applies, so a heading is
	// still a heading, it is just set in the bundled face.
	ImGui::PushFont(Get(face), size_px);
}

void Pop()
{
	ImGui::PopFont();
}

const std::string& ResourceDirectory()
{
	return g_directory;
}

}  // namespace fonts
}  // namespace sf
