#include <orbit/studio_ui/StudioExpansionShell.hpp>

#include <orbit/content/ContentService.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/editor_ui/FocusState.hpp>
#include <orbit/editor_ui/PanelExtensions.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/studio_ui/CommandPaletteModel.hpp>
#include <orbit/studio_ui/SelectionBreadcrumbs.hpp>
#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/terrain_debug/TerrainDebugField.hpp>
#include <orbit/terrain_debug/TerrainDebugSeam.hpp>
#include <orbit/world_model/PrimitiveBinding.hpp>
#include <orbit/world_model/VisibilityProxyBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "StudioShellInternals.hpp"

namespace orbit::studio_ui
{
using namespace shell_detail;

bool StudioViewportPanels::CanCreateAtViewport(
    const std::string_view id) const noexcept
{
    if (views_ == nullptr ||
        session_ == nullptr ||
        !session_->World().HasWorld() ||
        views_->Find(id) == nullptr)
    {
        return false;
    }

    const auto* target =
        session_->Viewports().Find(id);
    return target != nullptr &&
        target->target.has_value();
}

void StudioViewportPanels::CreateLocalLightAtViewport(
    const std::string_view id,
    const bool spot)
{
    if (!CanCreateAtViewport(id))
    {
        throw std::runtime_error(
            "A targeted viewport is required to add a local light.");
    }

    auto* renderView = views_->Find(id);
    const auto* target = session_->Viewports().Find(id);
    auto& world = session_->World();

    const auto bodyObject =
        world.Universe().ObjectForBody(
            target->target->body);

    if (!bodyObject.has_value())
    {
        throw std::runtime_error(
            "Target body has no semantic object for local-light parenting.");
    }

    auto& commands = world.Commands();
    commands.BeginTransaction(
        spot ? "Add Spot Light" : "Add Point Light");

    scene::ObjectId created{};
    try
    {
        created = commands.CreateObject(
            spot
                ? world_model::kSpotLightType
                : world_model::kPointLightType,
            spot ? "Spot Light" : "Point Light",
            *bodyObject);

        const auto& camera = renderView->Camera();
        const math::Double3 position{
            camera.localPositionMeters.x +
                static_cast<f64>(camera.forward.x) * 5.0,
            camera.localPositionMeters.y +
                static_cast<f64>(camera.forward.y) * 5.0,
            camera.localPositionMeters.z +
                static_cast<f64>(camera.forward.z) * 5.0
        };

        commands.SetProperty(
            created,
            world_model::kLightPositionMeters,
            position);

        if (spot)
        {
            commands.SetProperty(
                created,
                world_model::kLightDirection,
                math::Double3{
                    static_cast<f64>(camera.forward.x),
                    static_cast<f64>(camera.forward.y),
                    static_cast<f64>(camera.forward.z)
                });
        }

        commands.CommitTransaction();
    }
    catch (...)
    {
        if (commands.HasActiveTransaction())
        {
            commands.RollbackTransaction();
        }
        throw;
    }

    const std::array selected{created};
    world.Selection().Set(
        std::span<const scene::ObjectId>(selected));
    status_ = spot
        ? "Spot light created and selected."
        : "Point light created and selected.";
}

void StudioViewportPanels::CreateVisibilityProxyAtViewport(
    const std::string_view id,
    const bool box)
{
    if (!CanCreateAtViewport(id))
    {
        throw std::runtime_error(
            "A targeted viewport is required to add a visibility proxy.");
    }

    auto* renderView = views_->Find(id);
    const auto* target = session_->Viewports().Find(id);
    auto& world = session_->World();

    const auto bodyObject =
        world.Universe().ObjectForBody(
            target->target->body);

    if (!bodyObject.has_value())
    {
        throw std::runtime_error(
            "Target body has no semantic object for visibility-proxy parenting.");
    }

    auto& commands = world.Commands();
    commands.BeginTransaction(
        box
            ? "Add Box Visibility Proxy"
            : "Add Sphere Visibility Proxy");

    scene::ObjectId created{};
    try
    {
        created = commands.CreateObject(
            world_model::kVisibilityProxyType,
            box
                ? "Box Visibility Proxy"
                : "Sphere Visibility Proxy",
            *bodyObject);

        const auto& camera = renderView->Camera();
        const math::Double3 position{
            camera.localPositionMeters.x +
                static_cast<f64>(camera.forward.x) * 5.0,
            camera.localPositionMeters.y +
                static_cast<f64>(camera.forward.y) * 5.0,
            camera.localPositionMeters.z +
                static_cast<f64>(camera.forward.z) * 5.0
        };

        commands.SetProperty(
            created,
            world_model::kVisibilityProxyPositionMeters,
            position);

        if (box)
        {
            commands.SetProperty(
                created,
                world_model::kVisibilityProxyShape,
                i64{1});
            commands.SetProperty(
                created,
                world_model::kVisibilityProxyHalfExtentsMeters,
                math::Double3{1.0, 1.0, 1.0});
        }
        else
        {
            commands.SetProperty(
                created,
                world_model::kVisibilityProxyShape,
                i64{0});
            commands.SetProperty(
                created,
                world_model::kVisibilityProxyRadiusMeters,
                1.0);
        }

        commands.CommitTransaction();
    }
    catch (...)
    {
        if (commands.HasActiveTransaction())
        {
            commands.RollbackTransaction();
        }
        throw;
    }

    const std::array selected{created};
    world.Selection().Set(
        std::span<const scene::ObjectId>(selected));
    status_ = box
        ? "Box visibility proxy created and selected."
        : "Sphere visibility proxy created and selected.";
}

void StudioViewportPanels::CreatePrimitiveAtViewport(
    const std::string_view id,
    const world_model::PrimitiveShape shape,
    const world_model::PrimitiveSurface surface)
{
    if (!CanCreateAtViewport(id))
    {
        throw std::runtime_error(
            "A targeted viewport is required to add a primitive.");
    }

    auto* renderView = views_->Find(id);
    const auto* target = session_->Viewports().Find(id);
    auto& world = session_->World();

    const auto bodyObject =
        world.Universe().ObjectForBody(
            target->target->body);
    if (!bodyObject.has_value())
    {
        throw std::runtime_error(
            "Target body has no semantic object for primitive parenting.");
    }

    auto request = world_model::MakePrimitivePreset(shape, surface);
    const auto& camera = renderView->Camera();
    request.positionMeters = {
        camera.localPositionMeters.x +
            static_cast<f64>(camera.forward.x) * 5.0,
        camera.localPositionMeters.y +
            static_cast<f64>(camera.forward.y) * 5.0,
        camera.localPositionMeters.z +
            static_cast<f64>(camera.forward.z) * 5.0};

    const auto created = world_model::CreatePrimitive(
        world.Commands(), *bodyObject, request);

    const std::array selected{created};
    world.Selection().Set(
        std::span<const scene::ObjectId>(selected));
    status_ = request.name + " created and selected.";
}
} // namespace orbit::studio_ui
