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

inline constexpr commands::CommandId kAssignMaterial{
    .high = 0x4f52424954434d44ULL,
    .low = 0x41535349474e4d54ULL
};

void Register(
    commands::CommandRegistry& registry,
    commands::CommandService& commandService,
    scene::ObjectStore& objects,
    selection::SelectionService& selection);
} // namespace orbit::editor_model::authoring_commands
