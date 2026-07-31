#include "PathProperty.h"

#include <cctype>

#include <lv2/atom/atom.h>
#include <lv2/patch/patch.h>

namespace sf
{

namespace
{

// mod:fileTypes is a MOD Devices extension rather than core LV2, but it is
// what plugins in this world actually declare, and it's the difference
// between a picker showing four .nam files and showing a home directory.
constexpr const char* kFileTypesUri = "http://moddevices.com/ns/mod#fileTypes";
constexpr const char* kRdfsLabel = "http://www.w3.org/2000/01/rdf-schema#label";
constexpr const char* kRdfsRange = "http://www.w3.org/2000/01/rdf-schema#range";

std::string ToLower(const std::string& text)
{
	std::string result = text;

	for (char& c : result)
	{
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}

	return result;
}

// "http://.../plugin#model" -> "model". Only used when a property declares no
// label of its own, which is rare but shouldn't produce a blank row.
std::string LastSegment(const std::string& uri)
{
	const size_t cut = uri.find_last_of("#/");

	return cut == std::string::npos ? uri : uri.substr(cut + 1);
}

std::vector<std::string> SplitFileTypes(const std::string& text)
{
	std::vector<std::string> result;
	std::string current;

	for (const char c : text)
	{
		if (c == ',' || c == ' ')
		{
			if (!current.empty())
			{
				result.push_back(ToLower(current));
				current.clear();
			}

			continue;
		}

		current.push_back(c);
	}

	if (!current.empty())
	{
		result.push_back(ToLower(current));
	}

	return result;
}

}  // namespace

std::vector<PathProperty> FindPathProperties(LilvWorld* world, const LilvPlugin* plugin)
{
	std::vector<PathProperty> result;

	LilvNode* writable = lilv_new_uri(world, LV2_PATCH__writable);
	LilvNode* range = lilv_new_uri(world, kRdfsRange);
	LilvNode* label = lilv_new_uri(world, kRdfsLabel);
	LilvNode* file_types = lilv_new_uri(world, kFileTypesUri);
	LilvNode* atom_path = lilv_new_uri(world, LV2_ATOM__Path);

	LilvNodes* properties = lilv_world_find_nodes(world, lilv_plugin_get_uri(plugin), writable, nullptr);

	if (properties)
	{
		LILV_FOREACH (nodes, it, properties)
		{
			const LilvNode* property = lilv_nodes_get(properties, it);

			// A patch:writable can be any kind of parameter -- an int, a
			// string. Only the file-valued ones belong in a file picker.
			LilvNode* property_range = lilv_world_get(world, property, range, nullptr);
			const bool is_path = property_range && lilv_node_equals(property_range, atom_path);
			lilv_node_free(property_range);

			if (!is_path)
			{
				continue;
			}

			PathProperty found;
			found.uri = lilv_node_as_uri(property);

			LilvNode* property_label = lilv_world_get(world, property, label, nullptr);
			found.label = property_label ? lilv_node_as_string(property_label) : LastSegment(found.uri);
			lilv_node_free(property_label);

			if (found.label.empty())
			{
				found.label = LastSegment(found.uri);
			}

			LilvNode* types = lilv_world_get(world, property, file_types, nullptr);

			if (types && lilv_node_is_string(types))
			{
				found.file_types = SplitFileTypes(lilv_node_as_string(types));
			}

			lilv_node_free(types);

			result.push_back(std::move(found));
		}

		lilv_nodes_free(properties);
	}

	lilv_node_free(writable);
	lilv_node_free(range);
	lilv_node_free(label);
	lilv_node_free(file_types);
	lilv_node_free(atom_path);

	return result;
}

bool MatchesFileTypes(const PathProperty& property, const std::string& filename)
{
	// A plugin that doesn't say what it accepts gets everything shown. Hiding
	// files on a guess would be worse than showing too many.
	if (property.file_types.empty())
	{
		return true;
	}

	const size_t dot = filename.find_last_of('.');

	if (dot == std::string::npos || dot + 1 >= filename.size())
	{
		return false;
	}

	const std::string extension = ToLower(filename.substr(dot + 1));

	for (const std::string& accepted : property.file_types)
	{
		if (extension == accepted)
		{
			return true;
		}
	}

	return false;
}

}  // namespace sf
