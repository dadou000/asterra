#pragma once

#include <orbit/core/Types.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace orbit::paths
{
enum class PathProfileKind : u8
{
    Road,
    Rail,
    Waterway,
    Reference
};

struct PathProfile
{
    std::string name;
    PathProfileKind kind{PathProfileKind::Road};
    f64 widthMeters{6.0};
    u32 lanes{2};
    f64 minimumRadiusMeters{0.0};
    f64 maximumGrade{1.0};
    bool allowBridge{true};
    bool allowTunnel{true};
    f64 terrainCutCost{1.0};
    f64 terrainFillCost{1.0};
    f64 waterCrossingCost{1.0};
    std::vector<std::string> preferredCostFields;

    [[nodiscard]] bool operator==(
        const PathProfile&) const = default;
};

[[nodiscard]] PathProfile LoadPathProfile(
    const std::filesystem::path& source);

void SavePathProfile(
    const std::filesystem::path& destination,
    const PathProfile& profile);

[[nodiscard]] std::string_view PathProfileKindName(
    PathProfileKind kind) noexcept;
} // namespace orbit::paths
