#pragma once

#include <orbit/commands/CommandService.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace orbit::world_model
{
enum class AtmosphereSolveOutcome : u8
{
    NoChange = 0,
    Derived = 1,
    Conflict = 2,
    InvalidInput = 3
};

struct AtmosphereSolveEvent
{
    schema::PropertyId target{};
    AtmosphereSolveOutcome outcome{
        AtmosphereSolveOutcome::NoChange};
    std::string explanation;
};

struct AtmosphereSolveReport
{
    std::vector<AtmosphereSolveEvent> events;

    [[nodiscard]] bool HasConflict() const noexcept;
    [[nodiscard]] bool HasInvalidInput() const noexcept;
    [[nodiscard]] u32 DerivedCount() const noexcept;
};

class AtmospherePropertySolver
{
public:
    AtmospherePropertySolver(
        scene::ObjectStore& objects,
        commands::CommandService& commands);

    [[nodiscard]] AtmosphereSolveReport Solve(
        scene::ObjectId atmosphereCapability);

    [[nodiscard]] AtmosphereSolveReport ApplyPreset(
        scene::ObjectId atmosphereCapability,
        std::string_view presetName);

    [[nodiscard]] static std::vector<std::string_view>
    Presets();

private:
    [[nodiscard]] AtmosphereSolveReport SolveInternal(
        scene::ObjectId atmosphereCapability);

    scene::ObjectStore& objects_;
    commands::CommandService& commands_;
};
} // namespace orbit::world_model
