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

inline constexpr commands::CommandId kAssignMaterial{
    .high = 0x4f52424954434d44ULL,
    .low = 0x41535349474e4d54ULL
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

void Register(
    commands::CommandRegistry& registry,
    commands::CommandService& commandService,
    scene::ObjectStore& objects,
    selection::SelectionService& selection);
} // namespace orbit::editor_model::authoring_commands
