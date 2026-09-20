#pragma once

#include <array>
#include <string_view>

namespace orbit::terrain_geology::detail
{
inline constexpr std::array<std::string_view, 5U>
kReferenceGeologicalMaterialToml{
    R"ORBIT_GEO(@ORBIT_REFERENCE_BASALT_TOML@)ORBIT_GEO",
    R"ORBIT_GEO(@ORBIT_REFERENCE_GRANITE_TOML@)ORBIT_GEO",
    R"ORBIT_GEO(@ORBIT_REFERENCE_SANDSTONE_TOML@)ORBIT_GEO",
    R"ORBIT_GEO(@ORBIT_REFERENCE_LIMESTONE_TOML@)ORBIT_GEO",
    R"ORBIT_GEO(@ORBIT_REFERENCE_VOLCANIC_ASH_TOML@)ORBIT_GEO"
};
} // namespace orbit::terrain_geology::detail
