#pragma once

#include <orbit/celestial_compact_objects/CompactObject.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>

namespace orbit::world_model
{
struct ResolvedCompactObject
{
    scene::ObjectId body{};
    scene::ObjectId capability{};
    celestial_compact_objects::CompactObjectParameters
        parameters{};
    u64 fingerprint{0U};
};

struct ResolvedAccretionFlow
{
    scene::ObjectId body{};
    scene::ObjectId capability{};
    celestial_compact_objects::AccretionFlowParameters
        parameters{};
    u64 fingerprint{0U};
};

[[nodiscard]] std::optional<ResolvedCompactObject>
ResolveCompactObject(
    const scene::ObjectStore& objects,
    scene::ObjectId body);

[[nodiscard]] std::optional<ResolvedAccretionFlow>
ResolveAccretionFlow(
    const scene::ObjectStore& objects,
    scene::ObjectId body,
    const celestial_compact_objects::
        CompactObjectParameters& compact);
} // namespace orbit::world_model
