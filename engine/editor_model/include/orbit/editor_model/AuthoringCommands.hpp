#pragma once

#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/selection/SelectionService.hpp>

namespace orbit::editor_model::authoring_commands
{
inline constexpr commands::CommandId kUndo{
    .high = 0x4f52424954434d44ULL,
    .low = 0x554e444f00000001ULL
};

inline constexpr commands::CommandId kRedo{
    .high = 0x4f52424954434d44ULL,
    .low = 0x5245444f00000001ULL
};

inline constexpr commands::CommandId kClearSelection{
    .high = 0x4f52424954434d44ULL,
    .low = 0x53454c434c454152ULL
};

inline constexpr commands::CommandId kMoveToRoot{
    .high = 0x4f52424954434d44ULL,
    .low = 0x4d4f5645524f4f54ULL
};

inline constexpr commands::CommandId kCreateCelestialSystem{
    .high = 0x4f52424954434d44ULL,
    .low = 0x4352454154455359ULL
};

inline constexpr commands::CommandId kCreateCelestialBody{
    .high = 0x4f52424954434d44ULL,
    .low = 0x4352454154454244ULL
};

inline constexpr commands::CommandId kCreateRockyPlanet{
    .high = 0x4f52424954434d44ULL,
    .low = 0x524f434b59504c4eULL
};

inline constexpr commands::CommandId kCreateTerrainSurface{
    .high = 0x4f52424954434d44ULL,
    .low = 0x4352454154455452ULL
};

inline constexpr commands::CommandId kRemoveTerrainSurface{
    .high = 0x4f52424954434d44ULL,
    .low = 0x52454d4f56455452ULL
};

inline constexpr commands::CommandId kAssignMaterial{
    .high = 0x4f52424954434d44ULL,
    .low = 0x41535349474e4d54ULL
};

inline constexpr commands::CommandId kAttachDecal{
    .high = 0x4f52424954434d44ULL,
    .low = 0x415454444543414cULL
};

inline constexpr commands::CommandId kConnectPathDirect{
    .high = 0x4f52424954434d44ULL,
    .low = 0x5041544844495245ULL
};

inline constexpr commands::CommandId kConnectPathBezier{
    .high = 0x4f52424954434d44ULL,
    .low = 0x5041544842455a49ULL
};

inline constexpr commands::CommandId kConnectPathRouted{
    .high = 0x4f52424954434d44ULL,
    .low = 0x50415448524f5554ULL
};

inline constexpr commands::CommandId kCreateVolume{
    .high = 0x4f52424954434d44ULL,
    .low = 0x4352454154564f4cULL
};

inline constexpr commands::CommandId kRemoveVolume{
    .high = 0x4f52424954434d44ULL,
    .low = 0x52454d4f56564f4cULL
};

inline constexpr commands::CommandId kAddVolumeSource{
    .high = 0x4f52424954434d44ULL,
    .low = 0x564f4c5352430001ULL
};

inline constexpr commands::CommandId kAddVolumeEffector{
    .high = 0x4f52424954434d44ULL,
    .low = 0x564f4c4546460001ULL
};

inline constexpr commands::CommandId kRemoveVolumeInput{
    .high = 0x4f52424954434d44ULL,
    .low = 0x564f4c494e505201ULL
};

inline constexpr commands::CommandId kMoveVolumeInputUp{
    .high = 0x4f52424954434d44ULL,
    .low = 0x564f4c494e505501ULL
};

inline constexpr commands::CommandId kMoveVolumeInputDown{
    .high = 0x4f52424954434d44ULL,
    .low = 0x564f4c494e504401ULL
};

inline constexpr commands::CommandId kPaintVolumeTerrainSource{
    .high = 0x4f52424954434d44ULL,
    .low = 0x564f4c5041494e54ULL
};

void Register(
    commands::CommandRegistry& registry,
    commands::CommandService& commandService,
    scene::ObjectStore& objects,
    selection::SelectionService& selection);

// Material-specific commands remain in the same shared command registry but
// are split into their own translation unit so the core authoring command
// implementation stays focused on hierarchy/path operations.
void RegisterMaterialCommands(
    commands::CommandRegistry& registry,
    commands::CommandService& commandService,
    scene::ObjectStore& objects,
    selection::SelectionService& selection);

// Registers real surface capability creation/removal. This is separate from
// the core hierarchy commands because it is valid only for capabilities with
// an actual runtime composition path.
void RegisterTerrainCommands(
    commands::CommandRegistry& registry,
    commands::CommandService& commandService,
    scene::ObjectStore& objects,
    selection::SelectionService& selection);

void RegisterVolumeCommands(
    commands::CommandRegistry& registry,
    commands::CommandService& commandService,
    scene::ObjectStore& objects,
    selection::SelectionService& selection);
} // namespace orbit::editor_model::authoring_commands
