#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <iostream>
#include <type_traits>

namespace
{
[[nodiscard]] bool Check(
    const bool condition,
    const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }

    return true;
}
} // namespace

int main()
{
    using namespace orbit;

    // Persistent semantic state cannot be mutated by terrain/editor/runtime
    // code directly: only commands::CommandService can construct MutationKey.
    static_assert(
        !std::is_default_constructible_v<scene::MutationKey>,
        "Persistent scene mutation must remain command-service gated.");
    static_assert(
        !std::is_copy_constructible_v<scene::MutationKey> ||
            !std::is_default_constructible_v<scene::MutationKey>,
        "A mutation key must not be independently obtainable.");

    static_assert(
        world_model::kGeologyAssetType !=
            world_model::kTerrainProcessAssetType &&
        world_model::kGeologyAssetType !=
            world_model::kBiomeAssetType &&
        world_model::kTerrainProcessAssetType !=
            world_model::kBiomeAssetType,
        "V0.0.4 authored surface asset type IDs must remain distinct.");

    schema::SchemaRegistry schemas;
    world_model::RegisterSchemas(schemas);

    const auto* geology =
        schemas.FindType(
            world_model::kGeologyAssetType);
    const auto* process =
        schemas.FindType(
            world_model::kTerrainProcessAssetType);
    const auto* biome =
        schemas.FindType(
            world_model::kBiomeAssetType);

    bool ok = true;
    ok &= Check(
        geology != nullptr,
        "Geology asset schema must be registered.");
    ok &= Check(
        process != nullptr,
        "Terrain process asset schema must be registered.");
    ok &= Check(
        biome != nullptr,
        "Biome asset schema must be registered.");

    if (geology != nullptr)
    {
        ok &= Check(
            geology->id ==
                world_model::kGeologyAssetType,
            "Geology asset schema ID changed after registration.");
    }

    if (process != nullptr)
    {
        ok &= Check(
            process->id ==
                world_model::kTerrainProcessAssetType,
            "Terrain process asset schema ID changed after registration.");
    }

    if (biome != nullptr)
    {
        ok &= Check(
            biome->id ==
                world_model::kBiomeAssetType,
            "Biome asset schema ID changed after registration.");
    }

    return ok ? 0 : 1;
}
