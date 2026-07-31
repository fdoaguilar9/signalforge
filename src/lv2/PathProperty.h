#pragma once

#include <string>
#include <vector>

#include <lilv/lilv.h>

namespace sf
{

// A file-valued parameter a plugin will accept: neural-amp-modeler-lv2's
// model, a convolver's impulse response. Declared in the plugin's TTL as a
// patch:writable whose rdfs:range is atom:Path.
struct PathProperty
{
	std::string uri;
	std::string label;  // rdfs:label, falling back to the URI's last segment

	// Extensions the plugin says it accepts, lowercase and without the dot,
	// from mod:fileTypes. Empty when the plugin doesn't say -- which means
	// "show everything", not "accept nothing".
	std::vector<std::string> file_types;
};

// Every file-valued parameter of `plugin`, in TTL order. Empty for the vast
// majority of plugins, which have no files to load.
std::vector<PathProperty> FindPathProperties(LilvWorld* world, const LilvPlugin* plugin);

// Whether `filename` looks like something `property` would accept. Always true
// when the plugin declared no file types.
bool MatchesFileTypes(const PathProperty& property, const std::string& filename);

}  // namespace sf
