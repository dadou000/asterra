#pragma once

#include <orbit/paths/PathProfile.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace orbit::infrastructure_catalog
{
// Authored specifications, not generated mesh/collision or a second network authority.
enum class ConstructionMode { Terrain, Supported };
enum class TransitionMode { Circular, CurvatureEased };
enum class CantMode { Automatic, Capped, Manual };

struct DesignRules {
    double designSpeedKmh{50.0};
    double preferredRadiusMeters{0.0};
    double maximumGrade{0.10};
    TransitionMode transition{TransitionMode::CurvatureEased};
    double transitionLengthMeters{0.0}; // zero = solver-selected
    CantMode cantMode{CantMode::Automatic};
    double maximumCantMeters{0.15};
    double manualCantMeters{0.0};
};

struct Entry {
    std::string id;
    paths::PathProfile profile;
    DesignRules rules;
    ConstructionMode defaultConstruction{ConstructionMode::Terrain};
    unsigned int version{1};
};

// A segment may override construction without changing the catalog preset.
// Distinct typed road/rail topology continues to be owned by Orbit::Paths.
struct SegmentIntent {
    std::string catalogId;
    unsigned int catalogVersion{1};
    ConstructionMode construction{ConstructionMode::Terrain};
    std::optional<double> designSpeedKmh;
    std::optional<TransitionMode> transition;
};

[[nodiscard]] inline bool Valid(const Entry& entry) noexcept
{
    const auto& r = entry.rules;
    return !entry.id.empty() && entry.version != 0 &&
        std::isfinite(entry.profile.widthMeters) && entry.profile.widthMeters > 0.0 &&
        std::isfinite(r.designSpeedKmh) && r.designSpeedKmh > 0.0 &&
        std::isfinite(r.preferredRadiusMeters) && r.preferredRadiusMeters >= 0.0 &&
        std::isfinite(r.maximumGrade) && r.maximumGrade >= 0.0 &&
        std::isfinite(r.transitionLengthMeters) && r.transitionLengthMeters >= 0.0 &&
        std::isfinite(r.maximumCantMeters) && r.maximumCantMeters >= 0.0 &&
        std::isfinite(r.manualCantMeters) && r.manualCantMeters >= 0.0;
}

class Catalog {
public:
    [[nodiscard]] bool Register(Entry entry)
    {
        if (!Valid(entry) || entries_.contains(entry.id))
            return false;
        entries_.emplace(entry.id, std::move(entry));
        return true;
    }

    [[nodiscard]] const Entry* Find(std::string_view id) const noexcept
    {
        const auto it = entries_.find(std::string(id));
        return it == entries_.end() ? nullptr : &it->second;
    }

    // Version mismatch is explicit; callers must migrate saved intent, not silently drift.
    [[nodiscard]] std::optional<Entry> Resolve(const SegmentIntent& intent) const
    {
        const Entry* entry = Find(intent.catalogId);
        if (!entry || entry->version != intent.catalogVersion)
            return std::nullopt;
        Entry resolved = *entry;
        if (intent.designSpeedKmh)
            resolved.rules.designSpeedKmh = *intent.designSpeedKmh;
        if (intent.transition)
            resolved.rules.transition = *intent.transition;
        resolved.defaultConstruction = intent.construction;
        if (!Valid(resolved))
            return std::nullopt;
        return resolved;
    }

private:
    std::unordered_map<std::string, Entry> entries_;
};
} // namespace orbit::infrastructure_catalog
