#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/terrain_bake/TerrainBakeService.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>

namespace orbit::studio_session
{
// Connects the planet bake service to the open world. Every terrain body's
// recipe is observed each tick; a finished bake is installed on the body's
// services and the composition is rebuilt, which composes the terrain source
// from the baked rasters. Terrain therefore never evaluates the plate model
// while it generates: the first bake of a planet happens before any terrain
// page is built (a blocking bake when none exists), and later recipe edits
// rebake in the background while the previous bake keeps driving the terrain.
//
// Bakes live in <project>/Bakes. Authority stays with the recipe on the
// Terrain Surface; the bake is derived and can always be rebuilt.
class StudioTerrainBakeController
{
public:
    explicit StudioTerrainBakeController(
        editor_session::EditorWorldSession& world);
    ~StudioTerrainBakeController();

    StudioTerrainBakeController(const StudioTerrainBakeController&) = delete;
    StudioTerrainBakeController& operator=(
        const StudioTerrainBakeController&) = delete;

    // Returns true when a bake was installed and the composition rebuilt.
    [[nodiscard]] bool Tick();

    // Drops the service (and cancels any bake) when the world closes.
    void Clear();

    [[nodiscard]] std::optional<terrain_bake::BakeStatus> Status(
        scene::ObjectId terrainObject) const;

    // Explicit bake; cancels one in flight. `resolution` also becomes the
    // body's persisted-for-this-session resolution only when given.
    [[nodiscard]] bool StartBake(
        scene::ObjectId terrainObject,
        std::optional<u32> resolution = {});

    void Cancel(scene::ObjectId terrainObject);

    void SetGeologyBakeBackend(
        std::shared_ptr<const terrain_bake::GeologyBakeBackend> backend);

private:
    [[nodiscard]] std::optional<world::PlanetId> PlanetFor(
        scene::ObjectId terrainObject) const;

    editor_session::EditorWorldSession& world_;
    std::unique_ptr<terrain_bake::TerrainBakeService> service_;
    std::filesystem::path root_;
    std::shared_ptr<const terrain_bake::GeologyBakeBackend> geologyBakeBackend_;
    std::chrono::steady_clock::time_point lastTick_{};
};
} // namespace orbit::studio_session
