#include <orbit/studio_session/StudioTerrainBakeController.hpp>

#include <algorithm>

namespace orbit::studio_session
{
StudioTerrainBakeController::StudioTerrainBakeController(
    editor_session::EditorWorldSession& world)
    : world_(world)
{
}

StudioTerrainBakeController::~StudioTerrainBakeController() = default;

void StudioTerrainBakeController::Clear()
{
    service_.reset();
    root_.clear();
}

std::optional<world::PlanetId> StudioTerrainBakeController::PlanetFor(
    const scene::ObjectId terrainObject) const
{
    if (!world_.HasWorld())
    {
        return std::nullopt;
    }

    const auto body = world_.Surfaces().BodyForTerrainObject(terrainObject);
    if (!body.has_value())
    {
        return std::nullopt;
    }

    const auto* services = world_.Surfaces().ServicesForBody(*body);
    if (services == nullptr || !services->Recipe().has_value())
    {
        return std::nullopt;
    }
    return services->Recipe()->planet.id;
}

bool StudioTerrainBakeController::Tick()
{
    if (!world_.HasWorld())
    {
        Clear();
        return false;
    }

    const std::filesystem::path root = world_.Project().RootDirectory();
    if (service_ == nullptr || root != root_)
    {
        service_ = std::make_unique<terrain_bake::TerrainBakeService>(
            root / "Bakes");
        root_ = root;
        lastTick_ = {};
    }

    const auto now = std::chrono::steady_clock::now();
    const f64 delta = lastTick_.time_since_epoch().count() == 0
        ? 0.0
        : std::clamp(
              std::chrono::duration<f64>(now - lastTick_).count(), 0.0, 0.25);
    lastTick_ = now;

    // Observe every terrain body's current recipe. A planet seen for the first
    // time with no bake on disk is baked here, on the calling thread, so no
    // terrain page is ever generated from the plate model.
    for (const universe::BodyId body : world_.Surfaces().TerrainBodies())
    {
        const auto* services = world_.Surfaces().ServicesForBody(body);
        if (services == nullptr || !services->Recipe().has_value())
        {
            continue;
        }

        const auto& recipe = *services->Recipe();
        const auto& policy = services->Processes().bake;
        const bool known = service_->Knows(recipe.planet.id);
        service_->Observe(
            recipe.planet.id,
            recipe.planet,
            recipe.desc,
            {.resolution = policy.resolution, .autoRebake = policy.autoRebake});

        if (!known &&
            (service_->Active(recipe.planet.id) == nullptr ||
             service_->ActiveRivers(recipe.planet.id) == nullptr))
        {
            static_cast<void>(service_->BakeNow(recipe.planet.id));
        }
    }

    service_->Tick(delta);

    bool installed = false;
    for (auto& completed : service_->TakeCompleted())
    {
        for (const universe::BodyId body : world_.Surfaces().TerrainBodies())
        {
            auto* services = world_.Surfaces().ServicesForBody(body);
            if (services != nullptr && services->Recipe().has_value() &&
                services->Recipe()->planet.id == completed.planet)
            {
                services->SetTectonicBake(completed.tectonics);
                services->SetRiverBake(completed.rivers);
                installed = true;
            }
        }
    }

    if (installed)
    {
        // The terrain source is composed from the installed bake; its
        // revision changes with the bake, which invalidates every cache that
        // was built from the previous terrain.
        static_cast<void>(world_.RebuildUniverse());
    }
    return installed;
}

std::optional<terrain_bake::BakeStatus> StudioTerrainBakeController::Status(
    const scene::ObjectId terrainObject) const
{
    const auto planet = PlanetFor(terrainObject);
    if (!planet.has_value() || service_ == nullptr)
    {
        return std::nullopt;
    }
    return service_->Status(*planet);
}

bool StudioTerrainBakeController::StartBake(
    const scene::ObjectId terrainObject,
    const std::optional<u32> resolution)
{
    const auto planet = PlanetFor(terrainObject);
    return planet.has_value() && service_ != nullptr &&
           service_->StartBake(*planet, resolution);
}

void StudioTerrainBakeController::Cancel(const scene::ObjectId terrainObject)
{
    if (const auto planet = PlanetFor(terrainObject);
        planet.has_value() && service_ != nullptr)
    {
        service_->Cancel(*planet);
    }
}
} // namespace orbit::studio_session
