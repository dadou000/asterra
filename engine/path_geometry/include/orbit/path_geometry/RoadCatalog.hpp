#pragma once
#include <orbit/core/Types.hpp>
#include <array>
#include <string_view>

namespace orbit::path_geometry
{
// True first-party section catalog used by Studio, renderer, terrain and
// junction construction. These are geometry/transport profiles, not claims
// of road marking, sidewalks, traffic law or road furniture implementation.
struct RoadCatalogPreset
{
    std::string_view id;
    std::string_view label;
    f64 pavedWidthMeters;
    u32 laneCount;
    f64 designSpeedKmh;
};
inline constexpr std::array<RoadCatalogPreset,5> kRoadCatalog{{
    {"service-road","Service road",3.5,1,30.0},
    {"city-street","Two-lane street",6.5,2,50.0},
    {"rural-road","Two-lane rural road",7.0,2,60.0},
    {"city-avenue","Four-lane avenue",14.0,4,60.0},
    {"expressway","Six-lane expressway",21.0,6,100.0}
}};
[[nodiscard]] constexpr const RoadCatalogPreset*
FindRoadCatalog(std::string_view id) noexcept
{
    for(const auto& entry:kRoadCatalog)
        if(entry.id==id)return &entry;
    return nullptr;
}
} // namespace orbit::path_geometry
