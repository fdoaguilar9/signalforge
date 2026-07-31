#include "UridMap.h"

namespace sf
{

UridMap::UridMap()
{
	map_data_.handle = this;
	map_data_.map = &UridMap::MapCallback;

	unmap_data_.handle = this;
	unmap_data_.unmap = &UridMap::UnmapCallback;

	map_feature_.URI = LV2_URID__map;
	map_feature_.data = &map_data_;

	unmap_feature_.URI = LV2_URID__unmap;
	unmap_feature_.data = &unmap_data_;
}

LV2_URID UridMap::Map(const char* uri)
{
	const auto it = uri_to_id_.find(uri);
	if (it != uri_to_id_.end())
	{
		return it->second;
	}

	id_to_uri_.emplace_back(uri);

	// URIDs are 1-based; 0 is reserved to mean "no such URID" by the spec.
	const LV2_URID id = static_cast<LV2_URID>(id_to_uri_.size());
	uri_to_id_[uri] = id;

	return id;
}

const char* UridMap::Unmap(LV2_URID urid) const
{
	if (urid == 0 || urid > id_to_uri_.size())
	{
		return nullptr;
	}

	return id_to_uri_[urid - 1].c_str();
}

LV2_URID UridMap::MapCallback(LV2_URID_Map_Handle handle, const char* uri)
{
	return static_cast<UridMap*>(handle)->Map(uri);
}

const char* UridMap::UnmapCallback(LV2_URID_Unmap_Handle handle, LV2_URID urid)
{
	return static_cast<const UridMap*>(handle)->Unmap(urid);
}

}  // namespace sf
