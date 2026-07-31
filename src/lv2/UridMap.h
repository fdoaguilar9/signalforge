#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <lv2/urid/urid.h>
#include <lv2/core/lv2.h>

namespace sf
{

// Minimal LV2_URID_Map / LV2_URID_Unmap implementation. Not RT-safe (uses a
// std::unordered_map + std::string storage), which is fine as long as
// mapping only happens during setup, not from the audio thread's run().
class UridMap
{
public:
	UridMap();

	LV2_URID Map(const char* uri);
	const char* Unmap(LV2_URID urid) const;

	// Features referencing this instance's Map()/Unmap(); valid for the
	// lifetime of this UridMap.
	const LV2_Feature* MapFeature() const { return &map_feature_; }
	const LV2_Feature* UnmapFeature() const { return &unmap_feature_; }

	// The raw interfaces, for lilv APIs that take them directly rather than
	// as features (lilv_state_new_from_instance, lilv_state_to_string, ...).
	LV2_URID_Map* MapData() { return &map_data_; }
	LV2_URID_Unmap* UnmapData() { return &unmap_data_; }

private:
	static LV2_URID MapCallback(LV2_URID_Map_Handle handle, const char* uri);
	static const char* UnmapCallback(LV2_URID_Unmap_Handle handle, LV2_URID urid);

	std::unordered_map<std::string, LV2_URID> uri_to_id_;
	std::vector<std::string> id_to_uri_;

	LV2_URID_Map map_data_;
	LV2_URID_Unmap unmap_data_;
	LV2_Feature map_feature_;
	LV2_Feature unmap_feature_;
};

}  // namespace sf
