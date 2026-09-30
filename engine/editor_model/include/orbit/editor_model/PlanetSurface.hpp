#pragma once

#include <orbit/commands/CommandService.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/selection/SelectionService.hpp>

#include <optional>

namespace orbit::editor_model
{
// True for a spherical celestial body that should own solid ground: not an
// ellipsoid (analytic terrain is spherical only) and not a star, giant or
// compact object.
[[nodiscard]] bool IsSurfaceEligiblePlanet(
    const scene::ObjectStore& objects,
    scene::ObjectId body);

// Idempotent. Returns the body's Terrain Surface, creating it with Orbit's
// default terrain settings (and process settings) when missing. Returns
// nullopt when the object is not a spherical Celestial Body. Joins the active
// transaction, or runs its own.
[[nodiscard]] std::optional<scene::ObjectId> EnsureTerrainSurface(
    scene::ObjectStore& objects,
    commands::CommandService& commands,
    selection::SelectionService& selection,
    scene::ObjectId body);

// Creates a Terrain Surface for the body only when IsSurfaceEligiblePlanet.
// Used by every body-creation path so new planets are never surfaceless.
// Returns true when a surface was created.
bool EnsurePlanetSurface(
    scene::ObjectStore& objects,
    commands::CommandService& commands,
    selection::SelectionService& selection,
    scene::ObjectId body);

// Gives every eligible planet in the world a Terrain Surface in one undoable
// transaction. Returns how many surfaces were created.
u32 EnsureAllPlanetSurfaces(
    scene::ObjectStore& objects,
    commands::CommandService& commands,
    selection::SelectionService& selection);
} // namespace orbit::editor_model
