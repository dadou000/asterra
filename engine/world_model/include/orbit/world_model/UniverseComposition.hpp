#pragma once

#include <orbit/celestial_gravity/GravityService.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <memory>
#include <optional>
#include <unordered_map>

namespace orbit::world_model
{
struct UniverseCompositionStats
{
    u32 systems{0};
    u32 referenceNodes{0};
    u32 bodies{0};
    u32 gravitySources{0};
    u64 sourceRevision{0};
};

// Builds the active celestial runtime representation from authoritative
// semantic world objects. Rebuild is transactional at the composition level:
// a candidate FrameGraph/BodyRegistry is constructed first and only replaces
// the live representation after every semantic record validates successfully.
class UniverseComposition
{
public:
    UniverseComposition();
    ~UniverseComposition();

    UniverseComposition(
        const UniverseComposition&) = delete;
    UniverseComposition& operator=(
        const UniverseComposition&) = delete;

    UniverseComposition(
        UniverseComposition&&) noexcept;
    UniverseComposition& operator=(
        UniverseComposition&&) noexcept;

    [[nodiscard]] UniverseCompositionStats
    Rebuild(const scene::ObjectStore& objects);

    [[nodiscard]] bool RebuildIfChanged(
        const scene::ObjectStore& objects);

    [[nodiscard]] frames::FrameGraph&
    Frames() noexcept;
    [[nodiscard]] const frames::FrameGraph&
    Frames() const noexcept;

    [[nodiscard]] universe::BodyRegistry&
    Bodies() noexcept;
    [[nodiscard]] const universe::BodyRegistry&
    Bodies() const noexcept;

    [[nodiscard]] celestial_gravity::GravityService&
    Gravity() noexcept;
    [[nodiscard]] const celestial_gravity::GravityService&
    Gravity() const noexcept;

    [[nodiscard]] std::optional<universe::SystemId>
    SystemForObject(scene::ObjectId object) const noexcept;

    [[nodiscard]] std::optional<universe::BodyId>
    BodyForObject(scene::ObjectId object) const noexcept;

    [[nodiscard]] std::optional<frames::FrameId>
    FrameForObject(scene::ObjectId object) const noexcept;

    [[nodiscard]] std::optional<
        celestial_gravity::GravitySourceId>
    GravitySourceForObject(
        scene::ObjectId object) const noexcept;

    [[nodiscard]] std::optional<scene::ObjectId>
    ObjectForBody(universe::BodyId body) const noexcept;

    [[nodiscard]] u64 SourceRevision() const noexcept;

private:
    std::unique_ptr<frames::FrameGraph> frames_;
    std::unique_ptr<universe::BodyRegistry> bodies_;
    std::unique_ptr<celestial_gravity::GravityService> gravity_;
    std::unordered_map<scene::ObjectId, universe::SystemId>
        systemByObject_;
    std::unordered_map<scene::ObjectId, universe::BodyId>
        bodyByObject_;
    std::unordered_map<scene::ObjectId, frames::FrameId>
        frameByObject_;
    std::unordered_map<
        scene::ObjectId,
        celestial_gravity::GravitySourceId>
        gravitySourceByObject_;
    std::unordered_map<universe::BodyId, scene::ObjectId>
        objectByBody_;
    u64 sourceRevision_{~u64{0}};
};
} // namespace orbit::world_model
