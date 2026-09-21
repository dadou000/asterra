#pragma once

#include <orbit/commands/CommandService.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/world_model/PropertyProvenance.hpp>

#include <optional>

namespace orbit::world_model
{
[[nodiscard]] std::optional<scene::ObjectId>
FindPropertyProvenanceRecord(
    const scene::ObjectStore& objects,
    scene::ObjectId owner,
    schema::PropertyId targetProperty);

[[nodiscard]] std::optional<PropertyProvenance>
ReadStoredPropertyProvenance(
    const scene::ObjectStore& objects,
    scene::ObjectId owner,
    schema::PropertyId targetProperty);

[[nodiscard]] PropertyProvenance
EffectivePropertyProvenance(
    const scene::ObjectStore& objects,
    scene::ObjectId owner,
    schema::PropertyId targetProperty);

scene::ObjectId WritePropertyProvenance(
    scene::ObjectStore& objects,
    commands::CommandService& commands,
    scene::ObjectId owner,
    schema::PropertyId targetProperty,
    const PropertyProvenance& provenance);
} // namespace orbit::world_model
